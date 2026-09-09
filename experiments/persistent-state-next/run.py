#!/usr/bin/env python3
"""Tighter performance gate -> continuity -> genuine online compilation, in order."""
from __future__ import annotations
import argparse,hashlib,importlib.util,json,os,shutil,subprocess,sys,tarfile
from pathlib import Path
HERE=Path(__file__).resolve().parent;REPO=HERE.parents[1]
import prepare
prepare.main()
import generate,analyze
sys.path.insert(0,str(HERE.parent/'retained-groups'))
spec=importlib.util.spec_from_file_location('previous_groups',HERE.parent/'retained-groups/run.py');previous=importlib.util.module_from_spec(spec);spec.loader.exec_module(previous)
SHAPES=[('ben',4),('ben',32),('ben',64),('serial',32),('parallel',32),('control',16),('nonlinear',16),('feedback',8),('memory',8)]
def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);args=p.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True);commands=[];previous.verify_snapshot()
    def run(cmd,name,timeout=180):
        r=previous.command(cmd,out/(name+'.log'),timeout=timeout,cwd=REPO);commands.append(r);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        if r['returncode']:raise RuntimeError(name+' failed; exact log retained')
    identity={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),'source_sha256':{}}
    for folder in [HERE,HERE.parent/'persistent-state']:
        for f in folder.iterdir():
            if f.is_file():identity['source_sha256'][str(f.relative_to(REPO))]=hashlib.sha256(f.read_bytes()).hexdigest()
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    with tarfile.open(out/'executed-experiment-source.tar.gz','w:gz') as tf:
        for folder in [HERE,HERE.parent/'persistent-state',HERE.parent/'persistent-state-audit']:
            for f in folder.iterdir():
                if f.is_file():tf.add(f,arcname=str(f.relative_to(REPO)))
    run([sys.executable,HERE/'test_analysis.py'],'evaluator-tests',40)
    prefix=Path(subprocess.check_output(['brew','--prefix','faust'],text=True).strip());os.environ['PS_FAUST_PREFIX']=str(prefix)
    build=HERE/'build';exe=build/'PersistentStateNextBench'
    run(['cmake','-S',HERE,'-B',build,'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DFAUST_ROOT='+str(prefix)],'configure',400)
    run(['cmake','--build',build,'--target','PersistentStateNextBench','RetainedPatchingBench','--parallel','3'],'build',1500)
    identity['executable_sha256']=hashlib.sha256(exe.read_bytes()).hexdigest();(out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    regressions=[f for f in build.rglob('RetainedPatchingBench') if f.is_file() and os.access(f,os.X_OK)];assert len(regressions)==1
    run([sys.executable,HERE/'regression.py',regressions[0],out/'original-regression'],'original-regression',900)
    kernels=out/'kernels';kernels.mkdir(exist_ok=True)
    for family,n in SHAPES:
        folder=kernels/f'{family}-{n}';folder.mkdir(exist_ok=True)
        run([exe,'export',family,n,128,folder,folder],f'export-{family}-{n}')
        generate.emit(folder,folder/'graph.json',prefix)
    gout=out/'gate';gout.mkdir(exist_ok=True);cases=[]
    for family,n in SHAPES:
      for frames in ((64,128,512) if (family,n)==('ben',64) else (64,128)):
        name=f'{family}-{n}-b{frames}';folder=gout/name;folder.mkdir(exist_ok=True)
        r=previous.command([exe,'gate',family,n,frames,kernels/f'{family}-{n}',folder],gout/(name+'.log'),timeout=180,cwd=REPO)
        r.update(name=name,family=family,size=n,frames=frames);cases.append(r);(gout/'cases.json').write_text(json.dumps(cases,indent=2)+'\n');print('OUTPUT_ONLY_GATE',name,r['returncode'],flush=True)
    gate=analyze.gate(gout,cases);identity['gate_positive']=gate['gate_positive']
    print('OUTPUT_ONLY_GATE_SUMMARY',json.dumps({k:gate[k] for k in ['gate_positive','ratio_geomean','ratio_max','failures']}),flush=True)
    for row in gate['cases']:print('OUTPUT_ONLY_COST',row['name'],json.dumps(row['ratio']),json.dumps(row['metrics']),flush=True)
    identity['transitions_passed']=None;identity['online_passed']=None
    if gate['gate_positive']:
        tout=out/'transitions';tout.mkdir(exist_ok=True);tcases=[]
        for c in cases:
            folder=tout/c['name'];folder.mkdir(exist_ok=True)
            r=previous.command([exe,'transition',c['family'],c['size'],c['frames'],kernels/f"{c['family']}-{c['size']}",folder],tout/(c['name']+'.log'),timeout=180,cwd=REPO)
            r.update({k:c[k] for k in ['name','family','size','frames']});tcases.append(r);(tout/'cases.json').write_text(json.dumps(tcases,indent=2)+'\n');print('CONTINUITY',c['name'],r['returncode'],flush=True)
        result=analyze.transitions(tout,tcases);identity['transitions_passed']=result['passed'];print('CONTINUITY_SUMMARY',json.dumps(result),flush=True)
    # Same independent auditor, new exact output contract. Not the benchmark evaluator.
    run([sys.executable,HERE.parent/'persistent-state-audit/audit.py',out,out/'independent'],'independent-static-state-audit',300)
    print((out/'independent-static-state-audit.log').read_text(),flush=True)
    if gate['gate_positive'] and identity['transitions_passed']:
        liveout=out/'live';liveout.mkdir(exist_ok=True);livecases=[]
        for mode in ('live','live-stress'):
          for family,n in [('ben',32),('parallel',32),('feedback',8),('memory',8)]:
            name=f'{mode}-{family}-{n}-b128';folder=liveout/name;folder.mkdir(exist_ok=True)
            r=previous.command([exe,mode,family,n,128,kernels/f'{family}-{n}',folder],liveout/(name+'.log'),timeout=90,cwd=REPO)
            r.update(name=name,mode=mode,family=family,size=n,frames=128);livecases.append(r);(liveout/'live-cases.json').write_text(json.dumps(livecases,indent=2)+'\n');print('ONLINE_COMPILE',name,r['returncode'],flush=True)
            if (folder/'live-result.json').exists():print('ONLINE_RESULT',name,(folder/'live-result.json').read_text(),flush=True)
        run([sys.executable,HERE/'audit_live.py',liveout,out/'independent'],'independent-online-audit',300)
        identity['online_passed']=json.loads((out/'independent/live-independent-audit.json').read_text())['passed'];print((out/'independent-online-audit.log').read_text(),flush=True)
    else:(out/'ONLINE-NOT-RUN.txt').write_text('The preceding gate was not positive; no online compile result claimed.\n')
    previous.verify_snapshot()
    for name,want in identity['source_sha256'].items():
        if hashlib.sha256((REPO/name).read_bytes()).hexdigest()!=want:raise RuntimeError('source changed during run '+name)
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n');print('FINAL_STAGE_STATUS',json.dumps(identity),flush=True)
    if gate['failures'] or identity['transitions_passed'] is False or identity['online_passed'] is False:raise RuntimeError('functional/evidence validation failure')
if __name__=='__main__':main()
