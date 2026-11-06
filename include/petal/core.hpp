//          .--.
//     .--.(    ).--.
//    (    )'--'(    )
//     '--'.--.'--'
//        (    )
//         '--'
//
// petal/core.hpp - what the Petal modules share, header-only C++17, no
// dependencies: the tuning and the ratio garden, the sine, the integer phasor,
// the halfband oversampler, the parameter smoother and the DC blocker's pole.
//
// Nothing module-specific lives here, so a module that needs these pieces
// can carry this file unchanged.
//
// Copyright (c) 2026 Petals. MIT License, see LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace petal {

constexpr int kControlBlock = 64;      // samples between control updates
constexpr int kMaxDegrees = 64;        // largest tuning a Tuning holds
constexpr int kMaxGardenSteps = 8;     // ratios a garden holds
constexpr double kC4Hz = 261.6255653;  // middle C, the default root and the resting pitch
constexpr float kDefaultSampleRate = 48000.0f;

// ---------------------------------------------------------------------------
// Tuning: a scale as plain ratios plus a period. degree 0 is always 1/1.
// size == 0 means no tuning (12-TET at A4 = 440 Hz, and the garden is off).
// Note `rootNote` sounds `rootHz`; every other note is found by degree and period.
// ---------------------------------------------------------------------------
struct Tuning {
    double ratio[kMaxDegrees] = {1.0};
    int size = 0;
    double period = 2.0;
    int rootNote = 60;
    double rootHz = kC4Hz;

    bool active() const { return size > 0; }

    // Build from `count` ratios above 1/1, the last of which is the period,
    // which is how a .scl file lists them. Returns false if they cannot form a
    // scale, and then changes nothing: every value is checked before any is
    // written, so a bad list cannot leave a half-replaced tuning behind.
    bool setFromIntervals(const double* intervals, int count) {
        if (count < 1 || count > kMaxDegrees) return false;
        const double p = intervals[count - 1];
        if (!(p > 1.0) || !std::isfinite(p)) return false;
        for (int i = 0; i < count - 1; ++i)
            if (!(intervals[i] > 0.0) || !std::isfinite(intervals[i])) return false;
        ratio[0] = 1.0;
        for (int i = 1; i < count; ++i) ratio[i] = intervals[i - 1];
        size = count;
        period = p;
        return true;
    }

    double noteToFrequency(int note) const {
        double hz;
        if (!active()) {
            // 12-TET is computed in float on purpose: it is the formula Folium
            // uses, and the match is bit-exact only if the rounding is the same.
            hz = static_cast<double>(440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f));
        } else {
            const int n = size;
            const int delta = note - rootNote;
            const int periods = (delta >= 0) ? (delta / n) : -((-delta + n - 1) / n);
            const int degree = delta - periods * n;
            const double r = (degree >= 0 && degree < size) ? ratio[degree] : 1.0;
            const double per = (period > 1.0 && std::isfinite(period)) ? period : 2.0;
            hz = rootHz * r * std::pow(per, static_cast<double>(periods));
        }
        if (!std::isfinite(hz)) return kC4Hz;
        return std::clamp(hz, 0.001, 1.0e9);
    }
};

// A garden degree: scale degree `degree` in octave (period) `octave` above the root.
struct GardenDegree { int degree; int octave; };

namespace detail {

constexpr float kPi = 3.1415926535897932384626433f;
constexpr double kTwoPiD = 6.283185307179586476925286766559;
constexpr float kTwoPow32 = 4294967296.0f;   // steps in one turn of a 32-bit phasor

// Where the band limit sits, as a fraction of the sample rate: partials and
// shape harmonics above 0.45 sr are dropped (the shape fades them from 0.40).
constexpr float kBandLimitTop = 0.45f;
constexpr float kBandLimitFadeFrom = 0.40f;

// Parameter smoothing: the ratio glides in 5 ms (either way, locked or free),
// every other knob in 10 ms. One-pole time constants.
constexpr float kRatioGlideMs = 5.0f;
constexpr float kParamSmoothMs = 10.0f;

// Ring-buffer index wrap for a value in [0, 2n): one compare instead of a
// modulo on every tap.
inline int wrapOnce(int i, int n) { return (i >= n) ? i - n : i; }

// The sine, on a phasor (any value; whole cycles drop out). Two steps.
// First a pure sine: the phase folds into a triangle x in [-1, 1], and an odd
// 9th-order polynomial bends it, fitted for the least harmonic energy with the
// peak held at 1 and the first and third derivatives at 0 where the triangle
// turns, so the wave is smooth there. Every harmonic sits more than 150 dB
// under the fundamental. Then Le Brun's Chebyshev waveshaping (JAES 27(4),
// 1979) puts back one harmonic on purpose: sin 3x = 3s - 4s^3, so the third,
// a fifth an octave up, at 38 dB under the fundamental and scaled to a peak of
// 1, is (77 s + 4 s^3) / 81. The cycle starts at -1, and the modulator's zero
// sits a quarter turn later. Plain arithmetic, one operation per statement.
// Measured on one Apple arm64 machine. Under clang the sine alone gives
// the same samples at -O2 and -O0, with -ffp-contract=off, and built for
// x86_64 under Rosetta; -ffp-contract=fast, which fuses across statements,
// moves it by up to 5 ULP. The 72 rows of tests/references.txt were written
// from the clang arm64 -O2 build, which fails none of them; x86_64 under
// Rosetta fails 52, -ffp-contract=off 52, -ffp-contract=fast 72 and -O0 6.
// GCC 15.2 on arm64 fails 72 at -O2 and 52 with -ffp-contract=off, and
// passes every other check. MSVC, Windows and Intel hardware were not run.
inline float sineS(float phasor) {
    const float q = phasor - std::floor(phasor);
    float x = q * 4.0f;
    x = x - 1.0f;
    const float folded = 2.0f - x;
    x = (x > 1.0f) ? folded : x;
    const float x2 = x * x;
    float p = x2 * 0.000147349696f;
    p = p + -0.00466379384f;
    p = p * x2;
    p = p + 0.0796816796f;
    p = p * x2;
    p = p + -0.645961404f;
    p = p * x2;
    p = p + 1.57079613f;
    const float s = x * p;
    float s3 = s * s;
    s3 = s3 * 4.0f;
    const float poly = s3 + 77.0f;
    const float y = s * poly;
    return y * (1.0f / 81.0f);
}

// The same function: sineS takes any phase.
inline float sineW(float phasor) { return sineS(phasor); }

// 32-bit integer phasor: increments rounded to nearest-even, read back as a
// float of the whole 32 bits.
struct Phasor {
    uint32_t omega = 0;
    float next(float cyclesPerSample) {
        const float steps = cyclesPerSample * kTwoPow32;
        int32_t inc;
        if (!(steps < 2147483648.0f) || !(steps >= -2147483648.0f)) inc = INT32_MIN;
        else inc = static_cast<int32_t>(std::nearbyint(steps));
        omega += static_cast<uint32_t>(inc);
        return static_cast<float>(omega) * (1.0f / kTwoPow32);
    }
};

// Halfband 2x oversampler for a memoryless nonlinearity (Kaiser-windowed).
template <int N, int BetaX10, int OutPhase>
struct Halfband2x {
    static constexpr int kNumTaps = N;

    // The taps, and the index of every non-zero one (a halfband filter is zero
    // at every other tap, so the loops skip them). Filled once by init().
    static inline float sTaps[N] = {};
    static inline int sNzIdx[N] = {};
    static inline int sNumNz = 0;

    // Fill the tables once. The work runs under the language's thread-safe
    // initialisation of a local static, so two oscillators constructed on two
    // threads at once cannot race it. The constructor calls this, off the
    // audio thread.
    static void init() {
        static const bool built = [] {
            constexpr int M = (N - 1) / 2;
            constexpr double beta = BetaX10 / 10.0;
            auto besselI0 = [](double x) {
                double sum = 1.0, term = 1.0;
                for (int k = 1; k < 40; ++k) {
                    term *= (x / 2.0) * (x / 2.0) / (static_cast<double>(k) * k);
                    sum += term;
                }
                return sum;
            };
            const double i0beta = besselI0(beta);
            double v[N], dcSum = 0.0;
            for (int n = 0; n < N; ++n) {
                const int k = n - M;
                const double sinc = (k == 0) ? 0.5 : std::sin(kPi * k * 0.5) / (kPi * k);
                const double a = 2.0 * n / (N - 1) - 1.0;
                v[n] = sinc * besselI0(beta * std::sqrt(1.0 - a * a)) / i0beta;
                if (k != 0 && (k % 2) == 0) v[n] = 0.0;
                dcSum += v[n];
            }
            for (int n = 0; n < N; ++n) sTaps[n] = static_cast<float>(v[n]);
            for (int n = 0; n < N; ++n) sTaps[n] = static_cast<float>(sTaps[n] / dcSum);
            for (int n = 0; n < N; ++n)
                if (std::abs(sTaps[n]) > 1e-12f) sNzIdx[sNumNz++] = n;
            return true;
        }();
        (void)built;
    }

    float upRing[N] = {0};
    int upPos = 0;
    float dnRing[N] = {0};
    int dnPos = 0;

    template <class F>
    float process(float x, F&& f) {
        float out = 0.0f;
        for (int phase = 0; phase < 2; ++phase) {
            float in = (phase == 0) ? x : 0.0f;
            upRing[upPos] = in;
            upPos = wrapOnce(upPos + 1, N);
            float up = 0.0f;
            for (int j = 0; j < sNumNz; ++j) {
                int t = sNzIdx[j];
                int idx = wrapOnce(upPos - 1 - t + N, N);
                up += 2.0f * sTaps[t] * upRing[idx];
            }
            float y = f(up);
            dnRing[dnPos] = y;
            dnPos = wrapOnce(dnPos + 1, N);
            if (phase == OutPhase) {
                float acc = 0.0f;
                for (int j = 0; j < sNumNz; ++j) {
                    int t = sNzIdx[j];
                    int idx = wrapOnce(dnPos - 1 - t + N, N);
                    acc += sTaps[t] * dnRing[idx];
                }
                out = acc;
            }
        }
        return out;
    }
    float delayedInput() const { return upRing[wrapOnce(upPos + 1, N)]; }
    void push(float x) {
        upRing[upPos] = x;
        upRing[wrapOnce(upPos + 1, N)] = 0.0f;
        upPos = wrapOnce(upPos + 2, N);
    }
    void clear() {
        for (int i = 0; i < N; ++i) { upRing[i] = 0.0f; dnRing[i] = 0.0f; }
        upPos = dnPos = 0;
    }
};

using PMHalfband = Halfband2x<15, 70, 1>;     // the PM oversampler's decimator
using FoldHalfband = Halfband2x<31, 40, 0>;   // the wavefolder's oversampler

// One-pole parameter smoother.
struct Smoother {
    float current = 0.0f;
    float coeff = 1.0f;
    void setTime(float timeMs, float sampleRate) {
        coeff = (timeMs <= 0.0f || sampleRate <= 0.0f)
              ? 1.0f : 1.0f - std::exp(-1.0f / (timeMs * 0.001f * sampleRate));
    }
    void set(float v) { current = v; }
    void run(float target, float* out) {
        for (int i = 0; i < kControlBlock; ++i) {
            current += coeff * (target - current);
            out[i] = current;
        }
    }
};

// The simplicity scan, when no garden degrees are chosen: among the first
// twelve notes of the scale, a note counts as a ratio p/q when a fraction with
// q at most 16 lands within 0.005 of it; the simplest (smallest p + q) win.
constexpr int kScanNotes = 12;
constexpr int kScanMaxDenominator = 16;
constexpr double kScanFitTolerance = 0.005;
constexpr double kScanDuplicate = 0.001;     // two found ratios closer than this are one
constexpr double kChosenDuplicate = 1e-6;    // two chosen degrees closer than this are one
constexpr int kScanBaseScore = 100;          // simplicity = base - p - q; 1/1 scores the base
constexpr int kScanCandidates = 64;
constexpr int kScanFill = kScanCandidates - 4;   // room left for 2, 3 and 4 after the scan

// The DC blocker, one pole at about 10 Hz. The coefficient is
// typed as Folium types it (2 pi times 10, to four figures) so the match holds.
constexpr float kDcBlockRadians = 62.83f;

}  // namespace detail

// ---------------------------------------------------------------------------
// Garden: the ratios the modulator may lock to.
// With chosen degrees: each degree's frequency over the root, deduplicated,
// sorted, with octave projection by the tuning's period. With none: the eight
// simplest p/q ratios (q <= 16) found among the first twelve notes, plus 2, 3, 4.
// ---------------------------------------------------------------------------
class Garden {
public:
    int count() const { return count_; }
    double step(int i) const { return step_[i]; }   // 0 <= i < count(), ascending
    double period() const { return period_; }       // > 1 when octave projection is on

    void build(const Tuning& t, const GardenDegree* sel, int selCount) {
        using namespace detail;
        count_ = 0;
        if (!t.active()) return;
        const double baseFreq = t.noteToFrequency(t.rootNote);
        if (baseFreq <= 0.0) return;
        if (sel && selCount > 0) {
            const int scaleSize = t.size > 0 ? t.size : 12;
            int out = 0;
            for (int i = 0; i < selCount; ++i) {
                const int degree = sel[i].degree, octave = sel[i].octave;
                if (degree < 0 || degree >= scaleSize) continue;
                const int note = t.rootNote + octave * scaleSize + degree;
                if (note < 0 || note > 127) continue;
                const double freq = t.noteToFrequency(note);
                if (freq <= 0.0) continue;
                const double ratio = freq / baseFreq;
                bool dup = false;
                for (int j = 0; j < out; ++j) if (std::abs(step_[j] - ratio) < kChosenDuplicate) { dup = true; break; }
                if (!dup && out < kMaxGardenSteps) step_[out++] = ratio;
            }
            std::sort(step_, step_ + out);
            count_ = out;
            if (out > 0) {
                const double pf = t.noteToFrequency(t.rootNote + scaleSize);
                period_ = (pf > 0.0 && baseFreq > 0.0) ? pf / baseFreq : 2.0;
                if (!(period_ > 1.0)) period_ = 2.0;
                return;
            }
        }
        simplicityScan(t, baseFreq);
    }

    // The garden ratio nearest `target`, within 0.5..16.
    double nearest(double target) const {
        constexpr double kMin = 0.5, kMax = 16.0;
        double best = step_[0];
        double minDist = 1e30;
        if (period_ > 1.0) {
            const double logTgt = std::log2(std::max(target, 1e-9));
            const double logPeriod = std::log2(period_);
            for (int i = 0; i < count_; ++i) {
                const double r = step_[i];
                if (r <= 0.0) continue;
                const long k0 = std::lround((logTgt - std::log2(r)) / logPeriod);
                for (long k = k0 - 1; k <= k0 + 1; ++k) {
                    const double cand = r * std::pow(period_, static_cast<double>(k));
                    if (cand < kMin || cand > kMax) continue;
                    const double d = std::abs(target - cand);
                    if (d < minDist) { minDist = d; best = cand; }
                }
            }
        } else {
            minDist = std::abs(target - best);
            for (int i = 1; i < count_; ++i) {
                const double d = std::abs(target - step_[i]);
                if (d < minDist) { minDist = d; best = step_[i]; }
            }
        }
        return best;
    }

private:
    double step_[kMaxGardenSteps] = {};
    int count_ = 0;
    double period_ = 0.0;

    // No degrees chosen: find the simplest whole-number ratios hiding in the
    // first notes of the scale, add 2, 3 and 4, keep the eight simplest.
    void simplicityScan(const Tuning& t, double baseFreq) {
        using namespace detail;
        double intervals[kScanNotes];
        for (int i = 0; i < kScanNotes; ++i) intervals[i] = t.noteToFrequency(t.rootNote + i) / baseFreq;
        struct Cand { double value; int simplicity; };
        Cand c[kScanCandidates];
        int n = 0;
        c[n++] = {1.0, kScanBaseScore};
        for (int i = 1; i < kScanNotes && n < kScanFill; ++i) {
            const double iv = intervals[i];
            int bp = 1, bq = 1;
            double be = 1e9;
            for (int q = 1; q <= kScanMaxDenominator; ++q) {
                const int p = static_cast<int>(std::round(iv * q));
                if (p < 1) continue;
                const double e = std::abs(static_cast<double>(p) / q - iv);
                if (e < be) { be = e; bp = p; bq = q; }
            }
            if (be < kScanFitTolerance) {
                const double r = static_cast<double>(bp) / bq;
                bool dup = false;
                for (int k = 0; k < n; ++k) if (std::abs(c[k].value - r) < kScanDuplicate) { dup = true; break; }
                if (!dup) c[n++] = {r, kScanBaseScore - bp - bq};
            }
        }
        for (int m = 2; m <= 4 && n < kScanFill; ++m) {
            bool dup = false;
            for (int k = 0; k < n; ++k) if (std::abs(c[k].value - m) < kScanDuplicate) { dup = true; break; }
            if (!dup) c[n++] = {static_cast<double>(m), kScanBaseScore - m};
        }
        // Simplest first. Kept as the plain exchange it always was: a library
        // sort with a different tie order would change which eight survive.
        for (int i = 0; i < n - 1; ++i)
            for (int j = i + 1; j < n; ++j)
                if (c[j].simplicity > c[i].simplicity) std::swap(c[i], c[j]);
        count_ = std::min(n, kMaxGardenSteps);
        for (int i = 0; i < count_; ++i) step_[i] = c[i].value;
        period_ = 0.0;
        std::sort(step_, step_ + count_);
    }
};

}  // namespace petal
