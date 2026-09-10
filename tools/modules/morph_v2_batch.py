"""Actual-Faust stereo Morph qualification, same-sound benchmark, and replay.
Nord is architectural/listening prior art, NOT a captured dry calibration target.
"""
from __future__ import annotations
import argparse,hashlib,itertools,json,math,os,platform,shutil,statistics,subprocess,wave
from pathlib import Path
import numpy as np
from scipy.signal import resample_poly
from generate_morph_bank import write as generate_bank
ROOT=Path(__file__).resolve().parents[2];MOD=ROOT/'modules/morph-wavetable/v2'

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def cmd(args,timeout=180):
 p=subprocess.run(list(map(str,args)),cwd=ROOT,capture_output=True,text=True,timeout=timeout)
 if p.returncode:raise RuntimeError(f'{args}\n{p.stdout}\n{p.stderr}')
 return p.stdout

def validate(params,controls):
 for k,v in params.items():
  if k not in controls or isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v):raise ValueError('invalid '+k)
  c=controls[k]
  if not c['min']<=v<=c['max']:raise ValueError('bounds '+k)
  if k in ('stack','gate') and int(v)!=v:raise ValueError('integer '+k)

def make_events(defaults,params,events,frames,controls):
 validate(params,controls);rows={(0,k):v for k,v in (defaults|params).items()};seen=set()
 for n,k,v in events:
  if isinstance(n,bool) or not isinstance(n,int) or n<0 or n>=frames:raise ValueError('frame')
  validate({k:v},controls)
  if (n,k) in seen:raise ValueError('duplicate event')
  seen.add((n,k));rows[n,k]=v
 return [(n,k,v) for (n,k),v in sorted(rows.items())]

def wav(path,x,rate=48000):
 x=np.asarray(x);x=x[:,None] if x.ndim==1 else x
 if not len(x) or not np.isfinite(x).all() or abs(x).max()>=1:raise ValueError('invalid/unbounded audio')
 with wave.open(str(path),'wb') as f:
  f.setnchannels(x.shape[1]);f.setsampwidth(2);f.setframerate(rate);f.writeframes(np.rint(x*32767).astype('<i2').tobytes())
 with wave.open(str(path),'rb') as f:
  if len(f.readframes(f.getnframes()))!=len(x)*x.shape[1]*2:raise ValueError('truncated WAV')

def pitch_estimate(x,rate):
 y=np.asarray(x,dtype=float);y=y-y.mean();n=1<<max(19,(len(y)-1).bit_length())
 s=abs(np.fft.rfft(y*np.hanning(len(y)),n));s[0]=0;k=int(s.argmax());z=np.log(np.maximum(s[k-1:k+2],1e-30));den=z[0]-2*z[1]+z[2]
 return (k+(.5*(z[0]-z[2])/den if den else 0))*rate/n

class Study:
 def __init__(self,out,replay=None):
  self.out=out;out.mkdir(parents=True,exist_ok=True);self.replay=replay
  self.man=json.loads((MOD/'manifest.json').read_text());self.defaults={k:c['default'] for k,c in self.man['controls'].items()}
  self.patches=json.loads((MOD/'patches.json').read_text())['anchors'];self.controls={}
  self.report={'passed':False,'checks':[],'renders':[],'builds':{},'auditions':{},'platform':platform.platform(),'hardware_matched':False,'user_approved':False,'target_qualified':False}
 def check(self,n,ok,**details):
  self.report['checks'].append(dict(name=n,passed=bool(ok),**details))
  if not ok:raise AssertionError(n+': '+str(details))
 def build(self,label,source,vector=False,diagnostic=False):
  d=self.out/label;d.mkdir(exist_ok=True);flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vector else [])
  if self.replay:
   old=self.replay/label;expected=json.loads((self.replay/'report.json').read_text())['builds'][label]
   self.check(label+':header-hash',sha(old/'generated.hpp')==expected['generated_sha256'])
   shutil.copyfile(old/'generated.hpp',d/'generated.hpp')
  else:
   # Each module owns its library directory: do not accidentally import Morph's
   # engine.lib when compiling the independent Tone comparison.
   cmd([os.getenv('FAUST','faust'),'-I',source.parent,'-I',self.out/'bank',*flags,source,'-o',d/'generated.hpp'],timeout=25 if vector else 180)
   cmd([os.getenv('FAUST','faust'),'-I',source.parent,'-I',self.out/'bank','-e',source,'-o',d/'expanded.dsp'])
  cppflags=['-std=c++17','-O2','-ffp-contract=off','-fstack-usage','-I'+str(d)]
  cmd([os.getenv('CXX','c++'),*cppflags,ROOT/'tools/modules/render.cpp','-o',d/'render'])
  text=cmd([d/'render','--controls']);(d/'controls.tsv').write_text(text)
  rows=text.splitlines();self.controls[label]={x.split('\t')[0]:{'min':float(x.split('\t')[1]),'max':float(x.split('\t')[2]),'default':float(x.split('\t')[3])} for x in rows[1:]}
  if not diagnostic:
   self.check(label+':ids',set(self.controls[label])==set(self.defaults));self.check(label+':stereo',rows[0]=='io\t0\t2')
   for k,c in self.man['controls'].items():self.check(label+':'+k,all(abs(self.controls[label][k][f]-c[f])<1e-5 for f in ('min','max','default')))
   cmd([os.getenv('CXX','c++'),*cppflags,ROOT/'tools/modules/morph_v2_benchmark.cpp','-o',d/'benchmark'])
  self.report['builds'][label]={'source':str(source.relative_to(ROOT)),'source_sha256':sha(source),'generated_sha256':sha(d/'generated.hpp'),'binary_sha256':sha(d/'render'),'faust_flags':flags,'cpp_flags':cppflags}
  return d/'render'
 def render(self,label,exe,params=None,events=None,seconds=1,rate=48000,block=128,diagnostic=False):
  controls=self.controls[exe.parent.name];defaults={k:c['default'] for k,c in controls.items()};frames=round(seconds*rate)
  rows=make_events(defaults,params or {},events or [],frames,controls)
  score=self.out/(label+'.tsv');raw=self.out/(label+'.f32');score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for n,k,v in rows))
  d=json.loads(cmd([exe,score,raw,rate,block,frames,0]));x=np.fromfile(raw,dtype='<f4').reshape(frames,d['channels'])
  self.check(label+':finite',np.isfinite(x).all());peak=float(abs(x).max())
  if not diagnostic:self.check(label+':headroom',peak<.85,peak=peak)
  self.report['renders'].append({'label':label,'build':exe.parent.name,'rate':rate,'block':block,'frames':frames,'channels':d['channels'],'score_sha256':sha(score),'raw_sha256':sha(raw),'peak':peak,'mean':float(x.mean()),'rms':float(np.sqrt(np.mean(x.astype(float)**2))),'max_jump':float(abs(np.diff(x,axis=0)).max()),'diagnostic':diagnostic})
  return x
 @staticmethod
 def note(start,end):return [(start,'gate',1),(end,'gate',0)]
 def run(self):
  if self.replay:shutil.copytree(self.replay/'bank',self.out/'bank',dirs_exist_ok=True)
  else:generate_bank(self.out/'bank')
  self.report['bank_sha256']=sha(self.out/'bank/bank.lib')
  ref=self.build('reference',MOD/'reference.dsp');fast=self.build('morph',MOD/'morph.dsp');vec=None
  if not self.replay or 'vector' in json.loads((self.replay/'report.json').read_text())['builds']:
   try:
    vec=self.build('vector',MOD/'morph.dsp',True)
    self.report['vector_status']='compiled; musical parity still required'
   except (RuntimeError,subprocess.TimeoutExpired) as error:
    self.report['vector_status']='not qualified: '+str(error)
  else:self.report['vector_status']='not qualified in original build; not fabricated in replay'
  env=self.build('envelope',MOD/'envelope.dsp',diagnostic=True);tun=self.build('tuning',MOD/'tuning.dsp',diagnostic=True)
  tone=self.build('tone',ROOT/'modules/tone-pm/playable/tone.dsp',diagnostic=True)
  silent=self.render('initial-silence',fast,seconds=.2);self.check('silence-exact',not np.any(silent))
  for rate in (44100,48000,96000):
   for i in range(8):
    p={'pitch_hz':220,'morph':i/7,'shape':.8,'stack':1+i%4,'detune':.55,'drive':.7}
    ev=self.note(101,round(rate*.22));a=self.render(f'frame-{rate}-{i}-ref',ref,p,ev,seconds=.7,rate=rate)
    b=self.render(f'frame-{rate}-{i}-fast',fast,p,ev,seconds=.7,rate=rate)
    self.check(f'clenshaw:{rate}:{i}',abs(a-b).max()<.0005,max_error=float(abs(a-b).max()))
   for hz in (20,55,220,880,8000):
    x=self.render(f'pitch-{rate}-{hz}',fast,{'pitch_hz':hz,'morph':0,'drive':0,'stack':1,'detune':1},self.note(101,round(rate*1.2)),seconds=1.4,rate=rate)
    measured=pitch_estimate(x[round(rate*.2):round(rate*1.2),0],rate)
    self.check(f'pitch:{rate}:{hz}',abs(1200*math.log2(measured/hz))<1.0,measured_hz=measured)
   for count in (1,2,3,4):
    x=self.render(f'tuning-{rate}-{count}',tun,{'pitch_hz':440,'stack':count,'detune':1},[(101,'gate',1)],rate=rate,seconds=.05,diagnostic=True)
    pair=x[500];expected=440*np.array([2**(-28/1200),2**(28/1200)]) if count>1 else np.array([440.,440.])
    self.check(f'center:{rate}:{count}',np.max(abs(1200*np.log2(pair/expected)))<.01,outer_hz=pair.tolist())
   for decay in (0.,1.):
    for length in (1,round(.1*rate)):
     start=101;end=start+length;seconds=1 if decay==0 else 28
     x=self.render(f'env-{rate}-{decay}-{length}',env,{'decay':decay},self.note(start,end),seconds=seconds,rate=rate,diagnostic=True)[:,0]
     t=np.maximum(0,(np.arange(len(x))-start)/rate);r=np.maximum(0,(np.arange(len(x))-end)/rate);tau=.035*100**decay
     truth=np.where(np.arange(len(x))<end,1-np.exp(-t/.0025),(1-math.exp(-(end-start)/rate/.0025))*np.exp(-r/tau))
     truth[r>=14*tau]=0;truth[:start]=0;mask=truth>1e-3;error=float(np.max(abs(x[mask]-truth[mask])/truth[mask]))
     self.check(f'envelope-oracle:{rate}:{decay}:{length}',error<1e-4,max_relative=error)
  ev=self.note(101,20000);p=self.patches['Brass']|{'stack':1,'detune':0}
  base=self.render('zero-detune-1',fast,p,ev)
  for count in (2,3,4):
   x=self.render(f'zero-detune-{count}',fast,p|{'stack':count},ev)
   self.check('zero-detune:'+str(count),abs(x-base).max()<2e-6,max_error=float(abs(x-base).max()))
  ev=self.note(101,32001)+[(7001,'morph',.9),(9001,'shape',0),(12007,'drive',1),(17011,'detune',1),(22001,'pitch_hz',329.628)]+self.note(37001,43001)
  base=self.render('dynamic-128',fast,{'stack':4},ev)
  for b in (1,32,64,127,256,512):
   x=self.render('dynamic-'+str(b),fast,{'stack':4},ev,block=b);self.check('block:'+str(b),np.array_equal(x,base),max_error=float(abs(x-base).max()))
  if vec is not None:
   x=self.render('dynamic-vector',vec,{'stack':4},ev);self.check('vector-parity',abs(x-base).max()<.0005,max_error=float(abs(x-base).max()))
  for rate in (44100,48000,96000):
   p=self.patches['Wide']|{'pitch_hz':311.127,'velocity':.6};start=1001
   a=self.render(f'locks-pre-{rate}',fast,p,self.note(start,round(.3*rate)),rate=rate)
   b=self.render(f'locks-same-{rate}',fast,events=[(start,k,v) for k,v in p.items()]+self.note(start,round(.3*rate)),rate=rate)
   self.check('same-onset:'+str(rate),np.array_equal(a,b),max_error=float(abs(a-b).max()))
  base=self.render('velocity-1',fast,events=self.note(101,18001))
  half=self.render('velocity-half',fast,{'velocity':.5},self.note(101,18001));zero=self.render('velocity-zero',fast,{'velocity':0},self.note(101,18001))
  self.check('velocity-half-law',abs(half-base*.5).max()<2e-6);self.check('velocity-zero-law',not np.any(zero))
  change=self.render('latched-tail',fast,events=self.note(101,18001)+[(5001,'decay',1),(5001,'stack',4),(5001,'velocity',0)])
  self.check('latched-stack-decay-velocity',np.array_equal(change,base))
  ev=[];i=0;keys=['morph','shape','decay','detune','drive']
  for count,bits,hz in itertools.product(range(1,5),itertools.product((0.,1.),repeat=5),(20.,8000.)):
   n=101+i*4096;i+=1;ev += [(n,k,v) for k,v in zip(keys,bits)]+[(n,'pitch_hz',hz),(n,'stack',count)]+self.note(n,n+2000)
  self.render('endpoints-256',fast,events=ev,seconds=(101+i*4096+48000)/48000,block=127)
  spectral=[]
  for drive in (0.,1.):
   for frame in (0,2,4,6):
    p={'pitch_hz':375,'morph':frame/7,'shape':1,'stack':1,'detune':0,'drive':drive}
    x=self.render(f'spectral-{frame}-{int(drive)}',fast,p,[(0,'gate',1)],seconds=1.)
    s=abs(np.fft.rfft(x[12000:44000,0]*np.hanning(32000)));f=np.fft.rfftfreq(32000,1/48000)
    high=float(np.sum(s[f>20500]**2)/max(np.sum(s*s),1e-30));spectral.append({'frame':frame,'drive':drive,'energy_above_20500':high})
    self.check(f'spectral-ceiling:{frame}:{drive}',high<1e-7,energy_fraction=high)
  self.report['spectral_checks']=spectral
  rates=[]
  for name,p in [('clean',self.patches['Pure']),('bright',self.patches['Wide']|{'pitch_hz':2000,'shape':1,'drive':1})]:
   lo=self.render('rate-'+name+'-48',fast,p,self.note(4800,30000),seconds=1.5)
   hi=self.render('rate-'+name+'-96',fast,p,self.note(9600,60000),seconds=1.5,rate=96000)
   down=resample_poly(hi.astype(float),1,2,axis=0,window=('kaiser',10));sl=slice(6000,60000);e=lo[sl]-down[sl]
   rates.append({'patch':name,'relative_rms_db':float(20*np.log10(max(1e-15,np.sqrt(np.mean(e*e)))/max(1e-15,np.sqrt(np.mean(down[sl]**2)))))})
  self.report['rate_consistency']=rates
  self.auditions(fast,tone);self.benchmark();self.report['passed']=True
 def auditions(self,fast,tone):
  ev=[];timeline=[]
  for i,(name,p) in enumerate(self.patches.items()):
   n=2400+i*144000;ev += [(n,k,v) for k,v in p.items()]+[(n,'pitch_hz',220),(n,'velocity',1)]+self.note(n,n+40000)
   ev += [(n+65000,'velocity',.5)]+self.note(n+65000,n+95000);timeline.append({'start_s':n/48000,'name':name})
  x=self.render('anchors',fast,events=ev,seconds=25);wav(self.out/'morph-anchors.wav',x);self.report['auditions']['morph-anchors.wav']=timeline
  notes=[110,146.832,164.814,220,293.665,329.628,196,146.832];ev=[]
  for i in range(32):
   n=2400+i*12000;p=list(self.patches.values())[i//4]
   ev += [(n,k,v) for k,v in p.items()]+[(n,'pitch_hz',notes[i%8]),(n,'velocity',1 if i%4==0 else .65)]+self.note(n,n+8000)
  x=self.render('pattern',fast,events=ev,seconds=9);wav(self.out/'morph-pattern.wav',x)
  ev=[];order=self.man['musical_control_order']
  for j,k in enumerate(order):
   start=2400+j*168000;p=self.patches['Brass']|{'pitch_hz':220,'stack':4,'detune':.5,'decay':.25}
   ev += [(start,key,v) for key,v in p.items() if key!=k]
   if k in ('decay','stack'):
    for i in range(8):
     n=start+i*17000;ev += [(n,k,1+round(i/7*3) if k=='stack' else i/7)]+self.note(n,n+8000)
   else:
    ev+=self.note(start,start+140000)
    for i in range(140):ev.append((start+i*1000,k,i/139))
  x=self.render('controls',fast,events=ev,seconds=22);wav(self.out/'morph-controls.wav',x);self.report['auditions']['morph-controls.wav']={'order':order,'seconds_each':3.5,'mode':'live held sweeps except onset-latched Decay/Stack'}
  ev=[]
  for count in (1,2,3,4):
   n=2400+(count-1)*168000;ev += [(n,'stack',count),(n,'detune',.6),(n,'morph',.32),(n,'shape',.8)]+self.note(n,n+120000)
  x=self.render('unison',fast,events=ev,seconds=15);wav(self.out/'morph-unison.wav',x)
  ys=[]
  for i,hz in enumerate((130.8128,164.8138,195.9977,261.6256)):
   ev=self.note(2400,100000)+[(2400+j*1000,'morph',.1+.8*j/90) for j in range(91)]
   ys.append(self.render(f'host-voice-{i}',fast,self.patches['Wide']|{'pitch_hz':hz},ev,seconds=6))
  poly=sum(ys)*.25;wav(self.out/'morph-host-polyphony.wav',poly)
  comparisons=[];gain_report=[]
  target_patches=[('Hollow',{'ratio':.6,'modulation':.35,'feedback':.1}),('Vowel',{'ratio':.8,'modulation':.6,'feedback':.1}),('Organ',{'ratio':.4,'modulation':.25,'feedback':0})]
  for name,tp in target_patches:
   ev=[]
   for i,hz in enumerate((220,293.665,329.628)):
    n=2400+i*20000;ev +=[(n,'pitch_hz',hz)]+self.note(n,n+15000)
   a=self.render('comparison-morph-'+name,fast,self.patches[name]|{'stack':1,'detune':0,'decay':.22},ev,seconds=2)
   tau=.035*100**.22;td=math.log(tau/.03,200)
   b=self.render('comparison-tone-'+name,tone,tp|{'gate_mode':1,'punch':0,'drive':0,'decay':td,'mod_env':0},ev,seconds=2)
   b=np.repeat(b,2,axis=1);ra=float(np.sqrt(np.mean(a.astype(float)**2)));rb=float(np.sqrt(np.mean(b.astype(float)**2)));gb=ra/max(rb,1e-10)
   comparisons.extend([b*gb,np.zeros((12000,2)),a,np.zeros((24000,2))]);gain_report.append({'Morph_frame':name,'tone_patch':tp,'tone_gain':gb,'matching':'same pitches/gates/release tau and whole-phrase RMS; attack curves differ; no EQ or per-note fitting'})
  collage=np.concatenate(comparisons);gain=min(1,.85/max(abs(collage).max(),1e-15));wav(self.out/'morph-versus-tone.wav',collage*gain)
  self.report['auditions']['morph-versus-tone.wav']={'order':'Tone then single-oscillator Morph for Hollow/Vowel/Organ; a bounded listening comparison, not proof Tone cannot reproduce every spectrum','pairs':gain_report,'shared_gain':gain}
  self.report['auditions']['morph-host-polyphony.wav']='four host notes/instances, four same-note oscillators each, summed at 1/4 per instance; no native chord expansion'
 def benchmark(self):
  data=[]
  for stack in (1,4):
   for b in (32,64,128,512):
    pairs=[]
    for rep in range(3):
     row={};order=[name for name in ('reference','morph','vector') if name in self.report['builds']];shift=rep%len(order);order=order[shift:]+order[:shift]
     for label in order:row[label]=json.loads(cmd([self.out/label/'benchmark',b,stack]))
     self.check(f'alloc:{stack}:{b}:{rep}',all(x['ordinary_new_in_compute']==0 for x in row.values()));pairs.append(row)
    data.append({'stack':stack,'frames':b,'pairs':pairs,'median_reference_over_fast':statistics.median(x['reference']['p50_us']/x['morph']['p50_us'] for x in pairs)})
  self.report['benchmarks']=data
  self.report['benchmark_scope']='four host voices, ordinary new hook only, DSP-only warm hosted/container timings; no target device/thermal/UI claim; all four oscillators compute even with Stack1'
 def save(self,error=None):
  if error:self.report['passed']=False
  self.report['failure']=error;self.report['cxx']=cmd([os.getenv('CXX','c++'),'--version']).strip()
  self.report['faust']='offline generated-C++ replay' if self.replay else cmd([os.getenv('FAUST','faust'),'-v']).strip()
  self.report['source_files']={}
  for p in list(MOD.glob('*'))+[ROOT/'modules/tone-pm/playable/engine.lib',ROOT/'tools/modules/render.cpp',ROOT/'tools/modules/generate_morph_bank.py',ROOT/'tools/modules/morph_v2_batch.py',ROOT/'tools/modules/morph_v2_benchmark.cpp']:
   if p.is_file():self.report['source_files'][str(p.relative_to(ROOT))]=sha(p)
  try:self.report['source_commit']=cmd(['git','rev-parse','HEAD']).strip()
  except Exception:self.report['source_commit']=json.loads((self.replay/'report.json').read_text()).get('source_commit') if self.replay else None
  (self.out/'report.json').write_text(json.dumps(self.report,indent=2)+'\n');print(json.dumps({'passed':self.report['passed'],'renders':len(self.report['renders']),'checks':len(self.report['checks']),'failure':error}))

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--replay',type=Path);a=p.parse_args();s=Study(a.out.resolve(),a.replay.resolve() if a.replay else None);error=None
 try:s.run()
 except Exception as e:error=str(e)
 finally:s.save(error)
 if error:raise SystemExit(error)
if __name__=='__main__':main()
