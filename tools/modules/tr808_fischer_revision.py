"""Compare current and versioned TR-808 candidates to the Fischer hardware anchors.
Reference WAVs are fetched transiently and never written to the evidence directory.
Candidate-only audition WAVs are safe to retain.
"""
from pathlib import Path
import argparse, json, hashlib
import numpy as np
from scipy.io import wavfile
import tr808_fischer_pass as F
from hats_v2_delivery import Lab
ROOT=Path(__file__).resolve().parents[2]

OLD={k:(p,v) for k,(p,v) in F.CAND.items() if v is not None}
for k,(eng,v) in F.AUX.items(): OLD[k]=(F.CAND[eng][0],v)

def render_set(L,tag,settings):
    built={}; audio={}; meta={}
    for key,vals in settings.items():
        src=ROOT/vals['source'] if isinstance(vals,dict) else ROOT/vals[0]
        p=vals if isinstance(vals,dict) else vals[1]
        p={k:v for k,v in p.items() if k!='source'}
        ss=str(src)
        if ss not in built: built[ss]=L.build(tag+'-'+str(len(built)),src)
        y=F.score_render(L,tag+'-'+key,built[ss],p)
        audio[key]=y; meta[key]={'source':str(src.relative_to(ROOT)),'settings':p,'descriptor':F.desc(y,F.OUTRATE),'sha256':hashlib.sha256(y.astype('<f4').tobytes()).hexdigest()}
    return audio,meta

def run(out):
    out=Path(out);L=Lab(out); (out/'audition').mkdir(exist_ok=True)
    revised=json.loads((ROOT/'modules/drums-808-reference/v1/classic-presets.json').read_text())
    revised={k:v for k,v in revised.items() if isinstance(v,dict) and 'source' in v}
    old_audio,old_meta=render_set(L,'old',OLD)
    new_audio,new_meta=render_set(L,'new',revised)
    refs={};comparisons={};fail=[]
    for key,path in F.REFS.items():
        _,data,sr,x=F.download(path);rd=F.desc(x,sr);refs[key]={'path':path,'sha256':hashlib.sha256(data).hexdigest(),'descriptor':rd}
        od=F.distance(rd,old_meta[key]['descriptor']);nd=F.distance(rd,new_meta[key]['descriptor'])
        comparisons[key]={'current_distance':od,'revised_distance':nd,'ratio':nd/(od+1e-30),'improvement_percent':100*(od-nd)/(od+1e-30)}
        # Major rewrites must make a clear descriptor improvement. Preset refinements may be neutral,
        # but no proposed classic anchor may degrade grossly without an explicit listening reason.
        if key in ('kick808','snare808','cymbal808') and not nd < od*.90: fail.append(key+'-major-improvement')
        if nd > od*1.12: fail.append(key+'-regression')
        wavfile.write(out/'audition'/(key+'_current.wav'),F.OUTRATE,old_audio[key].astype(np.float32))
        wavfile.write(out/'audition'/(key+'_revised.wav'),F.OUTRATE,new_audio[key].astype(np.float32))
        # 4 s current + 4 s revised; no normalization or reference audio.
        wavfile.write(out/'audition'/(key+'_current_then_revised.wav'),F.OUTRATE,np.concatenate((old_audio[key],new_audio[key])).astype(np.float32))
    order=['kick808','snare808','clap808','cymbal808','closed808','open808','lowtom808','midtom808','hitom808','lowconga808','midconga808','hiconga808','rim808','claves808','maracas808','cowbell808']
    bank=[]
    for k in order:
        bank.extend((old_audio[k][:F.OUTRATE*2],new_audio[k][:F.OUTRATE*2]))
    wavfile.write(out/'audition/00_all_current_then_revised.wav',F.OUTRATE,np.concatenate(bank).astype(np.float32))
    report={'passed':not fail,'failures':fail,'reference_policy':'Fischer WAVs transient only; hashes/descriptors retained; no hardware audio repackaged','comparisons':comparisons,'current':old_meta,'revised':new_meta,'references':refs}
    (out/'tr808-fischer-revision.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'passed':not fail,'failures':fail,'comparisons':comparisons},indent=2))
    if fail: raise SystemExit(1)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
