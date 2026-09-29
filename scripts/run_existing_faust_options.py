#!/usr/bin/env python3
"""Bounded existing-option/linkage controls. No new scheduler; no host integration.
Reuses scaling_factory_prep.cpp, scheduler_scaling_bench.cpp, the existing
ARM/lifetime/atomic-queue patches, and upstream dsp_optimizer unchanged.
"""
from array import array
from pathlib import Path
import csv, hashlib, json, math, os, random, re, shutil, subprocess, sys, time

CAPTURE_CPP = r'''
#include <faust/dsp/llvm-dsp.h>
#include <faust/dsp/dsp.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>
struct ReleaseFactory { void operator()(llvm_dsp_factory* p) const { if(p) deleteDSPFactory(p); } };
int main(int argc,char** argv) {
    if(argc!=5) return 2;
    int p=std::atoi(argv[2]); if(p<1 || p>3) return 2;
    setenv("OMP_NUM_THREADS",argv[2],1); setenv("OMP_DYN_THREAD","0",1);
    std::ifstream input(argv[1],std::ios::binary); if(!input) return 3;
    std::ostringstream data; data<<input.rdbuf(); std::string error;
    std::unique_ptr<llvm_dsp_factory,ReleaseFactory> f(readDSPFactoryFromBitcode(data.str(),getDSPMachineTarget(),error,-1));
    if(!f) { std::cerr<<error; return 4; }
    std::unique_ptr<dsp> d(f->createDSPInstance()); if(!d) return 5;
    d->init(48000); if(d->getNumInputs()!=0 || d->getNumOutputs()!=2) return 6;
    std::vector<FAUSTFLOAT> a(515),b(515); FAUSTFLOAT* out[]={a.data()+1,b.data()+1};
    std::ofstream file(argv[4],std::ios::binary); if(!file) return 7;
    const int irregular[]={1,7,31,33,127,255,513};
    int at=0,cycle=0;
    while(at<131072) {
        int n=std::string(argv[3])=="irregular"?irregular[(cycle++)%7]:512;
        n=std::min(n,131072-at);
        std::fill(a.begin(),a.end(),FAUSTFLOAT(123456)); std::fill(b.begin(),b.end(),FAUSTFLOAT(123456));
        d->compute(n,nullptr,out);
        if(a[0]!=123456 || b[0]!=123456 || a[n+1]!=123456 || b[n+1]!=123456) return 8;
        for(int i=0;i<n;++i) {
            float pair[]={float(out[0][i]),float(out[1][i])};
            if(!std::isfinite(pair[0]) || !std::isfinite(pair[1]) || pair[0]==123456 || pair[1]==123456) return 9;
            file.write(reinterpret_cast<char*>(pair),sizeof(pair));
        }
        at+=n;
    }
    if(!file) return 10;
    std::cout<<"capture_samples="<<at*2<<"\n";
    return 0;
}
'''
OPT_CPP = r'''
#include <faust/dsp/dsp-optimizer.h>
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    const char* flags[]={nullptr};
    dsp_optimizer opt(argv[1],0,flags,getDSPMachineTarget(),512,1,-1,true,false);
    auto result=opt.findOptimizedScalarParameters();
    std::ofstream f(argv[2]);
    for(const auto& s:std::get<3>(result)) { f<<s<<"\n"; std::cout<<s<<" "; }
    std::cout<<"\n";
    return f?0:3;
}
'''

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def replace_once(s,old,new):
    if s.count(old)!=1: raise ValueError(f'expected one source match, got {s.count(old)}: {old[:80]}')
    return s.replace(old,new,1)

def generated_structure(text):
    # Structural evidence from generated C++; not a claim about final JIT timings.
    creation=re.search(r'createScheduler\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)',text)
    match=re.search(r'void\s+computeThread\w*\s*\([^)]*\)\s*\{',text)
    body=''
    if match:
        start=match.end()-1; depth=0
        for i in range(start,len(text)):
            depth+=(text[i]=='{')-(text[i]=='}')
            if depth==0: body=text[start:i+1]; break
    return dict(queue_capacity=int(creation[1]) if creation else None,
                initially_ready=int(creation[2]) if creation else None,
                task_cases=len(set(re.findall(r'case\s+(\d+)\s*:',body))),
                compute_body_sha256=hashlib.sha256(body.encode()).hexdigest() if body else None,
                generated_bytes=len(text.encode()))

def compare_audio(a,b):
    x,y=array('f'),array('f'); x.frombytes(Path(a).read_bytes()); y.frombytes(Path(b).read_bytes())
    if len(x)!=262144 or len(y)!=len(x) or not all(map(math.isfinite,x)) or not all(map(math.isfinite,y)):
        raise ValueError('incomplete/nonfinite capture')
    diffs=[abs(float(i)-float(j)) for i,j in zip(x,y)]
    # Declared before running; do not relax on failure. No gain/time normalization.
    return dict(samples=len(x),bit_identical=Path(a).read_bytes()==Path(b).read_bytes(),
                max_abs_error=max(diffs),rms_error=math.sqrt(sum(d*d for d in diffs)/len(diffs)),
                pass_tolerance=all(d<=1e-5+1e-5*abs(i) for d,i in zip(diffs,x)),
                tolerance='abs <= 1e-5 + 1e-5 * abs(reference)')

def main():
    root=Path(__file__).resolve().parents[1]; os.chdir(root)
    build=root/'build/existing-options'; ev=root/'evidence-existing-options'
    build.mkdir(parents=True,exist_ok=True); ev.mkdir(parents=True,exist_ok=True)
    commands=[]; errors=[]; candidates=[]; captures=[]; results=[]; structure=[]
    def save():
        for name,value in [('commands',commands),('errors',errors),('candidates',candidates),('correctness',captures),('timings',results),('structure',structure)]:
            (ev/(name+'.json')).write_text(json.dumps(value,indent=2))
    def run(cmd,name,env=None,timeout=180,required=True):
        cmd=list(map(str,cmd)); row=dict(name=name,argv=cmd)
        commands.append(row); print('stage='+name,flush=True); t=time.monotonic()
        try:
            r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors='replace',env=env,timeout=timeout)
            row.update(exit_code=r.returncode,seconds=time.monotonic()-t)
            (ev/(name+'.txt')).write_text(r.stdout)
            if r.returncode: raise RuntimeError(f'{name}: exit {r.returncode}: {r.stdout[-2000:]}')
            return r.stdout.strip()
        except Exception as e:
            row['error']=str(e); errors.append(dict(stage=name,error=str(e),required=required)); save()
            print('failure='+str(e),flush=True)
            if required: raise
            return None
    try:
        run(['git','rev-parse','HEAD'],'commit'); run(['uname','-a'],'os')
        version=run(['faust','-v'],'faust-version')
        if 'FAUST Version 2.85.9' not in version: raise RuntimeError('expected Faust 2.85.9; no silent toolchain drift')
        run(['faust','--help'],'faust-help')
        run(['sysctl','hw.physicalcpu','hw.logicalcpu','machdep.cpu.brand_string'],'cpu')
        inc=run(['faust','--includedir'],'include'); lib=run(['faust','--libdir'],'lib')
        prefix=run(['brew','--prefix','faust'],'faust-prefix'); llvm=Path(run(['brew','--prefix','llvm@22'],'llvm-prefix'))
        cxx=llvm/'bin/clang++'; cv=run([cxx,'--version'],'clang-version')
        if '22.1.8' not in cv: raise RuntimeError('expected matching Clang 22.1.8')
        flags=[cxx,'-std=c++17','-O3','-I'+inc]; libs=['-L'+lib,'-Wl,-rpath,'+lib,'-lfaust','-lpthread','-lz']
        prep=(root/'scripts/scaling_factory_prep.cpp').read_text()
        prep=replace_once(prep,'    std::vector<const char*> opts;', '''    if (const char* extra = std::getenv("SWEEP_OPTIONS")) {
        optStorage.clear(); std::istringstream words(extra); std::string word;
        while (words >> word) optStorage.push_back(word);
    }
    if (!writeFile(output + ".dsp", src.str())) return 8;
    std::cout << "target=" << getDSPMachineTarget() << "\\n";
    std::vector<const char*> opts;''')
        prep=replace_once(prep,'    const auto bc = writeDSPFactoryToBitcode(f);','    if (!writeFile(output + ".ll", writeDSPFactoryToIR(f))) return 9;\n    const auto bc = writeDSPFactoryToBitcode(f);')
        (build/'prep.cpp').write_text(prep); shutil.copy2(build/'prep.cpp',ev/'prepared-source.cpp')
        (build/'capture.cpp').write_text(CAPTURE_CPP); (build/'optimizer.cpp').write_text(OPT_CPP)
        for source,dest in [(build/'prep.cpp','prep'),(root/'scripts/scheduler_scaling_bench.cpp','bench'),(build/'capture.cpp','capture')]:
            run(flags+[source]+libs+['-o',build/dest],'build-'+dest)
        optimizer_ok=run(flags+[build/'optimizer.cpp']+libs+['-o',build/'optimizer'],'build-upstream-optimizer',required=False) is not None
        up=Path(prefix)/'share/faust/scheduler.cpp'; shutil.copy2(up,ev/'upstream-scheduler.cpp')
        p=build/'scheduler.cpp'; shutil.copy2(up,p)
        run([sys.executable,'scripts/patch_scheduler_arm64.py',p],'existing-arm-patch')
        sys.path.insert(0,str(root/'scripts'))
        import run_scheduler_liveness as life
        import scheduler_liveness_queue_controls as queue
        runtime=queue.queue_control(life.instrument_runtime(p.read_text(),barrier=False,trace=False),'atomic')
        # ONLY restore always_inline while retaining emitted external definitions.
        inline_runtime=replace_once(runtime,'#define EXPORT __attribute__ ((visibility("default")))',
              '#define EXPORT __attribute__ ((visibility("default"))) __attribute__((always_inline))')
        (build/'target.cpp').write_text('#include <faust/dsp/llvm-dsp.h>\n#include <iostream>\nint main(){std::cout<<getDSPMachineTarget();}\n')
        run(flags+[build/'target.cpp']+libs+['-o',build/'target'],'build-target')
        target=run([build/'target'],'target'); triple=target.split(':')[0]
        for label,text in [('baseline',runtime),('inline',inline_runtime)]:
            p=build/('scheduler-'+label+'.cpp'); p.write_text(text); shutil.copy2(p,ev/p.name)
            run(flags+['-target',triple,'-S','-emit-llvm',p,'-o',build/('scheduler-'+label+'.ll')],'build-runtime-'+label)
        configurations=[('scalar',[],None,0)]
        for vs in (32,64,128,256,512):
            configurations += [(f'vec-v{vs}',['-vec','-vs',str(vs)],None,vs),
                               (f'sch-v{vs}',['-sch','-vs',str(vs)],'baseline',vs)]
        for vs in (32,512):
            configurations += [(f'schg-v{vs}',['-sch','-g','-vs',str(vs)],'baseline',vs),
                               (f'sch-inline-v{vs}',['-sch','-vs',str(vs)],'inline',vs)]
        def prepare(label,options,runtime,vs):
            full=options+(['-L',str(build/('scheduler-'+runtime+'.ll'))] if runtime else [])
            env=os.environ.copy(); env['SWEEP_OPTIONS']=' '.join(full)
            bc=build/(label+'.bc')
            out=run([build/'prep','scalar','32',bc,'-','heavy'],'prep-'+label,env=env)
            cand=dict(label=label,options=options,runtime=runtime,vs=vs,bitcode=str(bc),bitcode_sha256=sha(bc),source_sha256=sha(str(bc)+'.dsp'))
            if candidates and cand['source_sha256']!=candidates[0]['source_sha256']: raise RuntimeError('DSP source changed between candidates')
            candidates.append(cand); shutil.copy2(str(bc)+'.ll',ev/(label+'.ll'))
            if runtime:
                generated=build/(label+'.cpp')
                run(['faust','-lang','cpp',*options,'-tg',str(bc)+'.dsp','-o',generated],'generate-'+label)
                text=generated.read_text(); row=dict(label=label,**generated_structure(text)); structure.append(row)
                shutil.copy2(generated,ev/generated.name)
                for dot in build.glob(label+'*.dot'): shutil.copy2(dot,ev/dot.name)
                print('structure='+json.dumps(row),flush=True)
            save(); return cand
        for conf in configurations: prepare(*conf)
        source=Path(candidates[0]['bitcode']+'.dsp'); shutil.copy2(source,ev/'same-heavy-32.dsp')
        if optimizer_ok:
            opts_file=build/'upstream-scalar-options.txt'
            out=run([build/'optimizer',source,opts_file],'upstream-scalar-search',timeout=240,required=False)
            if out is not None:
                options=opts_file.read_text().split(); shutil.copy2(opts_file,ev/opts_file.name)
                prepare('upstream-scalar-winner',options,None,0)
        # Reader processes cannot depend on support IR remaining available.
        for p in build.glob('scheduler-*.ll'): p.rename(p.with_suffix('.ll.hidden'))
        def capture(c,p,pattern='fixed'):
            name=f"capture-{c['label']}-p{p}-{pattern}"; path=build/(name+'.f32')
            run([build/'capture',c['bitcode'],p,pattern,path],name,timeout=60)
            return path
        ref=capture(candidates[0],1)
        good={}; fixed={}
        for c in candidates:
            for p in ([1,2,3] if c['runtime'] else [1]):
                path=ref if c is candidates[0] else capture(c,p)
                check=dict(label=c['label'],participants=p,pattern='fixed',**compare_audio(ref,path))
                captures.append(check); good[(c['label'],p)]=check['pass_tolerance']; fixed[(c['label'],p)]=path
                print('correctness='+json.dumps(check),flush=True)
        # Repeat changing block boundaries for all scheduler chunk-size controls.
        for c in candidates:
            if c['runtime']:
                path=capture(c,3,'irregular'); check=dict(label=c['label'],participants=3,pattern='irregular-vs-fixed',**compare_audio(fixed[(c['label'],3)],path))
                captures.append(check); print('correctness='+json.dumps(check),flush=True)
                if not check['pass_tolerance']:
                    for participant in (1,2,3): good[(c['label'],participant)]=False
        save()
        # Two independently shuffled passes; each cell reuses the existing 9-trial reader.
        cells=[(c,f,p) for c in candidates for f in (128,512) for p in ([1,2,3] if c['runtime'] else [1])]
        for repeat in range(2):
            ordered=cells.copy(); random.Random(20260907+repeat).shuffle(ordered)
            for c,f,p in ordered:
                name=f"timing-r{repeat}-{c['label']}-f{f}-p{p}"
                output=run([build/'bench',c['bitcode'],p,f,max(128,65536//f)],name,timeout=75)
                values={k:v for k,v in re.findall(r'^([a-z_]+)=(.+)$',output,re.M)}
                row=dict(label=c['label'],frames=f,participants=p,repeat=repeat,**{k:float(values[k]) for k in ('median_ns_per_frame','best_ns_per_frame','worst_ns_per_frame')})
                if any(not math.isfinite(row[k]) or row[k]<=0 for k in ('median_ns_per_frame','best_ns_per_frame','worst_ns_per_frame')): raise RuntimeError('invalid timing')
                results.append(row); print('result='+json.dumps(row),flush=True); save()
        # Low-frequency stack samples are diagnostic only, never timing inputs.
        profile_source=(root/'scripts/scheduler_scaling_bench.cpp').read_text()
        profile_source=replace_once(profile_source,'    std::vector<double> nsPerFrame;',
                                   '    std::cerr << "profile_compute_ready\\n" << std::flush;\n    std::vector<double> nsPerFrame;')
        (build/'profile.cpp').write_text(profile_source)
        if run(flags+['-g','-fno-omit-frame-pointer',build/'profile.cpp']+libs+['-o',build/'profile'],'build-profiler',required=False) is not None:
            for label in ('sch-v32','sch-v512'):
                for participant in (1,3):
                    name=f'profile-{label}-p{participant}'
                    c=next(c for c in candidates if c['label']==label)
                    cmd=[str(build/'profile'),c['bitcode'],str(participant),'512','4096']
                    with (ev/(name+'.txt')).open('w') as log:
                        proc=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT)
                        try:
                            until=time.monotonic()+60
                            while proc.poll() is None and time.monotonic()<until:
                                if 'profile_compute_ready' in (ev/(name+'.txt')).read_text(): break
                                time.sleep(0.1)
                            if proc.poll() is None:
                                run(['/usr/bin/sample',str(proc.pid),'1','1','-file',ev/(name+'-sample.txt')],name+'-sample-command',timeout=15,required=False)
                            proc.wait(timeout=60)
                        except subprocess.TimeoutExpired:
                            errors.append(dict(stage=name,error='profiling timeout',required=False))
                        finally:
                            if proc.poll() is None: proc.kill(); proc.wait()
        for (label,participant),path in fixed.items():
            if label in ('scalar','sch-v32','sch-v512','sch-inline-v32','sch-inline-v512') and participant in (1,3):
                shutil.copy2(path,ev/path.name)
        # Preserve actual linked IR call sites, not just the existence of a flag.
        for row in structure:
            ir=(ev/(row['label']+'.ll')).read_text()
            row['scheduler_api_call_sites']=len(re.findall(r'\b(?:call|invoke)\b[^\n]*@(?:getNextTask|initTaskList|activateOutputTask[12]|activateOneOutputTask|getReadyTask|initTask|pushHead)\(',ir))
        save()
        fields=['label','frames','participants','repeat','median_ns_per_frame','best_ns_per_frame','worst_ns_per_frame']
        with (ev/'results.tsv').open('w') as f:
            w=csv.DictWriter(f,fields,delimiter='\t'); w.writeheader(); w.writerows(results)
        report=['# Existing Faust options / linkage controls','',
                'Offline hosted-M1 throughput; 32 heavy voices from the unchanged scaling source. Not Core Audio deadline/headroom acceptance.',
                'Two shuffled fresh-process passes, each the existing nine-trial benchmark. All loads are cached bitcode.',
                'No new scheduler; baseline runtime uses the previous ARM, lifetime and atomic-queue adaptations.',
                'The inline control restores always_inline on exported helpers, retaining external definitions.',
                'Timings are trial averages: worst_ns_per_frame is NOT worst callback latency.',
                'Correctness tolerance was declared in the script before execution; no normalization.',
                '', '| Frames | Repeat | Scalar ns/frame | Best scheduled candidate | P | ns/frame | Scalar / scheduled |',
                '|---:|---:|---:|---|---:|---:|---:|']
        for f in (128,512):
            for rep in range(2):
                rows=[r for r in results if r['frames']==f and r['repeat']==rep]
                scalar=next(r['median_ns_per_frame'] for r in rows if r['label']=='scalar')
                scheduled=[r for r in rows if r['label'].startswith('sch') and good.get((r['label'],r['participants']),False)]
                if not scheduled: raise RuntimeError('no numerically qualified scheduler candidate')
                best=min(scheduled,key=lambda r:r['median_ns_per_frame'])
                report.append(f"| {f} | {rep} | {scalar:.3f} | {best['label']} | {best['participants']} | {best['median_ns_per_frame']:.3f} | {scalar/best['median_ns_per_frame']:.3f} |")
        failures=[c for c in captures if not c['pass_tolerance']]
        report+=['',f'Correctness comparisons: {len(captures)}; failed tolerance: {len(failures)}.',
                 'Full rows, audio error metrics, generated task structures, linked IR, toolchain and exact checked-out commit are in this artifact.',
                 'The scalar optimizer is upstream dsp_optimizer::findOptimizedScalarParameters(), not a replacement implementation.',
                 'Remaining scope: callback-tail tests, production worker-team/renderer integration, multi-graph corpus, CPU-target tuning and upstream fusion research.']
        (ev/'REPORT.md').write_text('\n'.join(report)+'\n'); print('\n'.join(report),flush=True)
        if failures: return 1
        return 0
    except Exception as e:
        errors.append(dict(stage='main',error=str(e),required=True)); print('fatal='+str(e),flush=True); return 1
    finally:
        save()
        (ev/'provenance.json').write_text(json.dumps(dict(script_sha256=sha(__file__),base_prep_sha256=sha(root/'scripts/scaling_factory_prep.cpp'),base_bench_sha256=sha(root/'scripts/scheduler_scaling_bench.cpp'),clock='steady_clock wall time',sample_rate=48000),indent=2))

if __name__=='__main__':
    if '--self-test' in sys.argv:
        import tempfile, unittest
        class Checks(unittest.TestCase):
            def test_replace(self):
                self.assertEqual(replace_once('abc','b','d'),'adc')
                with self.assertRaises(ValueError): replace_once('bb','b','d')
            def test_structure(self):
                x=generated_structure('auto s=createScheduler(17,4); void computeThread(int t) { switch(t) {case 0: break; case 2: {x();}}}')
                self.assertEqual((x['queue_capacity'],x['initially_ready'],x['task_cases']),(17,4,2))
                self.assertIsNotNone(x['compute_body_sha256'])
            def test_audio(self):
                with tempfile.TemporaryDirectory() as d:
                    a=Path(d)/'a'; b=Path(d)/'b'; values=array('f',[0.5])*262144
                    a.write_bytes(values.tobytes()); b.write_bytes(values.tobytes())
                    self.assertTrue(compare_audio(a,b)['bit_identical'])
                    values[100]=0.7; b.write_bytes(values.tobytes())
                    self.assertFalse(compare_audio(a,b)['pass_tolerance'])
        unittest.main(argv=[sys.argv[0]])
    else: raise SystemExit(main())
