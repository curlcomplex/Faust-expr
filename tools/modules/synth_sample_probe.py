"""Bounded sample spectral probes. Not whole-synth/circuit calibration.
Source WAVs remain in memory. All generated audio comes from actual Faust.
Normalized spectra do not establish envelopes, gain or hardware knob mappings.
"""
from pathlib import Path
import argparse, hashlib, html, io, json, os, re, tempfile, urllib.request, zipfile
import numpy as np
import soundfile as sf
from scipy.signal import correlate, find_peaks
from synth_batch import SynthLab
from synth_oracle_candidates import PAIRS, BASE
from hats_v2_delivery import command, digest

ARCHIVES={
 'j60':('https://files.scene.org/get/mirrors/hornet/music/samples/aq-j60v1.zip','f04faa458e81d345213faffab6ca7275597f5f1e37719a8bf661a5b2260a8b4c'),
 'j106':('https://files.scene.org/get/mirrors/hornet/music/samples/swjuno2.zip','b671f055d388d739a65bb4ec0279d3eabd064e729212dda19d93d25fbc69f5f9'),
}
def sha(b):return hashlib.sha256(b).hexdigest()
def fetch(url,limit=40000000):
 req=urllib.request.Request(url,headers={'User-Agent':'Mozilla/5.0 Faust-expr reference study'})
 with urllib.request.urlopen(req,timeout=25) as r:data=r.read(limit+1)
 if len(data)>limit:raise RuntimeError('download byte limit')
 return data

def read_wav(raw):
 # Float conversion precedes channel averaging, including PCM8/24 input.
 x,sr=sf.read(io.BytesIO(raw),dtype='float64',always_2d=True)
 if len(x)<64 or not np.isfinite(x).all():raise ValueError('invalid audio')
 return sr,x.mean(axis=1)

def estimate_pitch(x,sr):
 y=x-np.mean(x);ac=correlate(y,y,mode='full',method='fft')[len(y)-1:]
 ac=ac/np.arange(len(y),0,-1);ac/=max(ac[0],1e-20)
 lo=max(2,int(sr/2500));hi=min(len(y)//3,int(sr/35))
 peaks,_=find_peaks(ac[lo:hi]);peaks=peaks+lo
 if len(peaks)==0:raise ValueError('no periodicity peak')
 best=float(np.max(ac[peaks]));valid=peaks[ac[peaks]>=max(.70,.96*best)]
 if len(valid)==0:raise ValueError('insufficient periodicity')
 k=int(valid[0]);den=ac[k-1]-2*ac[k]+ac[k+1]
 frac=.5*(ac[k-1]-ac[k+1])/den if abs(den)>1e-15 else 0
 return float(sr/(k+np.clip(frac,-.5,.5))),best

def spectrum(x,sr,f0):
 y=np.asarray(x,float)-np.mean(x);n=len(y)
 if n<64 or np.linalg.norm(y)<1e-10:raise ValueError('silent/short spectrum')
 p=abs(np.fft.rfft(y*np.hanning(n)))**2;hz=np.fft.rfftfreq(n,1/sr)
 v=np.array([p[(hz>(h-.35)*f0)&(hz<(h+.35)*f0)].sum() for h in range(1,25)])
 if v.sum()<1e-20:raise ValueError('no harmonic energy')
 v=v/v.sum();return np.maximum(-60,10*np.log10(np.maximum(v,1e-12)))

def reference_window(raw):
 sr,x=read_wav(raw);ix=np.flatnonzero(abs(x)>.03*np.max(abs(x)))
 if not len(ix):raise ValueError('silent reference')
 onset=int(ix[0]);a=onset+round(.12*sr);b=a+round(.24*sr)
 if b>len(x):raise ValueError('too short for fixed window')
 w=x[a:b];f0,q=estimate_pitch(w,sr)
 left,_=estimate_pitch(w[:len(w)//2],sr);right,_=estimate_pitch(w[len(w)//2:],sr)
 if abs(np.log2(left/right))>.06:raise ValueError('pitch changes in window')
 return spectrum(w,sr,f0),dict(sample_rate=sr,frames=len(x),onset_frame=onset,window_frames=[a,b],estimated_frequency_hz=f0,periodicity=q,half_window_estimates=[left,right],peak=float(np.max(abs(x))),reference_kind='recorded note window; controls/chorus not inferred')

def run(out):
 L=SynthLab(out);L.report['presets']={}
 L.report.update(version='sample-spectral-probe-1',commit=os.getenv('GITHUB_SHA','local'),hardware_approved=False,selected_for_promotion=False,reference_results=[],acquisition={},limitations=['Spectrum-only fitting; envelopes not fitted.','Hardware knob settings are incomplete. Juno60 chorus state is unknown.','Distances are not authenticity percentages.','Single-cycle captures cannot calibrate note contours or original gain.'])
 (L.out/'audition').mkdir(exist_ok=True);targets=[]
 for key,(url,expected) in ARCHIVES.items():
  try:
   raw=fetch(url)
   if sha(raw)!=expected:raise ValueError('archive hash mismatch')
   z=zipfile.ZipFile(io.BytesIO(raw));L.report['acquisition'][key]=dict(url=url,sha256=sha(raw),bytes=len(raw),wav_count=sum(n.lower().endswith('.wav') for n in z.namelist()))
   selected=['52-lo.wav','64-lo.wav'] if key=='j60' else ['JUNO2-3.WAV','JUNO2-4.WAV']
   for fn in selected:
    try:
     data=z.read(fn);feat,info=reference_window(data);info.update(file=fn,wav_sha256=sha(data),archive=key)
     targets.append(('juno60' if key=='j60' else 'juno106',fn,feat,info))
    except Exception as e:L.report['reference_results'].append(dict(file=fn,skipped=str(e)))
  except Exception as e:L.report['acquisition'][key]=dict(url=url,error=str(e))
 # One attempt at the publisher-linked public download button, no gate bypass.
 try:
  pageurl='https://www.mediafire.com/?b0v9zou2dvd4qpv';text=fetch(pageurl,3000000).decode('utf-8','replace');link=None
  for tag in re.findall(r'<a\b[^>]*>',text,re.I):
   if re.search(r'id=[\"\']downloadButton[\"\']',tag,re.I):
    m=re.search(r'href=[\"\']([^\"\']+)',tag,re.I)
    if m:link=html.unescape(m.group(1));break
  if not link or not link.startswith('https://'):raise ValueError('public download button unavailable')
  data=fetch(link);z=zipfile.ZipFile(io.BytesIO(data));names=[n for n in z.namelist() if n.lower().endswith('.wav') and not n.startswith('__MACOSX/')]
  L.report['acquisition']['dms']=dict(publisher='https://www.dancemidisamples.com/download-free-analogue-synthesizer-wavetables-from-dancemidisamples/',landing_page=pageurl,archive_sha256=sha(data),bytes=len(data),wav_files=names,scope='Publisher-described single-cycle hardware captures; original capture pitch/gain/settings unknown.')
  for name,pattern in [('mono101',r'sh[ _-]?101'),('mini',r'mini[ _-]?moog|mini[ _-]?mo?g')]:
   choices=[n for n in names if re.search(pattern,n,re.I) and re.search('saw',n,re.I)]
   if not choices:
    L.report['reference_results'].append(dict(instrument=name,skipped='No unambiguous named saw capture in DMS archive'));continue
   fn=sorted(choices)[0];raw=z.read(fn);sr,x=read_wav(raw)
   if len(x)>8192:raise ValueError('unexpected single-cycle length')
   mag=abs(np.fft.rfft(x-x.mean()))**2;v=mag[1:25];v=np.pad(v,(0,max(0,24-len(v))));v=v/max(v.sum(),1e-20)
   feat=np.maximum(-60,10*np.log10(np.maximum(v,1e-12)))
   targets.append((name,fn,feat,dict(file=fn,wav_sha256=sha(raw),archive='dms',frames=len(x),sample_rate=sr,estimated_frequency_hz=220.,reference_kind='single-cycle shape only; 220 Hz is chosen playback, not original capture pitch')))
 except Exception as e:L.report['acquisition']['dms']=dict(error=str(e),status='bounded download stopped; not acquired')
 exes={}
 for name,pair in PAIRS.items():
  for label,src in zip(('old','candidate'),pair):exes[name,label]=L.build(name+'-'+label,src)
 tried=[]
 with tempfile.TemporaryDirectory(prefix='synth-exploration-') as tmp:
  tmp=Path(tmp);score=tmp/'score.tsv';audio=tmp/'audio.f32'
  for name,fn,feat,info in targets:
   f0=info['estimated_frequency_hz'];result=dict(instrument=name,reference=info,target_harmonic_db=feat.tolist(),versions={})
   for label in ('old','candidate'):
    base=dict(BASE[name]);base.update(freq=min(8000,max(20,f0)),gate=0,attack=.003,sustain=1,release=.15,noise=0)
    if name=='mini':base.update(osc1=1,osc2=0,osc3=0,contour=0,drive=0,emphasis=.707)
    else:
     base.update(saw=1,pulse=0,sub=0,pwmDepth=0,filterEnv=0,resonance=0)
     if name.startswith('juno'):base['hpf']=20
    trials=[dict(base,cutoff=16000)];rng=np.random.default_rng(600106)
    for i in range(32):
     cf=float(2**rng.uniform(np.log2(max(100,f0)),np.log2(16000)))
     p=dict(base,cutoff=cf)
     if name=='mini':p.update(emphasis=float(rng.uniform(.707,8)),drive=float(rng.uniform(0,.5)))
     else:p.update(resonance=float(rng.uniform(0,.85)),pulse=float(rng.uniform(0,1)),saw=float(rng.uniform(0,1)),pwm=float(rng.uniform(.12,.88)))
     trials.append(p)
    best=None
    for i,p in enumerate(trials):
     rows={(0,k):v for k,v in p.items()};rows[4800,'gate']=1
     score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
     command([exes[name,label],score,audio,48000,128,28800,0]);x=np.fromfile(audio,'<f4')
     got=spectrum(x[14400:25920],48000,f0);loss=float(np.sqrt(np.mean((got-feat)**2)))
     record=dict(instrument=name,file=fn,version=label,index=i,loss_db=loss,controls=p,output_sha256=sha(audio.read_bytes()));tried.append(record)
     if best is None or loss<best['loss_db']:best=record
    tag=name+'-'+str(len(L.report['reference_results']))+'-'+label
    y=L.render(tag,exes[name,label],best['controls'],[(4800,'gate',1),(36000,'gate',0)],frames=72000);L.wav(tag+'.wav',y)
    result['versions'][label]=best
   result['candidate_minus_old_loss_db']=result['versions']['candidate']['loss_db']-result['versions']['old']['loss_db']
   result['interpretation']='Spectral distance only. Equal 33-setting budgets. Neither version promoted; envelopes not compared.'
   L.report['reference_results'].append(result)
 L.report['exploratory_native_renders']=len(tried);L.report['trial_log']=tried
 L.report['coverage_complete']=set(t[0] for t in targets)==set(PAIRS)
 L.report['passed']=bool(targets) and not any(q.get('passed') is not True for q in L.report['checks'])
 L.report['sources']={n:{label:dict(path=str(src),sha256=digest(src)) for label,src in zip(('old','candidate'),pair)} for n,pair in PAIRS.items()}
 (L.out/'results.json').write_text(json.dumps(L.report,indent=2))
 print(json.dumps({'targets':len(targets),'trials':len(tried),'coverage_complete':L.report['coverage_complete'],'acquisition':L.report['acquisition'],'results':[{k:v for k,v in r.items() if k not in ('versions','target_harmonic_db')} for r in L.report['reference_results']]},indent=2))
 return 0 if L.report['passed'] else 1

if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();raise SystemExit(run(a.out))
