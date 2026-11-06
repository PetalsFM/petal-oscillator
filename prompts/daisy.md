# Prompt: the Petal Oscillator on an Electrosmith Daisy

Paste everything below this line as your first message, with `include/petal/` attached or in the project.

---

I am running the Petal Oscillator on an Electrosmith Daisy (Seed or Pod, Cortex-M7 with FPU, libDaisy toolchain, arm-none-eabi-g++ with `-std=c++17`). It is a header-only C++17 oscillator in two modes, Complex (phase modulation, `petal/oscillator.hpp`) and Analog (two band-limited oscillators with hard sync, `petal/analog.hpp`), and both lock their ratios to the intervals of a tuning. Nothing allocates on the audio thread. What the headers need:

- Analog (`analog.hpp`): `<algorithm>`, `<cmath>` and `<cstdint>`, and no tables.
- Complex (`oscillator.hpp`): the same, plus `<vector>`. It reads band-limited tables built once, the first time a `petal::Oscillator` is constructed, into one static shared by every Complex voice: 508,152 bytes (2 x 31 x 2049 floats). Building it puts three temporary `std::vector<double>` of 2,048 values (48 KB) on the heap.
- `parseScl` (`scl.hpp`): `<cstddef>`, `<cstdio>` and `<cstring>`.
- One `petal::Oscillator` is 4,496 bytes and one `petal::AnalogOscillator` 2,208, as built with clang on arm64. None of this has been built with arm-none-eabi or run on a Daisy.

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
bool parseScl(const char* buf, std::size_t len, Tuning& out);   // petal/scl.hpp, from a buffer, no filesystem needed
}
```

What I want:

1. A fixed array of voices (start with 4; `petal::Oscillator voices[4];` as globals or in a static struct, never on the heap). Before anything else, build it and show me the memory report: the Complex table is about 496 KB of static RAM, and the Seed's STM32H750 has 512 KB in its largest internal SRAM (ST's datasheet, not measured here). If it does not fit beside the program, tell me, and offer Analog voices instead, which have no table. Do not move or shrink the table by editing the headers. `setSampleRate(48000.f)` for each in `main` after `hw.Init()`, matching `hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ)`. Block size 48 is fine; the oscillator does not care how the block is sliced.
2. The tuning comes from a `.scl` text embedded as a string constant (no filesystem): `petal::parseScl(text, strlen(text), tuning)` once at startup, then `setTuning(&tuning)` for every voice. Make the constant easy to swap.
3. MIDI in (USB or TRS through libDaisy's `MidiUsbHandler` or `MidiUartHandler`): note on allocates the oldest free voice, `hz = tuning.noteToFrequency(note)`, `noteOn(hz, voiceWasSounding)`; note off releases through my envelope. One `daisysp::Adsr` per voice for the envelope and gain.
4. Knobs (Pod has two; Seed has whatever I wire): map to RATIO 0.5 to 16 and FM 0 to 0.2 first, then FEEDBACK -1 to 1. One button toggles LOCK (`setRatioLock`) on every voice. Read the ADCs once per audio callback and push to every voice before `process`. Do not change these ranges.
5. In the audio callback, for each sample sum `voice.process() * env.Process(gate)` over the voices, scale by `1.f / 4`, write to both outputs. No allocation, no printing in the callback.
6. Compiler flags: set `CPP_STANDARD = -std=c++17` in the Makefile, because libDaisy's core Makefile defaults to gnu++14 and the header needs C++17. `-O2 -ffast-math` is fine if I also add `-fno-finite-math-only`, so the header's `std::isfinite` guards stay in. GCC fuses multiplies and adds by default in C++ (`-ffp-contract=fast`), with or without `-ffast-math`, so the output can differ from the reference build's: by little for Complex when only fusing differs, but Analog's cross FM drifts apart in timing over a note (`MODES.md` has the numbers). How far `-ffast-math` moves the sound has not been measured. Add `-ffp-contract=off` if I want the sine unfused. The Seed's internal flash is only 128 KB, and with libDaisy's Pod support and the load report an `-O2` build can overflow it, so build with `APP_TYPE = BOOT_SRAM` and the Daisy bootloader if it does, and then check the program and the Complex table fit in RAM together; keep `-fsingle-precision-constant` off because the `Tuning` maths is `double` at control rate, which the M7's FPU handles.
7. Tell me the measured load: after it runs, report the time for one callback against the block's duration, so I know how many voices fit. Start at 4 and I will raise it.
8. Keep the headers in `petal/` unmodified.

My board and wiring: [Seed or Pod, which pins or pots, MIDI over USB or TRS, libDaisy version]

Please write `main.cpp` and the Makefile changes.
