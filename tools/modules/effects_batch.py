"""Analog Classics effects batch: actual Faust/C++ renders for rack reverb, tape echo and retro mixer EQ."""
from pathlib import Path
import argparse, hashlib, json, os
import numpy as np
from scipy.io import wavfile
from hats_v2_delivery import Lab, command, digest
ROOT=Path(__file__).resolve().parents[2]
SOURCES={
 'reverb':ROOT/'modules/vintage-rack-reverb/v1/reverb.dsp',
 'echo':ROOT/'modules/tape-echo/v1/echo.dsp',
 'eq':ROOT/'modules/retro-mixer-eq/v1/eq.dsp',
}
DEFAULTS={
 'reverb':dict(decay=.58,size=.52,tone=.46,character=.64,mix=.35),
 'echo':dict(time=.36,feedback=.48,tone=.58,age=.32,drive=.18,head1=1.,head2=.65,head3=.8,mix=.38),
 'eq':dict(input=.1,treble=.5,bass=.5,output=1.,mix=1.),
}
PRESETS={
 'reverb':{'Small':dict(decay=.25,size=.18,tone=.65,character=.45,mix=.28),'Rack':{},'Dark':dict(decay=.72,size=.6,tone=.2,character=.7,mix=.42),'Bloom':dict(decay=.9,size=.82,tone=.55,character=.86,mix=.48)},
 'echo':{'Slap':dict(time=.095,feedback=.16,head1=1,head2=0,head3=.2,mix=.32),'Classic':{},'Dub':dict(time=.44,feedback=.82,tone=.35,age=.62,drive=.42,head1=.35,head2=.75,head3=1,mix=.55),'Worn':dict(time=.29,feedback=.58,tone=.24,age=.9,drive=.58,head1=.8,head2=.5,head3=.9,mix=.5)},
 'eq':{'Clean':{},'Warm':dict(input=.18,bass=.62,treble=.44,output=.82),'Slam':dict(input=.42,bass=.56,treble=.62,output=.58),'Dark':dict(input=.24,bass=.72,treble=.22,output=.72)},
}

def controls(exe):
 lines=command([exe,'--controls']).splitlines();io=tuple(map(int,lines[0].split('\t')[1:]));p={}
 for line in lines[1:]:
  s=line.split('\t');p[s[0]]=tuple(map(float,s[1:4]))
 return io,p

class FxLab(Lab):
 def __init__(self,out):
  super().__init__(out);self.report.update(version='analog-classics-effects-0.1.0-experiment',human_approved=False,host_integrated=False,hardware_approved=False,presets=PRESETS)
 def render_audio(self,name,exe,values,x,sr=48000,block=128,events=None):
  x=np.asarray(x,np.float32);assert x.ndim==2
  score=self.out/(name+'.tsv');rawin=self.out/(name+'-input.f32');raw=self.out/(name+'.f32')
  rows={(0,k):float(v) for k,v in values.items()}
  for n,k,v in (events or []): rows[n,k]=float(v)
  score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
  x.tofile(rawin)
  dg=json.loads(command([exe,score,raw,sr,block,len(x),0,rawin]));y=np.fromfile(raw,'<f4').reshape(-1,dg['channels'])
  self.check(name+':finite',len(y)==len(x) and np.isfinite(y).all(),peak=float(abs(y).max()))
  self.report['renders'].append(dict(name=name,rate=sr,block=block,input_sha256=digest(rawin),raw_sha256=digest(raw),score_sha256=digest(score),diagnostics=dg))
  return y

def impulse(sr,seconds=8):
 x=np.zeros((round(sr*seconds),2),np.float32);x[round(.01*sr),:]=.5;return x

def program(sr,seconds=10):
 n=round(sr*seconds);t=np.arange(n)/sr;x=np.zeros((n,2),np.float32)
 # dry synthetic test material only: kick-like low sine bursts, short clicks and held bass tone.
 for beat in np.arange(.25,seconds-.5,.5):
  i=int(beat*sr);m=min(n-i,int(.16*sr));tt=np.arange(m)/sr
  burst=.34*np.sin(2*np.pi*(55+45*np.exp(-tt*28))*tt)*np.exp(-tt*18)
  x[i:i+m,0]+=burst;x[i:i+m,1]+=burst*.96
 for beat in np.arange(.5,seconds-.5,1.0):
  i=int(beat*sr);m=min(n-i,int(.035*sr));rng=np.random.default_rng(i);burst=(rng.standard_normal(m)*np.exp(-np.arange(m)/(sr*.008))*.14).astype(np.float32)
  x[i:i+m,0]+=burst;x[i:i+m,1]+=burst
 x[:,0]+=(.055*np.sin(2*np.pi*110*t)).astype(np.float32);x[:,1]+=(.055*np.sin(2*np.pi*110*t+.2)).astype(np.float32)
 return np.clip(x,-.8,.8)

def run(out):
 L=FxLab(out);c=L.check;r=L.render_audio;(L.out/'audition').mkdir(exist_ok=True)
 try:
  exes={};vecs={};meta={}
  for name,src in SOURCES.items():
   exes[name]=L.build(name+'-scalar',src);vecs[name]=L.build(name+'-vector',src,True);io,p=controls(exes[name]);meta[name]=p
   c(name+':stereo-io',io==(2,2),io=io);c(name+':controls',set(p)==set(DEFAULTS[name]),actual=sorted(p))
   c(name+':defaults',all(abs(p[k][2]-v)<1e-6 for k,v in DEFAULTS[name].items()))
  for name in SOURCES:
   z=np.zeros((48000,2),np.float32);y=r(name+'-silence',exes[name],DEFAULTS[name],z);c(name+':silence',float(abs(y).max())<1e-7,peak=float(abs(y).max()))
   imp=impulse(48000);base=r(name+'-impulse',exes[name],DEFAULTS[name],imp)
   c(name+':audible',float(abs(base).max())>1e-5,peak=float(abs(base).max()))
   for b in (1,32,64,127,256,512):
    y=r(name+'-block-'+str(b),exes[name],DEFAULTS[name],imp,block=b);c(name+':block-'+str(b),float(abs(y-base).max())<2e-5,residual=float(abs(y-base).max()))
   vy=r(name+'-vector',vecs[name],DEFAULTS[name],imp);c(name+':vector',float(abs(vy-base).max())<2e-4,residual=float(abs(vy-base).max()))
   for sr in (44100,96000):r(name+'-rate-'+str(sr),exes[name],DEFAULTS[name],impulse(sr,4),sr=sr)
   test=program(48000)
   live=[]
   for i,k in enumerate(DEFAULTS[name]):
    lo,hi,_=meta[name][k];live.append((48000+i*2400,k,lo));live.append((72000+i*2400,k,hi))
   y=r(name+'-live-controls',exes[name],DEFAULTS[name],test,events=live);c(name+':live-bounded',float(abs(y).max())<8,peak=float(abs(y).max()))
   banks=[]
   for preset,changes in PRESETS[name].items():
    y=r(name+'-preset-'+preset,exes[name],DEFAULTS[name]|changes,test);wavfile.write(L.out/'audition'/f'{name}_{preset}.wav',48000,y.astype(np.float32));banks.append(y)
   wavfile.write(L.out/'audition'/f'{name}_four_presets.wav',48000,np.concatenate(banks).astype(np.float32))
  # Reverb must create a tail after the dry impulse and settle by the end of a long render.
  ri=impulse(48000,14);ry=r('reverb-long-tail',exes['reverb'],DEFAULTS['reverb']|dict(mix=1,decay=.92),ri)
  early=float(np.sqrt(np.mean(ry[1000:48000]**2)));late=float(np.sqrt(np.mean(ry[-48000:]**2)));c('reverb-tail-present',early>1e-5,early=early);c('reverb-tail-decays',late<early*.25,early=early,late=late)
  # Echo impulse should produce energy after 50 ms and remain finite at high feedback.
  ei=impulse(48000,12);ey=r('echo-high-feedback',exes['echo'],DEFAULTS['echo']|dict(mix=1,feedback=.94,drive=.5),ei)
  c('echo-delayed-energy',float(np.max(abs(ey[2400:])))>1e-4);c('echo-high-feedback-bounded',float(np.max(abs(ey)))<8,peak=float(np.max(abs(ey))))
  # EQ controls must materially change low/high sine responses.
  sr=48000;t=np.arange(sr*2)/sr
  low=np.stack([.1*np.sin(2*np.pi*100*t)]*2,axis=1).astype(np.float32);high=np.stack([.1*np.sin(2*np.pi*8000*t)]*2,axis=1).astype(np.float32)
  l0=r('eq-low-default',exes['eq'],DEFAULTS['eq'],low);l1=r('eq-low-boost',exes['eq'],DEFAULTS['eq']|dict(bass=1),low)
  h0=r('eq-high-default',exes['eq'],DEFAULTS['eq'],high);h1=r('eq-high-boost',exes['eq'],DEFAULTS['eq']|dict(treble=1),high)
  c('eq-bass-effective',np.linalg.norm(l1-l0)/(np.linalg.norm(l0)+1e-20)>.05);c('eq-treble-effective',np.linalg.norm(h1-h0)/(np.linalg.norm(h0)+1e-20)>.05)
  combo=program(48000,12);a=r('chain-eq',exes['eq'],PRESETS['eq']['Warm']|DEFAULTS['eq'],combo);b=r('chain-echo',exes['echo'],PRESETS['echo']['Classic']|DEFAULTS['echo'],a);d=r('chain-reverb',exes['reverb'],PRESETS['reverb']['Rack']|DEFAULTS['reverb'],b)
  peak=float(abs(d).max());gain=min(1.,.9/(peak+1e-20));wavfile.write(L.out/'audition/00_effects_chain.wav',48000,(d*gain).astype(np.float32));L.report['chain']=dict(raw_peak=peak,audition_gain=gain,order='Retro Mixer EQ -> Tape Echo -> Vintage Rack Reverb',input='deterministic synthetic dry test material')
  L.report['source_sha256']={str(p.relative_to(ROOT)):digest(p) for p in SOURCES.values()};L.report['passed']=all(x['passed'] for x in L.report['checks'])
 except Exception as e:
  L.report.update(passed=False,error=repr(e))
 finally:
  (L.out/'results.json').write_text(json.dumps(L.report,indent=2)+'\n')
 print(json.dumps({'passed':L.report.get('passed',False),'checks':len(L.report['checks']),'renders':len(L.report['renders']),'error':L.report.get('error')}))
 if not L.report.get('passed'):raise SystemExit(1)

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
