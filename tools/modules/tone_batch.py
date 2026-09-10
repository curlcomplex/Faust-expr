"""Full offline Tone PM qualification and listening evidence."""
from __future__ import annotations
import argparse, hashlib, itertools, json, math, os, shutil, statistics, subprocess, wave
from pathlib import Path
import numpy as np
from scipy.io import wavfile
ROOT=Path(__file__).resolve().parents[2]; MOD=ROOT/'modules/tone-pm'
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def cmd(args,timeout=180):
 p=subprocess.run(list(map(str,args)),cwd=ROOT,capture_output=True,text=True,timeout=timeout)
 if p.returncode: raise RuntimeError(f'{args}\n{p.stdout}\n{p.stderr}')
 return p.stdout
def wav(path,x,rate=48000):
 x=np.asarray(x); assert np.isfinite(x).all() and np.max(np.abs(x))<1
 with wave.open(str(path),'wb') as f:f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate);f.writeframes(np.rint(x*32767).astype('<i2').tobytes())
def f0(x,rate):
 y=np.asarray(x,dtype=float);y-=y.mean();n=1<<max(18,(len(y)-1).bit_length());s=np.abs(np.fft.rfft(y*np.hanning(len(y)),n=n));s[0]=0;k=int(s.argmax());return k*rate/n
def desc(x,rate=48000):
 x=np.asarray(x,dtype=float);e=x*x;tot=max(e.sum(),1e-30);cum=np.cumsum(e)/tot
 spec=np.abs(np.fft.rfft(x*np.hanning(len(x))))**2;freq=np.fft.rfftfreq(len(x),1/rate);st=max(spec.sum(),1e-30)
 centroid=float((spec*freq).sum()/st);flat=float(np.exp(np.mean(np.log(spec+1e-20)))/(np.mean(spec)+1e-20))
 return [float(np.searchsorted(cum,.5)/rate),float(np.searchsorted(cum,.9)/rate),math.log1p(centroid)/10,flat,float(np.sqrt(np.mean(x*x)))]
def distance(a,b):
 a=np.array(a);b=np.array(b);scale=np.array([.5,1,.25,.2,.1]);return float(np.sqrt(np.mean(((a-b)/scale)**2)))
class Study:
 def __init__(self,out,refs):
  self.out=out;out.mkdir(parents=True,exist_ok=True);self.refs=refs;self.man=json.loads((MOD/'manifest.json').read_text());self.defaults={k:v['default'] for k,v in self.man['controls'].items()};self.patches=json.loads((MOD/'patches.json').read_text())['anchors'];self.report={'checks':[],'renders':[],'passed':False}
 def check(self,n,ok,**d):self.report['checks'].append(dict(name=n,passed=bool(ok),**d));assert ok,(n,d)
 def build(self,label,vec=False):
  d=self.out/label;d.mkdir(exist_ok=True);flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vec else []);cmd([os.getenv('FAUST','faust'),*flags,MOD/'tone.dsp','-o',d/'generated.hpp']);cmd([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render']);self.report.setdefault('builds',{})[label]={'generated_sha256':sha(d/'generated.hpp'),'flags':flags};return d/'render'
 def render(self,label,exe,p=None,events=None,seconds=2,rate=48000,block=128):
  vals=self.defaults|(p or {});rows={(0,k):v for k,v in vals.items()};
  for ev in events or []: rows[(ev[0],ev[1])]=ev[2]
  sc=self.out/(label+'.tsv');sc.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())));raw=self.out/(label+'.f32');diag=json.loads(cmd([exe,sc,raw,rate,block,round(seconds*rate),0]));x=np.fromfile(raw,dtype='<f4');self.check(label+':finite',np.isfinite(x).all() and np.max(np.abs(x))<.95,peak=float(np.max(np.abs(x))));self.report['renders'].append({'label':label,'raw_sha256':sha(raw),'score_sha256':sha(sc),'peak':float(np.max(np.abs(x))),'diag':diag});return x
 @staticmethod
 def hit(n,length=64):return [(n,'gate',1),(n+length,'gate',0)]
 def run(self):
  scalar=self.build('scalar');vector=self.build('vector',True);self.report['source_commit']=cmd(['git','rev-parse','HEAD']).strip();self.report['source_sha256']=sha(MOD/'tone.dsp')
  z=self.render('silence',scalar,seconds=.2);self.check('initial-silence',not np.any(z))
  for rate in (44100,48000,96000):
   for hz in (55,220,880):
    x=self.render(f'pitch-{rate}-{hz}',scalar,{'pitch_hz':hz,'ratio':.4,'feedback':0,'modulation':0,'punch':0,'drive':0,'decay':.8},self.hit(round(.05*rate)),seconds=1.4,rate=rate)
    m=f0(x[round(.25*rate):round(1.25*rate)],rate);self.check(f'pitch:{rate}:{hz}',abs(m-hz)<.5,measured=m)
  ev=self.hit(101)+[(12001,'ratio',.76),(12001,'modulation',.82),(12001,'feedback',.54),(12001,'mod_env',.7),(12001,'drive',.5)]+self.hit(12001)+self.hit(27007)
  ref=self.render('dynamic-128',scalar,events=ev,seconds=1.)
  for b in (1,32,64,127,256,512):self.check('block:'+str(b),np.array_equal(ref,self.render('dynamic-'+str(b),scalar,events=ev,seconds=1.,block=b)))
  self.check('vector-parity',np.max(np.abs(ref-self.render('dynamic-vector',vector,events=ev,seconds=1.)))<3e-4)
  pulse=self.render('gate-pulse',scalar,events=self.hit(101,1));held=self.render('gate-held',scalar,events=[(101,'gate',1)]);self.check('noteoff-independent',np.array_equal(pulse,held))
  half=self.render('velocity-half',scalar,{'velocity':.5},self.hit(101));self.check('velocity-linear',np.max(np.abs(half-pulse*.5))<2e-6)
  tailchange=self.render('latched-tail',scalar,events=self.hit(101)+[(3001,'ratio',1),(3001,'decay',0),(3001,'feedback',1),(3001,'modulation',1),(3001,'mod_env',1),(3001,'drive',1),(3001,'punch',1),(3001,'pitch_hz',900)])
  self.check('all-controls-latched',np.array_equal(pulse,tailchange))
  p=self.patches['Acid'];pre=self.render('lock-pre',scalar,p,self.hit(1001));same=self.render('lock-same',scalar,events=[(1001,k,v) for k,v in p.items()]+self.hit(1001));self.check('same-sample-locks',np.array_equal(pre,same))
  ev=[];keys=['ratio','punch','decay','feedback','modulation','mod_env','drive'];i=0
  for bits in itertools.product((0.,1.),repeat=7):
   n=101+i*1024;i+=1;ev += [(n,k,v) for k,v in zip(keys,bits)]+[(n,'pitch_hz',55 if i%2 else 1760)]+self.hit(n)
  self.render('endpoint-stress',scalar,events=ev,seconds=(101+i*1024+48000)/48000,block=127)
  ev=[];timeline=[]
  for i,(name,p) in enumerate(self.patches.items()):
   n=2400+i*120000;ev += [(n,k,v) for k,v in p.items()]+[(n,'velocity',1)]+self.hit(n);ev += [(n+48000,'velocity',.55)]+self.hit(n+48000);timeline.append({'start_s':n/48000,'patch':name})
  a=self.render('anchors',scalar,events=ev,seconds=20.5);wav(self.out/'tone-anchors.wav',a)
  notes=[55,73.416,82.407,110,146.832,164.814,220,293.665];ev=[]
  for i in range(48):
   n=2400+i*6000;p=list(self.patches.values())[(i//8)%len(self.patches)]
   if i%2==0 or i in (7,15,23,31,39,47):ev += [(n,k,v) for k,v in p.items()]+[(n,'pitch_hz',notes[i%len(notes)]),(n,'velocity',1 if i%8==0 else .55)]+self.hit(n)
  pat=self.render('pattern',scalar,events=ev,seconds=6.5);wav(self.out/'tone-pattern.wav',pat)
  ev=[];order=self.man['musical_control_order']
  for j,key in enumerate(order):
   for q in range(8):
    n=2400+j*144000+q*18000;p=self.defaults.copy();p['pitch_hz']=110
    if key=='pitch_hz':p[key]=55*16**(q/7)
    else:p[key]=q/7
    ev += [(n,k,v) for k,v in p.items() if k!='gate']+self.hit(n)
  c=self.render('controls',scalar,events=ev,seconds=25);wav(self.out/'tone-controls.wav',c)
  if self.refs and (self.refs/'manifest.json').exists():
   manifest=json.loads((self.refs/'manifest.json').read_text());rd={}
   for row in manifest['records']:
    rate,x=wavfile.read(self.refs/(row['id']+'.wav'));x=np.asarray(x,dtype=float).reshape(-1);rd[row['id']]=desc(x,rate)
   pool=[]
   for name,p in self.patches.items():
    x=self.render('refpool-'+name,scalar,p,self.hit(101),seconds=2.5);pool.append((name,desc(x)))
   nearest={k:min((distance(v,d),n) for n,d in pool) for k,v in rd.items()};self.report['reference_coverage']={'nearest':nearest,'mean':float(np.mean([v[0] for v in nearest.values()])), 'warning':'broad descriptors; unknown settings; not clone score or fitted presets'}
  perf={}
  for label,exe in [('scalar',scalar),('vector',vector)]:
   vals=[]
   for r in range(5):
    self.render(f'perf-{label}-{r}',exe,self.patches['Growl'],self.hit(101),seconds=3,block=128)
    vals.append(float(self.report['renders'][-1]['diag']['instrumented_compute_ns']))
   perf[label]=vals
  self.report['performance']={k:{'median_compute_ns':statistics.median(v),'runs':v} for k,v in perf.items()};self.report['auditions']={'tone-anchors.wav':timeline,'tone-pattern.wav':'persistent 48-step pitched/locked phrase','tone-controls.wav':order};self.report['passed']=True
 def save(self,error=None):
  self.report['failure']=error
  if error:self.report['passed']=False
  (self.out/'report.json').write_text(json.dumps(self.report,indent=2)+'\n');print(json.dumps({'passed':self.report['passed'],'checks':len(self.report['checks']),'renders':len(self.report['renders']),'failure':error}))
def main():
 a=argparse.ArgumentParser();a.add_argument('--out',type=Path,required=True);a.add_argument('--references',type=Path);x=a.parse_args();s=Study(x.out.resolve(),x.references.resolve() if x.references else None);e=None
 try:s.run()
 except Exception as z:e=str(z)
 finally:s.save(e)
 if e:raise SystemExit(e)
if __name__=='__main__':main()
