"""Actual-Faust clap qualification, auditions, ablations and performance evidence."""
from __future__ import annotations
import argparse,hashlib,itertools,json,math,os,platform,subprocess,wave
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[2]; MOD=ROOT/'modules/clap/v1'
def cmd(a,t=300):
 p=subprocess.run(list(map(str,a)),cwd=ROOT,capture_output=True,text=True,timeout=t)
 if p.returncode: raise RuntimeError(str(a)+'\n'+p.stdout+'\n'+p.stderr)
 return p.stdout
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def writewav(p,x,r=48000):
 x=np.asarray(x); assert np.isfinite(x).all() and np.max(np.abs(x))<1
 with wave.open(str(p),'wb') as f:f.setnchannels(1);f.setsampwidth(2);f.setframerate(r);f.writeframes(np.rint(x*32767).astype('<i2').tobytes())
def descriptor(x,r=48000):
 x=np.asarray(x,float);e=x*x;c=np.cumsum(e)/max(e.sum(),1e-30);n=1<<max(16,(len(x)-1).bit_length());s=np.abs(np.fft.rfft(x*np.hanning(len(x)),n))**2+1e-30;fr=np.fft.rfftfreq(n,1/r);q=s/s.sum();return [float(np.searchsorted(c,.5)/r),float(np.searchsorted(c,.9)/r),float((fr*q).sum()),float(q[(fr<1000)].sum()),float(q[(fr>5000)].sum())]
class Study:
 def __init__(self,o):self.o=o;o.mkdir(parents=True,exist_ok=True);self.m=json.loads((MOD/'manifest.json').read_text());self.d={k:v['default'] for k,v in self.m['controls'].items()};self.p=json.loads((MOD/'patches.json').read_text())['anchors'];self.r={'schema':1,'passed':False,'checks':[],'renders':[],'builds':{},'hardware_clone_claim':False,'human_approved':False,'device_qualified':False,'platform':platform.platform()}
 def ck(self,n,v,**d):self.r['checks'].append({'name':n,'passed':bool(v),**d});assert v,(n,d)
 def build(self,label,src='clap.dsp',vec=False):
  d=self.o/label;d.mkdir(exist_ok=True);fl=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vec else []);cmd([os.getenv('FAUST','faust'),'-I',MOD,*fl,MOD/src,'-o',d/'generated.hpp']);c=['-std=c++17','-O2','-ffp-contract=off','-I'+str(d)];cmd([os.getenv('CXX','c++'),*c,ROOT/'tools/modules/render.cpp','-o',d/'render']);cmd([os.getenv('CXX','c++'),*c,ROOT/'tools/modules/clap_benchmark.cpp','-o',d/'benchmark']);self.r['builds'][label]={'source':src,'generated_sha256':sha(d/'generated.hpp'),'faust_flags':fl};return d/'render'
 def render(self,n,e,p=None,ev=None,r=48000,b=128,sec=1.3):
  vals=self.d|(p or {});rows={(0,k):v for k,v in vals.items()};
  for z,k,v in ev or []:rows[z,k]=v
  sc=self.o/(n+'.tsv');raw=self.o/(n+'.f32');sc.write_text(''.join(f'{z}\t{k}\t{v:.9g}\n' for(z,k),v in sorted(rows.items())));frames=round(sec*r);dg=json.loads(cmd([e,sc,raw,r,b,frames,0]));x=np.fromfile(raw,'<f4');self.ck(n+':finite',len(x)==frames and np.isfinite(x).all() and np.max(np.abs(x))<.95,peak=float(np.max(np.abs(x))));self.r['renders'].append({'label':n,'build':e.parent.name,'rate':r,'block':b,'raw_sha256':sha(raw),'score_sha256':sha(sc),'peak':float(np.max(np.abs(x))),'diag':dg});return x
 @staticmethod
 def hit(n,l=64):return [(n,'gate',1),(n+l,'gate',0)]
 def go(self):
  scalar=self.build('scalar'); vector=self.build('vector',vec=True); single=self.build('single','single-burst.dsp'); nt=self.build('no-tail','no-tail.dsp'); nb=self.build('no-body','no-body.dsp')
  self.ck('initial-silence',not np.any(self.render('silence',scalar,sec=.1)))
  for rate in (44100,48000,96000):
   for hz in (90,210,700):
    x=self.render(f'body-pitch-{rate}-{hz}',scalar,{'pitch_hz':hz,'balance':1,'body':0,'body_env':1,'drive':0,'punch':0},self.hit(round(.04*rate)),r=rate,sec=1.2);y=x[round(.15*rate):round(.8*rate)];N=1<<19;sp=np.abs(np.fft.rfft(y*np.hanning(len(y)),N));fq=np.fft.rfftfreq(N,1/rate);m=(fq>50)&(fq<1200);me=float(fq[m][np.argmax(sp[m])]);self.ck(f'pitch:{rate}:{hz}',abs(me-hz)<1.5,measured=me)
  ev=self.hit(101)+[(9001,'spacing',.9),(9001,'punch',.8),(9001,'decay',.8),(9001,'balance',.7),(9001,'body',.85),(9001,'body_env',.75),(9001,'drive',.6),(9001,'pitch_hz',430)]+self.hit(9001);base=self.render('dynamic',scalar,ev=ev)
  for b in (1,32,64,127,128,256,512):self.ck('block:'+str(b),np.array_equal(base,self.render('dynamic-'+str(b),scalar,ev=ev,b=b)))
  vx=self.render('vector-parity',vector,ev=ev);self.ck('vector-equivalence',np.max(np.abs(vx-base))<4e-4,max_abs=float(np.max(np.abs(vx-base))))
  pulse=self.render('pulse',scalar,ev=self.hit(101,1));held=self.render('held',scalar,ev=[(101,'gate',1)]);self.ck('noteoff-does-not-choke',np.array_equal(pulse,held))
  half=self.render('half-velocity',scalar,{'velocity':.5},self.hit(101));self.ck('velocity-linear',np.max(np.abs(half-pulse*.5))<2e-6)
  changed=self.render('latched-tail',scalar,ev=self.hit(101,1)+[(3001,'spacing',1),(3001,'punch',1),(3001,'decay',1),(3001,'balance',1),(3001,'body',1),(3001,'body_env',1),(3001,'drive',1),(3001,'pitch_hz',900)]);self.ck('tail-controls-latched',np.array_equal(pulse,changed))
  keys=['spacing','punch','decay','balance','body','body_env','drive'];ev=[];i=0
  for bits in itertools.product((0.,1.),repeat=7):
   n=101+i*1400;i+=1;ev += [(n,k,x) for k,x in zip(keys,bits)]+[(n,'pitch_hz',90 if i%2 else 700)]+self.hit(n,64)
  corners=self.render('corners',scalar,ev=ev,sec=(101+i*1400+24000)/48000,b=127);self.ck('corner-dc',abs(float(np.mean(corners)))<.02,settings=i)
  # Persistent retrigger stress and long silence.
  ev=[]
  for i in range(96):
   n=101+i*400;ev += [(n,'spacing',(i%13)/12),(n,'balance',(i%9)/8),(n,'body',(i%7)/6)]+self.hit(n,50)
  self.render('rapid-retriggers',scalar,ev=ev,sec=1.2,b=32)
  self.render('long-tail-silence',scalar,{'decay':1,'body_env':1,'balance':.45},self.hit(101),sec=10.)
  # Ablations at the exact eight authored anchors.
  ab={}
  for name,p in self.p.items():
   full=self.render('abl-full-'+name,scalar,p,self.hit(101),sec=2.2);a=self.render('abl-single-'+name,single,p,self.hit(101),sec=2.2);b=self.render('abl-notail-'+name,nt,p,self.hit(101),sec=2.2);c=self.render('abl-nobody-'+name,nb,p,self.hit(101),sec=2.2);ab[name]={'full':descriptor(full),'single':descriptor(a),'no_tail':descriptor(b),'no_body':descriptor(c),'single_rel_rms_db':float(20*np.log10(max(1e-12,np.sqrt(np.mean((full-a)**2)))/max(1e-12,np.sqrt(np.mean(full**2))))),'no_tail_rel_rms_db':float(20*np.log10(max(1e-12,np.sqrt(np.mean((full-b)**2)))/max(1e-12,np.sqrt(np.mean(full**2))))),'no_body_rel_rms_db':float(20*np.log10(max(1e-12,np.sqrt(np.mean((full-c)**2)))/max(1e-12,np.sqrt(np.mean(full**2)))))}
  self.r['ablations']=ab
  # Auditions: anchors, pattern and controls.
  ev=[]
  for i,(name,p) in enumerate(self.p.items()):
   n=round((i*2.2+.05)*48000);ev += [(n,k,v) for k,v in p.items()]+[(n,'velocity',1)]+self.hit(n);ev += [(n+36000,'velocity',.55)]+self.hit(n+36000)
  x=self.render('anchors',scalar,ev=ev,sec=18.);writewav(self.o/'clap-anchors.wav',x)
  ev=[];names=list(self.p);step=6000
  for i in range(64):
   if i%4==0 or i in (3,7,11,15,22,23,31,39,47,55,62,63):
    n=2400+i*step;p=self.p[names[(i//12)%len(names)]];ev += [(n,k,v) for k,v in p.items()]+[(n,'velocity',1 if i%4==0 else .5)]+self.hit(n,80)
  x=self.render('pattern',scalar,ev=ev,sec=8.5);writewav(self.o/'clap-pattern.wav',x)
  order=self.m['musical_control_order'][1:];ev=[]
  for j,k in enumerate(order):
   for i in range(8):
    n=2400+j*144000+i*18000;p=self.d.copy();p.update(self.p['Classic']);p[k]=i/7;p.pop('gate');p.pop('velocity');ev += [(n,key,val) for key,val in p.items()]+self.hit(n,80)
  x=self.render('controls',scalar,ev=ev,sec=22.);writewav(self.o/'clap-controls.wav',x)
  # Same-sound scalar/vector benchmark; no optimization winner assumed.
  perf=[]
  for block in (32,64,128,512):
   s=json.loads(cmd([self.o/'scalar'/'benchmark',block]));v=json.loads(cmd([self.o/'vector'/'benchmark',block]));perf.append({'block':block,'scalar':s,'vector':v,'vector_speedup_p50':s['p50_us']/v['p50_us']})
  self.r['performance']=perf;self.r['performance_scope']='Hosted offline four-voice DSP benchmark only; not device callback or thermal acceptance.';self.r['passed']=True
  self.r['source_commit']=cmd(['git','rev-parse','HEAD']).strip();self.r['compilers']={'faust':cmd(['faust','-v']).strip(),'cxx':cmd(['c++','--version']).splitlines()[0]};(self.o/'report.json').write_text(json.dumps(self.r,indent=2)+'\n')
def main():
 a=argparse.ArgumentParser();a.add_argument('--out',type=Path,required=True);x=a.parse_args();s=Study(x.out.resolve());s.go();print(json.dumps({'passed':s.r['passed'],'renders':len(s.r['renders']),'checks':len(s.r['checks'])}))
if __name__=='__main__':main()
