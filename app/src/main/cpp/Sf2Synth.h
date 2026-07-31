#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sf2live {

struct PresetInfo {
    std::string name;
    uint16_t bank = 0;
    uint16_t program = 0;
};

class Sf2Synth {
public:
    Sf2Synth();
    ~Sf2Synth();

    Sf2Synth(const Sf2Synth&) = delete;
    Sf2Synth& operator=(const Sf2Synth&) = delete;

    bool loadFromFile(const std::string& path, std::string& error);
    void unload();
    bool isLoaded() const;

    const std::vector<PresetInfo>& presets() const;
    bool selectPreset(int index);
    void prefetchPreset(int index) const;
    int selectedPreset() const;

    void setSampleRate(int sampleRate);
    void setMasterVolume(float volume);
    void setReleaseMilliseconds(float milliseconds);
    void setMaxVoices(size_t count);

    void noteOn(int channel, int note, int velocity);
    void noteOff(int channel, int note);
    void controlChange(int channel, int controller, int value);
    void pitchBend(int channel, int value14);
    void programChange(int channel, int program);
    void allNotesOff(bool immediate);

    void render(float* stereoInterleaved, int frames);
    int activeVoiceCount() const;
    int estimateVoiceCountForNote(int note, int velocity) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sf2live
