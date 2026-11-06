# For agents and assistants

This is the Petal Oscillator: a header-only C++17 oscillator in two modes, Complex and Analog, whose ratios can lock to any tuning of up to 64 notes. It plays what Folium's oscillator plays, sample for sample before the declick on the reference build in `MODES.md`, with Folium's per-voice variation turned off, and it has to stay that way. This file says how to carry it into a project, and how to change it without breaking what it promises. People read `README.md`; you read this.

## Where things are

```
include/petal/core.hpp         what both modes share: the tuning, the garden, the sine, the phasor
include/petal/oscillator.hpp   Complex, petal::Oscillator
include/petal/analog.hpp       Analog, petal::AnalogOscillator
include/petal/declick.hpp      the onset window and DC blocker every mode runs last
include/petal/scl.hpp          read a Scala .scl into a petal::Tuning (optional)
include/petal/mts_esp.hpp      a petal::Tuning from an MTS-ESP master (optional, needs ODDSound's client)
examples/                      render_wav (--mode complex|analog), a JUCE instrument, a madronalib sketch
tests/                         check.sh, check.cpp and references.txt: the repo held to Folium's renders
prompts/                       a first message per platform, ready to paste
MODES.md                       what holds each mode to Folium, how versions move
```

## The API

```cpp
namespace petal {

struct Tuning {                       // a scale as ratios plus a period
    double ratio[64]; int size; double period; int rootNote; double rootHz;
    bool setFromIntervals(const double* intervals, int count);  // ratios above 1/1, period last
    double noteToFrequency(int midiNote) const;
    bool active() const;              // size > 0; otherwise 12-TET at A4 = 440 Hz
};
struct GardenDegree { int degree; int octave; };
enum class Wave : int { Sine, Skewed, Carrier };

// Both modes share these.
//   void setSampleRate(float sr);  void reset();
//   void setTuning(const Tuning* t, const GardenDegree* garden = nullptr, int gardenCount = 0);
//   void noteOn(float hz, bool tailSounding = false);  void setFrequency(float hz);
//   float process();  void process(float* out, int n);
//   double lockedRatio() const;    // where the lock landed
//   void setDeclickInside(bool on);  Declick& declick();   // put a stage of your own before the declick
//   const Sidechain& sidechain() const;   // for a folder after it: the fundamental's phase, frequency and level

class Oscillator {                    // Complex
    void setRatio(float r);           // modulator to carrier, 0.5 .. 16
    void setRatioLock(bool on);       // snap the ratio to the garden
    void setFm(float index);          // 0 .. 0.2 cycles
    void setFeedback(float f);        // the carrier on itself, -1 .. 1
    void setFoldAfter(float f);       // how hard a folder after it folds, 1 (none) .. 8: the skewed wave leaves it room
    void setWave(Wave w);
    void setShape(float s);           // brightness within the wave, 0 .. 1
};

class AnalogOscillator {              // Analog
    void setWave(float s);            // osc 1: 0 tri, 1/3 saw, 2/3 square, 1 a 10 % pulse
    void setSync(float r);            // hard-sync ratio, 0.5 .. 16; 1 is no sync
    void setSyncLock(bool on);        // snap the sync ratio and osc 2's ratio to the garden
    void setUnison(float u);          // 0 one voice .. 1 seven
    void setDetune(float cents);      // the outer pair at full unison, 0 .. 200
    void setSub(float s);             // 0 .. 1 an octave under, 0 .. -1 two octaves under
    void setWave2(float s);           // osc 2, the same sweep
    void setRatio2(float r);          // osc 2 against the note, 0.5 .. 16
    void setLevel2(float l);          // osc 2 in the mix, 0 .. 1
    void setRing(float r);            // the mix toward osc 1 x osc 2, 0 .. 1
    void setCross(float x);           // osc 2 bends osc 1's rate, 0 .. 1
};

bool parseScl(const char* buf, std::size_t len, Tuning& out);     // scl.hpp
bool loadScl(const char* path, Tuning& out);                       // scl.hpp, off the audio thread
bool tuningFromMts(MTSClient* c, Tuning& out, int rootNote = 60);  // mts_esp.hpp
}
```

The headers are the full list and the last word. Each mode is one voice with no envelope and no amplifier; the host brings polyphony, envelopes and gain.

## Rules every change keeps

1. The modes play what Folium's oscillator plays, sample for sample before the declick, with Folium's per-voice variation turned off. `sh tests/check.sh` holds it on the reference build in `MODES.md`; its exit code is the verdict there. On any other build the match rows can fail and every other check must still pass: do not change code to chase those rows. The difference is not always small: Analog's cross FM drifts in timing between builds, to 24 dB under the peak within two seconds on a full patch (`MODES.md`). A change that moves a sample is a change in Folium first, then here, never here alone.
2. No dependencies. The includes each header may have are listed in `CONTRIBUTING.md`.
3. No allocation, lock or system call after construction. Fixed arrays, named constants.
4. The output never depends on the host's block size. Control-rate work runs every `kControlBlock` (64) samples counted from `reset()` or `noteOn()`.
5. Any rate from 8 kHz to 192 kHz; the phasors read the frequency every sample.
6. A non-finite value into a setter is ignored and the last good one kept.
7. A parameter range is never invented. It comes from the maths or from a shipping instrument, and the derivation is written down.
8. No code transcribed from another maker's product. A technique is written from the principle and credited in `CREDITS.md`.
9. Comments say why. The small ASCII flowers in the file headers are the project's signature: keep them, do not add more.
10. GCC fuses multiplies and adds across statements by default (`-ffp-contract=fast`). Build with `-ffp-contract=off`: with clang that flag leaves the sine's samples identical to the reference build's. Measured with GCC 15.2 on arm64: 72 of 72 match rows fail at `-O2` and 52 with `-ffp-contract=off`, and every other check passes (`MODES.md`). The repo's CMake does not set the flag, because it would change how your own code compiles. With `-ffast-math`, add `-fno-finite-math-only`: without it the window rows fail (`MODES.md`).

## Carrying it into another project

Copy `include/petal/`: `core.hpp`, `declick.hpp` and the mode you want, plus `scl.hpp` if the project reads tuning files. Build a `petal::Tuning` off the audio thread, then `setSampleRate`, `setTuning`, `noteOn(hz)` and `process` on the audio thread. `prompts/` has a complete first message for JUCE, VCV Rack, madronalib and Daisy.

## What is not here

- No envelope, VCA or voice allocation. `examples/juce/` sketches one way to add them.
- No Rack module and no firmware. Each mode needs a sample rate and one call per sample.
