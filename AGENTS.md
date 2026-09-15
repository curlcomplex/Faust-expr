# Faust-expr development

This is a standalone public DSP laboratory. Never import private host code, credentials, configuration, recordings or Git history without explicit permission. Check actual repository visibility before making publication assumptions; the module-planning change does not alter it.

- Read the source before editing; preserve unrelated user changes.
- For reusable module work, read [modules/README.md](modules/README.md), [modules/DECISIONS.md](modules/DECISIONS.md), the owning issue, and the relevant contract/reference protocol. Issues own status; documents own contracts; identified builds/renders own evidence. Do not restart product discovery or convert reference hypotheses into established algorithms.
- After bootstrap, use a focused branch and PR for instrument work. Do not force-push or merge without permission.
- Run `python3 -m unittest discover -s tests -v` and `bash scripts/build.sh` for DSP/build changes, or inspect their exact-commit CI results when local Faust is unavailable. Add module-specific commands only with actual implementations.
- Report executed tests separately from code inspection. A synthetic analyser fixture is not a Faust render; a waveform plot is not a GUI screenshot.
- Tie audio and measurements to the tested commit. Keep failures visible and do not normalise away level errors. Felix's sonic approval and target-device acceptance remain distinct from automated checks.
- The current probe only tests the toolchain. Do not present it as a cymbal, a new module or proof of host integration.

## Independent execution queues

Treat GitHub-hosted execution and owner-authorized local-machine execution as **two independent queues**. A job waiting/running in one queue is not a reason to stop work that is eligible for the other queue.

- **GitHub-hosted queue:** deterministic CI-friendly compilation, unit/integration fixtures, static/compiler diagnostics, reproducible offline analysis and other work that does not require the owner's hardware. Keep it bounded, read-only with respect to external systems, and free of model/API calls, private checkout steps and paid larger runners.
- **Local-machine queue:** work that materially requires or benefits from the owner's environment or physical hardware, such as platform-specific builds, representative performance/latency benchmarks, target-device checks, real-time/audio-device work, or explicitly authorized heavier local experiments. Follow [modules/EXECUTION.md](modules/EXECUTION.md); do not infer authorization for arbitrary local execution from this policy.
- Before waiting on a long compile/test, check whether another ready issue/task can safely advance in the other queue. Parallelism must not violate issue dependencies, mutate the same branch/worktree unsafely, or overlap benchmark loads on shared physical hardware.
- Report queue placement explicitly when handing work to another agent. Distinguish `blocked by dependency` from merely `waiting for GitHub` or `waiting for local execution`.
- Results from either queue must remain tied to the exact tested commit and environment. Do not compare measurements from different machines as if they were one benchmark population.

Existing private-controller workflows are separately authorized; this public repository must not start untrusted jobs on a personal machine.
- Do not change repository visibility, credentials, runner registration or automatic releases without explicit owner approval. See [modules/EXECUTION.md](modules/EXECUTION.md).
