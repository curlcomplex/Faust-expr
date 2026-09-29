#!/usr/bin/env python3
"""Run the bounded compiler-plan/LLVM semantic checkpoint, retaining raw evidence.

Producer: pinned patched Faust 2.88.0 super-node planner. Consumer: the installed
libfaust signal API + LLVM backend (version recorded, not assumed identical).
The bridge is serial and correctness-only: local state stays in LLVM instances,
external histories in the host, and subchunks obey the upstream chunk bound.
"""
from __future__ import annotations
import argparse
from array import array
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from typing import Any

from plan_llvm_codegen import emit_header, validate, compile_specs

ROOT = Path(__file__).resolve().parents[1]
FRAMES = 16384
# Retain the previously declared region/reference and reload criteria unchanged.
REGION_MAX = 3e-5
REGION_RMS = 5e-6
RELOAD_MAX = 1e-7
RATES = (44100, 48000, 96000)
READERS = 3


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def run(cmd: list[str | Path], log: Path, *, cwd: Path | None = None,
        env: dict[str, str] | None = None, timeout: int = 90,
        expect_failure: bool = False) -> subprocess.CompletedProcess:
    started = time.monotonic()
    try:
        p = subprocess.run(list(map(str, cmd)), cwd=cwd, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        partial = exc.stdout or ""
        if isinstance(partial, bytes): partial = partial.decode("utf-8", errors="replace")
        log.write_text(partial)
        write_json(log.with_suffix(log.suffix + ".command.json"), {
            "command": list(map(str, cmd)), "timed_out": True,
            "timeout_seconds": timeout, "cwd": str(cwd) if cwd else None,
        })
        raise RuntimeError(f"bounded command timed out; partial evidence: {log}") from exc
    log.write_text(p.stdout)
    write_json(log.with_suffix(log.suffix + ".command.json"), {
        "command": list(map(str, cmd)), "returncode": p.returncode,
        "elapsed_seconds_not_benchmark": time.monotonic() - started,
        "cwd": str(cwd) if cwd else None, "expected_failure": expect_failure,
    })
    if (p.returncode != 0) != expect_failure:
        raise RuntimeError(f"unexpected exit={p.returncode}; see {log}")
    return p


def fixtures() -> list[dict]:
    n = 16
    f = [f"f{i}" for i in range(n)]
    x = [f"x{i}" for i in range(n)]
    feedback = ("mix(" + ",".join(f + x) + ") = " +
                ",".join(f"({f[i]}+{x[i]})" for i in range(n)) + ";\n")
    feedback += ("feedback(" + ",".join(x) + ") = " +
                 ",".join(f"(({x[(i-1)%n]}@{63+17*i})*{0.2+i*0.003})" for i in range(n)) + ";\n")
    feedback += "process = _ <: (mix ~ feedback) :> _ : *(0.03);\n"
    return [
        {"name": "feedback-fused", "source": feedback, "options": ["-ls-fuse", "-ls-sched", "cs2", "-vs", "32"], "require_feedback": True},
        {"name": "feedback-unfused", "source": feedback, "options": ["-ls", "-ls-sched", "df", "-vs", "32"], "require_feedback": True},
        {"name": "nonlinear-control", "source": "process(x,c) = ((x*c) @ 7) + 0.25*(x*(x@1));\n", "options": ["-ls-fuse", "-ls-sched", "cs2", "-vs", "32"]},
        {"name": "delayed-constant", "source": "process(x) = x * 0.125 + (0.25 @ 7);\n", "options": ["-ls-fuse", "-ls-sched", "cs2", "-vs", "32"]},
    ]


def read_samples(path: Path, channels: int) -> array:
    data = path.read_bytes()
    if len(data) != FRAMES * channels * 4:
        raise ValueError(f"exact capture length mismatch: {path}")
    values = array("f"); values.frombytes(data)
    if sys.byteorder != "little":
        values.byteswap()
    if any(not math.isfinite(v) for v in values):
        raise ValueError(f"nonfinite capture: {path}")
    return values


def metrics(a: array, b: array) -> dict:
    if len(a) != len(b) or not a:
        raise ValueError("comparison requires equal nonempty arrays")
    diffs = [float(x) - float(y) for x, y in zip(a, b)]
    at = max(range(len(diffs)), key=lambda i: abs(diffs[i]))
    return {"max_abs": abs(diffs[at]), "rms": math.sqrt(math.fsum(d*d for d in diffs) / len(diffs)),
            "max_index": at, "bit_identical": a.tobytes() == b.tobytes(),
            "reference_peak": max(map(abs, a))}


def evaluate(directory: Path, plan: dict, stateful: bool = True) -> dict:
    checks: list[dict] = []
    loaded: dict[Path, array] = {}
    no = plan["outputs"]
    def audio(path: Path) -> array:
        if path not in loaded:
            loaded[path] = read_samples(path, no)
        return loaded[path]
    def compare(label: str, a: Path, b: Path, strict: bool = False, negative: bool = False) -> None:
        m = metrics(audio(a), audio(b))
        within = m["max_abs"] <= (RELOAD_MAX if strict else REGION_MAX) and m["rms"] <= (RELOAD_MAX if strict else REGION_RMS)
        passed = not within if negative else within
        checks.append({"label": label, "reference": str(a.relative_to(directory)),
                       "candidate": str(b.relative_to(directory)), "strict_reload_or_partition": strict,
                       "expected_rejection": negative, "pass": passed, **m})
    processes = ["source"] + [f"reader-{i}" for i in range(READERS)]
    reports = [json.loads((directory / proc / "process.json").read_text()) for proc in processes]
    if len({r["pid"] for r in reports}) != len(reports):
        raise ValueError("writer and readers were not distinct processes")
    expected_plan = hashlib.sha256(json.dumps(plan, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()
    for i, r in enumerate(reports):
        if r["plan_sha256"] != expected_plan or r["frames"] != FRAMES or r["kernel_min_frames"] != 1 or r["kernel_max_frames"] != (plan["chunk_size"] if stateful else 1):
            raise ValueError("reader plan/capture identity mismatch")
        if r["execution_abi"] != ("stateful-chunk-v1" if stateful else "host-transition-v1") or r["kernel_calls"] <= 0:
            raise ValueError("execution ABI or actual kernel-call coverage mismatch")
        if i and (r["source_compiles"] != 0 or r["signal_compiles"] != 0 or r["bitcode_reads"] != len(plan["blocks"]) + 2):
            raise ValueError("read-only process performed a forbidden compile or missed a factory")
    for proc in processes:
        base = directory / proc
        for sr in RATES:
            for p in range(3):
                suffix = f"-{sr}-p{p}.f32"
                compare(f"{proc}/regions-v-whole/{sr}/{p}", base / ("whole" + suffix), base / ("regions" + suffix))
                if proc != "source":
                    for backend in ("whole", "regions"):
                        compare(f"{proc}/fresh-reload/{backend}/{sr}/{p}", directory / "source" / (backend + suffix), base / (backend + suffix), strict=True)
            for backend in ("whole", "regions"):
                for p in (1, 2):
                    compare(f"{proc}/partition/{backend}/{sr}/{p}", base / f"{backend}-{sr}-p0.f32", base / f"{backend}-{sr}-p{p}.f32", strict=True)
    reference = directory / "source" / "whole-48000-p1.f32"
    for negative in ("reset", "delay"):
        compare(f"negative-control/{negative}", reference, directory / "source" / f"negative-{negative}.f32", negative=True)
    if max(map(abs, audio(reference))) <= 1e-4:
        raise ValueError("reference is silent or too small to challenge the gates")
    if directory.name == "delayed-constant":
        values = audio(reference)
        if list(values[:7]) != [0.0]*7 or list(values[7:16]) != [0.25]*9:
            raise ValueError("constant-history initial-delay semantics are wrong")
    passed = all(c["pass"] for c in checks)
    return {"pass": passed, "positive_checks": sum(not c["expected_rejection"] for c in checks),
            "negative_controls": sum(c["expected_rejection"] for c in checks),
            "processes": reports, "checks": checks}


def checkpoint(args: argparse.Namespace) -> dict:
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError("refusing to overwrite an earlier evidence directory")
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if args.libdir:
        key = "DYLD_LIBRARY_PATH" if sys.platform == "darwin" else "LD_LIBRARY_PATH"
        env[key] = str(args.libdir.resolve()) + os.pathsep + env.get(key, "")
    run([args.cxx, "--version"], output / "cxx-version.txt", env=env)
    run(["uname", "-a"], output / "platform.txt", env=env)
    if args.faust:
        run([args.faust, "--version"], output / "producer-faust-version.txt", env=env)
    if args.system_faust:
        run([args.system_faust, "--version"], output / "consumer-faust-version.txt", env=env)
    result = {"schema": "plan-llvm-evidence-v1", "pass": False,
              "source_script_sha256": sha(Path(__file__)), "runtime_sha256": sha(ROOT / "scripts/plan_llvm_runtime.cpp"),
              "gates": {"region_max_abs": REGION_MAX, "region_rms": REGION_RMS, "reload_max_abs": RELOAD_MAX},
              "producer_consumer_versions_may_differ": True, "realtime_or_throughput_test": False,
              "execution_abi": "host-transition-v1" if args.state_transition_only else "stateful-chunk-v1", "fixtures": {}}
    implementation = output / "implementation"; implementation.mkdir()
    for code in sorted((ROOT / "scripts").glob("plan_llvm_*")):
        if code.is_file(): shutil.copyfile(code, implementation / code.name)
    definitions = fixtures()
    stateful = not args.state_transition_only
    for definition in definitions:
        name = definition["name"]; d = output / name; d.mkdir()
        print(f"stage={name}/export", flush=True)
        source = d / "fixture.dsp"; source.write_text(definition["source"])
        planfile = d / "plan.json"
        if args.replay_plans:
            upstream = args.replay_plans.resolve() / name
            if (upstream / "fixture.dsp").read_text() != definition["source"]:
                raise ValueError("replayed plan's source does not match the test definition")
            origin_manifest = json.loads((args.replay_plans.resolve() / "manifest.json").read_text())
            for filename in ("plan.json", "fixture.dsp"):
                if origin_manifest.get(f"{name}/{filename}") != sha(upstream / filename):
                    raise ValueError("replayed producer artifact checksum mismatch")
            shutil.copyfile(upstream / "plan.json", planfile)
            write_json(d / "replay-origin.json", {"plan_sha256": sha(planfile), "source_sha256": sha(source)})
        else:
            if not args.faust:
                raise ValueError("a pinned exporter or replay-plan directory is required")
            export_env = env.copy(); export_env["FAUST_PLAN_LLVM_EXPORT"] = str(planfile)
            run([args.faust, "-lang", "ocpp", *definition["options"], source, "-o", d / "upstream.cpp"], d / "export.txt", env=export_env)
        plan = json.loads(planfile.read_text()); structure = validate(plan)
        if definition.get("require_feedback") and not (structure["blocks"] >= 2 and structure["cross_delayed"] and structure["cross_region_feedback"]):
            raise ValueError("fixture failed the predeclared multi-region delayed-feedback structural requirement")
        specs = compile_specs(plan, stateful)
        local = {tap[1] for spec in specs for tap in spec["local_history"]}
        external = {tap[1] for spec in specs for tap in spec["inputs"] if tap[0] == "history"}
        structure.update(local_history_owners=len(local), host_history_owners=len(external),
                         kernel_chunk_limit=plan["chunk_size"] if stateful else 1)
        write_json(d / "structure.json", structure)
        header = d / "plan_generated.hpp"; header.write_text(emit_header(plan, definition["source"], stateful))
        common = [args.cxx, "-std=c++17", "-O2", "-I" + str(d)]
        if args.include:
            common += ["-I" + str(args.include.resolve())]
        link = []
        if args.libdir:
            link += ["-L" + str(args.libdir.resolve()), "-Wl,-rpath," + str(args.libdir.resolve())]
        link += ["-lfaust", "-lpthread", "-lz"]
        for label, flags in (("prepare", []), ("reader", ["-DPLAN_READER"])):
            run(common + flags + [ROOT / "scripts/plan_llvm_runtime.cpp"] + link + ["-o", d / label], d / f"compile-{label}.txt", env=env)
        undefined = run(["nm", "-u", d / "reader"], d / "reader-undefined-symbols.txt", env=env).stdout
        if "createDSPFactoryFrom" in undefined or "construct_region" in undefined:
            raise ValueError("reader binary unexpectedly references source/signal compilation")
        cache = d / "cache"
        print(f"stage={name}/execute-writer", flush=True)
        run([d / "prepare", cache, d / "source"], d / "writer.txt", env=env)
        before = {p.name: sha(p) for p in cache.glob("*.bc")}
        for reader in range(READERS):
            isolated = d / f"reader-{reader}"; isolated.mkdir()
            isolated_cache = isolated / "cache"; isolated_cache.mkdir()
            shutil.copy2(d / "reader", isolated / "reader")
            for p in cache.glob("*.bc"):
                shutil.copyfile(p, isolated_cache / p.name)
                (isolated_cache / p.name).chmod(0o444)
            # The isolated reader directory contains no Faust source or generated
            # source and the reader executable has no source/signal compile path.
            run([isolated / "reader", isolated_cache, isolated], d / f"reader-{reader}.txt", cwd=isolated, env=env)
            after = {p.name: sha(p) for p in isolated_cache.glob("*.bc")}
            if after != before:
                raise ValueError("reader mutated cached bitcode")
            # One canonical cache remains. Keep hashes, not redundant binaries.
            write_json(isolated / "input-bitcode-sha256.json", after)
            for p in isolated_cache.glob("*.bc"):
                p.chmod(0o644)
            shutil.rmtree(isolated_cache); (isolated / "reader").unlink()
        evaluation = evaluate(d, plan, stateful)
        write_json(d / "evaluation.json", evaluation)
        result["fixtures"][name] = {"structure": structure, "pass": evaluation["pass"],
                                    "positive_checks": evaluation["positive_checks"], "negative_controls": evaluation["negative_controls"],
                                    "plan_sha256": sha(planfile), "source_sha256": sha(source), "bitcode": before}
        write_json(output / "results.json", result)
        if not evaluation["pass"]:
            raise ValueError(f"numerical checkpoint failed for {name}; thresholds unchanged")
        print(f"stage_done={name} positive_checks={evaluation['positive_checks']} negatives={evaluation['negative_controls']}", flush=True)

    # Same source through upstream pre/post-fusion plans. No hand-authored cuts.
    cross = []
    for sr in RATES:
        for p in range(3):
            suffix = f"regions-{sr}-p{p}.f32"
            a = read_samples(output / "feedback-unfused/source" / suffix, 1)
            b = read_samples(output / "feedback-fused/source" / suffix, 1)
            m = metrics(a, b)
            cross.append({"rate": sr, "partition": p, "pass": m["max_abs"] <= REGION_MAX and m["rms"] <= REGION_RMS, **m})
    result["fusion_comparisons"] = cross
    if not all(c["pass"] for c in cross):
        raise ValueError("pre/post-fusion numerical check failed")

    if not args.replay_plans:
        rejects = []
        for name, source_text in (("sine", "process = sin;\n"), ("integer-state", "process(x) = int(x) @ 3;\n")):
            d = output / ("unsupported-" + name); d.mkdir()
            s = d / "fixture.dsp"; s.write_text(source_text)
            env2 = env.copy(); env2["FAUST_PLAN_LLVM_EXPORT"] = str(d / "plan.json")
            p = run([args.faust, "-lang", "ocpp", "-ls-fuse", s, "-o", d / "generated.cpp"], d / "compiler.txt", env=env2, expect_failure=True)
            if "plan-llvm unsupported:" not in p.stdout or (d / "plan.json").exists():
                raise ValueError("unsupported input was not rejected by the exporter without a partial plan")
            rejects.append({"name": name, "rejected_without_plan": True})
        result["unsupported_source_controls"] = rejects
    else:
        result["unsupported_source_controls"] = "not rerun: this consumer-only job replays producer plans"
    result["pass"] = True
    result["positive_checks"] = sum(f["positive_checks"] for f in result["fixtures"].values()) + len(cross)
    result["negative_controls"] = sum(f["negative_controls"] for f in result["fixtures"].values())
    manifest = {str(p.relative_to(output)): sha(p) for p in sorted(output.rglob("*")) if p.is_file() and p.name not in ("manifest.json", "results.json")}
    write_json(output / "manifest.json", manifest)
    write_json(output / "results.json", result)
    lines = ["# Compiler-plan → LLVM semantic checkpoint", "", "**PASS — bounded semantic bridge, not a real-time or speedup result.**", "",
             f"{result['positive_checks']} full-array numerical comparisons passed; {result['negative_controls']} deliberately corrupted-state captures were correctly rejected.", "",
             "| Fixture | Upstream blocks | State owners | Cross-block delayed reads | Feedback across blocks |", "|---|---:|---:|---:|---|" ]
    for name, f in result["fixtures"].items():
        s = f["structure"]
        lines.append(f"| {name} | {s['blocks']} | {s['history_owners']} | {len(s['cross_delayed'])} | {s['cross_region_feedback']} |")
    lines += ["", "Each plan also has a separately executed output-tail kernel. Outputs are not omitted from the work.", "",
              "Real compiler-selected members and final expressions were exported before backend emission. Region kernels were constructed using the public signal API and compiled through libfaust LLVM; three separate source-free reader processes reconstructed cached factories. The reader binary has no source/signal-compilation entry points.", "",
              ("Stateful kernels execute 1–32 frames per call, bounded by the upstream planner's chunk contract. Local delay state is held inside real persistent LLVM DSP instances; only cross-kernel histories are held in the host. Host blocks of 1–513 are subdivided rather than assuming a 513-frame feedback cut is legal." if stateful else "Every kernel call is ONE SAMPLE; the host owns/commits all delay state. This is the state-transition oracle, NOT the stateful-block lowering."), "",
              "No thread pool, parallel execution, performance, Core Audio, live-state factory handoff, or general Faust support is claimed. Producer and consumer compiler versions are separately recorded. Rate cases exercise initialization at 44.1/48/96 kHz; these fixtures use fixed per-sample coefficients, not rate-adaptive physical parameters.", "",
              "Raw captures are exact-length, finite float32; no normalization, alignment, gain fitting or relaxed threshold. The original 3e-5 max / 5e-6 RMS region criteria and 1e-7 reload/partition gate are unchanged. Full metrics, negative captures, sources, plans, bitcode identities and process IDs accompany this report."]
    (output / "REPORT.md").write_text("\n".join(lines) + "\n")
    # Include final summaries, raw audio, sources and bitcode in one checksum set.
    manifest = {str(p.relative_to(output)): sha(p) for p in sorted(output.rglob("*")) if p.is_file() and p.name != "manifest.json"}
    write_json(output / "manifest.json", manifest)
    print((output / "REPORT.md").read_text(), flush=True)
    return result


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--faust", type=Path)
    p.add_argument("--system-faust", type=Path)
    p.add_argument("--replay-plans", type=Path)
    p.add_argument("--state-transition-only", action="store_true", help="use the one-sample host-state semantic oracle instead")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    p.add_argument("--include", type=Path)
    p.add_argument("--libdir", type=Path)
    args = p.parse_args()
    if args.faust:
        args.faust = args.faust.resolve()
    try:
        checkpoint(args)
    except Exception as e:
        args.output.mkdir(parents=True, exist_ok=True)
        write_json(args.output / "FAILURE.json", {"pass": False, "error": str(e)})
        raise

if __name__ == "__main__":
    main()
