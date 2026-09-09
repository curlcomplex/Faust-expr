#!/usr/bin/env python3
"""Generate and run adaptive-boundary cases by reusing the tested retained-group executable.

This slice deliberately uses the existing native group engine as the DSP executor.
It generates multiple fixed-layout stages representing one interactive edit sequence,
then reconciles which units are semantically unchanged. Native audio is still rendered
and checked by groups_main; this driver adds boundary/invalidation evidence.
"""
from __future__ import annotations
import argparse, csv, hashlib, json, os, subprocess, sys, time
from pathlib import Path
from test_policy import Policy, PolicyError

ROOT=Path(__file__).resolve().parents[2]
GROUPS=ROOT/'experiments'/'retained-groups'

def run(cmd, cwd, log, timeout=120):
    t=time.monotonic()
    with log.open('w') as f:
        p=subprocess.run([str(x) for x in cmd],cwd=cwd,stdout=f,stderr=subprocess.STDOUT,timeout=timeout)
    if p.returncode: raise RuntimeError(f'{log.name} failed')
    return time.monotonic()-t

def signature(units): return ['-'.join(map(str,u.members)) for u in units]

def semantic_reuse(old,new):
    a=set(signature(old));b=set(signature(new));return sorted(a&b),sorted(a-b),sorted(b-a)

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);args=ap.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    run([sys.executable,'test_policy.py'],Path(__file__).parent,out/'policy-tests.log',30)
    # Preserve the prior native group suite as a regression gate on this exact head.
    regression=run([sys.executable,'run.py','--phase','jit','--output',out/'group-regression'],GROUPS,out/'group-regression.log',900)
    cases=[]
    configs=[('serial',32,4,False,False),('control',16,4,False,False),('nonlinear',16,4,False,False)]
    for family,count,width,feedback,parallel in configs:
        p=Policy(count,width,feedback,parallel); initial=p.units[:]
        # Expose a middle member of group two. This is the expensive structural edit.
        hidden=width+2;p.expose(hidden); opened=p.units[:]
        retained,removed,created=semantic_reuse(initial,opened)
        if len(removed)!=1 or len(created)!=3: raise RuntimeError('non-minimal split')
        undo=p.units[:]
        if p.undo()!='retain-open-boundaries' or p.units!=undo:raise RuntimeError('undo regrouped')
        # Cross-boundary edit must not merge anything.
        before=p.units[:]; mode=p.connect(width,width+1)
        if mode!='external' or p.units!=before:raise RuntimeError('cross-group eager merge')
        cases.append({'family':family,'count':count,'width':width,'hidden_member':hidden,
                      'initial':signature(initial),'opened':signature(opened),
                      'retained':retained,'removed':removed,'created':created,
                      'undo':signature(undo),'cross_group_mode':mode})
    # Explicit negatives remain part of the executed evidence.
    negatives=[]
    for label,make,member in [('feedback',lambda:Policy(8,8,True,False),4),('parallel',lambda:Policy(8,4,False,True),2)]:
        try: make().expose(member)
        except PolicyError: negatives.append(label)
        else: raise RuntimeError(label+' unsafe cut accepted')
    identity={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              'group_engine_sha256':hashlib.sha256((GROUPS/'StableGroups.h').read_bytes()).hexdigest(),
              'group_bench_sha256':hashlib.sha256((GROUPS/'groups_main.cpp').read_bytes()).hexdigest(),
              'regression_seconds':regression,'cases':cases,'negatives':negatives,
              'native_regression':'existing 20 grouped-LLVM cases plus original 46 cases via retained-groups/run.py'}
    (out/'adaptive-summary.json').write_text(json.dumps(identity,indent=2)+'\n')
    print(json.dumps({'cases':len(cases),'negatives':negatives,'native_regression':'passed'},indent=2))

if __name__=='__main__':main()
