#!/usr/bin/env bash
# build_cp.sh -- macOS-Host-Build fuer die Reface-CP-Demo
# Kompiliert die Pico-Engine (src/mdaEPiano.cpp) + die header-only Reface-CP-
# Effektkette (effects/*.h) zusammen mit test/cp_test.cpp gegen CoreAudio +
# PortMidi. Die Pico-Audio-Subsystem wird ueber -DMDA_HOST_BUILD ausgeblendet.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/test/cp_test"

CXX="${CXX:-c++}"
CXXFLAGS=(-std=c++17 -O2 -Wall -DMDA_HOST_BUILD
          -I"$ROOT/include"
          -I"$ROOT/effects"
          -I/opt/homebrew/include)
LDFLAGS=(-L/opt/homebrew/lib -lportmidi
         -framework CoreAudio -framework AudioToolbox
         -framework CoreFoundation)

echo "[build] $CXX ${CXXFLAGS[*]} ..."
"$CXX" "${CXXFLAGS[@]}" \
    "$ROOT/test/cp_test.cpp" "$ROOT/src/mdaEPiano.cpp" \
    -o "$OUT" "${LDFLAGS[@]}"

echo "[ok]   $OUT"
