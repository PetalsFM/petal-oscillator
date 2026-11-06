//          .--.
//     .--.(    ).--.
//    (    )'--'(    )
//     '--'.--.'--'
//        (    )
//         '--'
//
// render_wav: play one note of the Petal Oscillator, either mode, into a WAV file.
//
//   c++ -std=c++17 -O2 -Iinclude examples/render_wav.cpp -o render_wav
//   ./render_wav --scl tunings/edo31.scl --note 60 --ratio 1.37 --lock --out note.wav
//
// Options (defaults in brackets):
//   --scl FILE      tuning; without one, 12-TET at A4 = 440 Hz
//   --garden D:O,.. garden degrees (degree:octave), e.g. 0:0,10:0,18:0
//   --note N        MIDI note [60]       --seconds S   length [2]
//   --ratio R       modulator ratio [1]  --lock        snap the ratio to the garden
//   --fm X          FM depth 0..0.2 [0.04]
//   --fold-after X  the fold of a folder after it, 1..8 [1]
//   --feedback X    -1..1 [0]            --wave sine|skewed|carrier [sine]
//   --shape X       0..1 [0]             --rate HZ     sample rate [48000]
//   --out FILE      [note.wav]
//
// Analog mode, --mode analog (the Complex options above are then not read):
//   --wave1 X       0 tri, 1/3 saw, 2/3 square, 1 pulse [0.333]
//   --sync R        hard-sync ratio 0.5..16 [1]; --lock snaps it to the garden
//   --unison X      0..1 [0]             --detune C    cents 0..200 [25]
//   --sub X         -1..1 [0]            --wave2 X     osc 2's wave [0.333]
//   --ratio2 R      0.5..16 [1]          --level2 X    0..1 [0]
//   --ring X        0..1 [0]             --cross X     0..1 [0]
//
//   ./render_wav --mode analog --unison 1 --detune 40 --sub 0.4 --out pad.wav
//
// Copyright (c) 2026 Petals. MIT License, see LICENSE.
#include "petal/analog.hpp"
#include "petal/oscillator.hpp"
#include "petal/scl.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool writeWav(const char* path, const std::vector<float>& x, int rate) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t bytes = static_cast<uint32_t>(x.size() * 4);
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(3); u16(1); u32(rate); u32(rate * 4); u16(4); u16(32);
    std::fwrite("data", 1, 4, f); u32(bytes);
    std::fwrite(x.data(), 4, x.size(), f);   // little-endian 32-bit float, mono
    return std::fclose(f) == 0;
}

int main(int argc, char** argv) {
    std::string scl, garden, out = "note.wav", wave = "sine", mode = "complex";
    float wave1 = 1.0f / 3.0f, sync = 1, unison = 0, detune = 25, sub = 0, wave2 = 1.0f / 3.0f, ratio2 = 1, level2 = 0, ring = 0, cross = 0;
    int note = 60, rate = 48000;
    float seconds = 2, ratio = 1, fm = 0.04f, foldAfter = 1, fb = 0, shape = 0;
    bool lock = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--scl") scl = next();
        else if (a == "--garden") garden = next();
        else if (a == "--note") note = std::atoi(next());
        else if (a == "--seconds") seconds = std::atof(next());
        else if (a == "--ratio") ratio = std::atof(next());
        else if (a == "--lock") lock = true;
        else if (a == "--fm") fm = std::atof(next());
        else if (a == "--fold-after") foldAfter = std::atof(next());
        else if (a == "--feedback") fb = std::atof(next());
        else if (a == "--wave") wave = next();
        else if (a == "--shape") shape = std::atof(next());
        else if (a == "--rate") rate = std::atoi(next());
        else if (a == "--out") out = next();
        else if (a == "--mode") mode = next();
        else if (a == "--wave1") wave1 = std::atof(next());
        else if (a == "--sync") sync = std::atof(next());
        else if (a == "--unison") unison = std::atof(next());
        else if (a == "--detune") detune = std::atof(next());
        else if (a == "--sub") sub = std::atof(next());
        else if (a == "--wave2") wave2 = std::atof(next());
        else if (a == "--ratio2") ratio2 = std::atof(next());
        else if (a == "--level2") level2 = std::atof(next());
        else if (a == "--ring") ring = std::atof(next());
        else if (a == "--cross") cross = std::atof(next());
        else { std::fprintf(stderr, "unknown option %s (see the top of render_wav.cpp)\n", a.c_str()); return 2; }
    }

    petal::Tuning tuning;
    if (!scl.empty() && !petal::loadScl(scl.c_str(), tuning)) {
        std::fprintf(stderr, "cannot read %s as a .scl scale\n", scl.c_str());
        return 1;
    }
    std::vector<petal::GardenDegree> g;
    for (const char* p = garden.c_str(); *p;) {
        int d, o, n = 0;
        if (std::sscanf(p, "%d:%d%n", &d, &o, &n) != 2) { std::fprintf(stderr, "bad --garden\n"); return 2; }
        g.push_back({d, o});
        p += n;
        if (*p == ',') ++p;
    }

    const double hz = tuning.noteToFrequency(note);
    std::vector<float> y(static_cast<size_t>(seconds * rate));
    double played = ratio;
    bool snapped = false;
    if (mode == "analog") {
        petal::AnalogOscillator osc;
        osc.setWave(wave1); osc.setSync(sync); osc.setSyncLock(lock);
        osc.setUnison(unison); osc.setDetune(detune); osc.setSub(sub);
        osc.setWave2(wave2); osc.setRatio2(ratio2); osc.setLevel2(level2); osc.setRing(ring); osc.setCross(cross);
        osc.setSampleRate(static_cast<float>(rate));
        osc.setTuning(scl.empty() ? nullptr : &tuning, g.empty() ? nullptr : g.data(), static_cast<int>(g.size()));
        osc.noteOn(static_cast<float>(hz));
        osc.process(y.data(), static_cast<int>(y.size()));
        snapped = lock && osc.garden().count();
        played = snapped ? osc.lockedRatio() : sync;
    } else {
        petal::Oscillator osc;
        osc.setRatio(ratio); osc.setRatioLock(lock); osc.setFm(fm);
        osc.setFoldAfter(foldAfter);
        osc.setFeedback(fb); osc.setShape(shape);
        osc.setWave(wave == "skewed" ? petal::Wave::Skewed : wave == "carrier" ? petal::Wave::Carrier : petal::Wave::Sine);
        osc.setSampleRate(static_cast<float>(rate));
        osc.setTuning(scl.empty() ? nullptr : &tuning, g.empty() ? nullptr : g.data(), static_cast<int>(g.size()));
        osc.noteOn(static_cast<float>(hz));
        osc.process(y.data(), static_cast<int>(y.size()));
        snapped = lock && osc.garden().count();
        if (snapped) played = osc.lockedRatio();
    }
    // A 50 ms fade at the end, so the file does not stop on a click.
    const size_t fade = std::min(y.size(), static_cast<size_t>(0.05 * rate));
    for (size_t i = 0; i < fade; ++i) y[y.size() - 1 - i] *= static_cast<float>(i) / fade;

    if (!writeWav(out.c_str(), y, rate)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    std::printf("%s: %s, note %d at %.3f Hz, ratio %.4f%s, %zu samples\n", out.c_str(), mode.c_str(), note, hz,
                played, snapped ? " (locked)" : "", y.size());
    return 0;
}
