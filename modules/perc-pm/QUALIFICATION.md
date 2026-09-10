# Perc qualification and contrary evidence

The first complete delivery implementation is commit `4adec2378a53f2bdb337aae50eb7acc0590b5647`. Workflow 34455104491 passed; artifact 10143164352, ZIP SHA256 `031f886d4cad411c754c75a9342695ea0c16361df9a974d6853b51390866ae8b`. Final documentation-head reruns are recorded in PR #43 rather than manufacturing a self-referential commit hash in this file.

## Measured behavior

169 actual Faust renders / 248 checks, plus 14 separate arithmetic/contract unit methods. This replaces the original truncated coverage search while retaining its instrument regression cases. Actual scalar/vector generation; 44.1/48/96 kHz; blocks 1/32/64/127/128/256/512; clean pitch; initial silence; linear velocity and zero velocity; gate-off independence; all-control latching; exact same-onset versus one-sample-prepared locks; long-envelope oracle; endpoint stress; full-duration reference coverage; container validation of five WAV auditions.

The independent float64 closed-form oracle tests the generated amplitude graph across three rates, three decay settings and two attack settings. Worst relative error above -60 dB is **1.16755e-6**, below the predeclared 1e-4 limit. No repeated-multiplication envelope approximation is used.

**Correction to the initial handoff:** its source visited 128 alternating-pitch endpoint settings, not the claimed 512. The new separate persistent trajectory visits all 128 seven-dimensional corners at BOTH true pitch endpoints, totaling 256 settings. These are settings inside one recording, not 256 separately executed tests or exhaustive continuous-space coverage.

Worst tested musical peak **0.489426**, maximum adjacent-sample change **0.679289**. Envelope diagnostics are excluded from musical peaks. Fixed kernel gain; no hidden limiter, EQ, normalization or reverb. The jump figure is why numeric headroom is not a click-free guarantee.

## Reference measurement correction

The initial comparison clipped long reference recordings to a 0.9-second candidate before measurement. Its approximately 0.464 mean distance cannot describe a full-tail match and is superseded, not compared numerically to the new metric.

The new study uses a full 10-second horizon and 32 energy-weighted time/spectral descriptors. A fixed 54-setting authored pool is rendered both through the PM instrument and a controlled additive-only ablation (PM index and feedback disabled; ratios, envelope, impact and drive retained).

Mean nearest descriptor distance: PM **0.54556**, additive **0.61429**. Medians: **0.40673** and **0.46906**. PM is closer for all eight samples in this particular pool/metric, but these are coarse feature distances, not hardware-fit or perceptual percentages. The long PCC-02 comparator remains poorly represented (~1.416 nearest PM distance). The ratios/mappings are not inferred from unknown settings.

Both originally named reference splits were already inspected in the old search. No untouched-holdout claim survives that exposure or the revised metric. The additive graph is a useful explanatory diagnostic, not a new product variant selected by score. The existing PM engine remains the audition candidate.

## Rate sensitivity and optimization

48/96 kHz comparison uses matching onset times, no noise impact, and filtered polyphase downsampling. Relative RMS differences: clean **-57.97 dB**, Bell **-50.66 dB**, strongly driven upper-register example **-13.00 dB**. This combines feedback-delay, phase/filter and nonlinear aliasing effects; it does not isolate alias energy. High-register/extreme-drive quality is plainly unresolved; no sample-rate-independent sound or alias-free claim.

Four simultaneous voices, warmed and repeated in alternating scalar/vector order. First completed delivery hosted p50 medians in microseconds:

| Frames @48 kHz | Scalar | Vector | Median paired scalar/vector |
| --- | ---: | ---: | ---: |
| 32 | 20.947 | 24.391 | 0.859 |
| 64 | 41.853 | 48.688 | 0.860 |
| 128 | 83.876 | 97.432 | 0.861 |
| 512 | 512.135 | 438.440 | 1.169 |

Independent-container ratios were ~0.806/0.809/0.812/0.958, favoring scalar at every size. There is no universal vector win. Keep accurate scalar as the conservative short-block starting point, retain vector and measure the target. No sonic approximation or relaxed threshold was used to claim a speedup.

Object size is 256 bytes scalar / 432 vector, excluding buffers/stack/host. Stack-usage outputs and p50/p95/p99/max distributions accompany evidence. Guarded compute observed no ordinary new/new[] allocations; malloc/aligned allocation and whole-host behavior are outside that narrow test. These are offline Linux/container timings, not iPhone callback/thermal acceptance.

## Independent replay and packaging

The initial completed delivery artifact's source/generated/score/raw hashes were checked. Unchanged generated C++ was independently recompiled and all 169 recordings replayed: 31 byte-identical, worst absolute sample difference **4.61936e-7**, mean per-render RMS difference ~**4.688e-9**. This is stronger than merely executing the original CI binary, but is not a second Faust compiler or device test.

The compact package includes original hashes/scores, generated and expanded source, source snapshots, reference assets/attribution, reports and auditions. Bulk raw float audio is regenerated, not falsely described as included. Package replay is executed before delivery; its exact result and final artifact identity are appended to PR #43. Files are decoded and checked, not just linked by name.
