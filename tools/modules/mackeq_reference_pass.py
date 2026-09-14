"""Whole-algorithm Airwindows MackEQ comparison for Analog Classics Retro Mixer EQ."""
from pathlib import Path
import argparse, hashlib, json, os, platform, re, time, urllib.request
import numpy as np
from scipy.io import wavfile
from airwindows_pair import PairLab, controls, program
from hats_v2_delivery import command, digest
ROOT=Path(__file__).resolve().parents[2]
PIN='03c9931839881bae6dfd4e36bfd3cced79f54b4a'
NAME='MackEQ'
V1=ROOT/'modules/retro-mixer-eq/v1/eq.dsp'
V2=ROOT/'modules/retro-mixer-eq/v2/eq.dsp'
DEFAULT=dict(input=.1,treble=.5,bass=.5,output=1.,mix=1.)
PRESETS={
 'Clean':dict(input=.1,treble=.5,bass=.5,output=.8,mix=1.),
 'Warm':dict(input=.18,treble=.43,bass=.62,output=.72,mix=1.),
 'Slam':dict(input=.34,treble=.56,bass=.58,output=.48,mix=1.),
 'Dark':dict(input=.24,treble=.26,bass=.64,output=.62,mix=1.),
}

def obtain(out):
 d=out/'upstream';d.mkdir(exist_ok=True);idx={}
 for suffix in ('.h','.cpp','Proc.cpp'):
  fn=NAME+suffix;p=d/fn
  url=f'https://raw.githubusercontent.com/airwindows/airwindows/{PIN}/plugins/LinuxVST/src/{NAME}/{fn}'
  if not p.exists():
   with urllib.request.urlopen(url,timeout=25) as r:p.write_bytes(r.read())
  idx[fn]={'url':url,'sha256':digest(p),'bytes':p.stat().st_size}
 (d/'sources.json').write_text(json.dumps({'commit':PIN,'files':idx},indent=2))
 return d,idx

def oracle_header(src,seed=0):
 h=(src/(NAME+'.h')).read_text();cpp=(src/(NAME+'.cpp')).read_text();proc=(src/(NAME+'Proc.cpp')).read_text()
 fields=h.split('private:',1)[1].rsplit('};',1)[0]
 fields='\n'.join(s for s in fields.splitlines() if '_programName' not in s and '_canDo' not in s)
 init=cpp.split('AudioEffectX(audioMaster, kNumPrograms, kNumParameters)',1)[1].split('{',1)[1].split('_canDo.insert',1)[0]
 init='\n'.join(s for s in init.splitlines() if not ('fpdL =' in s or 'fpdR =' in s))
 method=proc[proc.index('void '+NAME+'::processDoubleReplacing'):].replace(NAME+'::','',1)
 ui='ui->addHorizontalSlider("input",&A,.1,0,1,.001);ui->addHorizontalSlider("treble",&B,.5,0,1,.001);ui->addHorizontalSlider("bass",&C,.5,0,1,.001);ui->addHorizontalSlider("output",&D,1,0,1,.001);ui->addHorizontalSlider("mix",&E,1,0,1,.001);'
 return '''using VstInt32=int32_t; class ModuleDSP : public dsp { public:\n'''+fields+'''\nint rate=48000;double inL[8192],inR[8192],outL[8192],outR[8192];float getSampleRate(){return float(rate);}int getNumInputs(){return 2;}int getNumOutputs(){return 2;}void init(int sr){rate=sr;\n'''+init+f'\nfpdL={seed}u;fpdR={seed}u;\n'+'}\nvoid buildUserInterface(UI* ui){'+ui+'}\n'+method+'''\nvoid compute(int n,float** ins,float** outs){for(int i=0;i<n;i++){inL[i]=ins[0][i];inR[i]=ins[1][i];}double* ip[2]={inL,inR};double* op[2]={outL,outR};processDoubleReplacing(ip,op,n);for(int i=0;i<n;i++){outs[0][i]=float(outL[i]);outs[1][i]=float(outR[i]);}}};\n'''

class Lab(PairLab):
 def native(self,name,header):
  d=self.out/name;d.mkdir(exist_ok=True);(d/'generated.hpp').write_text(header)
  t=time.perf_counter();command([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render'])
  self.report['builds'][name]={'kind':'original-C++-adapter','generated_sha256':digest(d/'generated.hpp'),'cxx_seconds':time.perf_counter()-t};return d/'render'
 def faust(self,name,path,double=False,vector=False):
  d=self.out/name;d.mkdir(exist_ok=True);args=[os.getenv('FAUST','faust'),'-I',path.parent,'-lang','cpp','-double' if double else '-single','-cn','ModuleDSP']
  if vector:args+=['-vec','-lv','0','-vs','32']
  t=time.perf_counter();command(args+[path,'-o',d/'generated.hpp']);ft=time.perf_counter()-t
  t=time.perf_counter();command([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render']);ct=time.perf_counter()-t
  self.report['builds'][name]={'kind':'actual-Faust','source':str(path.relative_to(ROOT)),'source_sha256':digest(path),'generated_sha256':digest(d/'generated.hpp'),'faust_args':list(map(str,args)),'faust_seconds':ft,'cxx_seconds':ct,'controls':command([d/'render','--controls'])};return d/'render'

def errors(y,ref):
 y=y.astype(float);ref=ref.astype(float);d=y-ref;rms=lambda x:float(np.sqrt(np.mean(x*x)));rr=rms(ref);er=rms(d)
 return {'max_abs':float(abs(d).max()),'rms_error':er,'reference_rms':rr,'relative_rms':er/max(rr,1e-30),'residual_db':20*np.log10(max(er,1e-30)/max(rr,1e-30)),'gain_db':20*np.log10(max(rms(y),1e-30)/max(rr,1e-30))}

def stimulus(sr,seconds=3):
 n=round(sr*seconds);t=np.arange(n)/sr;rng=np.random.default_rng(1202)
 x=program(sr,seconds).astype(float);x+=rng.uniform(-.02,.02,(n,2));x[:,0]+=.035*np.sin(2*np.pi*9000*t);x[:,1]+=.025*np.sin(2*np.pi*13000*t);x[:19]=0;return x.astype(np.float32)

def run(out):
 L=Lab(out);c=L.check;r=L.render_audio;start=time.perf_counter();(L.out/'audition').mkdir(exist_ok=True)
 L.report.update(version='mackeq-reference-pass-0.1',commit=os.environ.get('GITHUB_SHA','local'),oracle_approved=False,comparisons=[],benchmarks={},source_pin=PIN,presets=PRESETS)
 try:
  src,idx=obtain(L.out);L.report['upstream']=idx
  ex={'original':L.native('mackeq-original',oracle_header(src)),'previous':L.faust('mackeq-v1',V1),'revised':L.faust('mackeq-v2',V2),'double':L.faust('mackeq-v2-double',V2,True)}
  for label,exe in ex.items():
   io,p=controls(exe);c(label+':contract',io==(2,2) and set(p)==set(DEFAULT),io=io,controls=sorted(p))
  settings=[('Default',DEFAULT)]+[(k,DEFAULT|v) for k,v in PRESETS.items()]+[
   ('BassMin',DEFAULT|{'bass':0}),('BassMax',DEFAULT|{'bass':1}),('TrebleMin',DEFAULT|{'treble':0}),('TrebleMax',DEFAULT|{'treble':1}),('Hot',DEFAULT|{'input':.65,'output':.35}),('HalfWet',DEFAULT|{'mix':.5})]
  for sr in (44100,48000,96000):
   for name,p in settings:
    x=stimulus(sr);ys={label:r(f'{name}-{sr}-{label}',exe,p,x,sr=sr) for label,exe in ex.items()}
    ep=errors(ys['previous'],ys['original']);en=errors(ys['revised'],ys['original']);ed=errors(ys['double'],ys['original'])
    L.report['comparisons'] += [dict(setting=name,rate=sr,implementation='previous',**ep),dict(setting=name,rate=sr,implementation='revised',**en),dict(setting=name,rate=sr,implementation='double',**ed)]
    c(f'{name}-{sr}:improved',en['relative_rms']<ep['relative_rms'],previous=ep['relative_rms'],revised=en['relative_rms'])
    c(f'{name}-{sr}:reference-single',en['relative_rms']<3e-4,relative_rms=en['relative_rms'])
    c(f'{name}-{sr}:reference-double',ed['relative_rms']<2e-6,relative_rms=ed['relative_rms'])
  x=stimulus(48000,4);events=[(12007,'input',.42),(36011,'bass',.8),(60013,'treble',.2),(90017,'output',.55),(120019,'mix',.45)]
  ys={label:r('automation-'+label,exe,DEFAULT,x,events=events) for label,exe in ex.items()}
  for label in ('previous','revised','double'):L.report['comparisons'].append(dict(setting='automation',rate=48000,implementation=label,**errors(ys[label],ys['original'])))
  c('automation-single',errors(ys['revised'],ys['original'])['relative_rms']<5e-4,**errors(ys['revised'],ys['original']))
  c('automation-double',errors(ys['double'],ys['original'])['relative_rms']<3e-6,**errors(ys['double'],ys['original']))
  for b in (1,32,127,512):
   y=r('partition-'+str(b),ex['revised'],DEFAULT,x,block=b,events=events);c('partition-'+str(b),np.array_equal(y,ys['revised']),max_abs=float(abs(y-ys['revised']).max()))
  vec=L.faust('mackeq-v2-vector',V2,vector=True);y=r('vector',vec,DEFAULT,x,events=events);c('vector',float(abs(y-ys['revised']).max())<2e-4,max_abs=float(abs(y-ys['revised']).max()))
  z=np.zeros((48000,2),np.float32);sil={k:r('silence-'+k,e,DEFAULT,z) for k,e in ex.items()};c('silence',all(not np.any(v) for v in sil.values()))
  # Audition original / previous / revised, no normalization or fitting.
  p=DEFAULT|PRESETS['Warm'];x=stimulus(48000,8);aud=[];wavfile.write(L.out/'audition'/'mackeq_dry.wav',48000,x)
  for label in ('original','previous','revised'):
   y=r('audition-'+label,ex[label],p,x);wavfile.write(L.out/'audition'/f'mackeq_{label}.wav',48000,y.astype(np.float32));aud.append(y)
  wavfile.write(L.out/'audition'/'mackeq_original_previous_revised.wav',48000,np.concatenate(aud).astype(np.float32))
  # Wrong output gain must be detectable.
  h=(L.out/'mackeq-v2'/'generated.hpp').read_text();mut=h.replace('output0[i0] =','output0[i0] = 0.99 *',1)
  c('mutation-created',mut!=h)
  if mut!=h:
   bad=L.native('mackeq-negative-gain',mut);xx=stimulus(48000,.5);good=r('negative-good',ex['revised'],DEFAULT,xx);wrong=r('negative-bad',bad,DEFAULT,xx);c('wrong-gain-detected',errors(wrong,good)['relative_rms']>.005)
  # Matched compute timing, five repeats on the same runner.
  xx=stimulus(48000,5)
  for label in ('original','previous','revised'):
   vals=[]
   for i in range(5):r(f'bench-{label}-{i}',ex[label],DEFAULT|PRESETS['Warm'],xx);vals.append(L.report['renders'][-1]['diagnostics'])
   L.report['benchmarks'][label]=vals
  L.report['passed']=all(q['passed'] for q in L.report['checks'])
 except Exception as e:
  L.report.update(passed=False,error=repr(e));raise
 finally:
  L.report['suite_wall_seconds']=time.perf_counter()-start;(L.out/'results.json').write_text(json.dumps(L.report,indent=2)+'\n')
 print(json.dumps({'passed':L.report.get('passed',False),'checks':len(L.report['checks']),'renders':len(L.report['renders']),'error':L.report.get('error')}))
 if not L.report.get('passed'):raise SystemExit(1)

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
