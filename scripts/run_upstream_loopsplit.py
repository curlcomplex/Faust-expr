#!/usr/bin/env python3
"""Evaluate upstream Faust loop-split/fusion work before inventing a Curlop partitioner.

Builds a pinned upstream compiler that contains the experimental ocpp -ls family,
generates the same heavy benchmark DSP under several existing upstream strategies,
benchmarks generated C++ with matched compiler flags, and compares deterministic
output. This is compiler/codegen research only: no custom scheduler is introduced.
"""
from __future__ import annotations

from pathlib import Path
import json, os, re, shutil, subprocess, time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "upstream-loopsplit"
EVID = ROOT / "evidence-upstream-loopsplit"
FAUST_REPO = BUILD / "faust"
BENCH_REPO = BUILD / "faustcompilerbenchtool"
FAUST_COMMIT = "3d4baa164c0dd31b7d147617ed84a626495148dd"
DSP = '''import("stdfaust.lib");
voice(i)=os.osc(70+i*3.17):fi.lowpass(4,900+i*7):fi.highpass(2,70+i*2):fi.lowpass(4,1700+i*5):fi.highpass(2,110+i*3):fi.lowpass(4,2600+i*4):fi.lowpass(4,3600+i*3):fi.highpass(2,150+i*2):fi.lowpass(4,5200+i*2):*(0.01);
bank=par(i,32,voice(i)):>_;
process=bank,bank;
'''

VARIANTS = [
    ("ocpp", []),
    ("ls-df", ["-ls", "-ls-sched", "df"]),
    ("ls-model", ["-ls", "-ls-sched", "model"]),
    ("ls-cs2", ["-ls", "-ls-sched", "cs2"]),
    ("ls-cs2b", ["-ls", "-ls-sched", "cs2b"]),
    ("ls-fuse-df", ["-ls-fuse", "-ls-sched", "df"]),
    ("ls-fuse-model", ["-ls-fuse", "-ls-sched", "model"]),
    ("ls-fuse-cs2", ["-ls-fuse", "-ls-sched", "cs2"]),
    ("ls-fuse-cs2b", ["-ls-fuse", "-ls-sched", "cs2b"]),
]


def run(cmd, name, cwd=None, env=None, timeout=600, required=True):
    EVID.mkdir(parents=True, exist_ok=True)
    print("stage=" + name, flush=True)
    t = time.monotonic()
    try:
        p = subprocess.run(list(map(str, cmd)), cwd=cwd or ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, errors="replace", timeout=timeout)
        (EVID / f"{name}.txt").write_text(p.stdout)
        if p.returncode:
            raise RuntimeError(f"{name} exit={p.returncode}: {p.stdout[-4000:]}")
        print(f"stage_done={name} seconds={time.monotonic()-t:.3f}", flush=True)
        return p.stdout
    except Exception as exc:
        (EVID / "FAILURE.txt").write_text(f"{name}: {exc}\n")
        if required:
            raise
        print(f"optional_failure={name}: {exc}", flush=True)
        return ""


def extract_number(text):
    vals = []
    for line in text.splitlines():
        m = re.fullmatch(r"\s*([0-9]+(?:\.[0-9]+)?)\s*", line)
        if m:
            vals.append(float(m.group(1)))
    return vals[-1] if vals else None


def main():
    shutil.rmtree(EVID, ignore_errors=True)
    BUILD.mkdir(parents=True, exist_ok=True)
    EVID.mkdir(parents=True, exist_ok=True)
    source = BUILD / "heavy.dsp"
    source.write_text(DSP)
    shutil.copy2(source, EVID / "heavy.dsp")
    run(["uname", "-a"], "os")
    run(["sysctl", "hw.physicalcpu", "hw.logicalcpu", "machdep.cpu.brand_string"], "cpu")
    run(["clang++", "--version"], "clang-version")

    if not FAUST_REPO.exists():
        run(["git", "clone", "--filter=blob:none", "--no-checkout",
             "https://github.com/grame-cncm/faust.git", FAUST_REPO], "clone-faust", timeout=300)
    run(["git", "fetch", "--depth", "1", "origin", FAUST_COMMIT], "fetch-faust", cwd=FAUST_REPO, timeout=300)
    run(["git", "checkout", "--detach", "FETCH_HEAD"], "checkout-faust", cwd=FAUST_REPO)
    env = os.environ.copy(); env["MAKEFLAGS"] = "-j3"
    run(["make", "compiler"], "build-faust-compiler", cwd=FAUST_REPO, env=env, timeout=900)
    faust = FAUST_REPO / "build" / "bin" / "faust"
    if not faust.exists():
        raise RuntimeError("pinned upstream faust binary not produced")
    help_text = run([faust, "-h"], "upstream-help")
    for token in ("-ls", "-ls-fuse", "-ls-sched"):
        if token not in help_text:
            raise RuntimeError(f"pinned compiler missing expected option {token}")
    run([faust, "-v"], "upstream-version")

    if not BENCH_REPO.exists():
        run(["git", "clone", "--depth", "1", "https://github.com/grame-cncm/faustcompilerbenchtool.git", BENCH_REPO], "clone-benchtool", timeout=300)
    # The wrappers expect their architecture fragments in /usr/local/share/fctool.
    # Install exactly as upstream documents; GitHub's macOS runner permits sudo.
    run(["sudo", "./install.sh"], "install-benchtool", cwd=BENCH_REPO, timeout=120)
    fcbench = Path("/usr/local/bin/fcbenchtool")
    fcplot = Path("/usr/local/bin/fcplottool")
    if not fcbench.exists() or not fcplot.exists():
        raise RuntimeError("installed upstream benchmark wrappers not found")

    results = []
    cxx_env = os.environ.copy()
    cxx_env["CXX"] = "clang++"
    cxx_env["FAUST_LIB_PATH"] = str(FAUST_REPO / "libraries")

    generated = {}
    for label, options in VARIANTS:
        cpp = BUILD / f"{label}.cpp"
        if cpp.exists(): cpp.unlink()
        cmd = [faust, "-lang", "ocpp", *options, "-I", str(FAUST_REPO / "libraries"), source, "-o", cpp]
        run(cmd, f"generate-{label}", timeout=180, required=False)
        if not cpp.exists():
            results.append({"label": label, "options": options, "generated": False})
            continue
        generated[label] = cpp
        shutil.copy2(cpp, EVID / cpp.name)
        row = {"label": label, "options": options, "generated": True,
               "generated_bytes": cpp.stat().st_size,
               "for_loops": len(re.findall(r"\bfor\s*\(", cpp.read_text(errors="replace")))}
        times = []
        for trial in range(3):
            trial_src = BUILD / f"{label}-t{trial}.cpp"
            binary = trial_src.with_suffix("")
            if binary.exists(): binary.unlink()
            shutil.copy2(cpp, trial_src)
            run([fcbench, trial_src], f"bench-build-{label}-t{trial}", cwd=BUILD, env=cxx_env, timeout=180)
            if not binary.exists():
                raise RuntimeError(f"fcbenchtool reported success without producing {binary}")
            text = run([binary, "300"], f"bench-run-{label}-t{trial}", cwd=BUILD, env=cxx_env, timeout=180)
            value = extract_number(text)
            if value is None:
                row.setdefault("unparsed_trials", []).append(trial)
            else:
                times.append(value)
        row["times"] = times
        row["best"] = min(times) if times else None
        results.append(row)

    reference = None
    correctness = []
    for label, cpp in generated.items():
        plot_src = BUILD / f"plot-{label}.cpp"
        binary = plot_src.with_suffix("")
        if binary.exists(): binary.unlink()
        shutil.copy2(cpp, plot_src)
        run([fcplot, plot_src], f"plot-build-{label}", cwd=BUILD, env=cxx_env, timeout=180)
        if not binary.exists():
            raise RuntimeError(f"fcplottool reported success without producing {binary}")
        text = run([binary], f"plot-run-{label}", cwd=BUILD, env=cxx_env, timeout=60)
        path = EVID / f"{label}.ir"
        path.write_text(text)
        if reference is None:
            reference = text
            correctness.append({"label": label, "exact_text": True})
        else:
            correctness.append({"label": label, "exact_text": text == reference})

    baseline = next((r.get("best") for r in results if r["label"] == "ocpp"), None)
    for row in results:
        if baseline and row.get("best"):
            row["speedup_vs_ocpp"] = baseline / row["best"]
    payload = {"faust_commit": FAUST_COMMIT, "variants": results, "correctness": correctness}
    (EVID / "results.json").write_text(json.dumps(payload, indent=2))

    lines = ["# Upstream Faust loop-split/fusion probe", "",
             f"Pinned Faust: `{FAUST_COMMIT}`", "",
             "| Variant | Generated bytes | for-loops | Best benchmark | Speedup vs ocpp |", "|---|---:|---:|---:|---:|"]
    for r in results:
        lines.append(f"| {r['label']} | {r.get('generated_bytes','—')} | {r.get('for_loops','—')} | {r.get('best','—')} | {r.get('speedup_vs_ocpp','—')} |")
    exact = all(c["exact_text"] for c in correctness)
    lines += ["", f"Exact fcplot text match across generated variants: **{exact}**.",
              "", "This probe evaluates existing upstream codegen only. It does not establish LLVM/JIT availability of -ls, realtime safety, or arbitrary Curlop graph partitioning."]
    (EVID / "REPORT.md").write_text("\n".join(lines) + "\n")
    print((EVID / "REPORT.md").read_text(), flush=True)


if __name__ == "__main__":
    main()
