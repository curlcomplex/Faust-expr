# Compiler and numerical diagnostics (#109)

This layer complements audio qualification. It does **not** change instrument DSP, sonic acceptance, reference scores or performance benchmarking.

## Compiler pass

`tools/modules/compiler_diagnostics.py` compiles with pinned Faust 2.88.0 using `-wall -me`. `-wall` warnings are retained as informational evidence by default. A compiler failure is hard. `-me` enables Faust's math-domain checks; it is a compiler diagnostic and is not described as runtime tracing.

Warnings must only become release blockers after a specific correctness impact is demonstrated and documented. This avoids turning every compiler advisory into a sonic gate.

## Runtime interpreter tracing

The optional `--interp-tracer` pass runs upstream `interp-tracer -trace 4 -noui`. Faust 2.88 documents trace mode 4 as collecting subnormal/infinite/NaN, integer overflow, divide-by-zero, cast overflow, negative bitshift and load/store faults, failing on the fatal FP/cast/load-store classes. The qualification fixtures include deliberate divide-by-zero and non-finite cases so the diagnostic path itself is tested.

The tracer uses Faust's interpreter backend. Its output is diagnostic evidence, not evidence about generated C++ timing or the sound quality of an instrument. The normal native render/qualification path remains authoritative for audio output.

## Structural output

`--structural` first checks whether the pinned compiler advertises `-sig`; when supported it invokes it only with the `ocpp` backend and labels the result `compiler structure only`. Structural output must never be presented as measured runtime performance. Existing scheduler/vector/JIT research remains separate.

## Evidence and policy

Each report records exact commands, return codes, stdout/stderr, Faust version, input hash, execution lane and the classification policy. Negative fixtures invert the expected outcome only inside tests; production DSP compilation/tracer failures remain visible.

Usage:

```sh
python3 tools/modules/compiler_diagnostics.py module.dsp \
  --faust /path/to/faust-2.88.0/build/bin/faust \
  --faust-libraries /path/to/faust-2.88.0/libraries \
  --interp-tracer /path/to/interp-tracer \
  --structural --out build/diagnostics/module
```

The GitHub-hosted qualification builds the tracer from the same verified Faust 2.88.0 release as the compiler. The owner's-machine queue remains independent and is not required for this compiler/interpreter diagnostic layer.
