//          .--.
//     .--.(    ).--.
//    (    )'--'(    )
//     '--'.--.'--'
//        (    )
//         '--'
//
// check.cpp - the checks this repo ships with. Run them with: sh tests/check.sh
//
//   match   every setting below, at 44.1, 48 and 96 kHz, against the reference
//           hashes in tests/references.txt, rendered from Folium's oscillator.
//           One hash per 1,024 samples, so a break is found to its chunk. A
//           setting with no reference fails; nothing is skipped.
//   blocks  the same notes in blocks of 1, 7, 64 and 512 samples: identical.
//   rates   ten rates from 8 kHz to 192 kHz: every sample finite, and the
//           pitch where it should be.
//   input   NaN and infinity into every setter: the last good value kept,
//           every sample finite.
//   scl     a ratio typed with spaces is refused.
//   window  each mode hands its rate and a retrigger's duck to its own
//           declick: at 96 kHz the window inside equals the wave run through
//           a Declick of its own.
//   declick the window and the blocker against their formulas.
//
// The exit code is the verdict: 0 every check passed, 1 one failed, 3 the
// references could not be read.
//
// Copyright (c) 2026 Petals. MIT License, see LICENSE.
#include "petal/analog.hpp"
#include "petal/oscillator.hpp"
#include "petal/scl.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace {

constexpr int kFrames = 48000;
constexpr int kChunk = 1024;
int gFailed = 0;
int gMatchFailed = 0;   // of gFailed, the match rows

void verdict(const char* check, const std::string& what, bool pass, const std::string& detail = "") {
    std::printf("%-6s %-28s %s  %s\n", check, what.c_str(), pass ? "PASS" : "FAIL", detail.c_str());
    if (!pass) { ++gFailed; if (std::strcmp(check, "match") == 0) ++gMatchFailed; }
}

// One knob of either mode, by the name the panel prints.
enum Knob { Ratio, Fm, Feedback, Shape, WaveSel,                                    // Complex
            Wave1, Sync, Unison, Detune, Sub, Wave2, Ratio2, Level2, Ring, Cross };  // Analog
struct Set { Knob k; double v; };
struct Move { int at; Knob k; double v; };   // at a multiple of 64
struct Setting {
    const char* name;
    int mode;            // 0 Complex, 1 Analog
    int note;
    const char* scl;     // nullptr: 12-TET
    bool lock;
    std::vector<std::pair<int, int>> garden;
    std::vector<Set> knobs;
    std::vector<Move> moves = {};
};

void set(petal::Oscillator& o, Knob k, double v) {
    switch (k) {
        case Ratio: o.setRatio((float)v); break;
        case Fm: o.setFm((float)v); break;
        case Feedback: o.setFeedback((float)v); break;
        case Shape: o.setShape((float)v); break;
        case WaveSel: o.setWave((petal::Wave)(int)v); break;
        default: break;
    }
}
void set(petal::AnalogOscillator& o, Knob k, double v) {
    switch (k) {
        case Wave1: o.setWave((float)v); break;
        case Sync: o.setSync((float)v); break;
        case Unison: o.setUnison((float)v); break;
        case Detune: o.setDetune((float)v); break;
        case Sub: o.setSub((float)v); break;
        case Wave2: o.setWave2((float)v); break;
        case Ratio2: o.setRatio2((float)v); break;
        case Level2: o.setLevel2((float)v); break;
        case Ring: o.setRing((float)v); break;
        case Cross: o.setCross((float)v); break;
        default: break;
    }
}

// Every Analog setting starts from Folium's defaults; the Complex ones from these.
const std::vector<Set> kComplexBase = {{Ratio, 1.0}, {Fm, 0.04}, {Feedback, 0.0}, {Shape, 0.0}, {WaveSel, 0}};
const std::vector<Set> kAnalogBase = {{Wave1, 1.0 / 3.0}, {Sync, 1.0}, {Unison, 0.0}, {Sub, 0.0}, {Wave2, 1.0 / 3.0},
                                      {Ratio2, 1.0}, {Level2, 0.0}, {Ring, 0.0}, {Cross, 0.0}, {Detune, 25.0}};

const std::vector<Setting>& settings() {
    static const std::vector<Setting> s = {
        {"c_pure_c4",      0, 60, nullptr, false, {}, {{Ratio, 1.0}, {Fm, 0.0}}},
        {"c_fm_c4",        0, 60, nullptr, false, {}, {{Ratio, 1.0}, {Fm, 0.04}}},
        {"c_edo31_garden", 0, 60, "edo31.scl", true, {{0,0},{10,0},{18,0},{31,0}}, {{Ratio, 1.37}, {Fm, 0.10}}},
        {"c_just_scan",    0, 57, "just5.scl", true, {}, {{Ratio, 2.2}, {Fm, 0.12}}},
        {"c_skew_fb",      0, 52, nullptr, false, {}, {{Ratio, 2.0}, {Fm, 0.08}, {Feedback, 0.5}, {Shape, 0.6}, {WaveSel, 1}}},
        {"c_carrier_kora", 0, 48, "petals_kora.scl", false, {}, {{Fm, 0.05}, {Shape, 0.8}, {WaveSel, 2}}},
        {"c_pmos_a6",      0, 93, nullptr, false, {}, {{Ratio, 7.0}, {Fm, 0.20}}},
        {"c_neg_fb_skew",  0, 52, nullptr, false, {}, {{Ratio, 2.0}, {Fm, 0.08}, {Feedback, -0.6}, {Shape, 0.6}, {WaveSel, 1}}},
        {"c_moves",        0, 60, "edo31.scl", true, {}, {{Ratio, 1.2}, {Fm, 0.05}, {Shape, 0.3}},
            {{6400, Fm, 0.18}, {19200, WaveSel, 1}, {25600, Ratio, 3.1}, {32000, WaveSel, 2}, {38400, Feedback, 0.4}}},
        {"a_saw",          1, 57, nullptr, false, {}, {}},
        {"a_tri",          1, 57, nullptr, false, {}, {{Wave1, 0.0}}},
        {"a_pulse",        1, 45, nullptr, false, {}, {{Wave1, 1.0}}},
        {"a_square_a6",    1, 93, nullptr, false, {}, {{Wave1, 2.0 / 3.0}}},
        {"a_sync_free",    1, 48, nullptr, false, {}, {{Sync, 2.37}}},
        {"a_sync_locked",  1, 48, "edo31.scl", true, {{0,0},{10,0},{18,0},{31,0}}, {{Sync, 3.3}, {Wave1, 0.5}}},
        {"a_unison",       1, 50, nullptr, false, {}, {{Unison, 1.0}}},
        {"a_unison_wide",  1, 50, nullptr, false, {}, {{Unison, 0.5}, {Detune, 200.0}}},
        {"a_sub_up",       1, 52, nullptr, false, {}, {{Sub, 0.6}}},
        {"a_sub_down",     1, 52, nullptr, false, {}, {{Sub, -0.6}, {Wave1, 0.0}}},
        {"a_osc2_level",   1, 57, nullptr, false, {}, {{Level2, 0.7}, {Ratio2, 1.5}, {Wave2, 0.0}}},
        {"a_ring",         1, 57, nullptr, false, {}, {{Ring, 0.8}, {Ratio2, 1.25}}},
        {"a_cross",        1, 45, nullptr, false, {}, {{Cross, 1.0}, {Ratio2, 1.5}, {Wave2, 2.0 / 3.0}}},
        {"a_everything",   1, 48, "petals_kora.scl", true, {}, {{Wave1, 0.8}, {Sync, 1.9}, {Unison, 0.25}, {Sub, 0.3},
            {Level2, 0.4}, {Ratio2, 2.1}, {Ring, 0.3}, {Cross, 0.4}, {Detune, 60.0}}},
        {"a_moves",        1, 55, nullptr, false, {}, {{Ratio2, 1.5}},
            {{6400, Wave1, 0.9}, {12800, Sync, 2.6}, {19200, Unison, 1.0}, {25600, Cross, 0.7}, {32000, Level2, 0.5}, {38400, Sub, -0.4}}},
    };
    return s;
}

template <class Osc>
std::vector<float> render(const Setting& s, const std::string& tunings, float rate, int block) {
    Osc o;
    for (const Set& x : s.mode == 0 ? kComplexBase : kAnalogBase) set(o, x.k, x.v);
    for (const Set& x : s.knobs) set(o, x.k, x.v);
    if constexpr (std::is_same_v<Osc, petal::Oscillator>) o.setRatioLock(s.lock); else o.setSyncLock(s.lock);
    o.setDeclickInside(false);
    o.setSampleRate(rate);
    petal::Tuning t;
    if (s.scl && !petal::loadScl((tunings + "/" + s.scl).c_str(), t)) { std::printf("cannot read %s\n", s.scl); std::exit(3); }
    petal::GardenDegree g[8]; const int gn = (int)s.garden.size();
    for (int i = 0; i < gn; ++i) g[i] = {s.garden[i].first, s.garden[i].second};
    o.setTuning(s.scl ? &t : nullptr, gn ? g : nullptr, gn);
    o.noteOn((float)t.noteToFrequency(s.note));
    std::vector<float> out(kFrames);
    size_t mi = 0;
    for (int done = 0; done < kFrames; done += block) {
        const int end = std::min(done + block, kFrames);
        int at = done;
        while (at < end) {   // a host block that crosses a move is cut there
            while (mi < s.moves.size() && s.moves[mi].at == at) { set(o, s.moves[mi].k, s.moves[mi].v); ++mi; }
            const int next = (mi < s.moves.size() && s.moves[mi].at < end) ? s.moves[mi].at : end;
            o.process(out.data() + at, next - at);
            at = next;
        }
    }
    return out;
}

std::vector<float> renderAny(const Setting& s, const std::string& tunings, float rate, int block) {
    return s.mode == 0 ? render<petal::Oscillator>(s, tunings, rate, block) : render<petal::AnalogOscillator>(s, tunings, rate, block);
}

uint64_t fnv1a(const float* x, int n) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char* b = reinterpret_cast<const unsigned char*>(x);
    for (int i = 0; i < n * 4; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

std::vector<uint64_t> chunkHashes(const std::vector<float>& x) {
    std::vector<uint64_t> h;
    for (int at = 0; at < (int)x.size(); at += kChunk) h.push_back(fnv1a(x.data() + at, std::min(kChunk, (int)x.size() - at)));
    return h;
}

}  // namespace

// The oscillator with its declick inside, against the same oscillator with it
// outside and a Declick set to the same rate: identical only if the oscillator
// handed its rate on, and at 1,024 a second note over the sounding tail, whose
// duck it must hand on too.
template <class Osc> bool windowFollowsRate(float sr) {
    Osc in, out; in.setSampleRate(sr); out.setSampleRate(sr); out.setDeclickInside(false);
    petal::Declick d; d.setSampleRate(sr); d.reset();
    in.noteOn(220.0f); out.noteOn(220.0f); d.noteOn(false);
    for (int i = 0; i < 4096; ++i) {
        if (i == 1024) { in.noteOn(330.0f, true); out.noteOn(330.0f, true); d.noteOn(true); }
        if (in.process() != d.process(out.process())) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    const std::string tunings = argc > 1 ? argv[1] : "tunings";
    const std::string refPath = argc > 2 ? argv[2] : "tests/references.txt";
    const bool write = argc > 3 && !std::strcmp(argv[3], "--write");

    // match
    std::map<std::string, std::vector<uint64_t>> refs;
    if (!write) {
        std::FILE* f = std::fopen(refPath.c_str(), "r");
        if (!f) { std::printf("cannot read %s\n", refPath.c_str()); return 3; }
        char line[4096];
        while (std::fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n') continue;
            char rate[16], name[64]; int used = 0;
            if (std::sscanf(line, "%15s %63s%n", rate, name, &used) != 2) continue;
            std::vector<uint64_t> h; const char* p = line + used; unsigned long long v; int k;
            while (std::sscanf(p, " %llx%n", &v, &k) == 1) { h.push_back(v); p += k; }
            refs[std::string(rate) + " " + name] = h;
        }
        std::fclose(f);
    }
    std::FILE* out = write ? std::fopen(refPath.c_str(), "w") : nullptr;
    if (out) std::fprintf(out, "# rate setting, then one FNV-1a 64 hash of every 1,024 samples (float bits)\n");
    for (float rate : {44100.0f, 48000.0f, 96000.0f}) {
        for (const Setting& s : settings()) {
            const std::vector<uint64_t> h = chunkHashes(renderAny(s, tunings, rate, 64));
            char key[96]; std::snprintf(key, sizeof key, "%.0f %s", rate, s.name);
            if (out) {
                std::fprintf(out, "%s", key);
                for (uint64_t x : h) std::fprintf(out, " %016llx", (unsigned long long)x);
                std::fprintf(out, "\n");
                continue;
            }
            auto it = refs.find(key);
            if (it == refs.end()) { verdict("match", key, false, "no reference"); continue; }
            int first = -1;
            for (size_t c = 0; c < h.size(); ++c) if (c >= it->second.size() || h[c] != it->second[c]) { first = (int)c; break; }
            if (it->second.size() != h.size() && first < 0) first = (int)std::min(h.size(), it->second.size());
            char d[96]; std::snprintf(d, sizeof d, first < 0 ? "every chunk" : "first break in samples %d..%d", first * kChunk, first * kChunk + kChunk - 1);
            verdict("match", key, first < 0, d);
        }
    }
    if (out) { std::fclose(out); std::printf("wrote %s\n", refPath.c_str()); return 0; }

    // blocks
    for (const char* name : {"c_moves", "c_skew_fb", "a_moves", "a_everything"}) {
        const Setting* s = nullptr;
        for (const Setting& x : settings()) if (!std::strcmp(x.name, name)) s = &x;
        const auto ref = renderAny(*s, tunings, 48000.0f, 64);
        for (int b : {1, 7, 512}) {
            const bool same = renderAny(*s, tunings, 48000.0f, b) == ref;
            verdict("blocks", std::string(name) + " at " + std::to_string(b), same, "against 64");
        }
    }

    // rates: finite, and the pitch (zero crossings upward) within 1 % of A3
    for (int mode : {0, 1}) {
        for (float rate : {8000.0f, 11025.0f, 16000.0f, 22050.0f, 32000.0f, 44100.0f, 48000.0f, 88200.0f, 96000.0f, 192000.0f}) {
            Setting s = {"rate", mode, 57, nullptr, false, {}, mode == 0 ? std::vector<Set>{{Fm, 0.0}} : std::vector<Set>{{Wave1, 0.0}}};
            const auto y = renderAny(s, tunings, rate, 64);
            bool finite = true; int up = 0, firstUp = -1, lastUp = -1;
            for (int i = 1; i < kFrames; ++i) {
                finite = finite && std::isfinite(y[i]);
                if (i > kFrames / 4 && y[i - 1] < 0.0f && y[i] >= 0.0f) { if (firstUp < 0) firstUp = i; lastUp = i; ++up; }
            }
            const double hz = up > 1 ? (up - 1) * rate / double(lastUp - firstUp) : 0.0;
            char d[64]; std::snprintf(d, sizeof d, "%.2f Hz", hz);
            verdict("rates", std::string(mode ? "analog " : "complex ") + std::to_string((int)rate), finite && std::fabs(hz / 220.0 - 1.0) < 0.01, d);
        }
    }

    // input: every setter takes NaN and infinity and keeps its last good value
    {
        const float bad[] = {NAN, INFINITY, -INFINITY};
        petal::Oscillator c; c.setSampleRate(48000.0f); c.noteOn(220.0f);
        petal::AnalogOscillator a; a.setSampleRate(48000.0f); a.setLevel2(0.5f); a.noteOn(220.0f);
        bool finite = true;
        for (float x : bad) {
            c.setRatio(x); c.setFm(x); c.setFoldAfter(x); c.setFeedback(x); c.setShape(x); c.setFrequency(x); c.noteOn(x);
            a.setWave(x); a.setSync(x); a.setUnison(x); a.setDetune(x); a.setSub(x); a.setWave2(x); a.setRatio2(x);
            a.setLevel2(x); a.setRing(x); a.setCross(x); a.setFrequency(x); a.noteOn(x);
            for (int i = 0; i < 4096; ++i) finite = finite && std::isfinite(c.process()) && std::isfinite(a.process());
        }
        verdict("input", "NaN and infinity, every setter", finite);
    }

    // declick: the window and the blocker against their formulas, worked in
    // double here: a fresh note, then a note over a sounding tail at 2,000.
    // The window is a length in time, so it is read at three rates.
    for (const double sr : {44100.0, 48000.0, 96000.0}) {
        // The blocker's coefficient as the header rounds it to float: near 1 at
        // 96 kHz its rounding, not the recursion, would be what this reads.
        const double R = (double)(1.0f - (62.83f / (float)sr));
        const double kPi = 3.14159265358979323846, k = 48000.0 / sr;
        petal::Declick d; d.setSampleRate((float)sr); d.reset();
        d.noteOn(false);
        double pos = 0.0, x1 = 0.0, y1 = 0.0, worst = 0.0; int dir = 1;
        for (int n = 0; n < 6000; ++n) {
            if (n == 2000) { d.noteOn(true); dir = -1; }
            double w = 1.0;
            if (dir != 0) {
                w = 0.5 * (1.0 - std::cos(kPi * pos));
                if (dir < 0) { pos -= k / 64.0; if (pos <= 0.0) { pos = 0.0; dir = 1; } }
                else { pos += k / 256.0; if (pos >= 1.0) { pos = 1.0; dir = 0; } }
            }
            const double y = w - x1 + R * y1; x1 = w; y1 = y;
            worst = std::max(worst, std::fabs(y - (double)d.process(1.0f)));
        }
        char det[64]; std::snprintf(det, sizeof det, "%.0f Hz, worst %.2e against the formula", sr, worst);
        verdict("declick", "rise, duck, blocker", worst < 1e-5, det);
    }

    // scl: a ratio typed with spaces is refused, not read as a whole number.
    {
        petal::Tuning t;
        const char spaced[] = "spaced\n1\n3 / 2\n", tight[] = "tight\n1\n3/2 the fifth\n";
        const bool refused = !petal::parseScl(spaced, sizeof spaced - 1, t);
        const bool loaded = petal::parseScl(tight, sizeof tight - 1, t);
        verdict("scl", "3 / 2 refused, 3/2 loads", refused && loaded);
    }

    // window: each mode's declick runs at the oscillator's rate.
    verdict("window", "complex 96000", windowFollowsRate<petal::Oscillator>(96000.0f), "inside against outside");
    verdict("window", "analog 96000", windowFollowsRate<petal::AnalogOscillator>(96000.0f), "inside against outside");

    std::printf("check: %s (%d failed)\n", gFailed ? "RED" : "GREEN", gFailed);
    if (gFailed > 0 && gFailed == gMatchFailed)
        std::printf("check: only match rows failed. That is expected on a build other than the reference in MODES.md, which also says how far the sound moves there; every other check passed.\n");
    return gFailed ? 1 : 0;
}
