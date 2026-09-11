"""Actual Faust/C++ hats v2 renders, interaction regressions and raw auditions.
Run with FAUST=/path/to/faust python3 tools/modules/hats_v2_delivery.py --out DIR.
No Python substitute synth. No musical acceptance inferred from test passes.
"""
from pathlib import Path
import argparse,hashlib,itertools,json,os,subprocess
import numpy as np
from scipy.io import wavfile
from scipy.signal import welch, resample_poly
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'modules/hats-analog/v2'
DEFAULT=dict(metal=.96,tone=.60,decay=.46,shape=.30,choke=.78,drive=.12,pitch_ratio=1.,articulation=0.,velocity=1.,gate=0.,choke_gate=0.)
PRESETS={
 '01_Classic':{},
 '02_Tight':dict(decay=.18,shape=.40,tone=.63),
 '03_Soft':dict(metal=.88,tone=.31,shape=.12,drive=.03,decay=.42),
 '04_Dust':dict(metal=.42,tone=.38,shape=.28,drive=.16,decay=.36),
 '05_Crunch':dict(metal=.90,tone=.55,shape=.74,drive=.78,decay=.39),
 '06_LowMetal':dict(metal=1.,tone=.22,shape=.46,pitch_ratio=.72,drive=.20,decay=.51),
 '07_Glass':dict(metal=1.,tone=.78,shape=.91,pitch_ratio=1.25,drive=.12,decay=.41),
 '08_Long':dict(metal=.92,tone=.48,shape=.36,decay=.75,choke=.5),
}

def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def command(args):return subprocess.check_output([str(a) for a in args],text=True,stderr=subprocess.STDOUT,timeout=120)
def describe(x,sr=48000):
 x=np.asarray(x,float);f,p=welch(np.pad(x,(512,512)),sr,nperseg=1024,noverlap=768);p/=p.sum()+1e-30
 c=np.cumsum(x*x);c/=c[-1]+1e-30
 return dict(centroid_hz=float((f*p).sum()),below3k_pct=float(100*p[f<3000].sum()),above8k_pct=float(100*p[f>=8000].sum()),t50_ms=float(np.searchsorted(c,.5)*1000/sr),t90_ms=float(np.searchsorted(c,.9)*1000/sr),peak=float(np.max(abs(x))))

class Lab:
 def __init__(self,out):
  self.out=Path(out);self.out.mkdir(parents=True,exist_ok=True)
  self.report=dict(version='0.2.0-experiment',checks=[],renders=[],builds={},presets=PRESETS,human_approved=False,device_qualified=False)
 def check(self,name,ok,**details):
  self.report['checks'].append(dict(name=name,passed=bool(ok),**details))
  if not ok:raise AssertionError(name+': '+str(details))
 def build(self,name,src,vector=False):
  d=self.out/name;d.mkdir(exist_ok=True)
  a=[os.getenv('FAUST','faust'),'-I',src.parent,'-lang','cpp','-single','-cn','ModuleDSP']
  if vector:a+=['-vec','-lv','0','-vs','32']
  command(a+[src,'-o',d/'generated.hpp'])
  command([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render'])
  ui=command([d/'render','--controls']);(d/'controls.tsv').write_text(ui)
  self.report['builds'][name]=dict(source=str(src.relative_to(ROOT)) if src.is_relative_to(ROOT) else str(src),source_sha256=digest(src),generated_sha256=digest(d/'generated.hpp'),controls=ui)
  return d/'render'
 def render(self,name,exe,p=None,events=None,sr=48000,block=128,seconds=2.,legacy=False):
  values=DEFAULT|(p or {})
  if legacy:
   values.pop('choke_gate');values['pitch_hz']=values.pop('pitch_ratio')
  events=[(480,'gate',1),(481,'gate',0)] if events is None else events
  rows={(0,k):v for k,v in values.items()}
  for n,k,v in events:
   if legacy and k=='pitch_ratio':k='pitch_hz'
   if legacy and k=='choke_gate':continue
   rows[n,k]=v
  score=self.out/(name+'.tsv');raw=self.out/(name+'.f32')
  score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
  frames=round(seconds*sr);dg=json.loads(command([exe,score,raw,sr,block,frames,0]));x=np.fromfile(raw,'<f4').reshape(-1,dg['channels'])
  if dg['channels']==1:x=x[:,0]
  self.check(name+':finite',len(x)==frames and np.isfinite(x).all(),peak=float(abs(x).max()))
  self.report['renders'].append(dict(name=name,rate=sr,block=block,raw_sha256=digest(raw),score_sha256=digest(score),diagnostics=dg))
  return x

def run(out):
 L=Lab(out);r=L.render;c=L.check
 try:
  mono=L.build('scalar',SOURCE/'hats.dsp');vec=L.build('vector',SOURCE/'hats.dsp',True);voices=L.build('voices',SOURCE/'voices.dsp');old=L.build('legacy',ROOT/'modules/hats-analog/v1/hats.dsp')
  ui=L.report['builds']['scalar']['controls'].splitlines();actual={a.split('\t')[0]:float(a.split('\t')[3]) for a in ui[1:]}
  c('canonical-ui',set(actual)==set(DEFAULT) and all(abs(actual[k]-v)<1e-6 for k,v in DEFAULT.items()))
  ch=r('closed',mono);oh=r('open',mono,dict(articulation=1))
  c('pre-onset-silence',np.max(abs(ch[:480]))==0 and np.max(abs(oh[:480]))==0)
  c('never-triggered',not np.any(r('never',mono,events=[])))
  c('zero-velocity',not np.any(r('zero',mono,dict(velocity=0))))
  for art,base in [(0,ch),(1,oh)]:
   c('velocity-linear-'+str(art),np.max(abs(r('half-'+str(art),mono,dict(articulation=art,velocity=.5))-base*.5))<2e-7)
   c('noteoff-no-choke-'+str(art),np.array_equal(r('held-'+str(art),mono,dict(articulation=art),events=[(480,'gate',1)]),base))
  for b in (1,32,64,127,256,512):c('block-'+str(b),np.array_equal(r('block-'+str(b),mono,block=b),ch))
  c('vector-parity',np.max(abs(r('vector',vec)-ch))<3e-5)
  locks=[(480,'articulation',1),(480,'gate',1),(481,'gate',0)]+[(8000,k,.9 if k!='pitch_ratio' else 1.6) for k in ['metal','tone','decay','shape','drive','pitch_ratio','velocity']]
  c('ringing-controls-latched',np.array_equal(r('latched',mono,events=locks),oh))
  # Silent CH with all different controls must not change an unchoked ringing OH.
  later=480+5760
  ev=[(480,'articulation',1),(480,'gate',1),(481,'gate',0),(later,'articulation',0),(later,'gate',1),(later+1,'gate',0),(later,'velocity',0),(later,'choke',0)]
  ev += [(later,k,v) for k,v in dict(metal=.1,tone=.1,decay=1.,shape=.9,drive=.9,pitch_ratio=.7).items()]
  unchanged=r('independent-open',mono,events=ev)
  c('closed-locks-cannot-revoice-open',np.array_equal(unchanged,oh))
  oldbase=r('old-open',old,dict(metal=.72,tone=.58,decay=.42,shape=.52,choke=.70,drive=.12,articulation=1),legacy=True)
  oldtest=r('old-independence-fails',old,events=ev,legacy=True)
  # Compare old under the same test controls, not its different default patch.
  oldsame=r('old-open-test-patch',old,dict(articulation=1),legacy=True)
  c('independence-gate-rejects-v1',np.max(abs(oldsame[later:]-oldtest[later:]))>.01)
  L.report['legacy_default_descriptors']={'open':describe(oldbase[480:])}
  oldch=r('old-closed',old,dict(metal=.72,tone=.58,decay=.42,shape=.52,choke=.70,drive=.12),legacy=True)
  L.report['legacy_default_descriptors']['closed']=describe(oldch[480:])
  # Voice-separated diagnostics prevent a loud CH from hiding OH choke failure.
  choke_metrics={}
  basevoices=r('open-voices',voices,dict(articulation=1))
  for name,strength in [('off',0),('fade',.5),('hard',1)]:
   seq=[(480,'articulation',1),(480,'gate',1),(481,'gate',0),(later,'articulation',0),(later,'choke',strength),(later,'gate',1),(later+1,'gate',0)]
   x=r('choke-'+name,voices,events=seq);mixed=r('choke-'+name+'-mono',mono,events=seq)
   c('voice-sum-'+name,np.max(abs(x.sum(axis=1)-mixed))<3e-6)
   if name=='off':c('choke-off-exact',np.array_equal(x[:,1],basevoices[:,1]))
   start=later+480;choke_metrics[name]=float(np.sum(x[start:start+6000,1].astype(float)**2))
  c('choke-orders-open-only',choke_metrics['off']>choke_metrics['fade']>choke_metrics['hard'])
  ext=[(480,'articulation',1),(480,'gate',1),(481,'gate',0),(later,'choke',1),(later,'choke_gate',1),(later+1,'choke_gate',0)]
  extx=r('external-choke',voices,events=ext)
  c('external-choke-no-closed-hit',not np.any(extx[:,0]))
  c('hard-choke-silence-after-10ms',abs(extx[later+480:,1]).max()<1e-6)
  c('simultaneous-choke-wins',not np.any(r('simultaneous',mono,dict(articulation=1),events=[(480,'gate',1),(480,'choke_gate',1),(481,'gate',0),(481,'choke_gate',0)])))
  twice=ext+[(later+2400,'choke_gate',1),(later+2401,'choke_gate',0)]
  c('repeated-choke-no-resurrection',np.array_equal(r('double-choke',voices,events=twice),extx))
  restart=twice+[(later+4800,'gate',1),(later+4801,'gate',0)]
  c('fresh-open-clears-choke',abs(r('fresh-open',voices,events=restart)[later+4900:,1]).max()>.01)
  for art in (0,1):
   x=r('long-silence-'+str(art),mono,dict(articulation=art,decay=1),seconds=20)
   c('no-frozen-tail-'+str(art),abs(x[-48000:]).max()<1e-8)
  oldlong=r('old-frozen-tail',old,dict(articulation=1,decay=1),seconds=20,legacy=True)
  c('tail-gate-rejects-v1',np.sqrt(np.mean(oldlong[-48000:].astype(float)**2))>1e-5)
  # Endpoint trajectory and rapid articulation alternation on one persistent instance.
  events=[];keys=['metal','tone','decay','shape','choke','drive']
  for i,bits in enumerate(itertools.product((0.,1.),repeat=6)):
   n=480+2400*i;events += [(n,k,v) for k,v in zip(keys,bits)]+[(n,'pitch_ratio',.6 if i%2 else 1.7),(n,'articulation',i%2),(n,'gate',1),(n+1,'gate',0)]
  r('64-corners',mono,events=events,seconds=6)
  events=[]
  for i in range(128):
   n=480+i*211;events += [(n,'articulation',i%2),(n,'velocity',.2+.8*(i%7)/6),(n,'gate',1),(n+1,'gate',0)]
  r('128-retriggers',mono,events=events)
  for sr in (44100,48000,96000):
   for name,p in PRESETS.items():
    for art in (0,1):
     x=r(name+'-'+str(art)+'-'+str(sr),mono,p|dict(articulation=art),sr=sr,seconds=3)
     if sr==48000:wavfile.write(L.out/(name+('_CH' if art==0 else '_OH')+'_raw.wav'),sr,x)
  impacts={}
  for k in ('metal','tone','decay','shape','drive','pitch_ratio'):
   value=1.7 if k=='pitch_ratio' else (0 if DEFAULT[k]>.7 else 1)
   x=r('control-'+k,mono,{k:value});change=float(np.linalg.norm(x-ch)/(np.linalg.norm(ch)+1e-20));impacts[k]=change;c('control-effect-'+k,change>.02)
  L.report['control_relative_l2']=impacts
  # Audible ablations preserve filter, envelope and drive routing. Noise-only
  # uses Metal=0; a separate source mutation reduces ONLY the oscillator bank.
  simple=L.out/'single-oscillator-source';simple.mkdir(exist_ok=True)
  engine=(SOURCE/'engine.lib').read_text()
  oldbank='bank=(1-ringAmt)*summed+ringAmt*ringed;'
  if engine.count(oldbank)!=1:raise ValueError('Ablation source anchor changed')
  (simple/'engine.lib').write_text(engine.replace(oldbank,'bank=s1;'))
  (simple/'hats.dsp').write_text((SOURCE/'hats.dsp').read_text())
  simple_exe=L.build('single-oscillator',simple/'hats.dsp')
  L.report['ablations']={}
  for art in (0,1):
   full=ch if art==0 else oh
   noise=r('ablation-noise-'+str(art),mono,dict(metal=0,articulation=art))
   one=r('ablation-one-'+str(art),simple_exe,dict(articulation=art))
   L.report['ablations'][str(art)]={}
   for label,x in [('noise',noise),('one-oscillator',one)]:
    delta=float(np.linalg.norm(x-full)/(np.linalg.norm(full)+1e-20))
    c('ablation-detectable-'+label+'-'+str(art),delta>.1,relative_l2=delta)
    L.report['ablations'][str(art)][label]=describe(x[480:])
    wavfile.write(L.out/('diagnostic_'+label+'_'+str(art)+'.wav'),48000,x)
  # Matched real-time onset, noiseless excitation, filtered 96->48 replay.
  # This residual includes filter/oscillator approximation; it is NOT an
  # isolated alias-energy measurement or a pass/fail sound-quality score.
  L.report['rate_residuals']={}
  for label,patch in [('clean',dict(metal=1,shape=.3,drive=0)),('extreme',dict(metal=1,shape=1,drive=1,pitch_ratio=1.7))]:
   a=r('rate-'+label+'-48',mono,patch|dict(articulation=1),seconds=2)
   b=r('rate-'+label+'-96',mono,patch|dict(articulation=1),sr=96000,seconds=2,events=[(960,'gate',1),(962,'gate',0)])
   down=resample_poly(b.astype(float),1,2)
   residual=float(20*np.log10((np.linalg.norm(a-down)+1e-30)/(np.linalg.norm(down)+1e-30)))
   L.report['rate_residuals'][label]={'relative_db':residual,'interpretation':'Combined oscillator/filter/nonlinearity rate residual, not alias energy.'}
  # Invalid values must be rejected by the existing native score validator.
  for label,row in [('nan','0\tmetal\tnan\n'),('range','0\tmetal\t2\n'),('duplicate','0\tgate\t0\n0\tgate\t1\n')]:
   score=L.out/('bad-'+label+'.tsv');score.write_text(row)
   code=subprocess.run([mono,score,L.out/'invalid.f32','48000','128','1024','0'],capture_output=True).returncode;c('reject-'+label,code!=0)
  events=[]
  for i,(name,p) in enumerate(PRESETS.items()):
   n=4800+i*144000
   events += [(n,k,v) for k,v in (DEFAULT|p).items() if k not in ('gate','choke_gate','articulation')]
   for off,art,vel in [(0,0,1),(12000,0,.65),(24000,1,1),(72000,1,1),(79200,0,.8)]:
    events += [(n+off,'articulation',art),(n+off,'velocity',vel),(n+off,'gate',1),(n+off+1,'gate',0)]
  bank=r('preset-bank',mono,events=events,seconds=25);wavfile.write(L.out/'preset_bank_raw.wav',48000,bank)
  events=[]
  for i in range(64):
   n=4800+i*6000;events += [(n,'articulation',int(i%8 in (3,7))),(n,'velocity',[1,.42,.68,.65][i%4]),(n,'gate',1),(n+1,'gate',0)]
  groove=r('groove',mono,events=events,seconds=9);wavfile.write(L.out/'groove_raw.wav',48000,groove)
  L.report['default_descriptors']={'closed':describe(ch[480:]),'open':describe(oh[480:])}
  c('closed-is-brighter-than-open',L.report['default_descriptors']['closed']['centroid_hz']>1.25*L.report['default_descriptors']['open']['centroid_hz'])
  L.report['choke_open_energy']=choke_metrics
  L.report['source_sha256']={p.name:digest(p) for p in SOURCE.glob('*') if p.suffix in ('.lib','.dsp')}
  L.report['compiler']={'faust':command([os.getenv('FAUST','faust'),'--version']),'cxx':command([os.getenv('CXX','c++'),'--version']).splitlines()[0]}
  L.report['passed']=True
 finally:
  (L.out/'qualification.json').write_text(json.dumps(L.report,indent=2)+'\n')
 print(json.dumps({'passed':L.report.get('passed',False),'checks':len(L.report['checks']),'renders':len(L.report['renders'])}))
if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('--out',type=Path,required=True);args=a.parse_args();run(args.out.resolve())
