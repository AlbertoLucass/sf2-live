#include "AudioEngine.h"
#include "MidiBatchScheduler.h"

#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <time.h>
#include <oboe/OboeExtensions.h>

namespace sf2live {

class StreamCallback final : public oboe::AudioStreamDataCallback,
                             public oboe::AudioStreamErrorCallback {
public:
    StreamCallback(AudioEngine& engine, uint64_t generation) noexcept
        : engine_(engine), generation_(generation) {}

    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream* audioStream,
        void* audioData,
        int32_t numFrames) override {
        return engine_.onAudioReady(generation_, audioStream, audioData, numFrames);
    }

    void onErrorBeforeClose(oboe::AudioStream* audioStream, oboe::Result error) override {
        engine_.onErrorBeforeClose(generation_, audioStream, error);
    }

    void onErrorAfterClose(oboe::AudioStream* audioStream, oboe::Result error) override {
        engine_.onErrorAfterClose(generation_, audioStream, error);
    }

private:
    AudioEngine& engine_;
    const uint64_t generation_;
};

namespace {
constexpr const char* kTag = "SF2LiveNative";
constexpr double kTwoPi = 6.28318530717958647692;

void logError(const std::string& text) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", text.c_str());
}
}

AudioEngine& AudioEngine::instance() {
    static AudioEngine engine;
    return engine;
}

AudioEngine::AudioEngine() {
    // Mantém as correções específicas por aparelho fornecidas pela Oboe.
    // Não forçamos extensões globais: a biblioteca decide o fast path seguro.
    oboe::OboeGlobals::setWorkaroundsEnabled(true);
}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (initialized_.load(std::memory_order_acquire) && stream_) return true;
    const bool opened = openStreamLocked();
    initialized_.store(opened, std::memory_order_release);
    return opened;
}

void AudioEngine::shutdown() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    initialized_.store(false, std::memory_order_release);
    closeStreamLocked();
    disconnectNativeMidiLocked();
    {
        std::lock_guard<std::mutex> producerLock(commandProducerMutex_);
        commands_.clear();
    }
}

bool AudioEngine::openStreamLocked() {
    closeStreamLocked();
    routeFallbackUsed_.store(false, std::memory_order_relaxed);

    const int preferredDevice = preferredDeviceId_.load(std::memory_order_relaxed);
    if (openStreamWithModeLocked(oboe::SharingMode::Exclusive, preferredDevice)) return true;
    if (openStreamWithModeLocked(oboe::SharingMode::Shared, preferredDevice)) return true;

    // IDs de dispositivos USB podem mudar após uma reconexão. Se a saída
    // preferida ainda não estiver disponível, mantenha o instrumento tocável
    // pela rota automática em vez de deixar o stream permanentemente fechado.
    if (preferredDevice != oboe::kUnspecified) {
        routeFallbackUsed_.store(true, std::memory_order_relaxed);
        if (openStreamWithModeLocked(oboe::SharingMode::Exclusive, oboe::kUnspecified)) return true;
        if (openStreamWithModeLocked(oboe::SharingMode::Shared, oboe::kUnspecified)) return true;
    }

    routeFallbackUsed_.store(false, std::memory_order_relaxed);
    return false;
}

bool AudioEngine::openStreamWithModeLocked(oboe::SharingMode sharingMode, int deviceId) {
    // Cada tentativa recebe uma geração única, inclusive quando open/start falha.
    // Isso evita ABA: uma callback antiga nunca coincide com uma sessão posterior.
    const uint64_t candidateGeneration =
        streamGeneration_.fetch_add(1, std::memory_order_relaxed) + 1U;
    auto callback = std::make_shared<StreamCallback>(*this, candidateGeneration);

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
        ->setChannelCount(2)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(sharingMode)
        ->setUsage(oboe::Usage::Game)
        ->setContentType(oboe::ContentType::Music)
        ->setSpatializationBehavior(oboe::SpatializationBehavior::Never)
        ->setIsContentSpatialized(false)
        ->setDataCallback(std::static_pointer_cast<oboe::AudioStreamDataCallback>(callback))
        ->setErrorCallback(std::static_pointer_cast<oboe::AudioStreamErrorCallback>(callback));

    // Não fixamos sample rate, formato nem frames por callback. Oboe/AAudio
    // escolhem o formato nativo do endpoint para evitar resampler e adaptadores.
    if (deviceId != oboe::kUnspecified) builder.setDeviceId(deviceId);

    std::shared_ptr<oboe::AudioStream> candidate;
    const oboe::Result openResult = builder.openStream(candidate);
    if (openResult != oboe::Result::OK || !candidate) {
        lastError_.store(static_cast<int>(openResult), std::memory_order_relaxed);
        return false;
    }

    const oboe::AudioFormat format = candidate->getFormat();
    if (format != oboe::AudioFormat::Float && format != oboe::AudioFormat::I16) {
        lastError_.store(static_cast<int>(oboe::Result::ErrorInvalidFormat), std::memory_order_relaxed);
        candidate->close();
        return false;
    }

    stream_ = std::move(candidate);
    streamCallback_ = std::move(callback);
    activeStreamGeneration_.store(candidateGeneration, std::memory_order_release);
    activeStream_.store(stream_.get(), std::memory_order_release);
    const int rate = stream_->getSampleRate();
    const int burst = std::max(1, stream_->getFramesPerBurst());
    const int profile = latencyProfile_.load(std::memory_order_relaxed);
    const RealtimeProfile realtimeProfile = RealtimeProfile::fromId(profile);
    const int minimumBuffer = burst * realtimeProfile.bufferBursts;
    const auto bufferResult = stream_->setBufferSizeInFrames(minimumBuffer);
    const int actualBuffer = bufferResult ? bufferResult.value() : stream_->getBufferSizeInFrames();

    stream_->setPerformanceHintEnabled(true);
    performanceHintEnabled_.store(stream_->isPerformanceHintEnabled(), std::memory_order_relaxed);

    const int maximumTunedBuffer = burst * (profile == 2 ? 4 : 2);
    latencyTuner_ = std::make_unique<oboe::LatencyTuner>(*stream_, maximumTunedBuffer);
    latencyTuner_->setMinimumBufferSize(minimumBuffer);
    latencyTuner_->setBufferSizeIncrement(burst);
    previousCallbackWorkload_ = 0;
    workloadIncreaseActive_ = false;
    latencyRecoveryCounter_ = 0;
    latencyRecoveryCooldown_ = 0;
    latencyStableChecks_ = 0;
    idleAudioFrames_ = 0;
    lastObservedXruns_ = 0;

    sampleRate_.store(rate, std::memory_order_relaxed);
    framesPerBurst_.store(burst, std::memory_order_relaxed);
    framesPerCallback_.store(stream_->getFramesPerDataCallback(), std::memory_order_relaxed);
    bufferSizeFrames_.store(actualBuffer, std::memory_order_relaxed);
    bufferCapacityFrames_.store(stream_->getBufferCapacityInFrames(), std::memory_order_relaxed);
    sharingMode_.store(static_cast<int>(stream_->getSharingMode()), std::memory_order_relaxed);
    performanceMode_.store(static_cast<int>(stream_->getPerformanceMode()), std::memory_order_relaxed);
    dataFormat_.store(static_cast<int>(format), std::memory_order_relaxed);
    audioApi_.store(static_cast<int>(stream_->getAudioApi()), std::memory_order_relaxed);
    activeDeviceId_.store(stream_->getDeviceId(), std::memory_order_relaxed);
    mmapUsed_.store(oboe::OboeExtensions::isMMapUsed(stream_.get()), std::memory_order_relaxed);

    const int maxCallbackFrames = std::max({
        2048,
        burst * 4,
        stream_->getFramesPerDataCallback(),
        stream_->getBufferCapacityInFrames()
    });
    floatScratch_.assign(static_cast<size_t>(maxCallbackFrames) * 2U, 0.0f);

    synth_.setSampleRate(rate);
    synth_.setMasterVolume(masterVolume_.load(std::memory_order_relaxed));
    synth_.setReleaseMilliseconds(releaseMs_.load(std::memory_order_relaxed));
    loadController_.reset(profile);
    const int initialVoiceLimit = loadController_.voiceLimit();
    synth_.setMaxVoices(static_cast<size_t>(initialVoiceLimit));
    voiceLimit_.store(initialVoiceLimit, std::memory_order_relaxed);
    callbackLoadPermille_.store(0, std::memory_order_relaxed);
    appliedMasterVolume_ = masterVolume_.load(std::memory_order_relaxed);
    appliedReleaseMs_ = releaseMs_.load(std::memory_order_relaxed);

    const oboe::Result startResult = stream_->requestStart();
    if (startResult != oboe::Result::OK) {
        lastError_.store(static_cast<int>(startResult), std::memory_order_relaxed);
        closeStreamLocked();
        return false;
    }

    lastError_.store(static_cast<int>(oboe::Result::OK), std::memory_order_relaxed);
    return true;
}

void AudioEngine::closeStreamLocked() {
    performanceHintEnabled_.store(false, std::memory_order_relaxed);
    mmapUsed_.store(false, std::memory_order_relaxed);
    activeStream_.store(nullptr, std::memory_order_release);
    activeStreamGeneration_.store(0, std::memory_order_release);

    // O stream guarda shared_ptr para os callbacks. Mantenha também uma referência
    // local até close() terminar: nenhum callback de erro/dados pode sobreviver à
    // sessão e tocar nos recursos da próxima rota.
    std::shared_ptr<oboe::AudioStream> closing = std::move(stream_);
    std::shared_ptr<StreamCallback> closingCallback = std::move(streamCallback_);
    if (closing) {
        closing->requestStop();
        closing->close();
    }
    latencyTuner_.reset();
    closingCallback.reset();
}

int AudioEngine::loadSoundFont(const std::string& path, std::string& error) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    const bool wasRunning = stream_ != nullptr;
    if (wasRunning) closeStreamLocked();

    const bool loaded = synth_.loadFromFile(path, error);
    synth_.setMasterVolume(masterVolume_.load(std::memory_order_relaxed));
    synth_.setReleaseMilliseconds(releaseMs_.load(std::memory_order_relaxed));

    if (wasRunning || initialized_.load(std::memory_order_relaxed)) {
        if (!openStreamLocked() && loaded) {
            error = "O SF2 foi carregado, mas não foi possível reabrir a saída de áudio.";
            return -2;
        }
    }
    return loaded ? static_cast<int>(synth_.presets().size()) : -1;
}

void AudioEngine::unloadSoundFont(bool restartAudio) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    const bool shouldRestartAudio = restartAudio &&
        (stream_ != nullptr || initialized_.load(std::memory_order_relaxed));
    if (stream_) closeStreamLocked();

    synth_.allNotesOff(true);
    synth_.unload();
    activeVoices_.store(0, std::memory_order_relaxed);
    workloadReported_.store(0, std::memory_order_relaxed);
    previousCallbackWorkload_ = 0;
    {
        std::lock_guard<std::mutex> producerLock(commandProducerMutex_);
        commands_.clear();
    }

    if (shouldRestartAudio && initialized_.load(std::memory_order_relaxed)) {
        if (!openStreamLocked()) logError("Falha ao reabrir o áudio após descarregar o SF2.");
    }
}

std::vector<PresetInfo> AudioEngine::presetInfos() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return synth_.presets();
}

bool AudioEngine::send(const AudioCommand& command) {
    std::lock_guard<std::mutex> producerLock(commandProducerMutex_);
    const bool ok = commands_.push(command);
    if (!ok) droppedCommands_.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

void AudioEngine::setMasterVolume(float value) {
    value = std::max(0.0f, std::min(1.5f, value));
    masterVolume_.store(value, std::memory_order_relaxed);
}

void AudioEngine::setReleaseMilliseconds(float value) {
    value = std::max(0.0f, std::min(10000.0f, value));
    releaseMs_.store(value, std::memory_order_relaxed);
}

void AudioEngine::setOutputDevice(int deviceId) {
    if (deviceId < 0) deviceId = oboe::kUnspecified;
    const int previous = preferredDeviceId_.exchange(deviceId, std::memory_order_relaxed);
    if (previous == deviceId) return;
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (initialized_.load(std::memory_order_relaxed)) openStreamLocked();
}

void AudioEngine::configureOutput(
    int deviceId,
    int profile,
    bool,
    int,
    int) {
    if (deviceId < 0) deviceId = oboe::kUnspecified;
    profile = std::max(0, std::min(2, profile));

    const int oldDevice = preferredDeviceId_.exchange(deviceId, std::memory_order_relaxed);
    const int oldProfile = latencyProfile_.exchange(profile, std::memory_order_relaxed);
    usbMixerOptimized_.store(false, std::memory_order_relaxed);
    requestedSampleRate_.store(0, std::memory_order_relaxed);
    requestedDataFormat_.store(static_cast<int>(oboe::AudioFormat::Unspecified), std::memory_order_relaxed);

    // AudioDeviceCallback pode publicar a mesma lista várias vezes durante uma
    // conexão USB. Reiniciar o Oboe sem mudança real produz silêncio transitório
    // e pode disparar callbacks de desconexão de um stream já substituído.
    if (oldDevice == deviceId && oldProfile == profile) return;

    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (initialized_.load(std::memory_order_relaxed)) openStreamLocked();
}

void AudioEngine::selectPreset(int index) {
    // O mapeamento do SF2 carrega páginas sob demanda. Antecipar as amostras do
    // preset fora da callback evita page faults quando a primeira nota é tocada.
    synth_.prefetchPreset(index);
    send({CommandType::SelectPreset, index, 0, 0});
}

void AudioEngine::setLatencyProfile(int profile) {
    profile = std::max(0, std::min(2, profile));
    const int old = latencyProfile_.exchange(profile, std::memory_order_relaxed);
    if (old == profile) return;
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (initialized_.load(std::memory_order_relaxed)) openStreamLocked();
}

bool AudioEngine::connectNativeMidi(JNIEnv* env, jobject midiDevice, int portNumber) {
    if (!env || !midiDevice || portNumber < 0) return false;

    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    const bool shouldRestartAudio = stream_ != nullptr || initialized_.load(std::memory_order_relaxed);
    if (stream_) closeStreamLocked();
    disconnectNativeMidiLocked();

    AMidiDevice* nativeDevice = nullptr;
    media_status_t status = AMidiDevice_fromJava(env, midiDevice, &nativeDevice);
    if (status != AMEDIA_OK || !nativeDevice) {
        if (shouldRestartAudio) openStreamLocked();
        return false;
    }

    AMidiOutputPort* nativePort = nullptr;
    status = AMidiOutputPort_open(nativeDevice, portNumber, &nativePort);
    if (status != AMEDIA_OK || !nativePort) {
        AMidiDevice_release(nativeDevice);
        if (shouldRestartAudio) openStreamLocked();
        return false;
    }

    midiDevice_ = nativeDevice;
    midiOutputPort_ = nativePort;
    resetMidiParser();
    nativeMidiConnected_.store(true, std::memory_order_release);

    if (shouldRestartAudio && !openStreamLocked()) {
        disconnectNativeMidiLocked();
        return false;
    }
    return true;
}

void AudioEngine::disconnectNativeMidi() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (!midiDevice_ && !midiOutputPort_) return;
    const bool shouldRestartAudio = stream_ != nullptr || initialized_.load(std::memory_order_relaxed);
    if (stream_) closeStreamLocked();
    disconnectNativeMidiLocked();
    if (shouldRestartAudio) openStreamLocked();
}

void AudioEngine::disconnectNativeMidiLocked() {
    nativeMidiConnected_.store(false, std::memory_order_release);
    if (midiOutputPort_) {
        AMidiOutputPort_close(midiOutputPort_);
        midiOutputPort_ = nullptr;
    }
    if (midiDevice_) {
        AMidiDevice_release(midiDevice_);
        midiDevice_ = nullptr;
    }
    resetMidiParser();
}

void AudioEngine::resetMidiParser() {
    midiRunningStatus_ = 0;
    midiPendingStatus_ = 0;
    midiExpectedData_ = 0;
    midiDataCount_ = 0;
    midiFirstData_ = 0;
    midiInSysEx_ = false;
    midiEventCount_ = 0;
}

void AudioEngine::pollNativeMidi() {
    midiEventCount_ = 0;
    AMidiOutputPort* port = midiOutputPort_;
    if (!port) return;

    uint8_t buffer[128];
    // AMidiOutputPort_receive() é não bloqueante. Lemos todos os pacotes que
    // chegaram desde a callback anterior, mas guardamos os eventos em um array
    // fixo para preservar a ordem e o espaçamento de retriggers rápidos.
    for (int packetIndex = 0; packetIndex < 96; ++packetIndex) {
        int32_t opcode = 0;
        size_t receivedBytes = 0;
        int64_t timestamp = 0;
        const ssize_t received = AMidiOutputPort_receive(
            port,
            &opcode,
            buffer,
            sizeof(buffer),
            &receivedBytes,
            &timestamp);
        if (received <= 0) break;
        if (opcode == AMIDI_OPCODE_DATA && receivedBytes > 0) {
            lastMidiTimestampNanos_.store(timestamp, std::memory_order_relaxed);
            consumeMidiBytes(buffer, receivedBytes, timestamp);
        }
    }
}

void AudioEngine::consumeMidiBytes(
    const uint8_t* data,
    size_t count,
    int64_t timestampNanos) {
    for (size_t index = 0; index < count; ++index) {
        const int value = data[index] & 0xFF;

        if (value >= 0xF8) continue; // realtime não altera running status
        if (midiInSysEx_) {
            if (value == 0xF7) midiInSysEx_ = false;
            continue;
        }
        if (value == 0xF0) {
            midiInSysEx_ = true;
            midiRunningStatus_ = 0;
            continue;
        }
        if ((value & 0x80) != 0) {
            if (value >= 0xF0) {
                midiRunningStatus_ = 0;
                midiPendingStatus_ = 0;
                midiDataCount_ = 0;
                continue;
            }
            midiRunningStatus_ = value;
            midiPendingStatus_ = value;
            const int command = value & 0xF0;
            midiExpectedData_ = (command == 0xC0 || command == 0xD0) ? 1 : 2;
            midiDataCount_ = 0;
            continue;
        }

        if (midiPendingStatus_ == 0) {
            if (midiRunningStatus_ == 0) continue;
            midiPendingStatus_ = midiRunningStatus_;
            const int command = midiPendingStatus_ & 0xF0;
            midiExpectedData_ = (command == 0xC0 || command == 0xD0) ? 1 : 2;
            midiDataCount_ = 0;
        }

        if (midiDataCount_ == 0) midiFirstData_ = value;
        ++midiDataCount_;
        if (midiDataCount_ >= midiExpectedData_) {
            queueMidiMessage(
                midiPendingStatus_,
                midiFirstData_,
                midiExpectedData_ == 2 ? value : 0,
                timestampNanos);
            midiPendingStatus_ = midiRunningStatus_;
            midiDataCount_ = 0;
        }
    }
}

void AudioEngine::queueMidiMessage(
    int status,
    int data1,
    int data2,
    int64_t timestampNanos) {
    const int command = status & 0xF0;
    if (command != 0x80 && command != 0x90 && command != 0xB0 &&
        command != 0xC0 && command != 0xE0) {
        return;
    }

    const bool noteEvent = command == 0x80 || command == 0x90;
    const bool criticalCc = command == 0xB0 &&
        (data1 == 64 || data1 == 120 || data1 == 121 || data1 == 123);

    // Controladores contínuos podem gerar centenas de mensagens entre duas
    // callbacks. Quando duas atualizações equivalentes são adjacentes, somente
    // a mais nova importa. Note On/Off e comandos de sustain/panic nunca são
    // condensados.
    if (!noteEvent && !criticalCc && midiEventCount_ > 0) {
        NativeMidiEvent& previous = midiEvents_[midiEventCount_ - 1];
        const int previousCommand = previous.status & 0xF0;
        const bool sameContinuousMessage =
            previous.status == status &&
            ((command == 0xE0) ||
             (command == 0xC0) ||
             (command == 0xB0 && previousCommand == 0xB0 && previous.data1 == data1));
        if (sameContinuousMessage) {
            previous.data1 = data1;
            previous.data2 = data2;
            previous.timestampNanos = timestampNanos;
            previous.sequence = ++midiSequence_;
            return;
        }
    }

    if (midiEventCount_ >= midiEvents_.size()) {
        if (noteEvent) {
            // Em uma rajada extrema, preserve a execução musical: substitua a
            // atualização contínua mais recente em vez de perder uma nota.
            for (size_t offset = midiEventCount_; offset > 0; --offset) {
                NativeMidiEvent& candidate = midiEvents_[offset - 1];
                const int candidateCommand = candidate.status & 0xF0;
                const bool replaceable = candidateCommand == 0xE0 ||
                    (candidateCommand == 0xB0 && candidate.data1 != 64 &&
                     candidate.data1 != 120 && candidate.data1 != 121 &&
                     candidate.data1 != 123);
                if (replaceable) {
                    candidate.status = status;
                    candidate.data1 = data1;
                    candidate.data2 = data2;
                    candidate.timestampNanos = timestampNanos;
                    candidate.sequence = ++midiSequence_;
                    return;
                }
            }
        }
        droppedMidiEvents_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    NativeMidiEvent& event = midiEvents_[midiEventCount_++];
    event.status = status;
    event.data1 = data1;
    event.data2 = data2;
    event.timestampNanos = timestampNanos;
    event.sequence = ++midiSequence_;
}

int AudioEngine::estimateCallbackWorkload() const noexcept {
    int estimatedVoices = synth_.activeVoiceCount();
    for (size_t index = 0; index < midiEventCount_; ++index) {
        const NativeMidiEvent& event = midiEvents_[index];
        const int command = event.status & 0xF0;
        if (command == 0x90 && event.data2 > 0) {
            estimatedVoices += synth_.estimateVoiceCountForNote(event.data1, event.data2);
        }
    }
    return std::max(1, estimatedVoices);
}

int AudioEngine::countIncomingNoteOns() const noexcept {
    int count = 0;
    for (size_t index = 0; index < midiEventCount_; ++index) {
        const NativeMidiEvent& event = midiEvents_[index];
        if ((event.status & 0xF0) == 0x90 && event.data2 > 0) ++count;
    }
    return count;
}

void AudioEngine::updateWorkloadHints(oboe::AudioStream* audioStream, int workload) {
    if (!audioStream) return;

    const int incomingNotes = countIncomingNoteOns();
    const int increase = workload - previousCallbackWorkload_;
    if (incomingNotes >= 8) {
        audioStream->notifyWorkloadSpike(true, false, "SF2LiveMidiBurst");
        workloadIncreaseActive_ = true;
    } else if (incomingNotes >= 3 && increase >= 4) {
        // Android 16/API 36 pode reagir imediatamente a uma subida de carga.
        // Em versões anteriores a chamada simplesmente retorna Unimplemented.
        audioStream->notifyWorkloadIncrease(true, false, "SF2LiveChord");
        workloadIncreaseActive_ = true;
    } else if (workloadIncreaseActive_ && workload <= 2) {
        audioStream->notifyWorkloadReset(true, false, "SF2LiveIdle");
        workloadIncreaseActive_ = false;
    }

    audioStream->reportWorkload(workload);
    previousCallbackWorkload_ = workload;
}

void AudioEngine::maybeRecoverMinimumLatency(oboe::AudioStream* audioStream) {
    if (!audioStream || !latencyTuner_) return;

    if (latencyRecoveryCooldown_ > 0) --latencyRecoveryCooldown_;
    ++latencyRecoveryCounter_;
    if ((latencyRecoveryCounter_ & 63U) != 0U) return;

    const auto xrunResult = audioStream->getXRunCount();
    const int xruns = xrunResult ? xrunResult.value() : lastObservedXruns_;
    const bool newXrun = xruns != lastObservedXruns_;
    lastObservedXruns_ = xruns;

    const int minimumBuffer = std::max(
        1,
        framesPerBurst_.load(std::memory_order_relaxed) *
            loadController_.profile().bufferBursts);
    const bool bufferAboveMinimum = audioStream->getBufferSizeInFrames() > minimumBuffer;
    const bool hasHeadroom = callbackLoadPermille_.load(std::memory_order_relaxed) < 380;

    // Depois de uma rajada pesada, o tuner pode subir para dois bursts. Assim
    // que o instrumento ficar realmente ocioso por cerca de um segundo,
    // voltamos ao mínimo em vez de carregar essa latência para a próxima frase.
    const uint64_t oneSecondOfAudio = static_cast<uint64_t>(std::max(
        1,
        sampleRate_.load(std::memory_order_relaxed)));
    if (!newXrun && bufferAboveMinimum && latencyRecoveryCooldown_ == 0 &&
        idleAudioFrames_ >= oneSecondOfAudio) {
        latencyTuner_->requestReset();
        latencyRecoveryResets_.fetch_add(1, std::memory_order_relaxed);
        latencyRecoveryCooldown_ = 1500;
        latencyStableChecks_ = 0;
        idleAudioFrames_ = 0;
        return;
    }

    static constexpr uint32_t kStableChecksBeforeReset = 64; // ~8-16 s
    static constexpr uint32_t kCooldownCallbacks = 3000;
    if (newXrun || !hasHeadroom) {
        latencyStableChecks_ = 0;
        return;
    }
    if (!bufferAboveMinimum || latencyRecoveryCooldown_ > 0) {
        latencyStableChecks_ = 0;
        return;
    }

    if (++latencyStableChecks_ >= kStableChecksBeforeReset) {
        // LatencyTuner só aumenta o buffer depois de um underrun. Um problema
        // transitório não deve manter a sessão inteira com latência maior.
        latencyTuner_->requestReset();
        latencyRecoveryResets_.fetch_add(1, std::memory_order_relaxed);
        latencyRecoveryCooldown_ = kCooldownCallbacks;
        latencyStableChecks_ = 0;
    }
}

void AudioEngine::applyMidiMessage(const NativeMidiEvent& event) {
    const int command = event.status & 0xF0;
    const int channel = event.status & 0x0F;

    switch (command) {
        case 0x80:
            synth_.noteOff(channel, event.data1);
            break;
        case 0x90:
            if (event.data2 == 0) synth_.noteOff(channel, event.data1);
            else synth_.noteOn(channel, event.data1, event.data2);
            break;
        case 0xB0:
            synth_.controlChange(channel, event.data1, event.data2);
            break;
        case 0xC0:
            synth_.programChange(channel, event.data1);
            break;
        case 0xE0:
            synth_.pitchBend(channel, (event.data2 << 7) | event.data1);
            break;
        default:
            break;
    }
}

void AudioEngine::updateMidiDiagnostics(
    const NativeMidiEvent& event,
    int64_t callbackStartNanos) {
    lastMidiStatus_.store(event.status, std::memory_order_relaxed);
    lastMidiData1_.store(event.data1, std::memory_order_relaxed);
    lastMidiData2_.store(event.data2, std::memory_order_relaxed);
    lastMidiTimestampNanos_.store(event.timestampNanos, std::memory_order_relaxed);

    if (event.timestampNanos > 0) {
        const int64_t deltaNanos = callbackStartNanos - event.timestampNanos;
        if (deltaNanos >= 0 && deltaNanos < 1000000000LL) {
            lastMidiToAudioMicros_.store(deltaNanos / 1000, std::memory_order_relaxed);
        }
    }
}

void AudioEngine::renderMidiScheduled(
    float* output,
    int32_t numFrames,
    int64_t callbackStartNanos) {
    if (!output || numFrames <= 0) return;
    if (midiEventCount_ == 0) {
        synth_.render(output, numFrames);
        return;
    }

    // O AMidi pode entregar vários eventos antigos no mesmo lote. Aplicar todos
    // no frame zero reduz a latência, mas colapsa Note On/Off repetidos e pode
    // apagar ataques. Preservamos a ordem em uma janela de recuperação de no
    // máximo 0,5 ms. Assim o retrigger continua audível sem acrescentar um burst
    // inteiro de atraso.
    int64_t firstPastTimestamp = 0;
    int64_t lastPastTimestamp = 0;
    for (size_t index = 0; index < midiEventCount_; ++index) {
        const int64_t timestamp = midiEvents_[index].timestampNanos;
        if (timestamp <= 0 || timestamp > callbackStartNanos) continue;
        if (firstPastTimestamp == 0 || timestamp < firstPastTimestamp) {
            firstPastTimestamp = timestamp;
        }
        if (timestamp > lastPastTimestamp) lastPastTimestamp = timestamp;
    }

    MidiBatchScheduler scheduler(
        sampleRate_.load(std::memory_order_relaxed),
        numFrames,
        callbackStartNanos,
        firstPastTimestamp,
        lastPastTimestamp);

    int renderedFrames = 0;
    for (size_t eventIndex = 0; eventIndex < midiEventCount_; ++eventIndex) {
        const NativeMidiEvent& event = midiEvents_[eventIndex];
        const int eventFrame = scheduler.nextFrame(event.timestampNanos);

        const int segmentFrames = eventFrame - renderedFrames;
        if (segmentFrames > 0) {
            synth_.render(
                output + static_cast<size_t>(renderedFrames) * 2U,
                segmentFrames);
            renderedFrames = eventFrame;
        }
        applyMidiMessage(event);
    }

    if (renderedFrames < numFrames) {
        synth_.render(
            output + static_cast<size_t>(renderedFrames) * 2U,
            numFrames - renderedFrames);
    }

    const NativeMidiEvent& lastEvent = midiEvents_[midiEventCount_ - 1];
    updateMidiDiagnostics(lastEvent, callbackStartNanos);
    midiMessageCount_.fetch_add(midiEventCount_, std::memory_order_relaxed);
}

void AudioEngine::processCommands() {
    AudioCommand command;
    while (commands_.pop(command)) {
        switch (command.type) {
            case CommandType::NoteOn:
                synth_.noteOn(command.a, command.b, command.c);
                break;
            case CommandType::NoteOff:
                synth_.noteOff(command.a, command.b);
                break;
            case CommandType::ControlChange:
                synth_.controlChange(command.a, command.b, command.c);
                break;
            case CommandType::PitchBend:
                synth_.pitchBend(command.a, command.b);
                break;
            case CommandType::ProgramChange:
                synth_.programChange(command.a, command.b);
                break;
            case CommandType::SelectPreset:
                synth_.selectPreset(command.a);
                break;
            case CommandType::Panic:
                synth_.allNotesOff(true);
                break;
            case CommandType::TestTone:
                testToneFrames_.store(
                    std::max(1, sampleRate_.load(std::memory_order_relaxed) / 2),
                    std::memory_order_relaxed);
                testTonePhase_ = 0.0;
                break;
        }
    }
}

void AudioEngine::updateRealtimeBudget(int32_t numFrames, int64_t elapsedNanos) {
    const int previousLimit = loadController_.voiceLimit();
    const int nextLimit = loadController_.update(
        numFrames,
        sampleRate_.load(std::memory_order_relaxed),
        elapsedNanos);
    callbackLoadPermille_.store(loadController_.loadPermille(), std::memory_order_relaxed);
    if (nextLimit != previousLimit) {
        synth_.setMaxVoices(static_cast<size_t>(nextLimit));
        voiceLimit_.store(nextLimit, std::memory_order_relaxed);
    }
}

oboe::DataCallbackResult AudioEngine::onAudioReady(
    uint64_t generation,
    oboe::AudioStream* audioStream,
    void* audioData,
    int32_t numFrames) {
    // Uma callback atrasada de um stream substituído nunca deve tocar no synth,
    // no LatencyTuner ou nos buffers pertencentes à sessão nova.
    if (generation != activeStreamGeneration_.load(std::memory_order_acquire) ||
        audioStream != activeStream_.load(std::memory_order_acquire)) {
        return oboe::DataCallbackResult::Stop;
    }

    timespec callbackStart{};
    clock_gettime(CLOCK_MONOTONIC, &callbackStart);
    framesPerCallback_.store(numFrames, std::memory_order_relaxed);

    pollNativeMidi();
    processCommands();

    // Informa a mudança de carga antes de renderizar. O Oboe documenta este
    // caso especificamente para sintetizadores que saltam de uma nota para
    // acordes, permitindo ao sistema elevar a CPU antes de ocorrer underrun.
    const int workload = estimateCallbackWorkload();
    workloadReported_.store(workload, std::memory_order_relaxed);
    updateWorkloadHints(audioStream, workload);

    const oboe::AudioFormat format = static_cast<oboe::AudioFormat>(
        dataFormat_.load(std::memory_order_relaxed));
    float* renderBuffer = nullptr;
    if (format == oboe::AudioFormat::Float) {
        renderBuffer = static_cast<float*>(audioData);
    } else {
        const size_t required = static_cast<size_t>(numFrames) * 2U;
        if (required > floatScratch_.size()) {
            auto* output = static_cast<int16_t*>(audioData);
            std::fill(output, output + required, static_cast<int16_t>(0));
            return oboe::DataCallbackResult::Continue;
        }
        renderBuffer = floatScratch_.data();
    }

    const float requestedVolume = masterVolume_.load(std::memory_order_relaxed);
    if (requestedVolume != appliedMasterVolume_) {
        synth_.setMasterVolume(requestedVolume);
        appliedMasterVolume_ = requestedVolume;
    }
    const float requestedRelease = releaseMs_.load(std::memory_order_relaxed);
    if (requestedRelease != appliedReleaseMs_) {
        synth_.setReleaseMilliseconds(requestedRelease);
        appliedReleaseMs_ = requestedRelease;
    }
    const int64_t callbackStartNanos =
        static_cast<int64_t>(callbackStart.tv_sec) * 1000000000LL + callbackStart.tv_nsec;
    renderMidiScheduled(renderBuffer, numFrames, callbackStartNanos);

    int toneFrames = testToneFrames_.load(std::memory_order_relaxed);
    if (toneFrames > 0) {
        const int rate = std::max(1, sampleRate_.load(std::memory_order_relaxed));
        const double increment = kTwoPi * 440.0 / static_cast<double>(rate);
        for (int32_t frame = 0; frame < numFrames && toneFrames > 0; ++frame, --toneFrames) {
            const float envelope = std::min(
                1.0f,
                static_cast<float>(toneFrames) / static_cast<float>(rate / 20 + 1));
            const float value = std::sin(testTonePhase_) * 0.18f * envelope;
            renderBuffer[frame * 2] = std::clamp(renderBuffer[frame * 2] + value, -1.0f, 1.0f);
            renderBuffer[frame * 2 + 1] = std::clamp(renderBuffer[frame * 2 + 1] + value, -1.0f, 1.0f);
            testTonePhase_ += increment;
            if (testTonePhase_ >= kTwoPi) testTonePhase_ -= kTwoPi;
        }
        testToneFrames_.store(toneFrames, std::memory_order_relaxed);
    }

    if (format == oboe::AudioFormat::I16) {
        auto* output = static_cast<int16_t*>(audioData);
        const size_t sampleCount = static_cast<size_t>(numFrames) * 2U;
        for (size_t sample = 0; sample < sampleCount; ++sample) {
            const float clamped = std::clamp(renderBuffer[sample], -1.0f, 1.0f);
            output[sample] = static_cast<int16_t>(std::lrintf(clamped * 32767.0f));
        }
    }

    const int currentActiveVoices = synth_.activeVoiceCount();
    activeVoices_.store(currentActiveVoices, std::memory_order_relaxed);
    if (midiEventCount_ == 0 && currentActiveVoices == 0) {
        idleAudioFrames_ += static_cast<uint64_t>(std::max(0, numFrames));
    } else {
        idleAudioFrames_ = 0;
    }

    timespec callbackEnd{};
    clock_gettime(CLOCK_MONOTONIC, &callbackEnd);
    const int64_t endNanos =
        static_cast<int64_t>(callbackEnd.tv_sec) * 1000000000LL + callbackEnd.tv_nsec;
    updateRealtimeBudget(numFrames, endNanos - callbackStartNanos);

    if (latencyTuner_) {
        latencyTuner_->tune();
        maybeRecoverMinimumLatency(audioStream);
        if (audioStream) {
            bufferSizeFrames_.store(
                audioStream->getBufferSizeInFrames(),
                std::memory_order_relaxed);
        }
    }
    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorBeforeClose(
    uint64_t generation,
    oboe::AudioStream* audioStream,
    oboe::Result error) {
    lastError_.store(static_cast<int>(error), std::memory_order_relaxed);
    if (generation != activeStreamGeneration_.load(std::memory_order_acquire)) return;

    oboe::AudioStream* expected = audioStream;
    if (activeStream_.compare_exchange_strong(
            expected,
            nullptr,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        activeStreamGeneration_.store(0, std::memory_order_release);
    }
}

void AudioEngine::onErrorAfterClose(
    uint64_t generation,
    oboe::AudioStream* audioStream,
    oboe::Result error) {
    lastError_.store(static_cast<int>(error), std::memory_order_relaxed);
    restartAfterError(audioStream, generation);
}

void AudioEngine::restartAfterError(
    oboe::AudioStream* failedStream,
    uint64_t failedGeneration) {
    bool expected = false;
    if (!restarting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    std::thread([this, failedStream, failedGeneration]() {
        constexpr std::array<int, 5> kRetryDelaysMs{40, 120, 300, 700, 1500};
        oboe::AudioStream* knownFailedStream = failedStream;

        for (int delayMs : kRetryDelaysMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            restartAttempts_.fetch_add(1, std::memory_order_relaxed);

            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            if (!initialized_.load(std::memory_order_relaxed)) {
                restarting_.store(false, std::memory_order_release);
                return;
            }

            if (stream_) {
                oboe::AudioStream* current = stream_.get();
                const uint64_t currentGeneration =
                    streamGeneration_.load(std::memory_order_relaxed);
                const oboe::StreamState state = stream_->getState();
                const bool sameFailedSession =
                    current == knownFailedStream && currentGeneration == failedGeneration;
                const bool currentLooksFailed =
                    sameFailedSession ||
                    state == oboe::StreamState::Closed ||
                    state == oboe::StreamState::Disconnected;

                // Um callback de erro antigo não pode fechar um stream novo que
                // já foi aberto por uma troca de rota ou configuração. A geração
                // também protege contra reutilização do mesmo endereço de memória.
                if (!currentLooksFailed) {
                    restarting_.store(false, std::memory_order_release);
                    return;
                }

                knownFailedStream = current;
                activeStream_.store(nullptr, std::memory_order_release);
                activeStreamGeneration_.store(0, std::memory_order_release);
                stream_.reset(); // onErrorAfterClose garante que ele já foi fechado.
                streamCallback_.reset();
                latencyTuner_.reset();
            }

            if (openStreamLocked()) {
                restarting_.store(false, std::memory_order_release);
                return;
            }
            restartFailures_.fetch_add(1, std::memory_order_relaxed);
        }

        logError("Falha ao reiniciar o stream Oboe após várias tentativas.");
        restarting_.store(false, std::memory_order_release);
    }).detach();
}

std::string AudioEngine::statusJson() {
    int32_t xruns = 0;
    oboe::StreamState state = oboe::StreamState::Uninitialized;
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        running = static_cast<bool>(stream_);
        if (stream_) {
            const auto xrunResult = stream_->getXRunCount();
            if (xrunResult) xruns = xrunResult.value();
            state = stream_->getState();
        }
    }

    std::ostringstream out;
    out << "{"
        << "\"running\":" << (running ? "true" : "false") << ','
        << "\"sampleRate\":" << sampleRate_.load(std::memory_order_relaxed) << ','
        << "\"framesPerBurst\":" << framesPerBurst_.load(std::memory_order_relaxed) << ','
        << "\"framesPerCallback\":" << framesPerCallback_.load(std::memory_order_relaxed) << ','
        << "\"bufferFrames\":" << bufferSizeFrames_.load(std::memory_order_relaxed) << ','
        << "\"bufferCapacityFrames\":" << bufferCapacityFrames_.load(std::memory_order_relaxed) << ','
        << "\"sharingMode\":" << sharingMode_.load(std::memory_order_relaxed) << ','
        << "\"performanceMode\":" << performanceMode_.load(std::memory_order_relaxed) << ','
        << "\"dataFormat\":" << dataFormat_.load(std::memory_order_relaxed) << ','
        << "\"audioApi\":" << audioApi_.load(std::memory_order_relaxed) << ','
        << "\"mmapUsed\":" << (mmapUsed_.load(std::memory_order_relaxed) ? "true" : "false") << ','
        << "\"routeFallbackUsed\":" << (routeFallbackUsed_.load(std::memory_order_relaxed) ? "true" : "false") << ','
        << "\"streamGeneration\":" << streamGeneration_.load(std::memory_order_relaxed) << ','
        << "\"restartAttempts\":" << restartAttempts_.load(std::memory_order_relaxed) << ','
        << "\"restartFailures\":" << restartFailures_.load(std::memory_order_relaxed) << ','
        << "\"performanceHintEnabled\":" << (performanceHintEnabled_.load(std::memory_order_relaxed) ? "true" : "false") << ','
        << "\"workloadReported\":" << workloadReported_.load(std::memory_order_relaxed) << ','
        << "\"latencyRecoveryResets\":" << latencyRecoveryResets_.load(std::memory_order_relaxed) << ','
        << "\"state\":" << static_cast<int>(state) << ','
        << "\"xruns\":" << xruns << ','
        << "\"lastError\":" << lastError_.load(std::memory_order_relaxed) << ','
        << "\"preferredDeviceId\":" << preferredDeviceId_.load(std::memory_order_relaxed) << ','
        << "\"activeDeviceId\":" << activeDeviceId_.load(std::memory_order_relaxed) << ','
        << "\"usbMixerOptimized\":false,"
        << "\"lowLatencyMode\":"
        << (performanceMode_.load(std::memory_order_relaxed) == static_cast<int>(oboe::PerformanceMode::LowLatency) ? "true" : "false") << ','
        << "\"activeVoices\":" << activeVoices_.load(std::memory_order_relaxed) << ','
        << "\"voiceLimit\":" << voiceLimit_.load(std::memory_order_relaxed) << ','
        << "\"callbackLoadPermille\":" << callbackLoadPermille_.load(std::memory_order_relaxed) << ','
        << "\"droppedCommands\":" << droppedCommands_.load(std::memory_order_relaxed) << ','
        << "\"droppedMidiEvents\":" << droppedMidiEvents_.load(std::memory_order_relaxed) << ','
        << "\"nativeMidiConnected\":" << (nativeMidiConnected_.load(std::memory_order_relaxed) ? "true" : "false") << ','
        << "\"midiMessageCount\":" << midiMessageCount_.load(std::memory_order_relaxed) << ','
        << "\"lastMidiStatus\":" << lastMidiStatus_.load(std::memory_order_relaxed) << ','
        << "\"lastMidiData1\":" << lastMidiData1_.load(std::memory_order_relaxed) << ','
        << "\"lastMidiData2\":" << lastMidiData2_.load(std::memory_order_relaxed) << ','
        << "\"lastMidiTimestampNanos\":" << lastMidiTimestampNanos_.load(std::memory_order_relaxed) << ','
        << "\"lastMidiToAudioMicros\":" << lastMidiToAudioMicros_.load(std::memory_order_relaxed)
        << "}";
    return out.str();
}

} // namespace sf2live
