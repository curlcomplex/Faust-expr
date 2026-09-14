"""Hardware-reference descriptor pass for the Analog Classics TR-808 family.

Downloads exact public mirror paths from Michael Fischer/Technopolis' 1994
TR-808 set at runtime. Reference WAV bytes are NOT copied to the evidence
artifact; only URL/path, SHA256, decoded format and descriptors are retained.
Candidate audio is compiled Faust/C++ through the existing Lab renderer.
"""
from pathlib import Path
import argparse, hashlib, io, json, math, os, urllib.request
import numpy as np
from scipy.io import wavfile
from scipy.signal import welch
from hats_v2_delivery import Lab

ROOT=Path(__file__).resolve().parents[2]
BASE='https://raw.githubusercontent.com/fluid-music/open-drums/main/tr-808/TR808WAV'
OUTRATE=44100

# Mid-panel anchors where adjustable. Fischer naming is documented in TR808.TXT:
# first hardware control before second; 50 = panel midpoint. Fixed voices have no suffix.
REFS={
 'kick808':'BD/BD5050.WAV', 'snare808':'SD/SD5050.WAV',
 'clap808':'CP/CP.WAV', 'cymbal808':'CY/CY5050.WAV',
 'closed808':'CH/CH.WAV', 'open808':'OH/OH50.WAV',
 'lowtom808':'LT/LT50.WAV', 'midtom808':'MT/MT50.WAV', 'hitom808':'HT/HT50.WAV',
 'lowconga808':'LC/LC50.WAV','midconga808':'MC/MC50.WAV','hiconga808':'HC/HC50.WAV',
 'rim808':'RS/RS.WAV','claves808':'CL/CL.WAV','maracas808':'MA/MA.WAV','cowbell808':'CB/CB.WAV'
}

CAND={
 'kick808':('modules/analog-classics/drums-v2/kick808.dsp',dict(tone=.32,decay=.62,punch=.58,click=.16,drive=.08,freq=52.,velocity=1.,gate=0.)),
 'snare808':('modules/analog-classics/drums-v2/snare808.dsp',dict(tone=.48,snappy=.62,decay=.42,noise_color=.56,drive=.05,freq=180.,velocity=1.,gate=0.)),
 'clap808':('modules/analog-classics/drums-v2/clap808.dsp',dict(spacing=.40,tone=.48,snap=.62,decay=.32,tail=.35,drive=.08,freq=1500.,velocity=1.,gate=0.)),
 'cymbal808':('modules/analog-classics/drums-v2/cymbal808.dsp',dict(metal=.96,tone=.52,decay=.68,shape=.28,drive=.05,freq=440.,velocity=1.,gate=0.)),
 'closed808':('modules/hats-analog/single-note-v1/closed.dsp',dict(gate=0.,freq=440.,velocity=1.,tone=.5,decay=.25,metal=.5)),
 'open808':('modules/hats-analog/single-note-v1/open.dsp',dict(gate=0.,chokeGate=0.,freq=440.,velocity=1.,tone=.5,decay=.5,metal=.5)),
 'tomconga808':('modules/drums-808-aux/v1/tom-conga.dsp',None),
 'rimclaves808':('modules/drums-808-aux/v2/rim-claves.dsp',None),
 'maracas808':('modules/drums-808-aux/v1/maracas.dsp',None),
 'cowbell808':('modules/drums-808-aux/v1/cowbell.dsp',None),
}

# Exact retained classic settings from percussion-finish/manifest.json.
AUX={
 'lowtom808':('tomconga808',dict(gate=0,freq=90.,velocity=1.,accent=0.,mode=0.,decay=.36,bend=.22,noise=.16,tone=.5,drive=.05,level=.8)),
 'midtom808':('tomconga808',dict(gate=0,freq=140.,velocity=1.,accent=0.,mode=0.,decay=.25,bend=.22,noise=.16,tone=.5,drive=.05,level=.8)),
 'hitom808':('tomconga808',dict(gate=0,freq=190.,velocity=1.,accent=0.,mode=0.,decay=.20,bend=.22,noise=.15,tone=.5,drive=.05,level=.8)),
 'lowconga808':('tomconga808',dict(gate=0,freq=190.,velocity=1.,accent=0.,mode=1.,decay=.35,bend=.22,noise=0.,tone=.5,drive=.05,level=.8)),
 'midconga808':('tomconga808',dict(gate=0,freq=280.,velocity=1.,accent=0.,mode=1.,decay=.17,bend=.22,noise=0.,tone=.5,drive=.05,level=.8)),
 'hiconga808':('tomconga808',dict(gate=0,freq=410.,velocity=1.,accent=0.,mode=1.,decay=.145,bend=.22,noise=0.,tone=.5,drive=.05,level=.8)),
 'rim808':('rimclaves808',dict(gate=0,freq=455.,velocity=1.,accent=0.,mode=0.,decay=.014,crack=.55,tone=.5,drive=.22,level=.8)),
 'claves808':('rimclaves808',dict(gate=0,freq=2500.,velocity=1.,accent=0.,mode=1.,decay=.062,crack=.15,tone=.5,drive=.02,level=.8)),
 'maracas808':('maracas808',dict(gate=0,freq=7200.,velocity=1.,accent=0.,decay=.045,tone=.55,grit=.18,level=.75)),
 'cowbell808':('cowbell808',dict(gate=0,freq=540.,velocity=1.,accent=0.,ratio=1.48,decay=.42,tone=.55,drive=.08,level=.78)),
}

def mono(x):
    x=np.asarray(x)
    if x.ndim==2: x=x.mean(axis=1)
    if np.issubdtype(x.dtype,np.integer): x=x.astype(np.float64)/max(abs(np.iinfo(x.dtype).min),np.iinfo(x.dtype).max)
    else: x=x.astype(np.float64)
    return x

def onset_trim(x,sr):
    x=mono(x); peak=float(np.max(np.abs(x))) if len(x) else 0
    if peak<=0:return x,0
    win=max(8,int(sr*.0005)); env=np.sqrt(np.convolve(x*x,np.ones(win)/win,'same'))
    idx=np.flatnonzero(env>max(peak*.005,1e-5)); start=max(0,int(idx[0])-int(sr*.002)) if len(idx) else 0
    return x[start:],start

def desc(x,sr):
    x,_=onset_trim(x,sr); n=min(len(x),int(sr*4));x=x[:n]
    if not len(x) or np.max(np.abs(x))==0:return {'peak':0,'rms':0}
    e=x*x; total=e.sum()+1e-30; cs=np.cumsum(e)/total
    t50=float(np.searchsorted(cs,.5)/sr);t90=float(np.searchsorted(cs,.9)/sr);t99=float(np.searchsorted(cs,.99)/sr)
    def spec(a,b):
        y=x[int(a*sr):min(len(x),int(b*sr))]
        if len(y)<64:return {'centroid':0,'bands':[0]*8}
        f,p=welch(y,sr,nperseg=min(2048,len(y)));s=p.sum()+1e-30
        edges=(0,100,250,500,1000,2500,5000,10000,sr/2+1)
        bands=[float(p[(f>=lo)&(f<hi)].sum()/s) for lo,hi in zip(edges,edges[1:])]
        return {'centroid':float((f*p).sum()/s),'bands':bands}
    y=x[int(.03*sr):min(len(x),int(.25*sr))]; f0=0.0
    if len(y)>256:
        f,p=welch(y,sr,nperseg=min(8192,len(y)));m=(f>=25)&(f<=1200)
        if np.any(m):f0=float(f[m][np.argmax(p[m])])
    return {'peak':float(np.max(np.abs(x))),'rms':float(np.sqrt(np.mean(e))), 't50_s':t50,'t90_s':t90,'t99_s':t99,'f0_peak_hz':f0,'attack':spec(0,.03),'body':spec(.03,.3)}

def distance(a,b):
    terms=[]
    for k,w in [('t50_s',1),('t90_s',1),('t99_s',.5)]:
        if a.get(k,0)>0 and b.get(k,0)>0:terms.append(w*abs(math.log((a[k]+1e-6)/(b[k]+1e-6))))
    if a.get('f0_peak_hz',0)>20 and b.get('f0_peak_hz',0)>20:terms.append(.5*abs(math.log(a['f0_peak_hz']/b['f0_peak_hz'])))
    for section in ('attack','body'):
        aa=np.array(a.get(section,{}).get('bands',[0]*8));bb=np.array(b.get(section,{}).get('bands',[0]*8));terms.append(float(np.sqrt(np.mean((np.sqrt(aa+1e-12)-np.sqrt(bb+1e-12))**2))))
    return float(np.mean(terms))

def download(path):
    url=BASE+'/'+path;req=urllib.request.Request(url,headers={'User-Agent':'Faust-expr-reference-lab/1.0'})
    with urllib.request.urlopen(req,timeout=30) as r:data=r.read(8_000_001)
    if len(data)>8_000_000:raise RuntimeError('oversize '+path)
    sr,x=wavfile.read(io.BytesIO(data));return url,data,sr,mono(x)

def score_render(L,name,exe,vals,seconds=4):
    rows={(0,k):float(v) for k,v in vals.items()};rows[(441,'gate')]=1.;rows[(442,'gate')]=0.
    score=L.out/(name+'.tsv');raw=L.out/(name+'.f32');score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
    from hats_v2_delivery import command
    d=json.loads(command([exe,score,raw,OUTRATE,128,round(seconds*OUTRATE),0]));x=np.fromfile(raw,'<f4').reshape(-1,d['channels']);return x[:,0] if x.shape[1]==1 else x.mean(axis=1)

def run(out):
    L=Lab(out);out=Path(out);report={'source':{'creator':'Michael Fischer / Technopolis','set':'TR-808 Sound Sample Set 1.0.0 (1994-09-08)','mirror':'fluid-music/open-drums','recording':'TR-808 serial 103852, individual outputs, PCM16/44.1kHz; panel level full; documented knob grid'},'references':{},'current':{},'comparisons':{},'candidate_audio':{},'note':'Descriptor distances are diagnostic only, level-independent and are not authenticity percentages.'}
    for key,path in REFS.items():
        url,data,sr,x=download(path);report['references'][key]={'path':path,'url':url,'sha256':hashlib.sha256(data).hexdigest(),'rate':sr,'frames':len(x),'descriptor':desc(x,sr)}
    ex={}
    for key,(path,vals) in CAND.items():
        ex[key]=L.build('fischer-'+key,ROOT/path)
        if vals is not None:
            y=score_render(L,key,ex[key],vals);report['current'][key]={'descriptor':desc(y,OUTRATE),'source':path,'settings':vals};report['candidate_audio'][key]=hashlib.sha256(y.astype('<f4').tobytes()).hexdigest();report['comparisons'][key]={'descriptor_distance':distance(report['references'][key]['descriptor'],report['current'][key]['descriptor'])}
    for key,(engine,vals) in AUX.items():
        y=score_render(L,key,ex[engine],vals);report['current'][key]={'descriptor':desc(y,OUTRATE),'source':CAND[engine][0],'settings':vals};report['candidate_audio'][key]=hashlib.sha256(y.astype('<f4').tobytes()).hexdigest();report['comparisons'][key]={'descriptor_distance':distance(report['references'][key]['descriptor'],report['current'][key]['descriptor'])}
    grids={'kick808':[f'BD/BD{a}{b}.WAV' for a in ('00','25','50','75','10') for b in ('00','25','50','75','10')], 'snare808':[f'SD/SD{a}{b}.WAV' for a in ('00','25','50','75','10') for b in ('00','25','50','75','10')], 'cymbal808':[f'CY/CY{a}{b}.WAV' for a in ('00','25','50','75','10') for b in ('00','25','50','75','10')], 'open808':[f'OH/OH{x}.WAV' for x in ('00','25','50','75','10')]}
    report['hardware_grids']={}
    for inst,paths in grids.items():
        arr=[]
        for p in paths:
            try:
                url,data,sr,x=download(p);arr.append({'path':p,'sha256':hashlib.sha256(data).hexdigest(),'descriptor':desc(x,sr)})
            except Exception as e:arr.append({'path':p,'error':repr(e)})
        report['hardware_grids'][inst]=arr
    (out/'tr808-fischer-current.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'reference_files':len(report['references'])+sum(len(v) for v in report['hardware_grids'].values()),'comparisons':report['comparisons']},indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);a=p.parse_args();run(a.out)
