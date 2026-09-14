"""Reference/tuning checkpoint for Analog Classics synths.

This first executable checkpoint changes only the SH-101-inspired candidate where
circuit/reference evidence identifies a concrete architectural error: v1 applies
Juno-style resonance/Q output compensation to the SH-101 filter path. Juno and
Mini are rendered unchanged and their oracle gaps remain explicit rather than
being guessed at.
"""
from pathlib import Path
import argparse, json, os, platform, time
import numpy as np
from scipy.io import wavfile
from hats_v2_delivery import command, digest
from synth_batch import SynthLab, DEFAULTS, phrase
from acid_batch import controls

ROOT=Path(__file__).resolve().parents[2]
COMMON=ROOT/'modules/analog-classics/synth-batch'
V1={
 'mono101':ROOT/'modules/mono-101/v1/voice.dsp',
 'juno106':ROOT/'modules/juno-106/v1/voice.dsp',
 'mini':ROOT/'modules/minimoog/v1/voice.dsp',
}
V2=ROOT/'modules/mono-101/v2/voice.dsp'

def rel(a,b):
 return float(np.linalg.norm(a-b)/(np.linalg.norm(a)+1e-20))

def writewav(path,x,sr=48000):
 x=np.asarray(x,np.float32)
 if not np.isfinite(x).all(): raise RuntimeError(f'non-finite audition {path}')
 if np.max(np.abs(x))>1: raise RuntimeError(f'clipping audition {path}: {np.max(np.abs(x))}')
 wavfile.write(path,sr,x)

def run(out):
 L=SynthLab(out); c=L.check; r=L.render
 (L.out/'audition').mkdir(exist_ok=True)
 L.report.update(version='synth-reference-pass-0.1',commit=os.environ.get('GITHUB_SHA','local'),hardware_approved=False,human_approved=False,host_integrated=False,
  scope='SH101 concrete architecture correction; Juno/Mini preserved pending stronger controlled sonic references')
 L.report['environment']=dict(platform=platform.platform(),faust=command([os.getenv('FAUST','faust'),'--version']),cxx=command([os.getenv('CXX','c++'),'--version']))
 start=time.perf_counter()
 old=L.build('mono101-v1',V1['mono101']); new=L.build('mono101-v2',V2)
 old_ui,old_p=controls(old); new_ui,new_p=controls(new)
 c('mono101:io-preserved',old_ui==new_ui==(0,1),old=old_ui,new=new_ui)
 c('mono101:control-names-preserved',set(old_p)==set(new_p)==set(DEFAULTS['mono101']))
 c('mono101:defaults-preserved',all(abs(old_p[k][2]-new_p[k][2])<1e-12 for k in old_p))
 base=DEFAULTS['mono101']
 events=[(480,'gate',1),(24480,'gate',0)]
 # At zero resonance k=0, so removing the compensation term must not change DSP.
 o=r('mono101-v1-res0',old,base|{'resonance':0},events,frames=96000)[:,0]
 n=r('mono101-v2-res0',new,base|{'resonance':0},events,frames=96000)[:,0]
 c('mono101:zero-resonance-preserved',np.array_equal(o,n),max_error=float(np.max(np.abs(o-n))))
 # With resonance active, v1 adds output compensation. v2 intentionally removes it.
 o=r('mono101-v1-default',old,base,events,frames=96000)[:,0]
 n=r('mono101-v2-default',new,base,events,frames=96000)[:,0]
 c('mono101:resonant-path-changes',rel(o,n)>.01,relative_difference=rel(o,n))
 c('mono101:revised-bounded',np.max(np.abs(n))<1,peak=float(np.max(np.abs(n))))
 # Preserve lifecycle/sample-rate/block behavior.
 for sr in (44100,48000,96000):
  ev=[(round(.01*sr),'gate',1),(round(.51*sr),'gate',0)]
  y=r(f'mono101-v2-{sr}',new,base,ev,sr=sr,frames=sr*3)[:,0]
  c(f'mono101:rate-{sr}-finite',np.isfinite(y).all() and np.max(np.abs(y))<1,peak=float(np.max(np.abs(y))))
 for block in (1,64,127,512):
  y=r(f'mono101-v2-block-{block}',new,base,events,frames=96000,block=block)[:,0]
  c(f'mono101:block-{block}',np.array_equal(y,n),max_error=float(np.max(np.abs(y-n))))
 # High resonance remains finite and audible; this is not a hardware null test.
 hi=base|{'resonance':.95,'cutoff':700,'filterEnv':0,'sustain':1,'release':.5}
 y=r('mono101-v2-hires',new,hi,[(480,'gate',1),(48480,'gate',0)],frames=120000)[:,0]
 c('mono101:high-resonance-finite',np.isfinite(y).all() and .001<np.max(np.abs(y))<2,peak=float(np.max(np.abs(y))))
 # Candidate-only listening: identical score/settings, previous then revised.
 po=r('mono101-v1-phrase',old,base,phrase(root=110),frames=384000)
 pn=r('mono101-v2-phrase',new,base,phrase(root=110),frames=384000)
 writewav(L.out/'audition'/'mono101_previous_then_revised.wav',np.concatenate([po,pn]))
 writewav(L.out/'audition'/'mono101_revised.wav',pn)
 # Preserve the other two exact baseline synths and render them for this checkpoint.
 preserved={}
 for name in ('juno106','mini'):
  exe=L.build(name+'-preserved',V1[name]); ui,p=controls(exe)
  c(name+':source-hash-preserved',digest(V1[name])==L.report['builds'][name+'-preserved']['source_sha256'])
  c(name+':contract',ui==(0,1) and set(p)==set(DEFAULTS[name]))
  y=r(name+'-preserved-note',exe,DEFAULTS[name],[(480,'gate',1),(24480,'gate',0)],frames=96000)[:,0]
  c(name+':preserved-bounded',np.isfinite(y).all() and np.max(np.abs(y))<1,peak=float(np.max(np.abs(y))))
  preserved[name]=dict(source_sha256=digest(V1[name]),note_sha256=__import__('hashlib').sha256(y.tobytes()).hexdigest())
 L.report['preserved']=preserved
 L.report['references']={
  'sh101_measured_model':'BonzaiCoin SH101: source mixer+filter circuit, transistor-level VCA/envelope, measured oscillator/noise models; no source/audio copied.',
  'roland_filter':'SH-101 IR3109 has no Q compensation; Juno family uses compensation. This pass removes only the SH-101 v1 output compensation.',
  'juno106_software_oracle':'stevengoldberg/juno106 provides DCO/VCF/HPF/envelope behavior; useful structural oracle, not hardware proof.',
  'faug':'t2techno/Faug is an inspectable Faust Minimoog research oracle, but repo has no declared license and known 44.1k assumptions; no code copied.'
 }
 L.report['remaining']=[
  'Controlled Juno-106 dry hardware capture/reference before sonic retuning.',
  'Controlled Mini hardware capture or stronger licensed whole-instrument oracle before sonic retuning.',
  'SH-101 measured-model/hardware A-B listening; v2 is architecture-corrected, not hardware-approved.',
  'No GUI/control expansion in this pass.'
 ]
 L.report['wall_seconds']=time.perf_counter()-start
 (L.out/'results.json').write_text(json.dumps(L.report,indent=2))
 print(json.dumps({'checks':L.report.get('checks'),'renders':L.report.get('renders'),'builds':len(L.report.get('builds',{})),'wall_seconds':L.report['wall_seconds']},indent=2))
 return 0

if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True);raise SystemExit(run(a.out))
