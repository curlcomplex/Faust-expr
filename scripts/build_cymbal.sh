#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build/cymbal evidence/cymbal
export OPENBLAS_NUM_THREADS=1
# Keep the public compiler available for offline diagnosis of generated code.
# Explicit file allowlist: no environment variables, credentials or private data.
if [[ "${GITHUB_ACTIONS:-}" == true ]]; then
  mkdir -p build/faust-sdk/include build/faust-sdk/share build/faust-sdk/lib
  cp /usr/bin/faust build/faust-sdk/
  cp -r /usr/include/faust build/faust-sdk/include/
  cp -r /usr/share/faust build/faust-sdk/share/
  cp -r /usr/share/doc/faust build/faust-sdk/
  ldd /usr/bin/faust > evidence/cymbal/faust-ldd.txt
  ldd /usr/bin/faust | awk '/=> \/[^ ]+/ {print $3}' | while IFS= read -r lib; do cp "$lib" build/faust-sdk/lib/; done
  tar -czf evidence/cymbal/faust-sdk.tar.gz -C build faust-sdk
fi
python3 scripts/generate_cymbal.py --out build/cymbal
faust --version > evidence/cymbal/faust-version.txt 2>&1
git rev-parse HEAD > evidence/cymbal/commit.txt
faust -time -t 180 -double -lang cpp -cn CymbalDSP build/cymbal/cymbal-standalone.dsp -o build/cymbal/cymbal.hpp
cp build/cymbal/cymbal.hpp evidence/cymbal/
g++ -std=c++17 -O2 -Ibuild/cymbal scripts/cymbal_render.cpp -o build/cymbal/cymbal-render
cp build/cymbal/cymbal-standalone.dsp build/cymbal/model.json evidence/cymbal/
python3 scripts/cymbal_experiments.py --renderer build/cymbal/cymbal-render --out evidence/cymbal
