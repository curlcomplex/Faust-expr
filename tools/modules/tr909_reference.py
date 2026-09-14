"""TR909 recording acquisition and baseline descriptors. No hardware WAV redistribution.
Uses the existing SynthLab/Faust renderer; no Python-generated instrument audio.
The normalized 1995 Rob Roy recordings constrain shape, not absolute gain.
"""
from pathlib import Path
import argparse,hashlib,io,json,math,os,re,subprocess,tempfile,urllib.request
import numpy as np
from scipy.io import wavfile
from scipy.signal import welch,find_peaks,resample_poly
from synth_batch import SynthLab
from drums909_batch import DEFAULTS
from hats_v2_delivery import digest
ROOT=Path(__file__).resolve().parents[2]
URL='https://mirrors.xmission.com/aminet/mods/smpl/tr-909.lha'
ANCHORS={'kick':'BT3A0D7.WAV','snare':'ST3T3S3.WAV','low-tom':'LT3D3.WAV','mid-tom':'MT3D3.WAV','high-tom':'HT3D3.WAV','rim':'RIM127.WAV','clap':'HANDCLP1.WAV'}
TONAL={'kick','low-tom','mid-tom','high-tom','rim'}
def mono(x):
 x=np.asarray(x)
 if np.issubdtype(x.dtype,np.integer):x=x.astype(float)/max(abs(np.iinfo(x.dtype).min),np.iinfo(x.dtype).max)
 else:x=x.astype(float)
 return x.mean(axis=1) if x.ndim==2 else x

def features(x,sr):
 x=mono(x)
 if not len(x) or not np.isfinite(x).all():return {'silent':True}
 peak=float(abs(x).max())
 if peak<1e-10:return {'silent':True,'peak':peak}
 # Fixed 0.5%-of-peak onset with 1ms pre-roll. No normalization of output audio.
 on=np.flatnonzero(abs(x)>peak*.005);start=max(0,int(on[0])-round(.001*sr));x=x[start:]
 x=x[:sr*4];energy=x*x;total=energy.sum();cum=np.cumsum(energy)/total
 out={'peak':peak,'rms':float(np.sqrt(energy.mean())),'dc':float(x.mean()),'trim_sample':start,'rate':sr,'duration':len(x)/sr,'silent':False}
 for p in (10,50,90,99):out['t'+str(p)]=float(np.searchsorted(cum,p/100)/sr)
 edges=[0,80,160,320,640,1280,2560,5120,10240,24001]
 for label,a,b in [('attack',0,.025),('body',.025,.180),('tail',.180,.5)]:
  y=x[round(a*sr):round(b*sr)];y=np.pad(y,(0,max(0,round((b-a)*sr)-len(y))))
  f,p=welch(y,sr,nperseg=min(2048,len(y)),nfft=8192);s=float(p.sum())+1e-30
  peaks,_=find_peaks(p);peaks=sorted(peaks,key=lambda k:p[k],reverse=True)[:8]
  low=(f>=30)&(f<=3500)
  out[label]={'centroid':float((f*p).sum()/s),'bands':[float(p[(f>=lo)&(f<hi)].sum()/s) for lo,hi in zip(edges,edges[1:])],'peaks':[[float(f[k]),float(p[k]/s)] for k in peaks], 'tonal_peak':float(f[low][np.argmax(p[low])]),'energy_fraction':float((y*y).sum()/total)}
 return out

def components(a,b,tonal=False):
 if b.get('silent',False):return {'silent':1e6}
 out={k+'_log_error':abs(math.log(max(1e-5,b[k])/max(1e-5,a[k]))) for k in ('t50','t90','t99')}
 for s in ('attack','body'):
  out[s+'_spectral']=float(np.linalg.norm(np.sqrt(a[s]['bands'])-np.sqrt(b[s]['bands']))/math.sqrt(2))
  out[s+'_centroid']=abs(math.log(max(1,b[s]['centroid'])/max(1,a[s]['centroid'])))
 if tonal:out['body_pitch']=abs(math.log(b['body']['tonal_peak']/a['body']['tonal_peak']))
 return out

def loss(a,b,tonal=False):
 m=components(a,b,tonal)
 if 'silent' in m:return 1e6
 return sum((.4 if k.startswith(('t50','t90','t99')) else .2 if k.endswith('centroid') else .7 if k=='body_pitch' else 1)*v for k,v in m.items())/(3*.4+2*.2+2+(.7 if tonal else 0))

def acquire(expected=None):
 req=urllib.request.Request(URL,headers={'User-Agent':'Faust-expr-reference-research/1.0'})
 with urllib.request.urlopen(req,timeout=25) as r:data=r.read(12_000_001)
 if len(data)>12_000_000:raise ValueError('Reference archive size bound')
 archive_sha=hashlib.sha256(data).hexdigest()
 if expected and archive_sha!=expected:raise ValueError('Reference archive changed')
 report={'url':URL,'archive_sha256':archive_sha,'archive_bytes':len(data),'recordings':{},'documents':{},'raw_audio_retained':False}
 with tempfile.TemporaryDirectory() as td:
  p=Path(td)/'original.lha';p.write_bytes(data)
  listing=subprocess.check_output(['lha','l',str(p)],text=True,timeout=20)
  # Read exact whitelisted members to stdout, never extract arbitrary paths to disk.
  names=[]
  for line in listing.splitlines():
   q=line.split()[-1] if line.split() else ''
   if re.fullmatch(r'[A-Za-z0-9_./-]+\.(wav|txt)',q,re.I) and not q.startswith('/') and '..' not in q.split('/'):names.append(q)
  if not 100<=len(names)<=180:raise ValueError('Unexpected archive manifest '+listing)
  for member in names:
   b=subprocess.check_output(['lha','pq',str(p),member],timeout=15)
   if len(b)>2_000_000:raise ValueError('Member size bound')
   name=Path(member).name.upper();h=hashlib.sha256(b).hexdigest()
   if name.endswith('.TXT'):
    text=b.decode('latin1');report['documents'][name]={'sha256':h,'bytes':len(b),'text':text};continue
   sr,x=wavfile.read(io.BytesIO(b))
   if sr!=44100 or x.dtype!=np.int16 or x.ndim!=1:raise ValueError('Unexpected original PCM format '+name)
   report['recordings'][name]={'member':member,'sha256':h,'frames':len(x),'rate':sr,'features':features(x,sr)}
 report['unique_recordings']=len(set(r['sha256'] for r in report['recordings'].values()))
 if not set(ANCHORS.values())<=report['recordings'].keys():raise ValueError('Missing 909 anchors')
 return report

def source(name):return ROOT/'modules/drums-909/v1'/('tom.dsp' if name.endswith('-tom') else name+'.dsp')
def run(out):
 L=SynthLab(out);(L.out/'audition').mkdir(exist_ok=True);report={'commit':os.getenv('GITHUB_SHA'),'references':{},'baseline':{},'status':'acquisition/baseline only; no tuned module approval'}
 try:
  report['references']=acquire();ex={}
  (L.out/'reference-index.json').write_text(json.dumps(report['references'],indent=2))
  for name,ref in ANCHORS.items():
   src=source(name)
   if src not in ex:ex[src]=L.build('baseline-'+src.stem,src)
   y=L.render('baseline-'+name,ex[src],DEFAULTS[name],[(4800,'gate',1),(4801,'gate',0)],frames=192000)[:,0]
   b=features(y,48000);a=report['references']['recordings'][ref]['features']
   report['baseline'][name]={'source':str(src.relative_to(ROOT)),'source_sha256':digest(src),'settings':DEFAULTS[name],'reference':ref,'features':b,'loss':loss(a,b,name in TONAL),'component_errors':components(a,b,name in TONAL)}
   L.wav(name+'_baseline.wav',y)
   print(name,json.dumps(report['baseline'][name]),flush=True)
  report['passed']=True
 except Exception as e:report.update(passed=False,error=repr(e),output=getattr(e,'output',None));raise
 finally:
  report['lab']=L.report;(L.out/'baseline.json').write_text(json.dumps(report,indent=2));print(json.dumps({'passed':report.get('passed'),'count':len(report['references'].get('recordings',{})),'error':report.get('error')}),flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
