//          .--.
//     .--.(    ).--.
//    (    )'--'(    )
//     '--'.--.'--'
//        (    )
//         '--'
//
// petal/analog.hpp - the Petal Oscillator's Analog mode, header-only C++17, no
// dependencies beyond core.hpp and declick.hpp beside it.
//
// Two band-limited oscillators. Osc 1 sweeps one wave from triangle through saw
// and square to a 10 % pulse, hard-synced to a master at the note's pitch, in a
// unison of up to seven, over a square sub one octave down (two octaves on the
// minus side). Osc 2 plays the same sweep at its own ratio, adds to the mix,
// rings it toward osc 1 x osc 2, and bends osc 1's rate with its own wave
// (cross FM). The sync ratio and osc 2's ratio both lock to the tuning's ratio
// garden, so a sync sweep lands on the scale's intervals.
//
// Every step in a wave is corrected with a two-point polyBLEP and every corner
// with a polyBLAMP, at the fraction of a sample where it happens, at twice the
// rate, then decimated by a 31-tap halfband: 8 samples of delay.
//
// Use it as the Complex mode is used: process() once per sample, or
// process(out, n), at any rate. Nothing allocates after construction, nothing
// locks, nothing calls the system.
//
// Copyright (c) 2026 Petals. MIT License, see LICENSE.
#pragma once

#include "core.hpp"
#include "declick.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace petal {

namespace detail {

constexpr int kAnalogUnison = 7;
// Unison offsets in units of the spread, centre first: the outer pair, then
// two thirds, then one third.
constexpr float kAnalogOffset[kAnalogUnison] = { 0.0f, -1.0f, 1.0f, -0.6667f, 0.6667f, -0.3333f, 0.3333f };
// Where a note into silence starts each unison voice, centre first. Spread
// evenly (v / 7), the six side voices would cancel the centre's first six
// harmonics; all at 0, the note would start 8.5 dB loud. These, found by
// search, hold every harmonic to the 12th within 3.1 dB of the voices'
// equal-power sum at unison 1, 1/2 and 1/4.
constexpr float kAnalogStartPhase[kAnalogUnison] = { 0.0f, 0.196f, 0.033f, 0.12f, 0.009f, 0.413f, 0.672f };
using AnalogHalfband = Halfband2x<31, 40, 0>;
// The output's delay: the halfband's (taps - 1) / 2 at twice the rate, plus the
// one 2x step the corrections are paid late.
constexpr float kAnalogLatency = (AnalogHalfband::kNumTaps - 1) * 0.25f + 0.5f;

// Tri, saw and pulse weights and the pulse width, for a sweep position s.
struct AnalogWave { float a, b, c, pw; };
inline AnalogWave analogWave(float s) {
    s = std::clamp(s, 0.0f, 1.0f);
    if (s < 1.0f / 3.0f) { const float t = 3.0f * s; return { 1.0f - t, t, 0.0f, 0.5f }; }
    if (s < 2.0f / 3.0f) { const float t = 3.0f * s - 1.0f; return { 0.0f, 1.0f - t, t, 0.5f }; }
    return { 0.0f, 0.0f, 1.0f, 0.5f - 0.4f * (3.0f * s - 2.0f) };
}
inline float analogValue(const AnalogWave& w, float p) {
    const float tri = p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
    return w.a * tri + w.b * (2.0f * p - 1.0f) + w.c * (p < w.pw ? 1.0f : -1.0f);
}
inline float analogSlope(const AnalogWave& w, float p) {   // per unit phase
    return w.a * (p < 0.5f ? 4.0f : -4.0f) + 2.0f * w.b;
}

// One step (jump) or corner (slope change) at fraction d of the sample after
// the held sample: its polyBLEP and polyBLAMP residuals, owed to the held
// sample and to the one just computed.
struct AnalogCorr { float held = 0.0f, cur = 0.0f; };
inline void analogEvent(AnalogCorr& c, float d, float jump, float slope) {
    const float e = 1.0f - d;
    c.held += jump * 0.5f * e * e + slope * (e * e * e) * (1.0f / 6.0f);
    c.cur  += -jump * 0.5f * d * d + slope * (d * d * d) * (1.0f / 6.0f);
}

// The corners a wave meets running from phase p for times t0..t1 (in 2x
// samples) at ds per sample, with weight g: the triangle's top, the pulse's
// fall, the wrap, and the first two again after the wrap.
inline void analogSegment(AnalogCorr& c, const AnalogWave& w, float p, float ds,
                          float t0, float t1, float g) {
    const float slope8 = 8.0f * w.a * ds * g;
    auto at = [&](float x) { return t0 + (x - p) / ds; };   // the time p reaches x
    if (w.a > 0.0f && p < 0.5f && at(0.5f) < t1) analogEvent(c, at(0.5f), 0.0f, -slope8);
    if (w.c > 0.0f && p < w.pw && at(w.pw) < t1) analogEvent(c, at(w.pw), -2.0f * w.c * g, 0.0f);
    // <= : a wrap that lands exactly on t1 (a master reset at sync ratio 1) is
    // the wrap; the reset after it then jumps nothing, so the step is paid once.
    if (at(1.0f) <= t1) {
        const float tw = at(1.0f);
        analogEvent(c, tw, (2.0f * w.c - 2.0f * w.b) * g, slope8);
        auto at2 = [&](float x) { return tw + x / ds; };
        if (w.a > 0.0f && at2(0.5f) < t1) analogEvent(c, at2(0.5f), 0.0f, -slope8);
        if (w.c > 0.0f && at2(w.pw) < t1) analogEvent(c, at2(w.pw), -2.0f * w.c * g, 0.0f);
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// AnalogOscillator: one voice.
// ---------------------------------------------------------------------------
class AnalogOscillator {
public:
    AnalogOscillator() {
        detail::AnalogHalfband::init();
        setSampleRate(kDefaultSampleRate);
    }

    // Sets the rate and resets every state; parameter values jump to their
    // targets (no glide).
    void setSampleRate(float sr) {
        using namespace detail;
        sr_ = sr > 0.0f ? sr : kDefaultSampleRate;
        smSync_.setTime(kRatioGlideMs, sr_);
        smUnison_.setTime(kParamSmoothMs, sr_);
        smSub_.setTime(kParamSmoothMs, sr_);
        smWave_.setTime(kParamSmoothMs, sr_);
        declick_.setSampleRate(sr_);
        reset();
    }
    float sampleRate() const { return sr_; }

    // Clears every state and snaps every parameter to its target. The
    // oscillator idles at its resting pitch, note 0, until noteOn() or
    // setFrequency() sets another. Every phase starts at 0.
    void reset() {
        for (int v = 0; v < detail::kAnalogUnison; ++v) master_[v] = slave_[v] = 0.0f;
        held_ = owed_ = 0.0f; sub_ = sub2_ = 1.0f;
        ph2_ = held2_ = owed2_ = 0.0f;
        for (float& r : ramp_) r = 0.0f;
        rampSeeded_ = false;
        for (float& r : ring_) r = 0.0f;
        ringPos_ = 0;
        side_ = {};
        declick_.reset();
        smSync_.set(syncKnob_); smUnison_.set(unisonKnob_);
        smSub_.set(subKnob_); smWave_.set(wave_);
        lastSnap_ = -1.0;
        pos_ = 0;
        onsetPending_ = busy_ = false;
        hzSwapPending_ = false;
        played_ = false;
        hz_ = static_cast<float>(tuning_.noteToFrequency(0));
    }

    // ---- parameters (applied at the next control update) -------------------
    // A value that is not finite is ignored and the last one kept.
    void setWave(float s)   { if (std::isfinite(s)) wave_ = s; }               // osc 1: 0 tri, 1/3 saw, 2/3 square, 1 10 % pulse
    void setSync(float r)   { if (std::isfinite(r)) syncKnob_ = r; }           // hard-sync ratio, 0.5..16; 1 is no sync
    void setSyncLock(bool on) { lock_ = on; }                                  // snap the sync ratio and osc 2's ratio to the garden
    void setUnison(float u) { if (std::isfinite(u)) unisonKnob_ = u * kUnisonScale; }   // 0 one voice .. 1 seven, side voices in by 1/4
    void setDetune(float cents) { if (std::isfinite(cents)) detune_ = cents; } // the outer pair's offset at full unison, 0..200 cents
    void setSub(float s)    { if (std::isfinite(s)) subKnob_ = s; }            // 0..1 one octave under, 0..-1 two octaves under
    void setWave2(float s)  { if (std::isfinite(s)) wave2_ = s; }              // osc 2, the same sweep as osc 1
    void setRatio2(float r) { if (std::isfinite(r)) ratio2_ = r; }             // osc 2 against the note, 0.5..16
    void setLevel2(float l) { if (std::isfinite(l)) level2_ = l; }             // osc 2 in the mix, 0..1
    void setRing(float r)   { if (std::isfinite(r)) ring2_ = r; }              // the mix toward osc 1 x osc 2, 0..1
    void setCross(float x)  { if (std::isfinite(x)) cross_ = x; }              // osc 2's wave bends osc 1's rate, 0..1 (up to +/-100 %)

    // The tuning the garden reads. Pass nullptr for none.
    void setTuning(const Tuning* t, const GardenDegree* garden = nullptr, int gardenCount = 0) {
        tuning_ = t ? *t : Tuning{};
        selCount_ = std::clamp(gardenCount, 0, kMaxGardenSteps);
        for (int i = 0; i < selCount_; ++i) sel_[i] = garden[i];
        garden_.build(tuning_, sel_, selCount_);
        if (!played_) hz_ = static_cast<float>(tuning_.noteToFrequency(0));
    }
    const Garden& garden() const { return garden_; }
    double lockedRatio() const { return lastSnap_; }   // the sync ratio the garden snapped to, -1 before any

    void setFrequency(float hz) {
        if (!std::isfinite(hz)) return;
        if (hzSwapPending_) nextHz_ = hz; else hz_ = hz;
    }

    // Start a note. `tailSounding`: the previous note is still audible, so it
    // is ducked over 64 samples first and the phases run on. A note into
    // silence starts every voice at its own start phase (kAnalogStartPhase).
    // The onset sample still runs at the previous frequency; the new one takes
    // over from the next sample.
    void noteOn(float hz, bool tailSounding = false) {
        if (!std::isfinite(hz)) return;
        nextHz_ = hz;
        hzSwapPending_ = true;
        played_ = true;
        onsetPending_ = true;
        busy_ = tailSounding;
        pos_ = 0;
    }

    // What a folder after this oscillator reads, for the sample process() last
    // returned: the fundamental's phase in cycles (delayed as far as the output
    // is), its frequency in cycles per sample, and its level.
    struct Sidechain { float phase = 0.0f, freq = 0.0f, level = 0.0f; };
    const Sidechain& sidechain() const { return side_; }

    void setDeclickInside(bool on) { declickInside_ = on; }
    Declick& declick() { return declick_; }

    // ---- rendering -----------------------------------------------------------
    float process() {
        if (pos_ == 0) controlUpdate();
        const float y = renderSample(pos_);
        if (++pos_ == kControlBlock) pos_ = 0;
        return y;
    }
    void process(float* out, int n) { for (int i = 0; i < n; ++i) out[i] = process(); }

private:
    static constexpr float kUnisonScale = 0.2f;   // the unison runs on a 0..0.2 scale inside

    void controlUpdate() {
        using namespace detail;
        const bool onset = onsetPending_;

        double target = static_cast<double>(syncKnob_);
        if (lock_ && garden_.count() > 0) { lastSnap_ = garden_.nearest(target); target = lastSnap_; }
        float syncTarget = static_cast<float>(std::clamp(target, 0.5, 16.0));
        if (!std::isfinite(syncTarget)) syncTarget = 1.0f;
        smSync_.run(syncTarget, svSync_);
        smUnison_.run(std::clamp(unisonKnob_, 0.0f, kUnisonScale), svUnison_);
        smSub_.run(std::clamp(subKnob_, -1.0f, 1.0f), svSub_);
        smWave_.run(std::clamp(wave_, 0.0f, 1.0f), svWave_);

        // Unison, read at the block's middle.
        constexpr int kMid = kControlBlock / 2;
        const float uni = std::clamp(svUnison_[kMid] / kUnisonScale, 0.0f, 1.0f);
        const float side = std::min(1.0f, 4.0f * uni);
        const float norm = 1.0f / std::sqrt(1.0f + (kAnalogUnison - 1) * side * side);
        const float spread = std::clamp(detune_, 0.0f, 200.0f);
        for (int v = 0; v < kAnalogUnison; ++v) {
            det_[v]  = std::exp2(kAnalogOffset[v] * spread * uni * (1.0f / 1200.0f));
            gain_[v] = (v == 0 ? 1.0f : side) * norm;
        }

        // Osc 2's wave, level, ring and cross ramp across the block from where
        // the last ended (they move the waveform); its ratio is read once.
        const float tgt[4] = { std::clamp(wave2_, 0.0f, 1.0f), std::clamp(level2_, 0.0f, 1.0f),
                               std::clamp(ring2_, 0.0f, 1.0f), std::clamp(cross_, 0.0f, 1.0f) };
        if (!rampSeeded_ || (onset && !busy_)) {
            for (int k = 0; k < 4; ++k) ramp_[k] = tgt[k];
            rampSeeded_ = true;
        }
        for (int k = 0; k < 4; ++k) { r0_[k] = ramp_[k]; rd_[k] = (tgt[k] - r0_[k]) * (1.0f / kControlBlock); ramp_[k] = tgt[k]; }
        osc2On_ = r0_[1] > 0.0f || tgt[1] > 0.0f || r0_[2] > 0.0f || tgt[2] > 0.0f || r0_[3] > 0.0f || tgt[3] > 0.0f;
        ratio2Eff_ = std::clamp(ratio2_, 0.5f, 16.0f);
        if (lock_ && garden_.count() > 0) ratio2Eff_ = static_cast<float>(garden_.nearest(static_cast<double>(ratio2Eff_)));

        if (onset) {
            if (!busy_) {
                for (int v = 0; v < kAnalogUnison; ++v) master_[v] = slave_[v] = kAnalogStartPhase[v];
                master_[0] = slave_[0] = 0.0f; sub_ = 1.0f;
                ph2_ = 0.0f; held2_ = owed2_ = 0.0f;
            }
            declick_.noteOn(busy_);
            onsetPending_ = false;
        }
    }

    float renderSample(int i) {
        using namespace detail;
        const float inc = hz_ * (1.0f / sr_);
        const float y = step(i, inc);
        float cph = master_[0] - kAnalogLatency * inc; if (cph < 0.0f) cph += 1.0f;
        side_ = { cph, inc, 1.0f };
        const float out = declickInside_ ? declick_.process(y) : y;
        if (hzSwapPending_) { hz_ = nextHz_; hzSwapPending_ = false; }
        return out;
    }

    // One output sample: two steps at twice the rate, decimated.
    float step(int i, float inc) {
        using namespace detail;
        const float fi = static_cast<float>(i + 1);
        const float lvl2 = r0_[1] + rd_[1] * fi, ring = r0_[2] + rd_[2] * fi, xfm = r0_[3] + rd_[3] * fi;
        const AnalogWave w2 = analogWave(r0_[0] + rd_[0] * fi);
        const AnalogWave w = analogWave(svWave_[i]);
        const float ratio = svSync_[i];
        const float subK = std::clamp(svSub_[i], -1.0f, 1.0f);
        const float subLvl = std::max(subK, 0.0f);
        const float sub2Lvl = std::max(-subK, 0.0f);
        float twice[2];
        for (int h = 0; h < 2; ++h) {
            // Osc 2 first; cross FM reads the wave it plays, held at 0 so osc 1
            // never runs backwards.
            float o2 = 0.0f, fmMul = 1.0f;
            if (osc2On_) {
                AnalogCorr c2;
                c2.held = owed2_;
                const float d2 = std::clamp(inc * ratio2Eff_ * 0.5f, 1e-6f, 0.45f);
                analogSegment(c2, w2, ph2_, d2, 0.0f, 1.0f, 1.0f);
                ph2_ += d2; if (ph2_ >= 1.0f) ph2_ -= 1.0f;
                o2 = held2_ + c2.held;
                held2_ = analogValue(w2, ph2_);
                owed2_ = c2.cur;
                fmMul = std::max(0.0f, 1.0f + xfm * o2);
            }
            AnalogCorr c;
            c.held = owed_;
            float naive = 0.0f;
            for (int v = 0; v < kAnalogUnison; ++v) {
                const float g = gain_[v];
                if (g == 0.0f) continue;
                const float dm = std::min(inc * fmMul * det_[v] * 0.5f, 0.45f);
                const float ds = std::clamp(dm * ratio, 1e-6f, 0.45f);
                const float m0 = master_[v], s0 = slave_[v];
                const float mN = m0 + dm;
                if (mN >= 1.0f) {   // the master wraps at dSync, and the slave resets there
                    const float dSync = (1.0f - m0) / dm;
                    analogSegment(c, w, s0, ds, 0.0f, dSync, g);
                    float q = s0 + ds * dSync; if (q >= 1.0f) q -= 1.0f;
                    analogEvent(c, dSync, (analogValue(w, 0.0f) - analogValue(w, q)) * g,
                                (analogSlope(w, 0.0f) - analogSlope(w, q)) * ds * g);
                    analogSegment(c, w, 0.0f, ds, dSync, 1.0f, g);
                    slave_[v] = ds * (1.0f - dSync);
                    master_[v] = mN - 1.0f;
                    if (v == 0) {   // the sub flips on the centre master's wrap
                        analogEvent(c, dSync, -2.0f * sub_ * subLvl, 0.0f);
                        sub_ = -sub_;
                        if (sub_ > 0.0f) {   // every second flip: the sub two octaves under
                            if (sub2Lvl > 0.0f) analogEvent(c, dSync, -2.0f * sub2_ * sub2Lvl, 0.0f);
                            sub2_ = -sub2_;
                        }
                    }
                } else {
                    analogSegment(c, w, s0, ds, 0.0f, 1.0f, g);
                    float sN = s0 + ds; if (sN >= 1.0f) sN -= 1.0f;
                    slave_[v] = sN; master_[v] = mN;
                }
                naive += g * analogValue(w, slave_[v]);
            }
            naive += sub_ * subLvl;
            if (sub2Lvl > 0.0f) naive += sub2_ * sub2Lvl;
            float o1 = held_ + c.held;   // one step late, with its corrections paid
            held_ = naive;
            owed_ = c.cur;
            if (osc2On_) {   // LEVEL adds osc 2; RING fades the whole toward osc 1 x osc 2
                const float mix = o1 + lvl2 * o2;
                o1 = mix + ring * (o1 * o2 - mix);
            }
            twice[h] = o1;
        }
        return decimate(twice[0], twice[1]);
    }

    // The halfband as a decimator: push two 2x samples, return one.
    float decimate(float a, float b) {
        using HB = detail::AnalogHalfband;
        constexpr int N = HB::kNumTaps;
        int pos = ringPos_;
        ring_[pos] = a; pos = (pos + 1) % N;
        float acc = 0.0f;
        for (int j = 0; j < HB::sNumNz; ++j) {
            const int t = HB::sNzIdx[j];
            // The multiply and the add are separate statements. Clang fuses only
            // within one expression, so it emits no fused multiply-add here
            // (measured: Apple clang 21, arm64, -O2). GCC fuses across
            // statements: GCC 15.2 at -O2 emits one fmadd here, and none with
            // -ffp-contract=off, so a GCC build rounds this sum differently.
            const float m = HB::sTaps[t] * ring_[(pos - 1 - t + N) % N];
            acc = acc + m;
        }
        ring_[pos] = b; pos = (pos + 1) % N;
        ringPos_ = pos;
        return acc;
    }

    // parameters
    float sr_ = kDefaultSampleRate, hz_ = static_cast<float>(kC4Hz);
    float wave_ = 1.0f / 3.0f, syncKnob_ = 1.0f, unisonKnob_ = 0.0f, detune_ = 25.0f, subKnob_ = 0.0f;
    float wave2_ = 1.0f / 3.0f, ratio2_ = 1.0f, level2_ = 0.0f, ring2_ = 0.0f, cross_ = 0.0f;
    bool lock_ = false;
    Tuning tuning_{};
    GardenDegree sel_[kMaxGardenSteps] = {};
    int selCount_ = 0;
    Garden garden_{};

    // control state
    int pos_ = 0;
    bool onsetPending_ = false, busy_ = false, hzSwapPending_ = false, played_ = false;
    float nextHz_ = 0.0f;
    double lastSnap_ = -1.0;
    detail::Smoother smSync_, smUnison_, smSub_, smWave_;
    float svSync_[kControlBlock], svUnison_[kControlBlock], svSub_[kControlBlock], svWave_[kControlBlock];
    float det_[detail::kAnalogUnison] = {}, gain_[detail::kAnalogUnison] = {};
    float r0_[4] = {}, rd_[4] = {};
    bool osc2On_ = false;
    float ratio2Eff_ = 1.0f;

    // voice state
    float master_[detail::kAnalogUnison] = {}, slave_[detail::kAnalogUnison] = {};
    float held_ = 0.0f, owed_ = 0.0f, sub_ = 1.0f, sub2_ = 1.0f;
    float ph2_ = 0.0f, held2_ = 0.0f, owed2_ = 0.0f;
    float ramp_[4] = {};
    bool rampSeeded_ = false;
    float ring_[detail::AnalogHalfband::kNumTaps] = {};
    int ringPos_ = 0;
    Sidechain side_{};
    Declick declick_;
    bool declickInside_ = true;
};

}  // namespace petal
