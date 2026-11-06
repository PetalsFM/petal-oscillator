#!/bin/sh
#          .--.
#     .--.(    ).--.
#    (    )'--'(    )
#     '--'.--.'--'
#        (    )
#         '--'
#
# check.sh - build every header alone, then run tests/check.cpp.
#   sh tests/check.sh           the checks; the exit code is the verdict
#   sh tests/check.sh --arms    also break the real headers on purpose (in a copy)
#                               and require each check to go red
# CXX and CXXFLAGS are read from the environment (default: c++, -O2).
#
# Copyright (c) 2026 Petals. MIT License, see LICENSE.
set -eu
cd "$(dirname "$0")/.."
CXX="${CXX:-c++}"
FLAGS="-std=c++17 ${CXXFLAGS:--O2} -Wall -Wextra -Werror"
OUT="${TMPDIR:-/tmp}/petal-check.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

# Every header but mts_esp.hpp (it needs the MTS-ESP client) builds on its
# own, from the include directory it is given.
alone() {   # include-dir
    for h in core declick oscillator analog scl; do
        printf '#include "petal/%s.hpp"\nint main() { return 0; }\n' "$h" > "$OUT/alone_$h.cpp"
        $CXX $FLAGS -I"$1" -fsyntax-only "$OUT/alone_$h.cpp" 2>/dev/null || { printf '%-6s %-28s FAIL\n' alone "$h.hpp"; return 1; }
        printf '%-6s %-28s PASS\n' alone "$h.hpp"
    done
}
# tests/check.cpp against the headers in the include directory it is given.
run() {   # include-dir tag
    $CXX $FLAGS -I"$1" tests/check.cpp -o "$OUT/check_$2" || return 2
    "$OUT/check_$2" tunings tests/references.txt
}

alone include
run include ship

[ "${1:-}" = "--arms" ] || exit 0

# Each arm breaks one real line, in a copy, and must go red the way a real
# fault does: exit 1 (a crash or a build that fails is not red), at least one
# FAIL, and every FAIL in the rows that arm is aimed at.
arm() {   # name file from to aimed-rows
    rm -rf "$OUT/arm" && mkdir -p "$OUT/arm" && cp -R include "$OUT/arm/"
    FROM="$3" TO="$4" perl -0pi -e '$n = s/\Q$ENV{FROM}\E/$ENV{TO}/g; die "arm did not land\n" unless $n == 1' "$OUT/arm/include/petal/$2"
    set +e
    ( alone "$OUT/arm/include" && run "$OUT/arm/include" arm ) > "$OUT/arm.log" 2>&1
    rc=$?
    set -e
    red=$(grep -cE ' FAIL( |$)' "$OUT/arm.log" || true)
    stray=$(grep -E ' FAIL( |$)' "$OUT/arm.log" | grep -cvE "$5" || true)
    if [ "$rc" -ne 1 ] || [ "$red" -eq 0 ] || [ "$stray" -ne 0 ]; then
        echo "arm    $1: NOT RED OR NOT AIMED (exit $rc, $red failed, $stray outside its rows)"; exit 1
    fi
    echo "arm    $1: red, $red checks failed, all in its rows"
}
arm sine    core.hpp    's3 = s3 * 4.0f;' 's3 = s3 * 4.00008f;' '^match  [0-9]+ c_'
arm blep    analog.hpp  'c.held += jump * 0.5f * e * e + slope' 'c.held += jump * 0.50001f * e * e + slope' '^match  [0-9]+ a_'
arm window  declick.hpp 'const float scale = sr_ / 48000.0f;' 'const float scale = 1.0f;' '^declick'
arm alone   declick.hpp '#include "core.hpp"' '#include <cstdint>' '^alone  declick\.hpp +FAIL'
arm blocks  analog.hpp  'void process(float* out, int n) { for (int i = 0; i < n; ++i) out[i] = process(); }' 'void process(float* out, int n) { pos_ = 0; for (int i = 0; i < n; ++i) out[i] = process(); }' '^blocks a_'
arm rates   analog.hpp  'sr_ = sr > 0.0f ? sr : kDefaultSampleRate;' 'sr_ = sr > 0.0f ? std::min(sr, 96000.0f) : kDefaultSampleRate;' '^rates  analog 192000'
arm input   analog.hpp  'void setDetune(float cents) { if (std::isfinite(cents)) detune_ = cents; }' 'void setDetune(float cents) { detune_ = cents; }' '^input '
arm cwin    oscillator.hpp '        declick_.setSampleRate(sr_);' '' '^window complex'
arm awin    analog.hpp  '        declick_.setSampleRate(sr_);' '' '^window analog'
arm cduck   oscillator.hpp 'declick_.noteOn(busy_);' 'declick_.noteOn(false);' '^window complex'
arm aduck   analog.hpp  'declick_.noteOn(busy_);' 'declick_.noteOn(false);' '^window analog'
arm spaced  scl.hpp     "if (!hasSlash && after < te && *after == '/') return false;" '' '^scl '
echo "arms: every arm red, each in its own rows"
