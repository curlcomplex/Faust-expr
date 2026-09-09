"""Complete sharp-family qualification, including vector cost and onset locks.

Extends the frozen first batch without duplicating its DSP or render harness.
--replay checks/recompiles previously generated C++, not a fresh Faust build.
"""
from __future__ import annotations

import argparse
import json
import shutil
import statistics
from pathlib import Path

import numpy as np

from sharp_batch import ROOT, Study, cmd, sha


def extra_checks(study: Study) -> None:
    fast = study.out / "optimized" / "render"
    lock = {"wave": 8, "sweep": .8, "sweep_time": .2, "decay": .75,
            "hold": .1, "click": .3, "velocity": .6}
    before = [(100, key, value) for key, value in lock.items()] + study.hit(101)
    onset = [(101, key, value) for key, value in lock.items()] + study.hit(101)
    x = study.render("onset-prepared", fast, events=before)
    y = study.render("onset-same-sample-lock", fast, events=onset)
    study.check("all-onset-locks-no-one-sample-lag", np.array_equal(x, y))
    x = study.render("drive-prepared", fast,
                     events=[(100, "drive", .9)] + study.hit(101))
    y = study.render("drive-at-onset", fast,
                     events=[(101, "drive", .9)] + study.hit(101))
    study.check("drive-onset-snap", np.array_equal(x, y))

    results = []
    for block in (32, 64, 128, 512):
        pairs = []
        labels = ["reference", "optimized", "vector"]
        for repeat in range(3):
            order = labels[repeat:] + labels[:repeat]
            measured = {label: json.loads(cmd([
                study.out / label / "benchmark", block])) for label in order}
            study.check(f"three-build-allocation-check:{block}:{repeat}",
                        all(row["ordinary_new_allocations_in_compute"] == 0
                            for row in measured.values()))
            pairs.append(measured)
        results.append({
            "block": block, "rotating_order_repetitions": pairs,
            "scalar_clenshaw_speedup": statistics.median([
                row["reference"]["p50_us"] / row["optimized"]["p50_us"]
                for row in pairs]),
            "vector_clenshaw_speedup": statistics.median([
                row["reference"]["p50_us"] / row["vector"]["p50_us"]
                for row in pairs]),
        })
    study.report["paired_three_build_benchmark"] = results
    study.report["benchmark_scope"] = (
        "Same full synth, controls, four voices and scores. DSP-only offline "
        "timing; ordinary new hook, not malloc/aligned-new/OS contention proof. "
        "No universal fastest backend or target-device acceptance is asserted.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--replay", type=Path)
    args = parser.parse_args()
    study = Study(args.out.resolve(), args.replay.resolve() if args.replay else None)
    failure = None
    try:
        study.execute()
        extra_checks(study)
    except Exception as exc:
        failure = str(exc)
    finally:
        study.save(failure)
        for relative in ("tools/modules/sharp_qualification.py",
                         "tests/test_sharp_contract.py"):
            source = ROOT / relative
            if source.exists():
                target = study.out / "source" / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
                study.report["source_files"][relative] = sha(source)
        (study.out / "report.json").write_text(
            json.dumps(study.report, indent=2) + "\n")
    if failure:
        raise SystemExit(failure)


if __name__ == "__main__":
    main()
