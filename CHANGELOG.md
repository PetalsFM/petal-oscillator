# Changelog

Newest first. An entry that changes the sound says which rows of `tests/references.txt` moved.

## 1.0.0

- Complex (`include/petal/oscillator.hpp`) and Analog (`include/petal/analog.hpp`), sharing `core.hpp` and `declick.hpp`. Both play what Folium's oscillator plays, sample for sample before the declick on the reference build, with Folium's per-voice variation turned off.
- The `.scl` reader, the optional MTS-ESP adapter and five tunings.
- `tests/check.sh`, which holds the repo to 72 renders of Folium.
- The WAV renderer, a JUCE instrument and a madronalib sketch.
- `AGENTS.md`, `llms.txt` and `prompts/`, for building with an assistant.
