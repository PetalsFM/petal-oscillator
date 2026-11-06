//          .--.
//     .--.(    ).--.
//    (    )'--'(    )
//     '--'.--.'--'
//        (    )
//         '--'
//
// petal/mts_esp.hpp - optional: read the live MTS-ESP tuning into a petal::Tuning.
//
// Needs ODDSound's MTS-ESP client (https://github.com/ODDSound/MTS-ESP, the
// Client folder: libMTSClient.h and libMTSClient.cpp) on your include path and
// in your build. Nothing else in petal/ needs it.
//
//   MTSClient* c = MTS_RegisterClient();
//   petal::Tuning t;
//   if (petal::tuningFromMts(c, t)) osc.setTuning(&t);
//   osc.noteOn(petal::mtsNoteFrequency(c, note));   // the master's own pitch
//
// The garden and the carrier bank read the scale as periodic; a master that
// maps each note freely still sounds right through mtsNoteFrequency.
//
// Call tuningFromMts off the audio thread, or at most once per block when
// the master's scale changes; it reads one octave of notes.
//
// Copyright (c) 2026 Petals. MIT License, see LICENSE.
#pragma once

#include "core.hpp"
#include "libMTSClient.h"

namespace petal {

// The master's scale around `rootNote`: size from the map size (12 if the
// master gives none), degrees as frequency ratios to the root, the period
// from the master. Returns false when no master is connected.
inline bool tuningFromMts(MTSClient* c, Tuning& out, int rootNote = 60) {
    if (!c || !MTS_HasMaster(c)) return false;
    int size = static_cast<int>(MTS_GetMapSize(c));
    if (size <= 0 || size > kMaxDegrees) size = 12;
    const double root = MTS_NoteToFrequency(c, static_cast<char>(rootNote), -1);
    if (!(root > 0.0)) return false;
    double period = MTS_GetPeriodRatio(c);
    if (!(period > 1.0)) {
        const int up = rootNote + size;
        period = (up <= 127) ? MTS_NoteToFrequency(c, static_cast<char>(up), -1) / root : 2.0;
    }
    double iv[kMaxDegrees];
    for (int d = 1; d < size; ++d) {
        const int n = rootNote + d;
        iv[d - 1] = (n <= 127) ? MTS_NoteToFrequency(c, static_cast<char>(n), -1) / root : 1.0;
    }
    iv[size - 1] = period;
    Tuning t;
    if (!t.setFromIntervals(iv, size)) return false;
    t.rootNote = rootNote;
    t.rootHz = root;
    out = t;
    return true;
}

inline float mtsNoteFrequency(MTSClient* c, int note, int channel = -1) {
    return static_cast<float>(MTS_NoteToFrequency(c, static_cast<char>(note), static_cast<signed char>(channel)));
}

}  // namespace petal
