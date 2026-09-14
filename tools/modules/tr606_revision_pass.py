"""Search classic TR-606 anchor settings against the Roland Clan hardware set.
Keeps the existing broad parameter ranges; this is a neutral-anchor fit, not a
restriction of extended behaviour. Candidate-only audition WAVs are emitted.
"""
from pathlib import Path
import argparse, itertools, io, json, zipfile
import numpy as np
from scipy.io import wavfile
import tr606_reference_pass as B
from hats_v2_delivery import Lab

ROOT=B.ROOT
GRIDS={
 'kick606': {},
 'snare606': {'freq':[185.,192.,200.],'decay':[.10,.12,.14,.16,.18],'tone':[0.,.1,.2,.3],'snappy':[.35,.5,.65]},
 'lowtom606': {'freq':[120.,128.,132.,136.],'decay':[.22,.26,.28,.30,.32]},
 'hitom606': {'freq':[195.,205.,210.,215.],'decay':[.16,.18,.20,.22]},
 'closed606': {'decay':[.08,.10,.12,.14,.16],'tone':[.5,.65,.8,1.]},
 'open606': {'decay':[1.2,1.6,2.0,2.2],'tone':[.5,.65,.8,1.]},
 'cymbal606': {'decay':[.6,.8,1.0,1.2,1.5],'tone':[.5,.65,.8,1.]},
}

def variants(base,grid):
    if not grid: yield dict(base); return
    keys=list(grid)
    for values in itertools.product(*(grid[k] for k in keys)):
        x=dict(base);x.update(dict(zip(keys,values)));yield x

def pair_wav(cur,rev,sr=44100):
    gap=np.zeros(int(.2*sr));x=np.concatenate([cur,gap,rev]);peak=max(1e-9,float(np.max(np.abs(x))));x=np.clip(x/peak*.92,-1,1)
    return (x*32767).astype(np.int16)

def run(out):
    L=Lab(out);out=Path(out);aud=out/'audition';aud.mkdir(parents=True,exist_ok=True)
    rawzip=B.fetch_zip();z=zipfile.ZipFile(io.BytesIO(rawzip));refs={}
    for key,path in B.REFS.items():
        sr,x=wavfile.read(io.BytesIO(z.read(path)));refs[key]=B.desc(B.mono(x),sr)
    report={'reference':'Roland Clan TR-606 raw sample archive','current':{},'revised':{},'selected':{},'failures':[]}
    for key,(src,base) in B.CAND.items():
        exe=L.build('tr606-rev-'+key,ROOT/src);cur=B.render(L,key+'-current',exe,base);cd=B.distance(refs[key],B.desc(cur,B.OUTRATE))
        best=(cd,dict(base),cur)
        for i,vals in enumerate(variants(base,GRIDS[key])):
            y=B.render(L,f'{key}-v{i}',exe,vals);dist=B.distance(refs[key],B.desc(y,B.OUTRATE))
            if dist<best[0]:best=(dist,vals,y)
        rd,vals,rev=best;ratio=rd/cd if cd else 1
        report['current'][key]={'distance':cd,'settings':base};report['revised'][key]={'distance':rd,'settings':vals}
        report['selected'][key]={'ratio':ratio,'improvement_percent':(1-ratio)*100}
        # Guard: if a search cannot improve, retain exact current anchor.
        if rd>cd+1e-12: report['failures'].append(key+'-regression')
        wavfile.write(aud/f'{key}-current-then-revised.wav',B.OUTRATE,pair_wav(cur,rev))
    report['passed']=not report['failures']
    (out/'tr606-revision.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'passed':report['passed'],'failures':report['failures'],'selected':report['selected'],'revised':report['revised']},indent=2))
    if report['failures']:raise SystemExit(1)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);a=p.parse_args();run(a.out)
