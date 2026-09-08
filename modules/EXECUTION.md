# Local execution, private source and CI

Migration owner: [#30](https://github.com/curlcomplex/Faust-expr/issues/30). This document is a plan, not runner registration, a visibility change, credential setup or a dispatched benchmark.

## Policy verified on 8 September 2026

GitHub's billing documentation and self-hosted-runner overview state that self-hosted Actions execution is free. Its proposed self-hosted platform fee was postponed. This permits private source with local execution; public visibility is not a requirement for free self-hosted compute. Hosted jobs, artifact/cache storage and other metered services require separate allowance/budget management. Recheck policy when changing infrastructure; no future-price guarantee or account-billing audit is implied.

Primary sources are collected in [SOURCES.md](SOURCES.md), GH1–GH6.

## Keep source authority separate from runner placement

An existing trusted private repository can remain the execution controller and check out a reviewed Faust-expr commit. That is usually less disruptive than moving a working runner. Canonical module source/tests remain here even when a workflow elsewhere invokes them.

The default token belongs to the repository that invokes the workflow. It does not automatically gain read access to a second private repository. Before making Faust-expr private, arrange an explicitly limited read credential for cross-repository checkouts, or deliberately configure a runner scoped directly to private Faust-expr. Use owner-controlled secret storage, not issue bodies or chat. A workflow label is a scheduler filter, not a credential or security boundary.

Do not copy a private controller's source, logs, machine name, local paths or secrets into the currently public lab. Record sensitive operational evidence in the private controller. This documentation stores the generic design only.

## Safe local job design

Use a trusted workflow and an explicitly reviewed immutable commit. Manual dispatch is the initial default. No untrusted fork PR, pull_request_target or arbitrary branch-head execution on the personal Mac. Private visibility alone does not sandbox code contributed by collaborators or dependencies.

Pin action revisions, grant minimal read permissions, avoid persistent checkout credentials and scope authentication to the fetch where practicable. Validate every user input and pass it through environment/argument boundaries rather than direct shell interpolation. A SHA string being well formed does not make its code trusted; authorization/review must identify the revision too.

Use a dedicated non-admin account/workspace without personal signing keys, SSH/cloud credentials or unrelated private data. Do not clean an active developer worktree. A separate user is risk reduction, not a complete sandbox. Keep jobs bounded, cancellable and auditable; stop before modifying the machine's global toolchain merely to satisfy a diagnostic.

Benchmarks sharing physical hardware must not overlap other benchmark/build/inference loads. GitHub concurrency groups operate in a repository context; different repositories need an explicit shared scheduling convention or equivalent exclusion. Do not launch multiple runner services on one Mac and assume measurements remain comparable.

The runner application must be running, connected and able to execute jobs. A completed run establishes historical execution, not current online availability. Offline tests also do not demonstrate a logged-in audio-device/realtime context.

## Private transition checklist

Before an owner-approved visibility change:

1. Inventory existing workflows on main AND research branches, cross-repository checkouts, anonymous raw URLs/API downloads, submodules and dependency fetches. Authentication changes can affect more than one checkout action.
2. Preserve selected experiment artifacts in a recoverable location. Check which CI artifacts will expire.
3. Prepare private read access for the established controller without publishing credentials. Check the connector/agent retains access to the private repository.
4. Identify hosted jobs that should remain within included quotas, be disabled, or be deliberately replaced by local jobs. Changing visibility does not alter runs-on labels or make apt-based Ubuntu scripts run on macOS.
5. Review plan-dependent repository protections and visibility consequences. Existing public forks/copies cannot be made secret retroactively.

After explicit approval and the switch, validate a clean authenticated fetch and actual compile/render at a pinned source commit. Confirm the intended runner identity in private evidence, result access and no unplanned hosted compute. Do not call the cutover done from a YAML diff alone.

## Cost discipline

Local compute has no current GitHub per-minute self-hosted charge; electricity and machine maintenance remain local costs. Large audio uploads, caches, Git LFS or package storage are not made unlimited by self-hosting. Keep raw bulk audio locally/private with backups and hashes in Git. Upload small reports and selected auditions with short retention. A budget alert is not necessarily a spending stop; verify the actual configured control before claiming a zero-bill guarantee.

No new workflow, billing setting, runner or permission was installed by the shared-module planning change.
