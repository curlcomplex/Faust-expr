#!/usr/bin/env python3
from pathlib import Path
import json, os, shutil, subprocess, time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "supernode-plan"
EVID = ROOT / "evidence-supernode-plan"
FAUST = BUILD / "faust"
COMMIT = "3d4baa164c0dd31b7d147617ed84a626495148dd"
DSP = '''import("stdfaust.lib");
voice(i)=os.osc(70+i*3.17):fi.lowpass(4,900+i*7):fi.highpass(2,70+i*2):fi.lowpass(4,1700+i*5):fi.highpass(2,110+i*3):fi.lowpass(4,2600+i*4):fi.lowpass(4,3600+i*3):fi.highpass(2,150+i*2):fi.lowpass(4,5200+i*2):*(0.01);
bank=par(i,32,voice(i)):>_;
process=bank,bank;
'''

def run(cmd, name, cwd=None, timeout=900):
    print(f"stage={name}", flush=True); t=time.monotonic()
    p=subprocess.run([str(x) for x in cmd], cwd=cwd or ROOT, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, text=True, errors="replace", timeout=timeout)
    (EVID/f"{name}.txt").write_text(p.stdout)
    if p.returncode: raise RuntimeError(f"{name} exit={p.returncode}: {p.stdout[-4000:]}")
    print(f"stage_done={name} seconds={time.monotonic()-t:.3f}", flush=True)
    return p.stdout

def find_dot(start):
    dots=list(start.rglob("*-sn.dot"))
    if not dots: dots=list(start.rglob("*sn.dot"))
    if not dots: raise RuntimeError("-sng did not produce a super-node DOT file")
    return max(dots, key=lambda p:p.stat().st_mtime_ns)

def main():
    shutil.rmtree(BUILD, ignore_errors=True); shutil.rmtree(EVID, ignore_errors=True)
    BUILD.mkdir(parents=True); EVID.mkdir(parents=True)
    src=BUILD/"heavy.dsp"; src.write_text(DSP); shutil.copy2(src,EVID/"heavy.dsp")
    run(["uname","-a"],"os"); run(["sysctl","hw.physicalcpu","hw.logicalcpu","machdep.cpu.brand_string"],"cpu")
    run(["git","clone","--filter=blob:none","--no-checkout","https://github.com/grame-cncm/faust.git",FAUST],"clone-faust",timeout=300)
    run(["git","fetch","--depth","1","origin",COMMIT],"fetch-faust",cwd=FAUST,timeout=300)
    run(["git","checkout","--detach","FETCH_HEAD"],"checkout-faust",cwd=FAUST)
    env=os.environ.copy(); env["MAKEFLAGS"]="-j3"
    print("stage=build-faust",flush=True); t=time.monotonic()
    p=subprocess.run(["make","compiler"],cwd=FAUST,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors="replace",timeout=900)
    (EVID/"build-faust.txt").write_text(p.stdout)
    if p.returncode: raise RuntimeError("Faust build failed: "+p.stdout[-4000:])
    print(f"stage_done=build-faust seconds={time.monotonic()-t:.3f}",flush=True)
    faust=FAUST/"build/bin/faust"; libs=FAUST/"libraries"
    run([faust,"-v"],"faust-version")
    variants=[("ls-df",["-ls","-ls-sched","df"]),("ls-fuse-cs2",["-t","600","-ls-fuse","-ls-sched","cs2"])]
    meta={"faust_commit":COMMIT,"variants":{}}
    for label,opts in variants:
        out=BUILD/f"{label}.cpp"
        before=set(BUILD.rglob("*sn.dot"))
        started=time.monotonic()
        run([faust,"-lang","ocpp",*opts,"-sng","-I",libs,src,"-o",out],f"generate-{label}",timeout=700)
        dot=find_dot(BUILD)
        target=EVID/f"{label}-sn.dot"; shutil.copy2(dot,target); shutil.copy2(out,EVID/f"{label}.cpp")
        meta["variants"][label]={"options":opts,"compile_seconds":time.monotonic()-started,"dot_bytes":target.stat().st_size,"cpp_bytes":out.stat().st_size}
        # remove graph so the next variant cannot be mistaken for this one
        for pth in BUILD.rglob("*sn.dot"):
            try: pth.unlink()
            except OSError: pass
    (EVID/"meta.json").write_text(json.dumps(meta,indent=2))
    (EVID/"REPORT.md").write_text("# Upstream super-node plan probe\n\nPinned Faust: `"+COMMIT+"`\n\nGenerated raw pre-fusion (`-ls`) and profitable post-fusion (`-ls-fuse -ls-sched cs2`) `-sng` DOT plans for the same heavy 32-voice DSP. This probe intentionally preserves raw DOT/C++ for independent analysis; no Curlop scheduler or custom partitioner is introduced.\n")
    print((EVID/"REPORT.md").read_text(),flush=True)

if __name__=="__main__": main()
