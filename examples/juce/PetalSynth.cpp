//          .--.
//     .--.(    ).--.
//    (    )'--'(    )
//     '--'.--.'--'
//        (    )
//         '--'
//
// PetalSynth.cpp - the entry point JUCE calls to make the example instrument.
//
// Copyright (c) 2026 Petals. MIT License, see LICENSE.
#include "PetalSynth.h"
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PetalSynth(); }
