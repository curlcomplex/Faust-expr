#!/usr/bin/env python3
"""Analyze existing float renders with a separately pinned Faust toolchain.

This deliberately does not compile instrument DSP. It accepts interleaved raw
float32 audio and records both existing simple measurements and Faust analyzer
outputs, keeping analysis-version changes independent from sound-generation.
"""
from __future__ import annotations
import argparse, hashlib, json, os, pathlib, subprocess
import numpy as np

ROOT=pathlib.Path(__file__).resolve().parents[2]
DSP=ROOT/'tools/modules/faust_analysis_meter.dsp'
RUNNER=ROOT/'tools/modules/faust_analysis_runner.cpp'

def sha(p): return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def run(a):
 p=subprocess.run([str(x) for x in a],cwd=ROOT,capture_output=True,text=True)
 if p.returncode: raise RuntimeError(f"command failed: {a}\n{p.stdout}\n{p.stderr}")
 return p.stdout.strip()

def build(out,faust,cxx,library_path=None):
 out.mkdir(parents=True,exist_ok=True); gen=out/'analysis_generated.hpp'; exe=out/'analysis_runner'
 faust=pathlib.Path(faust).resolve() if pathlib.Path(faust).exists() else pathlib.Path(faust)
 version=run([faust,'--version'])
 flags=['-lang','cpp','-single','-cn','ModuleDSP']
 if library_path:
  library_path=pathlib.Path(library_path).resolve()
  if not (library_path/'stdfaust.lib').is_file(): raise RuntimeError(f'Faust library path has no stdfaust.lib: {library_path}')
  flags += ['-I',str(library_path)]
 run([faust,*flags,str(DSP),'-o',str(gen)])
 run([cxx,'-std=c++17','-O2','-ffp-contract=off','-I'+str(out),str(RUNNER),'-o',str(exe)])
 return exe,{'faust_version':version,'analysis_dsp_sha256':sha(DSP),'generated_sha256':sha(gen),
             'runner_source_sha256':sha(RUNNER),'runner_binary_sha256':sha(exe),
             'faust_flags':flags,'faust_library_path':str(library_path) if library_path else None,
             'stdfaust_sha256':sha(library_path/'stdfaust.lib') if library_path else None,
             'cxx_flags':['-std=c++17','-O2','-ffp-contract=off']}

def main():
 ap=argparse.ArgumentParser(); ap.add_argument('input'); ap.add_argument('--rate',type=int,required=True)
 ap.add_argument('--channels',type=int,required=True); ap.add_argument('--out',type=pathlib.Path,required=True)
 ap.add_argument('--faust',default=os.environ.get('FAUST','faust')); ap.add_argument('--faust-libraries',default=os.environ.get('FAUST_LIBRARIES'))
 ap.add_argument('--cxx',default=os.environ.get('CXX','c++'))
 a=ap.parse_args(); src=pathlib.Path(a.input)
 if not 8000<=a.rate<=192000 or not 1<=a.channels<=32: raise SystemExit('invalid rate/channels')
 x=np.fromfile(src,dtype='<f4');
 if not len(x) or len(x)%a.channels or not np.isfinite(x).all(): raise SystemExit('invalid input')
 x=x.reshape(-1,a.channels); a.out.mkdir(parents=True,exist_ok=True)
 exe,prov=build(a.out/'build',a.faust,a.cxx,a.faust_libraries); per=[]
 for ch in range(a.channels):
  inp=a.out/f'ch{ch}.f32'; raw=a.out/f'ch{ch}-analysis.f32'; np.asarray(x[:,ch],dtype='<f4').tofile(inp)
  run([exe,inp,raw,a.rate,len(x)]); y=np.fromfile(raw,dtype='<f4').reshape(len(x),6)
  if not np.isfinite(y).all(): raise RuntimeError('nonfinite analyzer output')
  per.append({'channel':ch,'sample_peak':float(np.max(np.abs(x[:,ch]))),
              'rms':float(np.sqrt(np.mean(np.asarray(x[:,ch],dtype=np.float64)**2))),
              'faust_true_peak_estimate_max':float(np.max(y[:,1])),
              'faust_true_peak_hold_final':float(y[-1,2]),
              'faust_loudness_momentary_final_lufs':float(y[-1,3]),
              'faust_loudness_shortterm_final_lufs':float(y[-1,4]),
              'faust_loudness_integrated_streaming_approx_final_lufs':float(y[-1,5])})
  inp.unlink(); raw.unlink()
 report={'schema':1,'input':{'path':str(src),'sha256':sha(src),'rate':a.rate,'channels':a.channels,'frames':len(x)},
         'provenance':prov,'channels':per,
         'notes':['Faust integrated loudness is reported as a streaming approximation, not exact offline two-pass loudness.',
                  'No normalization or release threshold is implied by these measurements.']}
 (a.out/'analysis.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
 print(json.dumps(report,indent=2,sort_keys=True))
if __name__=='__main__': main()
