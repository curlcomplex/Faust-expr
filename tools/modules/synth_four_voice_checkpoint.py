"""Four-synth Analog Classics checkpoint: SH-101, Juno-60, Juno-106, Mini.
Behavioral qualification only; hardware-reference selection is a separate gate.
"""
from pathlib import Path
import argparse, json, os
import numpy as np
from scipy.io import wavfile
from synth_batch import SynthLab, DEFAULTS, phrase
from acid_batch import controls
from hats_v2_delivery import digest

ROOT=Path(__file__).resolve().parents[2]
PATHS={
 'mono101':ROOT/'modules/mono-101/v1/voice.dsp',
 'juno60':ROOT/'modules/juno-60/v1/voice.dsp',
 'juno106':ROOT/'modules/juno-106/v1/voice.dsp',
 'mini':ROOT/'modules/minimoog/v1/voice.dsp',
}
DEFAULT=dict(gate=0,freq=220,velocity=1,saw=.72,pulse=.32,sub=.48,noise=.015,pwm=.50,pwmDepth=.14,lfoRate=4.5,cutoff=1850,resonance=.22,hpf=30,filterEnv=.38,attack=.006,decay=.26,sustain=.70,release=.42,level=.65)
ALL={**DEFAULTS,'juno60':DEFAULT}

def wav(path,x,sr=48000):
 x=np.asarray(x,np.float32)
 if not np.isfinite(x).all() or np.max(np.abs(x))>1: raise RuntimeError(f'bad audition {path}')
 wavfile.write(path,sr,x)

def run(out):
 L=SynthLab(out); c=L.check; r=L.render; (L.out/'audition').mkdir(exist_ok=True)
 L.report.update(version='four-synth-checkpoint-0.1',commit=os.environ.get('GITHUB_SHA','local'),hardware_approved=False,human_approved=False,host_integrated=False,voices=list(PATHS))
 exes={}; phrases={}
 try:
  for name,src in PATHS.items():
   exe=L.build(name,src); vec=L.build(name+'-vec',src,True); exes[name]=exe
   io,p=controls(exe); base=ALL[name]
   c(name+':io',io==(0,1),io=io)
   c(name+':canonical-inputs',all(k in p for k in ('gate','freq','velocity')))
   c(name+':freq-hz-range',p['freq'][0]==20 and p['freq'][1]==8000)
   c(name+':controls-match-default-map',set(p)==set(base),actual=sorted(p),expected=sorted(base))
   ev=[(480,'gate',1),(24480,'gate',0)]
   x=r(name+'-note',exe,base,ev,frames=144000)[:,0]
   c(name+':audible-finite',np.isfinite(x).all() and .001<np.max(abs(x))<1,peak=float(np.max(abs(x))))
   c(name+':pre-onset-silent',not np.any(x[:480]))
   c(name+':release-settles',float(np.max(abs(x[-4800:])))<1e-6)
   c(name+':never-triggered',not np.any(r(name+'-never',exe,base,frames=48000)))
   z=r(name+'-zero-velocity',exe,base|{'velocity':0},ev,frames=96000)
   c(name+':zero-velocity',not np.any(z))
   for block in (1,127,512):
    y=r(name+f'-block-{block}',exe,base,ev,frames=144000,block=block)[:,0]
    c(name+f':block-{block}',np.array_equal(x,y),max_error=float(np.max(abs(x-y))))
   y=r(name+'-vec-note',vec,base,ev,frames=144000)[:,0]
   c(name+':vector',float(np.max(abs(x-y)))<1e-4,max_error=float(np.max(abs(x-y))))
   for sr in (44100,96000):
    y=r(name+f'-rate-{sr}',exe,base,[(round(.01*sr),'gate',1),(round(.51*sr),'gate',0)],sr=sr,frames=sr*2)
    c(name+f':rate-{sr}',np.isfinite(y).all() and np.max(abs(y))<1,peak=float(np.max(abs(y))))
   ph=r(name+'-phrase',exe,base,phrase(root=110 if name!='juno60' and name!='juno106' else 220),frames=384000)
   phrases[name]=ph; wav(L.out/'audition'/f'{name}_phrase.wav',ph)
  # Same dry phrase for the related Junos; no chorus or loudness fitting.
  wav(L.out/'audition'/'juno60_then_juno106.wav',np.concatenate([phrases['juno60'],phrases['juno106']]))
  wav(L.out/'audition'/'four_synths_same_family_phrase.wav',np.concatenate([phrases[k] for k in ('mono101','juno60','juno106','mini')]))
  # Explicitly assert no internal polyphony/chord controls were introduced.
  forbidden={'voices','polyphony','chord','unison','interval','inversion'}
  for name,exe in exes.items():
   _,p=controls(exe); c(name+':no-internal-polyphony',not(set(p)&forbidden),found=sorted(set(p)&forbidden))
  L.report['sources']={k:{'path':str(v.relative_to(ROOT)),'sha256':digest(v)} for k,v in PATHS.items()}
  failures=[q['name'] for q in L.report.get('checks',[]) if q.get('passed') is not True]
  L.report['passed']=not failures; L.report['failures']=failures
  (L.out/'results.json').write_text(json.dumps(L.report,indent=2))
  return 1 if failures else 0
 finally:
  if not (L.out/'results.json').exists():
   L.report['passed']=False; L.report['failures']=['exception-before-finalize']; (L.out/'results.json').write_text(json.dumps(L.report,indent=2))

if __name__=='__main__':
 ap=argparse.ArgumentParser(); ap.add_argument('--out',type=Path,required=True); a=ap.parse_args(); a.out.mkdir(parents=True,exist_ok=True); raise SystemExit(run(a.out))
