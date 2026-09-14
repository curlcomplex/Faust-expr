"""Bounded public-reference discovery; no reference audio is republished.
Actual candidate synthesis uses the existing Faust/C++ Lab and render.cpp.
"""
from pathlib import Path
import argparse,hashlib,html,io,json,os,re,urllib.request,urllib.parse,zipfile
import numpy as np
from scipy.io import wavfile
from scipy.signal import welch,find_peaks
from synth_batch import SynthLab
from drums606_batch import DEFAULTS
ROOT=Path(__file__).resolve().parents[2]
PAGES={
 'wavealchemy':'https://www.wavealchemy.co.uk/product-tag/free-products/',
 'rolandclan':'https://www.rolandclan.com/library/tr-606/',
 'aminet':'https://mirrors.xmission.com/aminet/mods/smpl/tr-606.readme',
}
def sha(data):return hashlib.sha256(data).hexdigest()
def fetch(url,limit=80_000_000):
 req=urllib.request.Request(url,headers={'User-Agent':'Faust-expr-reference-lab/1.0'})
 with urllib.request.urlopen(req,timeout=18) as r:
  data=r.read(limit+1)
  if len(data)>limit:raise ValueError('download exceeds bound')
  return data,r.geturl()
def features(x,sr):
 if np.issubdtype(x.dtype,np.integer):x=x.astype(float)/(2**(np.iinfo(x.dtype).bits-1))
 else:x=x.astype(float)
 if x.ndim==2:x=x.mean(axis=1)
 peak=float(abs(x).max());rms=float(np.sqrt(np.mean(x*x)))
 if peak<1e-10:return {'peak':peak,'rms':rms,'silent':True}
 active=np.flatnonzero(abs(x)>peak*.005);start=max(0,int(active[0])-round(.001*sr));y=x[start:min(len(x),start+4*sr)]
 energy=np.cumsum(y*y);energy/=energy[-1]+1e-30
 result={'peak':peak,'rms':rms,'dc':float(x.mean()),'seconds':len(x)/sr,'onset_sample':start,'rate':sr,
 't50':float(np.searchsorted(energy,.5)/sr),'t90':float(np.searchsorted(energy,.9)/sr),'t99':float(np.searchsorted(energy,.99)/sr)}
 # Fixed windows expose evolution; bins are normalized only for shape diagnosis.
 for name,a,b in [('attack',0,.025),('body',.025,.15),('tail',.15,.65)]:
  z=y[round(a*sr):min(len(y),round(b*sr))]
  if len(z)<32:continue
  f,p=welch(z,sr,nperseg=min(4096,len(z)),nfft=8192)
  p/=p.sum()+1e-30;edges=[0,100,250,500,1000,2500,5000,10000,sr/2+1]
  ix,_=find_peaks(p);ix=sorted(ix,key=lambda i:p[i],reverse=True)[:8]
  result[name]={'centroid':float(f@p),'bands':[float(p[(f>=a)&(f<b)].sum()) for a,b in zip(edges,edges[1:])],
   'peaks':[[float(f[i]),float(p[i])] for i in ix], 'rms':float(np.sqrt(np.mean(z*z)))}
 return result

def run(out):
 out=Path(out);out.mkdir(parents=True,exist_ok=True);L=SynthLab(out);(out/'audition').mkdir(exist_ok=True)
 report={'commit':os.getenv('GITHUB_SHA','unknown'),'pages':{},'archives':{},'baseline':{},'reference_audio_redistributed':False}
 try:
  for key,url in PAGES.items():
   print('REFERENCE PAGE',key,flush=True)
   try:
    data,final=fetch(url,4_000_000);text=data.decode('utf-8','replace');(out/(key+'-page.txt')).write_text(text)
    links=[urllib.parse.urljoin(final,html.unescape(x)) for x in re.findall(r'href=[\"\']([^\"\']+)',text,re.I)]
    zips=sorted(set(x for x in links if '.zip' in x.lower() and '606' in x.lower()))
    interesting=sorted(set(x for x in links if any(s in x.lower() for s in ('606','download','free-sample'))))
    report['pages'][key]={'url':url,'final_url':final,'sha256':sha(data),'zip_links':zips,'related_links':interesting,'text_excerpt':re.sub('<[^>]+>',' ',text)[-2500:]}
    print(key,json.dumps(report['pages'][key]),flush=True)
    for zurl in zips[:2]:
     try:
      zd,zfinal=fetch(zurl);archive=zipfile.ZipFile(io.BytesIO(zd));members=archive.infolist()
      if sum(i.file_size for i in members)>150_000_000:raise ValueError('archive expansion bound')
      info={'url':zurl,'final_url':zfinal,'sha256':sha(zd),'bytes':len(zd),'members':[i.filename for i in members],'documents':{},'recordings':{}}
      for i in members:
       if i.filename.lower().endswith(('.txt','.md','.rtf')) and i.file_size<100_000:info['documents'][i.filename]=archive.read(i).decode('utf-8','replace')
       if i.filename.lower().endswith('.wav') and i.file_size<5_000_000 and '__MACOSX' not in i.filename:
        b=archive.read(i);sr,x=wavfile.read(io.BytesIO(b));info['recordings'][i.filename]={'sha256':sha(b),'dtype':str(x.dtype),'shape':list(x.shape),'features':features(x,sr)}
      report['archives'][key]=info;print('ARCHIVE',key,len(info['recordings']),flush=True)
     except Exception as e:report['archives'][key]={'url':zurl,'error':repr(e)}
   except Exception as e:report['pages'][key]={'url':url,'error':repr(e)}
  # Six engines: low/high tom are independent instances/settings of one source.
  ex={}
  for name,p in DEFAULTS.items():
   stem='tom' if name.endswith('-tom') else name
   if stem not in ex:ex[stem]=L.build('baseline-'+stem,ROOT/'modules/drums-606/v1'/f'{stem}.dsp')
   sr=48000;hit=4800
   y=L.render('baseline-'+name+'-hit',ex[stem],p,[(hit,'gate',1),(hit+1,'gate',0)],frames=sr*4)[:,0]
   L.wav(name+'_baseline.wav',y)
   report['baseline'][name]={'source':'modules/drums-606/v1/'+stem+'.dsp','settings':p,'features':features(y,sr)}
  report['execution_ok']=True
 except Exception as e:
  report['execution_ok']=False;report['error']=repr(e);report['compiler_output']=getattr(e,'output',None);raise
 finally:
  report['lab']=L.report
  (out/'discovery.json').write_text(json.dumps(report,indent=2))
  print(json.dumps({k:v for k,v in report.items() if k not in ('archives','lab','baseline')},indent=2),flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
