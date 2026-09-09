#!/usr/bin/env python3
"""Actual engine extension: qualify the state ABI cost before live transitions."""
from __future__ import annotations
import argparse,hashlib,importlib.util,json,os,subprocess,sys
from pathlib import Path
import generate,analyze
HERE=Path(__file__).resolve().parent;REPO=HERE.parents[1]
# Reuse preceding isolated-process timeout/log wrapper and immutable-source checks.
spec=importlib.util.spec_from_file_location('previous',HERE.parent/'retained-groups/run.py');previous=importlib.util.module_from_spec(spec)
sys.path.insert(0,str(HERE.parent/'retained-groups'));spec.loader.exec_module(previous)
SHAPES=[('ben',4),('ben',32),('ben',64),('serial',32),('parallel',32),('control',16),('nonlinear',16),('feedback',8),('memory',8)]
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);a=ap.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);previous.verify_snapshot();commands=[]
    def run(cmd,name,timeout=180):
        row=previous.command(cmd,out/(name+'.log'),timeout=timeout,cwd=REPO);commands.append(row);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        if row['returncode']:raise RuntimeError(name+' failed; log preserved')
    run([sys.executable,HERE/'test_analysis.py'],'analysis-tests',30)
    prefix=Path(subprocess.check_output(['brew','--prefix','faust'],text=True).strip());build=HERE/'build';exe=build/'PersistentStateBench'
    run(['cmake','-S',HERE,'-B',build,'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DFAUST_ROOT='+str(prefix)],'configure',400)
    run(['cmake','--build',build,'--target','PersistentStateBench','RetainedPatchingBench','--parallel','3'],'build',1500)
    identity=dict(head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),executable=hashlib.sha256(exe.read_bytes()).hexdigest(),new_source_hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in HERE.glob('*') if p.is_file()})
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    kernels=out/'kernels';kernels.mkdir(exist_ok=True)
    for family,n in SHAPES:
        folder=kernels/f'{family}-{n}';folder.mkdir(exist_ok=True)
        run([exe,'export',family,n,128,folder,folder],f'export-{family}-{n}')
        generate.emit(folder,folder/'graph.json',prefix)
    cases=[];gateout=out/'gate';gateout.mkdir(exist_ok=True)
    for family,n in SHAPES:
      for frames in (64,128) if family!='ben' or n!=64 else (64,128,512):
        name=f'{family}-{n}-b{frames}';folder=gateout/name;folder.mkdir(exist_ok=True)
        row=previous.command([exe,'gate',family,n,frames,kernels/f'{family}-{n}',folder],gateout/(name+'.log'),timeout=180,cwd=REPO)
        row.update(name=name,family=family,size=n,frames=frames);cases.append(row);(gateout/'cases.json').write_text(json.dumps(cases,indent=2));print('GATE',name,row['returncode'],flush=True)
    result=analyze.gate(gateout,cases)
    # Successful child exit is not enough to proceed: numerical and cost gates count.
    if result['gate_positive']:
        tout=out/'transitions';tout.mkdir(exist_ok=True);tcases=[]
        for c in cases:
            folder=tout/c['name'];folder.mkdir(exist_ok=True)
            row=previous.command([exe,'transition',c['family'],c['size'],c['frames'],kernels/f"{c['family']}-{c['size']}",folder],tout/(c['name']+'.log'),timeout=180,cwd=REPO)
            row.update({k:c[k] for k in ('name','family','size','frames')});tcases.append(row);(tout/'cases.json').write_text(json.dumps(tcases,indent=2));print('TRANSITION',c['name'],row['returncode'],flush=True)
        identity['transitions_passed']=analyze.transitions(tout,tcases)['passed']
    else:
        identity['transitions_passed']=None;(out/'NEXT-GATE-NOT-RUN.txt').write_text('Cost/correctness screen did not pass. No state-transition success is claimed.\n')
    previous.verify_snapshot();identity['gate_positive']=result['gate_positive'];(out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    print(json.dumps(identity,indent=2))
    if result['failures'] or identity['transitions_passed'] is False:raise RuntimeError('native correctness/evaluation failure')
    # A negative cost screen is valid diagnostic evidence, not a passing product result.
if __name__=='__main__':main()
