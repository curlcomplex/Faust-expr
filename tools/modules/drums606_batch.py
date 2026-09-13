"""Seven 606-inspired single-note voices; existing real Faust/C++ renderer.
Hardware fidelity is explicitly NOT certified by these tests.
"""
from pathlib import Path
import argparse, itertools, json, os, platform, time
import numpy as np
from scipy.signal import welch
from synth_batch import SynthLab
from acid_batch import controls
from hats_v2_delivery import command, digest
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'modules/drums-606/v1'
DEFAULTS=json.loads((SRC/'defaults.json').read_text())
PRESETS={
 'kick':{'Classic':{},'Tight':dict(decay=.13,tone=.68,click=.28),'Low':dict(freq=39,decay=.65,tone=.12),'Knock':dict(freq=76,tone=.9,click=.55)},
 'snare':{'Classic':{},'Tight':dict(decay=.08,snappy=.85),'Body':dict(freq=160,snappy=.15,tone=.2),'Sizzle':dict(decay=.4,tone=.85,snappy=.9)},
 'low-tom':{'Classic':{},'Dry':dict(noise=0,decay=.22),'Sub':dict(freq=55,noise=.08,decay=.65),'Noisy':dict(noise=.65,tone=.65,decay=.48)},
 'high-tom':{'Classic':{},'Dry':dict(noise=0,decay=.16),'Tuned':dict(freq=245,noise=.1),'Noisy':dict(noise=.6,tone=.72,decay=.38)},
 'closed-hat':{'Classic':{},'Tight':dict(decay=.035,tone=.65),'Dark':dict(tone=.12,freq=340),'Spread':dict(metalSpread=.95,decay=.15)},
 'open-hat':{'Classic':{},'Tight':dict(decay=.22),'Dark':dict(tone=.15,freq=340),'Long':dict(decay=1.1,metalSpread=.7)},
 'cymbal':{'Classic':{},'Short':dict(decay=.45),'Body':dict(tone=.05,freq=350),'Shimmer':dict(tone=.95,metalSpread=.8,decay=2.1)},
}

def describe(x,sr=48000):
 x=np.asarray(x,dtype=np.float64); f,p=welch(x,sr,nperseg=min(2048,len(x)))
 energy=np.cumsum(x*x);e=energy[-1]
 return dict(peak=float(abs(x).max()),rms=float(np.sqrt(np.mean(x*x))),mean=float(x.mean()),centroid_hz=float(np.dot(f,p)/(p.sum()+1e-30)),t90_ms=float(np.searchsorted(energy,.9*e)*1000/sr),energy=e)


def run(out):
 L=SynthLab(out);c=L.check;r=L.render;L.out.joinpath('audition').mkdir(exist_ok=True)
 L.report.update(version='606-drums-0.1.0-experiment',commit=os.environ.get('GITHUB_SHA','local-snapshot'),defaults=DEFAULTS,presets=PRESETS,human_approved=False,hardware_approved=False,host_integrated=False,device_qualified=False,reference_comparison='DEFERRED by owner; source-informed hypotheses only')
 L.report['environment']=dict(platform=platform.platform(),machine=platform.machine(),faust=command([os.getenv('FAUST','faust'),'--version']),cxx=command([os.getenv('CXX','c++'),'--version']))
 start=time.perf_counter()
 try:
  exes={};baseline={};uis={};audition_defaults={}
  for name,p in DEFAULTS.items():
   print('QUALIFY',name,flush=True)
   src=SRC/(name+'.dsp');exe=L.build(name,src);exes[name]=exe
   vec=L.build(name+'-vector',src,True)
   io,ui=controls(exe);uis[name]=ui
   c(name+':single-note-io',io==(0,1));c(name+':exact-control-set',set(ui)==set(p))
   c(name+':defaults',all(abs(ui[k][2]-v)<1e-5 for k,v in p.items()))
   c(name+':four-faceplate-controls',len(set(p)-{'gate','freq','velocity','accent','chokeGate'})==4)
   events=[(480,'gate',1),(481,'gate',0)]
   x=r(name+'-default',exe,p,events,frames=144000)[:,0];baseline[name]=x
   c(name+':audible',.005<float(abs(x).max())<1,peak=float(abs(x).max()))
   c(name+':pretrigger-silence',not np.any(x[:480]))
   c(name+':never-triggered-silent',not np.any(r(name+'-never',exe,p,frames=24000)))
   c(name+':zero-velocity',not np.any(r(name+'-zero',exe,p|{'velocity':0},events,frames=24000)))
   half=r(name+'-half',exe,p|{'velocity':.5},events,frames=48000)[:,0]
   c(name+':linear-velocity',abs(half-.5*x[:48000]).max()<2e-6)
   held=r(name+'-held',exe,p,[(480,'gate',1)],frames=48000)[:,0]
   c(name+':held-gate-is-one-hit',np.array_equal(held,x[:48000]))
   latched=events+[(5000,k,ui[k][1]) for k in ('freq','velocity','accent','decay')]
   c(name+':onset-contract',np.array_equal(r(name+'-latched',exe,p,latched,frames=48000)[:,0],x[:48000]))
   ac=r(name+'-accent',exe,p|{'accent':1},events,frames=48000)[:,0]
   c(name+':accent-increases-energy',np.sum(ac.astype(float)**2)>np.sum(x[:48000].astype(float)**2)*1.1)
   for block in (1,64,127,512):
    y=r(name+'-block-'+str(block),exe,p,events,block=block,frames=24000)[:,0]
    c(name+':block-'+str(block),np.array_equal(y,x[:24000]))
   y=r(name+'-vector',vec,p,events,frames=48000)[:,0]
   c(name+':vector-parity',abs(y-x[:48000]).max()<3e-5,max_absolute=float(abs(y-x[:48000]).max()))
   for sr in (44100,96000):
    n=round(.01*sr);r(name+'-rate-'+str(sr),exe,p,[(n,'gate',1),(n+1,'gate',0)],sr=sr,frames=sr*2)
   # One persistent instance, not 96 fresh offline sample copies.
   rapid=[(480+i*311+j,'gate',1-j) for i in range(96) for j in (0,1)]
   y=r(name+'-rapid',exe,p,rapid,frames=72000)
   c(name+':rapid-bounded',abs(y).max()<1.6)
   long=r(name+'-tail',exe,p|{'decay':ui['decay'][1]},events,frames=480000)
   c(name+':tail-terminates',abs(long[-48000:]).max()<1e-7,tail_peak=float(abs(long[-48000:]).max()))
   colour=[k for k in ('click','snappy','noise','metalSpread') if k in p][0]
   for key in ('freq','decay','tone',colour):
    a=r(name+'-'+key+'-min',exe,p|{key:ui[key][0]},events,frames=48000)
    b=r(name+'-'+key+'-max',exe,p|{key:ui[key][1]},events,frames=48000)
    effect=float(np.linalg.norm(a-b)/(np.linalg.norm(a)+1e-20))
    c(name+':effective-'+key,effect>.003,relative_l2=effect)
   # Verify live controls on a ringing hit rather than only at the next trigger.
   lp=p|{'decay':ui['decay'][1]}
   ref=r(name+'-live-reference',exe,lp,events,frames=48000)
   for key in ('tone',colour):
    value=0 if p[key]>=.5 else 1
    y=r(name+'-live-'+key,exe,lp,events+[(6000,key,value)],frames=48000)
    delta=float(np.linalg.norm(y[6500:]-ref[6500:])/(np.linalg.norm(ref[6500:])+1e-20))
    # Click is an onset-only transient; a later click control must not resurrect it.
    if key=='click':c(name+':late-click-no-resurrection',delta<.001,relative_l2=delta)
    else:c(name+':live-'+key,delta>.002,relative_l2=delta)
   edge=[]
   for i,bits in enumerate(itertools.product((0,1),repeat=4)):
    n=480+i*2500
    edge += [(n,k,ui[k][bit]) for k,bit in zip(('freq','decay','tone',colour),bits)]
    edge += [(n,'accent',i%2),(n,'gate',1),(n+1,'gate',0)]
   y=r(name+'-corners',exe,p,edge,frames=96000)
   c(name+':corners-bounded',abs(y).max()<1.6,peak=float(abs(y).max()))
   bank=[]
   for title,preset in PRESETS[name].items():
    # Two hits per 3-second slot; second is softer, unaccented.
    pe=[(4800,'gate',1),(4801,'gate',0),(67200,'velocity',.65),(67200,'gate',1),(67201,'gate',0)]
    y=r(name+'-preset-'+title,exe,p|preset,pe,frames=144000)[:,0]
    L.wav(name+'_'+title+'.wav',y);bank.append(y)
    if title=='Classic':audition_defaults[name]=y
   L.wav(name+'_four_presets.wav',np.concatenate(bank))
  # Independent equation check for the new damped-resonator primitive.
  test=L.out/'ring-test.dsp';test.write_text(f'u=library("{SRC / "drums606.lib"}");process=u.ring(110,.32);\n')
  ring=L.build('ring-equation',test)
  for sr in (44100,48000,96000):
   frames=8192;stim=np.zeros((frames,1),np.float32);stim[0]=1
   y=r('ring-equation-'+str(sr),ring,{},sr=sr,frames=frames,inputs=stim)[:,0]
   n=np.arange(frames);expected=-np.exp(-n*6.907755278982137/(.32*sr))*np.sin(2*np.pi*110*n/sr)
   err=float(abs(y-expected).max());c('ring-equation-'+str(sr),err<2e-4,max_absolute=err)
  # Choke is an explicit external event on an individual OH instance.
  p=DEFAULTS['open-hat']|{'decay':1.4};exe=exes['open-hat']
  onset=[(480,'gate',1),(481,'gate',0)];kill=[(7200,'chokeGate',1),(7201,'chokeGate',0)]
  reference=r('oh-unchoked',exe,p,onset,frames=48000)
  killed=r('oh-choked',exe,p,onset+kill,frames=48000)
  c('choke-no-early-change',np.array_equal(reference[:7200],killed[:7200]))
  ratio=float(np.linalg.norm(killed[9600:])/(np.linalg.norm(reference[9600:])+1e-20))
  c('oh-choke-suppresses-tail',ratio<.01,residual_ratio=ratio)
  repeated=r('oh-repeat-choke',exe,p,onset+kill+[(12000,'chokeGate',1),(12001,'chokeGate',0)],frames=48000)
  c('oh-repeat-choke-no-resurrection',np.array_equal(repeated,killed))
  collision=r('oh-collision',exe,p,onset+[(480,'chokeGate',1),(481,'chokeGate',0)],frames=48000)
  c('oh-simultaneous-choke-wins',not np.any(collision))
  restart=r('oh-restart',exe,p,onset+kill+[(14400,'gate',1),(14401,'gate',0)],frames=48000)
  c('oh-new-note-rearms',abs(restart[14500:20000]).max()>.005)
  L.wav('08_open_hat_choke_comparison.wav',np.concatenate([reference[:,0],killed[:,0],restart[:,0]]))
  # Negative control: source mutation delays the real gate edge by one sample.
  original=(SRC/'kick.dsp').read_text();anchor="h=gate>gate';"
  c('mutation-anchor',original.count(anchor)==1)
  mutant=L.out/'late-kick.dsp';mutant.write_text(original.replace('library("drums606.lib")',f'library("{SRC / "drums606.lib"}")').replace(anchor,"h=(gate>gate')';"))
  traces=[]
  for label,path in [('on-time',SRC/'kick.dsp'),('late',mutant)]:
   trace=L.out/(label+'-trace.dsp');trace.write_text(f'm=library("{path}");process=float(m.h);\n')
   tx=L.build(label+'-trace',trace);z=r(label+'-trace',tx,{'gate':0},[(480,'gate',1),(481,'gate',0)],frames=1024)[:,0]
   traces.append(z)
  expected=np.zeros(1024,np.float32);expected[480]=1
  c('gate-trace-sample-exact',np.array_equal(traces[0],expected))
  c('trace-rejects-delayed-source',not np.array_equal(traces[1],expected))
  # Existing Faust Trigger Seq -> actual persistent voices. No Python synthesizer.
  seq=L.build('existing-trigger-seq',ROOT/'modules/trigger-seq/v1/trigger.dsp')
  seq_defaults=dict(run=1,length=16,clock=0,reset=0)|{f'step{i+1:02d}':0 for i in range(32)}
  patterns={'kick':[0,6,8,14],'snare':[4,12],'low-tom':[7,15],'high-tom':[3,10,14],'closed-hat':[0,2,4,6,8,10,12,14,15],'open-hat':[3,11],'cymbal':[0]}
  clocks=[(4800+i*6000+j,'clock',1-j) for i in range(64) for j in (0,1)]
  gates={};stems={}
  for name,pattern in patterns.items():
   sd=seq_defaults|{f'step{i+1:02d}':1 for i in pattern}
   if name=='cymbal':sd['length']=32
   z=r(name+'-sequence',seq,sd,clocks,frames=576000)
   gates[name]=np.flatnonzero(z[:,0]>.5).tolist()
   expected_gates=[4800+i*6000 for i in range(64) if i%int(sd['length']) in pattern]
   c(name+':sequencer-onsets',gates[name]==expected_gates)
  for name,p in DEFAULTS.items():
   ev=[]
   for n in gates[name]:
    i=(n-4800)//6000
    ev += [(n,'gate',1),(n+1,'gate',0),(n,'velocity',.72 if i%4 else 1),(n,'accent',float(i%4==0))]
   if name=='open-hat':ev += [(n+j,'chokeGate',1-j) for n in gates['closed-hat'] for j in (0,1)]
   y=r(name+'-persistent-groove',exes[name],p,sorted(ev),frames=576000)[:,0]
   stems[name]=y;L.wav(name+'_stem.wav',y)
  mix=sum(stems.values())*.45
  L.wav('00_606_seven_voice_groove.wav',mix)
  L.wav('01_606_isolated_voices.wav',np.concatenate(list(audition_defaults.values())))
  L.report['audition']=dict(bpm=120,bars=4,seconds=12,groove_fixed_gain=.45,preset_gain=1,processing='Dry actual Faust voices. No EQ, compression, reverb, limiter or normalization. Offline adapter, not CURLOP.',isolation_order=list(DEFAULTS),preset_slot_seconds=3)
  L.report['features']={name:describe(x[480:]) for name,x in baseline.items()}
  L.report['sources']={str(p.relative_to(ROOT)):digest(p) for p in sorted(SRC.iterdir()) if p.is_file()}
  L.report['driver_sha256']=digest(__file__);L.report['renderer_sha256']=digest(ROOT/'tools/modules/render.cpp')
  L.report['passed']=all(t['passed'] for t in L.report['checks'])
  if not L.report['passed']:raise AssertionError([t['name'] for t in L.report['checks'] if not t['passed']])
 except Exception as e:
  L.report.update(passed=False,error=repr(e),compiler_output=getattr(e,'output',None));raise
 finally:
  L.report['suite_wall_seconds']=time.perf_counter()-start
  L.report['all_renders_compute_seconds']=sum(a['diagnostics']['instrumented_compute_ns'] for a in L.report['renders'])/1e9
  (L.out/'results.json').write_text(json.dumps(L.report,indent=2)+'\n')
  print(json.dumps(dict(passed=L.report.get('passed',False),checks=len(L.report['checks']),renders=len(L.report['renders']),builds=len(L.report['builds']),error=L.report.get('error'),compiler_output=L.report.get('compiler_output'))),flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
