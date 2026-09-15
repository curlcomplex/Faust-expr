#!/usr/bin/env python3
"""#107: additive offline Faust filter-bank traces over original f32le renders.

No instrument compilation, resampling, normalization, FFT-score replacement or
release gate. See modules/SPECTRAL_TRACES.md for the measurement contract.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
DSP = ROOT / "tools/modules/faust_spectral_descriptors.dsp"
RUNNER = ROOT / "tools/modules/faust_analysis_runner.cpp"
_spec = importlib.util.spec_from_file_location("_spectral_toolchain", ROOT / "tools/modules/faust_analysis.py")
_toolchain = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_toolchain)
sha, run = _toolchain.sha, _toolchain.run
FIELDS = ["input", "faust_filterbank_centroid_hz", "faust_filterbank_spread_hz",
          "faust_filterbank_flux", "faust_filterbank_power"]
MAX_FRAMES = 16_000_000  # also subject to the shared 600-second limit
MAX_ROWS = 200_000      # storage windows * channels, not a DSP sampling limit
EPSILON = float(np.finfo(np.float64).eps)


def profile() -> dict:
    """Read the literal profile from the exact DSP we compile, never a copy."""
    text = DSP.read_text()
    values = {}
    for name in ("O", "M", "FTOP", "N", "T", "HOP"):
        found = re.findall(r"^" + name + r"\s*=\s*([0-9]+(?:\.[0-9]+)?)\s*;", text, re.M)
        if len(found) != 1:
            raise ValueError(f"missing/ambiguous literal DSP setting: {name}")
        values[name] = float(found[0])
    for name in ("O", "M", "N"):
        if values[name] != int(values[name]):
            raise ValueError(f"noninteger DSP setting: {name}")
        values[name] = int(values[name])
    return {"filter_order": values["O"], "bands_per_octave": values["M"],
            "top_hz": values["FTOP"], "bands": values["N"],
            "power_window_seconds": values["T"], "flux_hop_seconds": values["HOP"]}


CONFIG = profile()


def build(out: Path, faust: str, cxx: str, libs, archive=None):
    """Compile the separate five-output DSP with the bounded shared offline host."""
    if not libs:
        raise ValueError("explicit --faust-libraries is required")
    libraries = Path(libs).resolve()
    manifest = _toolchain.library_manifest(libraries)
    archive_hash = _toolchain.verify_release_archive(Path(archive), manifest) if archive else None
    compiler, cpp = _toolchain.executable(faust), _toolchain.executable(cxx)
    version = run([compiler, "--version"])
    if not re.search(r"^FAUST Version 2\.88\.0\s*$", version, re.M):
        raise ValueError("spectral traces require exactly Faust 2.88.0")
    out = Path(out).resolve()
    if out == libraries or out in libraries.parents:
        raise ValueError("build output must not contain the toolchain libraries")
    out.mkdir(parents=True, exist_ok=True)
    generated, binary = out / "analysis_generated.hpp", out / "spectral_runner"
    binary.unlink(missing_ok=True)
    flags = ["-lang", "cpp", "-double", "-cn", "ModuleDSP", "-I", str(libraries)]
    cpp_flags = ["-std=c++17", "-O2", "-ffp-contract=off", "-Werror=vexing-parse",
                 "-DFAUST_ANALYSIS_OUTPUTS=" + str(len(FIELDS))]
    commands = [[compiler, *flags, DSP, "-o", generated],
                [cpp, *cpp_flags, "-I" + str(out), RUNNER, "-o", binary]]
    for index, command in enumerate(commands):
        try:
            log = run(command)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            (out / f"build-{index}.log").write_text(str(error) + "\n")
            raise
        (out / f"build-{index}.log").write_text(log + "\n")
    provenance = {
        "faust_version": version, "faust_binary_sha256": sha(compiler),
        "release_archive_verified": archive_hash is not None,
        "release_archive_sha256": archive_hash, "library_manifest": manifest,
        "dsp_sha256": sha(DSP), "generated_sha256": sha(generated),
        "runner_source_sha256": sha(RUNNER), "runner_binary_sha256": sha(binary),
        "driver_sha256": sha(__file__), "toolchain_helper_sha256": sha(_toolchain.__file__),
        "cxx_version": run([cpp, "--version"]), "cxx_binary_sha256": sha(cpp),
        "commands": [[str(x) for x in c] for c in commands], "config": profile(),
        "internal_precision": "double", "io_precision": "float32",
        "build_environment": environment(), "purpose": "offline-diagnostics-only",
    }
    (out / "build.json").write_text(json.dumps(provenance, indent=2, sort_keys=True) + "\n")
    return binary, provenance


def environment() -> dict:
    return {"system": platform.system(), "architecture": platform.machine(),
            "python_version": platform.python_version(), "numpy_version": np.__version__,
            "execution_lane": os.environ.get("EXECUTION_LANE", "local-unspecified"),
            "github_sha": os.environ.get("GITHUB_SHA"), "github_run_id": os.environ.get("GITHUB_RUN_ID")}


def validate(src: Path, rate: int, channels: int, stride: int, block: int):
    for name, value, low, high in (("rate", rate, 8000, 192000), ("channels", channels, 1, 32),
                                   ("stride", stride, 1, 192000), ("block", block, 1, 65536)):
        if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
            raise ValueError(f"invalid {name}")
    if rate / 2 <= CONFIG["top_hz"]:
        raise ValueError("sample rate must put the 10000-Hz crossover below Nyquist")
    data = _toolchain.read_input(src, rate, channels)
    if len(data) > MAX_FRAMES or ((len(data) + stride - 1) // stride) * channels > MAX_ROWS:
        raise ValueError("trace resource limit; split the input or increase --stride")
    return data


def band_centers(rate: int) -> list[float]:
    n, m, top = CONFIG["bands"], CONFIG["bands_per_octave"], CONFIG["top_hz"]
    return [float(np.sqrt(top * rate / 2))] + [top * 2 ** ((1 - 2*i) / (2*m))
            for i in range(1, n-1)] + [0.5 * top * 2 ** ((2-n) / m)]


def reduce_window(values: np.ndarray, start: int, rate: int, channel: int) -> dict:
    """Reduce ALL descriptor samples; stride affects storage only, not the DSP."""
    if values.ndim != 2 or values.shape[1] != len(FIELDS) or not len(values):
        raise ValueError("invalid descriptor window shape")
    if not np.isfinite(values).all():
        raise RuntimeError("nonfinite descriptor output")
    end = start + len(values)
    statistics = {field: {"min": float(np.min(values[:, index])),
                          "max": float(np.max(values[:, index])),
                          "mean": float(np.mean(values[:, index], dtype=np.float64)),
                          "last": float(values[-1, index])}
                  for index, field in enumerate(FIELDS[1:], 1)}
    active = values[:, 4] > EPSILON
    return {"type": "window", "channel": channel, "start_frame": start,
            "end_frame_exclusive": end, "last_frame": end - 1,
            "time_seconds": (end - 1) / rate,
            **{field: statistics[field]["last"] for field in FIELDS[1:]},
            "statistics": statistics, "power_above_epsilon_last": bool(active[-1]),
            "power_above_epsilon_samples": int(np.count_nonzero(active)),
            "power_window_filled": end >= round(CONFIG["power_window_seconds"] * rate),
            "flux_history_filled": end >= round(CONFIG["flux_hop_seconds"] * rate)
                                   + int(CONFIG["flux_hop_seconds"] * rate)}


def analyze(src, rate, out, runner, provenance, stride=240, *, channels=1, block=256,
            lab_report=None) -> dict:
    """Write a hashed JSONL sidecar and completion manifest; never edit input/FFT reports."""
    src, out, runner = Path(src).resolve(), Path(out).resolve(), Path(runner).resolve()
    if src == out or out in src.parents:
        raise ValueError("input must be outside the output directory")
    if lab_report and (Path(lab_report).resolve() == out or out in Path(lab_report).resolve().parents):
        raise ValueError("lab report must be outside the output directory")
    out.mkdir(parents=True, exist_ok=True)
    destination = out / "spectral-trace.json"
    destination.unlink(missing_ok=True)  # failed reruns must not leave a success manifest
    data = validate(src, rate, channels, stride, block)
    if provenance.get("config") != CONFIG or provenance.get("dsp_sha256") != sha(DSP):
        raise ValueError("descriptor profile/source provenance mismatch")
    if provenance.get("runner_binary_sha256") != sha(runner):
        raise ValueError("runner binary provenance mismatch")
    frames, input_hash = len(data), sha(src)
    parent = None
    if lab_report:
        path = Path(lab_report).resolve()
        parent_hash = sha(path)
        existing = json.loads(path.read_text())
        matches = [r for r in existing.get("renders", []) if r.get("raw_sha256") == input_hash]
        if not matches or any(r.get("diagnostics", {}).get(k) != v for r in matches
                              for k, v in (("frames", frames), ("rate", rate), ("channels", channels))):
            raise ValueError("lab report has no matching raw hash and dimensions")
        parent = {"path": str(path), "sha256": parent_hash,
                  "render_labels": [r.get("label") for r in matches],
                  "source_commit": existing.get("source_commit"), "unchanged": True}
    windows = 0
    with tempfile.TemporaryDirectory(prefix="spectral-", dir=out) as work:
        work = Path(work)
        trace = work / "trace.jsonl"
        with trace.open("w") as output:
            for channel in range(channels):
                inp, raw = work / "input.f32", work / "output.f32"
                np.asarray(data[:, channel], dtype="<f4").tofile(inp)
                # The IIR/averager has no finite flush: original frames only, tail=0.
                run([runner, inp, raw, rate, frames, block, 0])
                if raw.stat().st_size != frames * len(FIELDS) * 4:
                    raise RuntimeError("descriptor output size mismatch")
                values = np.memmap(raw, dtype="<f4", mode="r", shape=(frames, len(FIELDS)))
                try:
                    for start in range(0, frames, stride):
                        end = min(start + stride, frames)
                        window = values[start:end]
                        if not np.array_equal(window[:, 0], data[start:end, channel]):
                            raise RuntimeError("descriptor passthrough changed input")
                        row = reduce_window(window, start, rate, channel)
                        output.write(json.dumps(row, sort_keys=True, allow_nan=False) + "\n")
                        windows += 1
                    del window
                finally:
                    values._mmap.close()
                raw.unlink()
        if sha(src) != input_hash or (parent and sha(parent["path"]) != parent["sha256"]):
            raise RuntimeError("input or parent report changed during analysis")
        if sha(runner) != provenance["runner_binary_sha256"]:
            raise RuntimeError("runner changed during analysis")
        report = {
            "schema": 2, "complete": True, "descriptor_family": "faust_filterbank_not_fft",
            "input": {"path": str(src), "sha256": input_hash, "rate": rate,
                      "channels": channels, "frames": frames, "format": "interleaved-f32le"},
            "config": CONFIG, "band_centers_hz_high_to_low": band_centers(rate),
            "sampling": {"dsp_cadence_frames": 1, "storage_stride_frames": stride,
                         "storage_stride_seconds": stride / rate, "compute_block_frames": block,
                         "window": "half-open; final partial included; last frame timestamp",
                         "reductions": ["min", "max", "mean", "last"],
                         "power_window_frames": round(CONFIG["power_window_seconds"] * rate),
                         "flux_amplitude_window_frames": round(CONFIG["flux_hop_seconds"] * rate),
                         "flux_delay_frames": int(CONFIG["flux_hop_seconds"] * rate),
                         "reset": "fresh DSP per channel/file", "tail_frames": 0,
                         "channel_policy": "independent mono, no summing/downmix",
                         "startup": "zero state, all frames retained; flags describe window fill only"},
            "trace": {"path": "spectral-trace.jsonl", "sha256": sha(trace), "rows": windows,
                      "ordering": "channel then start_frame", "fields": FIELDS[1:]},
            "provenance": provenance, "execution_environment": environment(), "parent_lab_report": parent,
            "benchmark_eligible": False,
            "notes": ["Filter-bank descriptors, not interchangeable with existing FFT/reference metrics.",
                      "Power averaging is rectangular, not an exponential tau. Flux is amplitude-dependent, not normalized or Hz.",
                      "Centroid/spread at or below double epsilon power are denominator-guarded; consult activity flags, not pitch claims.",
                      "Window-fill flags do not establish IIR settling. End-of-file contains no invented silence or normalization.",
                      "No existing score, metric, audio or release threshold is changed."],
        }
        manifest = work / "manifest.json"
        manifest.write_text(json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n")
        trace.replace(out / "spectral-trace.jsonl")
        manifest.replace(destination)  # publish completion last
    return report


def read_trace(out: Path):
    """Load windows only after validating the manifest, trace hash and row count."""
    out = Path(out)
    report = json.loads((out / "spectral-trace.json").read_text())
    if report.get("schema") != 2 or report.get("complete") is not True:
        raise ValueError("incomplete spectral trace")
    path = out / "spectral-trace.jsonl"
    if sha(path) != report["trace"]["sha256"]:
        raise ValueError("spectral trace hash mismatch")
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    if len(rows) != report["trace"]["rows"]:
        raise ValueError("spectral trace row count mismatch")
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--rate", type=int, required=True)
    parser.add_argument("--channels", type=int, default=1)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--stride", type=int, default=240)
    parser.add_argument("--block", type=int, default=256)
    parser.add_argument("--faust", default=os.environ.get("FAUST", "faust"))
    parser.add_argument("--faust-libraries", default=os.environ.get("FAUST_LIBRARIES"))
    parser.add_argument("--faust-archive", type=Path, default=os.environ.get("FAUST_ARCHIVE"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--lab-report", type=Path)
    args = parser.parse_args()
    try:
        source, out = args.input.resolve(), args.out.resolve()
        if source == out or out in source.parents:
            raise ValueError("input must be outside the output directory")
        if args.lab_report and (out == args.lab_report.resolve() or out in args.lab_report.resolve().parents):
            raise ValueError("lab report must be outside the output directory")
        out.mkdir(parents=True, exist_ok=True)
        (out / "spectral-trace.json").unlink(missing_ok=True)
        validate(source, args.rate, args.channels, args.stride, args.block)
        runner, provenance = build(out / "build", args.faust, args.cxx, args.faust_libraries, args.faust_archive)
        report = analyze(source, args.rate, out, runner, provenance, args.stride,
                         channels=args.channels, block=args.block, lab_report=args.lab_report)
    except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        parser.exit(2, str(error) + "\n")
    print(json.dumps({"report": str(out / "spectral-trace.json"), "windows": report["trace"]["rows"],
                      "complete": True}))


if __name__ == "__main__":
    main()
