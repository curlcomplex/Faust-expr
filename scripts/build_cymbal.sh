#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build/cymbal evidence/cymbal
export OPENBLAS_NUM_THREADS=1
python3 scripts/generate_cymbal.py --out build/cymbal
faust --version > evidence/cymbal/faust-version.txt 2>&1
git rev-parse HEAD > evidence/cymbal/commit.txt
faust -time -t 240 -double -lang cpp -cn CymbalDSP build/cymbal/cymbal-standalone.dsp -o build/cymbal/cymbal.hpp
cp build/cymbal/cymbal.hpp evidence/cymbal/
g++ -std=c++17 -O2 -Ibuild/cymbal scripts/cymbal_render.cpp -o build/cymbal/cymbal-render
cp build/cymbal/cymbal-standalone.dsp build/cymbal/model.json evidence/cymbal/
python3 scripts/cymbal_experiments.py --renderer build/cymbal/cymbal-render --out evidence/cymbal
