#!/usr/bin/env python3
"""Bounded private-machine measurement. No installs or edits to product sources."""
from __future__ import annotations
import argparse, hashlib, json, os, platform, signal, subprocess, sys, time
from pathlib import Path
import analyze

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent


def command(cmd, log, *, timeout, cwd=ROOT):
    start=time.monotonic()
    with log.open('w') as output:
        p=subprocess.Popen([str(x) for x in cmd],cwd=cwd,stdout=output,
                           stderr=subprocess.STDOUT,start_new_session=True)
        try:
            code=p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            # Terminate the whole isolated child process group, including make/compiler.
            try: os.killpg(p.pid,signal.SIGTERM)
            except ProcessLookupError: pass
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try: os.killpg(p.pid,signal.SIGKILL)
                except ProcessLookupError: pass
                p.wait()
            output.write('\nCHECKPOINT_TIMEOUT\n'); code=124
    return {'command':[str(x) for x in cmd], 'returncode':code,
            'elapsed_seconds_not_edit_latency':time.monotonic()-start}


def main():
    args=argparse.ArgumentParser();args.add_argument('--output',type=Path,required=True);opts=args.parse_args()
    out=opts.output.resolve();out.mkdir(parents=True,exist_ok=True)
    records=[]
    def run(cmd,name,timeout,cwd=ROOT):
        record=command(cmd,out/(name+'.log'),timeout=timeout,cwd=cwd); records.append(record)
        (out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
        if record['returncode']!=0: raise RuntimeError(f'{name} failed; see {name}.log')
    # Exact source identities and scope, including the actual renderer support.
    identity={'repository_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              'platform':platform.platform(),'machine':platform.machine(),'source_hashes':{}}
    for name in ('native/graph/transport/FaustGraphRenderer.cpp','native/graph/state/GraphState.cpp',
                 'native/modules/backend/FaustRuntime.cpp','native/CMakeLists.txt'):
        identity['source_hashes'][name]=hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    run([sys.executable, str(HERE/'test_analyze.py')], 'analyzer-tests', 30)
    # Only the repository's pinned public dependencies, in this job checkout.
    run(['git','submodule','update','--init','--depth','1','--',
         'native/juce','native/choc','native/third_party/utf8proc','native/third_party/fast_float'],
        'submodules',300)
    faust_prefix=subprocess.check_output(['brew','--prefix','faust'],text=True).strip()
    run(['cmake','--preset','dev','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
         '-DCURLOP_BUILD_WEB_UI=OFF','-DCURLOP_ENABLE_ABLETON_LINK=OFF',
         '-DFAUST_ROOT='+faust_prefix,
         '-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES='+str(HERE/'inject.cmake')],
        'configure',180,cwd=ROOT/'native')
    run(['cmake','--build','build/dev','--target','PatchingLatencyBench','--parallel','8'],
        'build',900,cwd=ROOT/'native')
    executable=ROOT/'native/build/dev/PatchingLatencyBench'
    identity['executable_sha256']=hashlib.sha256(executable.read_bytes()).hexdigest()
    # Fail closed if a source changed during the isolated build.
    for name,expected in identity['source_hashes'].items():
        if hashlib.sha256((ROOT/name).read_bytes()).hexdigest()!=expected:
            raise RuntimeError('product source changed during benchmark preparation: '+name)
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    # Force the intended module-cache backend; no inherited runtime override.
    os.environ['CURLOP_FAUST_BACKEND']='jit'
    cases=[]
    definitions=analyze.definitions()
    for mode,family,size,frames in definitions:
        name=f'{mode}-{family}-{size}-f{frames}'; folder=out/name;folder.mkdir(exist_ok=True)
        record=command([executable,mode,family,size,frames,folder],out/(name+'.log'),timeout=75)
        record.update(name=name,mode=mode,family=family,size=size,frames=frames,load_average=os.getloadavg())
        cases.append(record); (out/'cases.json').write_text(json.dumps(cases,indent=2)+'\n')
        print(name,'exit',record['returncode'],flush=True)
    summary=analyze.analyze(out)
    print('numeric_evidence_valid',summary['numeric_evidence_valid'],flush=True)
    if not summary['numeric_evidence_valid']:raise RuntimeError('one or more benchmark cases failed')

if __name__=='__main__':
    main()
