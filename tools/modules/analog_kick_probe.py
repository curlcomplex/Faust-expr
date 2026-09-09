"""Actual-Faust verification/evidence for kick-analog. No external reference audio."""
from __future__ import annotations
import argparse, hashlib, itertools, json, math, os, subprocess, wave
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[2]; MOD=ROOT/'modules/kick-analog'
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def run(cmd,cwd=ROOT):
 p=subprocess.run(list(map(str,cmd)),cwd=cwd,text=True,capture_output=True,timeout=180)
 if p.returncode: raise RuntimeError((p.stdout+'\n'+p.stderr).strip())
 return p.stdout
def build(out,label,source,vector=False):
 d=out/label; d.mkdir(parents=True,exist_ok=True); faust=os.getenv('FAUST','faust'); cxx=os.getenv('CXX','c++')
 flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vector else [])
 run([faust,*flags,source,'-o',d/'generated.hpp']); run([cxx,'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render'])
 (d/'controls.tsv').write_text(run([d/'render','--controls']))
 return d/'render',dict(source_sha256=sha(source),generated_sha256=sha(d/'generated.hpp'),binary_sha256=sha(d/'render'),flags=flags)
def render(out,label,exe,params,events,rate=48000,block=128,seconds=2):
 frames=int(rate*seconds); score=out/(label+'.tsv'); raw=out/(label+'.f32'); rows={(0,k):v for k,v in params.items()}
 for n,k,v in events: rows[n,k]=v
 score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
 diag=json.loads(run([exe,score,raw,rate,block,frames,0])); x=np.fromfile(raw,dtype='<f4').reshape(frames,diag['channels'])[:,0]
 if not np.isfinite(x).all(): raise AssertionError(label+' nonfinite')
 return x,diag,dict(raw_sha256=sha(raw),score_sha256=sha(score),rms=float(np.sqrt(np.mean(x.astype(float)**2))),peak=float(np.max(abs(x))),dc=float(np.mean(x)),max_jump=float(np.max(abs(np.diff(x)))))
def wav(path,x,rate=48000):
 peak=float(np.max(abs(x))); gain=.9/max(.9,peak)
 with wave.open(str(path),'wb') as f:
  f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate);f.writeframes(np.rint(np.clip(x*gain,-1,1)*32767).astype('<i2').tobytes())
def main(out):
 out.mkdir(parents=True,exist_ok=True); man=json.loads((MOD/'manifest.json').read_text()); patches=json.loads((MOD/'patches.json').read_text())['anchors']; defaults={k:v['default'] for k,v in man['controls'].items()}|{'gate':0.,'velocity':1.}
 report={'passed':False,'scope':man['status'],'builds':{},'checks':[],'renders':[],'listening':{},'limitations':man['acceptance_remaining']}
 def check(name,ok,**data): report['checks'].append({'name':name,'passed':bool(ok),**data}); assert ok,(name,data)
 scalar,b=build(out,'final-scalar',MOD/'kick.dsp'); report['builds']['final-scalar']=b
 vector,b=build(out,'final-vector',MOD/'kick.dsp',True); report['builds']['final-vector']=b
 baseline,b=build(out,'baseline',MOD/'candidates/body-01.dsp'); report['builds']['baseline']=b
 controls=(scalar.parent/'controls.tsv').read_text().splitlines(); names={r.split('\t')[0] for r in controls[1:]}
 check('control-contract',names==set(defaults),names=sorted(names)); check('mono-output',controls[0]=='io\t0\t1')
 # Sample-rate, segmentation, vector parity, velocity, release semantics.
 for rate in (44100,48000,96000):
  on=101; off=on+round(.08*rate); events=[(on,'gate',1.),(off,'gate',0.)]
  x,d,m=render(out,f'{rate}-base',scalar,defaults,events,rate); report['renders'].append({'label':f'{rate}-base',**m,'diag':d})
  check(f'{rate}:bounded',m['peak']<1.0 and abs(m['dc'])<.01,peak=m['peak'],dc=m['dc'])
  for block in (1,32,127,128,512):
   z,_,_=render(out,f'{rate}-block{block}',scalar,defaults,events,rate,block)
   check(f'{rate}:block{block}',np.max(abs(z-x))<1e-6,error=float(np.max(abs(z-x))))
  z,_,_=render(out,f'{rate}-vector',vector,defaults,events,rate,128)
  check(f'{rate}:vector',np.max(abs(z-x))<3e-4,error=float(np.max(abs(z-x))))
  h,_,_=render(out,f'{rate}-halfvel',scalar,defaults|{'velocity':.5},events,rate)
  check(f'{rate}:velocity',np.max(abs(h-.5*x))<3e-5,error=float(np.max(abs(h-.5*x))))
  held,_,_=render(out,f'{rate}-held',scalar,defaults,[(on,'gate',1.)],rate)
  check(f'{rate}:gateoff-trigger-only',np.max(abs(held[off:]-x[off:]))<2e-6,error=float(np.max(abs(held[off:]-x[off:]))))
  # 96k vs 48k deterministic nonlinear-body diagnostic. Click/noise is zero
  # here because independent random streams cannot be waveform-compared after
  # resampling and would turn this into a noise-seed test rather than HF evidence.
  if rate==96000:
   alias_params=defaults|{'tone':1.,'drive':1.,'click':0.,'body':1.}
   lo,_,_=render(out,'48000-alias-proxy',scalar,alias_params,[(101,'gate',1.)],48000)
   hi,_,_=render(out,'96000-alias-proxy',scalar,alias_params,[(101,'gate',1.)],96000)
   hi2=hi[::2][:len(lo)]; check('alias-proxy-bounded',np.sqrt(np.mean((lo-hi2)**2))<.18,rms_difference=float(np.sqrt(np.mean((lo-hi2)**2))))
 # Endpoint safety over all seven non-pitch timbre controls, alternating pitch endpoints.
 keys=[k for k in man['controls'] if k!='pitch_hz']; events=[]
 for i,bits in enumerate(itertools.product((0.,1.),repeat=len(keys))):
  n=101+i*2048
  events += [(n,k,v) for k,v in zip(keys,bits)]+[(n,'pitch_hz',20. if i%2==0 else 160.),(n,'gate',1.),(n+256,'gate',0.)]
 x,d,m=render(out,'corners',scalar,defaults,events,48000,127,seconds=(101+128*2048+48000)/48000); report['renders'].append({'label':'corners',**m,'diag':d}); check('corners-safe',m['peak']<1.25 and abs(m['dc'])<.02,peak=m['peak'],dc=m['dc'])
 # Retrigger stress with parameter locks.
 events=[]
 for i in range(96):
  n=100+i*480; p=list(patches.values())[i%4]
  events += [(n,k,v) for k,v in p.items()]+[(n,'velocity',.35+.65*((i%5)/4)),(n,'gate',1.),(n+120,'gate',0.)]
 x,d,m=render(out,'retrigger-stress',scalar,defaults,events,48000,32,2); report['renders'].append({'label':'retrigger-stress',**m,'diag':d}); check('retrigger-safe',m['peak']<1.25 and m['max_jump']<1.25,peak=m['peak'],max_jump=m['max_jump'])
 # Musical evidence: four anchors, fixed raw gain, no per-hit normalization.
 timeline=[]; events=[]
 for j,(name,p) in enumerate(patches.items()):
  start=round((j*1.5+.1)*48000); timeline.append({'name':name,'start_s':start/48000})
  for k,v in p.items(): events.append((start,k,v))
  events += [(start,'velocity',1.),(start,'gate',1.),(start+2400,'gate',0.)]
  second=start+round(.55*48000); events += [(second,'velocity',.55),(second,'gate',1.),(second+1800,'gate',0.)]
 x,d,m=render(out,'anchors',scalar,defaults,events,48000,128,6.2); wav(out/'analog-kick-anchors.wav',x); report['renders'].append({'label':'anchors',**m,'diag':d}); report['listening']['analog-kick-anchors.wav']=timeline
 # Cost evidence versus the deliberately simpler clean-body baseline.
 ev=[(101,'gate',1.),(2501,'gate',0.)]
 _,db,_=render(out,'perf-baseline',baseline,{'pitch_hz':52.,'decay':.52,'sweep':.48,'punch':.55,'gate':0.,'velocity':1.},ev,48000,128,4)
 _,df,_=render(out,'perf-final',scalar,defaults,ev,48000,128,4)
 report['performance']={'baseline_compute_ns':db['instrumented_compute_ns'],'final_compute_ns':df['instrumented_compute_ns'],'final_over_baseline':df['instrumented_compute_ns']/max(1,db['instrumented_compute_ns']),'note':'hosted offline instrumentation; not target realtime qualification'}
 check('cost-bounded-vs-baseline',report['performance']['final_over_baseline']<8.0,ratio=report['performance']['final_over_baseline'])
 report['passed']=True; (out/'report.json').write_text(json.dumps(report,indent=2)); print(json.dumps({'passed':True,'checks':len(report['checks']),'renders':len(report['renders']),'performance':report['performance']},indent=2))
if __name__=='__main__':
 ap=argparse.ArgumentParser(); ap.add_argument('--out',type=Path,default=ROOT/'build/kick-analog'); a=ap.parse_args(); main(a.out.resolve())
