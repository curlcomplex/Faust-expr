#!/usr/bin/env python3
"""Bounded extension of the public engine harness; no private accesses."""
from __future__ import annotations
import argparse,hashlib,importlib.util,json,os,platform,random,subprocess,sys
from pathlib import Path
import analyze_groups as analyze
HERE=Path(__file__).resolve().parent;REPO=HERE.parents[1];SNAPSHOT=REPO/'vendor/curlop-latency'
HARNESS=SNAPSHOT/'scripts/bench/patching_latency'
sys.path.insert(0,str(HARNESS))
spec=importlib.util.spec_from_file_location('old_run',HARNESS/'run.py');old=importlib.util.module_from_spec(spec);spec.loader.exec_module(old)
command=old.command

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def verify_snapshot():
    m=json.loads((SNAPSHOT/'SOURCE-MANIFEST.json').read_text())
    for name,want in m['files'].items():
        if Path(name).is_absolute() or '..' in Path(name).parts or (SNAPSHOT/name).is_symlink() or digest(SNAPSHOT/name)!=want['sha256']:raise ValueError('snapshot mismatch: '+name)
    m=json.loads((SNAPSHOT/'BENCHMARK-IMPORT.json').read_text())
    for name,want in m['original_git_blobs'].items():
        data=(SNAPSHOT/name).read_bytes()
        if hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()!=want:raise ValueError('import mismatch: '+name)

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--phase',choices=['jit','aot'],required=True);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);verify_snapshot();records=[]
    identity={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),'phase':a.phase,'platform':platform.platform(),'machine':platform.machine(),
              'source_sha256':{str(p.relative_to(REPO)):digest(p) for p in sorted(HERE.glob('*')) if p.is_file()},'native_executed':False}
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    def run(cmd,name,timeout):
        row=command(cmd,out/(name+'.log'),timeout=timeout,cwd=REPO);records.append(row);(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
        if row['returncode']:raise RuntimeError(name+' failed; retained log')
    build=HERE/'build';exe=build/'StableGroupBench'
    if a.phase=='jit':
        run([sys.executable,HERE/'test_analyze.py'],'group-analysis-tests',30)
        prefix=subprocess.check_output(['brew','--prefix','faust'],text=True).strip()
        run(['cmake','-S',HERE,'-B',build,'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DFAUST_ROOT='+prefix],'configure',360)
        run(['cmake','--build',build,'--target','StableGroupBench','RetainedPatchingBench','--parallel','3'],'build',1200)
        # Original 46 cases, unchanged source/evaluator, same native support build.
        import analyze_retained
        original=out/'original-46';original.mkdir(exist_ok=True);cases=[]
        for mode,family,n,frames in analyze_retained.definitions():
            name=f'{mode}-{family}-{n}-f{frames}';folder=original/name;folder.mkdir(exist_ok=True)
            r=command([build/'host/RetainedPatchingBench',mode,family,n,frames,folder],original/(name+'.log'),timeout=90,cwd=REPO)
            r.update(name=name,mode=mode,family=family,size=n,frames=frames);cases.append(r)
            (original/'retained-cases.json').write_text(json.dumps(cases,indent=2)+'\n')
        r=analyze_retained.analyze(original)
        if not r['passed']:raise RuntimeError('original retained regression failed')
    else:
        from compile_native import compile_kernels
        compile_kernels(exe,HERE/'native-kernels',out/'compiler-evidence',command)
    identity['executable_sha256']=digest(exe);verify_snapshot()
    todo=analyze.definitions();random.Random(20260909).shuffle(todo);cases=[]
    for mode,family,n,width,frames in todo:
        name=f'{mode}-{family}-{n}-g{width}-b{frames}';folder=out/name;folder.mkdir(exist_ok=True)
        native=HERE/'native-kernels' if a.phase=='aot' else 'none'
        r=command([exe,mode,family,n,width,frames,folder,native],out/(name+'.log'),timeout=120,cwd=REPO)
        r.update(name=name,mode=mode,family=family,size=n,width=width,frames=frames);cases.append(r)
        (out/'cases.json').write_text(json.dumps(cases,indent=2)+'\n');print(a.phase,name,'exit',r['returncode'],flush=True)
    result=analyze.analyze(out,a.phase);identity.update(native_executed=True,passed=result['passed'],native_processes=len(cases));(out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    if not result['passed']:raise RuntimeError('group native/check failures; inspect summary.json')
    print('PASS',a.phase,len(cases),'native group cases; no product scheduling claim',flush=True)
if __name__=='__main__':main()
