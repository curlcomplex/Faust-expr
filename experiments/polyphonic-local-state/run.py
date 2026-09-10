#!/usr/bin/env python3
from __future__ import annotations
import argparse,hashlib,importlib.util,json,os,random,subprocess,sys,tarfile
from pathlib import Path
HERE=Path(__file__).resolve().parent;REPO=HERE.parents[1]
import prepare
prepare.main()
import generate
sys.path.insert(0,str(HERE.parent/'retained-groups'))
spec=importlib.util.spec_from_file_location('previous_groups',HERE.parent/'retained-groups/run.py');previous=importlib.util.module_from_spec(spec);spec.loader.exec_module(previous)
SHAPES=[('serial',16),('parallel',16),('feedback',8),('memory',8)]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);ap.add_argument('--participants',default='1,2,3');ap.add_argument('--build-jobs',type=int,default=3);a=ap.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    participants=sorted(set(map(int,a.participants.split(','))));assert participants and min(participants)>=1 and max(participants)<=8 and 1<=a.build_jobs<=8
    if 1 not in participants:ap.error("participants must include the single-participant correctness baseline")
    previous.verify_snapshot();records=[]
    def run(cmd,name,timeout=180,check=True):
        r=previous.command(cmd,out/(name+'.log'),timeout=timeout,cwd=REPO);records.append(r);(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
        if check and r['returncode']:raise RuntimeError(name+' failed: preserved log')
        return r
    prefix=Path(subprocess.check_output(['brew','--prefix','faust'],text=True).strip());os.environ['PS_FAUST_PREFIX']=str(prefix)
    header=prefix/'include/faust/dsp/poly-dsp.h';poly=header.read_text();polyhash=hashlib.sha1(b'blob '+str(header.stat().st_size).encode()+b'\0'+header.read_bytes()).hexdigest()
    assert 'class mydsp_poly' in poly and 'voice->computeLegato(count, inputs, fMixBuffer)' in poly
    assert 'sumSquares / (count * numOutputs)' in poly, 'voice RMS contract changed'
    fade=int('fadeIn(count/2, count/2, fMixBuffer);' in poly)
    (out/'tested-poly-dsp.h').write_bytes(header.read_bytes())
    ui_headers={}
    for name in ('MapUI.h','PathBuilder.h'):
        data=(prefix/'include/faust/gui'/name).read_bytes();(out/('tested-'+name)).write_bytes(data);ui_headers[name]=hashlib.sha256(data).hexdigest()
    identity={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),'faust':subprocess.check_output(['faust','--version'],text=True),
        'clang':subprocess.check_output(['clang++','--version'],text=True),'poly_header_git_blob':polyhash,'poly_fade_in':fade,'tracktion_pin':'4536d8a21664fe6ec2aa34b25abc87fa2a0d3b86','participants':participants,'ui_header_sha256':ui_headers,'source_sha256':{}}
    for f in HERE.iterdir():
        if f.is_file():identity['source_sha256'][str(f.relative_to(REPO))]=sha(f)
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    with tarfile.open(out/'executed-source.tar.gz','w:gz') as t:
        for name in identity['source_sha256']:t.add(REPO/name,arcname=name)
    build=HERE/'build';exe=build/'CombinedPolyBench'
    run(['cmake','-S',HERE,'-B',build,'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DFAUST_ROOT='+str(prefix),'-DPOLY_HAS_FADE_IN='+str(fade)],'configure',600)
    run(['cmake','--build',build,'--target','CombinedPolyBench','RetainedPatchingBench','--parallel',a.build_jobs],'build',1600)
    identity['executable_sha256']=sha(exe);(out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    kernels=out/'kernels';kernels.mkdir(exist_ok=True)
    for family,n in SHAPES:
        folder=kernels/f'{family}-{n}';folder.mkdir(exist_ok=True)
        run([exe,'export',family,n,128,8,1,4,folder,folder],f'export-{family}-{n}')
        generate.emit(folder,folder/'graph.json',prefix)
    definitions=[]
    for family,n in SHAPES:
      for block in (64,128):
        for p in participants:definitions.append(('conformance',family,n,block,8,p,4))
    for family,n in SHAPES:
      for block in (64,128):
        for voices in (4,16):
          for p in participants:
            for grain in (1,4):definitions.append(('benchmark',family,n,block,voices,p,grain))
    for family,n in SHAPES:
      for p in sorted(set([1,max(participants)])):
        for mode in ('whole','local','deferred'):definitions.append((mode,family,n,128,16,p,4))
    # First run the dirty-buffer control and a complete one-participant case.
    # Reuse one member of the existing matrix, not a duplicate smoke workflow.
    first=('conformance','serial',16,64,8,1,4)
    assert first in definitions
    rng=random.Random(260910)
    groups=[[first],[d for d in definitions if d[0]=='conformance' and d!=first],
            [d for d in definitions if d[0]=='benchmark'],
            [d for d in definitions if d[0] not in ('conformance','benchmark')]]
    for group in groups[1:]:rng.shuffle(group)
    planned=[d for group in groups for d in group]
    assert len(planned)==len(definitions) and set(planned)==set(definitions)
    (out/'planned-cases.json').write_text(json.dumps(planned,indent=2)+'\n');cases=[]
    for stage,group in enumerate(groups):
        for mode,family,n,block,voices,p,grain in group:
            name=f'{mode}-{family}-{n}-b{block}-v{voices}-p{p}-g{grain}';folder=out/'cases'/name;folder.mkdir(parents=True,exist_ok=True)
            r=previous.command([exe,mode,family,n,block,voices,p,grain,kernels/f'{family}-{n}',folder],out/(name+'.log'),timeout=180,cwd=REPO)
            r.update(name=name,mode=mode,family=family,stages=n,block=block,voices=voices,participants=p,grain=grain);cases.append(r);(out/'cases.json').write_text(json.dumps(cases,indent=2)+'\n');print('COMBINED_CASE',name,r['returncode'],flush=True)
        if stage<=1 and any(c['returncode'] for c in cases):
            reason='Single-participant correctness failed' if stage==0 else 'Polyphonic conformance failed'
            (out/'LATER-STAGES-NOT-RUN.txt').write_text(reason+'; remaining combined stages were not run.\n');break
    # Old engine regressions remain separate: a legacy paced-event miss must not
    # become a fabricated success for either the old or the new engine.
    regressions=[f for f in build.rglob('RetainedPatchingBench') if f.is_file() and os.access(f,os.X_OK)]
    if len(regressions)==1:
        legacy=run([sys.executable,HERE.parent/'persistent-state-next/regression.py',regressions[0],out/'original-regression'],'original-regression',900,check=False);identity['original_regression_exit']=legacy['returncode']
    run([sys.executable,HERE/'verify.py',out],'independent-verification',300)
    print((out/'independent-verification.log').read_text(),flush=True)
    previous.verify_snapshot()
    for name,want in identity['source_sha256'].items():assert sha(REPO/name)==want,name
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    if any(c['returncode'] for c in cases):raise RuntimeError('native cases failed')
if __name__=='__main__':main()
