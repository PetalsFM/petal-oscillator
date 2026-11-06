//          .--.
//     .--.(    ).--.
//    (    )'--'(    )
//     '--'.--.'--'
//        (    )
//         '--'
//
// The Petal Oscillator inside a madronalib SignalBlock loop, the way a
// madronalib instrument wraps it. madronalib is not needed by petal/; this
// file only shows the shape. Build it against your own madronalib checkout.
//
// Copyright (c) 2026 Petals. MIT License, see LICENSE.
#include "MLDSPOps.h"
#include "petal/oscillator.hpp"

struct PetalVoice {
    petal::Oscillator osc;

    void prepare(float sampleRate) { osc.setSampleRate(sampleRate); }
    void noteOn(float hz, bool tailSounding) { osc.noteOn(hz, tailSounding); }

    // One 64-sample SignalBlock per call; the oscillator keeps its own control
    // count, so the result is the same at any vector size.
    ml::SignalBlock operator()(const ml::SignalBlock& hz) {
        ml::SignalBlock y;
        for (size_t i = 0; i < ml::kFramesPerBlock; ++i) {
            osc.setFrequency(hz[i]);
            y[i] = osc.process();
        }
        return y;
    }
};
