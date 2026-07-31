#pragma once

#include <algorithm>
#include <cstdint>

namespace sf2live {

struct RealtimeProfile {
    int bufferBursts;
    int targetVoices;
    int minimumVoices;
    int overloadPermille;
    int recoveryPermille;

    static constexpr RealtimeProfile fromId(int id) noexcept {
        switch (id) {
            case 0: return {1, 48, 32, 700, 430};
            case 2: return {2, 96, 40, 820, 430};
            default: return {1, 64, 32, 760, 430};
        }
    }
};

class RealtimeLoadController {
public:
    void reset(int profileId) noexcept {
        profile_ = RealtimeProfile::fromId(profileId);
        currentVoiceLimit_ = profile_.targetVoices;
        smoothedLoadPermille_ = 0;
        overloadStreak_ = 0;
        headroomStreak_ = 0;
    }

    int update(int32_t frames, int sampleRate, int64_t elapsedNanos) noexcept {
        if (frames <= 0 || sampleRate <= 0 || elapsedNanos <= 0) return currentVoiceLimit_;
        const int64_t budget = static_cast<int64_t>(frames) * 1000000000LL / sampleRate;
        if (budget <= 0) return currentVoiceLimit_;

        const int instant = static_cast<int>(std::min<int64_t>(2000, elapsedNanos * 1000LL / budget));
        smoothedLoadPermille_ = smoothedLoadPermille_ == 0
            ? instant
            : (smoothedLoadPermille_ * 7 + instant * 3) / 10;

        if (smoothedLoadPermille_ >= profile_.overloadPermille) {
            ++overloadStreak_;
            headroomStreak_ = 0;
            if (overloadStreak_ >= 3 && currentVoiceLimit_ > profile_.minimumVoices) {
                currentVoiceLimit_ = std::max(profile_.minimumVoices, currentVoiceLimit_ - 8);
                overloadStreak_ = 0;
            }
        } else if (smoothedLoadPermille_ <= profile_.recoveryPermille) {
            overloadStreak_ = 0;
            ++headroomStreak_;
            if (headroomStreak_ >= 240 && currentVoiceLimit_ < profile_.targetVoices) {
                currentVoiceLimit_ = std::min(profile_.targetVoices, currentVoiceLimit_ + 8);
                headroomStreak_ = 0;
            }
        } else {
            overloadStreak_ = 0;
            headroomStreak_ = 0;
        }
        return currentVoiceLimit_;
    }

    [[nodiscard]] int loadPermille() const noexcept { return smoothedLoadPermille_; }
    [[nodiscard]] int voiceLimit() const noexcept { return currentVoiceLimit_; }
    [[nodiscard]] const RealtimeProfile& profile() const noexcept { return profile_; }

private:
    RealtimeProfile profile_ = RealtimeProfile::fromId(1);
    int currentVoiceLimit_ = 64;
    int smoothedLoadPermille_ = 0;
    int overloadStreak_ = 0;
    int headroomStreak_ = 0;
};

} // namespace sf2live
