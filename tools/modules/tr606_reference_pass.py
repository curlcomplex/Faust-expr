"""TR-606 real-hardware reference descriptor pass.

Primary neutral archive: Roland Clan TR-606 sample set. Reference WAV bytes are
fetched at CI runtime and are never committed/repacked; reports retain hashes,
format metadata and descriptors only. Candidate audio is compiled from Faust
through the existing deterministic Lab renderer.
"""
from pathlib import Path
import argparse, hashlib, io, json, math, urllib.request, zipfile
import numpy as np
from scipy.io import wavfile
from scipy.signal import welch
from hats_v2_delivery import Lab, command

ROOT=Path(__file__).resolve().parents[2]
URL='https://www.rolandclan.com/media/39/Roland_TR-606.zip'
OUTRATE=44100

REFS={
 'kick606':'Roland_TR-606/TR606 kick.wav',
 'snare606':'Roland_TR-606/TR606 snare.wav',
 'lowtom606':'Roland_TR-606/TR606 lo tom.wav',
 'hitom606':'Roland_TR-606/TR606 hi tom.wav',
 'closed606':'Roland_TR-606/TR606 cl hat.wav',
 'open606':'Roland_TR-606/TR606 open hat.wav',
 'cymbal606':'Roland_TR-606/TR606 cymbal.wav',
}
CAND={
 'kick606':('modules/drums-606/v1/kick.dsp',dict(gate=0,freq=55.,velocity=1.,accent=0.,decay=.30,tone=.5,click=.18,level=.8)),
 'snare606':('modules/drums-606/v1/snare.dsp',dict(gate=0,freq=185.,velocity=1.,accent=0.,decay=.20,tone=.5,snappy=.65,level=.8)),
 'lowtom606':('modules/drums-606/v1/low-tom.dsp',dict(gate=0,freq=110.,velocity=1.,accent=0.,decay=.32,tone=.5,noise=.22,level=.8)),
 'hitom606':('modules/drums-606/v1/high-tom.dsp',dict(gate=0,freq=165.,velocity=1.,accent=0.,decay=.22,tone=.5,noise=.16,level=.8)),
 'closed606':('modules/drums-606/v1/closed-hat.dsp',dict(gate=0,freq=440.,velocity=1.,accent=0.,decay=.075,tone=.5,metalSpread=.5,level=.8)),
 'open606':('modules/drums-606/v1/open-hat.dsp',dict(gate=0,chokeGate=0,freq=440.,velocity=1.,accent=0.,decay=.45,tone=.5,metalSpread=.5,level=.8)),
 'cymbal606':('modules/drums-606/v1/cymbal.dsp',dict(gate=0,freq=440.,velocity=1.,accent=0.,decay=1.20,tone=.5,metalSpread=.5,level=.8)),
}

def fetch_zip():
    req=urllib.request.Request(URL,headers={'User-Agent':'Mozilla/5.0 Faust-expr hardware-reference lab','Referer':'https://www.rolandclan.com/library/tr-606/'})
    with urllib.request.urlopen(req,timeout=30) as r:data=r.read(8_000_001)
    if len(data)>8_000_000:raise RuntimeError('reference archive unexpectedly large')
    return data

def mono(x):
    x=np.asarray(x)
    if x.ndim==2:x=x.mean(axis=1)
    if np.issubdtype(x.dtype,np.integer):x=x.astype(np.float64)/max(abs(np.iinfo(x.dtype).min),np.iinfo(x.dtype).max)
    else:x=x.astype(np.float64)
    return x

def onset_trim(x,sr):
    x=mono(x); peak=float(np.max(np.abs(x))) if len(x) else 0
    if peak<=0:return x,0
    win=max(8,int(sr*.0005)); env=np.sqrt(np.convolve(x*x,np.ones(win)/win,'same'))
    idx=np.flatnonzero(env>max(peak*.005,1e-5)); start=max(0,int(idx[0])-int(sr*.002)) if len(idx) else 0
    return x[start:],start

def desc(x,sr):
    x,_=onset_trim(x,sr);x=x[:min(len(x),int(sr*4))]
    if not len(x) or np.max(np.abs(x))==0:return {'peak':0,'rms':0}
    e=x*x; total=e.sum()+1e-30; cs=np.cumsum(e)/total
    def spec(a,b):
        y=x[int(a*sr):min(len(x),int(b*sr))]
        if len(y)<64:return {'centroid':0,'bands':[0]*8}
        f,p=welch(y,sr,nperseg=min(2048,len(y)));s=p.sum()+1e-30
        edges=(0,100,250,500,1000,2500,5000,10000,sr/2+1)
        bands=[float(p[(f>=lo)&(f<hi)].sum()/s) for lo,hi in zip(edges,edges[1:])]
        return {'centroid':float((f*p).sum()/s),'bands':bands}
    y=x[int(.02*sr):min(len(x),int(.30*sr))]; f0=0.
    if len(y)>256:
        f,p=welch(y,sr,nperseg=min(8192,len(y)));m=(f>=25)&(f<=2500)
        if np.any(m):f0=float(f[m][np.argmax(p[m])])
    return {'peak':float(np.max(np.abs(x))),'rms':float(np.sqrt(np.mean(e))),
            't50_s':float(np.searchsorted(cs,.5)/sr),'t90_s':float(np.searchsorted(cs,.9)/sr),'t99_s':float(np.searchsorted(cs,.99)/sr),
            'f0_peak_hz':f0,'attack':spec(0,.03),'body':spec(.03,.30)}

def distance(a,b):
    terms=[]
    for k,w in [('t50_s',1),('t90_s',1),('t99_s',.5)]:
        if a.get(k,0)>0 and b.get(k,0)>0:terms.append(w*abs(math.log((a[k]+1e-6)/(b[k]+1e-6))))
    if a.get('f0_peak_hz',0)>20 and b.get('f0_peak_hz',0)>20:terms.append(.5*abs(math.log(a['f0_peak_hz']/b['f0_peak_hz'])))
    for section in ('attack','body'):
        aa=np.array(a.get(section,{}).get('bands',[0]*8));bb=np.array(b.get(section,{}).get('bands',[0]*8))
        terms.append(float(np.sqrt(np.mean((np.sqrt(aa+1e-12)-np.sqrt(bb+1e-12))**2))))
    return float(np.mean(terms))

def render(L,name,exe,vals,seconds=4):
    rows={(0,k):float(v) for k,v in vals.items()};rows[(441,'gate')]=1.;rows[(442,'gate')]=0.
    score=L.out/(name+'.tsv');raw=L.out/(name+'.f32')
    score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
    meta=json.loads(command([exe,score,raw,OUTRATE,128,round(seconds*OUTRATE),0]))
    x=np.fromfile(raw,'<f4').reshape(-1,meta['channels'])
    return x[:,0] if x.shape[1]==1 else x.mean(axis=1)

def run(out):
    L=Lab(out);out=Path(out);rawzip=fetch_zip();z=zipfile.ZipFile(io.BytesIO(rawzip))
    report={'source':{'publisher':'Roland Clan','url':URL,'description':'sampled sounds made using the Roland TR-606'},
            'archive_sha256':hashlib.sha256(rawzip).hexdigest(),'references':{},'current':{},'comparisons':{},
            'note':'Descriptor distance is diagnostic and level-independent; it is not an authenticity percentage.'}
    for key,path in REFS.items():
        data=z.read(path);sr,x=wavfile.read(io.BytesIO(data));x=mono(x)
        report['references'][key]={'path':path,'sha256':hashlib.sha256(data).hexdigest(),'rate':int(sr),'frames':len(x),'descriptor':desc(x,sr)}
    for key,(path,vals) in CAND.items():
        exe=L.build('tr606-'+key,ROOT/path);y=render(L,key,exe,vals)
        report['current'][key]={'source':path,'settings':vals,'descriptor':desc(y,OUTRATE),'sha256_f32':hashlib.sha256(y.astype('<f4').tobytes()).hexdigest()}
        report['comparisons'][key]={'descriptor_distance':distance(report['references'][key]['descriptor'],report['current'][key]['descriptor'])}
    (out/'tr606-current.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'archive_sha256':report['archive_sha256'],'comparisons':report['comparisons']},indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);a=p.parse_args();run(a.out)
