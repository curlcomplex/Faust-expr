#!/usr/bin/env python3
"""Run #33 + #34 LLVM regressions and evolving-boundary native tests."""
from __future__ import annotations
import argparse,hashlib,importlib.util,json,os,platform,random,subprocess,sys
from pathlib import Path
import analyze_adaptive
HERE=Path(__file__).resolve().parent;REPO=HERE.parents[1];PREVIOUS=HERE.parent/'retained-groups';SNAPSHOT=REPO/'vendor/curlop-latency'
HARNESS=SNAPSHOT/'scripts/bench/patching_latency';sys.path.insert(0,str(HARNESS));sys.path.insert(0,str(PREVIOUS))
import analyze_retained,analyze_groups
spec=importlib.util.spec_from_file_location('previous_runner',PREVIOUS/'run.py');previous=importlib.util.module_from_spec(spec);spec.loader.exec_module(previous)
command=previous.command

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);ap.add_argument('--reuse-build',action='store_true');a=ap.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);previous.verify_snapshot();records=[]
    identity=dict(head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),platform=platform.platform(),machine=platform.machine(),source_sha256={})
    for folder in (HERE,PREVIOUS):
        for p in folder.glob('*'):
            if p.is_file():identity['source_sha256'][str(p.relative_to(REPO))]=sha(p)
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    def run(cmd,name,timeout):
        row=command(cmd,out/(name+'.log'),timeout=timeout,cwd=REPO);records.append(row);(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
        if row['returncode']:raise RuntimeError(name+' failed; preserved log')
    build=HERE/'build';exe=build/'AdaptiveGroupBench'
    if not a.reuse_build:
        run([sys.executable,HERE/'test_analysis.py'],'analysis-tests',30)
        prefix=subprocess.check_output(['brew','--prefix','faust'],text=True).strip()
        run(['cmake','-S',HERE,'-B',build,'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DFAUST_ROOT='+prefix],'configure',400)
        run(['cmake','--build',build,'--target','AdaptiveGroupBench','RetainedPatchingBench','--parallel','3'],'build',1500)
    identity['adaptive_executable_sha256']=sha(exe);identity['original_executable_sha256']=sha(build/'previous/host/RetainedPatchingBench')
    os.environ['CURLOP_FAUST_BACKEND']='jit'
    # Invoke exact previous test functions, not rewritten substitutes.
    original=out/'original-46';original.mkdir(exist_ok=True);oldcases=[]
    for mode,family,n,frames in analyze_retained.definitions():
        name=f'{mode}-{family}-{n}-f{frames}';folder=original/name;folder.mkdir(exist_ok=True)
        row=command([build/'previous/host/RetainedPatchingBench',mode,family,n,frames,folder],original/(name+'.log'),timeout=90,cwd=REPO)
        row.update(name=name,mode=mode,family=family,size=n,frames=frames);oldcases.append(row);(original/'retained-cases.json').write_text(json.dumps(oldcases,indent=2))
    identity['original_passed']=analyze_retained.analyze(original)['passed']
    group=out/'previous-groups-20';group.mkdir(exist_ok=True);groupcases=[]
    for mode,family,n,w,frames in analyze_groups.definitions():
        name=f'{mode}-{family}-{n}-g{w}-b{frames}';folder=group/name;folder.mkdir(exist_ok=True)
        row=command([exe,'previous-groups',mode,family,n,w,frames,folder,'none'],group/(name+'.log'),timeout=120,cwd=REPO)
        row.update(name=name,mode=mode,family=family,size=n,width=w,frames=frames);groupcases.append(row);(group/'cases.json').write_text(json.dumps(groupcases,indent=2))
    identity['previous_group_passed']=analyze_groups.analyze(group,'jit')['passed']
    cases=[];todo=analyze_adaptive.definitions();random.Random(44).shuffle(todo)
    for mode,family,n,w,frames in todo:
        name=f'{mode}-{family}-{n}-g{w}-b{frames}';folder=out/name;folder.mkdir(exist_ok=True)
        row=command([exe,mode,family,n,w,frames,folder],out/(name+'.log'),timeout=180,cwd=REPO)
        row.update(name=name,mode=mode,family=family,size=n,width=w,frames=frames,load_average=os.getloadavg());cases.append(row)
        (out/'cases.json').write_text(json.dumps(cases,indent=2)+'\n');print(name,'exit',row['returncode'],flush=True)
    summary=analyze_adaptive.analyze(out);identity['adaptive_passed']=summary['passed'];previous.verify_snapshot()
    for name,want in identity['source_sha256'].items():
        if sha(REPO/name)!=want:raise RuntimeError('source changed during execution')
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    if not all(identity[x] for x in ('original_passed','previous_group_passed','adaptive_passed')):raise RuntimeError('native regression or adaptive case failed')
    print('PASS 46 original, 20 grouped-LLVM, 26 adaptive native cases',flush=True)
if __name__=='__main__':main()
