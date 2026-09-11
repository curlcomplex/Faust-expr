"""Actual-Faust analog-snare qualification. No hardware-clone claim.
Default builds source with Faust. --replay recompiles checked generated C++.
External audio acquisition is separate; qualification never silently substitutes
synthetic fixtures for hardware recordings.
"""
from __future__ import annotations
import argparse, hashlib, itertools, json, math, os, platform, re, shutil, statistics, subprocess, wave
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly, welch
ROOT=Path(__file__).resolve().parents[2]
MOD=ROOT/'modules/snare-analog/v2'
PARITY_ABS=3e-5
ENVELOPE_REL=2e-5

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def cmd(a,timeout=900):
 p=subprocess.run(list(map(str,a)),cwd=ROOT,capture_output=True,text=True,timeout=timeout)
 if p.returncode: raise RuntimeError(f'{a}\n{p.stdout}\n{p.stderr}')
 return p.stdout

def validate(values,manifest):
 for k,v in values.items():
  if k not in manifest['controls'] or isinstance(v,bool) or not isinstance(v,(float,int)) or not math.isfinite(v): raise ValueError('invalid '+k)
  c=manifest['controls'][k]
  if not c['min']<=v<=c['max'] or (k=='gate' and v not in (0,1)): raise ValueError('out of range '+k)

def envelopes(t,decay,balance,crack):
 t=np.asarray(t,float);b=.025*32**decay;n=.016*55**decay*(.7+.6*balance);atk=.00015+.0012*(1-crack)**2
 return np.stack([-np.expm1(-t/atk)*np.exp(-t/b)*(t<16*b),-np.expm1(-t/.00025)*np.exp(-t/n)*(t<16*n)],axis=-1)

def pitch_estimate(x,rate):
 y=np.asarray(x,float).reshape(-1);y=y-y.mean();n=1<<19
 z=np.abs(np.fft.rfft(y*np.hanning(len(y)),n));z[0]=0;k=int(z.argmax())
 q=np.log(np.maximum(z[k-1:k+2],1e-30));den=q[0]-2*q[1]+q[2]
 return (k+(.5*(q[0]-q[2])/den if den else 0))*rate/n

def descriptors(x,rate):
 x=np.asarray(x,float);x=x.mean(1) if x.ndim==2 else x
 if not len(x) or not np.isfinite(x).all(): raise ValueError('invalid audio')
 e=x*x
 if e.sum()<1e-20: raise ValueError('silent descriptor')
 onset=int(np.argmax(np.abs(x)>np.max(np.abs(x))*.005));x=x[onset:];e=x*x;c=np.cumsum(e)/e.sum()
 # Welch averages window POWER, so near-silent tail windows cannot dominate.
 f,p=welch(x,rate,nperseg=min(2048,len(x)),detrend=False,scaling='spectrum');p=np.maximum(p,0);q=p/p.sum()
 return np.array([np.searchsorted(c,.5)/rate,np.searchsorted(c,.9)/rate,np.log10(max(1,(f*q).sum())),q[f<500].sum(),q[(f>=2000)&(f<8000)].sum(),q[f>=8000].sum()])

def phrase_gain(x,target=.12,ceiling=.9):
 # Explicit native float: NumPy 2 scalar promotion must not break JSON evidence.
 x=np.asarray(x,float)
 if not x.size or not np.isfinite(x).all():raise ValueError('invalid audition data')
 return float(min(ceiling/max(float(abs(x).max()),1e-12),target/max(float(np.sqrt(np.mean(x*x))),1e-12)))

def write_wav(p,x,rate=48000):
 x=np.asarray(x)
 if not x.size or not np.isfinite(x).all() or np.max(abs(x))>=1: raise ValueError('refuse clipping/normalization')
 with wave.open(str(p),'wb') as f:
  f.setnchannels(1 if x.ndim==1 else x.shape[1]);f.setsampwidth(2);f.setframerate(rate);f.writeframes(np.rint(x*32767).astype('<i2').tobytes())

class Study:
 def __init__(self,out,replay=None,refs=None):
  self.out=out;out.mkdir(parents=True,exist_ok=True);self.replay=replay;self.refs=refs
  self.man=json.loads((MOD/'manifest.json').read_text());self.defaults={k:c['default'] for k,c in self.man['controls'].items()}
  self.patches=json.loads((MOD/'patches.json').read_text())['anchors'];self.controls={};self.executables={}
  self.report={'schema':2,'module':'snare-analog/0.2.0-experiment','checks':[],'renders':[],'builds':{},'auditions':{},'passed':False,'human_approved':False,'hardware_match':False,'device_qualified':False,'platform':platform.platform()}
 def check(self,name,ok,**details):
  self.report['checks'].append(dict(name=name,passed=bool(ok),**details))
  if not ok: raise AssertionError(name+': '+str(details))
 def build(self,label,source,vector=False):
  d=self.out/label;d.mkdir(exist_ok=True);flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vector else [])
  if self.replay:
   r=json.loads((self.replay/'report.json').read_text());old=self.replay/label/'generated.hpp'
   self.check(label+':generated-hash',sha(old)==r['builds'][label]['generated_sha256']);shutil.copyfile(old,d/'generated.hpp')
  else:
   cmd([os.getenv('FAUST','faust'),'-t','600','-e','-I',source.parent,source,'-o',d/'expanded.dsp'])
   cmd([os.getenv('FAUST','faust'),'-t','600','-I',source.parent,*flags,source,'-o',d/'generated.hpp'])
  opts=['-std=c++17','-O2','-ffp-contract=off','-fstack-usage','-I'+str(d)]
  cmd([os.getenv('CXX','c++'),*opts,ROOT/'tools/modules/render.cpp','-o',d/'render'])
  if label in ('direct','lookup','vector'): cmd([os.getenv('CXX','c++'),*opts,ROOT/'tools/modules/analog_snare_benchmark.cpp','-o',d/'benchmark'])
  text=cmd([d/'render','--controls']);(d/'controls.tsv').write_text(text);rows=text.splitlines();self.controls[label]={r.split('\t')[0]:dict(zip(['min','max','default'],map(float,r.split('\t')[1:4]))) for r in rows[1:]}
  self.check(label+':channels',rows[0]==('io\t0\t2' if label=='envelope' else 'io\t0\t1'))
  if label in ('direct','lookup','vector'):
   self.check(label+':controls',set(self.controls[label])==set(self.defaults))
   for k,c in self.man['controls'].items():self.check(label+':'+k,all(abs(self.controls[label][k][a]-c[a])<1e-6 for a in ('min','max','default')))
  self.report['builds'][label]={'source':str(source.relative_to(ROOT)),'source_sha256':sha(source),'generated_sha256':sha(d/'generated.hpp'),'binary_sha256':sha(d/'render'),'flags':flags,'cxx_flags':opts}
  self.executables[label]=d/'render';return d/'render'
 def render(self,name,label,params=None,events=None,rate=48000,block=128,seconds=1.2):
  ctl=self.controls[label];values={k:v['default'] for k,v in ctl.items()};values.update(params or {})
  for k,v in values.items():
   if k not in ctl or not np.isfinite(v) or not ctl[k]['min']<=v<=ctl[k]['max']:raise ValueError('invalid render control '+k)
  rows={(0,k):v for k,v in values.items()};seen=set()
  for n,k,v in events or []:
   if (n,k) in seen: raise ValueError('duplicate event')
   seen.add((n,k))
   if k not in ctl or not np.isfinite(v) or not ctl[k]['min']<=v<=ctl[k]['max']:raise ValueError('invalid event control '+k)
   rows[n,k]=v
  sc=self.out/(name+'.tsv');raw=self.out/(name+'.f32');sc.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
  frames=round(rate*seconds);diag=json.loads(cmd([self.executables[label],sc,raw,rate,block,frames,0]));x=np.fromfile(raw,dtype='<f4').reshape(frames,diag['channels'])
  if diag['channels']==1:x=x[:,0]
  self.check(name+':finite-bound',np.isfinite(x).all() and abs(x).max()<(1.01 if label=='envelope' else .98),peak=float(abs(x).max()))
  self.report['renders'].append(dict(label=name,build=label,rate=rate,block=block,frames=frames,channels=diag['channels'],score_sha256=sha(sc),raw_sha256=sha(raw),peak=float(abs(x).max()),rms=float(np.sqrt(np.mean(x.astype(float)**2))),mean=float(x.mean()),max_jump=float(abs(np.diff(x,axis=0)).max()),diagnostics=diag))
  return x
 @staticmethod
 def hit(n,length=1):return [(n,'gate',1),(n+length,'gate',0)]
 def execute(self):
  for label,file,vec in [('direct','reference.dsp',False),('lookup','snare.dsp',False),('vector','snare.dsp',True),('envelope','envelope.dsp',False),('no-tail','no-tail.dsp',False),('single-crack','single-crack.dsp',False)]:self.build(label,MOD/file,vec)
  self.build('pm',ROOT/'modules/snare-pm/hybrid.dsp')
  self.report['cxx']=cmd([os.getenv('CXX','c++'),'--version']);self.report['faust']='C++ replay; no Faust invocation' if self.replay else cmd([os.getenv('FAUST','faust'),'-v'])
  silence=self.render('silence','lookup',seconds=.2);self.check('silence:exact',not np.any(silence))
  for rate in (44100,48000,96000):
   for hz in (70,180,500):
    p=dict(pitch_hz=hz,balance=0,crack=0,drive=0,tone=0,decay=.9)
    x=self.render(f'pitch-{rate}-{hz}','lookup',p,self.hit(101),rate=rate,seconds=1.3);me=pitch_estimate(x[round(.2*rate):round(1.2*rate)],rate)
    self.check(f'pitch:{rate}:{hz}',abs(me-hz)<.3,measured=me)
   for de in (0.,1.):
    p=dict(decay=de,balance=1.,crack=.55);x=self.render(f'envelope-{rate}-{de}','envelope',p,self.hit(101),rate=rate,seconds=20 if de else .25)
    expected=np.zeros_like(x,dtype=float);expected[101:]=envelopes(np.arange(len(x)-101)/rate,de,1.,.55);mask=expected>1e-4
    rel=float(np.max(abs(x[mask]-expected[mask])/expected[mask]));self.check(f'envelope-oracle:{rate}:{de}',rel<ENVELOPE_REL,relative_error=rel)
  ev=self.hit(101)+[(8011,k,v) for k,v in self.patches['Hard'].items()]+self.hit(8011)+[(17003,k,v) for k,v in self.patches['Wire'].items()]+self.hit(17003)
  base=self.render('dynamic','lookup',events=ev)
  for block in (1,32,64,127,256,512):
   x=self.render('dynamic-'+str(block),'lookup',events=ev,block=block);self.check('block:'+str(block),np.array_equal(x,base))
  for label in ('direct','vector'):
   x=self.render('dynamic-'+label,label,events=ev);self.check(label+':parity',abs(x-base).max()<PARITY_ABS,max_abs=float(abs(x-base).max()))
  for name,p in self.patches.items():
   a=self.render('prepared-'+name,'lookup',p,self.hit(2301),seconds=.8)
   b=self.render('onset-'+name,'lookup',events=[(2301,k,v) for k,v in p.items()]+self.hit(2301),seconds=.8)
   self.check('onset-locks:'+name,np.array_equal(a,b))
   c=self.render('direct-'+name,'direct',p,self.hit(2301),seconds=.8);self.check('lookup-audio:'+name,abs(a-c).max()<PARITY_ABS,max_abs=float(abs(a-c).max()))
  a=self.render('pulse','lookup',events=self.hit(101));b=self.render('held','lookup',events=[(101,'gate',1)]);self.check('noteoff-independent',np.array_equal(a,b))
  for vel in (0.,.25,.5):
   x=self.render('velocity-'+str(vel),'lookup',dict(velocity=vel),self.hit(101));self.check('velocity-linear:'+str(vel),abs(x-a*vel).max()<2e-6)
  ev=self.hit(101)+[(3001,k,v) for k,v in dict(self.patches['Long'],velocity=.2).items()]
  b=self.render('tail-latch','lookup',events=ev);self.check('tail-not-rewritten',np.array_equal(a,b))
  keys=self.man['visible_columns'];ev=[]
  for i,(bits,hz) in enumerate(itertools.product(itertools.product((0.,1.),repeat=6),(70.,500.))):
   n=101+i*1536;ev +=[(n,k,v) for k,v in zip(keys,bits)]+[(n,'pitch_hz',hz)]+self.hit(n,48)
  x=self.render('128-endpoints','lookup',events=ev,seconds=4.7,block=127);self.check('endpoint-dc',abs(float(x.mean()))<.02)
  ev=[]
  for i in range(96):
   n=101+i*256;ev += [(n,'crack',(i%7)/6),(n,'balance',(i%13)/12),(n,'drive',(i%5)/4)]+self.hit(n)
  self.render('rapid-retriggers','lookup',events=ev,seconds=.8,block=32)
  self.render('long-gap-and-tail','lookup',dict(decay=1),self.hit(101)+self.hit(25*48000+3),seconds=27)
  for i,txt in enumerate(['0 made_up 0\n','0 decay nan\n','0 drive 2\n','0 gate .5\n','-1 gate 1\n','10 gate 0\n5 gate 1\n','0 gate 0\n0 gate 1\n','1000 gate 1\n']):
   p=self.out/f'bad-{i}.tsv';p.write_text(txt);r=subprocess.run([str(self.executables['lookup']),str(p),str(self.out/f'bad-{i}.f32'),'48000','128','1000','0'],capture_output=True,text=True)
   self.check('reject:'+str(i),r.returncode!=0)
  p=dict(balance=0,crack=.3,drive=.4)
  a=self.render('zero-tail-full','lookup',p,self.hit(101));b=self.render('zero-tail-ablated','no-tail',p,self.hit(101));self.check('tail-ablation-identity',np.array_equal(a,b))
  p=dict(crack=0,balance=.6)
  a=self.render('zero-crack-full','lookup',p,self.hit(101));b=self.render('zero-crack-ablated','single-crack',p,self.hit(101));self.check('cluster-ablation-identity',np.array_equal(a,b))
  rate_checks=[]
  for drive in (0.,1.):
   p=dict(pitch_hz=450,balance=0,crack=0,drive=drive,tone=.8,decay=.8)
   a=self.render('rate48-'+str(drive),'lookup',p,self.hit(4800,64),seconds=1.3)
   b=self.render('rate96-'+str(drive),'lookup',p,self.hit(9600,128),rate=96000,seconds=1.3)
   b=resample_poly(b.astype(float),1,2,window=('kaiser',10.));sl=slice(6000,48000)
   er=float(20*np.log10(np.sqrt(np.mean((a[sl]-b[sl])**2))/np.sqrt(np.mean(b[sl]**2))))
   rate_checks.append(dict(drive=drive,relative_rms_db=er))
  self.report['rate_consistency_not_isolated_alias_energy']=rate_checks
  mapping=json.loads((MOD/'adapter-map.json').read_text())['columns'];p={mapping[k]:self.patches['Hard'][mapping[k]] for k in mapping};p['pitch_hz']=self.patches['Hard']['pitch_hz']
  x=self.render('adapter-score','lookup',p,self.hit(2301),seconds=.8);gold=np.fromfile(self.out/'prepared-Hard.f32',dtype='<f4');self.check('adapter-lane-translation',np.array_equal(x,gold))
  self.auditions();self.reference_study();self.benchmarks();self.report['passed']=True
 def auditions(self):
  ev=[];timeline=[]
  for i,(name,p) in enumerate(self.patches.items()):
   n=2400+i*96000;ev +=[(n,k,v) for k,v in p.items()]+[(n,'velocity',1)]+self.hit(n);ev +=[(n+36000,'velocity',.5)]+self.hit(n+36000);timeline.append(dict(patch=name,start_s=n/48000))
  x=self.render('anchors','lookup',events=ev,seconds=16.5);write_wav(self.out/'analog-snare-anchors.wav',x);self.report['auditions']['analog-snare-anchors.wav']=timeline
  pattern=[]
  for i in range(64):
   if i%8==4 or i in (15,23,30,31,47,54,55,62,63):
    n=2400+i*6000;p=list(self.patches.values())[1+(i//12)%7];pattern +=[(n,k,v) for k,v in p.items()]+[(n,'velocity',1 if i%8==4 else .45)]+self.hit(n)
  for label in ('lookup','single-crack'):
   x=self.render('pattern-'+label,label,events=pattern,seconds=9);file='analog-snare-pattern.wav' if label=='lookup' else 'analog-snare-pattern-single-crack.wav';write_wav(self.out/file,x);self.report['auditions'][file]='Same persistent score, same gain; secondary impacts removed only in single-crack.'
  ev=[]
  for j,key in enumerate(self.man['musical_control_order']):
   for i in range(8):
    n=2400+j*144000+i*18000;p=self.patches['Classic'].copy();p[key]=70*(500/70)**(i/7) if key=='pitch_hz' else i/7;ev +=[(n,k,v) for k,v in p.items()]+self.hit(n)
  x=self.render('controls','lookup',events=ev,seconds=22);write_wav(self.out/'analog-snare-controls.wav',x);self.report['auditions']['analog-snare-controls.wav']=self.man['musical_control_order']
  pm=json.loads((ROOT/'modules/snare-pm/patches.json').read_text())['anchors'];audio=[]
  for old,new in [('Woody','Classic'),('Crack','Tight'),('Noise','Wire'),('Driven','Hard')]:
   comparison_pitch=self.patches[new]['pitch_hz'];comparison_decay=self.patches[new]['decay']*math.log(32)/math.log(36)
   pm_patch=pm[old]|{'pitch_hz':comparison_pitch,'decay':comparison_decay}
   a=self.render('compare-pm-'+old,'pm',pm_patch,self.hit(2400)+[(26400,'velocity',.5)]+self.hit(26400),seconds=1.3)
   b=self.render('compare-analog-'+new,'lookup',self.patches[new],self.hit(2400)+[(26400,'velocity',.5)]+self.hit(26400),seconds=1.3)
   ga=phrase_gain(a);gb=phrase_gain(b)
   audio +=[a*ga,np.zeros(6000),b*gb,np.zeros(12000)]
   self.report.setdefault('pm_comparison_gains',[]).append(dict(pm=old,analog=new,pm_gain=ga,analog_gain=gb,pitch_hz=comparison_pitch,pm_decay=comparison_decay,body_tau_s=.025*32**self.patches[new]['decay']))
  write_wav(self.out/'analog-snare-versus-pm.wav',np.concatenate(audio));self.report['auditions']['analog-snare-versus-pm.wav']='PM then analog, four patch pairs. Matched pitch, gates, velocity and body decay tau; attack/noise laws differ. Whole-phrase RMS target .12, bounded peak .9, recorded gains; no EQ. Authored counterparts, not exhaustive matching.'
 def reference_study(self):
  # These recordings are DIGITAL SD Basic/Vintage, NOT SD Classic or TR-808.
  if not self.refs or not (self.refs/'manifest.json').exists():
   self.report['references']={'status':'not supplied','hardware_match':False};return
  records=json.loads((self.refs/'manifest.json').read_text());R=[];names=[]
  for row in records['records']:
   p=self.refs/(row['id']+'.wav');self.check('reference-hash:'+row['id'],sha(p)==row['wav_sha256']);rate,x=wavfile.read(p);R.append(descriptors(x,rate));names.append(row['id'])
  rng=np.random.default_rng(471106);params=[]
  for i in range(24):
   p={k:float(rng.random()) for k in self.man['visible_columns']};p.update(pitch_hz=float(rng.uniform(100,300)),drive=.55*p['drive']);params.append(p)
  pools={};raws={}
  for label in ('lookup','no-tail','single-crack'):
   D=[];files=[]
   for i,p in enumerate(params):
    x=self.render(f'coverage-{label}-{i}',label,p,self.hit(2400),seconds=5);D.append(descriptors(x[2400:],48000));files.append(f'coverage-{label}-{i}.f32')
   pools[label]=np.vstack(D);raws[label]=files
  scale=np.array([.20,.45,.35,.25,.25,.15]);R=np.array(R);result={}
  for label,D in pools.items():
   distances=np.sqrt(np.mean(((R[:,None,:]-D[None,:,:])/scale)**2,axis=2));nearest=distances.argmin(axis=1)
   result[label]={'mean_nearest':float(distances.min(axis=1).mean()),'per_reference':{n:{'distance':float(distances[j,nearest[j]]),'candidate':int(nearest[j])} for j,n in enumerate(names)}}
  self.report['references']={'status':'evaluated','source_manifest':records,'scale':scale.tolist(),'parameters':params,'comparison':result,'limits':'Exploratory comparison to digital SD Basic/Vintage, unknown settings/firmware/gain and lossy codec. NOT analog hardware calibration or held-out validation. No fitted control curves.'}
  audio=[];gains=[]
  for j in (0,2,4,6):
   rate,a=wavfile.read(self.refs/(names[j]+'.wav'));a=np.asarray(a,float);k=result['lookup']['per_reference'][names[j]]['candidate'];b=np.fromfile(self.out/raws['lookup'][k],dtype='<f4')[2400:]
   a=a[:96000];b=b[:96000];g1=.1/max(np.sqrt(np.mean(a*a)),1e-12);g2=.1/max(np.sqrt(np.mean(b.astype(float)**2)),1e-12);s=max(1,abs(a*g1).max()/.9,abs(b*g2).max()/.9);g1/=s;g2/=s
   audio +=[a*g1,np.zeros(6000),b*g2,np.zeros(12000)];gains.append(dict(reference=names[j],candidate=k,reference_gain=g1,candidate_gain=g2))
  write_wav(self.out/'analog-snare-reference-context.wav',np.concatenate(audio));self.report['reference_listening_gains']=gains
  self.report['auditions']['analog-snare-reference-context.wav']='Digital snare preview then nearest analog-style candidate, not a clone comparison. Whole-hit gains above, no EQ; excerpts <=2 seconds.'
 def benchmarks(self):
  perf=[]
  for block in (32,64,128,512):
   reps=[]
   for i in range(3):
    labels=['direct','lookup','vector'];labels=labels[i:]+labels[:i];row={}
    for label in labels:row[label]=json.loads(cmd([self.out/label/'benchmark',block]));self.check(f'ordinary-new:{block}:{i}:{label}',row[label]['ordinary_new_allocations']==0)
    reps.append(row)
   perf.append({'block':block,'pairs':reps,'lookup_speedup':statistics.median(r['direct']['p50_us']/r['lookup']['p50_us'] for r in reps),'vector_speedup':statistics.median(r['direct']['p50_us']/r['vector']['p50_us'] for r in reps)})
  self.report['performance']=perf;self.report['performance_scope']='Same full synth, four voices; warmed, alternating order, 3 reps; ordinary new hook excludes malloc/aligned/host allocation. Offline Linux, not a device callback test.'
  header=(self.out/'lookup/generated.hpp').read_text();arrays=re.findall(r'(?:static\s+float|static\s+const\s+float|const\s+static\s+float)\s+(\w+)\[(\d+)\]',header);self.report['shared_float_arrays']=[dict(name=n,elements=int(c),bytes=4*int(c)) for n,c in arrays]
 def save(self,error=None):
  self.report['failure']=error
  if error:self.report['passed']=False
  try:self.report['source_commit']=cmd(['git','rev-parse','HEAD']).strip()
  except Exception:self.report['source_commit']=None
  self.report['source_files']={}
  paths=list(MOD.glob('*'))+[ROOT/'tools/modules/analog_snare_delivery.py',ROOT/'tools/modules/analog_snare_benchmark.cpp',ROOT/'tools/modules/render.cpp',ROOT/'tests/test_analog_snare_delivery.py']+list((ROOT/'modules/snare-pm').glob('*'))
  for p in paths:
   if p.is_file():
    rel=p.relative_to(ROOT);dest=self.out/'source'/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,dest);self.report['source_files'][str(rel)]=sha(p)
  self.report['thresholds']={'lookup_max_sample_error':PARITY_ABS,'envelope_relative_error':ENVELOPE_REL,'envelope_floor':1e-4}
  (self.out/'report.json').write_text(json.dumps(self.report,indent=2,allow_nan=False)+'\n');print(json.dumps(dict(passed=self.report['passed'],renders=len(self.report['renders']),checks=len(self.report['checks']),failure=error)))

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--replay',type=Path);p.add_argument('--references',type=Path);a=p.parse_args();s=Study(a.out.resolve(),a.replay.resolve() if a.replay else None,a.references.resolve() if a.references else None);error=None
 try:s.execute()
 except Exception as e:error=str(e)
 finally:s.save(error)
 if error:raise SystemExit(error)
if __name__=='__main__':main()
