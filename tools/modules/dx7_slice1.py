#!/usr/bin/env python3
"""Execute #112 H1 against Faust 2.88 dx7/operator.lib and pinned MSFA.
This is identification evidence, not hardware validation or sonic acceptance.
"""
from __future__ import annotations
import argparse, hashlib, json, math, os, shutil, subprocess, tarfile, urllib.request
from pathlib import Path
import numpy as np

ROOT=Path(__file__).resolve().parents[2]
FAUST_VERSION='2.88.0'
FAUST_ARCHIVE_SHA256='e4e175cf236924b5b7d4784cbb8c50cc01e211159e169655dd9e6d8f92b871d9'
MSFA_COMMIT='f67d41d313b7dc85f6fb99e79e515cc9d208cfff'
RATE=44100; FRAMES=44100; NOTEOFF=22016
CASES={
 'DX7-C01':{'carrier_level':85,'mod_level':0,'mod_ratio':2,'mod_r1':99,'mod_r2':99,'mod_r3':99,'mod_r4':99,'mod_l1':99,'mod_l2':99,'mod_l3':99,'mod_l4':0},
 'DX7-C02':{'carrier_level':85,'mod_level':75,'mod_ratio':2,'mod_r1':99,'mod_r2':99,'mod_r3':99,'mod_r4':99,'mod_l1':99,'mod_l2':99,'mod_l3':99,'mod_l4':0},
 'DX7-C03':{'carrier_level':85,'mod_level':90,'mod_ratio':2,'mod_r1':80,'mod_r2':60,'mod_r3':50,'mod_r4':65,'mod_l1':99,'mod_l2':70,'mod_l3':55,'mod_l4':0},
}

def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def run(cmd, cwd=ROOT, timeout=300):
 p=subprocess.run([str(x) for x in cmd],cwd=cwd,capture_output=True,text=True,timeout=timeout)
 if p.returncode: raise RuntimeError(f"command failed {cmd}\n{p.stdout}\n{p.stderr}")
 return p.stdout

def metrics(x):
 x=np.asarray(x,dtype=np.float64); peak=float(np.max(np.abs(x))); rms=float(np.sqrt(np.mean(x*x)))
 spec=np.abs(np.fft.rfft(x*np.hanning(len(x))))
 freqs=np.fft.rfftfreq(len(x),1/RATE); k=int(np.argmax(spec[1:])+1)
 return {'peak':peak,'rms':rms,'dominant_hz':float(freqs[k]),'dc':float(np.mean(x))}

def compare(a,b):
 d=a.astype(np.float64)-b.astype(np.float64)
 # Raw residual is intentionally unaligned and gain-preserving.
 A=np.abs(np.fft.rfft(a*np.hanning(len(a)))); B=np.abs(np.fft.rfft(b*np.hanning(len(b))))
 ld=20*np.log10(np.maximum(A,1e-12))-20*np.log10(np.maximum(B,1e-12))
 return {'raw_rmse':float(np.sqrt(np.mean(d*d))), 'raw_max_abs':float(np.max(np.abs(d))),
         'log_spectrum_rmse_db':float(np.sqrt(np.mean(ld*ld))),
         'gain_invariant_log_spectrum_rmse_db':float(np.sqrt(np.mean((ld-ld.mean())**2)))}

def fetch_msfa(out):
 arc=out/'msfa.tar.gz'; src=out/'msfa-src'
 if not arc.exists(): urllib.request.urlretrieve(f'https://github.com/google/music-synthesizer-for-android/archive/{MSFA_COMMIT}.tar.gz',arc)
 if src.exists(): shutil.rmtree(src)
 src.mkdir();
 with tarfile.open(arc) as t:
  root=t.getmembers()[0].name.split('/')[0]
  for m in t.getmembers():
   if m.name.startswith(root+'/app/src/main/jni/'):
    m.name=m.name[len(root+'/app/src/main/jni/'):]
    if m.name: t.extract(m,src,filter='data')
 return src,arc

def build(out,faust,libs):
 cand=out/'candidate'; cand.mkdir(exist_ok=True)
 source=ROOT/'modules/dx7-reference-slice/dx7_slice.dsp'
 run([faust,'-I',libs,'-e',source,'-o',cand/'expanded.dsp'])
 run([faust,'-I',libs,'-lang','cpp','-single','-cn','ModuleDSP',cand/'expanded.dsp','-o',cand/'generated.hpp'])
 cxx=os.environ.get('CXX','c++')
 run([cxx,'-std=c++17','-O2','-ffp-contract=off','-I'+str(cand),ROOT/'tools/modules/render.cpp','-o',cand/'render'])
 msfa,arc=fetch_msfa(out); oracle=out/'msfa-oracle'
 sources=['dx7note.cc','env.cc','pitchenv.cc','fm_core.cc','fm_op_kernel.cc','freqlut.cc','exp2.cc','sin.cc']
 run([cxx,'-std=c++17','-O2','-ffp-contract=off','-I'+str(msfa),ROOT/'tools/modules/dx7_msfa_oracle.cpp',*[msfa/s for s in sources],'-o',oracle])
 return cand/'render',oracle,arc,{'candidate_source_sha256':sha(source),'expanded_sha256':sha(cand/'expanded.dsp'),'generated_sha256':sha(cand/'generated.hpp'),'candidate_binary_sha256':sha(cand/'render'),'oracle_adapter_sha256':sha(ROOT/'tools/modules/dx7_msfa_oracle.cpp'),'oracle_binary_sha256':sha(oracle),'msfa_archive_sha256':sha(arc)}

def render_candidate(exe,out,id,params,suffix=''):
 score=out/f'{id}-candidate{suffix}.tsv'; raw=out/f'{id}-candidate{suffix}.f32'
 values={'frequency_hz':261.625565,'velocity_gain':1,**params}
 rows=[f'0\t{k}\t{v}\n' for k,v in values.items()]+['0\tgate\t1\n',f'{NOTEOFF}\tgate\t0\n']
 # Runner requires rows ordered by frame; all frame-zero controls precede note-off.
 score.write_text(''.join(rows))
 run([exe,score,raw,RATE,64,FRAMES,0])
 return raw,np.fromfile(raw,dtype='<f4')

def render_oracle(exe,out,id,suffix=''):
 raw=out/f'{id}-oracle{suffix}.f32'; run([exe,id,raw,RATE,FRAMES,NOTEOFF]); return raw,np.fromfile(raw,dtype='<f4')

def wav(raw):
 dst=raw.with_suffix('.wav')
 run(['ffmpeg','-v','error','-y','-f','f32le','-ar',RATE,'-ac',1,'-i',raw,'-c:a','pcm_f32le',dst])
 return dst

def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--out',type=Path,required=True); ap.add_argument('--faust',required=True); ap.add_argument('--faust-libraries',required=True); ap.add_argument('--faust-archive',type=Path,required=True); args=ap.parse_args()
 out=args.out.resolve(); out.mkdir(parents=True,exist_ok=True)
 if sha(args.faust_archive)!=FAUST_ARCHIVE_SHA256: raise RuntimeError('Faust 2.88 archive hash mismatch')
 version=run([args.faust,'--version']).strip()
 if f'FAUST Version {FAUST_VERSION}' not in version: raise RuntimeError(version)
 candidate,oracle,msfa_arc,builds=build(out,args.faust,args.faust_libraries)
 report={'schema':1,'status':'executed','scope':'DX7 H1 software-oracle baseline; no DSP modification or hardware validation','rate':RATE,'frames':FRAMES,'noteoff_frame':NOTEOFF,'oracle_quantum_frames':64,
         'provenance':{'faust_version':version,'faust_archive_sha256':sha(args.faust_archive),'faust_libraries_root':str(Path(args.faust_libraries).resolve()),'msfa_commit':MSFA_COMMIT,**builds},'cases':{}}
 for id,params in CASES.items():
  cr,c=render_candidate(candidate,out,id,params); oraw,o=render_oracle(oracle,out,id)
  cr2,c2=render_candidate(candidate,out,id,params,'-repeat'); oraw2,o2=render_oracle(oracle,out,id,'-repeat')
  if not np.array_equal(c,c2): raise RuntimeError(id+' candidate repeat mismatch')
  if not np.array_equal(o,o2): raise RuntimeError(id+' oracle repeat mismatch')
  cw,ow=wav(cr),wav(oraw)
  report['cases'][id]={'params':params,'candidate':metrics(c),'oracle':metrics(o),'comparison':compare(c,o),
      'repeatability':{'candidate_exact':True,'oracle_exact':True},
      'artifacts':{'candidate_raw_sha256':sha(cr),'oracle_raw_sha256':sha(oraw),'candidate_wav_sha256':sha(cw),'oracle_wav_sha256':sha(ow)}}
 # Negative control: deliberately halve candidate amplitude; comparison must materially change.
 id='DX7-C02'; _,base=render_candidate(candidate,out,id,CASES[id],'-negative-base')
 mutated=base*.5
 good=compare(base,np.fromfile(out/f'{id}-oracle.f32',dtype='<f4'))['raw_rmse']
 bad=compare(mutated,np.fromfile(out/f'{id}-oracle.f32',dtype='<f4'))['raw_rmse']
 report['negative_control']={'mutation':'candidate amplitude * 0.5','baseline_raw_rmse':good,'mutated_raw_rmse':bad,'detected':abs(bad-good)>1e-6}
 if not report['negative_control']['detected']: raise RuntimeError('negative control not detected')
 (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n')
 (out/'README.txt').write_text('DX7 #112 H1 baseline. Raw/WAV files are unaligned and unnormalized. MSFA is a software oracle sharing lineage with the Faust implementation; this is not independent hardware validation.\n')
 print(json.dumps({'cases':list(CASES),'negative_control':'PASS','report':str(out/'comparison.json')}))
if __name__=='__main__': main()
