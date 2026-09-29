# Running the research on a physical Mac

## What this enables

A GitHub Actions self-hosted runner executes the job on the registered Mac, not in a GitHub-hosted VM. GitHub remains the job/log/artifact interface. This supports real-hardware DSP scaling and later Core Audio callback/workgroup tests. Merely running an offline compute loop under Actions does not make it an audio-device or realtime test.

The current correctness script is `python3 scripts/run_scheduler_correctness.py`. It uses the existing probe, atomic queue candidate and libfaust bitcode APIs. It is not the Curlop production renderer. Its matching diagnostic toolchain is Faust 2.85.9 / LLVM and Clang 22.1.8; it deliberately fails on a different version. Do not silently replace Curlop's installed/bundled compiler to make the diagnostic run. Production performance acceptance must use the actual Curlop renderer and its matching toolchain, separately identified in the evidence.

## Billing clarification

At verification on 2026-09-07, GitHub's billing documentation says self-hosted runner usage is free, as is standard GitHub-hosted runner use in public repositories. Faust-expr is public, so its standard hosted macOS diagnostic jobs are not consuming a private-repository included-minute allowance. Larger hosted runners and storage have separate billing considerations. This is not an account-billing audit.

Source: https://docs.github.com/en/billing/concepts/product-billing/github-actions

## Do not register a personal Mac on this public repository

GitHub explicitly warns against public-repository self-hosted runners because untrusted pull requests can execute code on the machine. A label is a scheduling filter, not a security boundary.

Use a separate **private, owner-only benchmark-control repository**. Register the runner only to that private repository, use manually dispatched jobs, and check out only an explicitly reviewed 40-character research commit. Do not enable fork PR jobs, `pull_request_target`, or unattended execution of arbitrary branch heads. Run under a separate non-admin macOS account with no SSH keys, cloud tokens, signing keys, or private project credentials. A separate user account limits exposure but is not a complete sandbox against hostile executable code. Trusted code remains essential.

An inert workflow template is provided in `research/self-hosted-correctness.yml.example`. It is intentionally outside `.github/workflows/` and does not register or activate any runner. It refuses to run outside a private repository or for an actor other than `curlcomplex`, and uses no GitHub write permissions.

Sources:
- https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/add-runners
- https://docs.github.com/en/actions/reference/security/secure-use#hardening-for-self-hosted-runners

## One-time owner setup

1. In the private control repository, open **Settings > Actions > Runners > New self-hosted runner** and choose **macOS / ARM64**. Run GitHub's currently displayed download, checksum-verification and registration commands locally. The short-lived registration token belongs in the local command, not in an issue, committed file, or chat transcript.
2. Add the custom label `curlop-bench`. For the first tests run `./run.sh` interactively rather than installing an always-on service. Stop it after the test session. Keep the Mac awake and connected while the job runs.
3. Put the example workflow into the private repository's default branch as `.github/workflows/faust-correctness.yml`. Review the exact research commit before running it. The workflow is manual-only (`workflow_dispatch`), with one job at a time.
4. In Actions, run the workflow with that full SHA and participant counts supported by the machine, initially `1,2,3`. Larger counts can be entered after checking `sysctl -n hw.physicalcpu`. Counts include the caller; the stock scheduler may allocate more idle helpers than it activates.

No local runner installation, registration, token request, or job execution on the owner's machine has been performed by this research change.

## Performance acceptance after correctness

Use physical hardware first for offline capacity/scaling, then for actual Core Audio callbacks. For the latter, test the app in an appropriate logged-in user/audio-device environment, explicitly confirm worker realtime policy and workgroup membership, and use Curlop's current scalar fused renderer as baseline. Keep build identity, graph identity, parameter/control segmentation, sample rate, buffer size, power mode and concurrent workload fixed and recorded. Compare one and multiple participants; measure callback p50/p99/max, missed deadlines and active/outgoing graph overlap, not just total process CPU. Avoid coding/model-inference jobs on the same Mac during baseline timing.

The current correctness driver allocates capture buffers, validates samples and writes diagnostic output. **Do not use its elapsed runtime as a performance benchmark.**

Apple primary guidance:
- https://developer.apple.com/documentation/audiotoolbox/workgroup-management
- https://developer.apple.com/videos/play/wwdc2020/10224/
