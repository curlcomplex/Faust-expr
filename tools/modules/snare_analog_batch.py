"""Full offline qualification for snare-analog; no hardware-clone claim."""
from __future__ import annotations
import argparse,json,math,os,subprocess,itertools,statistics,wave,hashlib
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[2];MOD=ROOT/'modules/snare-analog'
def cmd(a,t=300):
 p=subprocess.run(list(map(str,a)),cwd=ROOT,capture_output=True,text=True,timeout=t)
 if p.returncode: raise RuntimeError(str(a)+'\n'+p.stdout+'\n'+p.stderr)
 return p.stdout
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def wav(p,x,r=48000):
 if not np.isfinite(x).all() or np.max(np.abs(x))>=1:raise ValueError('audio')
 with wave.open(str(p),'wb') as f:f.setnchannels(1);f.setsampwidth(2);f.setframerate(r);f.writeframes(np.rint(x*32767).astype('<i2').tobytes())
def desc(x,r=48000):
 x=np.asarray(x,float);e=x*x;c=np.cumsum(e)/max(e.sum(),1e-30);n=1<<max(14,(len(x)-1).bit_length());s=np.abs(np.fft.rfft(x*np.hanning(len(x)),n=n))**2+1e-20;fr=np.fft.rfftfreq(n,1/r);q=s/s.sum()
 return np.array([np.searchsorted(c,.5)/r,np.searchsorted(c,.9)/r,math.log10(max((fr*q).sum(),1)),float(q[(fr<500)].sum()),float(q[(fr>=2000)&(fr<8000)].sum()),float(q[fr>=8000].sum())])
class S:
 def __init__(self,o):self.o=o;o.mkdir(parents=True,exist_ok=True);self.m=json.loads((MOD/'manifest.json').read_text());self.d={k:v['default'] for k,v in self.m['controls'].items()};self.p=json.loads((MOD/'patches.json').read_text())['anchors'];self.r={'checks':[],'renders':[],'passed':False,'hardware_clone_claim':False,'human_approved':False,'device_qualified':False}
 def ck(self,n,v,**d):self.r['checks'].append({'name':n,'passed':bool(v),**d});assert v,(n,d)
 def build(self,label,vec=False):
  d=self.o/label;d.mkdir(exist_ok=True);fl=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vec else []);cmd([os.getenv('FAUST','faust'),'-I',MOD,*fl,MOD/'snare.dsp','-o',d/'generated.hpp']);cmd([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render']);self.r.setdefault('builds',{})[label]={'generated_sha256':sha(d/'generated.hpp'),'flags':fl};return d/'render'
 def render(self,n,e,p=None,ev=None,r=48000,b=128,sec=1.2):
  vals=self.d|(p or {});rows={(0,k):v for k,v in vals.items()};
  for z,k,v in ev or []:rows[z,k]=v
  sc=self.o/(n+'.tsv');raw=self.o/(n+'.f32');sc.write_text(''.join(f'{z}\t{k}\t{v:.9g}\n' for (z,k),v in sorted(rows.items())));fr=round(r*sec);dg=json.loads(cmd([e,sc,raw,r,b,fr,0]));x=np.fromfile(raw,'<f4');self.ck(n+':finite',len(x)==fr and np.isfinite(x).all() and np.max(np.abs(x))<.95,peak=float(np.max(np.abs(x))));self.r['renders'].append({'label':n,'raw_sha256':sha(raw),'score_sha256':sha(sc),'rate':r,'block':b,'peak':float(np.max(np.abs(x))),'diag':dg});return x
 def hit(self,n,l=64):return [(n,'gate',1),(n+l,'gate',0)]
 def go(self):
  a=self.build('scalar');v=self.build('vector',True);self.ck('silence',not np.any(self.render('silence',a,sec=.1)))
  for r in (44100,48000,96000):
   for hz in (90,180,420):
    x=self.render(f'pitch-{r}-{hz}',a,{'pitch_hz':hz,'balance':0,'crack':0,'drive':0,'tone':0,'decay':.9},self.hit(round(.05*r)),r=r,sec=1.1);y=x[round(.25*r):round(.85*r)];N=1<<19;sp=np.abs(np.fft.rfft((y-y.mean())*np.hanning(len(y)),N));fq=np.fft.rfftfreq(N,1/r);m=(fq>50)&(fq<800);me=float(fq[m][np.argmax(sp[m])]);self.ck(f'pitch:{r}:{hz}',abs(me-hz)<1.2,measured=me)
  ev=self.hit(101)+[(8011,'balance',.9),(8011,'crack',.8),(8011,'noise_color',.8),(8011,'tone',.8),(8011,'drive',.7)]+self.hit(8011);base=self.render('dynamic',a,ev=ev)
  for b in (1,32,64,127,512):self.ck('block:'+str(b),np.array_equal(base,self.render('dynamic-'+str(b),a,ev=ev,b=b)))
  z=self.render('vector',v,ev=ev);self.ck('vector-parity',np.max(np.abs(z-base))<3e-4,max_abs=float(np.max(np.abs(z-base))))
  q=self.render('pulse',a,ev=self.hit(101,1));h=self.render('held',a,ev=[(101,'gate',1)]);self.ck('noteoff-independent',np.array_equal(q,h));half=self.render('half',a,{'velocity':.5},self.hit(101));self.ck('velocity',np.max(np.abs(half-q*.5))<2e-6)
  ch=self.render('latched',a,ev=self.hit(101,1)+[(3001,'balance',0),(3001,'crack',1),(3001,'decay',1),(3001,'noise_color',0),(3001,'tone',1),(3001,'drive',1),(3001,'pitch_hz',500)]);self.ck('tail-latched',np.array_equal(q,ch))
  keys=['balance','crack','decay','noise_color','tone','drive'];ev=[];i=0
  for bits in itertools.product((0.,1.),repeat=6):
   n=101+i*1536;i+=1;ev += [(n,k,x) for k,x in zip(keys,bits)]+[(n,'pitch_hz',90 if i%2 else 420)]+self.hit(n,64)
  x=self.render('corners',a,ev=ev,sec=(101+i*1536+24000)/48000,b=127);self.ck('dc',abs(float(np.mean(x)))<.02,settings=i)
  # Distinctness: same fixed authored pool, compare descriptor spread against existing snare-pm generated from its canonical source.
  rng=np.random.default_rng(4701);pool=[]
  for i in range(48):
   p={'pitch_hz':float(rng.uniform(80,420)),'balance':float(rng.random()),'crack':float(rng.random()),'decay':float(rng.random()),'noise_color':float(rng.random()),'tone':float(rng.random()),'drive':float(.65*rng.random())};pool.append(desc(self.render('pool-'+str(i),a,p,self.hit(101),sec=1.2)))
  P=np.vstack(pool);self.r['descriptor_spread']={'mean_std':float(P.std(0).mean()),'dimensions_std':P.std(0).tolist()}
  ev=[];tl=[]
  for i,(name,p) in enumerate(self.p.items()):
   n=round((i*2+.05)*48000);ev += [(n,k,val) for k,val in p.items()]+[(n,'velocity',1)]+self.hit(n);ev += [(n+36000,'velocity',.55)]+self.hit(n+36000);tl.append({'patch':name,'start_s':n/48000})
  x=self.render('anchors',a,ev=ev,sec=16.2);wav(self.o/'snare-analog-