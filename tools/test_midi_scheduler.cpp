#include "MidiBatchScheduler.h"

#include <array>
#include <iostream>

int main() {
    constexpr int rate = 48000;
    constexpr int frames = 192;
    constexpr int64_t callback = 10'000'000'000LL;

    // Um lote antigo de 8 ms deve manter a ordem, porém caber em no máximo 0,5 ms.
    sf2live::MidiBatchScheduler oldBatch(
        rate,
        frames,
        callback,
        callback - 8'000'000LL,
        callback - 1'000'000LL);
    const std::array<int64_t, 4> timestamps{
        callback - 8'000'000LL,
        callback - 6'000'000LL,
        callback - 3'000'000LL,
        callback - 1'000'000LL,
    };
    int previous = -1;
    int last = 0;
    for (int64_t timestamp : timestamps) {
        const int current = oldBatch.nextFrame(timestamp);
        if (current < previous) {
            std::cerr << "a ordem dos eventos foi alterada\n";
            return 1;
        }
        previous = current;
        last = current;
    }
    if (last > rate / 2000) {
        std::cerr << "o lote antigo acrescentou mais de 0,5 ms\n";
        return 2;
    }

    // Eventos futuros dentro do bloco conservam a posição real.
    sf2live::MidiBatchScheduler futureBatch(rate, frames, callback, 0, 0);
    if (futureBatch.nextFrame(callback + 500'000LL) != 24) {
        std::cerr << "timestamp futuro não foi posicionado corretamente\n";
        return 3;
    }
    if (futureBatch.nextFrame(callback + 250'000LL) != 24) {
        std::cerr << "a ordem monotônica não foi preservada\n";
        return 4;
    }

    std::cout << "OK: scheduler MIDI, catch-up <= 0,5 ms\n";
    return 0;
}
