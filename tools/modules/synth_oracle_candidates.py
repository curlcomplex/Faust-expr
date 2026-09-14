"""Compare preserved synth baselines with oracle-informed candidates.
Not a hardware null test. Every sound is compiled Faust; controls stay unchanged.
"""
from pathlib import Path
import argparse,json,os
import numpy as np
from scipy.io import wavfile
from synth_batch import SynthLab,DEFAULTS,phrase
from synth_four_voice_checkpoint import DEFAULT as J60_DEFAULT
from acid_batch import controls
from hats_v2_delivery import digest
ROOT=Path(__file__).resolve().parents[2]
PAIRS={
 'mono101':(ROOT/'modules/mono-101/v1/voice.dsp',ROOT/'modules/mono-101/v3/voice.dsp'),
 'juno60':(ROOT/'modules/juno-60/v1/voice.dsp',ROOT/'modules/juno-60/v2/voice.dsp'),
 'juno106':(ROOT/'modules/juno-106/v1/voice.dsp',ROOT/'modules/juno-106/v2/voice.dsp'),
 'mini':(ROOT/'modules/minimoog/v1/voice.dsp',ROOT/'modules/minimoog/v2/voice.dsp'),
}
BASE={**DEFAULTS,'juno60':J60_DEFAULT}

def rel(a,b): return float(np.linalg.norm(a-b)/(np.linalg.norm(a)+1e-20))
def writewav(p,x):
 x=np.asarray(x,np.float32)
 if not np.isfinite(x).all() or np.max(np.abs(x))>1: raise RuntimeError(f'bad audition {p}')
 wavfile.write(p,48000,x)

def run(out):
 L=SynthLab(out); c=L.check;r=L.render;(L.out/'audition').mkdir(exist_ok=True)
 L.report.update(version='oracle-candidates-0.1',commit=os.environ.get('GITHUB_SHA','local'),hardware_approved=False,human_approved=False,selected_for_promotion=False)
 comparisons={}
 for name,(oldsrc,newsrc) in PAIRS.items():
  old=L.build(name+'-old',oldsrc); new=L.build(name+'-candidate',newsrc); vec=L.build(name+'-candidate-vec',newsrc,True)
  oi,op=controls(old); ni,np_=controls(new); base=BASE[name]
  c(name+':io-preserved',oi==ni==(0,1)); c(name+':controls-preserved',set(op)==set(np_)==set(base)); c(name+':defaults-preserved',all(abs(op[k][2]-np_[k][2])<1e-9 for k in op))
  ev=[(480,'gate',1),(24480,'gate',0)]; a=r(name+'-old-note',old,base,ev,frames=144000)[:,0];b=r(name+'-candidate-note',new,base,ev,frames=144000)[:,0]
  c(name+':candidate-finite',np.isfinite(b).all() and .0001<np.max(abs(b))<1,peak=float(np.max(abs(b))))
  c(name+':candidate-changes',rel(a,b)>.001,relative_difference=rel(a,b))
  c(name+':candidate-release',float(np.max(abs(b[-4800:])))<1e-6)
  for block in (1,127,512):
   y=r(name+f'-candidate-block-{block}',new,base,ev,frames=144000,block=block)[:,0]; c(name+f':block-{block}',np.array_equal(y,b),max_error=float(np.max(abs(y-b))))
  y=r(name+'-candidate-vector',vec,base,ev,frames=144000)[:,0]; c(name+':vector',float(np.max(abs(y-b)))<1e-4,max_error=float(np.max(abs(y-b))))
  for sr in (44100,96000):
   y=r(name+f'-candidate-rate-{sr}',new,base,[(round(.01*sr),'gate',1),(round(.51*sr),'gate',0)],sr=sr,frames=sr*2); c(name+f':rate-{sr}',np.isfinite(y).all() and np.max(abs(y))<2,peak=float(np.max(abs(y))))
  root=220 if name.startswith('juno') else 110
  po=r(name+'-old-phrase',old,base,phrase(root=root),frames=384000);pn=r(name+'-candidate-phrase',new,base,phrase(root=root),frames=384000)
  writewav(L.out/'audition'/f'{name}_old_then_candidate.wav',np.concatenate([po,pn]));writewav(L.out/'audition'/f'{name}_candidate.wav',pn)
  comparisons[name]={'old_source':str(oldsrc.relative_to(ROOT)),'old_sha256':digest(oldsrc),'candidate_source':str(newsrc.relative_to(ROOT)),'candidate_sha256':digest(newsrc),'note_relative_difference':rel(a,b),'old_peak':float(np.max(abs(a))),'candidate_peak':float(np.max(abs(b))),'meaning':'DSP difference only; not hardware fidelity score.'}
 L.report['comparisons']=comparisons
 # Candidate family audition: same phrase order, no loudness fitting or effects.
 bank=[]
 for name,(_,newsrc) in PAIRS.items():
  exe=L.build(name+'-candidate-family',newsrc);root=220 if name.startswith('juno') else 110;bank.append(r(name+'-candidate-family-phrase',exe,BASE[name],phrase(root=root),frames=384000))
 writewav(L.out/'audition'/'four_candidates.wav',np.concatenate(bank))
 failures=[x['name'] for x in L.report.get('checks',[]) if x.get('passed') is not True];L.report['failures']=failures;L.report['passed']=not failures
 (L.out/'results.json').write_text(json.dumps(L.report,indent=2));return 1 if failures else 0
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True);raise SystemExit(run(a.out))
