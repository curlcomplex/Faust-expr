"""Actual-Faust Metal batch, frozen references, tests, audition and paired costs.
--replay checks source/generated hashes and reuses unchanged generated C++.
Descriptor coverage is not hardware fidelity. No private host dependency.
"""
from __future__ import annotations
import argparse, hashlib, itertools, json, math, os, platform, random, shutil, statistics, subprocess, wave
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
ROOT=Path(__file__).resolve().parents[2];MOD=ROOT/'modules/metal-pm'
TRAIN=('CYA-01','CYA-04','CYA-07','CYA-10','CYA-13','CYA-16');TEST=('CYA-03','CYA-09')
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def command(args,timeout=180):
 p=subprocess.run(list(map(str,args)),cwd=ROOT,text=True,capture_output=True,timeout=timeout)
 if p.returncode:raise RuntimeError(f'{args}\n{p.stdout}\n{p.stderr}')
 return p.stdout

def validate(values,manifest):
 for k,v in values.items():
  if k not in manifest['controls'] or isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v):raise ValueError('control '+str(k))
  c=manifest['controls'][k]
  if not c['min']<=v<=c['max'] or (k in ('gate','choke') and v not in (0,1)):raise ValueError('range '+k)

def score_rows(defaults,params,events,frames,manifest):
 validate(params,manifest);rows={(0,k):v for k,v in (defaults|params).items()};seen=set()
 for n,k,v in events:
  if not isinstance(n,int) or isinstance(n,bool) or n<0 or n>=frames:raise ValueError('frame')
  validate({k:v},manifest)
  if (n,k) in seen:raise ValueError('duplicate event')
  seen.add((n,k));rows[n,k]=v
 return sorted((n,k,v) for (n,k),v in rows.items())

def write_wav(path,x):
 x=np.asarray(x)
 if not np.isfinite(x).all() or np.max(np.abs(x))>=1:raise ValueError('refuse clipping or hidden normalization')
 with wave.open(str(path),'wb') as f:f.setnchannels(1);f.setsampwidth(2);f.setframerate(48000);f.writeframes(np.rint(x*32767).astype('<i2').tobytes())

def descriptor(samples,rate=48000):
 """Level-invariant time/spectrum coverage, never amplify a silent tail.
 Each window contributes its actual energy fraction and energy-weighted
 spectral features. Spectral floors must not turn silence into white noise.
 """
 x=np.asarray(samples,dtype=float).reshape(-1)
 if not len(x) or not np.isfinite(x).all():raise ValueError('audio')
 peak=float(np.max(np.abs(x)))
 if peak<1e-12:raise ValueError('silent descriptor')
 onset=int(np.flatnonzero(np.abs(x)>peak*.005)[0]);x=x[onset:onset+int(rate*6)]
 energy=x*x;total=max(float(energy.sum()),1e-30);cum=np.cumsum(energy)/total
 result=[np.searchsorted(cum,.5)/rate,np.searchsorted(cum,.9)/rate]
 for lo,hi in ((0,.05),(.05,.4),(.4,2.0),(2.0,6.0)):
  y=x[round(lo*rate):round(hi*rate)]
  fraction=float(np.dot(y,y)/total)
  if len(y)<16 or fraction<1e-8:
   result += [0.0]*6
   continue
  spec=np.abs(np.fft.rfft(y*np.hanning(len(y)),n=max(4096,1<<(len(y)-1).bit_length())))**2
  freq=np.fft.rfftfreq(2*(len(spec)-1),1/rate)
  power=float(spec.sum())
  if power<=0:
   result += [0.0]*6
   continue
  spec/=power;weight=math.sqrt(fraction)
  result += [fraction,weight*float(np.log10(max(1,(freq*spec).sum())))]
  result += [weight*float(spec[(freq>=a)&(freq<b)].sum()) for a,b in ((40,500),(500,2500),(2500,8000),(8000,20000))]
 return np.array(result)

def fullband_pitch(x,rate):
 y=np.asarray(x,dtype=float);y=y-y.mean();n=1<<20
 p=np.abs(np.fft.rfft(y*np.hanning(len(y)),n=n));p[0]=0;k=int(np.argmax(p))
 z=np.log(np.maximum(p[k-1:k+2],1e-30));off=.5*(z[0]-z[2])/(z[0]-2*z[1]+z[2]) if len(z)==3 else 0
 return (k+off)*rate/n

class Study:
 def __init__(self,out,references,replay=None):
  self.out=out;out.mkdir(parents=True,exist_ok=True);self.refs=references;self.replay=replay
  self.man=json.loads((MOD/'manifest.json').read_text());self.defaults={k:v['default'] for k,v in self.man['controls'].items()}
  self.patches=json.loads((MOD/'patches.json').read_text())['anchors'];self.limits=self.man['numerical_oracles']
  self.report=dict(schema=1,checks=[],renders=[],builds={},passed=False,platform=platform.platform(),hardware_matched=False,human_approved=False,device_qualified=False)
  if replay:
   old=json.loads((replay/'report.json').read_text())
   for path,d in old['source_files'].items():
    if sha(ROOT/path)!=d:raise ValueError('replay source hash '+path)
   self.old=old
 def check(self,name,ok,**details):
  self.report['checks'].append(dict(name=name,passed=bool(ok),**details))
  if not ok:raise AssertionError(name+str(details))
 def build(self,label,source,vector=False,diagnostic=False):
  d=self.out/label;d.mkdir(exist_ok=True);flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vector else [])
  if self.replay:
   old=self.replay/label/'generated.hpp'
   if sha(old)!=self.old['builds'][label]['generated_sha256']:raise ValueError('generated content hash')
   shutil.copyfile(old,d/'generated.hpp')
  else:
   command([os.getenv('FAUST','faust'),'-I',MOD,*flags,MOD/source,'-o',d/'generated.hpp'])
   command([os.getenv('FAUST','faust'),'-I',MOD,'-e',MOD/source,'-o',d/'expanded.dsp'])
  cflags=['-std=c++17','-O2','-ffp-contract=off','-fstack-usage','-I'+str(d)]
  command([os.getenv('CXX','c++'),*cflags,ROOT/'tools/modules/render.cpp','-o',d/'render'])
  controls=command([d/'render','--controls']);(d/'controls.tsv').write_text(controls)
  if not diagnostic:
   command([os.getenv('CXX','c++'),*cflags,ROOT/'tools/modules/metal_benchmark.cpp','-o',d/'benchmark'])
   self.check(label+':IO',controls.splitlines()[0]=='io\t0\t1')
   values={line.split('\t')[0]:list(map(float,line.split('\t')[1:4])) for line in controls.splitlines()[1:]}
   self.check(label+':control-ids',set(values)==set(self.defaults))
   self.check(label+':control-values',all(np.allclose(values[k],[v['min'],v['max'],v['default']],rtol=0,atol=1e-6) for k,v in self.man['controls'].items()))
  self.report['builds'][label]=dict(source=source,source_sha256=sha(MOD/source),engine_sha256=sha(MOD/'engine.lib'),generated_sha256=sha(d/'generated.hpp'),binary_sha256=sha(d/'render'),faust_flags=flags,cxx_flags=cflags)
  return d/'render'
 def render(self,label,exe,params=None,events=None,rate=48000,block=128,seconds=1.2):
  frames=round(seconds*rate);rows=score_rows(self.defaults,params or {},events or [],frames,self.man)
  score=self.out/(label+'.tsv');raw=self.out/(label+'.f32');score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for n,k,v in rows))
  diag=json.loads(command([exe,score,raw,rate,block,frames,0]));x=np.fromfile(raw,dtype='<f4')
  self.check(label+':finite/headroom',len(x)==frames and np.isfinite(x).all() and np.max(np.abs(x))<self.limits['peak_limit'],peak=float(np.max(np.abs(x))))
  self.report['renders'].append(dict(label=label,build=exe.parent.name,rate=rate,block=block,frames=frames,score_sha256=sha(score),raw_sha256=sha(raw),peak=float(np.max(np.abs(x))),rms=float(np.sqrt(np.mean(x.astype(float)**2))),mean=float(np.mean(x)),max_jump=float(np.max(np.abs(np.diff(x)))),diag=diag))
  return x
 @staticmethod
 def hit(n,length=64):return [(n,'gate',1),(n+length,'gate',0)]
 def equivalent(self,name,a,b):
  err=a.astype(float)-b;absolute=float(np.max(np.abs(err)));relative=float(np.linalg.norm(err)/max(np.linalg.norm(a),1e-20))
  self.check(name,absolute<self.limits['recurrence_max_abs'] and relative<self.limits['recurrence_relative_rms'],max_abs=absolute,relative_rms=relative)
 def execute(self):
  ref=self.build('reference','reference.dsp');fast=self.build('fast','metal.dsp');vec=self.build('vector','metal.dsp',True);sparse=self.build('sparse','sparse.dsp');nf=self.build('no-feedback','no-feedback.dsp')
  self.report['compilers']=dict(faust='replay: unchanged generated C++' if self.replay else command([os.getenv('FAUST','faust'),'-v']).strip(),cxx=command([os.getenv('CXX','c++'),'--version']).strip())
  self.check('initial-silence',not np.any(self.render('silence',fast,seconds=.15)))
  for rate in (44100,48000,96000):
   for hz in (70,420,3000):
    p=dict(pitch_hz=hz,shape=0,drive=0,decay=.95,punch=0)
    x=self.render(f'pitch-{rate}-{hz}',fast,p,self.hit(rate//20),rate=rate,seconds=1.2)
    measured=fullband_pitch(x[rate//5:rate],rate)
    self.check(f'clean-base-pitch:{rate}:{hz}',abs(measured-hz)<.4,measured_hz=measured)
   for name,p in self.patches.items():
    ev=self.hit(rate//20)
    a=self.render(f'{rate}-{name}-ref',ref,p,ev,rate=rate,seconds=2.5)
    b=self.render(f'{rate}-{name}-fast',fast,p,ev,rate=rate,seconds=2.5)
    self.equivalent(f'approximation:{rate}:{name}',a,b)
   for_target=self.patches['Industrial'];on=rate//3;history=self.hit(101)
   pre=history+[(on-1,k,v) for k,v in for_target.items()]+self.hit(on)
   now=history+[(on,k,v) for k,v in for_target.items()]+self.hit(on)
   a=self.render(f'locks-pre-{rate}',fast,events=pre,rate=rate)
   b=self.render(f'locks-on-{rate}',fast,events=now,rate=rate)
   self.check('same-sample-lock:'+str(rate),np.array_equal(a,b))
  ev=self.hit(101)+[(8011,k,v) for k,v in self.patches['Open'].items()]+self.hit(8011)+[(19003,'choke',1),(19067,'choke',0)]+[(24001,k,v) for k,v in self.patches['Bell'].items()]+self.hit(24001)
  a=self.render('dynamic-ref',ref,events=ev);b=self.render('dynamic-fast',fast,events=ev);self.equivalent('dynamic-approximation',a,b)
  for block in (1,32,64,127,512):
   x=self.render('dynamic-block-'+str(block),fast,events=ev,block=block)
   self.check('block-invariance:'+str(block),np.array_equal(x,b),max_abs=float(np.max(abs(x-b))))
  v=self.render('dynamic-vector',vec,events=ev);self.check('vector-parity',np.max(abs(v-b))<self.limits['vector_max_abs'],max_abs=float(np.max(abs(v-b))))
  a=self.render('gate-pulse',fast,events=self.hit(101,1));b=self.render('gate-held',fast,events=[(101,'gate',1)])
  self.check('gateoff-not-choke',np.array_equal(a,b))
  h=self.render('half-velocity',fast,dict(velocity=.5),self.hit(101,1));self.check('linear-velocity',np.max(abs(h-a*.5))<2e-6)
  z=self.render('zero-velocity',fast,dict(velocity=0),self.hit(101));self.check('zero-velocity-silent',not np.any(z))
  events=self.hit(101,1)+[(3001,k,v) for k,v in self.patches['Industrial'].items()]+[(3001,'velocity',.2)]
  x=self.render('latched-tail',fast,events=events);self.check('tail-not-rewritten',np.array_equal(a,x))
  for rate in (44100,48000,96000):
   on=rate//20;kill=rate//5;p=dict(decay=1,shape=.9)
   base=self.render(f'choke-base-{rate}',fast,p,self.hit(on),rate=rate,seconds=.8)
   x=self.render(f'choke-pulse-{rate}',fast,p,self.hit(on)+[(kill,'choke',1),(kill+1,'choke',0)],rate=rate,seconds=.8)
   n=np.arange(len(x));q=np.clip(1-(n-kill)/(rate*.008),0,1);expected=base*q*q*(3-2*q)
   self.check(f'choke-smoothstep:{rate}',np.max(abs(x-expected))<2e-6,max_abs=float(np.max(abs(x-expected))))
   self.check(f'choke-zero-no-resurrection:{rate}',not np.any(x[kill+math.ceil(rate*.008)+2:]))
   y=self.render(f'choke-retrigger-{rate}',fast,p,self.hit(on)+[(kill,'choke',1),(kill+1,'choke',0)]+self.hit(rate//2),rate=rate,seconds=.8)
   self.check(f'choke-new-hit:{rate}',np.max(abs(y[rate//2:]))>.02)
  a=self.render('priority-base',fast,events=self.hit(101));b=self.render('priority-both',fast,events=self.hit(101)+[(101,'choke',1),(102,'choke',0)])
  self.check('simultaneous-onset-priority',np.array_equal(a,b))
  keys=[k for k in self.man['musical_control_order'] if k!='pitch_hz'];ev=[];i=0
  for bits,hz in itertools.product(itertools.product((0.,1.),repeat=7),(70.,3000.)):
   n=101+i*1536;i+=1;ev += [(n,k,v) for k,v in zip(keys,bits)]+[(n,'pitch_hz',hz)]+self.hit(n,64)
  x=self.render('endpoints',fast,events=ev,seconds=(i*1536+48000)/48000,block=127)
  self.check('endpoint-mean',abs(float(np.mean(x)))<.02,settings=i,mean=float(np.mean(x)))
  ev=[]
  for i in range(128):
   n=101+i*192;ev += [(n,'color',(i%13)/12),(n,'sweep',(i%17)/16),(n,'shape',(i%11)/10),(n,'decay',(i%7)/6)]+self.hit(n,32)
  self.render('rapid-locks',fast,events=ev,seconds=1.2,block=32)
  for rate in (44100,48000,96000):
   p=dict(decay=1,shape=.5,contour=.2);events=self.hit(101)
   a=self.render(f'long-ref-{rate}',ref,p,events,rate=rate,seconds=18)
   b=self.render(f'long-fast-{rate}',fast,p,events,rate=rate,seconds=18)
   self.equivalent(f'long-envelope:{rate}',a,b)
  self.envelope_diagnostic()
  self.coverage(ref,fast,sparse)
  self.audition(fast,sparse)
  diagnostics=[]
  for name,p in [('clean',dict(pitch_hz=900,shape=0,drive=0)),('dense',dict(pitch_hz=900,shape=.9,drive=.2,contour=.1)),('extreme',dict(pitch_hz=2600,shape=1,drive=1,contour=0))]:
   for label,exe in [('full',fast),('no-feedback',nf)]:
    a=self.render(f'rate-{name}-{label}-48',exe,p,self.hit(2400),seconds=1)
    b=self.render(f'rate-{name}-{label}-96',exe,p,self.hit(4800,128),rate=96000,seconds=1)
    down=resample_poly(b.astype(float),1,2,window=('kaiser',10));sl=slice(4800,36000)
    error=float(np.linalg.norm(a[sl]-down[sl])/max(np.linalg.norm(down[sl]),1e-20))
    diagnostics.append(dict(case=name,entry=label,relative_rms_db=20*math.log10(max(error,1e-15))))
  self.report['rate_sensitivity_not_alias_proof']=diagnostics
  bench=[];labels=['reference','fast','vector']
  for block in (32,64,128,512):
   pairs=[]
   for repeat in range(4):
    order=labels[repeat%3:]+labels[:repeat%3];row={}
    for label in order:row[label]=json.loads(command([self.out/label/'benchmark',block]))
    self.check(f'ordinary-new-guard:{block}:{repeat}',all(r['ordinary_new_allocations']==0 for r in row.values()));pairs.append(row)
   bench.append(dict(block=block,pairs=pairs,median_reference_over_fast=statistics.median(r['reference']['p50_us']/r['fast']['p50_us'] for r in pairs),median_reference_over_vector=statistics.median(r['reference']['p50_us']/r['vector']['p50_us'] for r in pairs)))
  self.report['performance']=bench;self.report['performance_scope']='Offline four-voice compute only, warm hosted/container CPU; not device callback/thermal. Ordinary new/new[] guard excludes malloc/aligned allocation.'
  self.report['passed']=True
 def envelope_diagnostic(self):
  exe=self.build('envelopes','envelopes.dsp',diagnostic=True)
  controls=command([exe,'--controls']).splitlines()
  self.check('envelope-diagnostic:IO',controls[0]=='io\t0\t2')
  self.check('envelope-diagnostic:controls',{r.split('\t')[0] for r in controls[1:]}=={'decay','punch','gate'})
  for rate in (44100,48000,96000):
   for decay,punch in ((0.,0.),(.5,.5),(1.,1.)):
    seconds=.05+9*.015*160**decay;frames=round(seconds*rate);on=round(.05*rate)
    label=f'envelopes-{rate}-{decay}'
    score=self.out/(label+'.tsv');raw=self.out/(label+'.f32')
    score.write_text(f'0 decay {decay}\n0 punch {punch}\n0 gate 0\n{on} gate 1\n{on+1} gate 0\n')
    diag=json.loads(command([exe,score,raw,rate,127,frames,0]))
    x=np.fromfile(raw,dtype='<f4').reshape(frames,2)
    self.check(label+':finite',np.isfinite(x).all() and np.min(x)>=0 and np.max(x)<=1)
    mask=x[:,0]>.001
    relative=float(np.max(abs(x[mask,0]-x[mask,1])/x[mask,0]))
    self.check(label+':audible-relative',relative<self.limits['envelope_relative_above_minus60'],max_relative=relative)
    self.check(label+':onset-zero',np.all(x[:on+1]==0))
    self.report['renders'].append(dict(label=label,build='envelopes',rate=rate,block=127,frames=frames,channels=2,score_sha256=sha(score),raw_sha256=sha(raw),peak=float(np.max(x)),rms=float(np.sqrt(np.mean(x.astype(float)**2))),mean=float(np.mean(x)),max_jump=float(np.max(abs(np.diff(x,axis=0)))),diag=diag))
  cases=['0 made_up 1\n','0 gate 0.5\n','0 choke 0.5\n','0 decay nan\n','0 pitch_hz 0\n','10 gate 1\n5 gate 0\n','0 gate 0\n0 gate 1\n']
  for i,text in enumerate(cases):
   path=self.out/f'invalid-{i}.tsv';path.write_text(text)
   p=subprocess.run([str(self.out/'fast/render'),str(path),str(self.out/f'invalid-{i}.f32'),'48000','128','1000','0'],capture_output=True,text=True,timeout=10)
   self.check('native-score-rejection:'+str(i),p.returncode!=0)
 def coverage(self,reference,fast,sparse):
  manifest=json.loads((self.refs/'manifest.json').read_text());records={x['id']:x for x in manifest['records']}
  data={};rdesc={}
  for name in TRAIN+TEST:
   if sha(self.refs/(name+'.wav'))!=records[name]['wav_sha256']:raise ValueError('reference hash')
   rate,x=wavfile.read(self.refs/(name+'.wav'));data[name]=x; rdesc[name]=descriptor(x,rate)
  rng=random.Random(17092026);params=[]
  for i in range(48):
   p={k:rng.random() for k in self.man['musical_control_order'] if k!='pitch_hz'};p['pitch_hz']=70*(3000/70)**rng.random();p['drive']*=.7;params.append(p)
  pools={};raws={}
  for label,exe in [('dense',fast),('sparse',sparse)]:
   desc=[];raws[label]=[]
   for i,p in enumerate(params):
    x=self.render(f'coverage-{label}-{i}',exe,p,self.hit(101),seconds=6);raws[label].append(x);desc.append(descriptor(x))
   pools[label]=np.vstack(desc)
  development=np.vstack([np.vstack([rdesc[n] for n in TRAIN]),*pools.values()]);mu=development.mean(0);sd=np.maximum(development.std(0),.01)
  result={};nearest={}
  for label,P in pools.items():
   distances=np.abs(((np.vstack([rdesc[n] for n in TRAIN+TEST])-mu)/sd)[:,None,:]-((P-mu)/sd)[None,:,:]).mean(axis=2)
   best=np.argmin(distances,axis=1);values=distances[np.arange(8),best]
   result[label]=dict(development_mean=float(values[:6].mean()),reserved_mean=float(values[6:].mean()),per_reference={name:dict(distance=float(values[i]),pool_index=int(best[i]),params=params[int(best[i])]) for i,name in enumerate(TRAIN+TEST)})
   nearest[label]=best
  self.report['reference_coverage']=dict(metric='v2: mean absolute standardized energy-weighted temporal/spectral difference; no silent-tail floor',scale_mean=mu.tolist(),scale_std=sd.tolist(),candidate_seed=17092026,candidate_pool=params,development=list(TRAIN),reserved=list(TEST),results=result,limits='Fixed-pool nearest neighbors; not recovered patches, known-control generalization, or realism percentages. Analysis uses at most six seconds after detected onset. Reserved clips do not set scale or pools. They were inspected under v1 before correcting silent-tail bias; v2 is not untouched holdout validation.')
  clips=[];gains=[]
  for i,name in enumerate(TRAIN+TEST):
   a=np.asarray(data[name],float)[:96000];b=np.asarray(raws['dense'][int(nearest['dense'][i])],float)[:96000]
   gain=float(np.sqrt(np.mean(a*a))/max(np.sqrt(np.mean(b*b)),1e-12));gains.append(dict(reference=name,candidate_gain=gain))
   clips += [a,np.zeros(9600),b*gain,np.zeros(19200)]
  preview=np.concatenate(clips);atten=min(1.,.8/max(float(np.max(abs(preview))),1e-12));write_wav(self.out/'metal-reference-nearest.wav',preview*atten)
  self.report['reference_audition']=dict(per_candidate_whole_excerpt_gains=gains,global_attenuation=atten,reference_then_candidate=True,not_individually_fitted=True)
 def audition(self,fast,sparse):
  ev=[];timeline=[]
  for i,(name,p) in enumerate(self.patches.items()):
   n=2400+i*120000;ev += [(n,k,v) for k,v in p.items()]+[(n,'velocity',1)]+self.hit(n)
   n2=n+48000;ev += [(n2,'velocity',.55)]+self.hit(n2);timeline.append(dict(patch=name,start_s=n/48000,soft_s=n2/48000))
  x=self.render('anchors',fast,events=ev,seconds=21);write_wav(self.out/'metal-anchors.wav',x)
  self.report['anchors']=timeline
  ev=[]
  for i in range(64):
   if i%2==0 or i in (7,15,23,30,31,39,47,55,62,63):
    n=2400+i*6000;p=list(self.patches.values())[(i//8)%8]
    ev += [(n,k,v) for k,v in p.items()]+[(n,'velocity',.9 if i%4==0 else .4)]+self.hit(n)
    if i in (18,34,50):ev += [(n+3000,'choke',1),(n+3001,'choke',0)]
  for label,exe in [('dense',fast),('sparse',sparse)]:
   x=self.render('pattern-'+label,exe,events=ev,seconds=9);write_wav(self.out/('metal-pattern'+('' if label=='dense' else '-sparse')+'.wav'),x)
  ev=[]
  for j,key in enumerate(self.man['musical_control_order']):
   for i in range(8):
    n=2400+j*144000+i*18000;p=self.patches['Ride'].copy();p[key]=70*(3000/70)**(i/7) if key=='pitch_hz' else i/7
    ev += [(n,k,v) for k,v in p.items()]+[(n,'velocity',1)]+self.hit(n)
  x=self.render('controls',fast,events=ev,seconds=25);write_wav(self.out/'metal-controls.wav',x)
  self.report['audition_processing']='Fixed kernel gain, PCM16 only for anchors/patterns/controls; reference-nearest separately level-matched as recorded.'
 def save(self,error=None):
  if error:self.report['passed']=False
  self.report['failure']=error;self.report['source_files']={}
  paths=list(MOD.glob('*'))+list((ROOT/'tools/modules').glob('metal_*'))+[ROOT/'tools/modules/render.cpp',ROOT/'tools/modules/fetch_metal_references.py',ROOT/'tools/modules/fetch_snare_references.py',ROOT/'tests/test_metal_batch.py']
  for p in paths:
   if p.is_file():
    rel=p.relative_to(ROOT);self.report['source_files'][str(rel)]=sha(p);d=self.out/'source'/rel;d.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,d)
  try:self.report['source_commit']=command(['git','rev-parse','HEAD']).strip()
  except Exception:self.report['source_commit']=None
  if not self.replay:
   lib=Path('/usr/share/faust');self.report['system_faust_libraries']={str(p.relative_to(lib)):sha(p) for p in sorted(lib.glob('**/*.lib'))} if lib.exists() else {}
  (self.out/'report.json').write_text(json.dumps(self.report,indent=2)+'\n');print(json.dumps(dict(passed=self.report['passed'],renders=len(self.report['renders']),checks=len(self.report['checks']),failure=error)))

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--references',type=Path,required=True);p.add_argument('--replay',type=Path)
 a=p.parse_args();s=Study(a.out.resolve(),a.references.resolve(),a.replay.resolve() if a.replay else None);error=None
 try:s.execute()
 except Exception as e:error=str(e)
 finally:s.save(error)
 if error:raise SystemExit(error)
if __name__=='__main__':main()
