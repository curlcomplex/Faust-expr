#!/usr/bin/env python3
"""Focused evaluation of upstream Faust loop-split fusion.

The broad first pass established plain -ls performance and that -ls-fuse exceeds
Faust's default 120 s compiler timeout on the heavy DSP. This follow-up raises the
explicit upstream timeout and measures whether the fused code is worth that compile
cost. No custom partitioner or scheduler is introduced.
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
# df is the default upstream loop order. cs2 is included as one materially
# different scheduling control; the broad pass showed non-fused df/model/cs2/cs2b
# were essentially tied (~41.7-41.9 ms versus ~31.3 ms plain ocpp).
VARIANTS = [
    ("ocpp", []),
    ("ls-fuse-df", ["-t", "600", "-ls-fuse", "-ls-sched", "df"]),
    ("ls-fuse-cs2", ["-t", "600", "-ls-fuse", "-ls-sched", "cs2"]),
]


def run(cmd, name, cwd=None, env=None, timeout=720, required=True):
    EVID.mkdir(parents=True, exist_ok=True)
    print("stage=" + name, flush=True); t = time.monotonic()
    try:
        p = subprocess.run(list(map(str, cmd)), cwd=cwd or ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, errors="replace", timeout=timeout)
        (EVID / f"{name}.txt").write_text(p.stdout)
        if p.returncode:
            raise RuntimeError(f"{name} exit={p.returncode}: {p.stdout[-4000:]}")
        elapsed = time.monotonic()-t
        print(f"stage_done={name} seconds={elapsed:.3f}", flush=True)
        return p.stdout, elapsed
    except Exception as exc:
        (EVID / "FAILURE.txt").write_text(f"{name}: {exc}\n")
        if required: raise
        print(f"optional_failure={name}: {exc}", flush=True)
        return "", time.monotonic()-t


def bench_value(text):
    # Current fcbenchtool prints '<binary> 31.2 ms'. Parse the final ms value.
    m = re.search(r"([0-9]+(?:\.[0-9]+)?)\s*ms\s*$", text.strip())
    return float(m.group(1)) if m else None


def main():
    shutil.rmtree(EVID, ignore_errors=True); BUILD.mkdir(parents=True, exist_ok=True); EVID.mkdir(parents=True)
    source = BUILD / "heavy.dsp"; source.write_text(DSP); shutil.copy2(source, EVID / "heavy.dsp")
    run(["uname", "-a"], "os"); run(["sysctl", "hw.physicalcpu", "hw.logicalcpu", "machdep.cpu.brand_string"], "cpu")
    run(["clang++", "--version"], "clang-version")
    if not FAUST_REPO.exists():
        run(["git", "clone", "--filter=blob:none", "--no-checkout", "https://github.com/grame-cncm/faust.git", FAUST_REPO], "clone-faust", timeout=300)
    run(["git", "fetch", "--depth", "1", "origin", FAUST_COMMIT], "fetch-faust", cwd=FAUST_REPO, timeout=300)
    run(["git", "checkout", "--detach", "FETCH_HEAD"], "checkout-faust", cwd=FAUST_REPO)
    env = os.environ.copy(); env["MAKEFLAGS"] = "-j3"
    run(["make", "compiler"], "build-faust-compiler", cwd=FAUST_REPO, env=env, timeout=900)
    faust = FAUST_REPO / "build" / "bin" / "faust"
    if not faust.exists(): raise RuntimeError("pinned upstream faust binary not produced")
    help_text, _ = run([faust, "-h"], "upstream-help")
    for token in ("-ls", "-ls-fuse", "-ls-sched", "--timeout"):
        if token not in help_text: raise RuntimeError(f"pinned compiler missing expected option {token}")
    run([faust, "-v"], "upstream-version")
    if not BENCH_REPO.exists():
        run(["git", "clone", "--depth", "1", "https://github.com/grame-cncm/faustcompilerbenchtool.git", BENCH_REPO], "clone-benchtool", timeout=300)
    run(["sudo", "./install.sh"], "install-benchtool", cwd=BENCH_REPO, timeout=120)
    fcbench, fcplot = Path("/usr/local/bin/fcbenchtool"), Path("/usr/local/bin/fcplottool")
    if not fcbench.exists() or not fcplot.exists(): raise RuntimeError("installed benchmark wrappers missing")
    cxx_env = os.environ.copy(); cxx_env["CXX"] = "clang++"; cxx_env["FAUST_LIB_PATH"] = str(FAUST_REPO / "libraries")

    results, generated = [], {}
    for label, options in VARIANTS:
        cpp = BUILD / f"{label}.cpp"
        if cpp.exists(): cpp.unlink()
        _, gen_seconds = run([faust, "-lang", "ocpp", *options, "-I", str(FAUST_REPO / "libraries"), source, "-o", cpp],
                             f"generate-{label}", timeout=650, required=False)
        row = {"label": label, "options": options, "compile_seconds": gen_seconds, "generated": cpp.exists()}
        # A timed-out Faust may leave a partial file. Require a closing class and
        # successful benchmark compile before calling it generated code.
        if not cpp.exists() or "class mydsp" not in cpp.read_text(errors="replace"):
            row["generated"] = False; results.append(row); continue
        generated[label] = cpp; shutil.copy2(cpp, EVID / cpp.name)
        text = cpp.read_text(errors="replace")
        row.update(generated_bytes=cpp.stat().st_size, for_loops=len(re.findall(r"\bfor\s*\(", text)))
        times=[]
        # Two independent native compilations are enough for this go/no-go follow-up;
        # the broad run already established baseline variance.
        for trial in range(2):
            trial_src=BUILD/f"{label}-t{trial}.cpp"; binary=trial_src.with_suffix("")
            if binary.exists(): binary.unlink()
            shutil.copy2(cpp, trial_src)
            build_log,_=run([fcbench, trial_src], f"bench-build-{label}-t{trial}", cwd=BUILD, env=cxx_env, timeout=240, required=False)
            if not binary.exists():
                row.setdefault("bench_compile_failures",[]).append(trial); continue
            out,_=run([binary,"200"], f"bench-run-{label}-t{trial}", cwd=BUILD, env=cxx_env, timeout=180)
            value=bench_value(out)
            if value is not None: times.append(value)
        row["times_ms"]=times; row["best_ms"]=min(times) if times else None; results.append(row)

    reference=None; correctness=[]
    for label, cpp in generated.items():
        plot_src=BUILD/f"plot-{label}.cpp"; binary=plot_src.with_suffix("")
        if binary.exists(): binary.unlink()
        shutil.copy2(cpp, plot_src)
        run([fcplot,plot_src],f"plot-build-{label}",cwd=BUILD,env=cxx_env,timeout=240,required=False)
        if not binary.exists(): correctness.append({"label":label,"available":False}); continue
        text,_=run([binary],f"plot-run-{label}",cwd=BUILD,env=cxx_env,timeout=60)
        (EVID/f"{label}.ir").write_text(text)
        if reference is None: reference=text
        correctness.append({"label":label,"available":True,"exact_text":text==reference})

    baseline=next((r.get("best_ms") for r in results if r["label"]=="ocpp"),None)
    for row in results:
        if baseline and row.get("best_ms"): row["speedup_vs_ocpp"]=baseline/row["best_ms"]
    payload={"faust_commit":FAUST_COMMIT,"variants":results,"correctness":correctness,
             "broad_pass":{"ocpp_best_ms":31.2836,"ls_df_best_ms":41.749,"ls_model_best_ms":41.8578,
                           "ls_cs2_best_ms":41.7355,"ls_cs2b_best_ms":41.8337,
                           "fusion_hit_default_timeout_seconds":120}}
    (EVID/"results.json").write_text(json.dumps(payload,indent=2))
    lines=["# Focused upstream Faust loop-split fusion probe","",f"Pinned Faust: `{FAUST_COMMIT}`","",
           "Broad pass: plain `-ls` was ~33% slower than `ocpp`; `-ls-fuse` hit Faust's documented 120 s default compile timeout.","",
           "| Variant | compile s | generated bytes | loops | best ms | speedup vs ocpp |","|---|---:|---:|---:|---:|---:|"]
    for r in results:
        lines.append(f"| {r['label']} | {r.get('compile_seconds','—'):.3f} | {r.get('generated_bytes','—')} | {r.get('for_loops','—')} | {r.get('best_ms','—')} | {r.get('speedup_vs_ocpp','—')} |")
    lines += ["", "Correctness screening: `"+json.dumps(correctness)+"`", "",
              "This evaluates existing upstream ocpp codegen only; it does not establish LLVM/JIT support or production suitability."]
    (EVID/"REPORT.md").write_text("\n".join(lines)+"\n"); print((EVID/"REPORT.md").read_text(),flush=True)

if __name__=="__main__": main()
