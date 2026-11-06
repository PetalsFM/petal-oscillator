# Prompt: the Petal Oscillator as a VCV Rack module

Paste everything below this line as your first message, with `include/petal/` attached or in the plugin's `src/`.

---

I am wrapping the Petal Oscillator as a VCV Rack 2 module. It is a header-only C++17 oscillator in two modes, Complex (phase modulation, `petal/oscillator.hpp`) and Analog (two band-limited oscillators with hard sync, `petal/analog.hpp`), and both lock their ratios to the intervals of a tuning loaded from a Scala `.scl` file.

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

What the module does:

1. Polyphonic, up to 16 channels, following the channel count of the V/OCT input. One `petal::Oscillator` per channel, stored in a fixed array of 16.
2. V/OCT: 1 V per octave, 0 V = C4 = 261.6256 Hz. Each sample, `hz = 261.6256f * std::pow(2.f, voct)` goes to `setFrequency(hz)`; call `noteOn` once when the channel first appears or on a rising GATE edge if a GATE input is present.
3. Knobs: RATIO 0.5 to 16 (default 1.0), FM 0 to 0.2 displayed 0 to 100 percent (default 0.04), FEEDBACK -1 to 1, SHAPE 0 to 1, OCTAVE stepped -3 to 3. A LOCK button with a light. A WAVE button cycling SINE, WARP (the `Skewed` wave), CARRIER with three lights. Do not change these ranges.
4. CV inputs with attenuverters for RATIO, FM, FEEDBACK and SHAPE: ±5 V spans the full knob range, scaled by the attenuverter, added to the knob, clamped to the range, polyphonic per channel.
5. OUT: ±5 V, polyphonic. Complex can peak a little above ±1 (1.09 measured on the skewed wave at shape 2/3, MIDI note 36), so multiply by 5 and expect peaks past ±5 V. If I ask for Analog, it sums its parts: it measured 5.89 with pulse waves, the second oscillator, the sub two octaves down and the unison all full, so scale it before the output.
6. `setSampleRate` in `onSampleRateChange` and in the constructor. `reset` in `onReset`.
7. Context menu: "Load scale (.scl)" opens a file dialog (`osdialog`), reads the file with `petal::loadScl` on the UI thread, then swaps the new `Tuning` in and calls `setTuning` with it on every channel from `process` (the oscillator keeps its own copy), without writing it while `process` runs (a small ring of slots and an atomic generation counter, so a second load cannot overwrite the slot the audio thread is copying). Show the loaded scale's name. With no scale loaded, build a 12-TET `Tuning` in the constructor with `setFromIntervals` from the twelve ratios 2^(k/12), k = 1 to 12, so LOCK works from the start (a `Tuning` with no degrees leaves the garden empty and LOCK does nothing).
8. Parameters are read once per `process` call; that is per sample in Rack, which is fine. If CPU matters, read knobs every 64 samples and keep V/OCT per sample.
9. No panel artwork. The panel is a plain filled rectangle SVG with no text, and the knobs, ports, buttons and lights are Rack's stock components placed in code.
10. Keep the headers in `petal/` unmodified.

My plugin: [slug, whether you already have a plugin skeleton, panel width in HP if decided]

Please write `PetalOscillator.cpp` with the module and the widget skeleton, and `plugin.json` entry. The header is C++17 and Rack's build uses C++11, so tell me to add `CXXFLAGS += -std=c++17` to my Makefile after the `include $(RACK_DIR)/plugin.mk` line.
