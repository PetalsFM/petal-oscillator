# Prompt: the Petal Oscillator in a JUCE synth

Paste everything below this line as your first message, with `include/petal/` attached or in the project.

---

I am adding the Petal Oscillator to my JUCE synthesizer. It is a header-only C++17 oscillator in two modes, Complex (phase modulation, `petal/oscillator.hpp`) and Analog (two band-limited oscillators with hard sync, `petal/analog.hpp`), and both lock their ratios to the intervals of a tuning. I want one `petal::Oscillator` per voice inside my `juce::SynthesiserVoice`, with my own envelope and gain around it.

The calls I need, with the signatures the headers declare (a summary, not a copy of the headers):

```cpp
namespace petal {
struct Tuning { bool setFromIntervals(const double* intervals, int count); double noteToFrequency(int midiNote) const; int rootNote; double rootHz; };
struct GardenDegree { int degree; int octave; };
enum class Wave : int { Sine, Skewed, Carrier };
// Both modes have: void setSampleRate(float sr); void reset();
//   void setTuning(const Tuning* t, const GardenDegree* garden = nullptr, int gardenCount = 0);
//   void noteOn(float hz, bool tailSounding = false); void setFrequency(float hz);
//   float process(); void process(float* out, int n); double lockedRatio() const;
class Oscillator {          // Complex, petal/oscillator.hpp
  void setRatio(float r /*0.5..16*/); void setRatioLock(bool on); void setFm(float index /*0..0.2*/);
  void setFeedback(float f /*-1..1*/); void setWave(Wave w); void setShape(float s /*0..1*/);
};
class AnalogOscillator {    // Analog, petal/analog.hpp
  void setWave(float s /*0 tri, 1/3 saw, 2/3 square, 1 pulse*/); void setSync(float r /*0.5..16, 1 none*/); void setSyncLock(bool on);
  void setUnison(float u /*0..1*/); void setDetune(float cents /*0..200*/); void setSub(float s /*-1..1*/);
  void setWave2(float s /*0..1*/); void setRatio2(float r /*0.5..16*/); void setLevel2(float l /*0..1*/);
  void setRing(float r /*0..1*/); void setCross(float x /*0..1*/);
};
bool loadScl(const char* path, Tuning& out);   // petal/scl.hpp, call off the audio thread
}
```

Rules to keep:

1. One `petal::Oscillator` per voice. Call `setSampleRate` in `prepareToPlay` (or when the voice's sample rate changes), and `setTuning(&tuning)` with a `Tuning` the processor owns. The processor starts with a 12-TET `Tuning` built with `setFromIntervals` from the twelve ratios 2^(k/12), k = 1 to 12, so LOCK works before any `.scl` is loaded (a `Tuning` with no degrees leaves the garden empty and LOCK does nothing). `setTuning` copies the `Tuning` into the oscillator.
2. In `startNote`, compute the frequency with `tuning.noteToFrequency(midiNote)`; note 60 is the root at 261.63 Hz, so in 12-TET it matches `juce::MidiMessage::getMidiNoteInHertz`. JUCE calls `stopNote(0, false)` just before `startNote` when it reuses a voice, so record there whether my envelope was still open, then call `osc.noteOn(hz, voiceWasStillSounding)`.
3. In `renderNextBlock`, call `osc.process(buffer, numSamples)` into a scratch buffer, multiply by my envelope, add to the output. Do not allocate in the audio callback. JUCE's own `juce::Synthesiser` takes a lock in its note handling; that is JUCE's code, not the oscillator's.
4. Parameters: RATIO 0.5 to 16 (default 1.0), FM 0 to 0.2 shown as 0 to 100 percent (default 0.04), FEEDBACK -1 to 1 (default 0), SHAPE 0 to 1 (default 0), WAVE a three-way choice, LOCK a toggle. For Analog instead: WAVE 0 to 1 (default 0.333, a saw), SYNC 0.5 to 16 (default 1, no sync), UNISON 0 to 1, DETUNE 0 to 200 cents (default 25), SUB -1 to 1, WAVE 2 0 to 1 (default 0.333), RATIO 2 0.5 to 16 (default 1), LEVEL 2, RING and CROSS 0 to 1 (those default 0), LOCK on `setSyncLock`. Read them from an `AudioProcessorValueTreeState` and push them to every voice once per block, before `process`. Do not change these ranges.
5. Pitch bend and glide go through `osc.setFrequency(hz)`, not through a new `noteOn`.
6. Loading a `.scl` happens on the message thread with `petal::loadScl`, then the new `Tuning` is swapped in atomically (a pointer swap or a lock-free exchange), never written while the audio thread reads it. Because the oscillator keeps its own copy, the audio thread calls `osc.setTuning(newTuning)` on every voice at the top of the next block, and the old `Tuning` stays alive until no voice can still read it.
7. No custom UI. `hasEditor` returns true and `createEditor` returns a `juce::GenericAudioProcessorEditor`, so the host shows the parameters as plain sliders. With no file picker, the `.scl` path lives in the plugin state and is loaded in `setStateInformation`, so a saved session brings its tuning back.
8. Keep the headers in `petal/` unmodified. Anything host-specific lives in my code.

My project: [describe your plugin here: JUCE version, number of voices, where parameters live, whether you already have an envelope class]

Please write the voice class and the parameter plumbing, and show me where each call goes.
