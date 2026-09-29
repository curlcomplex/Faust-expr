#!/usr/bin/env python3
"""Broader correctness gate for the existing atomic queue candidate.
No timing acceptance; no changes to generated task partitioning. Runs each case
in a subprocess so a hang cannot suppress later cases. Failing audio is kept.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import run_scheduler_liveness as previous
from scheduler_liveness_queue_controls import queue_control

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/correctness'
OUT=ROOT/'evidence-correctness'
COMMANDS=[]
RESULTS=[]

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def command(argv,name):
    args=list(map(str,argv)); COMMANDS.append({'name':name,'argv':args})
    r=subprocess.run(args,cwd=ROOT,text=True,errors='replace',stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120)
    (OUT/(name+'.txt')).write_text(r.stdout)
    if r.returncode: raise RuntimeError(name+': '+r.stdout[-5000:])
    return r.stdout.strip()

def child(argv,name,env,timeout=25):
    args=list(map(str,argv)); path=OUT/(name+'.txt')
    COMMANDS.append({'name':name,'argv':args,'env':{k:v for k,v in env.items() if k.startswith(('OMP_','FAUST_'))}})
    timedout=False
    with path.open('w') as f:
        p=subprocess.Popen(args,cwd=ROOT,env=env,stdout=f,stderr=subprocess.STDOUT,start_new_session=True)
        try: p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timedout=True
            try:
                with (OUT/(name+'-sample.txt')).open('w') as d:
                    subprocess.run(['/usr/bin/sample',str(p.pid),'1'],stdout=d,stderr=subprocess.STDOUT,timeout=5)
            except (OSError,subprocess.TimeoutExpired): pass
            try: os.killpg(p.pid,signal.SIGKILL)
            except ProcessLookupError: pass
            p.wait()
    text=path.read_text(errors='replace')
    row={'name':name,'exit_code':p.returncode,'timeout':timedout,
         'pass':p.returncode==0 and 'stage=main_return' in text,
         'comparisons':len(re.findall(r'^comparison=',text,re.M)),
         'created_workers':len(re.findall(r'diag_thread_create id=\d+ rc=0',text)),
         'last_stage':(re.findall(r'^stage=(.*)',text,re.M) or ['none'])[-1],
         'error':(re.findall(r'^failure=(.*)',text,re.M) or [None])[-1]}
    RESULTS.append(row); print(json.dumps(row),flush=True)
    return row

def main():
    BUILD.mkdir(parents=True,exist_ok=True); OUT.mkdir(parents=True,exist_ok=True)
    inc=command(['faust','--includedir'],'include'); lib=command(['faust','--libdir'],'lib')
    prefix=command(['brew','--prefix','faust'],'faust-prefix')
    llvm=command(['brew','--prefix','llvm@22'],'llvm-prefix'); cc=Path(llvm)/'bin/clang++'
    version=command(['faust','-v'],'faust-version')
    if 'FAUST Version 2.85.9' not in version: raise RuntimeError('toolchain changed: expected Faust 2.85.9')
    clang=command([cc,'--version'],'clang-version')
    if '22.1.8' not in clang: raise RuntimeError('expected Clang/LLVM 22.1.8')
    cpu=command(['sysctl','hw.physicalcpu','hw.logicalcpu','machdep.cpu.brand_string'],'cpu')
    command(['uname','-a'],'os'); command(['otool','-L',Path(lib)/'libfaust.dylib'],'dependencies')
    threads=[int(x) for x in os.environ.get('CORRECTNESS_THREADS','1,2,3').split(',')]
    physical=int(command(['sysctl','-n','hw.physicalcpu'],'physical-cpus'))
    if any(x<1 or x>physical for x in threads): raise RuntimeError('requested participation exceeds physical CPUs')
    source=BUILD/'probe.dsp'
    old=ROOT/'scripts/scheduler_bitcode_probe.cpp'
    source.write_text(re.search(r'kSource\s*=\s*R"FAUST\((.*?)\)FAUST"',old.read_text(),re.S).group(1))
    shutil.copy2(source,OUT/'probe.dsp')
    flags=[cc,'-std=c++17','-O2','-g','-fno-omit-frame-pointer','-I'+inc]
    libs=['-L'+lib,'-Wl,-rpath,'+lib,'-lfaust','-lpthread','-lz']
    host=BUILD/'correctness'
    command(flags+[ROOT/'scripts/scheduler_correctness.cpp']+libs+['-o',host],'build-host')
    target_cpp=BUILD/'target.cpp'
    target_cpp.write_text('#include <faust/dsp/llvm-dsp.h>\n#include <iostream>\nint main(){std::cout<<getDSPMachineTarget();}\n')
    command(flags+[target_cpp]+libs+['-o',BUILD/'target'],'build-target')
    target=command([BUILD/'target'],'target')
    original=Path(prefix)/'share/faust/scheduler.cpp'
    shutil.copy2(original,OUT/'upstream-scheduler.cpp')
    (OUT/'provenance.json').write_text(json.dumps({'git_sha':command(['git','rev-parse','HEAD'],'git-sha'),
       'original_dsp_sha256':sha(source),'upstream_scheduler_sha256':sha(original),'faust':version,
       'clang':clang,'cpu':cpu,'participants':threads,'target':target,
       'scope':'existing probe; standalone correctness, not Curlop production or RT timing',
       'input_count':0,'sample_rates':[44100,48000,96000],
       'variable_positive_block_sizes':[1,3,7,15,16,31,32,33,63,64,65,127,128,129,255,256,257,511,512,513],
       'within_scheduled_absolute_tolerance':1e-7},indent=2))
    env=os.environ.copy()
    for key in list(env):
        if key.startswith(('OMP_','FAUST_')): env.pop(key)
    env.update(FAUST_JIT_TARGET=target,OMP_NUM_THREADS='1',OMP_DYN_THREAD='0')
    scenarios=['fixed','variable','partition','lifetimes','factory_cycles','overlap_same','overlap_distinct','retire']
    selection=os.environ.get('CORRECTNESS_VARIANT','both')
    if selection not in ('both','atomic','atomic_barrier'): raise RuntimeError('invalid variant')
    for variant,barrier in [('atomic',False),('atomic_barrier',True)]:
        if selection!='both' and selection!=variant: continue
        folder=BUILD/variant; folder.mkdir(exist_ok=True)
        runtime=folder/'scheduler.cpp'; shutil.copy2(original,runtime)
        command([sys.executable,ROOT/'scripts/patch_scheduler_arm64.py',runtime],'prior-patch-'+variant)
        s=previous.instrument_runtime(runtime.read_text(),barrier=barrier,trace=False)
        runtime.write_text(queue_control(s,'atomic'))
        shutil.copy2(runtime,OUT/(variant+'-scheduler.cpp'))
        ll=folder/'scheduler.ll'
        command(flags+['-target',target.split(':')[0],'-S','-emit-llvm',runtime,'-o',ll],'build-runtime-'+variant)
        shutil.copy2(ll,OUT/(variant+'-scheduler.ll'))
        cacheA=folder/'A.bc.txt'; cacheB=folder/'B.bc.txt'
        e=env.copy();e['FAUST_SCHEDULER_MODULE']=str(ll)
        prepared=child([host,'prepare','sch','fixed',1,48000,source,cacheA,cacheB,OUT/(variant+'-prepare')],variant+'-prepare',e)
        if not prepared['pass']: continue
        shutil.copy2(cacheA,OUT/(variant+'-A.bc.txt')); shutil.copy2(cacheB,OUT/(variant+'-B.bc.txt'))
        hidden=ll.with_suffix('.unavailable');ll.rename(hidden)
        try:
            for n in threads:
                for scenario in scenarios:
                    name=f'{variant}-p{n}-{scenario}'
                    child([host,'case','sch',scenario,n,48000,source,cacheA,cacheB,OUT/name],name,env)
            for rate in (44100,96000):
                name=f'{variant}-rate{rate}-variable'
                child([host,'case','sch','variable',max(threads),rate,source,cacheA,cacheB,OUT/name],name,env)
        finally: hidden.rename(ll)
    # Scalar controls separate ordinary numerical/block effects from scheduling.
    sa=BUILD/'scalarA.bc.txt'; sb=BUILD/'scalarB.bc.txt'
    prepare=child([host,'prepare','scalar','fixed',1,48000,source,sa,sb,OUT/'scalar-prepare'],'scalar-prepare',env)
    if prepare['pass']:
        for scenario in ('partition','overlap_distinct','retire'):
            name='scalar-'+scenario
            child([host,'case','scalar',scenario,1,48000,source,sa,sb,OUT/name],name,env)
    return 0 if RESULTS and all(x['pass'] for x in RESULTS) else 1

if __name__=='__main__':
    rc=1
    try: rc=main()
    except Exception as error:
        print('harness_error='+repr(error),flush=True); RESULTS.append({'name':'harness','pass':False,'error':repr(error)})
    finally:
        OUT.mkdir(parents=True,exist_ok=True)
        summary={'results':RESULTS,'pass':rc==0,
                 'warning':'No performance, formal race freedom, production-renderer or device-callback acceptance.'}
        (OUT/'summary.json').write_text(json.dumps(summary,indent=2))
        (OUT/'commands.json').write_text(json.dumps(COMMANDS,indent=2))
        print('CORRECTNESS_SUMMARY\n'+json.dumps(summary,indent=2),flush=True)
        if os.environ.get('GITHUB_STEP_SUMMARY'):
            with open(os.environ['GITHUB_STEP_SUMMARY'],'a') as f:
                f.write('## Scheduler correctness\n\n| Case | Pass | Error/stage |\n|---|---|---|\n')
                for r in RESULTS: f.write(f"| {r['name']} | {r['pass']} | {r.get('error') or r.get('last_stage','')} |\n")
    raise SystemExit(rc)
