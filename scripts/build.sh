#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
for tool in faust g++ python3; do
  command -v "$tool" >/dev/null || { echo "Missing dependency: $tool" >&2; exit 1; }
done
mkdir -p build evidence
if git rev-parse --verify HEAD >/dev/null 2>&1; then
  git rev-parse HEAD > evidence/commit.txt
else
  printf 'uncommitted-local-test\n' > evidence/commit.txt
fi
faust --version > evidence/faust-version.txt 2>&1
g++ --version > evidence/cpp-version.txt
# Headers come from the installed Faust package; no private host code is needed.
FAUST_INCLUDE="$(faust --includedir)"
faust -lang cpp -cn ProbeDSP dsp/probe.dsp -o build/probe.hpp
g++ -std=c++17 -O2 -I"${FAUST_INCLUDE}" -Ibuild scripts/render.cpp -o build/render
build/render evidence/probe.f32
python3 scripts/analyse.py evidence/probe.f32 evidence
