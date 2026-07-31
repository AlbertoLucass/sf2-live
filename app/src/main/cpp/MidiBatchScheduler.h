#pragma once

#include <algorithm>
#include <cstdint>

namespace sf2live {

// Converte timestamps AMidi em posições dentro do bloco atual sem alocar memória.
// Eventos já atrasados são comprimidos em uma janela de até meio milissegundo para preservar
// retriggers, mas sem acrescentar um burst inteiro de latência.
class MidiBatchScheduler final {
public:
    MidiBatchScheduler(
        int sampleRate,
        int numFrames,
        int64_t callbackStartNanos,
        int64_t firstPastTimestamp,
        int64_t lastPastTimestamp) noexcept
        : sampleRate_(std::max(1, sampleRate)),
          maxFrame_(std::max(0, numFrames - 1)),
          callbackStartNanos_(callbackStartNanos),
          firstPastTimestamp_(firstPastTimestamp),
          pastSpan_(firstPastTimestamp > 0 && lastPastTimestamp > firstPastTimestamp
              ? lastPastTimestamp - firstPastTimestamp
              : 0),
          catchupFrames_(std::min(maxFrame_, std::max(1, sampleRate_ / 2000))),
          catchupSpanNanos_(
              static_cast<int64_t>(catchupFrames_) * 1000000000LL / sampleRate_) {}

    int nextFrame(int64_t timestampNanos) noexcept {
        int frame = previousFrame_;
        if (timestampNanos > callbackStartNanos_ && maxFrame_ > 0) {
            const int64_t deltaNanos = timestampNanos - callbackStartNanos_;
            frame = static_cast<int>(
                deltaNanos * static_cast<int64_t>(sampleRate_) / 1000000000LL);
        } else if (pastSpan_ > 0 && timestampNanos > firstPastTimestamp_) {
            const int64_t deltaNanos = timestampNanos - firstPastTimestamp_;
            if (pastSpan_ > catchupSpanNanos_) {
                frame = static_cast<int>(
                    deltaNanos * static_cast<int64_t>(catchupFrames_) / pastSpan_);
            } else {
                frame = static_cast<int>(
                    deltaNanos * static_cast<int64_t>(sampleRate_) / 1000000000LL);
            }
        }
        previousFrame_ = std::clamp(frame, previousFrame_, maxFrame_);
        return previousFrame_;
    }

private:
    int sampleRate_ = 48000;
    int maxFrame_ = 0;
    int64_t callbackStartNanos_ = 0;
    int64_t firstPastTimestamp_ = 0;
    int64_t pastSpan_ = 0;
    int catchupFrames_ = 0;
    int64_t catchupSpanNanos_ = 0;
    int previousFrame_ = 0;
};

} // namespace sf2live
