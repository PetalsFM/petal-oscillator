#          .--.
#     .--.(    ).--.
#    (    )'--'(    )
#     '--'.--.'--'
#        (    )
#         '--'
#
# The Petal Oscillator: a header-only library target, and the WAV example when built on its own.
#
# Copyright (c) 2026 Petals. MIT License, see LICENSE.
cmake_minimum_required(VERSION 3.16)
project(petal_oscillator VERSION 1.0.0 LANGUAGES CXX)

# The oscillator: headers only, C++17, no dependencies.
add_library(petal_oscillator INTERFACE)
target_include_directories(petal_oscillator INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(petal_oscillator INTERFACE cxx_std_17)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(render_wav examples/render_wav.cpp)
  target_link_libraries(render_wav PRIVATE petal_oscillator)
endif()
