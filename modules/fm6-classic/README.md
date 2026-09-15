# FM6 Classic — DX7 architecture candidate

Owning issue: #96. Strategy: #111. Evidence pipeline: #112. Sound identity:
`fm6-classic`, sound version 1. This is a playable six-operator Faust instrument,
not another two-operator probe. Hardware/listening acceptance remains separate.

## Source and build

The canonical, reviewable source recipe is `tools/modules/fm6_prepare.py`.
It derives four local machine-specific engine files from **hash-verified Faust
2.88.0 files**, preserving original attribution and recording local changes.
It generates a runtime router with exactly six operators and all 32 algorithms,
a 150-control manifest, and the six original programs in `fm6_programs.py`.
No library installation is modified. No proprietary ROM or factory patches are used.

Generated `v1/`, `manifest.json` and `presets.json` are deliberately build products,
not hand-maintained duplicate sources. The qualified CI artifact includes these
ready-to-inspect/load source files, an expanded self-contained Faust source,
reference and candidate audio, patch exports, hashes and measurements.

```sh
python3 tools/modules/fm6_prepare.py \
  --libraries /path/to/verified/faust-2.88.0/libraries \
  --msfa /path/to/msfa-f67d41d \
  --license-file /path/to/Apache-2.0.txt
python3 tools/modules/fm6_delivery.py \
  --out build/fm6-evidence --faust /path/to/faust-2.88.0 \
  --libraries /path/to/verified/faust-2.88.0/libraries \
  --msfa /path/to/msfa-f67d41d
```

The dedicated workflow supplies these dependencies and runs generation before
unit tests. Legacy smoke jobs without prepared FM6 sources explicitly skip the
FM6-specific class; they are not the FM6 qualification.

## Instrument contract

One mono voice, `gate`, frequency in Hz and normalized `velocity`; polyphony is
host-owned. All operators expose ratio/fixed frequency, detune, output level,
four envelope rates/levels, velocity/rate/key scaling and amplitude sensitivity.
Global controls include algorithm, feedback, pitch envelope, LFO, sync/transpose,
plus smoothed brightness and output level. No new faceplate or host adapter.

Operator outputs preserve the upstream half-scale convention. Every internal
phase connection converts it once with a factor of two. Feedback has two samples
of history, reset on a fresh note. New voices are idle/silent until gate-on;
zero velocity is silent; gate-off does not cut release. Brightness and output
level remain active during release. Native DX envelope rates/levels are evaluated
at stage transitions; no claim that all patch edits instantly retarget an
in-flight envelope. Algorithm changes are live but may click: select between notes.

An overlapping new note needs host voice allocation. Repeated monophonic notes
require a computed low-gate sample before the next rising edge. Do not suspend
this voice only because an output block is quiet: envelopes and feedback persist.

## Listening and qualification

The first six original programs are Tine Keys, Rubber Bass, Glass Bells,
Digital Brass, Air Pad and Wood and Metal. Each comparison plays reference first,
then candidate, dry. One fixed reference-core multiplier of 0.1 matches the
candidate's known half-scale convention times output level 0.2. It is not fitted
per patch. Unmodified core floats are retained separately. No effects, peak
normalization, limiter, alignment or cropped attack/tail is hidden in the comparison.

The executable test covers 32 feedback-free routings, single-operator feedback,
carrier/pair/dynamic envelopes, fixed frequency, velocity/key/rate scaling,
pitch envelope/LFO diagnostics, cold/retrigger/release-tail behavior, scalar/vector
and 44.1/48/96 kHz candidate renders, plus actual musical phrases.

Faust and MSFA share lineage; software agreement is not independent hardware
validation. Original MSFA omits amplitude modulation and multi-operator feedback
in algorithms 4/6, so the adapter rejects those reference requests. Their
candidate implementation can be exercised, not falsely certified. Detailed
modern DX envelope, detune/LFO/AM and output-stage fidelity remain tracked in #96.
No hardware, Mac/iOS performance, CURLOP loading, factory promotion or release
approval is implied by successful Linux rendering.

Preserved baselines: #115 and #119. This version does not replace their source
or audio and must not silently replace any old project sound identity.
