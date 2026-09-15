# Faust-expr development

This is a standalone public DSP laboratory. Never import private host code, credentials, configuration, recordings or Git history without explicit permission.

- Read the source before editing; preserve unrelated user changes.
- After bootstrap, use a focused branch and PR for instrument work. Do not force-push or merge without permission.
- Run `python3 -m unittest discover -s tests -v` and `bash scripts/build.sh` for DSP/build changes, or inspect their exact-commit CI results when local Faust is unavailable.
- Report executed tests separately from code inspection. A synthetic analyser fixture is not a Faust render; a waveform plot is not a GUI screenshot.
- Tie audio and measurements to the tested commit. Keep failures visible and do not normalise away level errors.
- The current probe only tests the toolchain. Do not present it as a cymbal or as proof of host integration.
- Keep CI bounded, read-only and free of model/API calls, private checkout steps and paid larger runners.

## Independent execution queues (owner direction, 15 September 2026)

GitHub-hosted machines and the owner's machine are **separate execution queues**, not a single global queue. Either may execute authorized work when appropriate. A busy/offline local machine does not block hosted CI; a queued hosted build does not establish that the local machine is busy, idle or unavailable.

- **GitHub-hosted:** default for deterministic compilation, unit/integration tests, offline renders/analysis and compiler diagnostics that do not need the owner's hardware. Inspect the actual workflow's `runs-on`, run ID, event and tested SHA. Do not equate a GitHub Actions job with a GitHub-hosted machine: Actions may also dispatch to self-hosted hardware.
- **Owner machine:** use only the established trusted execution/controller path for reviewed immutable commits, within the owner's authorized scope. Verify current availability and the actual local queue/lock before claiming the lane is free. Keep personal paths, hostnames, credentials and private controller evidence out of this public repository. Follow `modules/EXECUTION.md` on branches containing that protocol. Do not invent runner labels, install/register runners or expose the machine to public/fork PR execution.
- **Scheduling:** independent ready tasks may progress in parallel across the two lanes. Only real dependencies or a shared resource justify cross-lane blocking. Locks/concurrency groups must be scoped to the physical resource, not an umbrella issue. All work sharing the owner's physical hardware must respect its existing machine-wide queue; repository-local Actions concurrency alone does not serialize other repositories or non-Actions workloads.
- **Safety and evidence:** use isolated worktrees/build directories; do not race edits to the same branch. Hardware benchmarks need exclusive machine time; ordinary hosted CI is not a representative benchmark of the owner's Mac. Preserve machine/toolchain/sample-rate/block-size provenance and never pool unlike environments as one benchmark population. Moving a task between lanes must preserve revision, inputs, logs and applicability of the results.
- **Handoffs:** record owning issue, lane, immutable source SHA, actual workflow/job or local task ID, queued/running/completed/failed status, dependency/resource blockers and next action. Unknown queue state stays `unknown`; a suggested task is not a dispatched task. Analysis umbrella #104 and its children are not implicitly blocked by unrelated instrument work.
- **Failure handling:** when instructed to finish a task, continue the diagnose/fix/test cycle through ordinary build/test failures without asking for permission after each one. Inspect actual logs and end-to-end measurements, not just source-contract tests. Poll at sensible intervals and do useful independent work between checks rather than busy-polling. Do not claim unattended/background monitoring or completion without evidence.

This is an execution-routing policy, not permission to merge code, change repository visibility, credentials, runner registration, automatic releases or billing settings. It does not authorize launching unrelated work merely because another lane has capacity.
