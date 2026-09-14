"""Bounded hardware-sample spectral probes for Analog Classics synths.

Reference WAVs remain transient. The probe deliberately selects stable periodic
windows from entire archives instead of assuming a named preset is suitable for
oscillator/filter comparison. Normalized spectra are diagnostics only: they do
not establish envelope timing, absolute gain, knob mapping, or authenticity.
"""
from pathlib import Path
import argparse, hashlib, io, json, os, tempfile, urllib.request, zipfile
import numpy as np
import soundfile as sf
from scipy.signal import correlate, find_peaks
from synth_batch import SynthLab
from synth_oracle_candidates import PAIRS, BASE
from hats_v2_delivery import command, digest

ARCHIVES={
 'j60':dict(url='https://files.scene.org/get/mirrors/hornet/music/samples/aq-j60v1.zip',sha='f04faa458e81d345213faffab6ca7275597f5f1e37719a8bf661a5b2260a8b4c',instrument='juno60'),
 'j106':dict(url='https://files.scene.org/get/mirrors/hornet/music/samples/swjuno2.zip',sha='b671f055d388d739a65bb4ec0279d3eabd064e729212dda19d93d25fbc69f5f9',instrument='juno106'),
 'sh101':dict(url='https://cdn.mos.musicradar.com/audio/samples/musicradar-roland-sh101-samples.zip',sha='949bf42a6af79efdc40bb9d99113ae28ed10f38c2f3b2dc6e6fcbcc18a7cdd45',instrument='mono101'),
}
EXTERNAL={
 'mini':{
  'url':'https://monosounds.studio/product/100-minimoog-model-d-samples-vol-1/',
  'note':'Real Model D one-shots are purchase/download gated at present; use as external listening corpus only until legitimately acquired. Faug and the licensed ladder component remain software/architecture oracles, not hardware truth.'
 }
}

def sha(b): return hashlib.sha256(b).hexdigest()
def fetch(url,limit=350_000_000):
 req=urllib.request.Request(url,headers={'User-Agent':'Mozilla/5.0 Faust-expr reference study'})
 with urllib.request.urlopen(req,timeout=60) as r:data=r.read(limit+1)
 if len(data)>limit: raise RuntimeError('download byte limit')
 return data

def read_wav(raw):
 x,sr=sf.read(io.BytesIO(raw),dtype='float64',always_2d=True)
 if len(x)<64 or not np.isfinite(x).all(): raise ValueError('invalid audio')
 return int(sr),x.mean(axis=1)

def estimate_pitch(x,sr):
 y=np.asarray(x,float)-np.mean(x)
 if np.linalg.norm(y)<1e-10: raise ValueError('silent window')
 ac=correlate(y,y,mode='full',method='fft')[len(y)-1:]
 ac=ac/np.arange(len(y),0,-1); ac/=max(ac[0],1e-20)
 lo=max(2,int(sr/2500)); hi=min(len(y)//3,int(sr/35))
 peaks,_=find_peaks(ac[lo:hi]); peaks=peaks+lo
 if not len(peaks): raise ValueError('no periodicity peak')
 best=float(np.max(ac[peaks])); valid=peaks[ac[peaks]>=max(.70,.955*best)]
 if not len(valid): raise ValueError('insufficient periodicity')
 k=int(valid[0]); den=ac[k-1]-2*ac[k]+ac[k+1]
 frac=.5*(ac[k-1]-ac[k+1])/den if abs(den)>1e-15 else 0
 return float(sr/(k+np.clip(frac,-.5,.5))),best

def spectrum(x,sr,f0):
 y=np.asarray(x,float)-np.mean(x); n=len(y)
 if n<64 or np.linalg.norm(y)<1e-10: raise ValueError('silent/short spectrum')
 p=abs(np.fft.rfft(y*np.hanning(n)))**2; hz=np.fft.rfftfreq(n,1/sr)
 v=np.array([p[(hz>(h-.35)*f0)&(hz<(h+.35)*f0)].sum() for h in range(1,25)])
 if v.sum()<1e-20: raise ValueError('no harmonic energy')
 v=v/v.sum(); return np.maximum(-60,10*np.log10(np.maximum(v,1e-12)))

def candidate_windows(raw):
 sr,x=read_wav(raw); peak=float(np.max(np.abs(x)))
 if peak<=1e-8: return []
 ix=np.flatnonzero(abs(x)>.03*peak)
 if not len(ix): return []
 onset=int(ix[0]); win=round(.24*sr)
 starts=[onset+round(t*sr) for t in (.08,.16,.28,.45,.70,1.0)]
 rows=[]
 for a in starts:
  b=a+win
  if b>len(x): continue
  w=x[a:b]
  try:
   f0,q=estimate_pitch(w,sr); left,ql=estimate_pitch(w[:len(w)//2],sr); right,qr=estimate_pitch(w[len(w)//2:],sr)
   drift=abs(np.log2(left/right))
   if q<.74 or min(ql,qr)<.68 or drift>.035 or not 35<=f0<=2500: continue
   rows.append((q-.5*drift,a,b,f0,q,left,right,drift,spectrum(w,sr,f0)))
  except Exception: pass
 return sorted(rows,key=lambda r:r[0],reverse=True)

def select_archive_targets(raw,key,instrument,max_targets=3):
 z=zipfile.ZipFile(io.BytesIO(raw)); candidates=[]
 for fn in z.namelist():
  if not fn.lower().endswith(('.wav','.wave')) or fn.startswith('__MACOSX/'): continue
  try:
   data=z.read(fn); wins=candidate_windows(data)
   if wins:
    score,a,b,f0,q,left,right,drift,feat=wins[0]
    candidates.append((score,fn,data,a,b,f0,q,left,right,drift,feat))
  except Exception: pass
 selected=[]
 for row in sorted(candidates,key=lambda r:r[0],reverse=True):
  f0=row[5]
  if any(abs(np.log2(f0/s[5]))<.32 for s in selected): continue
  selected.append(row)
  if len(selected)>=max_targets: break
 if not selected and candidates: selected=[max(candidates,key=lambda r:r[0])]
 targets=[]
 for score,fn,data,a,b,f0,q,left,right,drift,feat in selected:
  sr,x=read_wav(data)
  info=dict(sample_rate=sr,frames=len(x),window_frames=[a,b],estimated_frequency_hz=f0,periodicity=q,half_window_estimates=[left,right],pitch_drift_octaves=drift,peak=float(np.max(abs(x))),file=fn,wav_sha256=sha(data),archive=key,reference_kind='automatically selected stable periodic hardware-note window; panel controls/chorus not inferred')
  targets.append((instrument,fn,feat,info))
 return targets,dict(total_periodic_candidates=len(candidates),selected_files=[t[1] for t in targets])

def run(out):
 L=SynthLab(out); L.report['presets']={}
 L.report.update(version='sample-spectral-probe-3',commit=os.getenv('GITHUB_SHA','local'),hardware_approved=False,selected_for_promotion=False,reference_results=[],acquisition={},external_references=EXTERNAL,limitations=['Stable-note spectra only; envelopes and absolute levels are not fitted.','Legacy archive panel settings are incomplete; chorus/filter state may be unknown.','Distances are not authenticity percentages.','Automatic selection is frozen only after archive hashes and chosen windows are reported.'])
 (L.out/'audition').mkdir(exist_ok=True); targets=[]
 for key,spec in ARCHIVES.items():
  try:
   raw=fetch(spec['url']); got=sha(raw)
   if spec['sha'] and got!=spec['sha']: raise ValueError(f'archive hash mismatch {got}')
   chosen,scan=select_archive_targets(raw,key,spec['instrument'])
   z=zipfile.ZipFile(io.BytesIO(raw))
   L.report['acquisition'][key]=dict(url=spec['url'],sha256=got,bytes=len(raw),wav_count=sum(n.lower().endswith(('.wav','.wave')) for n in z.namelist()),**scan)
   targets.extend(chosen)
  except Exception as e:L.report['acquisition'][key]=dict(url=spec['url'],error=str(e))

 exes={}
 for name,pair in PAIRS.items():
  for label,src in zip(('old','candidate'),pair): exes[name,label]=L.build(name+'-'+label,src)
 tried=[]
 with tempfile.TemporaryDirectory(prefix='synth-exploration-') as tmp:
  tmp=Path(tmp); score=tmp/'score.tsv'; audio=tmp/'audio.f32'
  for name,fn,feat,info in targets:
   f0=info['estimated_frequency_hz']; result=dict(instrument=name,reference=info,target_harmonic_db=feat.tolist(),versions={})
   for label in ('old','candidate'):
    base=dict(BASE[name]); base.update(freq=min(8000,max(20,f0)),gate=0,attack=.003,sustain=1,release=.15,noise=0)
    if name=='mini': base.update(osc1=1,osc2=0,osc3=0,contour=0,drive=0,emphasis=.707)
    else:
     base.update(saw=1,pulse=0,sub=0,pwmDepth=0,filterEnv=0,resonance=0)
     if name.startswith('juno'): base['hpf']=20
    trials=[dict(base,cutoff=16000)]; rng=np.random.default_rng(600106+int(round(f0)))
    for i in range(32):
     cf=float(2**rng.uniform(np.log2(max(100,f0)),np.log2(16000))); p=dict(base,cutoff=cf)
     if name=='mini': p.update(emphasis=float(rng.uniform(.707,8)),drive=float(rng.uniform(0,.5)))
     else: p.update(resonance=float(rng.uniform(0,.85)),pulse=float(rng.uniform(0,1)),saw=float(rng.uniform(0,1)),pwm=float(rng.uniform(.12,.88)))
     trials.append(p)
    best=None
    for i,p in enumerate(trials):
     rows={(0,k):v for k,v in p.items()}; rows[4800,'gate']=1
     score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
     command([exes[name,label],score,audio,48000,128,28800,0]); x=np.fromfile(audio,'<f4')
     got=spectrum(x[14400:25920],48000,f0); loss=float(np.sqrt(np.mean((got-feat)**2)))
     record=dict(instrument=name,file=fn,version=label,index=i,loss_db=loss,controls=p,output_sha256=sha(audio.read_bytes())); tried.append(record)
     if best is None or loss<best['loss_db']: best=record
    tag=f'{name}-{len(L.report["reference_results"])}-{label}'
    y=L.render(tag,exes[name,label],best['controls'],[(4800,'gate',1),(36000,'gate',0)],frames=72000); L.wav(tag+'.wav',y)
    result['versions'][label]=best
   result['candidate_minus_old_loss_db']=result['versions']['candidate']['loss_db']-result['versions']['old']['loss_db']
   result['interpretation']='Harmonic-spectrum distance only, equal 33-setting budgets. Negative favors candidate. Neither version promoted; envelopes/gain are not compared.'
   L.report['reference_results'].append(result)
 L.report['exploratory_native_renders']=len(tried); L.report['trial_log']=tried
 represented=set(t[0] for t in targets); L.report['represented_instruments']=sorted(represented); L.report['missing_instruments']=sorted(set(PAIRS)-represented)
 L.report['passed']=bool(targets) and not any(q.get('passed') is not True for q in L.report['checks'])
 L.report['sources']={n:{label:dict(path=str(src),sha256=digest(src)) for label,src in zip(('old','candidate'),pair)} for n,pair in PAIRS.items()}
 (L.out/'results.json').write_text(json.dumps(L.report,indent=2))
 print(json.dumps({'targets':len(targets),'trials':len(tried),'represented':sorted(represented),'missing':L.report['missing_instruments'],'acquisition':L.report['acquisition'],'comparisons':[{'instrument':r.get('instrument'),'file':r.get('reference',{}).get('file'),'candidate_minus_old_loss_db':r.get('candidate_minus_old_loss_db')} for r in L.report['reference_results']]},indent=2))
 return 0 if L.report['passed'] else 1

if __name__=='__main__':
 ap=argparse.ArgumentParser(); ap.add_argument('--out',type=Path,required=True); a=ap.parse_args(); raise SystemExit(run(a.out))
