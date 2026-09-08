# Faust-expr development

This is a standalone public DSP laboratory. Never import private host code, credentials, configuration, recordings or Git history without explicit permission. Check actual repository visibility before making publication assumptions; the module-planning change does not alter it.

- Read the source before editing; preserve unrelated user changes.
- For reusable module work, read [modules/README.md](modules/README.md), [modules/DECISIONS.md](modules/DECISIONS.md), the owning issue, and the relevant contract/reference protocol. Issues own status; documents own contracts; identified builds/renders own evidence. Do not restart product discovery or convert reference hypotheses into established algorithms.
- After bootstrap, use a focused branch and PR for instrument work. Do not force-push or merge without permission.
- Run `python3 -m unittest discover -s tests -v` and `bash scripts/build.sh` for DSP/build changes, or inspect their exact-commit CI results when local Faust is unavailable. Add module-specific commands only with actual implementations.
- Report executed tests separately from code inspection. A synthetic analyser fixture is not a Faust render; a waveform plot is not a GUI screenshot.
- Tie audio and measurements to the tested commit. Keep failures visible and do not normalise away level errors. Felix's sonic approval and target-device acceptance remain distinct from automated checks.
- The current probe only tests the toolchain. Do not present it as a cymbal, a new module or proof of host integration.
- Keep CI bounded, read-only and free of model/API calls, private checkout steps and paid larger runners. Existing private-controller workflows are separately authorized; this public repository must not start untrusted jobs on a personal machine.
- Do not change repository visibility, credentials, runner registration or automatic releases without explicit owner approval. See [modules/EXECUTION.md](modules/EXECUTION.md).
