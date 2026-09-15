# Controlled harmonic / folded-component qualification (#108)

Owning issue: #108, under #104. Additive offline diagnostics. No instrument DSP,
existing FFT/reference score, 48-vs-96-kHz residual, benchmark or release threshold
is replaced. The comparison residual in `synth_batch.py` remains a combined
phase/filter/envelope/sample-rate diagnostic, not an isolated alias metric.

## Two distinct uses

`harmonic_analysis.py` analyzes a chosen stationary mono window from an existing
headerless float32 little-endian render. It never compiles the instrument. An
explicit fundamental and signal class are required; this is not a pitch detector
or an arbitrary-audio alias detector. Stereo must be deliberately extracted into
channels beforehand, not passed as a flattened mono signal.

```sh
python3 tools/modules/harmonic_analysis.py original.f32 --rate 48000 \
  --fundamental-hz 1999.51171875 --signal-class stationary-oscillator \
  --start 32768 --frames 32768 --out build/harmonics/example
```

`harmonic_qualification.py` compiles two dedicated actual-Faust **test fixtures**
through the existing native C++ renderer and sweeps known operating points. These
are test signals, not new instruments or instrument sound improvements:

```sh
python3 tools/modules/harmonic_qualification.py --out build/harmonic-sweep \
  --faust /path/to/faust-2.88.0/build/bin/faust \
  --faust-libraries /path/to/faust-2.88.0/libraries \
  --faust-archive /path/to/faust-2.88.0.tar.gz
```

The same complete 2.88.0 archive/hash and library-manifest verification as #105 is
required. Compiler commands, expanded/generated sources, compiler/library/binary
hashes, controls, score hashes, original raw files and measurement hashes remain
inspectable. Test fixture internals use double precision with float32 I/O; this
does not upgrade the instrument-generation baseline. Control zones are float32.
The declared coherent frequency and its independently checked output are recorded.
At 44.1 kHz float control rounding may create a small numerical leakage floor;
that is not called exact cross-platform reproducibility.

## Measurement contract

The analysis interval is half-open `[start, start+frames)` with an integer number
of fundamental cycles. Use 1024..262144 power-of-two frames, at least 32 cycles,
and keep f0 at least 32 bins below Nyquist. No padding or implicit resampling is
performed. These conservative limits are applicability constraints, not musical
quality criteria. Arbitrary noncoherent material is rejected rather than quietly
changing the window and reporting a different noise floor.

The one-sided rectangular DFT bin powers sum to the original mean square. DC is
reported separately. The known fundamental is used even when another harmonic
is louder. Fundamental presence/concentration, quarter-window RMS, complex
fundamental stability and spectral-shape stability reject missing/wrong f0,
leakage and moving/decaying content. Quarter-window Hann transforms are **guards**,
not the measurement window. They are conservative diagnostics, not a proof that
all possible modulation or wrong user-supplied classifications can be detected.

- `inband_harmonic_ratio` is square root of in-band harmonic power above f0 divided
  by fundamental power, including all integer multiples strictly below Nyquist.
  `inband_thd_ratio` is the same number only for declared sine-driven nonlinearities;
  it is null for oscillators because their harmonics can be intentional.
- `off_harmonic_to_fundamental_db` sums all other non-DC bins, including Nyquist.
  This contains spurs/noise/modulation/leakage **as well as possible aliases**.
  It is not a measurement of isolated alias energy.
- `sfdr_including_harmonics_db` is carrier-to-largest non-DC/non-carrier bin, including
  intended harmonics. `off_harmonic_sfdr_db` excludes the harmonic grid. Both names
  are explicit because tools use different SFDR conventions.
- `--max-generated-order H` enables a **finite, externally justified harmonic model**.
  For each order above/equal Nyquist, its bin folds by `min(h*k mod N, N-h*k mod N)`.
  Predicted folded locations and levels are reported. The identifiable fold ratio
  is a **power ratio**, not an amplitude ratio. Collisions with DC, Nyquist, in-band
  harmonics or other folded orders make that aggregate null. Do not deduce source
  identity from coincident spectral energy; unrelated spurs can occupy the same bin.
  A truncated H is not an exhaustive alias model for an infinite-harmonic waveform.
- dB values have a documented -150-dB ratio floor (thus +150-dB SFDR ceiling).
  This is a numerical display bound, not hardware dynamic range or audibility.

An optional `--lab-report` links an existing lab report only when the entire raw
hash and rate/frame/channel dimensions agree. It does not modify the parent report
or reuse an old acceptance decision as new qualification. Errors remove the current
success manifest; failed runs must not be inferred successful from stale output.

## Controlled domain and independently predictable answers

FFT length 32768, discarded startup 32768 frames, sample rates 44100/48000/96000.
Bin indices 137/1365/4093/6827 specify exact requested frequencies `rate*k/32768`.
All indices are odd to avoid fold collisions for these finite-order fixtures.
Each configuration starts a fresh native process/DSP, with constant controls.

The oscillator has three shapes: sine, eight-partial additive with all partials
at/above Nyquist removed, and the same eight partials without that removal. Partial
h has amplitude `0.2/h`. Expected harmonic/fold power ratios follow `sum(1/h^2)`
over their respective domains; no measured output is used to define the answer.

The sine-driven cubic stage is `x + drive*x^3`, input amplitude A=0.4, drives
0.25/1/4. Its fundamental amplitude is `A + 3*drive*A^3/4`; the third-harmonic
amplitude is `drive*A^3/4`. That third harmonic is either in-band or folded. In-band
THD alone therefore misses the high-register alias. The sweep contains **72 native
renders**, checked against these analytic power ratios with an absolute tolerance
of 2e-5. This tolerance belongs to test fixtures only, not to existing instruments.

Phase/delay changes and fixed harmonic-balance changes are separately tested:
they change whole-waveform residuals without creating off-harmonic energy. The
original 48-vs-96 diagnostic is retained without relabelling it.

## Optional faustprobe adapter and its validation boundary

Evaluated against the upstream user guide at faust-rs revision
`fbe0e282343cca2980642aa6dc81e2e9681bdc65` (15 September 2026). Upstream describes
Cranelift JIT, explicit controls, sample-rate/block/window settings and full-cadence
CSV. Its built-in `sfdr` excludes harmonics and uses Blackman-Harris; comparing that
scalar directly with inclusive coherent SFDR would be invalid.

The opt-in adapter instead captures CSV and applies **this same guarded reducer**.
It uses the same fixture source, verified library manifest, controls, excitation,
rate, block and startup/measurement boundaries. CSV channel count, exact frame
coverage and finite values are checked; both backend measurements remain visible.
Differences are investigation signals, not evidence that one backend is correct.

```sh
# Add to the complete native sweep command only after installing/reviewing faustprobe:
# --faustprobe /path/to/faustprobe --faustprobe-revision <full-40-character-source-SHA>
```

The binary hash and caller-declared source revision are recorded separately; a
caller-provided revision is not an attested binary/source linkage. The adapter
checks 48-kHz clean/folded oscillator and cubic examples. It is never required by
normal CI, does not fetch/build Rust dependencies, and does not silently turn an
unavailable backend into a successful comparison. **The default qualification does
not execute Cranelift.** CSV/argument transport tests do not establish backend parity;
full compatibility remains conditional on an actual separately recorded opt-in run.

## Verification and execution

```sh
HARMONIC_INTEGRATION=1 FAUST=/path/to/faust \
FAUST_LIBRARIES=/path/to/libraries FAUST_ARCHIVE=/path/to/faust-2.88.0.tar.gz \
HARMONIC_EVIDENCE=build/harmonics \
python3 -m unittest discover -s tests -p test_harmonic_analysis.py -v
```

The analytical/input tests run without Faust. The native group is opt-in locally
and required by the pinned hosted workflow. Skipped integration tests are not passes.
The hosted workflow retains #105/#106/#107 coverage and fail-closed log pipelines.
No owner's-machine availability is inferred from hosted status. Any supplementary
sandbox run is identified separately; this adds no local runner or machine lock.

## Primary references

- Coherent DFT/window assumptions: https://www.analog.com/en/resources/technical-articles/coherent-sampling-vs-window-sampling.html
- Faust 2.88.0: https://github.com/grame-cncm/faust/releases/tag/2.88.0
- faustprobe execution/CSV/SFDR definitions: https://github.com/grame-cncm/faust-rs/blob/fbe0e282343cca2980642aa6dc81e2e9681bdc65/docs/faustprobe-user-guide-en.md

The harmonic/fold and cubic formulas above are explicit mathematical definitions,
not claims of reference-hardware authenticity. Source code and executed tests, not
a status badge alone, own the evidence.
