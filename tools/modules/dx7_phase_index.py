#!/usr/bin/env python3
"""#112 next slice: isolate modulator-output -> carrier phase/index scaling.
No upstream Faust or MSFA DSP is modified. This is software-oracle identification,
not hardware validation and not a shipping correction.
"""
from __future__ import annotations
import argparse, json, math, os, shlex, subprocess
from pathlib import Path
import numpy as np
import lab

ROOT=lab.ROOT
MSFA='f67d41d313b7dc85f6fb99e79e515cc9d208cfff'
RATE=44100; FRAMES=65536; GATE_ON=2048; GATE_OFF=32768
LEVELS=(50,70,90)
SCALES=tuple(round(x,2) for x in np.arange(.5,2.51,.1))
CPP_FLAGS=['-std=c++11','-include','stddef.h','-O2','-ffp-contract=off','-fwrapv']
SOURCES=['dx7note.cc','fm_core.cc','fm_op_kernel.cc','env.cc','freqlut.cc','exp2.cc','sin.cc','pitchenv.cc']

def run(cmd,cwd=ROOT,timeout=180):
 p=subprocess.run([str(x) for x in cmd],cwd=cwd,capture_output=True,text=True,timeout=timeout)
 if p.returncode: raise RuntimeError(f'command failed: {cmd}\n{p.stdout}\n{p.stderr}')
 return p.stdout

def checkout(dst):
 run(['git','init','-q',dst]); run(['git','-C',dst,'remote','add','origin','https://github.com/google/music-synthesizer-for-android.git'])
 run(['git','-C',dst,'fetch','-q','--depth=1','origin',MSFA]); run(['git','-C',dst,'checkout','-q','--detach','FETCH_HEAD'])
 if run(['git','-C',dst,'rev-parse','HEAD']).strip()!=MSFA: raise RuntimeError('MSFA pin mismatch')

def shape_distance(a,b):
 start=GATE_ON+4096; end=GATE_OFF-2048
 def shape(x):
  y=np.asarray(x[start:end],np.float64); s=np.abs(np.fft.rfft(y*np.hanning(len(y))))
  return s/max(float(np.linalg.norm(s)),1e-30)
 return float(np.linalg.norm(shape(a)-shape(b)))

def candidate(exe,out,level,scale,suffix=''):
 score=out/f'candidate-l{level}-s{scale:.2f}{suffix}.tsv'; raw=out/f'candidate-l{level}-s{scale:.2f}{suffix}.f32'
 score.write_text(f'0\tmod_level\t{level}\n0\tphase_scale\t{scale}\n{GATE_ON}\tgate\t1\n{GATE_OFF}\tgate\t0\n')
 run([exe,score,raw,RATE,128,FRAMES,0]); return np.fromfile(raw,dtype='<f4')

def oracle(exe,out,level,suffix=''):
 raw=out/f'oracle-l{level}{suffix}.f32'; run([exe,raw,level,FRAMES,GATE_ON,GATE_OFF]); return np.fromfile(raw,dtype='<f4')

def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--out',type=Path,required=True); ap.add_argument('--faust',required=True); ap.add_argument('--faust-libraries',required=True); args=ap.parse_args()
 out=args.out.resolve(); out.mkdir(parents=True,exist_ok=False)
 libs=Path(args.faust_libraries).resolve(); faust=Path(args.faust).resolve()
 if run([faust,'-v']).splitlines()[0].strip()!='FAUST Version 2.88.0': raise RuntimeError('requires Faust 2.88.0')
 shim=out/'faust-pinned'; shim.write_text('#!/bin/sh\nexec '+shlex.quote(str(faust))+' -I '+shlex.quote(str(libs))+' -I '+shlex.quote(str(libs/'dx7'))+' "$@"\n'); shim.chmod(0o755)
 worker=lab.Lab(out/'faust-build'); worker.faust=str(shim)
 cand=worker.build('phase-index',ROOT/'modules/dx7-reference-slice/phase_index_probe.dsp')
 msfa=out/'msfa'; checkout(msfa); native=msfa/'app/src/main/jni'; oracle_exe=out/'msfa-oracle'
 run([os.environ.get('CXX','c++'),*CPP_FLAGS,'-I'+str(native),ROOT/'tools/modules/dx7_msfa_phase_oracle.cpp',*[native/s for s in SOURCES],'-o',oracle_exe])
 refs={}; rows=[]
 for level in LEVELS:
  o=oracle(oracle_exe,out,level); o2=oracle(oracle_exe,out,level,'-repeat')
  if not np.array_equal(o,o2): raise RuntimeError('oracle repeat mismatch')
  refs[level]=o
  for scale in SCALES:
   c=candidate(cand,out,level,scale); rows.append({'level':level,'scale':scale,'distance':shape_distance(c,o)})
 by_level={}
 for level in LEVELS:
  rr=[r for r in rows if r['level']==level]; best=min(rr,key=lambda r:r['distance']); base=next(r for r in rr if r['scale']==1.0)
  by_level[str(level)]={'baseline_scale_1_distance':base['distance'],'best_scale':best['scale'],'best_distance':best['distance'],'improvement_fraction':1-best['distance']/base['distance']}
 means=[]
 for scale in SCALES:
  vals=[r['distance'] for r in rows if r['scale']==scale]; means.append({'scale':scale,'mean_distance':float(np.mean(vals))})
 global_best=min(means,key=lambda r:r['mean_distance']); baseline=next(r for r in means if r['scale']==1.0)
 # Cold-repeat the identified global scale at all three levels.
 repeats={}
 for level in LEVELS:
  a=candidate(cand,out,level,global_best['scale'],'-repeat-a'); b=candidate(cand,out,level,global_best['scale'],'-repeat-b')
  repeats[str(level)]={'sample_identical':bool(np.array_equal(a,b))}
  if not np.array_equal(a,b): raise RuntimeError('candidate repeat mismatch')
 report={'schema':1,'status':'executed','scope':'phase/index scale identification only; no DSP correction','faust_version':'2.88.0','msfa_commit':MSFA,
  'rate':RATE,'frames':FRAMES,'gate_on':GATE_ON,'gate_off':GATE_OFF,'levels':list(LEVELS),'scales':list(SCALES),
  'by_level':by_level,'global':{'baseline_scale_1_mean_distance':baseline['mean_distance'],**global_best,'improvement_fraction':1-global_best['mean_distance']/baseline['mean_distance']},
  'repeatability':repeats,'all_measurements':rows,
  'interpretation_rule':'A common best scale across levels supports a constant phase/index conversion mismatch; level-dependent optima reject that simple hypothesis. This software-oracle result is not hardware validation.'}
 (out/'phase-index.json').write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps({'global':report['global'],'by_level':by_level}))
if __name__=='__main__': main()
