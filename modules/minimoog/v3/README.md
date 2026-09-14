# Mini v3 — recovery candidate, not a hardware-calibrated release

This separate sound version addresses the sustained-note collapse reproduced in
Mini v2. Both v1 and v2, their presets and all existing shared libraries remain
unchanged. No consumer or factory default selects v3 automatically.

## Confirmed old failure

At commit `33569587a937c5379af754db11e47fd204215223`, hosted actual-Faust run
34871411518 reproduced an effectively silent held note at 16 kHz cutoff at
44.1/48 kHz and at 96 kHz with full contour. Original v1 remained audible under
the same score. The old coefficient crosses zero at sample_rate/pi. A finite
output/peak check missed the collapse because the failed output stayed finite.
`tools/modules/mini_v2_reproduction.py` deliberately retains that negative result.

## Changed numerical scheme

The new local `ladder.lib` advances explicit stage voltage/derivative state in
four fixed substeps per output sample, with coefficients evaluated using the
internal integration rate. It retains the cubic saturator, thermal-voltage scale
and 2*r feedback hypothesis of the earlier candidate, and preserves all public
voice controls/defaults and the full cutoff range. Its coefficient argument is
at most .45*pi/4, so the former sign-changing region is outside its domain.

This is a separately authored Faust adaptation of the referenced procedural
D'Angelo integration, not a byte-identical edit of Hera's former Faust state
recurrence. It can change the sound below the old failure boundary as well.
The independent C++ cross-check tests this implemented numerical scheme; it does
not establish that the entire synth matches a physical Model D.

Input and controls are held across the four substeps. There is no interpolation
or anti-alias decimation filter, so this is not advertised as bandlimited
oversampling or alias-free processing. CPU cost and target-device behavior need
measurement before selection; a hosted offline test is not an iPhone callback
qualification. Exact oscillator/mixer/filter/envelope hardware tuning remains
open, and the historical 2*r feedback mapping is not newly hardware-certified.

## Tests and listening

Run `tools/modules/synth_recovery.py` and
`tools/modules/synth_recovery_verification.py`, each with `--out DIRECTORY`, in
the prepared environment documented by `.github/workflows/synth-recovery.yml`.
The verification command also needs the pinned PR83 baseline Git object.

At the first implementation checkpoint `6524c5da598018ce00b093dab335eb45cc1b3dd6`,
the recovery suite passed 365 checks / 122 actual Faust renders covering Mini v3
and the separately identified Juno-60 PR89 candidate. Further exact-commit results
and listening artifact links belong to draft PR90; do not infer later passes
from this historical count.

Comparison WAVs use the same phrase and each version's documented defaults:
Mini v1, then v2, then v3, eight seconds each, with no gain normalization or FX.
The old defect demonstration and high-cutoff sweep audio are separate files.
Retain the one-note gate/freq-Hz/velocity contract, host-owned polyphony and
legacy sound identities. Listening approval, hardware fidelity and target-device
acceptance are separate outstanding gates.
