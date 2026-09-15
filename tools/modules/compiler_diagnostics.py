#!/usr/bin/env python3
"""Issue #109: reproducible Faust compiler/interpreter diagnostics.

Compiler warnings are evidence by default. Compile failure, -me domain errors and
interp-tracer fatal numerical/memory faults are hard failures when scanning a
production DSP. Deliberate negative fixtures invert that expectation in tests.
"""
from __future__ import annotations
import argparse, hashlib, json, pathlib, subprocess, sys, time

ROOT=pathlib.Path(__file__).resolve().parents[2]

def sha(p): return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def run(cmd, timeout=60):
 p=subprocess.run([str(x) for x in cmd],capture_output=True,text=True,timeout=timeout)
 return {'command':[str(x) for x in cmd],'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
def classify_compile(result):
 text=result['stdout']+result['stderr']
 return {'hard_failure':result['returncode']!=0,
         'warning_present':'warning' in text.lower(),
         'policy':'compile failure hard; warnings informational unless separately promoted by demonstrated correctness impact'}
def diagnose(dsp,faust,libs,out,tracer=None,trace=4,structural=False):
 dsp=pathlib.Path(dsp).resolve(); out=pathlib.Path(out);out.mkdir(parents=True,exist_ok=True)
 version=run([faust,'--version']);
 if version['returncode']: raise RuntimeError('faust --version failed')
 compile_result=run([faust,'-wall','-me','-lang','cpp','-I',libs,dsp,'-o',out/'diagnostic.cpp'])
 result={'schema':1,'input':{'path':str(dsp),'sha256':sha(dsp)},'faust_version':version['stdout'].strip(),
         'compiler':compile_result,'compiler_classification':classify_compile(compile_result),
         'execution_lane':'github-hosted' if 'GITHUB_ACTIONS' in __import__('os').environ else 'local-unspecified',
         'timestamp_unix':time.time(),'interp_tracer':None,'structural':None,
         'notes':['-wall warnings are informational by default; compilation errors are hard failures.',
                  '-me enables Faust math-domain checking; it is not a runtime tracer.',
                  'interp-tracer is interpreter-backend diagnostics, not sonic acceptance or a performance benchmark.']}
 if tracer:
  tr=run([tracer,'-trace',str(trace),'-noui','-timeout','2','-I',libs,dsp],timeout=15)
  result['interp_tracer']={**tr,'trace_mode':trace,'hard_failure':tr['returncode']!=0,
      'policy':'trace 4 fatal FP infinite/NaN/cast/load-store faults are hard diagnostics; other counters remain evidence'}
 if structural:
  help_result=run([faust,'-h'])
  supported='-sig' in (help_result['stdout']+help_result['stderr'])
  sr={'backend':'ocpp','requested':'-sig','supported_by_help':supported,
      'interpretation':'compiler structure only; never runtime-performance evidence'}
  if supported:
   sr['result']=run([faust,'-lang','ocpp','-sig','-I',libs,dsp,'-o',out/'structural.cpp'])
  result['structural']=sr
 (out/'diagnostics.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
 return result

def main():
 p=argparse.ArgumentParser();p.add_argument('dsp');p.add_argument('--faust',required=True);p.add_argument('--faust-libraries',required=True)
 p.add_argument('--interp-tracer');p.add_argument('--trace',type=int,default=4);p.add_argument('--structural',action='store_true');p.add_argument('--out',type=pathlib.Path,required=True)
 a=p.parse_args()
 if a.trace not in range(1,7): p.error('--trace must be 1..6')
 r=diagnose(a.dsp,a.faust,a.faust_libraries,a.out,a.interp_tracer,a.trace,a.structural)
 print(json.dumps({'report':str(a.out/'diagnostics.json'),'compiler_hard_failure':r['compiler_classification']['hard_failure'],
                   'tracer_hard_failure':None if r['interp_tracer'] is None else r['interp_tracer']['hard_failure']}))
 return 2 if r['compiler_classification']['hard_failure'] or (r['interp_tracer'] and r['interp_tracer']['hard_failure']) else 0
if __name__=='__main__': raise SystemExit(main())
