#!/usr/bin/env python3
"""#107: run pinned Faust filter-bank descriptors over an existing mono f32 render."""
from __future__ import annotations
import argparse, hashlib, json, os, pathlib, subprocess, tempfile
import numpy as np

ROOT=pathlib.Path(__file__).resolve().parents[2]
DSP=ROOT/'tools/modules/faust_spectral_descriptors.dsp'
RUNNER=ROOT/'tools/modules/faust_analysis_runner.cpp'
CONFIG={'filter_order':3,'bands_per_octave':3,'top_hz':10000,'bands':24,'power_tau_seconds':0.02,'flux_hop_seconds':0.02}
FIELDS=['input','faust_filterbank_centroid_hz','faust_filterbank_spread_hz','faust_filterbank_flux']

def sha(p): return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def run(a,cwd=ROOT):
 p=subprocess.run([str(x) for x in a],cwd=cwd,capture_output=True,text=True,timeout=180)
 if p.returncode: raise RuntimeError(f"failed: {a}\n{p.stdout}\n{p.stderr}")
 return p.stdout.strip()

def build(out,faust,cxx,libs):
 out.mkdir(parents=True,exist_ok=True); gen=out/'analysis_generated.hpp'; exe=out/'runner'
 version=run([faust,'--version'])
 if 'FAUST Version 2.88.0' not in version: raise ValueError('spectral traces require pinned Faust 2.88.0')
 run([faust,'-lang','cpp','-single','-cn','ModuleDSP','-I',libs,DSP,'-o',gen])
 run([cxx,'-std=c++17','-O2','-ffp-contract=off','-I'+str(out),RUNNER,'-o',exe])
 return exe,{'faust_version':version,'dsp_sha256':sha(DSP),'generated_sha256':sha(gen),'runner_sha256':sha(exe),'config':CONFIG}

def analyze(src,rate,out,runner,provenance,stride):
 src=pathlib.Path(src); out=pathlib.Path(out); out.mkdir(parents=True,exist_ok=True)
 x=np.fromfile(src,dtype='<f4');
 if not len(x) or not np.isfinite(x).all(): raise ValueError('invalid mono f32 input')
 with tempfile.TemporaryDirectory(dir=out) as d:
  raw=pathlib.Path(d)/'trace.f32'; run([runner,src,raw,rate,len(x),256,0])
  y=np.fromfile(raw,dtype='<f4').reshape(len(x),4)
 if not np.array_equal(y[:,0],x): raise RuntimeError('descriptor passthrough changed audio')
 frames=list(range(0,len(x),stride)); trace=[]
 for frame in frames:
  trace.append({'frame':frame,'time_seconds':frame/rate,
                FIELDS[1]:float(y[frame,1]),FIELDS[2]:float(y[frame,2]),FIELDS[3]:float(y[frame,3])})
 report={'schema':1,'input':{'path':str(src),'sha256':sha(src),'rate':rate,'channels':1,'frames':len(x),'format':'mono-f32le'},
         'descriptor_family':'faust_filterbank_not_fft','trace_stride_frames':stride,'trace_stride_seconds':stride/rate,
         'provenance':provenance,'trace':trace,
         'notes':['These descriptors use Faust constant-Q/filter-bank analysis, not the existing FFT metrics.',
                  'Values are diagnostics; no existing reference score or release threshold is changed.']}
 (out/'spectral-trace.json').write_text(json.dumps(report,indent=2,sort_keys=True,allow_nan=False)+'\n')
 return report

def main():
 p=argparse.ArgumentParser();p.add_argument('input');p.add_argument('--rate',type=int,required=True);p.add_argument('--out',type=pathlib.Path,required=True)
 p.add_argument('--stride',type=int,default=240);p.add_argument('--faust',default=os.environ.get('FAUST','faust'));p.add_argument('--faust-libraries',default=os.environ.get('FAUST_LIBRARIES'));p.add_argument('--cxx',default=os.environ.get('CXX','c++'))
 a=p.parse_args()
 if not 8000<=a.rate<=192000 or a.stride<1 or not a.faust_libraries: p.error('invalid rate/stride or missing --faust-libraries')
 runner,prov=build(a.out/'build',a.faust,a.cxx,a.faust_libraries); report=analyze(a.input,a.rate,a.out,runner,prov,a.stride)
 print(json.dumps({'report':str(a.out/'spectral-trace.json'),'points':len(report['trace'])}))
if __name__=='__main__': main()
