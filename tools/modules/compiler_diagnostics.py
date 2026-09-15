#!/usr/bin/env python3
"""#109: bounded, fail-closed diagnostics; never a sonic/performance verdict."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import tarfile
import tempfile
import time

VERSION = "2.88.0"
ARCHIVE_SHA256 = "e4e175cf236924b5b7d4784cbb8c50cc01e211159e169655dd9e6d8f92b871d9"
COUNTERS = ("FP_SUBNORMAL", "FP_INFINITE", "FP_NAN", "INTEGER_OVERFLOW",
            "DIV_BY_ZERO_REAL", "DIV_BY_ZERO_INT", "CAST_INT_OVERFLOW", "NEGATIVE_BITSHIFT")
HARD_COUNTERS = set(COUNTERS) - {"FP_SUBNORMAL", "INTEGER_OVERFLOW", "NEGATIVE_BITSHIFT"}
BANNERS = {"Inf": "FP_INFINITE", "NaN": "FP_NAN", "REAL div by zero": "DIV_BY_ZERO_REAL",
           "Int div by zero": "DIV_BY_ZERO_INT", "CastIntOverflow": "CAST_INT_OVERFLOW",
           "Overflow": "INTEGER_OVERFLOW", "Bitshift": "NEGATIVE_BITSHIFT"}


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")


def executable(value):
    found = shutil.which(str(value))
    if not found:
        raise ValueError(f"executable not found: {value}")
    return Path(found).resolve()


def run(command, directory, name, *, env=None, timeout=60, max_bytes=8 << 20):
    """Keep raw logs even on timeout/crash. Never interpret an OS error as a DSP fault."""
    directory = Path(directory)
    paths = [directory / (name + suffix) for suffix in (".stdout", ".stderr")]
    command = [str(x) for x in command]
    status, rc, error = "completed", None, None
    start = time.monotonic()
    with paths[0].open("wb") as stdout, paths[1].open("wb") as stderr:
        try:
            process = subprocess.Popen(command, cwd=directory, env=env, stdout=stdout,
                                       stderr=stderr, start_new_session=True)
            while process.poll() is None:
                if time.monotonic() - start > timeout:
                    status = "timeout"
                elif sum(p.stat().st_size for p in paths) > max_bytes:
                    status = "output_limit"
                if status != "completed":
                    os.killpg(process.pid, signal.SIGKILL)
                    break
                time.sleep(0.02)
            rc = process.wait()
        except OSError as exc:
            status, error = "execution_error", str(exc)
    if sum(p.stat().st_size for p in paths) > max_bytes:
        status = "output_limit"
    result = {"command": command, "cwd": str(directory), "status": status,
              "returncode": rc, "error": error, "timeout_seconds": timeout,
              "logs": {p.suffix[1:]: {"path": str(p), "sha256": sha(p), "bytes": p.stat().st_size}
                       for p in paths}}
    for field, path in zip(("stdout", "stderr"), paths):
        with path.open("rb") as stream:
            result[field] = stream.read(max_bytes).decode("utf-8", errors="replace")
    write_json(directory / (name + ".command.json"), result)
    return result


def classify_compile(result):
    text = result["stdout"] + "\n" + result["stderr"]
    return {"hard_failure": result.get("status", "completed") != "completed" or
            result["returncode"] != 0 or bool(re.search(r"(?im)^\s*ERROR\s*:", text)),
            "warning_present": bool(re.search(r"(?im)^\s*WARNING\b", text)),
            "policy": "compiler errors hard; interval warnings informational, not sonic gates"}


def classify_trace(result, profile="input"):
    """Require positive counters/banners; mere mention of a zero counter proves nothing."""
    text = result["stdout"] + "\n" + result["stderr"]
    blocks = []
    for section in re.findall(r"Interpreter statistics\s*\n(.*?)(?=^-{3,}|\Z)", text, re.M | re.S):
        blocks.append({key: int(value) for key, value in
                       re.findall(r"^([A-Z_]+):\s*(\d+)\s*$", section, re.M) if key in COUNTERS})
    positive = sorted({key for block in blocks for key, value in block.items() if value > 0})
    banners = re.findall(r"Interpreter '([^']+)'[^\n]*trace start", text)
    hard = set(positive) & HARD_COUNTERS
    advisory = set(positive) - HARD_COUNTERS
    for banner in banners:
        category = BANNERS.get(banner)
        if category in HARD_COUNTERS:
            hard.add(category)
        elif category:
            advisory.add(category)
        else:
            hard.add("unrecognized_interpreter_fault:" + banner)
    # Do not clear an unrecognized abort just because a statistics block contains zeros.
    if "Interpreter exit" in text and not hard:
        hard.add("unclassified_interpreter_abort")
    version_ok = bool(re.search(r"^Libfaust version\s*:\s*" + re.escape(VERSION) + r"\s*$", text, re.M))
    markers = ("-------- Test impulse --------", "-------- Test noise ----------") if profile == "input" else ("Use RandomControlUI", "step: 9 until: 10")
    coverage = (version_ok and "Using interpreter backend" in text and bool(blocks) and
                all(set(block) == set(COUNTERS) for block in blocks) and
                all(marker in text for marker in markers))
    operational = (result.get("status", "completed") != "completed" or result["returncode"] != 0 or
                   bool(re.search(r"(?im)^\s*(?:ERROR\s*:|Cannot create instance)", text)))
    return {"hard_failure": bool(hard) or operational or not coverage,
            "numerical_faults": sorted(hard), "advisories": sorted(advisory),
            "reported_statistics": blocks, "trace_banners": banners,
            "coverage_complete": bool(coverage and not hard and not operational),
            "runtime_version_verified": version_ok, "operational_failure": operational,
            "interpretation": "counters describe interpreter operations, not distinct bad audio samples"}


def libraries_identity(libraries, archive=None):
    libraries = Path(libraries)
    if not (libraries / "stdfaust.lib").is_file():
        raise ValueError("explicit matching Faust libraries are required")
    manifest = {p.relative_to(libraries).as_posix(): sha(p) for p in sorted(libraries.rglob("*.lib"))}
    verified = False
    if archive:
        if sha(archive) != ARCHIVE_SHA256:
            raise ValueError("Faust archive SHA-256 mismatch")
        expected = {}
        with tarfile.open(archive, "r:gz") as source:
            for member in source:
                if member.isfile() and "/libraries/" in member.name and member.name.endswith(".lib"):
                    with source.extractfile(member) as stream:
                        expected[member.name.split("/libraries/", 1)[1]] = hashlib.sha256(stream.read()).hexdigest()
        if manifest != expected or not expected:
            raise ValueError("libraries do not match pinned archive")
        verified = True
    return {"manifest": manifest, "tree_sha256": hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest(),
            "archive_verified": verified, "archive_sha256": ARCHIVE_SHA256 if verified else None}


def dependency_manifest(text):
    match = re.search(r"List of file dependencies\s*:\s*\n-+\n(.*?)\n-+", text, re.S)
    if not match:
        raise ValueError("missing compiler dependency manifest")
    return {str(Path(name).resolve()): sha(name) for name in match.group(1).splitlines() if name}


def diagnose(dsp, faust, libs, out, tracer=None, trace=4, structural=False, *, archive=None, profile="auto"):
    if trace != 4 or profile not in ("auto", "input", "controls"):
        raise ValueError("this qualified interface requires trace 4 and a known profile")
    dsp, out, libs = Path(dsp).resolve(), Path(out).resolve(), Path(libs).resolve()
    if out == dsp.parent or out in dsp.parents or out == libs or out in libs.parents or libs in out.parents:
        raise ValueError("output must be isolated from source and toolchain libraries")
    out.mkdir(parents=True, exist_ok=True)
    lock = out / ".diagnostics-lock"
    try:
        lock.mkdir()
    except FileExistsError as exc:
        raise ValueError("output already in use; choose a separate directory") from exc
    report_path = out / "diagnostics.json"
    try:
        report_path.unlink(missing_ok=True)
        faust = executable(faust)
        tracer = executable(tracer) if tracer else None
        identity = libraries_identity(libs, archive)
        original_hash = sha(dsp)
        directory = Path(tempfile.mkdtemp(prefix="run-", dir=out))
        env = dict(os.environ, LC_ALL="C", FAUST_LIB_PATH=str(libs))
        for key in ("FAUST_INTERP_TRACE", "FAUST_INTERP_OUTPUT"):
            env.pop(key, None)
        version = run([faust, "--version"], directory, "version", env=env)
        if version["returncode"] != 0 or not re.search(r"^FAUST Version " + re.escape(VERSION) + r"\s*$", version["stdout"], re.M):
            raise ValueError("diagnostics requires exact Faust " + VERSION)
        compiler = run([faust, "-wall", "-me", "-single", "-scal", "-lang", "cpp", "-flist",
                        "-cn", "ModuleDSP", "-I", libs, dsp, "-o", directory / "diagnostic.cpp"], directory, "compiler", env=env)
        classification = classify_compile(compiler)
        dependencies, generated = {}, directory / "diagnostic.cpp"
        if not classification["hard_failure"]:
            if not generated.is_file():
                classification["hard_failure"] = True
                classification["missing_generated_output"] = True
            else:
                dependencies = dependency_manifest(compiler["stdout"])
        result = {"schema": 2, "input": {"path": str(dsp), "sha256": original_hash},
                  "compiler": compiler, "compiler_classification": classification,
                  "provenance": {"faust_version": version["stdout"].strip(), "faust_binary_sha256": sha(faust),
                                 "libraries": identity, "dependencies": dependencies,
                                 "generated_sha256": sha(generated) if generated.is_file() else None,
                                 "driver_sha256": sha(__file__), "python": platform.python_version(),
                                 "system": platform.system(), "machine": platform.machine(),
                                 "execution_lane": os.environ.get("EXECUTION_LANE", "local-unspecified"),
                                 "github_run_id": os.environ.get("GITHUB_RUN_ID"), "github_sha": os.environ.get("GITHUB_SHA")},
                  "interp_tracer": {"status": "not_requested"}, "structural": {"status": "not_requested"},
                  "notes": ["Diagnostic outputs are not release artifacts; no source, sound or acceptance threshold is changed.",
                            "Interval warnings may be false positives. Subnormals/integer overflow/negative shifts are advisories.",
                            "Executed divide-by-zero/NaN/Inf/cast/memory faults are hard diagnostics even with finite final output.",
                            "A successful bounded probe is not proof of safety for all inputs, parameters, rates or backends."]}
        if tracer:
            if classification["hard_failure"]:
                result["interp_tracer"] = {"status": "not_run_compile_failed"}
            else:
                ni = re.search(r"getNumInputs\(\)\s*\{\s*return (\d+);", generated.read_text())
                if not ni:
                    raise ValueError("cannot identify diagnostic DSP input count")
                effective = ("input" if int(ni.group(1)) else "controls") if profile == "auto" else profile
                if effective == "input" and int(ni.group(1)) == 0:
                    raise ValueError("input tracer profile requires an effect input")
                tr = run([tracer, "-trace", "4", "-" + ("input" if effective == "input" else "control"),
                          "-noui", "-timeout", "2", "-I", libs, dsp], directory, "tracer", env=env, timeout=15)
                result["interp_tracer"] = {**tr, "classification": classify_trace(tr, effective),
                                           "binary_sha256": sha(tracer), "backend": "interpreter", "trace_mode": 4,
                                           "profile": effective, "sample_rate": 44100, "block_frames": 16,
                                           "input_profile": "fresh impulse and noise clones, 1000 blocks each" if effective == "input" else "upstream control endpoint/zero checks and 10 random-control blocks",
                                           "source_linkage": "binary hash and runtime version recorded; see retained CI build evidence",
                                           "environment": {"FAUST_LIB_PATH": str(libs), "LC_ALL": "C"}}
        if structural:
            help_result = run([faust, "-h"], directory, "help", env=env)
            supported = bool(re.search(r"(?m)^\s*-sig\s", help_result["stdout"]))
            sr = {"backend": "ocpp", "interpretation": "static compiler structure; never runtime-performance evidence",
                  "status": "unsupported" if not supported else "not_run_compile_failed"}
            if supported and not classification["hard_failure"]:
                raw = run([faust, "-lang", "ocpp", "-single", "-sig", "-I", libs, dsp,
                           "-o", directory / "structural.cpp"], directory, "structural", env=env)
                signatures = re.findall(r"(?m)^SS_SIG .+$", raw["stdout"] + "\n" + raw["stderr"])
                ok = raw["status"] == "completed" and raw["returncode"] == 0 and len(signatures) == 1 and (directory / "structural.cpp").is_file()
                sr.update(status="completed" if ok else "failed", result=raw, signatures=signatures)
            result["structural"] = sr
        unchanged = sha(dsp) == original_hash and all(sha(p) == digest for p, digest in dependencies.items())
        unchanged = unchanged and libraries_identity(libs)["tree_sha256"] == identity["tree_sha256"]
        result["input_and_dependencies_unchanged"] = unchanged
        result["hard_failure"] = bool(classification["hard_failure"] or not unchanged or
                                       result["interp_tracer"].get("classification", {}).get("hard_failure", False) or
                                       result["structural"]["status"] == "failed")
        result["status"] = "failed" if result["hard_failure"] else "completed"
        write_json(directory / "report.json", result)
        # Publish only after every requested stage and identity check has completed.
        shutil.copyfile(directory / "report.json", out / ".report.tmp")
        (out / ".report.tmp").replace(report_path)
        return result
    finally:
        lock.rmdir()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dsp", type=Path)
    parser.add_argument("--faust", required=True)
    parser.add_argument("--faust-libraries", required=True)
    parser.add_argument("--faust-archive", type=Path)
    parser.add_argument("--interp-tracer")
    parser.add_argument("--trace", type=int, default=4, choices=[4])
    parser.add_argument("--tracer-profile", default="auto", choices=["auto", "input", "controls"])
    parser.add_argument("--structural", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = diagnose(args.dsp, args.faust, args.faust_libraries, args.out, args.interp_tracer,
                          args.trace, args.structural, archive=args.faust_archive, profile=args.tracer_profile)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(2, str(error) + "\n")
    print(json.dumps({"report": str(args.out / "diagnostics.json"), "status": result["status"],
                      "hard_failure": result["hard_failure"]}))
    return 2 if result["hard_failure"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
