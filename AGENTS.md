# Faust-expr development

This is a standalone public DSP laboratory. Never import private host code, credentials, configuration, recordings or Git history without explicit permission.

- Read the source before editing; preserve unrelated user changes.
- After bootstrap, use a focused branch and PR for instrument work. Do not force-push or merge without permission.
- Run `python3 -m unittest discover -s tests -v` and `bash scripts/build.sh` for DSP/build changes, or inspect their exact-commit CI results when local Faust is unavailable.
- Report executed tests separately from code inspection. A synthetic analyser fixture is not a Faust render; a waveform plot is not a GUI screenshot.
- Tie audio and measurements to the tested commit. Keep failures visible and do not normalise away level errors.
- The current probe only tests the toolchain. Do not present it as a cymbal or as proof of host integration.
- Keep CI bounded, read-only and free of model/API calls, private checkout steps and paid larger runners.
