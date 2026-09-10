#!/usr/bin/env python3
"""Matched scheduling-geometry extension of the existing PR45 experiment.
No new DSP, compiler, allocator, numerical tolerance, or workflow.
"""
from __future__ import annotations
import csv, hashlib, json, math, os, random, statistics, subprocess, time
from pathlib import Path
import tail_study as tail
import verify
from policy_evidence import policy_check
HERE=Path(__file__).resolve().parent
BUDGETS=('startup','geometry','periodic')
SHAPES=(('serial',16),('parallel',16),('feedback',8),('memory',8))
def write(path,value):path.write_text(json.dumps(value,indent=2)+'\n')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def execute(exe,kernels,out,c):
    out.mkdir(parents=True,exist_ok=True)
    cmd=[str(exe),c['mode'],c['family'],str(c['stages']),str(c['block']),str(c['voices']),str(c['participants']),str(c['grain']),str(kernels/f"{c['family']}-{c['stages']}"),str(out)]
    env=os.environ.copy();env.update(PS_RT_CONTEXT='workers',PS_RT_BUDGET=c['budget'],PS_JOB_PROBE=str(int(c.get('probe',False))),PS_HANDOFF=c.get('handoff','prepared'),PS_HANDOFF_FAULT='none')
    record=dict(c,command=cmd,environment={k:v for k,v in env.items() if k.startswith('PS_')})
    start=time.monotonic()
    try:
        with (out/'native.log').open('w') as log:r=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=180)
        record['returncode']=r.returncode
    except subprocess.TimeoutExpired:record['returncode']=124
    record['seconds']=time.monotonic()-start
    write(out/'command.json',record)
    print('BUDGET_CASE',c['budget'],c['name'],record['returncode'],flush=True)
    if record['returncode']:
        print((out/'native.log').read_text()[-12000:],flush=True)
        raise RuntimeError('Native scheduling case failed: '+c['name'])
    return record

def native_suite(exe,kernels,out,budget,definitions):
    out.mkdir(parents=True,exist_ok=True);records=[]
    write(out/'planned-cases.json',definitions)
    for mode,family,n,block,voices,p,grain in definitions:
        name=f'{mode}-{family}-{n}-b{block}-v{voices}-p{p}-g{grain}'
        c=dict(mode=mode,family=family,stages=n,block=block,voices=voices,participants=p,grain=grain,budget=budget,name=name)
        record=execute(exe,kernels,out/'cases'/name,c);records.append(record);write(out/'cases.json',records)
    verify.main(out)
    return tail.read(out/'summary.json')

def summarize(out,completed,costs):
    summaries=[]
    for budget in BUDGETS:
      for p in (4,8):
       for handoff in ('prepared','prebuilt'):
        items=[r for r in completed if r['case']['budget']==budget and r['case']['participants']==p and r['case']['handoff']==handoff]
        assert len(items)==8,(budget,p,handoff,len(items))
        callbacks=sum(r['audit']['all_callback_us']['count'] for r in items)
        over=sum(sum(v['callback_over_budget'] for v in r['audit']['phases'].values()) for r in items)
        summaries.append(dict(budget=budget,participants=p,handoff=handoff,cases=len(items),callbacks=callbacks,over_budget=over,over_budget_pct=100*over/callbacks,deadline_misses=sum(r['audit']['all_deadline_misses'] for r in items),maximum_callback_us=max(r['audit']['all_callback_us']['maximum'] for r in items),maximum_output_interval_ms=max(r['audit']['maximum_output_interval_ms'] for r in items),median_case_median_us=statistics.median(r['audit']['all_callback_us']['median'] for r in items)))
    throughput=[]
    if costs:
      for budget in BUDGETS:
       for p in (1,2,4,8):
        rows=[r for r in costs[budget]['cases'] if r['participants']==p]
        gm=lambda key:math.exp(statistics.mean(math.log(r[key]) for r in rows))
        throughput.append(dict(budget=budget,participants=p,cases=len(rows),speedup_vs_stock=gm('optimized_pool_speedup_vs_stock_llvm'),speedup_vs_shared_serial=gm('pool_speedup_vs_same_shared_serial'),local_over_whole=gm('local_over_whole_fallback')))
    result=dict(passed=True,online_cases=len(completed),throughput_cases=sum(len(v['cases']) for v in costs.values()),throughput_execution='executed' if costs else 'not rerun; retain separately identified prior evidence',additional_conformance_cases=64,online=summaries,throughput=throughput,device_acceptance=False)
    write(out/'summary.json',result)
    for name,rows in [('online',summaries),('throughput',throughput)]:
        if not rows:continue
        with (out/(name+'.csv')).open('w') as f:
            w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    print('BUDGET_SUMMARY',json.dumps(result),flush=True)
    return result

def run(exe,kernels,out,base_cases):
    out.mkdir(parents=True,exist_ok=True)
    online_only=os.environ.get('PS_BUDGET_ONLINE_ONLY','0')=='1'
    identity=dict(executable_sha256=sha(exe),source_sha256={p.name:sha(p) for p in HERE.iterdir() if p.is_file()},baseline='d473fbd66164b3421be67b5b68e7c0db384713aa',caller_policy='held at 512 frames /44100Hz in all worker-only arms',online_only=online_only)
    write(out/'identity.json',identity)
    # The baseline conformance has already passed in run.py. Reuse precisely
    # its existing inventory, complete one-participant serial case first.
    defs=[tuple(c[k] for k in ('mode','family','stages','block','voices','participants','grain')) for c in base_cases]
    assert len(defs)==32 and defs[0]==('conformance','serial',16,64,8,1,4)
    for b in BUDGETS[1:]:native_suite(exe,kernels,out/b/'conformance',b,defs)
    costs={}
    if not online_only:
        # Requalify against genuine stock whole-Faust LLVM, identical graph
        # families and nine trials. Grain4 only; no grain1 claim.
        definitions=[('benchmark',f,n,block,v,p,4) for f,n in SHAPES for block in (64,128) for v in (4,16) for p in (1,2,4,8)]
        shuffled=[(b,d) for b in BUDGETS for d in definitions];random.Random(261112).shuffle(shuffled)
        records={b:[] for b in BUDGETS}
        for b in BUDGETS:
            folder=out/b/'throughput';folder.mkdir(parents=True,exist_ok=True);write(folder/'planned-cases.json',definitions)
        for b,d in shuffled:
            mode,f,n,block,v,p,g=d;name=f'{mode}-{f}-{n}-b{block}-v{v}-p{p}-g{g}';c=dict(mode=mode,family=f,stages=n,block=block,voices=v,participants=p,grain=g,budget=b,name=name)
            folder=out/b/'throughput';records[b].append(execute(exe,kernels,folder/'cases'/name,c));write(folder/'cases.json',records[b])
        for b in BUDGETS:
            folder=out/b/'throughput';verify.main(folder);costs[b]=tail.read(folder/'summary.json')
    else:
        print('THROUGHPUT_NOT_RERUN: earlier immutable d473fbd results remain separate evidence',flush=True)
    planned=[]
    for repeat in range(2):
      for family,n in SHAPES[:2]:
       for p in (4,8):
        for policy in ('local','deferred'):
         for handoff in ('prepared','prebuilt'):
          for budget in BUDGETS:
           planned.append(dict(mode=policy,policy=policy,family=family,stages=n,block=128,voices=16,participants=p,grain=4,handoff=handoff,repeat=repeat,probe=True,context='workers',fault='none',budget=budget))
    random.Random(261113).shuffle(planned);assert len(planned)==96
    write(out/'planned-online.json',planned);completed=[]
    for index,c in enumerate(planned):
        c['name']=f"{index:03d}-{c['budget']}-{c['family']}-p{c['participants']}-{c['policy']}-{c['handoff']}-r{c['repeat']}"
        folder=out/'online'/c['name'];record=execute(exe,kernels,folder,c)
        record=dict(case=c,run=record,audit=tail.audit(folder,c),policy_readback=policy_check(folder,c));completed.append(record);write(out/'online-cases.json',completed)
    assert sha(exe)==identity['executable_sha256']
    for name,want in identity['source_sha256'].items():assert sha(HERE/name)==want,name
    summarize(out,completed,costs)
if __name__=='__main__':raise SystemExit('Use run.py --budget-study through the existing physical controller')
