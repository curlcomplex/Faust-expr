#!/usr/bin/env python3
"""Execute the existing retained-state benchmark, not a substitute DSP fixture."""
from __future__ import annotations
import argparse, hashlib, importlib.util, json, os, platform, random, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent
REPO=ROOT.parents[1]
SNAPSHOT=REPO/'vendor/curlop-latency'
HARNESS=SNAPSHOT/'scripts/bench/patching_latency'
sys.path.insert(0,str(HARNESS))
spec=importlib.util.spec_from_file_location('baseline_run',HARNESS/'run.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
command=module.command
import analyze_retained

def verify():
    manifest=json.loads((SNAPSHOT/'SOURCE-MANIFEST.json').read_text())
    for relative,entry in manifest['files'].items():
        path=Path(relative)
        if path.is_absolute() or '..' in path.parts:raise ValueError('unsafe source path')
        file=SNAPSHOT/path
        if file.is_symlink() or hashlib.sha256(file.read_bytes()).hexdigest()!=entry['sha256']:
            raise ValueError('source identity mismatch: '+relative)
    return manifest

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);args=ap.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    manifest=verify();records=[]
    identity={'public_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),
              'product_source_commit':manifest['product_source_commit'],'benchmark_source_commit':manifest['benchmark_source_commit'],
              'platform':platform.platform(),'machine':platform.machine(),'execution':'not_started'}
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    def checked(cmd,name,timeout):
        result=command(cmd,out/(name+'.log'),timeout=timeout,cwd=REPO);records.append(result)
        (out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
        if result['returncode']:raise RuntimeError(name+' failed; retained log')
    checked([sys.executable,HARNESS/'test_retained_analysis.py'],'retained-evaluator-tests',30)
    prefix=subprocess.check_output(['brew','--prefix','faust'],text=True).strip()
    checked(['cmake','-S',ROOT,'-B',ROOT/'build','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DFAUST_ROOT='+prefix],'configure',360)
    checked(['cmake','--build',ROOT/'build','--target','RetainedPatchingBench','--parallel','3'],'build',900)
    exe=ROOT/'build/RetainedPatchingBench';identity['executable_sha256']=hashlib.sha256(exe.read_bytes()).hexdigest()
    verify();os.environ['CURLOP_FAUST_BACKEND']='jit'
    todo=analyze_retained.definitions()
    if len(todo)!=46:raise RuntimeError('benchmark coverage changed unexpectedly')
    random.Random(20260908).shuffle(todo);cases=[]
    identity['execution']='in_progress';(out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    for mode,family,n,frames in todo:
        name=f'{mode}-{family}-{n}-f{frames}';folder=out/name;folder.mkdir(exist_ok=True)
        row=command([exe,mode,family,n,frames,folder],out/(name+'.log'),timeout=90,cwd=REPO)
        row.update(name=name,mode=mode,family=family,size=n,frames=frames,load_average=os.getloadavg())
        cases.append(row);(out/'retained-cases.json').write_text(json.dumps(cases,indent=2)+'\n')
        print(name,'exit',row['returncode'],flush=True)
    result=analyze_retained.analyze(out)
    identity.update(execution='completed',passed=result['passed'],case_count=len(cases),production_acceptance=False)
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    if not result['passed']:raise RuntimeError('native/verification failures; inspect retained-summary.json')
    print('46 native cases passed; not GUI/Core Audio acceptance',flush=True)
if __name__=='__main__':main()
