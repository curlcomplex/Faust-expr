#!/usr/bin/env python3
"""Issue #105: separately pinned, offline analysis of existing float32 renders.

Only the meter is compiled. No instrument, accepted baseline or input is edited.
See modules/FAUST_ANALYSIS.md for measurement scope and qualification commands.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
DSP = ROOT / "tools/modules/faust_analysis_meter.dsp"
RUNNER = ROOT / "tools/modules/faust_analysis_runner.cpp"
VERSION = "2.88.0"
ARCHIVE_SHA256 = "e4e175cf236924b5b7d4784cbb8c50cc01e211159e169655dd9e6d8f92b871d9"
TAIL_FRAMES = 11  # an.true_peak has 12 input-rate taps per phase in this release.


def sha(path: Path | str) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(args, *, timeout: int = 180, cwd: Path = ROOT) -> str:
    result = subprocess.run([str(x) for x in args], cwd=cwd, capture_output=True,
                            text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {args}\n"
                           f"{result.stdout}\n{result.stderr}")
    return result.stdout.strip()


def executable(name: str) -> Path:
    found = shutil.which(str(name))
    if not found:
        raise ValueError(f"executable not found: {name}")
    return Path(found).resolve()


def library_manifest(path: Path) -> dict[str, str]:
    if not (path / "stdfaust.lib").is_file() or not (path / "analyzers.lib").is_file():
        raise ValueError("--faust-libraries must contain stdfaust.lib and analyzers.lib")
    return {p.relative_to(path).as_posix(): sha(p) for p in sorted(path.rglob("*.lib"))}


def verify_release_archive(archive: Path, manifest: dict[str, str]) -> str:
    digest = sha(archive)
    if digest != ARCHIVE_SHA256:
        raise ValueError("Faust release archive SHA-256 mismatch")
    expected = {}
    with tarfile.open(archive, "r:gz") as source:
        for member in source:
            if member.isfile() and "/libraries/" in member.name and member.name.endswith(".lib"):
                name = member.name.split("/libraries/", 1)[1]
                with source.extractfile(member) as stream:
                    expected[name] = hashlib.sha256(stream.read()).hexdigest()
    if not expected or manifest != expected:
        raise ValueError("Faust libraries do not match the verified release archive")
    return digest


def build(out: Path, faust: str, cxx: str, library_path=None, archive=None):
    """Build one reusable mono meter; never trust an old output binary implicitly."""
    if not library_path:
        raise ValueError("explicit --faust-libraries is required")
    libraries = Path(library_path).resolve()
    manifest = library_manifest(libraries)
    archive_digest = verify_release_archive(Path(archive), manifest) if archive else None
    faust_path, cxx_path = executable(faust), executable(cxx)
    version = run([faust_path, "--version"])
    if not re.search(r"^FAUST Version " + re.escape(VERSION) + r"\s*$", version, re.M):
        raise ValueError(f"analysis requires Faust {VERSION}; got {version.splitlines()[0]}")
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    generated, runner = out / "analysis_generated.hpp", out / "analysis_runner"
    flags = ["-lang", "cpp", "-double", "-cn", "ModuleDSP", "-I", str(libraries)]
    cpp_flags = ["-std=c++17", "-O2", "-ffp-contract=off", "-Werror=vexing-parse"]
    commands = [[faust_path, *flags, DSP, "-o", generated],
                [cxx_path, *cpp_flags, "-I" + str(out), RUNNER, "-o", runner]]
    for index, command in enumerate(commands):
        try:
            text = run(command)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            (out / f"build-{index}.log").write_text(str(error) + "\n")
            raise
        (out / f"build-{index}.log").write_text(text + "\n")
    (out / "libraries.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    provenance = {
        "faust_version": version, "faust_binary_sha256": sha(faust_path),
        "release_archive_sha256": archive_digest, "release_archive_verified": bool(archive_digest),
        "library_tree_sha256": hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest(),
        "library_manifest": manifest, "analysis_dsp_sha256": sha(DSP),
        "driver_sha256": sha(Path(__file__)), "generated_sha256": sha(generated),
        "runner_source_sha256": sha(RUNNER), "runner_binary_sha256": sha(runner),
        "cxx_version": run([cxx_path, "--version"]), "cxx_binary_sha256": sha(cxx_path),
        "faust_flags": flags, "cxx_flags": cpp_flags,
        "commands": [[str(x) for x in command] for command in commands],
        "python_version": platform.python_version(), "numpy_version": np.__version__,
        "system": platform.system(), "architecture": platform.machine(),
        "execution_lane": os.environ.get("EXECUTION_LANE", "local-unspecified"),
        "github_sha": os.environ.get("GITHUB_SHA"), "github_run_id": os.environ.get("GITHUB_RUN_ID"),
    }
    return runner, provenance


def read_input(src: Path, rate: int, channels: int):
    if not 8000 <= rate <= 192000 or not 1 <= channels <= 32:
        raise ValueError("invalid rate/channels")
    size = src.stat().st_size
    if size == 0 or size % (4 * channels):
        raise ValueError("input must contain complete interleaved float32 frames")
    frames = size // (4 * channels)
    if frames > rate * 600:
        raise ValueError("input exceeds the 600-second analysis limit")
    data = np.memmap(src, dtype="<f4", mode="r", shape=(frames, channels))
    for base in range(0, frames, 65536):
        if not np.isfinite(data[base:base + 65536]).all():
            raise ValueError("nonfinite input")
    return data


def analyze(src: Path, rate: int, channels: int, out: Path, runner: Path,
            provenance: dict, *, block: int = 256) -> dict:
    """Return independent mono channel measurements (not summed programme LUFS)."""
    src, out = Path(src).resolve(), Path(out).resolve()
    if not 1 <= block <= 65536:
        raise ValueError("invalid block size")
    # Avoid any possibility of overwriting the input with report/build/temp data.
    if src == out or out in src.parents:
        raise ValueError("input must be outside the output directory")
    out.mkdir(parents=True, exist_ok=True)
    report_path = out / "analysis.json"
    report_path.unlink(missing_ok=True)  # a failed rerun must not leave a stale success
    data = read_input(src, rate, channels)
    frames = len(data)
    input_hash = sha(src)
    per_channel = []
    for channel in range(channels):
        with tempfile.TemporaryDirectory(prefix="meter-", dir=out) as work:
            inp, raw = Path(work) / "input.f32", Path(work) / "output.f32"
            np.asarray(data[:, channel], dtype="<f4").tofile(inp)
            run([runner, inp, raw, rate, frames, block, TAIL_FRAMES])
            expected_size = (frames + TAIL_FRAMES) * 6 * 4
            if raw.stat().st_size != expected_size:
                raise RuntimeError("analysis DSP output size contract changed")
            y = np.memmap(raw, dtype="<f4", mode="r", shape=(frames + TAIL_FRAMES, 6))
            if not np.array_equal(y[:frames, 0], data[:, channel]):
                raise RuntimeError("analyzer passthrough changed the input")
            peak = float(np.max(y[:, 1]))
            hold = float(y[-1, 2])
            if peak != hold:
                raise RuntimeError("true-peak max-hold differs from samplewise maximum")
            x = np.asarray(data[:, channel], dtype=np.float64)
            # Read loudness BEFORE FIR-tail padding: zeros must not redefine the programme.
            last = y[frames - 1]
            per_channel.append({
                "channel": channel, "sample_peak": float(np.max(np.abs(x))),
                "rms": float(np.sqrt(np.mean(x * x))),
                "faust_true_peak_estimate_max": peak, "faust_true_peak_hold_final": hold,
                "faust_loudness_momentary_final_lufs": float(last[3]),
                "faust_loudness_shortterm_final_lufs": float(last[4]),
                "faust_loudness_integrated_streaming_approx_final_lufs": float(last[5]),
                "momentary_full_window": frames >= round(rate * 0.4),
                "shortterm_full_window": frames >= rate * 3,
            })
            del last, y
    if sha(src) != input_hash:
        raise RuntimeError("input changed during analysis")
    report = {
        "schema": 2,
        "input": {"path": str(src), "sha256": input_hash, "rate": rate,
                  "channels": channels, "frames": frames, "format": "interleaved-f32le"},
        "provenance": provenance, "channels": per_channel,
        "measurement": {"channel_policy": "independent-mono-no-programme-sum",
                        "block_frames": block, "true_peak_tail_frames": TAIL_FRAMES,
                        "loudness_end_frame_exclusive": frames, "reset": "fresh-DSP-per-channel",
                        "warmup": "zero-initialized; no source frames discarded",
                        "loudness_floor_lufs": -100.0},
        "notes": ["Faust integrated loudness is a streaming approximation, not exact offline two-pass loudness.",
                  "Multichannel results are independent mono diagnostics, not aggregate programme LUFS; no LFE/layout weights are inferred.",
                  "Short-window readings include initial zero state; full-window flags identify sufficient duration.",
                  "True peak includes the 11-frame FIR tail; loudness ends at the original input boundary.",
                  "No normalization or release threshold is implied by these measurements."],
    }
    text = json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"
    with tempfile.NamedTemporaryFile("w", dir=out, prefix="report-", delete=False) as temp:
        temp.write(text)
        temp_path = Path(temp.name)
    temp_path.replace(report_path)
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--rate", type=int, required=True)
    parser.add_argument("--channels", type=int, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--faust", default=os.environ.get("FAUST", "faust"))
    parser.add_argument("--faust-libraries", default=os.environ.get("FAUST_LIBRARIES"))
    parser.add_argument("--faust-archive", type=Path)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--block", type=int, default=256)
    args = parser.parse_args()
    try:
        source, out = args.input.resolve(), args.out.resolve()
        if source == out or out in source.parents:
            raise ValueError("input must be outside the output directory")
        out.mkdir(parents=True, exist_ok=True)
        (out / "analysis.json").unlink(missing_ok=True)
        read_input(source, args.rate, args.channels)  # reject invalid input before compilation
        runner, provenance = build(out / "build", args.faust, args.cxx,
                                   args.faust_libraries, args.faust_archive)
        report = analyze(source, args.rate, args.channels, out, runner, provenance, block=args.block)
    except (ValueError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(2, str(error) + "\n")
    print(json.dumps({"report": str(out / "analysis.json"), "channels": report["channels"]}, indent=2))


if __name__ == "__main__":
    main()
