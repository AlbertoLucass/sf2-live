#include "Sf2Synth.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {
float renderPeak(sf2live::Sf2Synth& synth, int frames) {
    std::vector<float> buffer(static_cast<size_t>(frames) * 2u);
    synth.render(buffer.data(), frames);
    float peak = 0.0f;
    for (float value : buffer) peak = std::max(peak, std::fabs(value));
    return peak;
}

bool drain(sf2live::Sf2Synth& synth, int maxBlocks = 300) {
    for (int index = 0; index < maxBlocks; ++index) {
        renderPeak(synth, 256);
        if (synth.activeVoiceCount() == 0) return true;
    }
    return false;
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "uso: test_sf2_core arquivo.sf2\n";
        return 2;
    }

    sf2live::Sf2Synth synth;
    synth.setSampleRate(48000);
    synth.setMasterVolume(0.8f);
    synth.setReleaseMilliseconds(80.0f);

    std::string error;
    if (!synth.loadFromFile(argv[1], error)) {
        std::cerr << "falha ao carregar: " << error << '\n';
        return 3;
    }
    if (synth.presets().size() != 1 || synth.presets()[0].name != "Test Preset") {
        std::cerr << "lista de presets inesperada\n";
        return 4;
    }

    if (synth.estimateVoiceCountForNote(69, 100) != 1) {
        std::cerr << "estimativa de carga do acorde inesperada\n";
        return 16;
    }

    synth.noteOn(0, 69, 110);
    const float peak = renderPeak(synth, 2048);
    if (!(peak > 0.01f) || synth.activeVoiceCount() <= 0) {
        std::cerr << "a nota não produziu áudio\n";
        return 5;
    }
    synth.noteOff(0, 69);
    if (!drain(synth)) {
        std::cerr << "a cauda de release não terminou\n";
        return 6;
    }

    synth.noteOn(0, 69, 100);
    synth.controlChange(0, 64, 127);
    synth.noteOff(0, 69);
    for (int index = 0; index < 80; ++index) renderPeak(synth, 256);
    if (synth.activeVoiceCount() <= 0) {
        std::cerr << "o sustain CC64 não segurou a nota\n";
        return 7;
    }
    synth.controlChange(0, 64, 0);
    if (!drain(synth)) {
        std::cerr << "a nota não terminou após soltar CC64\n";
        return 8;
    }

    synth.noteOn(0, 69, 100);
    synth.allNotesOff(true);
    renderPeak(synth, 32);
    if (synth.activeVoiceCount() != 0) {
        std::cerr << "Panic não encerrou as vozes\n";
        return 9;
    }

    // Toque extremamente curto: Note On e Note Off podem chegar no mesmo lote
    // USB. Ainda assim, a nota precisa renderizar uma pequena janela audível.
    synth.noteOn(0, 69, 100);
    synth.noteOff(0, 69);
    const float immediateTapPeak = renderPeak(synth, 64);
    if (immediateTapPeak <= 0.005f) {
        std::cerr << "um toque curto recebido no mesmo lote foi perdido\n";
        return 10;
    }
    if (!drain(synth)) {
        std::cerr << "o toque curto deixou voz presa\n";
        return 11;
    }

    // Retrigger sobreposto: o primeiro Note Off não pode desligar a repetição
    // mais nova da mesma tecla.
    synth.noteOn(0, 69, 100);
    synth.noteOn(0, 69, 105);
    synth.noteOff(0, 69);
    const float overlapPeak = renderPeak(synth, 256);
    if (overlapPeak <= 0.01f || synth.activeVoiceCount() <= 0) {
        std::cerr << "o retrigger mais novo foi desligado pelo Note Off antigo\n";
        return 12;
    }
    synth.noteOff(0, 69);
    if (!drain(synth)) {
        std::cerr << "o retrigger sobreposto deixou voz presa\n";
        return 13;
    }

    // O fade da execução antiga pode terminar antes de seu Note Off chegar. Esse
    // Note Off ainda precisa ser consumido pela execução antiga, sem avançar e
    // desligar a repetição nova.
    synth.noteOn(0, 69, 100);
    synth.noteOn(0, 69, 108);
    renderPeak(synth, 128); // > 0,75 ms: a primeira voz já terminou o fade.
    synth.noteOff(0, 69);
    const float expiredOldVoicePeak = renderPeak(synth, 256);
    if (expiredOldVoicePeak <= 0.01f || synth.activeVoiceCount() <= 0) {
        std::cerr << "Note Off antigo desligou o retrigger após o fade terminar\n";
        return 14;
    }
    synth.noteOff(0, 69);
    if (!drain(synth)) {
        std::cerr << "retrigger após fade deixou voz presa\n";
        return 15;
    }

    float rapidPeak = 0.0f;
    for (int index = 0; index < 200; ++index) {
        synth.noteOn(0, 69, 90 + index % 30);
        rapidPeak = std::max(rapidPeak, renderPeak(synth, 16));
        synth.noteOff(0, 69);
        rapidPeak = std::max(rapidPeak, renderPeak(synth, 16));
    }
    if (rapidPeak <= 0.01f || synth.activeVoiceCount() > 8) {
        std::cerr << "retriggers rápidos acumularam vozes ou perderam áudio\n";
        return 16;
    }
    if (!drain(synth)) {
        std::cerr << "retriggers rápidos deixaram caudas presas\n";
        return 17;
    }

    // Reduzir a polifonia durante uma rajada não pode ordenar/alocar dentro da
    // callback nem manter dezenas de vozes acima do novo orçamento.
    synth.setReleaseMilliseconds(500.0f);
    synth.setMaxVoices(64);
    for (int note = 36; note < 96; ++note) synth.noteOn(0, note, 100);
    if (synth.activeVoiceCount() < 40) {
        std::cerr << "o teste não criou vozes suficientes\n";
        return 18;
    }
    synth.setMaxVoices(32);
    for (int block = 0; block < 8; ++block) renderPeak(synth, 64);
    if (synth.activeVoiceCount() > 32) {
        std::cerr << "a redução incremental de polifonia não convergiu\n";
        return 19;
    }
    synth.allNotesOff(true);

    std::cout << "OK: preset=" << synth.presets()[0].name
              << ", peak=" << peak
              << ", release=OK, sustain_CC64=OK, panic=OK, retrigger=OK, voice_budget=OK\n";
    return 0;
}
