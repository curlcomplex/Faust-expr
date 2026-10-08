# Faust-expr

Standalone public Faust experiments, beginning with **Spatial Cymbal v0.1**.
This is a working research prototype, not a validated simulation of every detail
of cymbal manufacture and contact. Read [the model and limitations](docs/CYMBAL.md).

## Cymbal

One spatially struck, still-ringing body with continuous diameter, thickness,
taper, bell shape, hammering approximation, beater properties and material morph.
The core uses 128 modes from a tapered-plate reference with approximate shell
stiffening and passive nonlinear modal coupling. No recorded cymbal samples.

```bash
# Ubuntu dependencies
sudo apt-get install faust g++ python3-numpy python3-scipy python3-matplotlib
bash scripts/build_cymbal.sh
```

Outputs under `evidence/cymbal/` include `audition.wav`, individual 48 kHz stereo
WAVs, `audition-timeline.json`, `results.json`, `precision.json`, compiler/version
provenance and the self-contained **spatial-cymbal.dsp**.

Open that generated DSP in a Faust host. Press/release gate to strike; leave the
same instance alive between hits. Try strike_radius 0.16, 0.55 and 0.92; material
0/1/2/3 means bronze/steel/glass/wood. The maintained source is `dsp/cymbal.dsp`
plus `scripts/generate_cymbal.py`. Begin with low monitoring volume.

CI runs the actual compiler and native renderer on a standard Linux runner and
publishes commit-specific artifacts. No Codex or other model calls are involved.
The original two-tone compiler check remains available via `bash scripts/build.sh`.

This repository does not contain or fetch private CURLOP code. CURLOP integration,
real interface evidence and device performance require separate tests.
