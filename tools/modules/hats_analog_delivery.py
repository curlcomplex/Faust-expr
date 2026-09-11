from pathlib import Path
import argparse, hashlib, json, os, subprocess
import numpy as np
from scipy.io import wavfile
from scipy.signal import welch
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'modules/hats-analog/v1/hats.dsp'
DEFAULT=dict(metal=.72,tone=.58,decay=.42,shape=.52,choke=.70,drive=.12,pitch_hz=1.,articulation=0.,velocity=1.,gate=0.)
PRESETS={
 '01_Classic':{},
 '02_Soft':dict(metal=.54,tone=.40,shape=.30,drive=.03,decay=.50),
 '03_Crunch':dict(metal=.68,tone=.61,shape=.70,drive=.72,decay=.34),
 '04_Glass':dict(metal=.95,tone=.82,shape=.78,pitch_hz=1.22,drive=.08),
 '05_Dust':dict(metal=.28,tone=.63,shape=.38,drive=.18,decay=.33),
 '06_LowMetal':dict(metal=.90,tone=.31,shape=.60,pitch_hz=.72,drive=.22,decay=.55),
 '07_Acid':dict(metal=.82,tone=.93,shape=.92,pitch_hz=1.38,drive=.48,decay=.28),
 '08_Long':dict(metal=.70,tone=.52,shape=.44,drive=.10,decay=.82,choke=.38),
}
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def cmd(a): return subprocess.check_output([str(x) for x in a],stderr=subprocess.STDOUT,text=True,timeout=120)
def desc(x,sr):
 x=np.asarray(x,float); e=x*x; c=np.cumsum(e); c/=c[-1]+1e-30
 f,p=welch(np.pad(x,(512,512)),sr,nperseg=min(1024,len(x)),noverlap=min(768,max(0,len(x)//2))); p/=p.sum()+1e-30
 return {'centroid_hz':float((f*p).sum()),'high_8k_pct':float(100*p[f>=8000].sum()),'t90_ms':float(np.searchsorted(c,.9)*1000/sr),'peak':float(np.max(np.abs(x)))}
def run(out):
 out.mkdir(parents=True,exist_ok=True); report={'version':'0.1.0-experiment','checks':[],'renders':[],'presets':PRESETS,'human_approved':False}
 def check(name,ok,**kw):
  report['checks'].append({'name':name,'passed':bool(ok),**kw})
  if not ok: raise AssertionError(f'{name}: {kw}')
 def build(label,vec=False):
  d=out/label; d.mkdir(exist_ok=True)
  a=[os.getenv('FAUST','faust'),'-lang','cpp','-single','-cn','ModuleDSP']
  if vec:a+=['-vec','-lv','0','-vs','32']
  cmd(a+[SRC,'-o',d/'generated.hpp'])
  cmd([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render'])
  controls=cmd([d/'render','--controls']); (d/'controls.tsv').write_text(controls)
  return d/'render'
 def render(name,exe,p=None,events=None,sr=48000,block=128,seconds=2.5):
  vals=DEFAULT|(p or {}); rows={(0,k):v for k,v in vals.items()}
  for n,k,v in events or [(101,'gate',1),(165,'gate',0)]: rows[n,k]=v
  score=out/(name+'.tsv'); raw=out/(name+'.f32'); score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
  frames=round(seconds*sr); dg=json.loads(cmd([exe,score,raw,sr,block,frames,0])); x=np.fromfile(raw,'<f4')
  check(name+':finite',len(x)==frames and np.isfinite(x).all() and np.max(abs(x))<1,peak=float(np.max(abs(x))))
  report['renders'].append({'name':name,'rate':sr,'block':block,'sha256':sha(raw),'diagnostics':dg})
  return x
 try:
  exe=build('scalar'); vec=build('vector',True)
  ch=render('closed-default',exe)
  oh=render('open-default',exe,dict(articulation=1))
  check('open-longer-than-closed',desc(oh,48000)['t90_ms']>desc(ch,48000)['t90_ms']*3,closed=desc(ch,48000),open=desc(oh,48000))
  half=render('half-velocity',exe,dict(velocity=.5)); check('velocity-linear',np.max(abs(half-.5*ch))<2e-7)
  v=render('vector',vec); check('scalar-vector',np.max(abs(v-ch))<3e-5,max_abs=float(np.max(abs(v-ch))))
  for b in (1,32,64,127,256,512): check('block-'+str(b),np.array_equal(render('block-'+str(b),exe,block=b),ch))
  # Open at 0ms, closed at 120ms. Compare no/fade/hard choke energy after CH onset.
  def choke_case(name,c):
   ev=[(101,'articulation',1),(101,'gate',1),(130,'gate',0),(101+5760,'articulation',0),(101+5760,'choke',c),(101+5760,'gate',1),(101+5790,'gate',0)]
   return render(name,exe,events=ev,seconds=.8)
  none=choke_case('choke-none',0); fade=choke_case('choke-fade',.55); hard=choke_case('choke-hard',1)
  s=101+5760+1000;e=s+7000
  energies=[float(np.sum(x[s:e]**2)) for x in (none,fade,hard)]
  check('choke-order',energies[0]>energies[1]>energies[2],energies=energies)
  # Six controls must materially affect either spectral or temporal output.
  base=desc(ch,48000); impacts={}
  for k in ('metal','tone','decay','shape','drive'):
   x=render('probe-'+k,exe,{k:1 if DEFAULT[k]<.75 else 0}); d=desc(x,48000)
   impacts[k]=d
   metric=max(abs(d['centroid_hz']-base['centroid_hz'])/max(base['centroid_hz'],1),abs(d['t90_ms']-base['t90_ms'])/max(base['t90_ms'],1),abs(d['peak']-base['peak'])/max(base['peak'],1e-6))
   check('control-impact-'+k,metric>.05,impact=float(metric))
  impacts['choke']={'sequence_energy_order':energies}; report['control_impacts']=impacts
  # Presets and rates.
  for sr in (44100,48000,96000):
   for name,p in PRESETS.items():
    c=render(name+'-CH-'+str(sr),exe,p|{'articulation':0},sr=sr,seconds=3)
    o=render(name+'-OH-'+str(sr),exe,p|{'articulation':1},sr=sr,seconds=3)
    if sr==48000:
     wavfile.write(out/(name+'_CH.wav'),sr,c); wavfile.write(out/(name+'_OH.wav'),sr,o)
  # Musical audition: each preset CH, CH, OH, then OH->CH choke.
  ev=[]
  for i,(name,p) in enumerate(PRESETS.items()):
   start=4800+i*96000
   for k,vv in (DEFAULT|p).items():
    if k!='gate': ev.append((start,k,vv))
   ev += [(start,'articulation',0),(start,'gate',1),(start+30,'gate',0),
          (start+12000,'gate',1),(start+12030,'gate',0),
          (start+24000,'articulation',1),(start+24000,'gate',1),(start+24030,'gate',0),
          (start+48000,'articulation',1),(start+48000,'gate',1),(start+48030,'gate',0),
          (start+60000,'articulation',0),(start+60000,'gate',1),(start+60030,'gate',0)]
  bank=render('analog_hats_bank',exe,events=ev,seconds=17)
  gain=.88/max(abs(bank)); wavfile.write(out/'Analog_Hats_8_presets.wav',48000,(bank*gain*32767).round().astype('<i2'))
  report['audition_global_gain']=float(gain); report['default_descriptors']={'closed':desc(ch,48000),'open':desc(oh,48000)}
  report['passed']=True; report['source_sha256']=sha(SRC); report['faust']=cmd([os.getenv('FAUST','faust'),'--version']).strip()
 finally:
  (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps({'passed':report.get('passed',False),'checks':len(report['checks']),'renders':len(report['renders'])}))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();run(a.out.resolve())
