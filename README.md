# Faust-expr

A standalone public laboratory for Faust instruments, starting with research into a physically modelled, playable cymbal.

**Current scope: build infrastructure only. `dsp/probe.dsp` is a compiler smoke test, not a cymbal model.** Check the actual Actions run and its artifacts for build evidence; source presence alone does not prove a successful build.

## Intended research

One connected vibrating object with a continuous size control; spatial strikes from bell through bow to edge; contact-based beaters; controllable thickness, bell geometry and hammering approximations; conventional and unconventional materials. Distinguish physical modelling from deliberate impossible-object behaviour. Do not label unvalidated approximations as real-world accuracy.

## Build and evidence

The GitHub workflow uses a standard Ubuntu runner with a ten-minute timeout. On pushes, pull requests and manual requests, it installs Faust, compiles the actual DSP into C++, renders one second of controlled audio, and validates pitch, amplitude, initial silence, release tail and finite sample values.

The commit-labelled workflow artifact contains `probe.wav`, `waveform.png`, `results.json`, compiler versions, raw float audio, and the build log. Outputs expire after three days. The waveform is an analysis image, not an application screenshot. Failed validation fails the job; it is not hidden by normalisation.

To run locally, install Faust with its C++ headers, g++, Python 3, NumPy and Matplotlib, then run:

```sh
python3 -m unittest discover -s tests -v
bash scripts/build.sh
```

The Python tests use explicitly synthetic fixtures to validate the analyser. They do not stand in for a Faust compile or physical-model validation.

## Separation and security

This repository contains only standalone laboratory files. It does not fetch, build, or publish any private host repository, its history, dependencies, settings, recordings or credentials. CI needs no API key and calls no AI model. Its GitHub token is read-only; it runs no deployment, release or automatic merge. Do not add a private checkout or private credentials to this public workflow.

No open-source licence has been selected; choosing one is a separate project decision.

## References

- Faust: https://github.com/grame-cncm/faust
- Compiler options: https://faustdoc.grame.fr/manual/options/
- GitHub Actions billing: https://docs.github.com/en/billing/concepts/product-billing/github-actions

Standard hosted execution for public repositories is free under GitHub's published policy. This does not promise unlimited storage or paid larger runners.
