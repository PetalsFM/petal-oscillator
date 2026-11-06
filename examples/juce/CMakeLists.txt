#          .--.
#     .--.(    ).--.
#    (    )'--'(    )
#     '--'.--.'--'
#        (    )
#         '--'
#
# The JUCE example instrument around the Petal Oscillator.
#
# Build the JUCE example against your own JUCE:
#   cmake -B build -DJUCE_DIR=/path/to/JUCE && cmake --build build
#
# Copyright (c) 2026 Petals. MIT License, see LICENSE.
cmake_minimum_required(VERSION 3.22)
project(PetalSynth VERSION 0.1.0)
if(NOT JUCE_DIR)
  message(FATAL_ERROR "Set -DJUCE_DIR=/path/to/your/JUCE checkout")
endif()
add_subdirectory(${JUCE_DIR} juce)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. petal)
juce_add_plugin(PetalSynth
  COMPANY_NAME "Example"
  IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE
  PLUGIN_MANUFACTURER_CODE Exmp PLUGIN_CODE Ptsy
  FORMATS VST3 Standalone
  PRODUCT_NAME "PetalSynth")
target_sources(PetalSynth PRIVATE PetalSynth.cpp)
target_compile_features(PetalSynth PRIVATE cxx_std_17)
target_compile_definitions(PetalSynth PUBLIC JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(PetalSynth PRIVATE petal_oscillator juce::juce_audio_utils
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
