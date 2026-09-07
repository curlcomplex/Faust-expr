#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/single evidence/cymbal
python3 scripts/generate_cymbal.py --out build
faust --version > evidence/cymbal/faust-version.txt 2>&1
g++ --version > evidence/cymbal/cpp-version.txt
git rev-parse HEAD > evidence/cymbal/commit.txt
# No custom import remains in this generated, host-loadable source.
{ cat build/cymbal-kernel.lib; grep -v '^import("cymbal-kernel.lib");' dsp/cymbal.dsp; } > evidence/cymbal/spatial-cymbal.dsp
echo "Compiling double-precision reference render engine"
faust -t 120 -double -I build -lang cpp -cn CymbalDSP dsp/cymbal.dsp -o build/cymbal.hpp
g++ -std=c++17 -O2 -Ibuild scripts/render_cymbal.cpp -o build/render_cymbal
echo "Compiling self-contained source in default single precision"
faust -t 120 -single -lang cpp -cn CymbalDSP evidence/cymbal/spatial-cymbal.dsp -o build/single/cymbal.hpp
g++ -std=c++17 -O2 -Ibuild/single scripts/render_cymbal.cpp -o build/single/render_cymbal
cp build/cymbal.hpp build/reference-modes.json scripts/render_cymbal.cpp evidence/cymbal/
python3 scripts/check_cymbal_precision.py --single build/single/render_cymbal --double build/render_cymbal --out evidence/cymbal/precision.json
echo "Rendering numerical and listening suite"
python3 scripts/cymbal_suite.py --renderer build/render_cymbal --out evidence/cymbal
