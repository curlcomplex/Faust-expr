"""Three actual Faust single-note candidates. Not hardware-fidelity approval.
Reuses the existing offline runner; Python supplies scores/analysis, never synth audio.
"""
from pathlib import Path
import argparse, hashlib, itertools, json, os, platform, subprocess, time, traceback
import numpy as np
from scipy.io import wavfile
from scipy.signal import bilinear, lfilter, resample_poly, welch
from hats_v2_delivery import Lab, command, digest
from acid_batch import Batch, controls
ROOT=Path(__file__).resolve().parents[2]
COMMON=ROOT/'modules/analog-classics/synth-batch'
PATHS={'mono101':ROOT/'modules/mono-101/v1/voice.dsp','juno106':ROOT/'modules/juno-106/v1/voice.dsp','mini':ROOT/'modules/minimoog/v1/voice.dsp'}
DEFAULTS={
 'mono101':dict(gate=0,freq=110,velocity=1,slide=0,glideTime=.06,saw=.75,pulse=.25,sub=.35,noise=0,pwm=.5,pwmDepth=0,cutoff=900,resonance=.30,filterEnv=.5,attack=.005,decay=.18,sustain=.45,release=.22,lfoRate=5.2,lfoPitch=0,lfoFilter=0,level=.65),
 'juno106':dict(gate=0,freq=220,velocity=1,saw=.7,pulse=.35,sub=.45,noise=.02,pwm=.48,pwmDepth=.18,lfoRate=4.8,cutoff=2000,resonance=.24,hpf=35,filterEnv=.35,attack=.015,decay=.32,sustain=.72,release=.55,level=.65),
 'mini':dict(gate=0,freq=110,velocity=1,slide=0,glideTime=.09,osc1=.8,osc2=.65,osc3=.4,detune2=.07,detune3=-12,noise=0,cutoff=800,emphasis=2.2,contour=.52,drive=.22,attack=.008,decay=.28,sustain=.72,release=.3,filterDecay=.2,level=.60),
}
PRESETS={
 'mono101':{'SubBass':dict(cutoff=350,sub=.7,filterEnv=.55,sustain=.15,decay=.23), 'PulseLead':dict(saw=0,pulse=1,sub=.12,pwmDepth=.25,cutoff=1800,sustain=.8,release=.35), 'Pluck':dict(cutoff=500,filterEnv=.9,resonance=.48,sustain=0,decay=.16,release=.1), 'Moving':dict(cutoff=1200,lfoFilter=.6,pwmDepth=.28,lfoRate=2.3,release=.7)},
 'juno106':{'SoftPad':dict(cutoff=1500,attack=.3,release=1.6,resonance=.12,filterEnv=.18,pwmDepth=.28), 'Bass':dict(freq=110,cutoff=380,sub=.8,hpf=20,filterEnv=.6,attack=.005,decay=.2,sustain=.25,release=.12), 'PWMStrings':dict(saw=.35,pulse=1,sub=.15,pwmDepth=.32,cutoff=4200,hpf=90,attack=.12,release=.8), 'Pluck':dict(cutoff=600,filterEnv=.8,resonance=.48,attack=.002,decay=.22,sustain=0,release=.16)},
 'mini':{'Deep':dict(cutoff=350,osc3=.7,drive=.28,contour=.75,sustain=.5), 'Lead':dict(detune3=-.06,osc3=.5,cutoff=1800,contour=.25,release=.4), 'Pluck':dict(cutoff=350,contour=.9,filterDecay=.1,decay=.2,sustain=.1,release=.15), 'Driven':dict(osc1=1,osc2=1,osc3=.8,drive=.85,cutoff=1800,emphasis=4,contour=.5)},
}

class SynthLab(Batch):
 def build(self,name,src,vector=False):
  d=self.out/name;d.mkdir(exist_ok=True)
  args=[os.getenv('FAUST','faust'),'-I',src.parent,'-I',COMMON,'-lang','cpp','-single','-cn','ModuleDSP']
  if vector:args+=['-vec','-lv','0','-vs','32']
  t=time.perf_counter();command(args+[src,'-o',d/'generated.hpp']);ft=time.perf_counter()-t
  t=time.perf_counter();command([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render']);ct=time.perf_counter()-t
  ui=command([d/'render','--controls']);(d/'controls.tsv').write_text(ui)
  self.report['builds'][name]=dict(source=str(src),source_sha256=digest(src),generated_sha256=digest(d/'generated.hpp'),controls=ui,faust_seconds=ft,cxx_seconds=ct)
  return d/'render'
 def wav(self,name,x,sr=48000):
  self.check(name+':audition-headroom',np.isfinite(x).all() and np.max(abs(x))<=1,peak=float(abs(x).max()))
  wavfile.write(self.out/'audition'/name,sr,np.asarray(x,np.float32))

def phrase(sr=48000,seconds=8.,root=110.):
 notes=(0,0,7,12,3,7,0,-5,0,7,10,12)
 ev=[]
 for i,st in enumerate(notes):
  n=round((.1+i*.5)*sr)
  ev.extend([(n,'freq',root*2**(st/12)),(n,'velocity',1 if i%4==0 else .75),(n,'gate',1),(n+round(.32*sr),'gate',0)])
 return ev

def run(out):
 L=SynthLab(out);c=L.check;r=L.render;L.out.joinpath('audition').mkdir(exist_ok=True)
 L.report.update(version='synth-batch-0.1.1-experiment',commit=os.environ.get('GITHUB_SHA','local'),human_approved=False,hardware_approved=False,host_integrated=False,device_qualified=False,presets=PRESETS,defaults=DEFAULTS)
 L.report['environment']=dict(platform=platform.platform(),machine=platform.machine(),faust=command([os.getenv('FAUST','faust'),'--version']),cxx=command([os.getenv('CXX','c++'),'--version']))
 start=time.perf_counter()
 try:
  exes={};vectors={};diags={};signals={};ui={};base_notes={};phrases={}
  for name,src in PATHS.items():
   print('BUILD',name,flush=True)
   exes[name]=L.build(name,src);vectors[name]=L.build(name+'-vector',src,True)
   base=DEFAULTS[name];io,p=controls(exes[name]);ui[name]=p
   c(name+':single-note-io',io==(0,1),io=io)
   c(name+':controls',set(p)==set(base),actual=sorted(p))
   c(name+':defaults',all(abs(p[k][2]-v)<1e-5 for k,v in base.items()))
   c(name+':hz-contract',p['freq'][:3]==[20.,8000.,base['freq']])
   dsrc=L.out/(name+'-diagnostic.dsp');ssrc=L.out/(name+'-signal.dsp')
   args='m.freq,m.gate,m.velocity'+(',m.slide' if name!='juno106' else '')
   dsrc.write_text(f'm=library("{src}");process=m.diagnostics({args});\n')
   ssrc.write_text(f'm=library("{src}");process=m.voice;\n')
   diags[name]=L.build(name+'-diagnostic',dsrc);signals[name]=L.build(name+'-signal',ssrc)
   c(name+':signal-io',controls(signals[name])[0]==((3 if name=='juno106' else 4),1))
   events=[(480,'gate',1),(24480,'gate',0)]
   x=r(name+'-note',exes[name],base,events,frames=144000)[:,0];base_notes[name]=x
   c(name+':note-audible',.005<float(abs(x).max())<1,peak=float(abs(x).max()))
   c(name+':pre-onset-silent',not np.any(x[:480]))
   c(name+':release-settles',float(abs(x[-4800:]).max())<1e-6)
   c(name+':never-triggered',not np.any(r(name+'-never',exes[name],base)))
   z=r(name+'-zero-velocity',exes[name],base|{'velocity':0},events,frames=144000)
   c(name+':zero-velocity-silent',not np.any(z))
   h=r(name+'-half-velocity',exes[name],base|{'velocity':.5},events,frames=144000)[:,0]
   c(name+':velocity-linear',float(abs(h-.5*x).max())<2e-6)
   for block in (1,64,127,512):
    y=r(name+'-block-'+str(block),exes[name],base,events,frames=144000,block=block)[:,0]
    c(name+':block-'+str(block),np.array_equal(y,x),max_error=float(abs(y-x).max()))
   y=r(name+'-vector',vectors[name],base,events,frames=144000)[:,0]
   c(name+':vector-parity',float(abs(y-x).max())<1e-4,max_error=float(abs(y-x).max()))
   for sr in (44100,96000):
    y=r(name+'-rate-'+str(sr),exes[name],base,[(round(.01*sr),'gate',1),(round(.51*sr),'gate',0)],sr=sr,frames=sr*3)
    c(name+':rate-bounded-'+str(sr),float(abs(y).max())<1)
   # Sustained pitch is independent of timbre, and release does not retune.
   dv={k:v for k,v in base.items() if k in controls(diags[name])[1]}
   trace=[(480,'gate',1),(6000,'freq',220),(18000,'freq',110),(30000,'gate',0),(36000,'freq',440)]
   d=r(name+'-note-trace',diags[name],dv,trace)
   c(name+':ordinary-notes-exact',d[6000,0]==220 and d[18000,0]==110)
   c(name+':legato-does-not-retrigger',np.flatnonzero(d[:,2]>.5).tolist()==[480])
   c(name+':release-keeps-pitch',d[36000,0]==110)
   if name!='juno106':
    d=r(name+'-slide-trace',diags[name],dv,trace+[(5000,'slide',1)])
    c(name+':slide-start',110<d[6000,0]<220)
    c(name+':slide-monotone',np.all(np.diff(d[6000:18000,0])>=0))
    z=r(name+'-zero-glide',diags[name],dv|{'glideTime':0,'slide':1},trace)
    c(name+':zero-glide-exact',z[6000,0]==220 and z[18000,0]==110)
   # Live cutoff movement must affect an already-releasing voice.
   p=base|{'release':1.5,'sustain':.85}
   a=r(name+'-tail-baseline',exes[name],p,[(480,'gate',1),(24000,'gate',0)],frames=96000)
   b=r(name+'-tail-cutoff',exes[name],p,[(480,'gate',1),(24000,'gate',0),(30000,'cutoff',80)],frames=96000)
   change=float(np.linalg.norm(a[32000:60000]-b[32000:60000])/(np.linalg.norm(a[32000:60000])+1e-20))
   c(name+':live-release-cutoff',change>.05,relative_change=change)
   # Each continuous timbre control is exercised under a held note.
   live=[(480,'gate',1),(132000,'gate',0)]
   for i,k in enumerate(k for k in base if k not in ('gate','freq','velocity','slide','glideTime')):
    lo,hi=ui[name][k][:2];live.extend([(6000+i*2400,k,lo),(72000+i*2400,k,hi)])
   y=r(name+'-live-controls',exes[name],base,sorted(live),frames=144000)
   c(name+':live-bounded',float(abs(y).max())<4,peak=float(abs(y).max()))
   rapid=[]
   for i in range(64):
    n=480+i*640;rapid.extend([(n,'freq',55*2**((i%24)/12)),(n,'gate',1),(n+480,'gate',0)])
   y=r(name+'-rapid',exes[name],base,rapid)
   c(name+':rapid-bounded',float(abs(y).max())<1)
   # Deliberately low/high controls and high register: finite does not mean alias-free.
   reskey='emphasis' if name=='mini' else 'resonance'
   for i,(f,cut,reson) in enumerate(itertools.product((20,8000),(40,16000),(ui[name][reskey][0],ui[name][reskey][1]))):
    y=r(name+'-corner-'+str(i),exes[name],base|{'freq':f,'cutoff':cut,reskey:reson,'sustain':1,'noise':1},[(480,'gate',1)],frames=24000)
    c(name+':corner-bounded-'+str(i),float(abs(y).max())<4)
   # Independent FFT check with only a fundamental oscillator, not diagnostic telemetry.
   tune=base|{'freq':220,'noise':0,'attack':.001,'sustain':1,'cutoff':16000}
   if name=='mini':tune.update(osc1=1,osc2=0,osc3=0,contour=0,drive=0,emphasis=.707)
   else:tune.update(saw=1,pulse=0,sub=0,filterEnv=0,resonance=0,pwmDepth=0)
   tx=r(name+'-tuning',exes[name],tune,[(0,'gate',1)],frames=96000)[:,0]
   win=tx[48000:96000]*np.hanning(48000);spectrum=abs(np.fft.rfft(win));peak=int(np.argmax(spectrum[150:301]))+150
   c(name+':fundamental-fft',abs(peak-220)<=1,peak_hz=peak)
   # A raw high-drive/high-register comparison is a diagnostic, not a fidelity pass.
   hp=tune|{'freq':3000}
   if name=='mini':hp['drive']=1
   a=r(name+'-high-register48',exes[name],hp,[(0,'gate',1)],frames=48000)[:,0]
   b=r(name+'-high-register96',exes[name],hp,[(0,'gate',1)],sr=96000,frames=96000)[:,0]
   down=resample_poly(b,1,2)
   # Sample-rate phase/filters differ: this residual is NOT a pure alias metric.
   L.report.setdefault('high_register_diagnostics',{})[name]=dict(relative_48_vs_down96=float(np.linalg.norm(a[24000:]-down[24000:])/(np.linalg.norm(down[24000:])+1e-20)),note='Phase/envelope/filter and alias differences are confounded; no alias-free claim.')
   bank=[]
   for preset,overrides in PRESETS[name].items():
    y=r(name+'-'+preset,exes[name],base|overrides,phrase(root=(base|overrides)['freq']),frames=384000)
    L.wav(name+'_'+preset+'.wav',y);bank.append(y)
   L.wav(name+'_four_presets.wav',np.concatenate(bank))
   y=r(name+'-common-phrase',exes[name],base,phrase(root=110),frames=384000);phrases[name]=y;L.wav(name+'_common_phrase.wav',y)
  L.wav('00_three_voices_same_phrase.wav',np.concatenate(list(phrases.values())))
  # Four INDEPENDENT Faust instances: test-adapter polyphony, not in a voice kernel.
  chord=[]
  for index,st in enumerate((0,3,7,10)):
   ev=[]
   for n,root in ((480,130.81278265),(144480,155.56349186)):
    ev.extend([(n,'freq',root*2**(st/12)),(n,'gate',1),(n+96000,'gate',0)])
   y=r('juno-chord-note-'+str(index),exes['juno106'],DEFAULTS['juno106']|PRESETS['juno106']['SoftPad'],ev,frames=384000)
   chord.append(y)
  dry=np.sum(chord,axis=0)*.25
  L.wav('01_juno_external_four_note_chords_dry.wav',dry)
  L.report['chord_adapter']=dict(instances=4,gain=.25,host='offline test adapter only, not CURLOP',effects='none')
  # Actual Bassline Seq to each mono voice; timing/pitch comes from Faust lanes.
  from acid_batch import SEQ_DEFAULT
  seq=L.build('bassline-seq',ROOT/'modules/bassline-seq/v1/signal.dsp')
  inp=np.zeros((384000,2),np.float32)
  for n in range(480,288480,6000):inp[n:n+3000,0]=1
  p=SEQ_DEFAULT|{f'on{i}':float(i not in (3,7,14)) for i in range(16)}|{f'note{i}':n for i,n in enumerate((33,33,45,33,36,33,40,33,33,45,43,40,36,33,33,45))}|{'slide0':1,'slide4':1,'slide8':1,'accent2':1,'accent5':1}
  lanes=r('bassline-lanes',seq,p,frames=len(inp),inputs=inp)
  for name in ('mono101','mini'):
   inputs=np.column_stack((lanes[:,0],lanes[:,1],.7+.3*lanes[:,2],lanes[:,3])).astype(np.float32)
   vals={k:v for k,v in DEFAULTS[name].items() if k in controls(signals[name])[1]}
   y=r(name+'-sequenced',signals[name],vals,frames=len(inputs),inputs=inputs);L.wav(name+'_bassline_seq.wav',y)
  # Source negative control: delaying the real gate breaks the onset trace gate.
  src=PATHS['mono101'];bad=L.out/'late-gate.dsp'
  bad.write_text(f'm=library("{src}");process=m.diagnostics(m.freq,m.gate\',m.velocity,m.slide);\n')
  exe=L.build('late-gate',bad);vals={k:v for k,v in DEFAULTS['mono101'].items() if k in controls(exe)[1]}
  x=r('late-gate',exe,vals,[(480,'gate',1),(1000,'gate',0)])
  c('onset-test-rejects-delayed-gate',np.flatnonzero(x[:,2]>.5).tolist()!=[480])
  # Independent bilinear-transform oracle for the NEW ideal four-pole component.
  probe=L.out/'ota-probe.dsp';probe.write_text('cs=library("common.lib");process=cs.ota4(hslider("cutoff",1000,15,16000,1),hslider("resonance",0,0,1,.001),.2);\n')
  filt=L.build('ota-probe',probe);errs=[]
  impulse=np.zeros((8192,1),np.float32);impulse[32]=.1
  for sr,fc,res in itertools.product((44100,48000,96000),(300,2400),(.0,.7)):
   y=r(f'ota-{sr}-{fc}-{res}',filt,dict(cutoff=fc,resonance=res),sr=sr,frames=len(impulse),inputs=impulse)[:,0]
   w=2*sr*np.tan(np.pi*fc/sr);k=3.85*res
   b,a=bilinear([(1+.2*k)*w**4],[1,4*w,6*w*w,4*w**3,(1+k)*w**4],fs=sr)
   expected=lfilter(b,a,impulse[:,0].astype(float));err=float(abs(y-expected).max());errs.append(err)
   c(f'ota-oracle-{sr}-{fc}-{res}',err<2e-5,max_error=err)
  L.report['ideal_filter_oracle']=dict(max_absolute_error=max(errs),reference='Independent double-precision scipy.signal.bilinear of (s+w)^4+k*w^4, not hardware')
  L.report['source_sha256']={str(p.relative_to(ROOT)):digest(p) for p in [*PATHS.values(),COMMON/'common.lib',Path(__file__)]}
  # Snapshot every installed .lib hash: excludes assumptions about transitive versions.
  libroot=Path('/usr/share/faust')
  L.report['installed_faust_libraries']={str(p.relative_to(libroot)):digest(p) for p in sorted(libroot.rglob('*.lib'))} if libroot.exists() else {}
  L.report['passed']=all(item['passed'] for item in L.report['checks'])
 except Exception as exc:
  traceback.print_exc();L.report.update(passed=False,error=repr(exc))
 finally:
  L.report['wall_seconds']=time.perf_counter()-start
  L.report['aggregate_instrumented_compute_seconds']=sum(x['diagnostics']['instrumented_compute_ns'] for x in L.report['renders'])/1e9
  (L.out/'results.json').write_text(json.dumps(L.report,indent=2)+'\n')
 print(json.dumps(dict(passed=L.report.get('passed',False),checks=len(L.report['checks']),renders=len(L.report['renders']),error=L.report.get('error'))),flush=True)
 if not L.report.get('passed'):raise SystemExit(1)

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
