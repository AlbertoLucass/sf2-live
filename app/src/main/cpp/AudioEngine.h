#pragma once

#include "Sf2Synth.h"
#include "RealtimePolicy.h"

#include <amidi/AMidi.h>
#include <jni.h>
#include <oboe/LatencyTuner.h>
#include <oboe/Oboe.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sf2live {

class StreamCallback;

enum class CommandType : uint8_t {
    NoteOn,
    NoteOff,
    ControlChange,
    PitchBend,
    ProgramChange,
    SelectPreset,
    Panic,
    TestTone
};

struct AudioCommand {
    CommandType type = CommandType::NoteOn;
    int a = 0;
    int b = 0;
    int c = 0;
};

struct NativeMidiEvent {
    int status = 0;
    int data1 = 0;
    int data2 = 0;
    int64_t timestampNanos = 0;
    uint64_t sequence = 0;
};

template <size_t Capacity>
class CommandQueue {
public:
    bool push(const AudioCommand& command) noexcept {
        const size_t write = writeIndex_.load(std::memory_order_relaxed);
        const size_t next = (write + 1) % Capacity;
        if (next == readIndex_.load(std::memory_order_acquire)) return false;
        items_[write] = command;
        writeIndex_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(AudioCommand& command) noexcept {
        const size_t read = readIndex_.load(std::memory_order_relaxed);
        if (read == writeIndex_.load(std::memory_order_acquire)) return false;
        command = items_[read];
        readIndex_.store((read + 1) % Capacity, std::memory_order_release);
        return true;
    }

    void clear() noexcept {
        readIndex_.store(0, std::memory_order_relaxed);
        writeIndex_.store(0, std::memory_order_relaxed);
    }

private:
    std::array<AudioCommand, Capacity> items_{};
    std::atomic<size_t> readIndex_{0};
    std::atomic<size_t> writeIndex_{0};
};

class AudioEngine final {
public:
    static AudioEngine& instance();

    bool initialize();
    void shutdown();

    int loadSoundFont(const std::string& path, std::string& error);
    void unloadSoundFont(bool restartAudio);
    std::vector<PresetInfo> presetInfos();

    bool send(const AudioCommand& command);
    void setMasterVolume(float value);
    void setReleaseMilliseconds(float value);
    void setLatencyProfile(int profile);
    void setOutputDevice(int deviceId);
    void configureOutput(int deviceId, int profile, bool usbMixerOptimized, int sampleRate, int dataFormat);
    void selectPreset(int index);

    bool connectNativeMidi(JNIEnv* env, jobject midiDevice, int portNumber);
    void disconnectNativeMidi();

    std::string statusJson();

private:
    friend class StreamCallback;
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool openStreamLocked();
    bool openStreamWithModeLocked(oboe::SharingMode sharingMode, int deviceId);
    void closeStreamLocked();
    void restartAfterError(oboe::AudioStream* failedStream, uint64_t failedGeneration);
    oboe::DataCallbackResult onAudioReady(
        uint64_t generation,
        oboe::AudioStream* audioStream,
        void* audioData,
        int32_t numFrames);
    void onErrorBeforeClose(
        uint64_t generation,
        oboe::AudioStream* audioStream,
        oboe::Result error);
    void onErrorAfterClose(
        uint64_t generation,
        oboe::AudioStream* audioStream,
        oboe::Result error);
    void processCommands();
    void updateRealtimeBudget(int32_t numFrames, int64_t elapsedNanos);
    void pollNativeMidi();
    void consumeMidiBytes(const uint8_t* data, size_t count, int64_t timestampNanos);
    void queueMidiMessage(int status, int data1, int data2, int64_t timestampNanos);
    void applyMidiMessage(const NativeMidiEvent& event);
    int estimateCallbackWorkload() const noexcept;
    int countIncomingNoteOns() const noexcept;
    void updateWorkloadHints(oboe::AudioStream* audioStream, int workload);
    void maybeRecoverMinimumLatency(oboe::AudioStream* audioStream);
    void renderMidiScheduled(float* output, int32_t numFrames, int64_t callbackStartNanos);
    void updateMidiDiagnostics(const NativeMidiEvent& event, int64_t callbackStartNanos);
    void resetMidiParser();
    void disconnectNativeMidiLocked();

    std::mutex lifecycleMutex_;
    std::shared_ptr<oboe::AudioStream> stream_;
    std::shared_ptr<StreamCallback> streamCallback_;
    std::atomic<oboe::AudioStream*> activeStream_{nullptr};
    std::atomic<uint64_t> activeStreamGeneration_{0};
    std::unique_ptr<oboe::LatencyTuner> latencyTuner_;
    Sf2Synth synth_;
    CommandQueue<1024> commands_;
    std::mutex commandProducerMutex_;

    AMidiDevice* midiDevice_ = nullptr;
    AMidiOutputPort* midiOutputPort_ = nullptr;
    int midiRunningStatus_ = 0;
    int midiPendingStatus_ = 0;
    int midiExpectedData_ = 0;
    int midiDataCount_ = 0;
    int midiFirstData_ = 0;
    bool midiInSysEx_ = false;
    std::array<NativeMidiEvent, 512> midiEvents_{};
    size_t midiEventCount_ = 0;
    uint64_t midiSequence_ = 0;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> restarting_{false};
    std::atomic<uint64_t> streamGeneration_{0};
    std::atomic<int> latencyProfile_{1};
    std::atomic<float> masterVolume_{0.8f};
    std::atomic<float> releaseMs_{800.0f};
    float appliedMasterVolume_ = -1.0f;
    float appliedReleaseMs_ = -1.0f;
    std::atomic<int> sampleRate_{0};
    std::atomic<int> framesPerBurst_{0};
    std::atomic<int> framesPerCallback_{0};
    std::atomic<int> bufferSizeFrames_{0};
    std::atomic<int> bufferCapacityFrames_{0};
    std::atomic<int> sharingMode_{0};
    std::atomic<int> performanceMode_{0};
    std::atomic<int> dataFormat_{static_cast<int>(oboe::AudioFormat::Unspecified)};
    std::atomic<int> audioApi_{static_cast<int>(oboe::AudioApi::Unspecified)};
    std::atomic<int> lastError_{static_cast<int>(oboe::Result::OK)};
    std::atomic<int> preferredDeviceId_{oboe::kUnspecified};
    std::atomic<int> activeDeviceId_{oboe::kUnspecified};
    std::atomic<bool> mmapUsed_{false};
    std::atomic<bool> routeFallbackUsed_{false};
    std::atomic<int> restartAttempts_{0};
    std::atomic<int> restartFailures_{0};
    std::atomic<bool> performanceHintEnabled_{false};
    std::atomic<bool> usbMixerOptimized_{false};
    std::atomic<int> requestedSampleRate_{0};
    std::atomic<int> requestedDataFormat_{static_cast<int>(oboe::AudioFormat::Unspecified)};
    std::atomic<int> activeVoices_{0};
    std::atomic<int> voiceLimit_{64};
    std::atomic<int> callbackLoadPermille_{0};
    RealtimeLoadController loadController_;
    std::atomic<int> droppedCommands_{0};
    std::atomic<int> droppedMidiEvents_{0};
    std::atomic<int> testToneFrames_{0};
    std::atomic<bool> nativeMidiConnected_{false};
    std::atomic<uint64_t> midiMessageCount_{0};
    std::atomic<int> lastMidiStatus_{0};
    std::atomic<int> lastMidiData1_{0};
    std::atomic<int> lastMidiData2_{0};
    std::atomic<int64_t> lastMidiTimestampNanos_{0};
    std::atomic<int64_t> lastMidiToAudioMicros_{-1};
    std::atomic<int> workloadReported_{0};
    std::atomic<int> latencyRecoveryResets_{0};
    int previousCallbackWorkload_ = 0;
    bool workloadIncreaseActive_ = false;
    uint32_t latencyRecoveryCounter_ = 0;
    uint32_t latencyRecoveryCooldown_ = 0;
    uint32_t latencyStableChecks_ = 0;
    uint64_t idleAudioFrames_ = 0;
    int lastObservedXruns_ = 0;
    std::vector<float> floatScratch_;
    double testTonePhase_ = 0.0;
};

} // namespace sf2live
