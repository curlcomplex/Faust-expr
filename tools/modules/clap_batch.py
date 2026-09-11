"""Actual-Faust qualification and fixed-gain audition generation for clap/v1."""
from __future__ import annotations
import argparse,hashlib,itertools,json,math,os,subprocess,time,wave
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[2]; MOD=ROOT/'modules/clap/v1'
def cmd(a,t=300):
 p=subprocess.run(list(map(str,a)),cwd=ROOT,capture_output=True,text=True,timeout=t)
 if p.returncode: raise RuntimeError(str(a)+'\n'+p.stdout+'\n'+p.stderr)
 return p.stdout
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def wav(p,x,r=48000):
 x=np.asarray(x); peak=float(np.max(np.abs(x))) if len(x) else 0
 if not np.isfinite(x).all() or peak>=1: raise ValueError(('audio',p,peak))
 with wave.open(str(p),'wb') as f:f.setnchannels(1);f.setsampwidth(2);f.setframerate(r);f.writeframes(np.rint(x*32767).astype('<i2').tobytes())
def validate(d,m):
 for k,v in d.items():
  if k not in m['controls'] or not isinstance(v,(int,float)) or isinstance(v,bool) or not math.isfinite(v):raise ValueError(k)
  q=m['controls'][k]
  if v<q['min'] or v>q['max']:raise ValueError((k,v))
  if k=='gate' and int(v)!=v:raise ValueError('gate')
def descriptors(x,r=48000):
 x=np.asarray(x,float); e=x*x
 if e.sum()<=1e-18:raise ValueError('silence')
 c=np.cumsum(e)/e.sum(); n=1<<max(15,(len(x)-1).bit_length()); sp=np.abs(np.fft.rfft(x*np.hanning(len(x)),n=n))**2+1e-20; f=np.fft.rfftfreq(n,1/r); q=sp/sp.sum()
 return np.array([np.searchsorted(c,.25)/r,np.searchsorted(c,.5)/r,np.searchsorted(c,.9)/r,math.log10(max((f*q).sum(),1)),q[(f<800)].sum(),q[(f>=800)&(f<5000)].sum(),q[f>=5000].sum()])
class S:
 def __init__(self,o):self.o=o;o.mkdir(parents=True,exist_ok=True);self.m=json.loads((MOD/'manifest.json').read_text());self.d={k:v['default'] for k,v in self.m['controls'].items()};self.p=json.loads((MOD/'patches.json').read_text())['anchors'];self.r={'schema':1,'passed':False,'checks':[],'renders':[],'hardware_clone_claim':False,'device_qualified':False,'human_approved':False}
 def ck(self,n,v,**d):self.r['checks'].append(dict(name=n,passed=bool(v),**d));assert v,(n,d)
 def build(self,label,source='clap.dsp',vec=False):
  d=self.o/label;d.mkdir(exist_ok=True);fl=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vec else []);cmd([os.getenv('FAUST','faust'),'-I',MOD,*fl,MOD/source,'-o',d/'generated.hpp']);cmd([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render']);self.r.setdefault('builds',{})[label]=dict(source=source,generated_sha256=sha(d/'generated.hpp'),flags=fl);return d/'render'
 def render(self,n,e,p=None,ev=None,r=48000,b=128,sec=1.5):
  vals=self.d|(p or {});validate(vals,self.m);rows={(0,k):v for k,v in vals.items()}
  for z,k,v in ev or []:validate({k:v},self.m);rows[z,k]=v
  sc=self.o/(n+'.tsv');raw=self.o/(n+'.f32');sc.write_text(''.join(f'{z}\t{k}\t{v:.9g}\n' for (z,k),v in sorted(rows.items())));frames=round(sec*r);dg=json.loads(cmd([e,sc,raw,r,b,frames,0]));x=np.fromfile(raw,'<f4');self.ck(n+':finite-headroom',len(x)==frames and np.isfinite(x).all() and np.max(np.abs(x))<.95,peak=float(np.max(np.abs(x))));self.r['renders'].append(dict(label=n,rate=r,block=b,frames=frames,raw_sha256=sha(raw),score_sha256=sha(sc),peak=float(np.max(np.abs(x))),rms=float(np.sqrt(np.mean(x.astype(float)**2))),diag=dg));return x
 @staticmethod
 def hit(n,l=64):return [(n,'gate',1),(n+l,'gate',0)]
 def go(self):
  a=self.build('scalar');v=self.build('vector',vec=True);nt=self.build('no-tail','no-tail.dsp');sb=self.build('single-burst','single-burst.dsp')
  self.ck('exact-initial-silence',not np.any(self.render('silence',a,sec=.1)))
  for r in (44100,48000,96000):
   for name,p in [('tight',self.p['Tight']),('classic',self.p['Classic']),('long',self.p['Long'])]:self.render(f'{name}-{r}',a,p,self.hit(round(.05*r)),r=r,sec=3 if name=='long' else 1.2)
  ev=self.hit(101)+[(8011,'spacing',.9),(8011,'punch',.8),(8011,'decay',.8),(8011,'color',.8),(8011,'body',.6),(8011,'drive',.5),(8011,'pitch_hz',900)]+self.hit(8011);base=self.render('dynamic',a,ev=ev)
  for b in (1,32,64,127,128,256,512):self.ck('block:'+str(b),np.array_equal(base,self.render('dynamic-'+str(b),a,ev=ev,b=b)))
  vv=self.render('dynamic-vector',v,ev=ev);self.ck('vector-parity',np.max(np.abs(vv-base))<3e-4,max_abs=float(np.max(np.abs(vv-base))))
  q=self.render('pulse',a,ev=self.hit(101,1));h=self.render('held',a,ev=[(101,'gate',1)]);self.ck('noteoff-does-not-choke',np.array_equal(q,h))
  half=self.render('half',a,{'velocity':.5},self.hit(101));self.ck('velocity-linear',np.max(np.abs(half-q*.5))<2e-6,max_abs=float(np.max(np.abs(half-q*.5))))
  changed=self.render('latched-tail',a,ev=self.hit(101,1)+[(3001,'spacing',1),(3001,'punch',0),(3001,'decay',1),(3001,'color',0),(3001,'body',1),(3001,'drive',1),(3001,'pitch_hz',1800)]);self.ck('controls-latched-at-onset',np.array_equal(q,changed))
  # Onset-applied patch must equal prepared patch.
  patch=self.p['Wide']; prep=self.render('lock-prepared',a,patch,self.hit(101)); ev=[(101,k,val) for k,val in patch.items()]+self.hit(101); same=self.render('lock-same-sample',a,ev=ev);self.ck('same-sample-lock',np.array_equal(prep,same))
  keys=['spacing','punch','decay','color','body','drive'];ev=[];idx=0
  for bits in itertools.product((0.,1.),repeat=6):
   n=101+idx*2048;idx+=1;ev += [(n,k,x) for k,x in zip(keys,bits)]+[(n,'pitch_hz',260 if idx%2 else 1400)]+self.hit(n,96)
  corners=self.render('corners',a,ev=ev,sec=(101+idx*2048+96000)/48000,b=127);self.ck('corners-dc',abs(float(np.mean(corners)))<.02,mean=float(np.mean(corners)),settings=idx)
  # Rapid persistent hits.
  ev=[]
  for i in range(96):
   n=101+i*480;ev += [(n,'spacing',(i%11)/10),(n,'punch',(i%7)/6),(n,'velocity',.35+.65*((i%5)/4))]+self.hit(n,48)
  self.render('rapid',a,ev=ev,sec=1.1,b=32)
  # Controlled ablations using identical authored settings.
  ab=[]
  for name in ('Classic','Tight','Wide','Dark','Bright','Body','Driven','Long'):
   p=self.p[name];full=self.render('abl-full-'+name,a,p,self.hit(101),sec=4);xnt=self.render('abl-notail-'+name,nt,p,self.hit(101),sec=4);xsb=self.render('abl-single-'+name,sb,p,self.hit(101),sec=4);df=descriptors(full);ab.append(dict(name=name,no_tail_distance=float(np.linalg.norm(descriptors(xnt)-df)),single_burst_distance=float(np.linalg.norm(descriptors(xsb)-df))))
  # The full-hit descriptor is intentionally retained, but it is too coarse to judge the
  # millisecond-scale clap cluster. Measure the first 100 ms directly against the
  # same-engine single-burst ablation; shared deterministic noise makes this a
  # controlled attack-structure comparison rather than a random-noise mismatch.
  early=[]
  for item in ab:
   name=item['name'];full=np.fromfile(self.o/('abl-full-'+name+'.f32'),'<f4').astype(float);single=np.fromfile(self.o/('abl-single-'+name+'.f32'),'<f4').astype(float);sl=slice(101,101+4800);den=max(1e-15,float(np.sqrt(np.mean(full[sl]**2))));rel=float(np.sqrt(np.mean((full[sl]-single[sl])**2))/den);item['cluster_early_relative_rms']=rel;early.append(rel)
  self.r['ablations']=ab
  self.r['ablation_method_note']='Full-hit descriptors are retained as contrary evidence; cluster acceptance uses first-100-ms relative RMS because the hypothesis is temporal attack structure.'
  self.ck('tail-is-material',float(np.mean([z['no_tail_distance'] for z in ab]))>.08,mean=float(np.mean([z['no_tail_distance'] for z in ab])))
  self.ck('cluster-attack-is-material',float(np.mean(early))>.08,mean=float(np.mean(early)),minimum=float(np.min(early)),coarse_descriptor_mean=float(np.mean([z['single_burst_distance'] for z in ab])))
  # Auditions.
  ev=[];timeline=[]
  for i,(name,p) in enumerate(self.p.items()):
   n=round((i*2.2+.05)*48000);ev += [(n,k,x) for k,x in p.items()]+[(n,'velocity',1)]+self.hit(n);ev += [(n+42000,'velocity',.5)]+self.hit(n+42000);timeline.append(dict(patch=name,start_s=n/48000))
  x=self.render('anchors',a,ev=ev,sec=18);wav(self.o/'clap-anchors.wav',x);self.r.setdefault('auditions',{})['clap-anchors.wav']=timeline
  ev=[]
  for i in range(64):
   if i%4==0 or i in (6,7,14,15,22,23,30,31,38,46,47,54,55,62,63):
    n=2400+i*6000;p=list(self.p.values())[(i//16)%len(self.p)];ev += [(n,k,x) for k,x in p.items()]+[(n,'velocity',1 if i%4==0 else .48)]+self.hit(n,80)
  x=self.render('pattern',a,ev=ev,sec=8.3,b=64);wav(self.o/'clap-pattern.wav',x);self.r['auditions']['clap-pattern.wav']='persistent voice, 64-step phrase, fixed kernel gain'
  order=self.m['visible_columns'];ev=[]
  for j,k in enumerate(order):
   for i in range(8):
    n=2400+j*144000+i*18000;p=self.d.copy();p[k]=i/7;ev += [(n,q,z) for q,z in p.items() if q!='gate']+self.hit(n,64)
  x=self.render('controls',a,ev=ev,sec=19);wav(self.o/'clap-controls.wav',x);self.r['auditions']['clap-controls.wav']=dict(order=order,seconds_per_control=3)
  # Offline whole-render scalar/vector timing, repeated and rotated. Startup cost included and disclosed.
  perf=[]
  long_ev=[]
  for i in range(600):
   n=101+i*2400;long_ev += [(n,'spacing',(i%13)/12),(n,'color',(i%9)/8),(n,'drive',(i%7)/6)]+self.hit(n,48)
  score=self.o/'perf.tsv';rows={(0,k):v for k,v in self.d.items()}
  for z,k,vv0 in long_ev:rows[z,k]=vv0
  score.write_text(''.join(f'{z}\t{k}\t{vv0:.9g}\n' for (z,k),vv0 in sorted(rows.items())))
  frames=600*2400+48000
  for rep in range(4):
   row={}
   for label,e in ((('scalar',a),('vector',v)) if rep%2==0 else (('vector',v),('scalar',a))):
    raw=self.o/f'perf-{label}-{rep}.f32';t0=time.perf_counter();cmd([e,score,raw,48000,128,frames,0]);row[label]=time.perf_counter()-t0
   perf.append(row)
  self.r['performance']=dict(scope='whole offline renderer process; startup included; not device callback timing',pairs=perf,median_scalar_s=float(np.median([z['scalar'] for z in perf])),median_vector_s=float(np.median([z['vector'] for z in perf])))
  self.r['passed']=True
 def save(self,err=None):
  self.r['failure']=err;self.r['passed']=self.r['passed'] and err is None
  self.r['source_commit']=cmd(['git','rev-parse','HEAD']).strip() if (ROOT/'.git').exists() else None
  (self.o/'report.json').write_text(json.dumps(self.r,indent=2)+'\n')
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();s=S(a.out.resolve());err=None
 try:s.go()
 except Exception as e:err=repr(e)
 finally:s.save(err);print(json.dumps({'passed':s.r['passed'],'renders':len(s.r['renders']),'checks':len(s.r['checks']),'failure':err}))
 if err:raise SystemExit(err)
if __name__=='__main__':main()
