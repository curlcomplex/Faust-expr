"""Replay frozen BDM-14 comparisons, not a new fitting run.
Raw outputs remain unchanged. Auditions use logged whole-hit gains only.
"""
from __future__ import annotations
import argparse, json, subprocess
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from fit_color import Objective, sha


def tail_slope(x, rate, window=(1.2,2.5)):
    x=np.asarray(x,dtype=float)
    if x.ndim!=1 or not np.isfinite(x).all() or rate<=0:raise ValueError('invalid mono audio/rate')
    hop=round(.025*rate);n=len(x)//hop
    if n<3:raise ValueError('short audio')
    env=np.sqrt(np.mean(x[:n*hop].reshape(n,hop)**2,axis=1));t=(np.arange(n)+.5)*hop/rate
    mask=(t>=window[0])&(t<=window[1])&(env>1e-8)
    if mask.sum()<3:raise ValueError('insufficient audible tail')
    db=20*np.log10(env[mask]);a,b=np.polyfit(t[mask],db,1)
    return dict(window_s=list(window),slope_db_per_s=float(a),rmse_db=float(np.std(db-a*t[mask]-b)))


def render(exe,out,name,p,rate,frames,off,block=128):
    events=[(0,k,v) for k,v in p.items() if k!='gate']+[(0,'gate',1)]
    if off<frames:events.append((off,'gate',0))
    score=out/(name+'.tsv');score.write_text(''.join(f'{n}\t{k}\t{v:.17g}\n' for n,k,v in events))
    raw=out/(name+'.f32')
    info=json.loads(subprocess.check_output([str(exe),str(score),str(raw),str(rate),str(block),str(frames),'0'],text=True))
    y=np.fromfile(raw,dtype='<f4').astype(float)
    if len(y)!=frames or not np.isfinite(y).all():raise ValueError('invalid native output')
    return y,dict(score_sha256=sha(score),raw_sha256=sha(raw),diagnostics=info,runner_sha256=sha(exe))


def main():
    a=argparse.ArgumentParser(description=__doc__)
    for n in ['frozen','references','baseline','candidate','out']:a.add_argument('--'+n,type=Path,required=True)
    args=a.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True);refs=args.references.resolve()
    frozen=json.loads(args.frozen.read_text());mf=json.loads((refs/'manifest.json').read_text())
    rec=next(r for r in mf['records'] if r['id']=='BDM-14');ref=(refs/rec['decoded_file']).resolve()
    if ref.parent!=refs or sha(ref)!=frozen['reference_decoded_sha256']:raise ValueError('reference mismatch')
    rate,x=wavfile.read(ref);x=x.astype(float)
    if rate!=44100 or x.ndim!=1:raise ValueError('reference format')
    obj=Objective(x,rate,(.8,1.5));cases={};sounds={}
    for name,item in frozen['cases'].items():
        exe=(args.candidate if 'body_hold_s' in item['parameters'] else args.baseline).resolve()
        y,proof=render(exe,out,name,item['parameters'],rate,len(x),item['gate_off_frame'])
        z,_=render(exe,out,name+'-127',item['parameters'],rate,len(x),item['gate_off_frame'],127)
        if np.max(abs(y-z))>1e-6:raise AssertionError('block-dependent selected fit')
        cases[name]=dict(parameters=item['parameters'],gate_off_frame=item['gate_off_frame'],metrics=obj.metrics(y),
            tail=tail_slope(y,rate),block_max_error=float(np.max(abs(y-z))),**proof);sounds[name]=y
    order=frozen['audition_order'];gap=np.zeros(round(.35*rate));parts=[];timeline=[];cursor=0
    for name in order:
        y=x if name=='reference' else sounds[name]*cases[name]['metrics']['rms_gain']
        timeline.append(dict(name=name,start_s=cursor/rate,gain=1 if name=='reference' else cases[name]['metrics']['rms_gain']))
        parts.extend([y,gap]);cursor+=len(y)+len(gap)
    joined=np.concatenate(parts);attenuation=min(1,.88/float(abs(joined).max()));joined*=attenuation
    if np.max(abs(joined))>=1:raise ValueError('would clip')
    wavfile.write(out/'reference-current-hold.wav',rate,np.rint(joined*32767).astype('<i2'))
    # A musical demonstration: retrigger the still-ringing voice, vary note length
    # and accents, but do not pretend these gates were used for the reference.
    patch=frozen['cases'][frozen['pattern_case']]['parameters'];events=[(0,k,v) for k,v in patch.items() if k!='gate']
    for i,(on,off,vel) in enumerate([(4800,7200,1),(28800,60000,.55),(76800,115200,1),(139200,143040,.65),(153600,201600,1)]):
        events += [(on,'velocity',vel),(on,'gate',1),(off,'gate',0)]
    events.sort(key=lambda e:e[0]);score=out/'pattern.tsv';score.write_text(''.join(f'{n}\t{k}\t{v:.17g}\n' for n,k,v in events))
    raw=out/'pattern.f32';exe=args.candidate.resolve()
    subprocess.run([str(exe),str(score),str(raw),'48000','127','264000','0'],check=True,capture_output=True)
    pattern=np.fromfile(raw,dtype='<f4');assert np.isfinite(pattern).all() and abs(pattern).max()<1
    wavfile.write(out/'pattern.wav',48000,np.rint(pattern*32767).astype('<i2'))
    report=dict(scope='same-preview frozen replay, not hardware macro fidelity or human approval',cases=cases,
        reference=frozen['reference'],reference_decoded_sha256=sha(ref),reference_tail=tail_slope(x,rate),
        frozen_sha256=sha(args.frozen),script_sha256=sha(__file__),
        listening=dict(order=order,timeline=timeline,common_attenuation=attenuation,
            processing='one RMS gain per synth plus common attenuation, silence gaps, PCM16; no EQ/limiter/reverb/alignment',
            sha256=sha(out/'reference-current-hold.wav')),
        pattern=dict(raw_sha256=sha(raw),score_sha256=sha(score),wav_sha256=sha(out/'pattern.wav'),processing='fixed kernel gain and PCM16 only'))
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    (out/'ATTRIBUTION.md').write_text('BDM-14 from Syntakt Designer Drums by Winston Edwards / Particles Into Waves, 13 June 2022.\n'
        'Source: https://particlesintowaves.bandcamp.com/album/syntakt-designer-drums\n'
        'License: CC BY 4.0, https://creativecommons.org/licenses/by/4.0/\n'
        'Reference: public MP3 preview decoded to float WAV, not lossless original. '
        'The audition applies the disclosed common attenuation, adds synth comparisons/gaps and converts to PCM16. '
        'No endorsement implied. Original bytes/decoded files remain unchanged.\n')
    print(json.dumps(dict(cases=len(cases),listening_sha256=report['listening']['sha256'])))
if __name__=='__main__':main()
