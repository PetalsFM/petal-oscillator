# Prompt: the Petal Oscillator in a madronalib console app

Paste everything below this line as your first message, with `include/petal/` attached or in the project.

---

I am building a small madronalib program around the Petal Oscillator, with no UI. It is a header-only C++17 oscillator in two modes, Complex (phase modulation, `petal/oscillator.hpp`) and Analog (two band-limited oscillators with hard sync, `petal/analog.hpp`), and both lock their ratios to the intervals of a tuning. I want it to play a short phrase live through my audio device, and to render the same phrase to a WAV file without opening any device.

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

The madronalib parts, as they are on current main (older releases called the block `DSPVector` and its size `kFloatsPerDSPVector`; main has neither):

```cpp
#include "madronalib.h"
ml::kFramesPerBlock                                   // 64; ml::SignalBlock holds one: y[i], y.data(), y * 0.2f
ml::AudioContext ctx(nInputs, nOutputs);              // ctx.setSampleRate(r), ctx.getSampleRate(), ctx.outputs[c] = block
ctx.process(const float** in, float** out, int frames, std::function<void(ml::AudioContext*)> fn);   // frames <= 4096
ml::AudioTask task(&ctx, fn /*void(*)(ml::AudioContext*, State*)*/, &state);   // opens the default device here
task.runConsoleApp();                                 // starts the device, runs until Ctrl+C
```

What the program does:

1. A plain `State` holds four voices (a `petal::Oscillator` and my own short attack and release gain each), the `petal::Tuning`, the phrase and a sample counter. Nothing allocates, locks or prints once audio runs.
2. `void processPetal(ml::AudioContext* ctx, State* s)` is a plain function, since `AudioTask` takes a function pointer. Each call makes one block: every voice runs `osc.process(block.data(), (int)ml::kFramesPerBlock)`, gets its gain, and the sum times 0.2 goes to `ctx->outputs[0]` and `[1]` (Complex can peak a little above 1: 1.09 measured on the skewed wave at shape 2/3, MIDI note 36). Then the counter moves on by `kFramesPerBlock`.
3. The phrase is a fixed list of start, MIDI note and length, starting on block boundaries. The frequency is `tuning.noteToFrequency(note)`, which is 12-TET at A4 = 440 Hz when no scale is loaded. With a scale loaded, note 60 is its root and each note up is one step of the scale, so I write the phrase in steps of the scale I load. A new note takes the quietest voice and calls `osc.noteOn(hz, voiceStillSounding)`.
4. Live, the default: `ml::AudioContext ctx(0, 2); ml::AudioTask task(&ctx, processPetal, &state);`. The sample rate is only known once the task exists, so call `setSampleRate(ctx.getSampleRate())` on every voice after that line, then `task.runConsoleApp()`. The phrase loops until Ctrl+C.
5. Offline, with `--render out.wav`: never construct an `AudioTask`, it opens the device. Make the same context, `ctx.setSampleRate(48000)`, prepare the voices at 48000, and loop `ctx.process(nullptr, outs, 512, [&](ml::AudioContext* c) { processPetal(c, &state); })` until the phrase and half a second of tail are done. Write a 32-bit float stereo WAV with a few lines of stdio (madronalib has none) and print the peak, the RMS and the count of non-finite samples.
6. Flags, set once before any audio: `--ratio` 0.5 to 16 (default 1.0), `--fm` 0 to 0.2 (default 0.04), `--feedback` -1 to 1, `--shape` 0 to 1 (those default 0), `--wave sine|skewed|carrier`, `--lock` (it snaps only once a scale is loaded; with no scale the garden is empty and the ratio stays free). Do not change these ranges. `--scl FILE` loads with `petal::loadScl`, then `setTuning(&state.tuning)` on every voice.
7. `CMakeLists.txt`: `project(... C CXX)`, because madronalib compiles cJSON's C source. Set `BUILD_EXAMPLES` and `BUILD_TESTS` OFF with `set(... CACHE BOOL "" FORCE)`, `add_subdirectory(${MADRONALIB_DIR} madronalib)`, `add_subdirectory(${PETAL_DIR} petal)` for the petal repo, which sits outside the project, link `madronalib` and `petal_oscillator`, C++17. madronalib fetches cJSON at configure time and forces a universal macOS build; leave both alone.
8. Keep the headers in `petal/` unmodified. Anything host-specific lives in my code.

My project: [madronalib checkout path or commit, operating system, the phrase you want, whether you need more than four voices]

Please write `main.cpp` and `CMakeLists.txt`, and give me the commands to build it, play it live and render the WAV.
