"""SH-101 gain experiment qualification; not hardware calibration.

The v2 sound is preserved for review but is NOT selected as a hardware correction.
Continuation found conflicting circuit descriptions and a nearly pure gain change.
Python predicts the explicit gain equation; every audible candidate is real Faust.
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


# Pinned against the previously delivered PR65 source archive, not today's
# compiler report. Intentional sound changes require a new version, not new pins.
PINNED = {
 'modules/mono-101/v1/voice.dsp': 'a099f9523479246312cd3b6c5ee5347c0067e3fa98e84739b268686044679506',
 'modules/mono-101/v2/voice.dsp': 'e268fae236b74fb7b29d607d56dca16bb5fdcc11c338c70ec967be4b7d62fffb',
 'modules/juno-106/v1/voice.dsp': '7d4c12ef4e9b1dd8f3907e94852a0bd2a391e97a34446ce3a59c386594b7be19',
 'modules/minimoog/v1/voice.dsp': '442916e387c9688fa0f29cfab832d65929d2a6dca321685c344dbbc105870899',
 'modules/analog-classics/synth-batch/common.lib': '95ee2dfc592cb68f5a39e0a845e4c8e8c5f5ddf805e05a047a47116b3e3c87b7',
}

def gain_prediction(resonance):
 return 1.0 / (1.0 + .20 * 3.85 * float(resonance))

def finalize_report(report, out):
 """A failing or empty assertion set must fail CI, while retaining diagnostics."""
 checks=report.get('checks', [])
 failures=[x['name'] for x in checks if x.get('passed') is not True]
 if not checks: failures.append('no-checks-executed')
 report['failures']=failures
 report['passed']=not failures
 (Path(out)/'results.json').write_text(json.dumps(report,indent=2))
 return 1 if failures else 0


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
 L.report.update(version='synth-reference-checkpoint-0.2',commit=os.environ.get('GITHUB_SHA','local'),hardware_approved=False,human_approved=False,host_integrated=False,
  scope='SH101 unapproved gain experiment; Juno/Mini preserved; controlled hardware comparison blocked',
  presets={'Mono101Comparison':dict(DEFAULTS['mono101'])},
  reference_comparison_passed=False, selected_for_promotion=False)
 L.report['environment']=dict(platform=platform.platform(),faust=command([os.getenv('FAUST','faust'),'--version']),cxx=command([os.getenv('CXX','c++'),'--version']))
 start=time.perf_counter()
 L.report['pinned_sources']=PINNED
 for path,expected in PINNED.items():
  c('pin:'+path,digest(ROOT/path)==expected,expected=expected,actual=digest(ROOT/path))
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
 # Actual lifecycle checks for v2, not only a rerun of the preserved v1 suite.
 never=r('mono101-v2-never',new,base,frames=96000)[:,0]
 c('mono101:v2-never-silent',not np.any(never))
 zero=r('mono101-v2-zero',new,base|{'velocity':0},events,frames=96000)[:,0]
 c('mono101:v2-zero-velocity',not np.any(zero))
 half=r('mono101-v2-half',new,base|{'velocity':.5},events,frames=96000)[:,0]
 c('mono101:v2-velocity-linear',float(np.max(abs(half-.5*n)))<2e-6)
 c('mono101:v2-before-onset',not np.any(n[:480]))
 c('mono101:v2-release-silent',float(np.max(abs(n[-4800:])))<1e-6)
 vec=L.build('mono101-v2-vector',V2,True)
 vx=r('mono101-v2-vector-note',vec,base,events,frames=96000)[:,0]
 c('mono101:v2-vector-parity',float(np.max(abs(vx-n)))<1e-4,max_error=float(np.max(abs(vx-n))))
 # Measure what changed: explicit fixed-resonance gain, NOT hardware agreement.
 # The first second is excluded because knob smoothing initializes at zero.
 gains=[]
 for resonance in (0.,.3,.6,.95):
  for cutoff in (350.,3000.):
   p=base|{'resonance':resonance,'cutoff':cutoff,'filterEnv':0,
           'sustain':1,'attack':.001,'noise':0}
   label=f'gain-r{resonance}-c{cutoff}'
   a=r(label+'-v1',old,p,[(4800,'gate',1)],frames=96000)[:,0]
   b=r(label+'-v2',new,p,[(4800,'gate',1)],frames=96000)[:,0]
   prediction=gain_prediction(resonance)
   residual=rel(b[48000:],prediction*a[48000:])
   c(label+':predicted-gain-law',residual<1e-4,relative_residual=residual)
   gains.append(dict(resonance=resonance,cutoff_hz=cutoff,
    predicted_gain=prediction,predicted_db=float(20*np.log10(prediction)),
    residual=residual,meaning='Same filter poles/feedback; fixed-resonance output gain only.'))
 L.report['gain_experiment']=gains
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
  c(name+':source-hash-preserved',digest(V1[name])==PINNED[str(V1[name].relative_to(ROOT))])
  c(name+':contract',ui==(0,1) and set(p)==set(DEFAULTS[name]))
  y=r(name+'-preserved-note',exe,DEFAULTS[name],[(480,'gate',1),(24480,'gate',0)],frames=96000)[:,0]
  c(name+':preserved-bounded',np.isfinite(y).all() and np.max(np.abs(y))<1,peak=float(np.max(np.abs(y))))
  preserved[name]=dict(source_sha256=digest(V1[name]),note_sha256=__import__('hashlib').sha256(y.tobytes()).hexdigest())
 L.report['preserved']=preserved
 L.report['references']={
  'sh101_service_notes':'Roland November 1982, p7: TR26/TR27 resonance/output network. See REFERENCES.md for the source and uncertainty.',
  'amsynths_overview':'https://amsynths.co.uk/2022/04/06/all-about-the-ir3109-chip/ says no SH101 Q compensation.',
  'amsynths_design':'https://amsynths.co.uk/2022/03/21/5631/ explicitly describes TR26/output-amplifier gain compensation in its SH101-derived circuit.',
  'interpretation':'These descriptions cannot justify deleting all resonance-linked output gain. V2 remains a hypothesis; v1 gain is not hardware calibrated either.',
  'bonzai':'https://bonzaicoin.net/ describes netlist-derived models, not independent physical SH101 recordings. Not run or compared here.',
  'juno106':'stevengoldberg/juno106 is a structural WebAudio model, not measured hardware truth.',
  'faug':'t2techno/Faug: source inspected previously; no declared licence established, no copied code or executed whole-instrument oracle.'
 }
 L.report['remaining']=[
  'Controlled resonance/passband-gain measurement or validated circuit transfer before selecting v1/v2/replacement.',
  'Controlled dry Juno-106 and Model D captures; none acquired in this checkpoint.',
  'Whole-instrument reference tuning, owner listening and host/device tests remain open.'
 ]
 L.report['wall_seconds']=time.perf_counter()-start
 status=finalize_report(L.report,L.out)
 print(json.dumps({'checks':L.report.get('checks'),'renders':L.report.get('renders'),'builds':len(L.report.get('builds',{})),'wall_seconds':L.report['wall_seconds']},indent=2))
 return status

if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True);raise SystemExit(run(a.out))
