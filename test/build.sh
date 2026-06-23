#!/usr/bin/env bash
# build.sh -- macOS-Host-Build fuer den mdaEPiano-Test
# Kompiliert die *unveraenderte* Pico-Engine (src/mdaEPiano.cpp) zusammen mit
# test/test.cpp gegen CoreAudio + PortMidi. Die Pico-Audio-Subsystem wird ueber
# -DMDA_HOST_BUILD ausgeblendet (siehe Guard in include/mdaEPiano.h).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/test/mdaepiano_test"

CXX="${CXX:-c++}"
CXXFLAGS=(-std=c++17 -O2 -Wall -DMDA_HOST_BUILD
          -I"$ROOT/include"
         
          -I/opt/homebrew/include)
LDFLAGS=(-L/opt/homebrew/lib -lportmidi
         -framework CoreAudio -framework AudioToolbox
         -framework CoreFoundation)

echo "[build] $CXX ${CXXFLAGS[*]} ..."
"$CXX" "${CXXFLAGS[@]}" \
    "$ROOT/test/test.cpp" "$ROOT/src/mdaEPiano.cpp" \
    -o "$OUT" "${LDFLAGS[@]}"

echo "[ok]   $OUT"
