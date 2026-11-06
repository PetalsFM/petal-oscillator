//          .--.
//     .--.(    ).--.
//    (    )'--'(    )
//     '--'.--.'--'
//        (    )
//         '--'
//
// petal/declick.hpp - what every Petal mode runs last: the onset window and
// the DC blocker. Header-only C++17, no dependencies beyond core.hpp.
//
// A fresh note rises over 5.33 ms (a raised cosine). A note over a sounding
// tail ducks the tail over 1.33 ms first, then rises. Both are lengths in time,
// 256 and 64 samples at 48 kHz. After the window, a
// one-pole highpass at about 10 Hz takes out any DC.
//
// An oscillator runs its own Declick by default. To put a folder (or anything
// else) between the wave and the window, as Folium does, turn that off with
// setDeclickInside(false) and run the oscillator's declick() on your output.
// A stage between them that has a DC blocker of its own should have it off,
// or the signal is blocked twice:
//
//     float y = osc.process();
//     y = folder.process(y);
//     y = osc.declick().process(y);
//
// Copyright (c) 2026 Petals. MIT License, see LICENSE.
#pragma once

#include "core.hpp"

#include <cmath>

namespace petal {

class Declick {
public:
    void setSampleRate(float sr) {
        sr_ = sr > 0.0f ? sr : kDefaultSampleRate;
        const float scale = sr_ / 48000.0f;
        step_ = 1.0f / scale;
    }

    // No window running, the blocker at rest.
    void reset() {
        pos_ = 1.0f; dir_ = 0; armed_ = false;
        x1_ = y1_ = 0.0f;
    }

    // Called by the oscillator at a note's first sample. `tailSounding`: the
    // last note is still audible, so duck it before the rise.
    void noteOn(bool tailSounding) {
        if (tailSounding) dir_ = -1;
        else { pos_ = 0.0f; dir_ = 1; }
        armed_ = true;
    }

    float process(float x) { return block(window(x)); }

private:
    float window(float x) {
        using namespace detail;
        if (!(dir_ != 0 || pos_ < 1.0f || armed_)) return x;
        armed_ = false;
        const float kRiseStep = (1.0f / 256.0f) * step_;
        const float kFallStep = (1.0f / 64.0f) * step_;
        if (dir_ == 0 && pos_ >= 1.0f) return x;
        x *= 0.5f * (1.0f - std::cos(kPi * pos_));
        if (dir_ < 0) {
            pos_ -= kFallStep;
            if (pos_ <= 0.0f) { pos_ = 0.0f; dir_ = 1; }
        } else {
            pos_ += kRiseStep;
            if (pos_ >= 1.0f) { pos_ = 1.0f; dir_ = 0; }
        }
        return x;
    }

    float block(float x) {
        const float R = 1.0f - (detail::kDcBlockRadians / sr_);
        y1_ = x - x1_ + R * y1_;
        x1_ = x;
        return y1_;
    }

    float sr_ = kDefaultSampleRate;
    float step_ = 48000.0f / kDefaultSampleRate;
    float pos_ = 1.0f;
    int dir_ = 0;
    bool armed_ = false;
    float x1_ = 0.0f, y1_ = 0.0f;
};

}  // namespace petal
