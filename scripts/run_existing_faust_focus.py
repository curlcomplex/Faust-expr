#!/usr/bin/env python3
"""Focused follow-up: existing Faust controls only.
Tests whether scheduler chunk size, restored force-inline, and the upstream scalar
optimizer's -mcd 2 winner explain the earlier -sch deficit. No new scheduler.
"""
from pathlib import Path
from array import array
import hashlib, json, math, os, re, shutil, subprocess, sys, time

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/existing-focus'; EV=ROOT/'evidence-existing-focus'
BUILD.mkdir(parents=True,exist_ok=True); EV.mkdir(parents=True,exist_ok=True)
os.chdir(ROOT)
sys.path.insert(0,str(ROOT/'scripts'))
import run_existing_faust_options as lab
import run_scheduler_liveness as life
import scheduler_liveness_queue_controls as queue

commands=[]; results=[]; checks=[]; structures=[]; errors=[]
def save():
    for n,v in [('commands',commands),('timings',results),('correctness',checks),('structure',structures),('errors',errors)]:
        (EV/f'{n}.json').write_text(json.dumps(v,indent=2))
def run(cmd,name,env=None,timeout=180):
    cmd=list(map(str,cmd)); commands.append({'name':name,'argv':cmd}); print('stage='+name,flush=True)
    try:
        r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors='replace',env=env,timeout=timeout)
    except Exception as e:
        errors.append({'stage':name,'error':str(e)}); save(); raise
    (EV/f'{name}.txt').write_text(r.stdout)
    if r.returncode:
        errors.append({'stage':name,'error':f'exit {r.returncode}: {r.stdout[-1500:]}' }); save(); raise RuntimeError(name)
    return r.stdout.strip()

def compare(a,b):
    x,y=array('f'),array('f'); x.frombytes(Path(a).read_bytes()); y.frombytes(Path(b).read_bytes())
    if len(x)!=len(y) or not x: raise RuntimeError('capture length')
    diffs=[abs(float(i)-float(j)) for i,j in zip(x,y)]
    ok=all(d <= 1e-5 + 1e-5*abs(i) for d,i in zip(diffs,x))
    return {'samples':len(x),'bit_identical':Path(a).read_bytes()==Path(b).read_bytes(),'max_abs_error':max(diffs),'pass_tolerance':ok}

def main():
    version=run(['faust','-v'],'faust-version')
    if 'FAUST Version 2.85.9' not in version: raise RuntimeError('Faust drift')
    inc=run(['faust','--includedir'],'include'); lib=run(['faust','--libdir'],'lib')
    prefix=Path(run(['brew','--prefix','faust'],'faust-prefix')); llvm=Path(run(['brew','--prefix','llvm@22'],'llvm-prefix'))
    cxx=llvm/'bin/clang++'; cv=run([cxx,'--version'],'clang-version')
    if '22.1.8' not in cv: raise RuntimeError('Clang drift')
    run(['sysctl','hw.physicalcpu','hw.logicalcpu','machdep.cpu.brand_string'],'cpu')
    flags=[cxx,'-std=c++17','-O3','-I'+inc]; libs=['-L'+lib,'-Wl,-rpath,'+lib,'-lfaust','-lpthread','-lz']

    # Reuse the existing prep source, adding only arbitrary option injection and IR capture.
    prep=(ROOT/'scripts/scaling_factory_prep.cpp').read_text()
    prep=lab.replace_once(prep,'    std::vector<const char*> opts;', '''    if (const char* extra = std::getenv("SWEEP_OPTIONS")) {
        optStorage.clear(); std::istringstream words(extra); std::string word;
        while (words >> word) optStorage.push_back(word);
    }
    if (!writeFile(output + ".dsp", src.str())) return 8;
    std::vector<const char*> opts;''')
    prep=lab.replace_once(prep,'    const auto bc = writeDSPFactoryToBitcode(f);','    if (!writeFile(output + ".ll", writeDSPFactoryToIR(f))) return 9;\n    const auto bc = writeDSPFactoryToBitcode(f);')
    (BUILD/'prep.cpp').write_text(prep); (BUILD/'capture.cpp').write_text(lab.CAPTURE_CPP)
    for src,out in [(BUILD/'prep.cpp','prep'),(ROOT/'scripts/scheduler_scaling_bench.cpp','bench'),(BUILD/'capture.cpp','capture')]:
        run(flags+[src]+libs+['-o',BUILD/out],'build-'+out)

    # Existing ARM/lifetime/atomic-queue adaptation; inline variant changes only EXPORT annotation.
    sched=BUILD/'scheduler.cpp'; shutil.copy2(prefix/'share/faust/scheduler.cpp',sched)
    run([sys.executable,'scripts/patch_scheduler_arm64.py',sched],'arm-patch')
    runtime=queue.queue_control(life.instrument_runtime(sched.read_text(),barrier=False,trace=False),'atomic')
    inline=lab.replace_once(runtime,'#define EXPORT __attribute__ ((visibility("default")))',
                            '#define EXPORT __attribute__ ((visibility("default"))) __attribute__((always_inline))')
    target_src=BUILD/'target.cpp'; target_src.write_text('#include <faust/dsp/llvm-dsp.h>\n#include <iostream>\nint main(){std::cout<<getDSPMachineTarget();}\n')
    run(flags+[target_src]+libs+['-o',BUILD/'target'],'build-target'); triple=run([BUILD/'target'],'target').split(':')[0]
    for label,text in [('baseline',runtime),('inline',inline)]:
        cpp=BUILD/f'scheduler-{label}.cpp'; ll=BUILD/f'scheduler-{label}.ll'; cpp.write_text(text)
        run(flags+['-target',triple,'-S','-emit-llvm',cpp,'-o',ll],f'build-runtime-{label}')

    # Focus only on the unanswered variables. -mcd 2 is the upstream optimizer winner from run 22.
    configs=[
      ('scalar',[],None), ('scalar-mcd2',['-scal','-mcd','2'],None),
      ('sch-v32',['-sch','-vs','32'],'baseline'), ('sch-v32-mcd2',['-sch','-vs','32','-mcd','2'],'baseline'),
      ('sch-v512',['-sch','-vs','512'],'baseline'), ('sch-v512-mcd2',['-sch','-vs','512','-mcd','2'],'baseline'),
      ('sch-inline-v32',['-sch','-vs','32'],'inline'), ('sch-inline-v32-mcd2',['-sch','-vs','32','-mcd','2'],'inline'),
      ('sch-inline-v512',['-sch','-vs','512'],'inline'), ('sch-inline-v512-mcd2',['-sch','-vs','512','-mcd','2'],'inline'),
    ]
    candidates=[]
    for label,opts,rt in configs:
        full=opts+(['-L',str(BUILD/f'scheduler-{rt}.ll')] if rt else [])
        env=os.environ.copy(); env['SWEEP_OPTIONS']=' '.join(full)
        bc=BUILD/f'{label}.bc'; run([BUILD/'prep','scalar','32',bc,'-','heavy'],'prep-'+label,env=env)
        cand={'label':label,'runtime':rt,'opts':opts,'bc':bc}; candidates.append(cand)
        if rt:
            cpp=BUILD/f'{label}.cpp'; run(['faust','-lang','cpp',*opts,'-tg',str(bc)+'.dsp','-o',cpp],'generate-'+label)
            st={'label':label,**lab.generated_structure(cpp.read_text())}; structures.append(st); print('structure='+json.dumps(st),flush=True)
    save()

    # Ensure cached readers cannot use support IR from disk.
    for p in BUILD.glob('scheduler-*.ll'): p.rename(p.with_suffix('.ll.hidden'))
    def capture(c,p):
        out=BUILD/f"capture-{c['label']}-p{p}.f32"; run([BUILD/'capture',c['bc'],p,'fixed',out],f"capture-{c['label']}-p{p}",timeout=60); return out
    ref=capture(candidates[0],1)
    good={}
    for c in candidates:
        ps=[1,3] if c['runtime'] else [1]
        for p in ps:
            out=ref if c is candidates[0] else capture(c,p)
            ch={'label':c['label'],'participants':p,**compare(ref,out)}; checks.append(ch); good[(c['label'],p)]=ch['pass_tolerance']; print('correctness='+json.dumps(ch),flush=True)
    save()

    # One complete randomized pass, 512-frame callback only. Existing reader gives 9 trials/cell.
    import random
    cells=[(c,p) for c in candidates for p in ([1,2,3] if c['runtime'] else [1])]
    random.Random(20260908).shuffle(cells)
    for c,p in cells:
        out=run([BUILD/'bench',c['bc'],p,512,128],f"timing-{c['label']}-p{p}",timeout=75)
        vals={k:v for k,v in re.findall(r'^([a-z_]+)=(.+)$',out,re.M)}
        row={'label':c['label'],'participants':p,**{k:float(vals[k]) for k in ('median_ns_per_frame','best_ns_per_frame','worst_ns_per_frame')}}
        results.append(row); print('result='+json.dumps(row),flush=True); save()

    # Compact report; keep conclusions descriptive, not realtime acceptance.
    med={(r['label'],r['participants']):r['median_ns_per_frame'] for r in results}
    scalar=med[('scalar',1)]; opt=med[('scalar-mcd2',1)]
    valid=[r for r in results if r['label'].startswith('sch')]
    best=min(valid,key=lambda r:r['median_ns_per_frame'])
    report=[
      '# Focused existing-Faust follow-up','',
      'Hosted 3-core Apple M1 VM; cached LLVM bitcode; 32-heavy-voice source; 512-frame blocks. Offline throughput only.',
      '',f'- default scalar: {scalar:.3f} ns/frame',f'- scalar -mcd 2: {opt:.3f} ns/frame ({scalar/opt:.3f}x vs default scalar)',
      f"- best scheduled: {best['label']} p{best['participants']} = {best['median_ns_per_frame']:.3f} ns/frame ({opt/best['median_ns_per_frame']:.3f}x relative to optimized scalar)",
      '', '## Scheduler cells','', '| Candidate | P | median ns/frame | optimized scalar / scheduled |','|---|---:|---:|---:|']
    for r in sorted(valid,key=lambda x:(x['label'],x['participants'])):
        report.append(f"| {r['label']} | {r['participants']} | {r['median_ns_per_frame']:.3f} | {opt/r['median_ns_per_frame']:.3f} |")
    report += ['',f"Correctness checks: {len(checks)}; failures: {sum(not c['pass_tolerance'] for c in checks)}.",
               'Generated task counts are recorded in structure.json. This test changes no scheduler algorithm.']
    (EV/'REPORT.md').write_text('\n'.join(report)+'\n'); print('\n'.join(report),flush=True)
    return 1 if any(not c['pass_tolerance'] for c in checks) else 0

if __name__=='__main__':
    try: raise SystemExit(main())
    except Exception as e:
        errors.append({'stage':'main','error':str(e)}); save(); print('fatal='+str(e),flush=True); raise
