"""Batch #57: real Faust voice, sequencer, fused patch and selected-filter oracle.
No Python sound synthesizer. Python predicts control traces only.
"""
from pathlib import Path
import argparse, itertools, json, os, subprocess, traceback
import numpy as np
from scipy.io import wavfile
from hats_v2_delivery import Lab, command, digest
ROOT=Path(__file__).resolve().parents[2]
VOICE=ROOT/'modules/acid-voice/v1'
SEQ=ROOT/'modules/bassline-seq/v1'
PATCH=ROOT/'modules/analog-classics/acid-batch'
VOICE_DEFAULT=dict(freq=110.,gate=0.,velocity=1.,accent=0.,slide=0.,cutoff=650.,resonance=.68,envMod=.62,decay=.24,accentAmount=.65,slideTime=.06,wave=0.,drive=.12,level=.7)
SEQ_DEFAULT=dict(run=1.,length=16.,transpose=0.)|{f'{key}{i}':(45. if key=='note' else 0.) for i in range(32) for key in ('on','note','accent','slide')}
PRESETS={'Round':dict(cutoff=400,resonance=.28,envMod=.35,decay=.32,drive=.03), 'Acid':{}, 'Hollow':dict(wave=1,cutoff=450,resonance=.73,envMod=.72,decay=.18), 'Driven':dict(cutoff=850,resonance=.86,envMod=.75,drive=.64,decay=.28)}

def controls(exe):
 rows=command([exe,'--controls']).splitlines()
 return tuple(map(int,rows[0].split('\t')[1:])),{r[0]:list(map(float,r[1:])) for r in (s.split('\t') for s in rows[1:])}

def oracle(inputs,initial,events):
 changes={}
 for n,k,v in events:changes.setdefault(n,{})[k]=v
 p=SEQ_DEFAULT|initial
 out=np.zeros((len(inputs),5),np.float32)
 pos=0; active=False; enabled=tie=incoming=accent=0; freq=110.;pc=pr=False
 for n,(clock,reset) in enumerate(inputs):
  p.update(changes.get(n,{}));c=clock>0;r=reset>0;reset_edge=r and not pr;tick=c and not pc and p['run']>0
  old_active=active;old_tie=tie
  if reset_edge:pos=0;active=False
  if p['run']<=0:active=False
  if tick:
   pos=1+pos%int(p['length']);active=True;i=pos-1
   enabled=p[f'on{i}'];tie=enabled*p[f'slide{i}']*p[f'on{pos%int(p["length"])}']
   incoming=old_tie*old_active*(not reset_edge)*enabled
   accent=p[f'accent{i}']
   if enabled:freq=440.*2**((p[f'note{i}']+p['transpose']-69)/12)
  out[n]=freq,active*enabled*max(c,tie),active*enabled*accent,active*incoming,pos
  pc,pr=c,r
 return out

class Batch(Lab):
 def check(self,name,ok,**detail):
  self.report['checks'].append(dict(name=name,passed=bool(ok),**detail))
  if not ok:print('FAIL',name,detail,flush=True)
 def render(self,name,exe,values,events=(),sr=48000,block=128,frames=48000,inputs=None):
  rows={(0,k):float(v) for k,v in values.items()};seen=set()
  for n,k,v in events:
   if (n,k) in seen:raise ValueError('duplicate score event')
   seen.add((n,k));rows[n,k]=float(v)
  score=self.out/(name+'.tsv');raw=self.out/(name+'.f32')
  score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
  args=[exe,score,raw,sr,block,frames,0];record=dict(name=name,rate=sr,block=block,score_sha256=digest(score))
  if inputs is not None:
   inp=self.out/(name+'-input.f32');np.asarray(inputs,dtype='<f4').tofile(inp);args.append(inp);record['input_sha256']=digest(inp)
  d=json.loads(command(args));x=np.fromfile(raw,'<f4').reshape(-1,d['channels'])
  self.check(name+':finite',len(x)==frames and np.isfinite(x).all(),peak=float(abs(x).max()))
  record.update(raw_sha256=digest(raw),diagnostics=d);self.report['renders'].append(record)
  return x

def run(out):
 L=Batch(out);c=L.check;r=L.render
 L.report.update(version='acid-batch-0.1.0-experiment',commit=os.environ.get('GITHUB_SHA','local-snapshot'),presets=PRESETS,human_approved=False,hardware_approved=False,host_integrated=False,reference_kind='Selected Open303 filter equations, not upstream full synth or hardware')
 audition=L.out/'audition';audition.mkdir(exist_ok=True)
 try:
  voice=L.build('voice',VOICE/'voice.dsp');vv=L.build('voice-vector',VOICE/'voice.dsp',True)
  diag=L.build('voice-diagnostic',VOICE/'diagnostic.dsp');signal=L.build('voice-signal',VOICE/'signal.dsp')
  seq=L.build('seq',SEQ/'signal.dsp');sv=L.build('seq-vector',SEQ/'signal.dsp',True);sui=L.build('seq-ui',SEQ/'sequence.dsp')
  patch=L.build('patch',PATCH/'patch.dsp');pv=L.build('patch-vector',PATCH/'patch.dsp',True);second=L.build('second-voice',PATCH/'second-voice.dsp')
  filt=L.build('filter',VOICE/'filter-test.dsp')
  c('one-note-voice-io',controls(voice)[0]==(0,1));c('note-control-labels',set(controls(voice)[1])==set(VOICE_DEFAULT))
  c('signal-voice-io',controls(signal)[0]==(5,1));c('seq-five-lanes',controls(seq)[0]==(2,5));c('seq-ui-five-lanes',controls(sui)[0]==(0,5))
  # Demonstrate that the previous two-channel runner rejects the actual new sequence.
  old=(ROOT/'tools/modules/render.cpp').read_text().replace('ni>8','ni>2').replace('no>8','no>2')
  (L.out/'old-runner.cpp').write_text(old)
  command([os.getenv('CXX','c++'),'-std=c++17','-O2','-I'+str(L.out/'seq'),L.out/'old-runner.cpp','-o',L.out/'old-runner'])
  empty=L.out/'empty.tsv';empty.write_text('');dummy=L.out/'dummy.f32';np.zeros((64,2),'<f4').tofile(dummy)
  rejected=subprocess.run([str(L.out/'old-runner'),str(empty),str(L.out/'rejected.f32'),'48000','32','64','0',str(dummy)],capture_output=True,text=True,timeout=20)
  c('old-runner-rejects-five-lanes',rejected.returncode!=0 and 'I/O contract' in rejected.stderr)
  # Independent double-precision transcription of the selected upstream filter mode.
  ref=L.out/'filter-reference';command([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off',ROOT/'tools/modules/acid_filter_reference.cpp','-o',ref])
  stimulus=np.zeros((8192,1),np.float32);stimulus[16]=.2
  stimulus[256:2048,0]=.08*np.sin(np.arange(1792)*.173)
  deltas=[]
  for sr,cf,res in itertools.product((44100,48000,96000),(200,800,3000,5000),(0.,.5,.92)):
   name=f'filter-{sr}-{cf}-{res}'
   actual=r(name,filt,dict(cutoff=cf,resonance=res),sr=sr,frames=len(stimulus),inputs=stimulus)[:,0]
   raw=L.out/(name+'-reference.f32');command([ref,L.out/(name+'-input.f32'),raw,sr,cf,res])
   expected=np.fromfile(raw,'<f4');error=float(abs(actual-expected).max());deltas.append(error)
   c(name+':port-error',error<2e-4,max_absolute=error,reference_sha256=digest(raw))
  L.report['filter_max_error']=max(deltas)
  # Voice behaviour and genuinely live macro movement.
  notes=[(480,'gate',1),(24480,'gate',0)]
  x=r('voice-note',voice,VOICE_DEFAULT,notes,frames=96000)[:,0]
  c('audible',.005<abs(x).max()<1);c('silent-before-note',not np.any(x[:480]));c('released',abs(x[-4800:]).max()<1e-8)
  c('never-triggered',not np.any(r('voice-never',voice,VOICE_DEFAULT)))
  c('zero-velocity',not np.any(r('voice-zero',voice,VOICE_DEFAULT|{'velocity':0},notes)))
  half=r('voice-half',voice,VOICE_DEFAULT|{'velocity':.5},notes,frames=96000)[:,0];c('linear-velocity',abs(half-.5*x).max()<1e-6)
  for b in (1,32,64,127,256,512):c('voice-block-'+str(b),np.array_equal(r('voice-block-'+str(b),voice,VOICE_DEFAULT,notes,block=b,frames=96000)[:,0],x))
  c('voice-vector',abs(r('voice-vector',vv,VOICE_DEFAULT,notes,frames=96000)[:,0]-x).max()<3e-5)
  for sr in (44100,96000):r('voice-rate-'+str(sr),voice,VOICE_DEFAULT,[(round(sr*.01),'gate',1),(round(sr*.51),'gate',0)],sr=sr,frames=sr*2)
  for key in ('cutoff','resonance','envMod','decay','accentAmount','wave','drive','level'):
   bound=controls(voice)[1][key];events=notes+[(6000,key,bound[1])]
   y=r('live-'+key,voice,VOICE_DEFAULT|{'accent':1},events,frames=48000)
   c('live-'+key+':bounded',abs(y).max()<1)
  edges=[]
  for i,bits in enumerate(itertools.product((0,1),repeat=5)):
   n=480+i*1200
   for key,bit in zip(('cutoff','resonance','envMod','drive','wave'),bits):edges.append((n,key,controls(voice)[1][key][bit]))
   edges.extend([(n,'gate',1),(n+900,'gate',0)])
  r('voice-corners',voice,VOICE_DEFAULT,edges,frames=48000)
  # Trace pitch/envelopes rather than guessing glide from audio zero crossings.
  dv={k:v for k,v in VOICE_DEFAULT.items() if k in controls(diag)[1]}
  slide_notes=[(480,'gate',1),(6000,'freq',220),(6000,'slide',1),(18000,'freq',110),(30000,'gate',0)]
  z=r('glide-trace',diag,dv,slide_notes)
  c('glide-no-retrigger',np.flatnonzero(z[:,3]>.5).tolist()==[480])
  c('up-glide-monotone',np.all(np.diff(z[6000:18000,0])>=0));c('down-glide-monotone',np.all(np.diff(z[18000:30000,0])<=0))
  c('glide-starts-between',110<z[6000,0]<220)
  zero=r('zero-glide-trace',diag,dv|{'slideTime':0},slide_notes)
  c('zero-glide-exact-target',zero[6000,0]==220 and zero[18000,0]==110)
  plain=r('ordinary-pitch-trace',diag,dv,[(480,'gate',1),(6000,'freq',220)])
  c('ordinary-note-no-portamento',plain[6000,0]==220)
  acc=r('accent-trace',diag,dv,[(480,'gate',1),(6000,'accent',1),(18000,'gate',0)])
  c('tied-accent-no-retrigger',np.flatnonzero(acc[:,3]>.5).tolist()==[480]);c('accent-follows-and-releases',acc[12000,4]>.99 and acc[20000,4]>.99)
  # Real buffer-rate clock/reset traces. Integer lanes must be sample-exact.
  clock=np.zeros((1024,2),np.float32)
  for n in range(5,1010,11):clock[n:n+5,0]=1
  def equal(actual,expected):return np.array_equal(actual[:,1:],expected[:,1:]) and np.max(abs(actual[:,0]-expected[:,0]))<.002
  for length in (1,2,7,16,31,32):
   p=SEQ_DEFAULT|{'length':length}|{f'on{i}':float(i%5!=3) for i in range(32)}|{f'note{i}':36+(i%12) for i in range(32)}|{f'slide{i}':float(i%3==0) for i in range(32)}|{f'accent{i}':float(i%4==0) for i in range(32)}
   y=r('sequence-length-'+str(length),seq,p,frames=1024,inputs=clock)
   c('sequence-oracle-'+str(length),equal(y,oracle(clock,p,[])))
  special=clock.copy();special[5:8,1]=1;special[94:98,1]=1;special[148:155,1]=1;special[500:508,0]=-1
  p=SEQ_DEFAULT|{f'on{i}':1 for i in range(32)}|{'slide0':1,'slide1':1,'on3':0,'accent1':1,'note1':57}
  events=[(100,'run',0),(173,'run',1),(278,'length',7),(377,'length',31),(450,'on0',0)]
  expected=oracle(special,p,events)
  for b in (1,32,64,127,128,256,512):c('sequence-edges-'+str(b),equal(r('sequence-edges-'+str(b),seq,p,events,frames=1024,block=b,inputs=special),expected))
  c('sequence-vector',equal(r('sequence-vector',sv,p,events,frames=1024,inputs=special),expected))
  for sr in (44100,96000):c('sequence-rate-'+str(sr),equal(r('sequence-rate-'+str(sr),seq,p,events,sr=sr,frames=1024,inputs=special),expected))
  ui_events=[]
  for channel,key in enumerate(('clock','reset')):
   previous=0
   for n,v in enumerate(special[:,channel]):
    if v!=previous:ui_events.append((n,key,max(0,float(v))));previous=v
  # UI buttons are binary, so the negative signal segment maps to zero.
  c('sequence-ui-parity',equal(r('sequence-ui',sui,p|{'clock':0,'reset':0},sorted(ui_events+events),frames=1024),expected))
  empty_pattern=r('empty-sequence',seq,SEQ_DEFAULT,frames=1024,inputs=clock)
  c('empty-pattern-no-gate',not np.any(empty_pattern[:,1]));c('empty-pattern-pitch-default',np.all(empty_pattern[:,0]==110))
  # Negative control: a delayed trigger lane must fail the same trace comparison.
  mutant=L.out/'mutant';mutant.mkdir(exist_ok=True)
  text=(SEQ/'engine.lib').read_text();anchor='gateOut=gateValue<:'
  c('mutation-anchor',text.count(anchor)==1)
  (mutant/'engine.lib').write_text(text.replace(anchor,"gateOut=gateValue'<:"));(mutant/'signal.dsp').write_text((SEQ/'signal.dsp').read_text())
  bad=L.build('late-gate',mutant/'signal.dsp')
  c('oracle-rejects-late-gate',not equal(r('late-gate',bad,p,events,frames=1024,inputs=special),expected))
  # Actual Faust-to-Faust signal patch and split-render routing parity.
  frames=480000;inputs=np.zeros((frames,2),np.float32)
  for n in range(480,384480,6000):inputs[n:n+3000,0]=1
  musical=SEQ_DEFAULT|{f'on{i}':float(i not in (3,7,14)) for i in range(16)}|{f'note{i}':n for i,n in enumerate((33,33,45,33,36,33,40,33,33,45,43,40,36,33,33,45))}|{f'accent{i}':float(i in (0,4,10,15)) for i in range(16)}|{f'slide{i}':float(i in (1,4,8,9,12,15)) for i in range(16)}
  stops=[(384480,'run',0)]
  synth_controls={k:v for k,v in VOICE_DEFAULT.items() if k not in ('freq','gate','velocity','accent','slide')}
  lanes=r('musical-control-lanes',seq,musical,stops,frames=frames,inputs=inputs)
  synth_inputs=np.column_stack((lanes[:,0],lanes[:,1],np.ones(frames),lanes[:,2],lanes[:,3])).astype(np.float32)
  standalone=r('split-signal-voice',signal,synth_controls,frames=frames,inputs=synth_inputs)
  fused=r('fused-patch',patch,musical|synth_controls,stops,frames=frames,inputs=inputs)
  c('separate-versus-fused-patch',abs(standalone-fused).max()<3e-5,max_absolute=float(abs(standalone-fused).max()))
  c('fused-vector',abs(r('fused-vector',pv,musical|synth_controls,stops,frames=frames,inputs=inputs)-fused).max()<3e-5)
  wavfile.write(audition/'01_acid_bassline.wav',48000,fused[:,0])
  versions=[fused[:,0]]
  for label,changes in [('no_accent',{f'accent{i}':0 for i in range(32)}),('no_slide',{f'slide{i}':0 for i in range(32)})]:
   y=r(label,patch,musical|synth_controls|changes,stops,frames=frames,inputs=inputs)[:,0]
   c(label+':audible-difference',np.linalg.norm(y-fused[:,0])>.1);wavfile.write(audition/(label+'.wav'),48000,y);versions.append(y)
  wavfile.write(audition/'02_accent_slide_comparison.wav',48000,np.concatenate(versions))
  bank=[]
  for label,changes in PRESETS.items():
   y=r('preset-'+label,patch,musical|synth_controls|changes,stops,frames=frames,inputs=inputs)[:,0]
   wavfile.write(audition/(label+'.wav'),48000,y);bank.append(y)
  wavfile.write(audition/'03_four_acid_presets.wav',48000,np.concatenate(bank))
  simple=r('second-consumer',second,musical,stops,frames=frames,inputs=inputs)[:,0]
  wavfile.write(audition/'04_second_voice_proof.wav',48000,simple)
  L.report['audition']={'sample_rate':48000,'gain':1.,'normalization':False,'fx':False,'description':'Actual fused Faust sequence->single-note voice; 120 BPM; four bars plus tail. Comparison: full, no accent, no slide. Presets Round/Acid/Hollow/Driven.'}
  L.report['source_sha256']={str(p.relative_to(ROOT)):digest(p) for folder in (VOICE,SEQ,PATCH) for p in folder.rglob('*') if p.is_file()}
  L.report['renderer_sha256']=digest(ROOT/'tools/modules/render.cpp');L.report['driver_sha256']=digest(__file__);L.report['reference_cpp_sha256']=digest(ROOT/'tools/modules/acid_filter_reference.cpp')
  L.report['standard_library_sha256']={p.name:digest(p) for p in Path('/usr/share/faust').glob('*.lib')}
  L.report['passed']=all(x['passed'] for x in L.report['checks'])
 except Exception as e:
  L.report.update(passed=False,error=repr(e),compiler_output=getattr(e,'output',None));traceback.print_exc()
 finally:
  (L.out/'results.json').write_text(json.dumps(L.report,indent=2)+'\n')
  print(json.dumps({'passed':L.report.get('passed',False),'checks':len(L.report['checks']),'renders':len(L.report['renders']),'error':L.report.get('error'),'compiler_output':L.report.get('compiler_output')}),flush=True)
 if not L.report.get('passed'):raise SystemExit(1)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
