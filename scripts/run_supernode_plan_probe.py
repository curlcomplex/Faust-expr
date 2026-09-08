#!/usr/bin/env python3
from pathlib import Path
import heapq, json, os, re, shutil, statistics, subprocess, time

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


def parse_dot(path):
    """Reduce Faust's -sng DOT to a weighted block DAG.

    The upstream dump defines one graphviz cluster per super-node. Its cluster
    label carries the model's operation estimate and pressure. Solid signal
    edges are instantaneous reads; dashed edges are delayed reads. Only solid
    cross-cluster edges are same-sample precedence constraints here.
    """
    text=path.read_text(errors="replace")
    blocks={}
    node_block={}
    current=None
    depth=0
    cluster_re=re.compile(r"subgraph\s+cluster_(\d+)\s*\{")
    label_re=re.compile(r'label="loop\s+(\d+)\s+.*?\s+(\d+)\s+ops\s+.*?pressure\s+(\d+)/(\d+)(.*?)"')
    node_re=re.compile(r'^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\[')
    edge_re=re.compile(r'^\s*([A-Za-z_][A-Za-z0-9_]*)\s*->\s*([A-Za-z_][A-Za-z0-9_]*)(.*)$')

    # Cluster bodies are flat in the upstream writer. Track brace depth anyway
    # so the parser fails conservatively if graphviz formatting grows.
    for raw in text.splitlines():
        m=cluster_re.search(raw)
        if m:
            current=int(m.group(1)); depth=raw.count("{")-raw.count("}")
            blocks.setdefault(current,{"ops":0,"pressure":0,"register_budget":0,"over_pressure":False,"nodes":[]})
            continue
        if current is not None:
            lm=label_re.search(raw)
            if lm:
                bid,ops,pressure,budget,tail=lm.groups(); bid=int(bid)
                if bid != current: raise RuntimeError(f"cluster/label mismatch {current}!={bid}")
                blocks[bid].update(ops=int(ops),pressure=int(pressure),register_budget=int(budget),over_pressure="over-pressure" in tail)
            nm=node_re.match(raw)
            if nm and "->" not in raw:
                n=nm.group(1); node_block[n]=current; blocks[current]["nodes"].append(n)
            depth += raw.count("{")-raw.count("}")
            if depth <= 0: current=None; depth=0

    instantaneous=set(); delayed=set(); unknown=[]
    for raw in text.splitlines():
        em=edge_re.match(raw)
        if not em: continue
        a,b,attrs=em.groups()
        if a not in node_block or b not in node_block:
            unknown.append([a,b]); continue
        ba,bb=node_block[a],node_block[b]
        if ba==bb: continue
        edge=(ba,bb)
        if re.search(r"style\s*=\s*dashed",attrs): delayed.add(edge)
        else: instantaneous.add(edge)

    ids=sorted(blocks)
    preds={b:set() for b in ids}; succ={b:set() for b in ids}
    for a,b in instantaneous:
        succ[a].add(b); preds[b].add(a)

    # Kahn topological order. Signal edge direction in -sng is producer->reader;
    # if that contract changes, a cycle here makes the probe fail rather than
    # silently publishing nonsense.
    indeg={b:len(preds[b]) for b in ids}; ready=sorted([b for b in ids if indeg[b]==0]); topo=[]
    while ready:
        b=ready.pop(0); topo.append(b)
        for c in sorted(succ[b]):
            indeg[c]-=1
            if indeg[c]==0:
                ready.append(c); ready.sort()
    if len(topo)!=len(ids): raise RuntimeError("instantaneous block graph is cyclic or DOT edge contract changed")

    # ASAP depth and an ops-weighted critical path (span).
    depth_by={}; span_to={}
    for b in topo:
        depth_by[b]=0 if not preds[b] else 1+max(depth_by[p] for p in preds[b])
        span_to[b]=blocks[b]["ops"]+(max((span_to[p] for p in preds[b]),default=0))
    total_ops=sum(blocks[b]["ops"] for b in ids)
    span=max(span_to.values(),default=0)
    level_counts={}
    level_ops={}
    for b,d in depth_by.items():
        level_counts[d]=level_counts.get(d,0)+1
        level_ops[d]=level_ops.get(d,0)+blocks[b]["ops"]

    def list_schedule(workers):
        """Greedy precedence schedule using modeled ops as duration.

        Priority is critical-path tail, which avoids the worst obvious choices.
        Returned speedup is a static work-model estimate only.
        """
        tail={}
        for b in reversed(topo):
            tail[b]=blocks[b]["ops"]+max((tail[c] for c in succ[b]),default=0)
        remaining={b:len(preds[b]) for b in ids}
        avail=[(-tail[b],b) for b in ids if remaining[b]==0]; heapq.heapify(avail)
        running=[]; now=0; done=set(); starts={}; finishes={}
        while len(done)<len(ids):
            while avail and len(running)<workers:
                _,b=heapq.heappop(avail); starts[b]=now
                heapq.heappush(running,(now+blocks[b]["ops"],b))
            if not running: raise RuntimeError("list scheduler stalled")
            now=running[0][0]
            finished=[]
            while running and running[0][0]==now:
                _,b=heapq.heappop(running); finished.append(b); finishes[b]=now; done.add(b)
            for b in finished:
                for c in succ[b]:
                    remaining[c]-=1
                    if remaining[c]==0: heapq.heappush(avail,(-tail[c],c))
        return {"workers":workers,"modeled_makespan_ops":now,
                "modeled_speedup":(total_ops/now if now else None),
                "modeled_efficiency":(total_ops/(workers*now) if now else None)}

    ops=[blocks[b]["ops"] for b in ids]
    pressures=[blocks[b]["pressure"] for b in ids]
    return {
        "block_count":len(ids), "materialized_node_count":len(node_block),
        "total_modeled_ops":total_ops,
        "block_ops":{"min":min(ops,default=0),"median":statistics.median(ops) if ops else 0,"max":max(ops,default=0)},
        "pressure":{"max":max(pressures,default=0),"over_pressure_blocks":sum(1 for b in ids if blocks[b]["over_pressure"])},
        "cross_block_instantaneous_edges":len(instantaneous),
        "cross_block_delayed_edges":len(delayed),
        "asap_levels":1+max(depth_by.values(),default=-1),
        "max_blocks_at_asap_level":max(level_counts.values(),default=0),
        "max_ops_at_asap_level":max(level_ops.values(),default=0),
        "modeled_critical_path_ops":span,
        "modeled_unbounded_parallelism":(total_ops/span if span else None),
        "list_schedules":{"2":list_schedule(2),"3":list_schedule(3)},
        "unknown_edge_endpoints":unknown,
        "blocks":{str(b):blocks[b] for b in ids},
        "instantaneous_block_edges":[list(e) for e in sorted(instantaneous)],
        "delayed_block_edges":[list(e) for e in sorted(delayed)],
    }


def fmt(v,digits=3):
    if v is None: return "—"
    if isinstance(v,float): return f"{v:.{digits}f}"
    return str(v)


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
        started=time.monotonic()
        run([faust,"-lang","ocpp",*opts,"-sng","-I",libs,src,"-o",out],f"generate-{label}",timeout=700)
        dot=find_dot(BUILD)
        target=EVID/f"{label}-sn.dot"; shutil.copy2(dot,target); shutil.copy2(out,EVID/f"{label}.cpp")
        analysis=parse_dot(target)
        (EVID/f"{label}-analysis.json").write_text(json.dumps(analysis,indent=2))
        meta["variants"][label]={"options":opts,"compile_seconds":time.monotonic()-started,
                                 "dot_bytes":target.stat().st_size,"cpp_bytes":out.stat().st_size,
                                 "analysis":analysis}
        # Remove graph so the next variant cannot be mistaken for this one.
        for pth in BUILD.rglob("*sn.dot"):
            try: pth.unlink()
            except OSError: pass
    (EVID/"meta.json").write_text(json.dumps(meta,indent=2))

    lines=["# Upstream super-node plan probe","",f"Pinned Faust: `{COMMIT}`","",
           "Same heavy 32-voice DSP as the profitable loop-fusion benchmark. Solid cross-cluster `-sng` edges are treated as same-sample precedence; dashed delayed reads are reported but do not constrain the static block schedule.","",
           "| plan | blocks | modeled ops | critical path ops | unbounded parallelism | max ASAP width | 2-worker model | 3-worker model | max pressure |",
           "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for label in ("ls-df","ls-fuse-cs2"):
        a=meta["variants"][label]["analysis"]
        lines.append(f"| {label} | {a['block_count']} | {a['total_modeled_ops']} | {a['modeled_critical_path_ops']} | {fmt(a['modeled_unbounded_parallelism'])}x | {a['max_blocks_at_asap_level']} | {fmt(a['list_schedules']['2']['modeled_speedup'])}x | {fmt(a['list_schedules']['3']['modeled_speedup'])}x | {a['pressure']['max']} |")
    pre=meta["variants"]["ls-df"]["analysis"]; post=meta["variants"]["ls-fuse-cs2"]["analysis"]
    lines += ["", "## Interpretation guardrails", "",
              "These are compiler-model work/span estimates, **not measured realtime speedups**. They answer whether fusion leaves useful independent regions at all, and whether 2-3 coarse workers are structurally plausible. Runtime overhead, cache effects, worker wakeup, Audio Workgroups, callback deadlines and imbalance still need renderer measurement.","",
              f"Fusion block-count change: `{pre['block_count']} -> {post['block_count']}`. Modeled unbounded parallelism: `{fmt(pre['modeled_unbounded_parallelism'])}x -> {fmt(post['modeled_unbounded_parallelism'])}x`. 3-worker static schedule estimate: `{fmt(pre['list_schedules']['3']['modeled_speedup'])}x -> {fmt(post['list_schedules']['3']['modeled_speedup'])}x`.","",
              "Correctness is screened independently by `run_upstream_loopsplit.py`, which renders every generated variant through `fcplottool` and compares output text against the baseline. Raw DOT, generated C++, per-plan JSON and this report are retained for audit."]
    (EVID/"REPORT.md").write_text("\n".join(lines)+"\n")
    print((EVID/"REPORT.md").read_text(),flush=True)

if __name__=="__main__": main()
