"""Replay frozen color-study presets through generated Faust and write comparisons."""
from __future__ import annotations
import argparse, hashlib, json, subprocess
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from fit_color import Objective, sha


def render(exe,out,label,p,off,rate,frames,block=128):
    rows=[(0,k,v) for k,v in p.items() if k!='gate']+[(0,'gate',1)]
    if off<frames:rows.append((off,'gate',0))
    score=out/(label+'.tsv');raw=out/(label+'.f32')
    score.write_text(''.join(f'{n}\t{k}\t{float(v):.17g}\n' for n,k,v in sorted(rows,key=lambda r:r[0])))
    r=subprocess.run([str(exe),str(score),str(raw),str(rate),str(block),str(frames),'0'],capture_output=True,text=True,timeout=20)
    if r.returncode:raise RuntimeError(r.stderr)
    y=np.fromfile(raw,dtype='<f4').astype(float)
    if len(y)!=frames or not np.isfinite(y).all():raise ValueError('native output')
    return y,dict(parameters=p,gate_off_frame=off,score=score.name,score_sha256=sha(score),
                  raw=raw.name,raw_sha256=sha(raw),diagnostics=json.loads(r.stdout))

def main():
    a=argparse.ArgumentParser(description=__doc__)
    for k in ['presets','references','runner','old-runner','out']:a.add_argument('--'+k,type=Path,required=True)
    args=a.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    presets=json.loads(args.presets.read_text());refs=args.references.resolve()
    manifest=json.loads((refs/'manifest.json').read_text())
    output=dict(scope='same-preview replay; no new fitting, hardware map, listening approval or realtime claim',
                presets_sha256=sha(args.presets),script_sha256=sha(__file__),
                kernel_runner_sha256=sha(args.runner),old_runner_sha256=sha(args.old_runner),
                reference_manifest_sha256=sha(refs/'manifest.json'),cases={})
    audio=[];timeline=[];position=0
    for name,case in presets['cases'].items():
        rec=next(r for r in manifest['records'] if r['id']==name)
        ref=(refs/rec['decoded_file']).resolve()
        if ref.parent!=refs or sha(ref)!=case['reference_sha256']:raise ValueError('reference path/hash')
        rate,x=wavfile.read(ref);x=x.astype(float)
        if rate!=44100 or x.ndim!=1:raise ValueError('format')
        objective=Objective(x,rate,(.22,.36));ys={};details={}
        variants=[('old',args.old_runner,case['old_parameters'],1),
                  ('new',args.runner,case['parameters'],case['gate_off_frame'])]
        for label,exe,p,off in variants:
            y,e=render(exe.resolve(),out,name+'-'+label,p,off,rate,len(x))
            y2,e2=render(exe.resolve(),out,name+'-'+label+'-127',p,off,rate,len(x),127)
            err=float(abs(y-y2).max())
            if err>1e-6:raise AssertionError('segmentation error')
            e.update(metrics=objective.metrics(y),replay_127_max_error=err)
            if not e['metrics']['pitch_acceptable']:raise AssertionError('pitch guard')
            details[label]=e;ys[label]=y
        output['cases'][name]=details
        for repetition in range(2):
            for label in ['reference','old','new']:
                y=x if label=='reference' else ys[label]*details[label]['metrics']['rms_gain']
                timeline.append(dict(seconds=position/rate,reference=name,item=label,repetition=repetition+1))
                audio.extend([y,np.zeros(round(.3*rate))]);position+=len(y)+round(.3*rate)
    audio=np.concatenate(audio);gain=min(.7,.9/max(float(abs(audio).max()),1e-20));audio*=gain
    if not np.isfinite(audio).all() or abs(audio).max()>=1:raise ValueError('audition headroom')
    dest=out/'reference-old-new.wav';wavfile.write(dest,44100,np.rint(audio*32767).astype('<i2'))
    output['listening']=dict(file=dest.name,sha256=sha(dest),timeline=timeline,global_gain=gain,
        transformations='Untrimmed reference; one whole-hit RMS match per synthesized version, one common attenuation, silence gaps, PCM16. No EQ, reverb, limiter or post-render timing alignment.')
    (out/'results.json').write_text(json.dumps(output,indent=2)+'\n')
    (out/'ATTRIBUTION.md').write_text(
        '# Reference attribution\n\nBDM-05 and BDM-09 from Syntakt Designer Drums, Winston Edwards / Particles Into Waves, 13 June 2022.\n'
        'Source: https://particlesintowaves.bandcamp.com/album/syntakt-designer-drums\n'
        'License: CC BY 4.0, https://creativecommons.org/licenses/by/4.0/\n\n'
        'Lossy public previews, not original lossless captures. Interleaved with synthesized comparison audio; '
        'whole-hit level changes, gaps and PCM16 conversion are detailed in results.json. No endorsement implied.\n')
    print(json.dumps({k:{v:d['metrics'] for v,d in r.items()} for k,r in output['cases'].items()},indent=2))
if __name__=='__main__':main()
