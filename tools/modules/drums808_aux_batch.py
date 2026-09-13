"""808 auxiliary percussion: consolidated Tom/Conga + Rim/Claves, Maracas, Cowbell.
Actual Faust/C++ renders; reference/hardware approval explicitly pending.
"""
from pathlib import Path
import argparse, json, os, platform, time
import numpy as np
from synth_batch import SynthLab
from acid_batch import controls
from hats_v2_delivery import command, digest
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'modules/drums-808-aux/v1'
DEFAULTS=json.loads((SRC/'defaults.json').read_text())
PRESETS={
 'tom-conga':{
  'LowTom':dict(mode=0,freq=90,decay=.36,noise=.16),
  'MidTom':dict(mode=0,freq=140,decay=.25,noise=.16),
  'HighTom':dict(mode=0,freq=190,decay=.20,noise=.15),
  'LowConga':dict(mode=1,freq=190,decay=.35,noise=0),
  'MidConga':dict(mode=1,freq=280,decay=.17,noise=0),
  'HighConga':dict(mode=1,freq=410,decay=.145,noise=0),
 },
 'rim-claves':{
  'Rim':dict(mode=0,freq=455,decay=.014,crack=.55,drive=.22),
  'DryRim':dict(mode=0,freq=520,decay=.009,crack=.8,drive=.38),
  'Claves':dict(mode=1,freq=2500,decay=.062,crack=.15,drive=.02),
  'Wood':dict(mode=1,freq=1900,decay=.09,tone=.25,drive=.05),
 },
 'maracas':{
  'Classic':{},'Short':dict(decay=.022),'Dark':dict(freq=4800,tone=.2),'Shaker':dict(decay=.16,grit=.45,tone=.7),
 },
 'cowbell':{
  'Classic':{},'Low':dict(freq=430,ratio=1.5,decay=.6),'Dry':dict(decay=.16,tone=.35),'Buzz':dict(ratio=1.62,drive=.45,tone=.75),
 },
}

def run(out):
 L=SynthLab(out);c=L.check;r=L.render;L.out.joinpath('audition').mkdir(exist_ok=True)
 L.report.update(version='808-aux-0.1.0-experiment',commit=os.environ.get('GITHUB_SHA','local'),defaults=DEFAULTS,presets=PRESETS,human_approved=False,hardware_approved=False,host_integrated=False,device_qualified=False,reference_comparison='PENDING later hardware/oracle pass')
 L.report['environment']=dict(platform=platform.platform(),faust=command([os.getenv('FAUST','faust'),'--version']),cxx=command([os.getenv('CXX','c++'),'--version']))
 start=time.perf_counter();exes={};classics={}
 try:
  for name,p in DEFAULTS.items():
   print('QUALIFY',name,flush=True)
   src=SRC/(name+'.dsp');exe=L.build(name,src);vec=L.build(name+'-vector',src,True);exes[name]=exe
   io,ui=controls(exe)
   c(name+':single-note-io',io==(0,1),io=io)
   c(name+':control-set',set(ui)==set(p),actual=sorted(ui))
   c(name+':defaults',all(abs(ui[k][2]-v)<1e-5 for k,v in p.items()))
   ev=[(480,'gate',1),(481,'gate',0)]
   x=r(name+'-default',exe,p,ev,frames=144000)[:,0]
   c(name+':audible',.002<float(abs(x).max())<1.2,peak=float(abs(x).max()))
   c(name+':pretrigger-silence',not np.any(x[:480]))
   c(name+':never-triggered-silent',not np.any(r(name+'-never',exe,p,frames=24000)))
   z=r(name+'-zero-velocity',exe,p|{'velocity':0},ev,frames=48000)
   c(name+':zero-velocity',not np.any(z))
   h=r(name+'-half-velocity',exe,p|{'velocity':.5},ev,frames=48000)[:,0]
   c(name+':velocity-linear',float(abs(h-.5*x[:48000]).max())<3e-6)
   held=r(name+'-held',exe,p,[(480,'gate',1)],frames=48000)[:,0]
   c(name+':held-one-shot',np.array_equal(held,x[:48000]))
   for block in (1,64,127,512):
    y=r(name+'-block-'+str(block),exe,p,ev,frames=48000,block=block)[:,0]
    c(name+':block-'+str(block),np.array_equal(y,x[:48000]))
   y=r(name+'-vector',vec,p,ev,frames=48000)[:,0]
   c(name+':vector-parity',float(abs(y-x[:48000]).max())<1e-4,max_error=float(abs(y-x[:48000]).max()))
   for sr in (44100,96000):
    n=round(.01*sr);y=r(name+'-rate-'+str(sr),exe,p,[(n,'gate',1),(n+1,'gate',0)],sr=sr,frames=sr*2)
    c(name+':rate-'+str(sr),np.isfinite(y).all() and float(abs(y).max())<1.5)
   rapid=[]
   for i in range(64):
    n=480+i*700;rapid += [(n,'gate',1),(n+1,'gate',0),(n,'velocity',.6 if i%3 else 1)]
   y=r(name+'-rapid',exe,p,rapid,frames=60000)
   c(name+':rapid-bounded',np.isfinite(y).all() and float(abs(y).max())<1.8)
   # controls beyond note contract must audibly affect the source
   testkeys=[k for k in p if k not in ('gate','velocity','accent','level')]
   for k in testkeys:
    lo,hi=ui[k][0],ui[k][1]
    a=r(name+'-'+k+'-min',exe,p|{k:lo},ev,frames=48000)
    b=r(name+'-'+k+'-max',exe,p|{k:hi},ev,frames=48000)
    rel=float(np.linalg.norm(a-b)/(np.linalg.norm(a)+1e-20))
    c(name+':effective-'+k,rel>.001,relative_l2=rel)
   bank=[]
   for title,over in PRESETS[name].items():
    pe=[(4800,'gate',1),(4801,'gate',0),(67200,'velocity',.65),(67200,'gate',1),(67201,'gate',0)]
    y=r(name+'-preset-'+title,exe,p|over,pe,frames=144000)[:,0]
    L.wav(name+'_'+title+'.wav',y);bank.append(y)
    if title in ('LowTom','Rim','Classic'): classics[name]=y
   L.wav(name+'_preset_bank.wav',np.concatenate(bank))
  # Explicit consolidation checks: presets share the exact same executable/source.
  tc=exes['tom-conga'];base=DEFAULTS['tom-conga'];ev=[(480,'gate',1),(481,'gate',0)]
  lt=r('tom-low',tc,base|PRESETS['tom-conga']['LowTom'],ev,frames=48000)
  lc=r('conga-low',tc,base|PRESETS['tom-conga']['LowConga'],ev,frames=48000)
  c('tom-conga:same-engine-different-mode',float(np.linalg.norm(lt-lc))>.1)
  rr=exes['rim-claves'];base=DEFAULTS['rim-claves']
  rim=r('rim-mode',rr,base|PRESETS['rim-claves']['Rim'],ev,frames=48000)
  clav=r('claves-mode',rr,base|PRESETS['rim-claves']['Claves'],ev,frames=48000)
  c('rim-claves:same-engine-different-mode',float(np.linalg.norm(rim-clav))>.1)
  # Persistent four-voice pattern driven by the existing Faust Trigger Seq.
  seq=L.build('trigger-seq',ROOT/'modules/trigger-seq/v1/trigger.dsp')
  sd=dict(run=1,length=16,clock=0,reset=0)|{f'step{i+1:02d}':0 for i in range(32)}
  patterns={'tom-conga':[3,7,11,15],'rim-claves':[4,12],'maracas':[2,6,10,14],'cowbell':[0,7,8,15]}
  clocks=[(4800+i*6000+j,'clock',1-j) for i in range(64) for j in (0,1)]
  stems=[]
  for name,pat in patterns.items():
   z=r(name+'-seq',seq,sd|{f'step{i+1:02d}':1 for i in pat},clocks,frames=576000)[:,0]
   ons=np.flatnonzero(z>.5).tolist();ev=[]
   for n in ons:ev += [(n,'gate',1),(n+1,'gate',0)]
   y=r(name+'-groove',exes[name],DEFAULTS[name]|(PRESETS[name].get('LowTom',{}) if name=='tom-conga' else PRESETS[name].get('Classic',{})),ev,frames=576000)[:,0]
   stems.append(y);L.wav(name+'_stem.wav',y)
  mix=sum(stems)*.5;L.wav('00_808_aux_groove.wav',mix)
  L.wav('01_808_aux_isolated.wav',np.concatenate([classics[k] for k in ('tom-conga','rim-claves','maracas','cowbell')]))
  L.report['audition']=dict(bpm=120,bars=4,seconds=12,groove_gain=.5,processing='Dry actual Faust voices; no external EQ/reverb/compression/limiting/normalization. Offline adapter, not CURLOP.')
  L.report['sources']={str(p.relative_to(ROOT)):digest(p) for p in sorted(SRC.iterdir()) if p.is_file()}
  L.report['driver_sha256']=digest(__file__);L.report['renderer_sha256']=digest(ROOT/'tools/modules/render.cpp')
  L.report['passed']=all(x['passed'] for x in L.report['checks']);L.report['wall_seconds']=time.perf_counter()-start
 finally:
  (L.out/'report.json').write_text(json.dumps(L.report,indent=2,sort_keys=True))
 print(json.dumps({'passed':L.report.get('passed'), 'checks':len(L.report['checks']), 'renders':len(L.report['renders']), 'builds':len(L.report['builds'])}),flush=True)
 if not L.report.get('passed'):raise SystemExit(1)

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
