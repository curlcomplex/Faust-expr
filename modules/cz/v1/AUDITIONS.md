# CZ v1 audition cases

Render the actual Faust source at 48 kHz with gate 1.2 s / total 1.8 s, velocity 1, A/D/S/R 0.006/0.35/0.72/0.5.

1. `square-dcw-sweep`: 220 Hz, waveMorph 0, DCW 0.05 -> 0.35 -> 0.65 -> 0.9.
2. `sinepulse-dcw-sweep`: 220 Hz, waveMorph 1, same DCW points.
3. `musical-low`: 110 Hz, DCW 0.72, waveMorph 0, short two-note phrase.
4. `musical-glass`: 440 Hz, DCW 0.82, waveMorph 1, short articulated phrase.

These are source/audition examples, not hardware-validation cases. Hardware/reference comparisons remain governed by issue #98.
