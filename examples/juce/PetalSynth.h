//          .--.
//     .--.(    ).--.
//    (    )'--'(    )
//     '--'.--.'--'
//        (    )
//         '--'
//
// A minimal JUCE instrument around the Petal Oscillator: eight voices, last
// note steals the oldest, a short release so notes do not cut off. It plays in
// 5-limit just intonation with the ratio locked to 5/4, so the lock has a
// scale to land on.
// Build it with your own JUCE (see CMakeLists.txt beside this file); JUCE is
// not part of this repository.
//
// Copyright (c) 2026 Petals. MIT License, see LICENSE.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "petal/oscillator.hpp"

class PetalSynth : public juce::AudioProcessor {
public:
    PetalSynth() : AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true)) {
        // tunings/just5.scl, as ratios to the root.
        const double just[] = { 16.0 / 15, 9.0 / 8, 6.0 / 5, 5.0 / 4, 4.0 / 3, 45.0 / 32,
                                3.0 / 2, 8.0 / 5, 5.0 / 3, 9.0 / 5, 15.0 / 8, 2.0 };
        tuning.setFromIntervals(just, 12);
        for (auto& v : voices) {
            v.osc.setTuning(&tuning);
            v.osc.setRatio(1.27f); v.osc.setRatioLock(true);  // snaps to 5/4
            v.osc.setFm(0.06f);
        }
    }

    void prepareToPlay(double sampleRate, int) override {
        for (auto& v : voices) { v.osc.setSampleRate(static_cast<float>(sampleRate)); v.note = -1; v.gain = 0; }
        release = 1.0f - std::exp(-1.0f / (0.08f * static_cast<float>(sampleRate)));   // 80 ms
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
        buffer.clear();
        int pos = 0;
        for (const auto m : midi) {
            render(buffer, pos, m.samplePosition - pos);
            pos = m.samplePosition;
            const auto msg = m.getMessage();
            if (msg.isNoteOn()) noteOn(msg.getNoteNumber());
            else if (msg.isNoteOff()) for (auto& v : voices) if (v.note == msg.getNoteNumber()) v.held = false;
        }
        render(buffer, pos, buffer.getNumSamples() - pos);
    }

    // The rest is JUCE boilerplate.
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "PetalSynth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    struct Voice { petal::Oscillator osc; int note = -1; bool held = false; float gain = 0; unsigned age = 0; };
    petal::Tuning tuning;
    Voice voices[8];
    unsigned clock = 0;
    float release = 0.001f;

    void noteOn(int note) {
        Voice* v = &voices[0];
        for (auto& c : voices) if (c.age < v->age) v = &c;   // the oldest
        const bool sounding = v->gain > 1e-4f;
        v->osc.noteOn(static_cast<float>(tuning.noteToFrequency(note)), sounding);
        v->note = note; v->held = true; v->gain = 1; v->age = ++clock;
    }
    void render(juce::AudioBuffer<float>& b, int start, int n) {
        for (int i = start; i < start + n; ++i) {
            float s = 0;
            for (auto& v : voices) {
                if (v.gain <= 1e-4f && !v.held) continue;
                if (!v.held) v.gain -= v.gain * release;
                s += v.gain * v.osc.process();
            }
            s *= 0.2f;
            for (int ch = 0; ch < b.getNumChannels(); ++ch) b.setSample(ch, i, s);
        }
    }
};
