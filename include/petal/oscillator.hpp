//          .--.
//     .--.(    ).--.
//    (    )'--'(    )
//     '--'.--.'--'
//        (    )
//         '--'
//
// petal/oscillator.hpp - the Petal Oscillator, header-only C++17, no dependencies
// beyond core.hpp beside it.
//
// A phase-modulation oscillator whose modulator ratio can lock to the intervals
// of any tuning (the "ratio garden"). Three waves: sine, skewed and carrier (an
// additive bank whose partials sit on the tuning's own degrees). Skewed morphs
// from the sine through a triangle (1/k^2) and a square (1/k), both read from
// band-limited wavetables, to a pivoted sine. In Folium a wavefolder follows
// it; the oscillator hands a folder the fundamental's phase and level
// (sidechain()) and is told the folder's fold (setFoldAfter()) so its skewed
// wave leaves the folder room under Nyquist, as Folium's does.
//
// Use it anywhere: call process() once per sample, or process(out, n) for a
// block of any length, at any sample rate. Nothing allocates after
// construction, nothing locks, nothing calls the system: safe on an audio thread.
//
// Control rate. Parameter targets, the ratio snap, the oversampler choice and
// the carrier bank are evaluated every kControlBlock samples, counted from the
// last reset() or noteOn(), never from the host's buffer size. So the output
// does not depend on how you slice the blocks. Frequency is read per sample.
//
// Copyright (c) 2026 Petals. MIT License, see LICENSE.
#pragma once

#include "core.hpp"
#include "declick.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace petal {

constexpr int kMaxCarrierPartials = 48;

enum class Wave : int { Sine = 0, Skewed = 1, Carrier = 2 };

namespace detail {

// The FM index fades out as the modulator nears Nyquist, from sr/3 to sr/2.5,
// and the modulator itself is held at sr/2.5.
constexpr float kFmAliasFadeStart = 1.0f / 3.0f;
constexpr float kFmAliasFadeEnd = 1.0f / 2.5f;

// 4x carrier + modulator + phase modulation, decimated by two halfband stages.
struct PMOversampler {
    static constexpr int kFactor = 4;
    static constexpr int kT = PMHalfband::kNumTaps;
    uint32_t omegaCar = 0, omegaMod = 0;
    float dn0[kT] = {0}; int dn0p = 0;
    float dn1[kT] = {0}; int dn1p = 0;
    void clear() {
        omegaCar = omegaMod = 0;
        for (int i = 0; i < kT; ++i) dn0[i] = dn1[i] = 0.0f;
        dn0p = dn1p = 0;
    }
    void seed(uint32_t car, uint32_t mod) { omegaCar = car; omegaMod = mod; }
    static float phaseStep(uint32_t& omega, uint32_t inc) {
        omega += inc;
        return static_cast<float>(omega) * (1.0f / kTwoPow32);
    }
    bool pushDecimate(float x, int nxIndex, float& out) {
        const int NN = PMHalfband::sNumNz;
        const int* NZ = PMHalfband::sNzIdx;
        const float* TP = PMHalfband::sTaps;
        dn0[dn0p] = x; dn0p = wrapOnce(dn0p + 1, kT);
        if ((nxIndex & 1) != 1) return false;
        float s0 = 0.0f;
        for (int j = 0; j < NN; ++j) { int t = NZ[j]; int idx = wrapOnce(dn0p - 1 - t + kT, kT); s0 += TP[t] * dn0[idx]; }
        dn1[dn1p] = s0; dn1p = wrapOnce(dn1p + 1, kT);
        if (((nxIndex >> 1) & 1) != 1) return false;
        float s1 = 0.0f;
        for (int j = 0; j < NN; ++j) { int t = NZ[j]; int idx = wrapOnce(dn1p - 1 - t + kT, kT); s1 += TP[t] * dn1[idx]; }
        out = s1;
        return true;
    }
};

// ---- the skewed wave --------------------------------------------------------
constexpr float kShapeSkewMax = 0.44f;
constexpr float kWaveXfadeSamps = 256.0f;            // a mid-note wave change crossfades over this
constexpr float kWaveXfadeStep = 1.0f / kWaveXfadeSamps;

// The square's odd harmonics alternate in sign (slot i is harmonic 2i + 1), so
// its edges are square; summed all positive they sag. The square-to-pivot
// crossfade is then renormalised by the two waves' correlation, kWarpSqPivotRho.
inline float warpSquareSign(int i) { return (i & 1) ? -1.0f : 1.0f; }
constexpr float kWarpSqPivotRho = 0.125f;

// The triangle (1/k^2) and the square (1/k) as wavetables, band-limited by
// octave quarters: table j holds every odd harmonic k with k * top_j under
// 0.45, from 511 harmonics in table 0 to the fundamental alone in table 30,
// each RMS-matched to the sine. A note reads the two tables around its band
// and blends them. Built once (the constructor warms it), 2 x 31 x 2049 floats.
struct WarpTables {
    static constexpr int kLen = 2048;          // points per cycle (power of 2)
    static constexpr int kPerOctave = 4;
    static constexpr int kMaxOdd = 511;        // the lowest table's top harmonic
    static constexpr float kTop = 0.45f;       // never reach this, cycles/sample
    static constexpr int kCount = 31;          // the last holds the fundamental alone
    float sq[kCount][kLen + 1];                // + 1 guard point = point 0
    float tri[kCount][kLen + 1];
    static double topOf(int j) { return double(kTop) / kMaxOdd * std::exp2(double(j) / kPerOctave); }
    static int oddCountOf(int j) {             // highest odd k with k * top_j < 0.45
        int k = std::min(kMaxOdd, int(double(kTop) / topOf(j)));
        if (!(k & 1)) --k;
        return std::max(k, 1);
    }
    WarpTables() {
        std::vector<double> cosTab(kLen), accSq(kLen, 0.0), accTri(kLen, 0.0);
        for (int n = 0; n < kLen; ++n) cosTab[static_cast<std::size_t>(n)] = std::cos(6.283185307179586 * n / kLen);
        double eSq = 0.0, eTri = 0.0;
        int next = 1;                          // next odd harmonic to add
        for (int j = kCount - 1; j >= 0; --j) {   // fewest harmonics first, then add
            for (; next <= oddCountOf(j); next += 2) {
                const int i = (next - 1) / 2;
                const double a = static_cast<double>(warpSquareSign(i)) / double(next);
                const double b = ((i & 1) ? -1.0 : 1.0) / (double(next) * next);
                for (int n = 0; n < kLen; ++n) {
                    const double c = -cosTab[(next * n) & (kLen - 1)];
                    accSq[static_cast<std::size_t>(n)] += a * c;
                    accTri[static_cast<std::size_t>(n)] += b * c;
                }
                eSq += a * a;
                eTri += b * b;
            }
            const double gs = 1.0 / std::sqrt(eSq), gt = 1.0 / std::sqrt(eTri);
            for (int n = 0; n < kLen; ++n) {
                sq[j][n] = static_cast<float>(gs * accSq[static_cast<std::size_t>(n)]);
                tri[j][n] = static_cast<float>(gt * accTri[static_cast<std::size_t>(n)]);
            }
            sq[j][kLen] = sq[j][0];
            tri[j][kLen] = tri[j][0];
        }
    }
};
inline const WarpTables& warpTables() {
    static const WarpTables t;
    return t;
}

// The two tables a note reads, and how far it sits between them.
struct ShapeTraverse {
    const float* sqA = nullptr;
    const float* sqB = nullptr;
    const float* triA = nullptr;
    const float* triB = nullptr;
    float blend = 0.0f;                        // 0 reads A, 1 reads B
};
// fNorm: the fastest the phase runs this block, cycles per sample.
inline ShapeTraverse buildShapeTraverse(float fNorm) {
    const WarpTables& w = warpTables();
    float c = static_cast<float>(static_cast<double>(WarpTables::kPerOctave *
                  std::log2(std::max(fNorm, 1e-9f) / static_cast<float>(WarpTables::topOf(0)))) + 1.0);
    int b = 0;
    float t = 0.0f;
    if (c > 0.0f) {                            // also false for a NaN
        c = std::min(c, static_cast<float>(WarpTables::kCount - 1));
        b = static_cast<int>(c);
        t = c - static_cast<float>(b);
    }
    const int b2 = std::min(b + 1, WarpTables::kCount - 1);
    ShapeTraverse tv;
    tv.sqA = w.sq[b];   tv.sqB = w.sq[b2];
    tv.triA = w.tri[b]; tv.triB = w.tri[b2];
    tv.blend = t;
    return tv;
}
inline float warpTableRead(const float* tab, float p) {
    const float x = p * static_cast<float>(WarpTables::kLen);
    int i = static_cast<int>(x);
    const float f = x - static_cast<float>(i);
    i &= WarpTables::kLen - 1;
    return tab[i] + f * (tab[i + 1] - tab[i]);
}
inline float shapeSquare(float p, const ShapeTraverse& t) {
    const float a = warpTableRead(t.sqA, p);
    return a + t.blend * (warpTableRead(t.sqB, p) - a);
}
inline float shapeTriangle(float p, const ShapeTraverse& t) {
    const float a = warpTableRead(t.triA, p);
    return a + t.blend * (warpTableRead(t.triB, p) - a);
}
// A fold above 1 multiplies the harmonics it is fed, so the table is read for a
// band widened by kWarpFoldBandK per unit of fold, capped at the band an
// 11-harmonic square fills (kWarpFoldOldCap, the cap Folium plays).
constexpr float kWarpFoldBandK = 1.5f;
constexpr float kWarpFoldOldCap = 12.0f;

inline float shapePivot(float p, float depth) {
    const float w = 0.5f - kShapeSkewMax * depth;
    const float pw = (p < w) ? (0.5f * p / w) : (0.5f + 0.5f * (p - w) / (1.0f - w));
    return sineW(pw);
}
inline float shapeSkewed(float phase, float s, const ShapeTraverse& tv) {
    float p = phase - std::floor(phase);
    constexpr float kThird = 1.0f / 3.0f;
    if (s <= kThird) {
        const float t = s * 3.0f;
        return (1.0f - t) * sineW(p) + t * shapeTriangle(p, tv);
    }
    if (s <= 2.0f * kThird) {
        const float t = s * 3.0f - 1.0f;
        return (1.0f - t) * shapeTriangle(p, tv) + t * shapeSquare(p, tv);
    }
    const float t = s * 3.0f - 2.0f;
    const float y = (1.0f - t) * shapeSquare(p, tv) + t * shapePivot(p, 1.0f);
    return y / std::sqrt(1.0f - 2.0f * t * (1.0f - t) * (1.0f - kWarpSqPivotRho));
}

// Scattered, repeatable start phase for carrier partial `degree`.
inline float carrierSeedPhase(int degree) {
    uint32_t x = static_cast<uint32_t>(degree) * 2654435761u + 0x9E3779B9u;
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return static_cast<float>(x & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

constexpr float kFeedbackScale = 0.16f;      // about 1 radian at feedback 1
// Feedback below 0, the minus law: the phase moves by fb x scale x (2 y^2 - 1),
// an even function of the wave, so the edge it leans is the other one. 0 at
// fb 0 from both sides, as the plus law is.
inline float feedbackMinus(float fb, float yAvg) { return fb * kFeedbackScale * (2.0f * yAvg * yAvg - 1.0f); }
// The PM oversampler's decimators delay its output by this many base samples.
constexpr float kPmosCleanLag = 5.0f;
constexpr float kCarrierRolloffMax = 2.5f;
constexpr float kPartialFloor = 1e-5f;       // a carrier partial below this is not rendered
constexpr double kCarrierPartialMinRatio = 0.999;   // partials below the root are skipped

// The PM oversampler engages for notes above this at the onset, and once on,
// stays on until the note falls below the gap, so a note near the edge does
// not flicker between paths.
constexpr float kPmOversampleEngageHz = 1000.0f;
constexpr float kPmOversampleHysteresisHz = 60.0f;

}  // namespace detail

// ---------------------------------------------------------------------------
// Oscillator: one voice.
// ---------------------------------------------------------------------------
class Oscillator {
public:
    Oscillator() {
        detail::PMHalfband::init();
        (void)detail::warpTables();   // built here, off the audio thread
        setSampleRate(kDefaultSampleRate);
    }

    // Sets the rate and resets every state; parameter values jump to their
    // targets (no glide).
    void setSampleRate(float sr) {
        using namespace detail;
        sr_ = sr > 0.0f ? sr : kDefaultSampleRate;
        smRatio_.setTime(kRatioGlideMs, sr_);
        Smoother* const knobs[] = {&smFm_, &smFold_, &smFeedback_, &smShape_};
        for (Smoother* s : knobs) s->setTime(kParamSmoothMs, sr_);
        declick_.setSampleRate(sr_);
        reset();
    }
    float sampleRate() const { return sr_; }

    // Clears every state and snaps every parameter to its target. The oscillator
    // then idles at its resting pitch, note 0, until noteOn() or setFrequency()
    // sets another.
    void reset() {
        modPh_ = {}; modPh_.omega = static_cast<uint32_t>(kModZeroPhase);
        carPh_ = {};
        pmOS_.clear();
        pmOSEngaged_ = pmOSSeeded_ = false;
        fbY1_ = fbY2_ = 0.0f;
        for (int d = 0; d < kMaxCarrierPartials; ++d) bankRe_[d] = bankIm_[d] = 0.0f;
        bankSeeded_ = false;
        waveCur_ = -1; waveFrom_ = 0; xfPos_ = 0.0f; xfActive_ = false;
        declick_.reset();
        side_ = {};
        smRatio_.set(ratioKnob_);
        smFm_.set(fm_); smFold_.set(foldAfter_);
        smFeedback_.set(feedback_); smShape_.set(shape_);
        lastSnap_ = -1.0;
        bankInit_ = false; bankDirty_ = true;
        pos_ = 0;
        onsetPending_ = false;
        hzSwapPending_ = false;
        played_ = false;
        hz_ = static_cast<float>(tuning_.noteToFrequency(0));   // the resting pitch: note 0
    }

    // ---- parameters (applied at the next control update) -------------------
    // A value that is not finite is ignored and the last one kept: std::clamp
    // passes NaN through, and a NaN in a smoother or a phase stays there until
    // reset(), so every output sample after it would be NaN too.
    void setRatio(float r) { if (std::isfinite(r)) ratioKnob_ = r; }         // modulator : carrier, 0.5..16
    void setRatioLock(bool on) { lock_ = on; }                               // snap the ratio to the garden
    void setFm(float index) { if (std::isfinite(index)) fm_ = index; }       // phase-mod depth, 0..0.2 cycles
    void setFoldAfter(float f) { if (std::isfinite(f)) foldAfter_ = f; }     // the fold of the folder after this, 1 (none) .. 8
    void setFeedback(float f) { if (std::isfinite(f)) feedback_ = f; }       // carrier self-PM, -1..1
    void setWave(Wave w) { wave_ = static_cast<int>(w); }
    void setShape(float s) { if (std::isfinite(s)) shape_ = s; }             // brightness within the wave, 0..1

    // The tuning the garden and the carrier bank read. Pass nullptr for none.
    // The garden is the chosen degrees (up to 8), or the simplest ratios if none.
    void setTuning(const Tuning* t, const GardenDegree* garden = nullptr, int gardenCount = 0) {
        tuning_ = t ? *t : Tuning{};
        selCount_ = std::clamp(gardenCount, 0, kMaxGardenSteps);
        for (int i = 0; i < selCount_; ++i) sel_[i] = garden[i];
        garden_.build(tuning_, sel_, selCount_);
        bankDirty_ = true;
        if (!played_) hz_ = static_cast<float>(tuning_.noteToFrequency(0));
    }
    const Garden& garden() const { return garden_; }
    double lockedRatio() const { return lastSnap_; }

    // A frequency that is not finite is ignored, as the setters above ignore one.
    void setFrequency(float hz) {
        if (!std::isfinite(hz)) return;
        if (hzSwapPending_) nextHz_ = hz; else hz_ = hz;
    }

    // Start a note. `tailSounding`: the previous note on this oscillator is
    // still audible, so it is ducked over 64 samples before the new one rises.
    // The phases run on, as in a real oscillator, except on the 4x path (a sine
    // or skewed note that starts above about 1000 Hz), whose onset restarts
    // them from fixed values. The onset sample itself still runs at the
    // previous frequency; the new frequency takes over from the next sample.
    // A frequency that is not finite starts no note.
    void noteOn(float hz, bool tailSounding = false) {
        if (!std::isfinite(hz)) return;
        nextHz_ = hz;
        hzSwapPending_ = true;
        played_ = true;
        onsetPending_ = true;
        busy_ = tailSounding;
        pos_ = 0;   // the onset begins a control block
    }

    // What a folder after this oscillator reads, for the
    // sample process() last returned: the phase of the wave's fundamental in
    // cycles, its frequency in cycles per sample, and its level (0 when the wave
    // has no fundamental to keep: a carrier bank whose scale drops degree 0).
    struct Sidechain { float phase = 0.0f, freq = 0.0f, level = 0.0f; };
    const Sidechain& sidechain() const { return side_; }

    // The onset window and DC blocker run inside process() by default. Off,
    // process() returns the wave as it is, and you run declick() after
    // whatever you put between (declick.hpp).
    void setDeclickInside(bool on) { declickInside_ = on; }
    Declick& declick() { return declick_; }

    // ---- rendering -----------------------------------------------------------
    float process() {
        if (pos_ == 0) controlUpdate();
        const float y = renderSample(pos_);
        if (++pos_ == kControlBlock) {
            pos_ = 0;
            finishControlBlock();
        }
        return y;
    }
    void process(float* out, int n) { for (int i = 0; i < n; ++i) out[i] = process(); }

private:
    static constexpr int32_t kModZeroPhase = -(2 << 29);

    void controlUpdate() {
        using namespace detail;
        const int onset = onsetPending_ ? 0 : -1;

        // Ratio target, snapped to the garden when locked; 5 ms glide either way.
        {
            double target = static_cast<double>(ratioKnob_);
            if (lock_ && garden_.count() > 0) {
                lastSnap_ = garden_.nearest(target);
                target = lastSnap_;
            }
            float ratioTarget = static_cast<float>(std::clamp(target, 0.5, 16.0));
            if (!std::isfinite(ratioTarget)) ratioTarget = 1.0f;
            smRatio_.run(ratioTarget, svRatio_);
        }
        smFm_.run(std::clamp(fm_, 0.0f, 0.2f), svFm_);
        smFold_.run(std::clamp(foldAfter_, 1.0f, 8.0f), svFold_);
        smFeedback_.run(std::clamp(feedback_, -1.0f, 1.0f), svFeedback_);
        smShape_.run(std::clamp(shape_, 0.0f, 1.0f), svShape_);

        const int waveParam = std::clamp(wave_, 0, 2);
        maintainCarrierBank(waveParam);

        // Wave switch: a mid-note change crossfades over 256 samples.
        if (waveCur_ < 0 || onset >= 0) {
            waveCur_ = waveParam; xfActive_ = false; xfPos_ = 0.0f;
        } else if (waveParam != waveCur_ && !xfActive_) {
            waveFrom_ = waveCur_; waveCur_ = waveParam; xfActive_ = true; xfPos_ = 0.0f;
        }
        waveTo_ = waveCur_;
        waveFromEff_ = xfActive_ ? waveFrom_ : waveTo_;

        const float ctrlHz = hzSwapPending_ ? nextHz_ : hz_;
        // Oversampler latch, decided at the onset only.
        if (onset >= 0) {
            if (!pmOSEngaged_) pmOSEngaged_ = (ctrlHz > kPmOversampleEngageHz);
            else pmOSEngaged_ = (ctrlHz > kPmOversampleEngageHz - kPmOversampleHysteresisHz);
            pmOSSeeded_ = false;
            declick_.noteOn(busy_);
            onsetPending_ = false;
        }

        feedbackActive_ = (std::fabs(svFeedback_[kControlBlock - 1]) > 1e-4f) || (std::fabs(svFeedback_[0]) > 1e-4f);
        shapeActive_ = (waveTo_ == static_cast<int>(Wave::Skewed));
        const bool carrierIn = (waveTo_ == static_cast<int>(Wave::Carrier))
                            || (xfActive_ && waveFromEff_ == static_cast<int>(Wave::Carrier));
        xfBlend_ = xfActive_ && !carrierIn;
        xfDip_ = xfActive_ && carrierIn;
        renderMode_ = xfDip_ ? ((xfPos_ < 0.5f) ? waveFromEff_ : waveTo_) : waveTo_;
        renderShaped_ = (renderMode_ == static_cast<int>(Wave::Skewed));

        // The skewed wave's tables are read for the fastest the phase runs this
        // block, at its middle sample: the carrier, plus the FM's deviation
        // (2 pi x depth x f_m, which moves harmonic k by k times that), plus the
        // feedback's steepest edge, then widened for the folder after it.
        constexpr int kTvMid = kControlBlock / 2;
        const SampleIn mid = sampleInputs(kTvMid, ctrlHz);
        const float f0Norm = mid.cfn;
        float fTv = f0Norm + 6.2831853f * mid.fmI * mid.aa * std::fabs(mid.mfn);
        if (feedbackActive_) {
            // the minus law's slope, 4 |fb| k y y', peaks at twice the plus law's
            const float fbTv = mid.fbAmt >= 0.0f ? mid.fbAmt : -2.0f * mid.fbAmt;
            const float b = 3.14159265f * kFeedbackScale * 0.9f * fbTv;
            const float h = fTv * (1.0f + b);
            fTv = 0.5f * (h + std::sqrt(h * h + 4.0f * WarpTables::kTop * b * fTv));
        }
        {
            const float fFold = std::min(fTv * (1.0f + kWarpFoldBandK * std::max(0.0f, svFold_[kTvMid] - 1.0f)),
                                         WarpTables::kTop / kWarpFoldOldCap);
            fTv = std::max(fTv, fFold);
        }
        shapeTv_ = buildShapeTraverse(fTv);

        if (renderMode_ == static_cast<int>(Wave::Carrier)) prepareCarrierBlock(f0Norm);
    }

    void maintainCarrierBank(int waveParam) {
        if (waveParam != static_cast<int>(Wave::Carrier)) return;
        const float knob = std::clamp(shape_, 0.0f, 1.0f);
        if (!bankInit_ || bankDirty_ || std::abs(knob - bankKnob_) > 1e-4f) {
            rebuildCarrierBank(knob);
            bankDirty_ = false;
            bankKnob_ = knob;
        }
        if (!bankInit_) {
            for (int d = 0; d < kMaxCarrierPartials; ++d) bankAmpPrev_[d] = bankAmpCur_[d] = bankAmpTgt_[d];
            bankRenderCount_ = bankPartials_;
            bankInit_ = true;
            return;
        }
        constexpr float kAmpLerp = 0.235f;
        for (int d = 0; d < kMaxCarrierPartials; ++d) {
            bankAmpPrev_[d] = bankAmpCur_[d];
            bankAmpCur_[d] += (bankAmpTgt_[d] - bankAmpCur_[d]) * kAmpLerp;
            // A partial fading to silence would settle on a subnormal and stay
            // there. A host may flush subnormals for the thread or may not, so
            // the core flushes this one itself, far below hearing.
            if (std::abs(bankAmpCur_[d]) < 1e-20f) bankAmpCur_[d] = 0.0f;
        }
        bankRenderCount_ = std::max(bankRenderCount_, bankPartials_);
        while (bankRenderCount_ > bankPartials_ && bankAmpCur_[bankRenderCount_ - 1] < detail::kPartialFloor) --bankRenderCount_;
    }

    void rebuildCarrierBank(float knob) {
        const float rolloff = (1.0f - knob) * detail::kCarrierRolloffMax;
        bool kept[kMaxDegrees] = {};
        int keptClasses = 0;
        const int scaleSize = tuning_.size > 0 ? tuning_.size : 12;
        if (scaleSize <= kMaxDegrees) {
            for (int i = 0; i < selCount_; ++i) {
                const int cls = sel_[i].degree;
                if (cls < 0 || cls >= scaleSize || cls >= kMaxDegrees) continue;
                if (!kept[cls]) { kept[cls] = true; ++keptClasses; }
            }
            if (keptClasses == 1 && kept[0]) keptClasses = 0;
        }
        double ratios[kMaxCarrierPartials];
        int n = 0;
        if (tuning_.active()) {
            const double base = tuning_.noteToFrequency(tuning_.rootNote);
            if (base > 0.0) {
                for (int m = 0; m < 127 && n < kMaxCarrierPartials; ++m) {
                    if (keptClasses > 0 && !kept[m % scaleSize]) continue;
                    const double f = tuning_.noteToFrequency(tuning_.rootNote + m);
                    if (f <= 0.0) continue;
                    const double r = f / base;
                    if (r < detail::kCarrierPartialMinRatio) continue;
                    ratios[n++] = r;
                }
            }
        }
        if (n == 0) {
            for (int m = 0; m < kMaxCarrierPartials; ++m) ratios[m] = static_cast<double>(m + 1);
            n = kMaxCarrierPartials;
        }
        double amp[kMaxCarrierPartials];
        double sumSq = 0.0;
        for (int d = 0; d < n; ++d) {
            amp[d] = std::pow(ratios[d], -static_cast<double>(rolloff));
            sumSq += amp[d] * amp[d];
        }
        const double norm = (sumSq > 1e-12) ? 1.0 / std::sqrt(sumSq) : 1.0;
        for (int d = 0; d < n; ++d) {
            bankRatio_[d] = static_cast<float>(ratios[d]);
            bankAmpTgt_[d] = static_cast<float>(amp[d] * norm);
            bankPhase01_[d] = detail::carrierSeedPhase(d);
        }
        for (int d = n; d < kMaxCarrierPartials; ++d) bankAmpTgt_[d] = 0.0f;
        bankPartials_ = n;
    }

    void prepareCarrierBlock(float f0Norm) {
        using namespace detail;
        if (!bankSeeded_) {
            for (int d = 0; d < kMaxCarrierPartials; ++d) {
                const float ph = bankPhase01_[d];
                bankRe_[d] = sineW(ph + 0.5f);
                bankIm_[d] = sineW(ph + 0.25f);
            }
            bankSeeded_ = true;
        }
        nActive_ = 0;
        for (int d = 0; d < bankRenderCount_; ++d) {
            if (bankRatio_[d] * f0Norm >= kBandLimitTop) break;
            const double ang = kTwoPiD * static_cast<double>(bankRatio_[d]) * static_cast<double>(f0Norm);
            wr_[d] = static_cast<float>(std::cos(ang));
            wi_[d] = static_cast<float>(std::sin(ang));
            nActive_ = d + 1;
        }
        // Band-limit makeup: give back the energy of the partials the cut dropped.
        const float* amps = bankAmpCur_;
        float voiceAmp[kMaxCarrierPartials];
        if (nActive_ > 0) {
            const int span = std::min(std::max(nActive_, bankRenderCount_), kMaxCarrierPartials);
            if (nActive_ < span) {
                double sumSpan = 0.0, sumAct = 0.0;
                for (int d = 0; d < span; ++d) {
                    const double a = static_cast<double>(amps[d]);
                    sumSpan += a * a;
                    if (d < nActive_) sumAct += a * a;
                }
                if (sumAct > 1e-18 && sumSpan > sumAct) {
                    const float g = static_cast<float>(std::sqrt(sumSpan / sumAct));
                    for (int d = 0; d < nActive_; ++d) voiceAmp[d] = amps[d] * g;
                    for (int d = nActive_; d < span; ++d) voiceAmp[d] = amps[d];
                    amps = voiceAmp;
                }
            }
        }
        for (int d = 0; d < nActive_; ++d) {
            const float cur = bankAmpCur_[d], prv = bankAmpPrev_[d];
            const float a0 = (prv == cur) ? amps[d] : (cur != 0.0f ? amps[d] * (prv / cur) : prv);
            aStep_[d] = (amps[d] - a0) * (1.0f / kControlBlock);
            aRun_[d] = a0;
        }
        bankFund_ = nActive_ > 0 && std::fabs(bankRatio_[0] - 1.0f) < 1e-4f;
    }

    // One output sample: the wave by its render path, the wave-change dip,
    // the onset ramp, the DC blocker. `i` is the sample's place in the control
    // block. The render path also writes the sidechain's phase.
    float renderSample(int i) {
        const SampleIn in = sampleInputs(i, hz_);
        side_.freq = in.cfn;
        side_.level = 1.0f;
        float osc = (renderMode_ == static_cast<int>(Wave::Carrier)) ? renderCarrier(in)
                  : pmOSEngaged_                                     ? renderOversampled(in, i)
                                                                     : renderPlain(in, i);
        if (xfDip_) { const float dip = waveDip(i); osc *= dip; side_.level *= dip; }
        if (declickInside_) osc = declick_.process(osc);
        if (hzSwapPending_) { hz_ = nextHz_; hzSwapPending_ = false; }
        return osc;
    }

    // What the three render paths share for one sample.
    struct SampleIn {
        float cfn;        // carrier frequency, cycles per sample
        float mfn;        // modulator frequency, cycles per sample, held at sr/2.5
        float aa;         // anti-alias fade of the FM index, 1 down to 0
        float fmI;        // FM index this sample
        float fbAmt;      // feedback amount, 0 unless feedback is active
        float shapeAmt;   // shape amount, 0 unless the skewed wave or a crossfade is live
    };

    SampleIn sampleInputs(int i, float hz) const {
        using namespace detail;
        const float invSr = 1.0f / sr_;
        SampleIn in;
        in.cfn = hz * invSr;
        float mfn = hz * svRatio_[i];
        mfn = mfn * invSr;
        constexpr float kAARange = kFmAliasFadeEnd - kFmAliasFadeStart;
        {
            float a = std::abs(mfn);
            if (a > kFmAliasFadeStart) {
                float clamped = std::min(a, kFmAliasFadeEnd);
                mfn = (mfn > 0) ? clamped : -clamped;
            }
            in.aa = 1.0f - std::clamp((a - kFmAliasFadeStart) / kAARange, 0.0f, 1.0f);
        }
        in.mfn = mfn;
        in.fmI = svFm_[i];
        in.fbAmt = feedbackActive_ ? std::clamp(svFeedback_[i], -1.0f, 1.0f) : 0.0f;
        in.shapeAmt = (shapeActive_ || xfActive_) ? std::clamp(svShape_[i], 0.0f, 1.0f) : 0.0f;
        return in;
    }

    // Carrier wave: every partial is a complex rotor stepped once, and the
    // whole bank is phase-modulated at once through cos and sin of the
    // deviation. The rotors are renormalised each step so they never drift.
    float renderCarrier(const SampleIn& in) {
        using namespace detail;
        const float modOut = sineS(modPh_.next(in.mfn));
        (void)carPh_.next(in.cfn);   // advanced for continuity
        float dcyc = modOut * in.fmI * in.aa;
        if (feedbackActive_) {
            if (in.fbAmt >= 0.0f) dcyc += in.fbAmt * kFeedbackScale * 0.5f * (fbY1_ + fbY2_);
            else dcyc += feedbackMinus(in.fbAmt, 0.5f * (fbY1_ + fbY2_));
        }
        float cosD = sineW(dcyc + 0.5f);
        float sinD = sineW(dcyc + 0.25f);
        float S = 0.0f, C = 0.0f;
        for (int d = 0; d < nActive_; ++d) {
            float re = bankRe_[d], im = bankIm_[d];
            float nre = re * wr_[d] - im * wi_[d];
            float nim = re * wi_[d] + im * wr_[d];
            float g = 1.5f - 0.5f * (nre * nre + nim * nim);
            nre *= g; nim *= g;
            bankRe_[d] = nre;
            bankIm_[d] = nim;
            float a = (aRun_[d] += aStep_[d]);
            S += a * nim;
            C += a * nre;
        }
        float y = S * cosD + C * sinD;
        if (feedbackActive_) { fbY2_ = fbY1_; fbY1_ = y; }
        // Partial 0 is the fundamental whenever the scale keeps degree 0; its
        // phase-modulated phase is read off its own rotor.
        if (bankFund_) {
            const float re0 = bankRe_[0], im0 = bankIm_[0];
            side_.phase = std::atan2(im0 * cosD + re0 * sinD, re0 * cosD - im0 * sinD) * (1.0f / 6.2831853f) + 0.25f;
        } else {
            side_.phase = 0.0f;
            side_.level = 0.0f;
        }
        return y;
    }

    // High notes: carrier and modulator run at 4x and the PM is decimated,
    // so the sidebands that would fold back are filtered off first.
    float renderOversampled(const SampleIn& in, int i) {
        using namespace detail;
        constexpr int kFactor = PMOversampler::kFactor;
        constexpr float kInvFactor = 1.0f / static_cast<float>(kFactor);
        if (!pmOSSeeded_) {
            pmOS_.seed(0u, static_cast<uint32_t>(kModZeroPhase));
            pmOSSeeded_ = true;
        }
        uint32_t carInc = static_cast<uint32_t>(std::lround(in.cfn * kTwoPow32 * kInvFactor));
        uint32_t modInc = static_cast<uint32_t>(std::lround(in.mfn * kTwoPow32 * kInvFactor));
        float fmDepth = in.fmI * in.aa;
        float decimated = 0.0f, lastCph = 0.0f;
        for (int n = 0; n < kFactor; ++n) {
            float cph = PMOversampler::phaseStep(pmOS_.omegaCar, carInc);
            lastCph = cph;
            float mph = PMOversampler::phaseStep(pmOS_.omegaMod, modInc);
            float msine = sineS(mph);
            const float pm = (feedbackActive_ || renderShaped_ || xfBlend_)
                           ? shapedSample(cph + msine * fmDepth, i, in.fbAmt, in.shapeAmt)
                           : sineS(cph + msine * fmDepth);
            pmOS_.pushDecimate(pm, n, decimated);
        }
        // The decimators delay the PM output by kPmosCleanLag samples against
        // the phasor.
        side_.phase = lastCph - kPmosCleanLag * in.cfn;
        return decimated;
    }

    // Everything else: one carrier sample, phase-modulated by one modulator
    // sample, on the integer phasors.
    float renderPlain(const SampleIn& in, int i) {
        using namespace detail;
        const float modOut = sineS(modPh_.next(in.mfn));
        const float carrierPhase = carPh_.next(in.cfn);
        side_.phase = carrierPhase;   // the PM'd wave's fundamental rides the unmodulated phasor
        float dev = modOut * in.fmI;
        dev = dev * in.aa;
        const float fmPhase = carrierPhase + dev;
        return (!feedbackActive_ && !renderShaped_ && !xfBlend_)
             ? sineS(fmPhase)
             : shapedSample(fmPhase, i, in.fbAmt, in.shapeAmt);
    }

    // A wave change into or out of the carrier bank cannot crossfade (the
    // bank has no phase to blend), so it dips to silence and back instead.
    float waveDip(int i) const {
        using namespace detail;
        float posI = std::min(1.0f, xfPos_ + static_cast<float>(i) * kWaveXfadeStep);
        return std::fabs(std::cos(kPi * posI));
    }

    // The sine or skewed wave at `phase` for render mode `mode`.
    float waveSample(float phase, float shapeAmt, int mode) const {
        return (mode == static_cast<int>(Wave::Skewed)) ? detail::shapeSkewed(phase, shapeAmt, shapeTv_)
                                                        : detail::sineW(phase);
    }

    // One carrier sample with the three things the plain sine path skips:
    // carrier feedback added to the phase, the skewed shape, and the wave
    // crossfade. `i` is the sample's place in the control block, which sets
    // where the crossfade stands. Serves the oversampled and the plain paths.
    float shapedSample(float ph, int i, float fbAmt, float shapeAmt) {
        using namespace detail;
        if (feedbackActive_) {
            float yAvg = 0.5f * (fbY1_ + fbY2_);
            if (fbAmt >= 0.0f) ph += fbAmt * kFeedbackScale * yAvg;
            else ph += feedbackMinus(fbAmt, yAvg);
        }
        float y;
        if (xfBlend_) {
            float posI = std::min(1.0f, xfPos_ + static_cast<float>(i) * kWaveXfadeStep);
            float w = 0.5f * (1.0f - std::cos(kPi * posI));
            y = w * waveSample(ph, shapeAmt, waveTo_)
              + (1.0f - w) * waveSample(ph, shapeAmt, waveFromEff_);
        } else {
            y = renderShaped_ ? shapeSkewed(ph, shapeAmt, shapeTv_) : sineW(ph);
        }
        if (feedbackActive_) { fbY2_ = fbY1_; fbY1_ = y; }
        return y;
    }

    void finishControlBlock() {
        if (xfBlend_ || xfDip_) {
            xfPos_ = std::min(1.0f, xfPos_ + kControlBlock * (1.0f / detail::kWaveXfadeSamps));
            if (xfPos_ >= 1.0f) xfActive_ = false;
        }
    }

    // parameters
    float sr_ = kDefaultSampleRate, hz_ = static_cast<float>(kC4Hz);
    float ratioKnob_ = 1.0f, fm_ = 0.04f, foldAfter_ = 1.0f;
    float feedback_ = 0.0f, shape_ = 0.0f;
    int wave_ = 0;
    bool lock_ = false;
    Tuning tuning_{};
    GardenDegree sel_[kMaxGardenSteps] = {};
    int selCount_ = 0;
    Garden garden_{};

    // control state
    int pos_ = 0;
    bool onsetPending_ = false, busy_ = false;
    bool hzSwapPending_ = false, played_ = false;
    float nextHz_ = 0.0f;
    detail::Smoother smRatio_, smFm_, smFold_, smFeedback_, smShape_;
    float svRatio_[kControlBlock], svFm_[kControlBlock], svFold_[kControlBlock];
    float svFeedback_[kControlBlock], svShape_[kControlBlock];
    double lastSnap_ = -1.0;
    bool feedbackActive_ = false, shapeActive_ = false, xfBlend_ = false, xfDip_ = false;
    bool renderShaped_ = false;
    int renderMode_ = 0, waveTo_ = 0, waveFromEff_ = 0;
    detail::ShapeTraverse shapeTv_{};

    // voice state
    detail::Phasor modPh_, carPh_;
    detail::PMOversampler pmOS_;
    bool pmOSEngaged_ = false, pmOSSeeded_ = false;
    float fbY1_ = 0.0f, fbY2_ = 0.0f;
    int waveCur_ = -1, waveFrom_ = 0;
    float xfPos_ = 0.0f;
    bool xfActive_ = false;
    Declick declick_;
    bool declickInside_ = true;
    Sidechain side_{};

    // carrier bank
    float bankRatio_[kMaxCarrierPartials] = {}, bankAmpTgt_[kMaxCarrierPartials] = {};
    float bankAmpCur_[kMaxCarrierPartials] = {}, bankAmpPrev_[kMaxCarrierPartials] = {};
    float bankPhase01_[kMaxCarrierPartials] = {};
    float bankRe_[kMaxCarrierPartials] = {}, bankIm_[kMaxCarrierPartials] = {};
    float wr_[kMaxCarrierPartials] = {}, wi_[kMaxCarrierPartials] = {};
    float aRun_[kMaxCarrierPartials] = {}, aStep_[kMaxCarrierPartials] = {};
    int bankPartials_ = 0, bankRenderCount_ = 0, nActive_ = 0;
    float bankKnob_ = 0.0f;
    bool bankInit_ = false, bankDirty_ = true, bankSeeded_ = false, bankFund_ = false;
};

}  // namespace petal
