#include "Sf2Synth.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace sf2live {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kUnset = std::numeric_limits<int>::min();
constexpr int kDefaultTimecents = -12000;
constexpr uint64_t kPhaseOne = uint64_t{1} << 32;
constexpr uint64_t kPhaseFractionMask = kPhaseOne - 1U;
constexpr float kPhaseFractionScale = 1.0f / 4294967296.0f;

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

std::string fixedString(const uint8_t* p, size_t size) {
    size_t length = 0;
    while (length < size && p[length] != 0) ++length;
    std::string result(reinterpret_cast<const char*>(p), length);
    while (!result.empty() && (result.back() == ' ' || result.back() == '\0')) result.pop_back();
    return result.empty() ? std::string("Preset sem nome") : result;
}

float clampFloat(float value, float lo, float hi) {
    return std::max(lo, std::min(hi, value));
}

int clampInt(int value, int lo, int hi) {
    return std::max(lo, std::min(hi, value));
}

float timecentsToSeconds(int tc) {
    if (tc <= -11950) return 0.0f;
    return clampFloat(std::pow(2.0f, static_cast<float>(tc) / 1200.0f), 0.0f, 100.0f);
}

float centibelsToGain(float cb) {
    if (cb <= 0.0f) return 1.0f;
    return std::pow(10.0f, -cb / 200.0f);
}

struct Range {
    int lo = 0;
    int hi = 127;
};

struct RawPresetHeader {
    std::string name;
    uint16_t program = 0;
    uint16_t bank = 0;
    uint16_t bagIndex = 0;
};

struct RawInstrumentHeader {
    std::string name;
    uint16_t bagIndex = 0;
};

struct RawBag {
    uint16_t genIndex = 0;
    uint16_t modIndex = 0;
};

struct RawGenerator {
    uint16_t oper = 0;
    uint16_t rawAmount = 0;
    int16_t amount = 0;
};

struct SampleHeader {
    std::string name;
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    uint32_t sampleRate = 44100;
    uint8_t originalPitch = 60;
    int8_t pitchCorrection = 0;
    uint16_t sampleLink = 0;
    uint16_t sampleType = 1;
};

struct GeneratorSet {
    std::unordered_map<uint16_t, int> values;
    bool hasKeyRange = false;
    bool hasVelocityRange = false;
    Range keyRange;
    Range velocityRange;
    int instrument = -1;
    int sampleId = -1;
    int sampleModes = kUnset;
    int rootKey = kUnset;

    bool has(uint16_t op) const {
        return values.find(op) != values.end();
    }

    int value(uint16_t op, int fallback = 0) const {
        const auto it = values.find(op);
        return it == values.end() ? fallback : it->second;
    }
};

GeneratorSet parseGenerators(const std::vector<RawGenerator>& generators, size_t begin, size_t end) {
    GeneratorSet result;
    end = std::min(end, generators.size());
    for (size_t i = begin; i < end; ++i) {
        const auto& gen = generators[i];
        switch (gen.oper) {
            case 41: // instrument
                result.instrument = static_cast<int>(gen.rawAmount);
                break;
            case 43: // keyRange
                result.hasKeyRange = true;
                result.keyRange.lo = static_cast<int>(gen.rawAmount & 0xFFu);
                result.keyRange.hi = static_cast<int>((gen.rawAmount >> 8) & 0xFFu);
                break;
            case 44: // velRange
                result.hasVelocityRange = true;
                result.velocityRange.lo = static_cast<int>(gen.rawAmount & 0xFFu);
                result.velocityRange.hi = static_cast<int>((gen.rawAmount >> 8) & 0xFFu);
                break;
            case 53: // sampleID
                result.sampleId = static_cast<int>(gen.rawAmount);
                break;
            case 54: // sampleModes
                result.sampleModes = static_cast<int>(gen.rawAmount);
                break;
            case 58: // overridingRootKey
                result.rootKey = static_cast<int>(gen.rawAmount);
                break;
            case 60: // endOper
                break;
            default:
                result.values[gen.oper] += static_cast<int>(gen.amount);
                break;
        }
    }
    return result;
}

GeneratorSet mergeGenerators(const GeneratorSet& a, const GeneratorSet& b) {
    GeneratorSet result = a;
    for (const auto& item : b.values) result.values[item.first] += item.second;

    if (b.hasKeyRange) {
        if (!result.hasKeyRange) {
            result.keyRange = b.keyRange;
            result.hasKeyRange = true;
        } else {
            result.keyRange.lo = std::max(result.keyRange.lo, b.keyRange.lo);
            result.keyRange.hi = std::min(result.keyRange.hi, b.keyRange.hi);
        }
    }

    if (b.hasVelocityRange) {
        if (!result.hasVelocityRange) {
            result.velocityRange = b.velocityRange;
            result.hasVelocityRange = true;
        } else {
            result.velocityRange.lo = std::max(result.velocityRange.lo, b.velocityRange.lo);
            result.velocityRange.hi = std::min(result.velocityRange.hi, b.velocityRange.hi);
        }
    }

    if (b.instrument >= 0) result.instrument = b.instrument;
    if (b.sampleId >= 0) result.sampleId = b.sampleId;
    if (b.sampleModes != kUnset) result.sampleModes = b.sampleModes;
    if (b.rootKey != kUnset) result.rootKey = b.rootKey;
    return result;
}

struct Region {
    int sampleId = -1;
    int keyLow = 0;
    int keyHigh = 127;
    int velocityLow = 0;
    int velocityHigh = 127;
    int rootKey = 60;
    int coarseTune = 0;
    int fineTune = 0;
    int scaleTuning = 100;
    int sampleMode = 0;
    int startOffset = 0;
    int endOffset = 0;
    int loopStartOffset = 0;
    int loopEndOffset = 0;
    float attenuationGain = 1.0f;
    float pan = 0.0f;
    float delaySeconds = 0.0f;
    float attackSeconds = 0.001f;
    float holdSeconds = 0.0f;
    float decaySeconds = 0.1f;
    float sustainLevel = 1.0f;
    float releaseSeconds = 0.1f;
};

struct Preset {
    PresetInfo info;
    std::vector<Region> regions;
    std::array<std::vector<uint16_t>, 128> regionsByKey;
    // Estimativa conservadora e O(1) usada apenas para antecipar a carga da CPU.
    // Evita varrer novamente as regiões dentro da callback quando chega um acorde.
    std::array<uint16_t, 128> estimatedLayersByKey{};
};

class MappedFile {
public:
    ~MappedFile() {
        if (data_ && data_ != MAP_FAILED) munmap(data_, size_);
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    static std::shared_ptr<MappedFile> openReadOnly(const std::string& path, std::string& error) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            error = "Não foi possível abrir o arquivo selecionado.";
            return {};
        }

        struct stat info {};
        if (fstat(fd, &info) != 0 || info.st_size <= 0) {
            ::close(fd);
            error = "O arquivo SF2 está vazio ou não pôde ser medido.";
            return {};
        }
        if (static_cast<uint64_t>(info.st_size) > static_cast<uint64_t>(SIZE_MAX)) {
            ::close(fd);
            error = "O arquivo é maior que o espaço de endereçamento disponível.";
            return {};
        }

        void* mapped = mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (mapped == MAP_FAILED) {
            error = "Não foi possível mapear o SF2 na memória virtual do Android.";
            return {};
        }

        auto result = std::shared_ptr<MappedFile>(new MappedFile());
        result->data_ = mapped;
        result->size_ = static_cast<size_t>(info.st_size);
        return result;
    }

    const uint8_t* bytes() const { return static_cast<const uint8_t*>(data_); }
    size_t size() const { return size_; }

    void warm(size_t offset, size_t length) const {
        if (!data_ || data_ == MAP_FAILED || length == 0 || offset >= size_) return;
        const long rawPageSize = sysconf(_SC_PAGESIZE);
        const size_t pageSize = rawPageSize > 0 ? static_cast<size_t>(rawPageSize) : 4096U;
        const size_t begin = offset - (offset % pageSize);
        const size_t boundedEnd = std::min(size_, offset + length);
        const size_t end = std::min(size_, (boundedEnd + pageSize - 1U) / pageSize * pageSize);
        if (end <= begin) return;

        auto* start = static_cast<uint8_t*>(data_) + begin;
        (void)madvise(start, end - begin, MADV_WILLNEED);

        // MADV_WILLNEED é apenas uma sugestão. Tocar uma posição por página fora
        // da callback reduz page faults justamente no ataque dos acordes.
        volatile uint8_t sink = 0;
        for (size_t page = 0; page < end - begin; page += pageSize) {
            sink = static_cast<uint8_t>(sink ^ start[page]);
        }
        (void)sink;
    }

private:
    MappedFile() = default;
    void* data_ = nullptr;
    size_t size_ = 0;
};

struct BankData {
    std::shared_ptr<MappedFile> mappedFile;
    const int16_t* samples = nullptr;
    size_t sampleCount = 0;
    size_t sampleByteOffset = 0;
    std::vector<SampleHeader> sampleHeaders;
    std::vector<Preset> presets;
    std::vector<PresetInfo> infos;
};

struct ChunkView {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

bool findChunks(const uint8_t* bytes,
                size_t byteCount,
                std::unordered_map<std::string, ChunkView>& chunks,
                std::string& error) {
    if (!bytes || byteCount < 12 || std::memcmp(bytes, "RIFF", 4) != 0 ||
        std::memcmp(bytes + 8, "sfbk", 4) != 0) {
        error = "O arquivo não possui uma estrutura SoundFont 2 válida.";
        return false;
    }

    size_t position = 12;
    while (position + 8 <= byteCount) {
        const char* id = reinterpret_cast<const char*>(bytes + position);
        const uint32_t size = readU32(bytes + position + 4);
        const size_t payload = position + 8;
        if (payload + size > byteCount) {
            error = "O arquivo SF2 está truncado.";
            return false;
        }

        if (std::memcmp(id, "LIST", 4) == 0 && size >= 4) {
            const std::string listType(reinterpret_cast<const char*>(bytes + payload), 4);
            size_t child = payload + 4;
            const size_t listEnd = payload + size;
            while (child + 8 <= listEnd) {
                const std::string childId(reinterpret_cast<const char*>(bytes + child), 4);
                const uint32_t childSize = readU32(bytes + child + 4);
                const size_t childPayload = child + 8;
                if (childPayload + childSize > listEnd || childPayload + childSize > byteCount) {
                    error = "Uma seção interna do SF2 está truncada.";
                    return false;
                }
                if (listType == "sdta" || listType == "pdta") {
                    chunks[childId] = {bytes + childPayload, childSize};
                }
                child = childPayload + childSize + (childSize & 1u);
            }
        }
        position = payload + size + (size & 1u);
    }
    return true;
}

bool parseBank(const std::string& path, std::shared_ptr<BankData>& bank, std::string& error) {
    auto mapped = MappedFile::openReadOnly(path, error);
    if (!mapped) return false;

    std::unordered_map<std::string, ChunkView> chunks;
    if (!findChunks(mapped->bytes(), mapped->size(), chunks, error)) return false;

    const char* required[] = {"smpl", "phdr", "pbag", "pgen", "inst", "ibag", "igen", "shdr"};
    for (const char* name : required) {
        if (chunks.find(name) == chunks.end()) {
            error = std::string("O SF2 não contém a seção obrigatória '") + name + "'.";
            return false;
        }
    }

    auto result = std::make_shared<BankData>();
    result->mappedFile = std::move(mapped);

    const ChunkView smpl = chunks["smpl"];
    if ((smpl.size & 1u) != 0u || (reinterpret_cast<uintptr_t>(smpl.data) & 1u) != 0u) {
        error = "A seção de amostras do SF2 é inválida ou está desalinhada.";
        return false;
    }
    result->samples = reinterpret_cast<const int16_t*>(smpl.data);
    result->sampleCount = smpl.size / sizeof(int16_t);
    result->sampleByteOffset = static_cast<size_t>(smpl.data - result->mappedFile->bytes());

    std::vector<RawPresetHeader> presetHeaders;
    const ChunkView phdr = chunks["phdr"];
    if (phdr.size < 38 || phdr.size % 38 != 0) {
        error = "A tabela de presets do SF2 é inválida.";
        return false;
    }
    for (size_t offset = 0; offset + 38 <= phdr.size; offset += 38) {
        RawPresetHeader item;
        item.name = fixedString(phdr.data + offset, 20);
        item.program = readU16(phdr.data + offset + 20);
        item.bank = readU16(phdr.data + offset + 22);
        item.bagIndex = readU16(phdr.data + offset + 24);
        presetHeaders.push_back(std::move(item));
    }

    std::vector<RawBag> presetBags;
    const ChunkView pbag = chunks["pbag"];
    if (pbag.size < 4 || pbag.size % 4 != 0) {
        error = "A tabela de zonas de presets é inválida.";
        return false;
    }
    for (size_t offset = 0; offset + 4 <= pbag.size; offset += 4) {
        presetBags.push_back({readU16(pbag.data + offset), readU16(pbag.data + offset + 2)});
    }

    std::vector<RawGenerator> presetGenerators;
    const ChunkView pgen = chunks["pgen"];
    if (pgen.size % 4 != 0) {
        error = "A tabela de geradores dos presets é inválida.";
        return false;
    }
    for (size_t offset = 0; offset + 4 <= pgen.size; offset += 4) {
        const uint16_t raw = readU16(pgen.data + offset + 2);
        presetGenerators.push_back({readU16(pgen.data + offset), raw, static_cast<int16_t>(raw)});
    }

    std::vector<RawInstrumentHeader> instruments;
    const ChunkView inst = chunks["inst"];
    if (inst.size < 22 || inst.size % 22 != 0) {
        error = "A tabela de instrumentos do SF2 é inválida.";
        return false;
    }
    for (size_t offset = 0; offset + 22 <= inst.size; offset += 22) {
        instruments.push_back({fixedString(inst.data + offset, 20), readU16(inst.data + offset + 20)});
    }

    std::vector<RawBag> instrumentBags;
    const ChunkView ibag = chunks["ibag"];
    if (ibag.size < 4 || ibag.size % 4 != 0) {
        error = "A tabela de zonas dos instrumentos é inválida.";
        return false;
    }
    for (size_t offset = 0; offset + 4 <= ibag.size; offset += 4) {
        instrumentBags.push_back({readU16(ibag.data + offset), readU16(ibag.data + offset + 2)});
    }

    std::vector<RawGenerator> instrumentGenerators;
    const ChunkView igen = chunks["igen"];
    if (igen.size % 4 != 0) {
        error = "A tabela de geradores dos instrumentos é inválida.";
        return false;
    }
    for (size_t offset = 0; offset + 4 <= igen.size; offset += 4) {
        const uint16_t raw = readU16(igen.data + offset + 2);
        instrumentGenerators.push_back({readU16(igen.data + offset), raw, static_cast<int16_t>(raw)});
    }

    const ChunkView shdr = chunks["shdr"];
    if (shdr.size < 46 || shdr.size % 46 != 0) {
        error = "A tabela de amostras do SF2 é inválida.";
        return false;
    }
    for (size_t offset = 0; offset + 46 <= shdr.size; offset += 46) {
        SampleHeader sample;
        sample.name = fixedString(shdr.data + offset, 20);
        sample.start = readU32(shdr.data + offset + 20);
        sample.end = readU32(shdr.data + offset + 24);
        sample.loopStart = readU32(shdr.data + offset + 28);
        sample.loopEnd = readU32(shdr.data + offset + 32);
        sample.sampleRate = readU32(shdr.data + offset + 36);
        sample.originalPitch = shdr.data[offset + 40];
        sample.pitchCorrection = static_cast<int8_t>(shdr.data[offset + 41]);
        sample.sampleLink = readU16(shdr.data + offset + 42);
        sample.sampleType = readU16(shdr.data + offset + 44);
        result->sampleHeaders.push_back(std::move(sample));
    }

    if (presetHeaders.size() < 2 || instruments.size() < 2 || result->sampleHeaders.size() < 2) {
        error = "O SF2 não contém presets, instrumentos ou amostras utilizáveis.";
        return false;
    }

    // Remove os registros terminais EOP, EOI e EOS.
    const size_t actualPresetCount = presetHeaders.size() - 1;
    const size_t actualInstrumentCount = instruments.size() - 1;
    result->sampleHeaders.pop_back();

    for (size_t presetIndex = 0; presetIndex < actualPresetCount; ++presetIndex) {
        const auto& header = presetHeaders[presetIndex];
        const size_t bagBegin = header.bagIndex;
        const size_t bagEnd = presetHeaders[presetIndex + 1].bagIndex;
        if (bagBegin >= presetBags.size() || bagEnd > presetBags.size() || bagBegin > bagEnd) continue;

        Preset preset;
        preset.info = {header.name, header.bank, header.program};
        GeneratorSet presetGlobal;

        for (size_t pz = bagBegin; pz < bagEnd; ++pz) {
            const size_t genBegin = presetBags[pz].genIndex;
            const size_t genEnd = (pz + 1 < presetBags.size()) ? presetBags[pz + 1].genIndex : presetGenerators.size();
            const GeneratorSet presetZone = parseGenerators(presetGenerators, genBegin, genEnd);
            if (presetZone.instrument < 0) {
                presetGlobal = mergeGenerators(presetGlobal, presetZone);
                continue;
            }

            if (presetZone.instrument >= static_cast<int>(actualInstrumentCount)) continue;
            const auto& instrument = instruments[static_cast<size_t>(presetZone.instrument)];
            const size_t instBagBegin = instrument.bagIndex;
            const size_t instBagEnd = instruments[static_cast<size_t>(presetZone.instrument) + 1].bagIndex;
            if (instBagBegin >= instrumentBags.size() || instBagEnd > instrumentBags.size() || instBagBegin > instBagEnd) continue;

            GeneratorSet instrumentGlobal;
            const GeneratorSet presetCombined = mergeGenerators(presetGlobal, presetZone);

            for (size_t iz = instBagBegin; iz < instBagEnd; ++iz) {
                const size_t igenBegin = instrumentBags[iz].genIndex;
                const size_t igenEnd = (iz + 1 < instrumentBags.size()) ? instrumentBags[iz + 1].genIndex : instrumentGenerators.size();
                const GeneratorSet instrumentZone = parseGenerators(instrumentGenerators, igenBegin, igenEnd);
                if (instrumentZone.sampleId < 0) {
                    instrumentGlobal = mergeGenerators(instrumentGlobal, instrumentZone);
                    continue;
                }

                if (instrumentZone.sampleId >= static_cast<int>(result->sampleHeaders.size())) continue;
                GeneratorSet combined = mergeGenerators(presetCombined, instrumentGlobal);
                combined = mergeGenerators(combined, instrumentZone);
                if ((combined.hasKeyRange && combined.keyRange.lo > combined.keyRange.hi) ||
                    (combined.hasVelocityRange && combined.velocityRange.lo > combined.velocityRange.hi)) {
                    continue;
                }

                const auto& sample = result->sampleHeaders[static_cast<size_t>(combined.sampleId)];
                Region region;
                region.sampleId = combined.sampleId;
                region.keyLow = combined.hasKeyRange ? combined.keyRange.lo : 0;
                region.keyHigh = combined.hasKeyRange ? combined.keyRange.hi : 127;
                region.velocityLow = combined.hasVelocityRange ? combined.velocityRange.lo : 0;
                region.velocityHigh = combined.hasVelocityRange ? combined.velocityRange.hi : 127;
                region.rootKey = combined.rootKey != kUnset ? clampInt(combined.rootKey, 0, 127) : sample.originalPitch;
                region.coarseTune = combined.value(51, 0);
                region.fineTune = combined.value(52, 0) + static_cast<int>(sample.pitchCorrection);
                region.scaleTuning = clampInt(combined.has(56) ? combined.value(56) : 100, 0, 1200);
                region.sampleMode = combined.sampleModes == kUnset ? 0 : (combined.sampleModes & 3);
                region.startOffset = combined.value(0, 0) + combined.value(4, 0) * 32768;
                region.endOffset = combined.value(1, 0) + combined.value(12, 0) * 32768;
                region.loopStartOffset = combined.value(2, 0) + combined.value(45, 0) * 32768;
                region.loopEndOffset = combined.value(3, 0) + combined.value(50, 0) * 32768;
                region.attenuationGain = centibelsToGain(static_cast<float>(std::max(0, combined.value(48, 0))));
                region.pan = clampFloat(static_cast<float>(combined.value(17, 0)) / 500.0f, -1.0f, 1.0f);
                region.delaySeconds = timecentsToSeconds(combined.has(33) ? combined.value(33) : kDefaultTimecents);
                region.attackSeconds = timecentsToSeconds(combined.has(34) ? combined.value(34) : kDefaultTimecents);
                region.holdSeconds = timecentsToSeconds(combined.has(35) ? combined.value(35) : kDefaultTimecents);
                region.decaySeconds = timecentsToSeconds(combined.has(36) ? combined.value(36) : kDefaultTimecents);
                region.sustainLevel = centibelsToGain(static_cast<float>(std::max(0, combined.has(37) ? combined.value(37) : 0)));
                region.releaseSeconds = timecentsToSeconds(combined.has(38) ? combined.value(38) : kDefaultTimecents);
                preset.regions.push_back(region);
            }
        }

        if (!preset.regions.empty()) {
            // Índice pronto no carregamento: a callback não precisa percorrer todas
            // as zonas do preset para cada Note On.
            for (size_t regionIndex = 0; regionIndex < preset.regions.size(); ++regionIndex) {
                const Region& region = preset.regions[regionIndex];
                if (regionIndex > std::numeric_limits<uint16_t>::max()) break;
                for (int key = region.keyLow; key <= region.keyHigh; ++key) {
                    const size_t keyIndex = static_cast<size_t>(clampInt(key, 0, 127));
                    preset.regionsByKey[keyIndex].push_back(static_cast<uint16_t>(regionIndex));
                    if (preset.estimatedLayersByKey[keyIndex] <
                        std::numeric_limits<uint16_t>::max()) {
                        ++preset.estimatedLayersByKey[keyIndex];
                    }
                }
            }
            result->infos.push_back(preset.info);
            result->presets.push_back(std::move(preset));
        }
    }

    if (result->presets.empty()) {
        error = "Nenhum preset compatível foi encontrado neste SF2.";
        return false;
    }

    bank = std::move(result);
    return true;
}

struct ChannelState {
    float volume = 1.0f;
    float expression = 1.0f;
    float pan = 0.0f;
    int pitchBend = 8192;
    float pitchRange = 2.0f;
    bool sustain = false;
    int bankMsb = 0;
    int bankLsb = 0;
    int program = 0;
};

enum class EnvelopeStage {
    Delay,
    Attack,
    Hold,
    Decay,
    Sustain,
    Release,
    Done
};

struct Voice {
    const Region* region = nullptr;
    const SampleHeader* sample = nullptr;
    int channel = 0;
    int note = 60;
    int velocity = 100;
    bool keyReleased = false;
    bool heldBySustain = false;
    bool releasePending = false;
    int minimumHoldSamplesLeft = 0;
    uint64_t age = 0;
    uint64_t noteInstance = 0;
    uint64_t samplePhase = 0;
    uint64_t phaseIncrement = kPhaseOne;
    uint32_t absoluteStart = 0;
    uint32_t absoluteEnd = 0;
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    EnvelopeStage stage = EnvelopeStage::Delay;
    int stageSamplesLeft = 0;
    float envelope = 0.0f;
    float stageDelta = 0.0f;
    float releaseDelta = 0.0f;
    float baseGain = 1.0f;
    float leftGain = 0.707f;
    float rightGain = 0.707f;
};

} // namespace

struct NoteInstanceQueue {
    static constexpr size_t capacity = 64;
    std::array<uint64_t, capacity> ids{};
    uint8_t head = 0;
    uint8_t count = 0;

    void clear() {
        head = 0;
        count = 0;
    }

    void push(uint64_t id) {
        if (count == capacity) {
            head = static_cast<uint8_t>((head + 1U) % capacity);
            --count;
        }
        const size_t tail = (static_cast<size_t>(head) + count) % capacity;
        ids[tail] = id;
        ++count;
    }

    uint64_t pop() {
        if (count == 0) return 0;
        const uint64_t id = ids[head];
        head = static_cast<uint8_t>((head + 1U) % capacity);
        --count;
        return id;
    }
};

struct Sf2Synth::Impl {
    std::shared_ptr<BankData> bank;
    std::vector<Voice> voices;
    std::vector<PresetInfo> emptyInfos;
    std::array<ChannelState, 16> channels{};
    std::array<std::array<NoteInstanceQueue, 128>, 16> noteInstances{};
    int sampleRate = 48000;
    int selectedPreset = 0;
    float masterVolume = 0.8f;
    float releaseMilliseconds = 800.0f;
    size_t maxVoices = 64;
    static constexpr size_t voiceCapacity = 96;
    static constexpr int minPitchCents = -30000;
    static constexpr int maxPitchCents = 30000;
    static constexpr size_t pitchTableSize =
        static_cast<size_t>(maxPitchCents - minPitchCents + 1);
    std::array<float, pitchTableSize> pitchRatioByCent{};
    std::array<float, 257> panLeft{};
    std::array<float, 257> panRight{};
    uint64_t voiceAge = 0;
    uint64_t noteInstanceCounter = 0;

    Impl() {
        // Custos trigonométricos e exponenciais ficam fora da callback.
        for (int cents = minPitchCents; cents <= maxPitchCents; ++cents) {
            pitchRatioByCent[static_cast<size_t>(cents - minPitchCents)] =
                std::exp2(static_cast<float>(cents) / 1200.0f);
        }
        for (size_t index = 0; index < panLeft.size(); ++index) {
            const float pan = static_cast<float>(index) / 128.0f - 1.0f;
            const float angle = (pan + 1.0f) * kPi * 0.25f;
            panLeft[index] = std::cos(angle);
            panRight[index] = std::sin(angle);
        }
    }

    float pitchRatio(int cents) const {
        cents = clampInt(cents, minPitchCents, maxPitchCents);
        return pitchRatioByCent[static_cast<size_t>(cents - minPitchCents)];
    }

    void resetChannels() {
        channels = {};
        for (auto& channelQueues : noteInstances) {
            for (auto& queue : channelQueues) queue.clear();
        }
        for (auto& channel : channels) {
            channel.volume = 1.0f;
            channel.expression = 1.0f;
            channel.pitchBend = 8192;
            channel.pitchRange = 2.0f;
        }
    }

    void startStage(Voice& voice, EnvelopeStage stage) {
        voice.stage = stage;
        const Region& r = *voice.region;
        switch (stage) {
            case EnvelopeStage::Delay:
                voice.envelope = 0.0f;
                voice.stageSamplesLeft = static_cast<int>(r.delaySeconds * sampleRate);
                if (voice.stageSamplesLeft <= 0) startStage(voice, EnvelopeStage::Attack);
                break;
            case EnvelopeStage::Attack:
                voice.stageSamplesLeft = static_cast<int>(std::max(0.001f, r.attackSeconds) * sampleRate);
                voice.stageDelta = (1.0f - voice.envelope) / static_cast<float>(std::max(1, voice.stageSamplesLeft));
                break;
            case EnvelopeStage::Hold:
                voice.envelope = 1.0f;
                voice.stageSamplesLeft = static_cast<int>(r.holdSeconds * sampleRate);
                if (voice.stageSamplesLeft <= 0) startStage(voice, EnvelopeStage::Decay);
                break;
            case EnvelopeStage::Decay:
                voice.stageSamplesLeft = static_cast<int>(std::max(0.001f, r.decaySeconds) * sampleRate);
                voice.stageDelta = (r.sustainLevel - voice.envelope) / static_cast<float>(std::max(1, voice.stageSamplesLeft));
                break;
            case EnvelopeStage::Sustain:
                voice.envelope = r.sustainLevel;
                voice.stageSamplesLeft = 0;
                voice.stageDelta = 0.0f;
                break;
            case EnvelopeStage::Release: {
                const float chosenRelease = releaseMilliseconds >= 1.0f
                    ? releaseMilliseconds / 1000.0f
                    : std::max(0.01f, r.releaseSeconds);
                voice.stageSamplesLeft = static_cast<int>(std::max(0.01f, chosenRelease) * sampleRate);
                voice.releaseDelta = voice.envelope / static_cast<float>(std::max(1, voice.stageSamplesLeft));
                break;
            }
            case EnvelopeStage::Done:
                voice.envelope = 0.0f;
                voice.stageSamplesLeft = 0;
                break;
        }
    }

    void advanceEnvelope(Voice& voice) {
        switch (voice.stage) {
            case EnvelopeStage::Delay:
                if (--voice.stageSamplesLeft <= 0) startStage(voice, EnvelopeStage::Attack);
                break;
            case EnvelopeStage::Attack:
                voice.envelope += voice.stageDelta;
                if (--voice.stageSamplesLeft <= 0 || voice.envelope >= 1.0f) {
                    voice.envelope = 1.0f;
                    startStage(voice, EnvelopeStage::Hold);
                }
                break;
            case EnvelopeStage::Hold:
                if (--voice.stageSamplesLeft <= 0) startStage(voice, EnvelopeStage::Decay);
                break;
            case EnvelopeStage::Decay:
                voice.envelope += voice.stageDelta;
                if (--voice.stageSamplesLeft <= 0) startStage(voice, EnvelopeStage::Sustain);
                break;
            case EnvelopeStage::Sustain:
                break;
            case EnvelopeStage::Release:
                voice.envelope = std::max(0.0f, voice.envelope - voice.releaseDelta);
                if (--voice.stageSamplesLeft <= 0 || voice.envelope <= 0.00001f) startStage(voice, EnvelopeStage::Done);
                break;
            case EnvelopeStage::Done:
                break;
        }
    }

    void beginRelease(Voice& voice) {
        voice.keyReleased = true;
        voice.heldBySustain = false;
        if (voice.stage == EnvelopeStage::Done || voice.stage == EnvelopeStage::Release) return;

        // Alguns teclados agrupam Note On e Note Off rápidos no mesmo pacote USB.
        // Sem esta pequena janela, a voz poderia entrar em release antes de
        // renderizar qualquer amostra e a repetição pareceria perdida. O ataque
        // continua imediato; apenas garantimos cerca de 0,5 ms de vida mínima.
        if (voice.minimumHoldSamplesLeft > 0) {
            voice.releasePending = true;
            return;
        }
        voice.releasePending = false;
        startStage(voice, EnvelopeStage::Release);
    }

    void updateVoicePitch(Voice& voice) {
        const ChannelState& channel = channels[static_cast<size_t>(voice.channel)];
        const float bendSemitones =
            (static_cast<float>(channel.pitchBend) - 8192.0f) / 8192.0f * channel.pitchRange;
        const int cents =
            (voice.note - voice.region->rootKey) * voice.region->scaleTuning +
            voice.region->coarseTune * 100 +
            voice.region->fineTune +
            static_cast<int>(std::lrintf(bendSemitones * 100.0f));
        const double increment =
            static_cast<double>(voice.sample->sampleRate) /
            static_cast<double>(sampleRate) *
            static_cast<double>(pitchRatio(cents));
        voice.phaseIncrement = std::max<uint64_t>(
            1,
            static_cast<uint64_t>(std::llround(increment * static_cast<double>(kPhaseOne))));
    }

    int findPresetByBankProgram(int bankNumber, int program) const {
        if (!bank) return -1;
        for (size_t i = 0; i < bank->presets.size(); ++i) {
            const auto& info = bank->presets[i].info;
            if (static_cast<int>(info.bank) == bankNumber && static_cast<int>(info.program) == program) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void beginFastRelease(Voice& voice, float milliseconds = 1.0f) {
        if (voice.stage == EnvelopeStage::Done) return;
        const int fastSamples = std::max(
            1,
            static_cast<int>(milliseconds * 0.001f * static_cast<float>(sampleRate)));
        // Não reinicie a contagem a cada novo retrigger; apenas encurte caudas
        // que ainda seriam mais longas que o fade rápido.
        if (voice.stage == EnvelopeStage::Release &&
            voice.stageSamplesLeft > 0 &&
            voice.stageSamplesLeft <= fastSamples) {
            return;
        }
        voice.keyReleased = true;
        voice.heldBySustain = false;
        voice.releasePending = false;
        voice.minimumHoldSamplesLeft = 0;
        voice.stage = EnvelopeStage::Release;
        voice.stageSamplesLeft = fastSamples;
        voice.releaseDelta = voice.envelope /
            static_cast<float>(voice.stageSamplesLeft);
    }

    void removeVoiceAt(size_t index) {
        if (index >= voices.size()) return;
        if (index + 1U != voices.size()) voices[index] = voices.back();
        voices.pop_back();
    }

    void compactDoneVoices() {
        size_t index = 0;
        while (index < voices.size()) {
            if (voices[index].stage == EnvelopeStage::Done) {
                removeVoiceAt(index);
            } else {
                ++index;
            }
        }
    }

    static float stealScore(const Voice& voice) {
        float score = voice.envelope;
        if (voice.stage == EnvelopeStage::Done) score -= 2.0f;
        else if (voice.stage == EnvelopeStage::Release) score -= 1.0f;
        else if (voice.keyReleased) score -= 0.25f;
        if (voice.heldBySustain) score += 0.15f;
        return score;
    }

    size_t chooseStealCandidate() const {
        size_t bestIndex = 0;
        float bestScore = std::numeric_limits<float>::max();
        uint64_t bestAge = std::numeric_limits<uint64_t>::max();
        for (size_t index = 0; index < voices.size(); ++index) {
            const Voice& voice = voices[index];
            const float score = stealScore(voice);
            if (score < bestScore - 0.001f ||
                (std::fabs(score - bestScore) <= 0.001f && voice.age < bestAge)) {
                bestIndex = index;
                bestScore = score;
                bestAge = voice.age;
            }
        }
        return bestIndex;
    }

    void shedExcessVoicesIncrementally() {
        compactDoneVoices();
        if (voices.size() <= maxVoices) return;

        // A política de carga reduz o limite em blocos de oito vozes. Em vez de
        // ordenar toda a lista dentro da callback, selecionamos no máximo oito
        // candidatos com varreduras limitadas e aplicamos um fade de 0,5 ms.
        // Isso mantém o custo previsível e evita clique ao aliviar a CPU.
        constexpr size_t kMaximumActionsPerCallback = 8;
        const size_t actions = std::min(
            kMaximumActionsPerCallback,
            voices.size() - maxVoices);
        std::array<uint8_t, voiceCapacity> selected{};

        for (size_t action = 0; action < actions; ++action) {
            size_t bestIndex = voices.size();
            float bestScore = std::numeric_limits<float>::max();
            uint64_t bestAge = std::numeric_limits<uint64_t>::max();
            for (size_t index = 0; index < voices.size(); ++index) {
                if (selected[index] != 0) continue;
                const Voice& voice = voices[index];
                const float score = stealScore(voice);
                if (score < bestScore - 0.001f ||
                    (std::fabs(score - bestScore) <= 0.001f && voice.age < bestAge)) {
                    bestIndex = index;
                    bestScore = score;
                    bestAge = voice.age;
                }
            }
            if (bestIndex >= voices.size()) break;
            selected[bestIndex] = 1;
            beginFastRelease(voices[bestIndex], 0.5f);
        }
    }

    Voice& allocateVoice() {
        if (voices.size() < maxVoices) {
            voices.emplace_back();
            return voices.back();
        }
        const size_t candidate = chooseStealCandidate();
        voices[candidate] = Voice{};
        return voices[candidate];
    }

    void noteOn(int channelIndex, int note, int velocity) {
        if (!bank || bank->presets.empty()) return;
        channelIndex = clampInt(channelIndex, 0, 15);
        note = clampInt(note, 0, 127);
        velocity = clampInt(velocity, 1, 127);
        const int presetIndex = clampInt(selectedPreset, 0, static_cast<int>(bank->presets.size()) - 1);
        const Preset& preset = bank->presets[static_cast<size_t>(presetIndex)];
        compactDoneVoices();
        const uint64_t noteInstance = ++noteInstanceCounter;
        bool startedVoice = false;

        // Retrigger rápido: uma execução anterior da mesma tecla pode ainda não
        // ter recebido seu Note Off quando o novo Note On chega. Faça fade também
        // dessa voz ativa, não apenas das caudas já soltas. Vozes sustentadas pelo
        // pedal continuam ressoando; as demais deixam espaço para o novo ataque.
        for (auto& existing : voices) {
            if (existing.channel == channelIndex &&
                existing.note == note &&
                !existing.heldBySustain &&
                existing.stage != EnvelopeStage::Done) {
                beginFastRelease(existing, 0.75f);
            }
        }

        const auto& candidates = preset.regionsByKey[static_cast<size_t>(note)];
        for (uint16_t regionIndex : candidates) {
            if (regionIndex >= preset.regions.size()) continue;
            const Region& region = preset.regions[regionIndex];
            if (velocity < region.velocityLow || velocity > region.velocityHigh) continue;
            if (region.sampleId < 0 || region.sampleId >= static_cast<int>(bank->sampleHeaders.size())) continue;
            const SampleHeader& sample = bank->sampleHeaders[static_cast<size_t>(region.sampleId)];

            const int64_t start = static_cast<int64_t>(sample.start) + region.startOffset;
            const int64_t end = static_cast<int64_t>(sample.end) + region.endOffset;
            const int64_t loopStart = static_cast<int64_t>(sample.loopStart) + region.loopStartOffset;
            const int64_t loopEnd = static_cast<int64_t>(sample.loopEnd) + region.loopEndOffset;
            if (start < 0 || end <= start + 1 || end > static_cast<int64_t>(bank->sampleCount)) continue;

            Voice& voice = allocateVoice();
            voice.region = &region;
            voice.sample = &sample;
            voice.channel = channelIndex;
            voice.note = note;
            voice.velocity = velocity;
            voice.age = ++voiceAge;
            voice.noteInstance = noteInstance;
            voice.minimumHoldSamplesLeft = std::max(8, sampleRate / 2000);
            voice.releasePending = false;
            voice.absoluteStart = static_cast<uint32_t>(start);
            voice.absoluteEnd = static_cast<uint32_t>(end);
            voice.loopStart = static_cast<uint32_t>(clampInt(static_cast<int>(loopStart), static_cast<int>(start), static_cast<int>(end - 1)));
            voice.loopEnd = static_cast<uint32_t>(clampInt(static_cast<int>(loopEnd), static_cast<int>(voice.loopStart + 1), static_cast<int>(end)));
            voice.samplePhase = static_cast<uint64_t>(voice.absoluteStart) << 32;

            const float velocityGain = static_cast<float>(velocity) / 127.0f;
            voice.baseGain = region.attenuationGain * velocityGain;
            const float pan = clampFloat(
                region.pan + channels[static_cast<size_t>(channelIndex)].pan,
                -1.0f,
                1.0f);
            const int panIndex = clampInt(
                static_cast<int>(std::lrintf((pan + 1.0f) * 128.0f)),
                0,
                256);
            voice.leftGain = panLeft[static_cast<size_t>(panIndex)];
            voice.rightGain = panRight[static_cast<size_t>(panIndex)];
            updateVoicePitch(voice);
            startStage(voice, EnvelopeStage::Delay);
            startedVoice = true;
        }

        if (startedVoice) {
            noteInstances[static_cast<size_t>(channelIndex)][static_cast<size_t>(note)]
                .push(noteInstance);
        }
    }

    void noteOff(int channelIndex, int note) {
        channelIndex = clampInt(channelIndex, 0, 15);
        note = clampInt(note, 0, 127);
        const bool sustain = channels[static_cast<size_t>(channelIndex)].sustain;

        // Cada Note Off consome exatamente uma execução de Note On. Mesmo que a
        // voz antiga já tenha terminado o fade ou sido roubada, não avance para
        // a execução seguinte: esse era o corte intermitente em retriggers rápidos.
        auto& instanceQueue =
            noteInstances[static_cast<size_t>(channelIndex)][static_cast<size_t>(note)];
        const uint64_t targetInstance = instanceQueue.pop();
        if (targetInstance == 0) return;

        for (auto& voice : voices) {
            if (voice.channel != channelIndex ||
                voice.note != note ||
                voice.keyReleased ||
                voice.noteInstance != targetInstance) {
                continue;
            }
            voice.keyReleased = true;
            if (sustain) {
                voice.heldBySustain = true;
            } else {
                beginRelease(voice);
            }
        }
    }

};

Sf2Synth::Sf2Synth() : impl_(std::make_unique<Impl>()) {
    impl_->voices.reserve(Impl::voiceCapacity);
    impl_->resetChannels();
}

Sf2Synth::~Sf2Synth() = default;

bool Sf2Synth::loadFromFile(const std::string& path, std::string& error) {
    std::shared_ptr<BankData> newBank;
    if (!parseBank(path, newBank, error)) return false;
    impl_->bank = std::move(newBank);
    impl_->voices.clear();
    impl_->selectedPreset = 0;
    impl_->resetChannels();
    return true;
}

void Sf2Synth::unload() {
    impl_->voices.clear();
    impl_->bank.reset();
    impl_->selectedPreset = 0;
}

bool Sf2Synth::isLoaded() const {
    return static_cast<bool>(impl_->bank);
}

const std::vector<PresetInfo>& Sf2Synth::presets() const {
    return impl_->bank ? impl_->bank->infos : impl_->emptyInfos;
}

bool Sf2Synth::selectPreset(int index) {
    if (!impl_->bank || index < 0 || index >= static_cast<int>(impl_->bank->presets.size())) return false;
    impl_->selectedPreset = index;
    impl_->voices.clear();
    for (auto& channelQueues : impl_->noteInstances) {
        for (auto& queue : channelQueues) queue.clear();
    }
    return true;
}

void Sf2Synth::prefetchPreset(int index) const {
    if (!impl_->bank || !impl_->bank->mappedFile || index < 0 ||
        index >= static_cast<int>(impl_->bank->presets.size())) {
        return;
    }

    constexpr size_t kAttackWarmBytes = 256U * 1024U;
    constexpr size_t kLoopWarmBytes = 64U * 1024U;
    std::unordered_set<int> warmedSamples;
    const Preset& preset = impl_->bank->presets[static_cast<size_t>(index)];

    for (const Region& region : preset.regions) {
        if (region.sampleId < 0 ||
            region.sampleId >= static_cast<int>(impl_->bank->sampleHeaders.size()) ||
            !warmedSamples.insert(region.sampleId).second) {
            continue;
        }

        const SampleHeader& sample =
            impl_->bank->sampleHeaders[static_cast<size_t>(region.sampleId)];
        const int64_t start = static_cast<int64_t>(sample.start) + region.startOffset;
        const int64_t end = static_cast<int64_t>(sample.end) + region.endOffset;
        if (start < 0 || end <= start || end > static_cast<int64_t>(impl_->bank->sampleCount)) {
            continue;
        }

        const size_t sampleBytes = static_cast<size_t>(end - start) * sizeof(int16_t);
        const size_t attackOffset =
            impl_->bank->sampleByteOffset + static_cast<size_t>(start) * sizeof(int16_t);
        impl_->bank->mappedFile->warm(
            attackOffset,
            std::min(kAttackWarmBytes, sampleBytes));

        const int64_t loopStart = static_cast<int64_t>(sample.loopStart) + region.loopStartOffset;
        if (loopStart > start && loopStart < end) {
            const size_t loopOffset =
                impl_->bank->sampleByteOffset + static_cast<size_t>(loopStart) * sizeof(int16_t);
            impl_->bank->mappedFile->warm(
                loopOffset,
                std::min(kLoopWarmBytes, static_cast<size_t>(end - loopStart) * sizeof(int16_t)));
        }
    }
}

int Sf2Synth::selectedPreset() const {
    return impl_->selectedPreset;
}

void Sf2Synth::setSampleRate(int sampleRate) {
    impl_->sampleRate = std::max(8000, sampleRate);
    for (auto& voice : impl_->voices) impl_->updateVoicePitch(voice);
}

void Sf2Synth::setMasterVolume(float volume) {
    impl_->masterVolume = clampFloat(volume, 0.0f, 1.5f);
}

void Sf2Synth::setReleaseMilliseconds(float milliseconds) {
    impl_->releaseMilliseconds = clampFloat(milliseconds, 0.0f, 10000.0f);
}

void Sf2Synth::setMaxVoices(size_t count) {
    // Somente atualiza a meta. A redução é incremental e sem ordenação no início
    // da próxima renderização, preservando as regras de tempo real da callback.
    impl_->maxVoices = std::max<size_t>(24, std::min<size_t>(count, Impl::voiceCapacity));
}

void Sf2Synth::noteOn(int channel, int note, int velocity) {
    impl_->noteOn(channel, note, velocity);
}

void Sf2Synth::noteOff(int channel, int note) {
    impl_->noteOff(channel, note);
}

void Sf2Synth::controlChange(int channel, int controller, int value) {
    channel = clampInt(channel, 0, 15);
    controller = clampInt(controller, 0, 127);
    value = clampInt(value, 0, 127);
    auto& state = impl_->channels[static_cast<size_t>(channel)];
    switch (controller) {
        case 0:
            state.bankMsb = value;
            break;
        case 7:
            state.volume = static_cast<float>(value) / 127.0f;
            break;
        case 10:
            state.pan = (static_cast<float>(value) - 64.0f) / 64.0f;
            break;
        case 11:
            state.expression = static_cast<float>(value) / 127.0f;
            break;
        case 32:
            state.bankLsb = value;
            break;
        case 64: {
            const bool newSustain = value >= 64;
            if (state.sustain && !newSustain) {
                for (auto& voice : impl_->voices) {
                    if (voice.channel == channel && voice.heldBySustain) impl_->beginRelease(voice);
                }
            }
            state.sustain = newSustain;
            break;
        }
        case 120:
            for (auto& voice : impl_->voices) {
                if (voice.channel == channel) voice.stage = EnvelopeStage::Done;
            }
            for (auto& queue : impl_->noteInstances[static_cast<size_t>(channel)]) queue.clear();
            break;
        case 121:
            if (state.sustain) {
                for (auto& voice : impl_->voices) {
                    if (voice.channel == channel && voice.heldBySustain) impl_->beginRelease(voice);
                }
            }
            state = ChannelState{};
            state.volume = 1.0f;
            state.expression = 1.0f;
            state.pitchBend = 8192;
            state.pitchRange = 2.0f;
            break;
        case 123:
            for (auto& voice : impl_->voices) {
                if (voice.channel == channel) impl_->beginRelease(voice);
            }
            for (auto& queue : impl_->noteInstances[static_cast<size_t>(channel)]) queue.clear();
            break;
        default:
            break;
    }
}

void Sf2Synth::pitchBend(int channel, int value14) {
    channel = clampInt(channel, 0, 15);
    impl_->channels[static_cast<size_t>(channel)].pitchBend = clampInt(value14, 0, 16383);
    for (auto& voice : impl_->voices) {
        if (voice.channel == channel) impl_->updateVoicePitch(voice);
    }
}

void Sf2Synth::programChange(int channel, int program) {
    channel = clampInt(channel, 0, 15);
    program = clampInt(program, 0, 127);
    auto& state = impl_->channels[static_cast<size_t>(channel)];
    state.program = program;
    const int bankNumber = state.bankMsb * 128 + state.bankLsb;
    const int preset = impl_->findPresetByBankProgram(bankNumber, program);
    if (preset >= 0) selectPreset(preset);
}

void Sf2Synth::allNotesOff(bool immediate) {
    for (auto& channelQueues : impl_->noteInstances) {
        for (auto& queue : channelQueues) queue.clear();
    }
    if (immediate) {
        impl_->voices.clear();
        return;
    }
    for (auto& voice : impl_->voices) impl_->beginRelease(voice);
}

void Sf2Synth::render(float* output, int frames) {
    if (!output || frames <= 0) return;
    const size_t outputSamples = static_cast<size_t>(frames) * 2U;
    std::fill(output, output + outputSamples, 0.0f);
    if (!impl_->bank) return;
    impl_->shedExcessVoicesIncrementally();

    constexpr float kSampleScale = 1.0f / 32768.0f;
    const int16_t* samples = impl_->bank->samples;
    const size_t sampleCount = impl_->bank->sampleCount;

    // Renderização por voz: mantém o estado da voz em registradores durante todo
    // o bloco e evita percorrer a lista completa de vozes a cada frame.
    for (auto& voice : impl_->voices) {
        if (voice.stage == EnvelopeStage::Done) continue;

        uint64_t phase = voice.samplePhase;
        const uint64_t phaseIncrement = voice.phaseIncrement;
        float envelope = voice.envelope;
        int stageSamplesLeft = voice.stageSamplesLeft;
        float stageDelta = voice.stageDelta;
        float releaseDelta = voice.releaseDelta;
        EnvelopeStage stage = voice.stage;
        int minimumHoldSamplesLeft = voice.minimumHoldSamplesLeft;
        bool releasePending = voice.releasePending;

        const ChannelState& channel = impl_->channels[static_cast<size_t>(voice.channel)];
        const float channelGain = channel.volume * channel.expression;
        const float leftGain = voice.baseGain * channelGain * voice.leftGain;
        const float rightGain = voice.baseGain * channelGain * voice.rightGain;
        const bool loopContinuous = voice.region->sampleMode == 1;
        const uint32_t loopStart = voice.loopStart;
        const uint32_t loopEnd = voice.loopEnd;
        const uint64_t loopStartPhase = static_cast<uint64_t>(loopStart) << 32;
        const uint64_t loopEndPhase = static_cast<uint64_t>(loopEnd) << 32;
        const uint64_t loopLengthPhase = loopEndPhase - loopStartPhase;

        for (int frame = 0; frame < frames; ++frame) {
            const uint32_t index = static_cast<uint32_t>(phase >> 32);
            if (index + 1 >= voice.absoluteEnd || index + 1 >= sampleCount) {
                stage = EnvelopeStage::Done;
                envelope = 0.0f;
                break;
            }

            const float fraction =
                static_cast<float>(phase & kPhaseFractionMask) * kPhaseFractionScale;
            const float s0 = static_cast<float>(samples[index]) * kSampleScale;
            const float s1 = static_cast<float>(samples[index + 1]) * kSampleScale;
            const float sampleValue = s0 + (s1 - s0) * fraction;
            const float value = sampleValue * envelope;
            output[frame * 2] += value * leftGain;
            output[frame * 2 + 1] += value * rightGain;

            switch (stage) {
                case EnvelopeStage::Delay:
                    if (--stageSamplesLeft <= 0) {
                        stage = EnvelopeStage::Attack;
                        stageSamplesLeft = std::max(
                            1,
                            static_cast<int>(std::max(0.001f, voice.region->attackSeconds) * impl_->sampleRate));
                        stageDelta = (1.0f - envelope) / static_cast<float>(stageSamplesLeft);
                    }
                    break;
                case EnvelopeStage::Attack:
                    envelope += stageDelta;
                    if (--stageSamplesLeft <= 0 || envelope >= 1.0f) {
                        envelope = 1.0f;
                        stage = EnvelopeStage::Hold;
                        stageSamplesLeft = static_cast<int>(voice.region->holdSeconds * impl_->sampleRate);
                        if (stageSamplesLeft <= 0) {
                            stage = EnvelopeStage::Decay;
                            stageSamplesLeft = std::max(
                                1,
                                static_cast<int>(std::max(0.001f, voice.region->decaySeconds) * impl_->sampleRate));
                            stageDelta = (voice.region->sustainLevel - envelope) /
                                static_cast<float>(stageSamplesLeft);
                        }
                    }
                    break;
                case EnvelopeStage::Hold:
                    if (--stageSamplesLeft <= 0) {
                        stage = EnvelopeStage::Decay;
                        stageSamplesLeft = std::max(
                            1,
                            static_cast<int>(std::max(0.001f, voice.region->decaySeconds) * impl_->sampleRate));
                        stageDelta = (voice.region->sustainLevel - envelope) /
                            static_cast<float>(stageSamplesLeft);
                    }
                    break;
                case EnvelopeStage::Decay:
                    envelope += stageDelta;
                    if (--stageSamplesLeft <= 0) {
                        envelope = voice.region->sustainLevel;
                        stage = EnvelopeStage::Sustain;
                    }
                    break;
                case EnvelopeStage::Sustain:
                    break;
                case EnvelopeStage::Release:
                    envelope = std::max(0.0f, envelope - releaseDelta);
                    if (--stageSamplesLeft <= 0 || envelope <= 0.00001f) {
                        envelope = 0.0f;
                        stage = EnvelopeStage::Done;
                    }
                    break;
                case EnvelopeStage::Done:
                    break;
            }

            if (minimumHoldSamplesLeft > 0) --minimumHoldSamplesLeft;
            if (releasePending && minimumHoldSamplesLeft <= 0 &&
                stage != EnvelopeStage::Done && stage != EnvelopeStage::Release) {
                const float chosenRelease = impl_->releaseMilliseconds >= 1.0f
                    ? impl_->releaseMilliseconds / 1000.0f
                    : std::max(0.01f, voice.region->releaseSeconds);
                stage = EnvelopeStage::Release;
                stageSamplesLeft = std::max(
                    1,
                    static_cast<int>(std::max(0.01f, chosenRelease) * impl_->sampleRate));
                releaseDelta = envelope / static_cast<float>(stageSamplesLeft);
                releasePending = false;
            }

            phase += phaseIncrement;
            const bool loopUntilRelease =
                voice.region->sampleMode == 3 && stage != EnvelopeStage::Release;
            if ((loopContinuous || loopUntilRelease) &&
                loopLengthPhase > kPhaseOne &&
                phase >= loopEndPhase) {
                // A fase fixa 32.32 evita divisão e operações double no caminho
                // quente. O caso comum exige apenas uma subtração.
                phase = loopStartPhase + (phase - loopEndPhase);
                if (phase >= loopEndPhase) phase = loopStartPhase;
            }
        }

        voice.samplePhase = phase;
        voice.envelope = envelope;
        voice.stageSamplesLeft = stageSamplesLeft;
        voice.stageDelta = stageDelta;
        voice.releaseDelta = releaseDelta;
        voice.stage = stage;
        voice.minimumHoldSamplesLeft = minimumHoldSamplesLeft;
        voice.releasePending = releasePending;
    }

    const float volume = impl_->masterVolume;
    for (size_t sample = 0; sample < outputSamples; ++sample) {
        output[sample] = clampFloat(output[sample] * volume, -1.0f, 1.0f);
    }

    impl_->compactDoneVoices();
}

int Sf2Synth::activeVoiceCount() const {
    return static_cast<int>(impl_->voices.size());
}

int Sf2Synth::estimateVoiceCountForNote(int note, int velocity) const {
    if (!impl_->bank || impl_->bank->presets.empty()) return 0;
    (void)velocity;
    note = clampInt(note, 0, 127);
    const int presetIndex = clampInt(
        impl_->selectedPreset,
        0,
        static_cast<int>(impl_->bank->presets.size()) - 1);
    const Preset& preset = impl_->bank->presets[static_cast<size_t>(presetIndex)];

    // Estimativa conservadora: pode superestimar presets divididos por velocity,
    // mas nunca cria vozes nem percorre vetores na callback. Para performance hints,
    // antecipar um pouco a carga é preferível a reagir uma callback atrasado.
    return static_cast<int>(preset.estimatedLayersByKey[static_cast<size_t>(note)]);
}

} // namespace sf2live
