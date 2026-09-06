#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build evidence/cymbal
python3 scripts/generate_cymbal.py --out build
faust --version > evidence/cymbal/faust-version.txt 2>&1
g++ --version > evidence/cymbal/cpp-version.txt
git rev-parse HEAD > evidence/cymbal/commit.txt
# Double internal states reduce accumulated round-off during energy-preserving morphs.
echo "Compiling Faust kernel"
faust -t 240 -time -double -I build -lang cpp -cn CymbalDSP dsp/cymbal.dsp -o build/cymbal.hpp
echo "Compiling native offline renderer"
g++ -std=c++17 -O2 -Ibuild scripts/render_cymbal.cpp -o build/render_cymbal
cp build/cymbal.hpp build/reference-modes.json scripts/render_cymbal.cpp evidence/cymbal/
# A self-contained file for the Faust IDE / host; no private or external custom imports.
{ cat build/cymbal-kernel.lib; grep -v '^import("cymbal-kernel.lib");' dsp/cymbal.dsp; } > evidence/cymbal/spatial-cymbal.dsp
echo "Rendering numerical and listening suite"
python3 scripts/cymbal_suite.py --renderer build/render_cymbal --out evidence/cymbal
