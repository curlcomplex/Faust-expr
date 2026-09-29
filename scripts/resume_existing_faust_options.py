#!/usr/bin/env python3
"""Complete diagnostics from the saved options sweep, without rerunning its search.
Input is the SHA256-verified artifact of run 34164929435, attempt 1. Saved linked
LLVM IR is reloaded and serialized to .bc, then fresh readers execute that .bc.
The original full first timing pass remains separate from these confirmation runs.
"""
from pathlib import Path
import collections, hashlib, json, math, os, random, re, shutil, subprocess, sys, time
import run_existing_faust_options as prior

REPACK = r'''
#include <faust/dsp/llvm-dsp.h>
#include <fstream>
#include <iostream>
#include <sstream>
int main(int argc,char**argv) {
 if(argc!=3) return 2;
 std::ifstream input(argv[1],std::ios::binary); if(!input) return 3;
 std::ostringstream s; s<<input.rdbuf(); std::string error;
 auto*f=readDSPFactoryFromIR(s.str(),getDSPMachineTarget(),error,-1);
 if(!f){std::cerr<<error; return 4;}
 const auto bc=writeDSPFactoryToBitcode(f);
 std::ofstream out(argv[2],std::ios::binary); out<<bc; out.close();
 bool valid=bool(out)&&!bc.empty();
 std::cout<<"target="<<getDSPMachineTarget()<<"\noptions="<<f->getCompileOptions()<<"\n";
 if(!deleteDSPFactory(f)) return 5;
 return valid?0:6;
}
'''
LABELS=('scalar','upstream-scalar-winner','vec-v32','sch-v32','sch-inline-v32','sch-v512')

def graph_stats(text):
 edges=re.findall(r'(L\w+)\s*->\s*(L\w+)',text)
 incoming=collections.Counter(b for a,b in edges); outgoing=collections.Counter(a for a,b in edges)
 nodes=set(incoming)|set(outgoing)
 return dict(nodes=len(nodes),edges=len(edges),maximum_fanout=max(outgoing.values()),
             indegree_counts=dict(collections.Counter(incoming[n] for n in nodes)))

def main():
 root=Path(__file__).resolve().parents[1]; os.chdir(root)
 source=root/'prior-artifact/evidence-existing-options'
 build=root/'build/focus'; ev=root/'evidence-focus'
 build.mkdir(parents=True,exist_ok=True); ev.mkdir(parents=True,exist_ok=True)
 commands=[]; errors=[]; timings=[]; checks=[]; profiles=[]
 def save():
  for name,value in [('commands',commands),('errors',errors),('timings',timings),('correctness',checks),('profiles',profiles)]:
   (ev/(name+'.json')).write_text(json.dumps(value,indent=2))
 def run(cmd,name,timeout=90):
  cmd=list(map(str,cmd)); row={'name':name,'argv':cmd}; commands.append(row)
  print('stage='+name,flush=True); t=time.monotonic()
  with (ev/(name+'.txt')).open('w') as log:
   try:
    p=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
    row.update(exit_code=p.returncode,seconds=time.monotonic()-t)
    if p.returncode: raise RuntimeError(f'{name}: exit {p.returncode}')
   except Exception as e:
    row['error']=str(e); errors.append(row.copy()); save(); raise
  save(); return (ev/(name+'.txt')).read_text().strip()
 try:
  run(['git','rev-parse','HEAD'],'commit');run(['sysctl','hw.physicalcpu','hw.logicalcpu','machdep.cpu.brand_string'],'cpu')
  if 'FAUST Version 2.85.9' not in run(['faust','-v'],'faust-version'): raise RuntimeError('unexpected Faust version')
  inc=run(['faust','--includedir'],'include'); lib=run(['faust','--libdir'],'lib')
  llvm=Path(run(['brew','--prefix','llvm@22'],'llvm')); cxx=llvm/'bin/clang++'
  if '22.1.8' not in run([cxx,'--version'],'clang-version'): raise RuntimeError('unexpected Clang version')
  flags=[cxx,'-O3','-std=c++17','-I'+inc]; libs=['-L'+lib,'-Wl,-rpath,'+lib,'-lfaust','-lpthread','-lz']
  (build/'repack.cpp').write_text(REPACK); (build/'capture.cpp').write_text(prior.CAPTURE_CPP)
  original=(root/'scripts/scheduler_scaling_bench.cpp').read_text()
  profile=prior.replace_once(original,'    std::vector<double> nsPerFrame;',
    '    std::cerr << "profile_compute_ready\\n" << std::flush;\n    std::vector<double> nsPerFrame;')
  (build/'profile.cpp').write_text(profile)
  for src,dest in [(build/'repack.cpp','repack'),(build/'capture.cpp','capture'),(root/'scripts/scheduler_scaling_bench.cpp','bench'),(build/'profile.cpp','profile')]:
   run(flags+[src]+libs+['-o',build/dest],'build-'+dest)
  provenance={'basis_run':34164929435,'basis_attempt':1,'basis_commit':(source/'commit.txt').read_text().strip(),
    'basis_zip_sha256':'0045e3edf40b1e25be9c43e42955434f0b956027c134a252d5fff73351c12fd6',
    'input_ir_sha256':{},'route':'saved linked LLVM IR -> factory -> .bc -> fresh reader; no Faust-source recompilation',
    'sample_rate':48000,'frames':512,'clock':'steady_clock','not_realtime_acceptance':True}
  for label in LABELS:
   ir=source/(label+'.ll'); provenance['input_ir_sha256'][label]=prior.sha(ir)
   run([build/'repack',ir,build/(label+'.bc')],'repack-'+label)
  (ev/'provenance.json').write_text(json.dumps(provenance,indent=2))
  structure=[]
  for label in ('sch-v32','schg-v32','sch-v512','schg-v512','sch-inline-v32'):
   cpp=(source/(label+'.cpp')).read_text(); ir=(source/(label+'.ll')).read_text()
   row=dict(label=label,**prior.generated_structure(cpp),**graph_stats((source/(label+'.bc.dsp.dot')).read_text()))
   row['scheduler_api_call_sites']=len(re.findall(r'\b(?:call|invoke)\b[^\n]*@(?:getNextTask|initTaskList|activateOutputTask[12]|activateOneOutputTask|getReadyTask|initTask|pushHead)\(',ir))
   structure.append(row)
  (ev/'structure.json').write_text(json.dumps(structure,indent=2))
  def capture(label,participant):
   out=ev/f'{label}-p{participant}.f32';run([build/'capture',build/(label+'.bc'),participant,'fixed',out],f'capture-{label}-p{participant}')
   return out
  captured={}
  for label in LABELS:
   for p in ((1,3) if label.startswith('sch') else (1,)):
    captured[(label,p)]=capture(label,p)
  ref=captured[('scalar',1)]
  for (label,p),path in captured.items():
   checks.append(dict(kind='cross-representation-scalar-reference',label=label,participants=p,**prior.compare_audio(ref,path)))
  for label in ('sch-v32','sch-inline-v32','sch-v512'):
   checks.append(dict(kind='participant-count',label=label,**prior.compare_audio(captured[(label,1)],captured[(label,3)])))
  for label in ('sch-inline-v32','sch-v512'):
   checks.append(dict(kind='scheduler-intervention',label=label,**prior.compare_audio(captured[('sch-v32',1)],captured[(label,1)])))
  save()
  cells=[(label,p) for label in LABELS for p in ((1,3) if label.startswith('sch') else (1,))]
  for repeat in range(2):
   order=cells.copy();random.Random(20260908+repeat).shuffle(order)
   for label,p in order:
    output=run([build/'bench',build/(label+'.bc'),p,512,256],f'timing-r{repeat}-{label}-p{p}')
    values=dict(re.findall(r'^([a-z_]+)=(.+)$',output,re.M))
    row=dict(label=label,participants=p,repeat=repeat,frames=512,
      **{k:float(values[k]) for k in ('median_ns_per_frame','best_ns_per_frame','worst_ns_per_frame')})
    if not all(math.isfinite(row[k]) and row[k]>0 for k in ('median_ns_per_frame','best_ns_per_frame','worst_ns_per_frame')): raise RuntimeError('bad timings')
    timings.append(row);print('result='+json.dumps(row),flush=True);save()
  # Sampling is on separate processes and never included in timing evidence.
  for label,p in [('sch-v32',1),('sch-v32',3),('sch-v512',3),('sch-inline-v32',3)]:
   name=f'profile-{label}-p{p}'; command=list(map(str,[build/'profile',build/(label+'.bc'),p,512,1024]))
   profile_row=dict(label=label,participants=p,argv=command);profiles.append(profile_row)
   with (ev/(name+'.txt')).open('w') as log:
    proc=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT)
    try:
     until=time.monotonic()+70;ready=False
     while proc.poll() is None and time.monotonic()<until:
      if 'profile_compute_ready' in (ev/(name+'.txt')).read_text(): ready=True; break
      time.sleep(.1)
     profile_row['ready']=ready
     if ready and proc.poll() is None:
      sample=subprocess.run(['/usr/bin/sample',str(proc.pid),'1','2','-file',str(ev/(name+'-sample.txt'))],stdout=log,stderr=log,timeout=15)
      profile_row['sample_exit_code']=sample.returncode
     proc.wait(timeout=60);profile_row['exit_code']=proc.returncode
    except subprocess.TimeoutExpired:
     profile_row['timeout']=True
    finally:
     if proc.poll() is None:proc.kill();proc.wait()
     save()
  report=['# Existing Faust options: focused confirmation and diagnostics','',
   'Basis: run 34164929435 timed out after one complete 68-cell pass and 41 cells of the second pass. Its partial data is preserved, not called a successful complete sweep.',
   'This confirmation reuses that artifact’s linked LLVM IR, repacks it as bitcode and uses fresh reader processes. No DSP/source/queue redesign.',
   'Same 32-heavy-voice graph; 512-frame blocks; two shuffled passes; nine timing trials per cell. No cross-run timings are pooled.',
   '', '| Candidate | Participants | Pass 1 ns/frame | Pass 2 ns/frame |','|---|---:|---:|---:|']
  for label,p in cells:
   pair=[next(r['median_ns_per_frame'] for r in timings if r['label']==label and r['participants']==p and r['repeat']==i) for i in (0,1)]
   report.append(f'| {label} | {p} | {pair[0]:.3f} | {pair[1]:.3f} |')
  cross=[x for x in checks if x['kind'].startswith('cross')]; intervention=[x for x in checks if not x['kind'].startswith('cross')]
  report+=['',f'Cross-representation tolerance failures: {sum(not x["pass_tolerance"] for x in cross)}/{len(cross)}. Threshold unchanged: abs <= 1e-5 + 1e-5*abs(reference).',
    f'Exact participant/intervention matches: {sum(x["bit_identical"] for x in intervention)}/{len(intervention)}.',
    'A timing win is not numerical equivalence or production approval. Worst trial average is not worst callback.',
    'Sample traces are statistical diagnostic evidence; anonymous JIT frames must not be mislabeled as a known CPU-time category.',
    'The original upstream scalar search recommended -scal -mcd 2. That recommendation is reused here, not searched again.']
  (ev/'REPORT.md').write_text('\n'.join(report)+'\n');print('\n'.join(report),flush=True)
  # Cross-representation differences remain explicitly unqualified. Gate only
  # the interventions intended to leave this exact scheduled computation intact.
  return 0 if all(x['bit_identical'] for x in intervention) else 1
 except Exception as e:
  errors.append(dict(stage='main',error=str(e)));print('fatal='+str(e),flush=True);return 1
 finally:save()

if __name__=='__main__':raise SystemExit(main())
