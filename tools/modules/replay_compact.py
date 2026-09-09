"""Rebuild verified generated C++, replay every score and add retriggered sweeps.
Requires C++17 and NumPy, not Faust. Never synthesize a substitute Python voice.
Input is the kick-controls-06 directory from an inspected CI artifact.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import wave
import numpy as np


def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def command(cmd):
    p=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=90)
    if p.returncode: raise RuntimeError(p.stderr or p.stdout or 'native command failed')
    return p.stdout

def wav(path,x):
    if not np.isfinite(x).all() or np.max(abs(x))>=1: raise ValueError('invalid listening output')
    with wave.open(str(path),'wb') as f:
        f.setnchannels(1);f.setsampwidth(2);f.setframerate(48000)
        f.writeframes(np.rint(x*32767).astype('<i2').tobytes())

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();e=args.evidence.resolve();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    report=json.loads((e/'results.json').read_text())
    if not report['passed'] or not all(c['passed'] for c in report['checks']):raise ValueError('CI evidence did not pass')
    for rel,digest in report['source_files'].items():
        if sha(e/'source'/rel)!=digest:raise ValueError('source mismatch: '+rel)
    executables={}
    for name,b in report['builds'].items():
        for file,key in [('generated.hpp','generated_sha256'),('expanded.dsp','expanded_sha256'),('controls.tsv','controls_sha256'),('render','binary_sha256')]:
            if sha(e/name/file)!=b[key]:raise ValueError('build hash mismatch: '+name+'/'+file)
        dest=out/name;dest.mkdir(exist_ok=True)
        command([os.environ.get('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(e/name),e/'source/tools/modules/render.cpp','-o',dest/'render'])
        executables[name]=dest/'render'
    verification=[]
    for item in report['renders']:
        label=item['label'];diag=item['diagnostics'];score=e/(label+'.tsv');raw=e/(label+'.f32')
        if sha(score)!=item['score_sha256'] or sha(raw)!=item['raw_sha256']:raise ValueError('score/raw mismatch: '+label)
        target=out/(label+'.f32')
        newdiag=json.loads(command([executables[item['build']],score,target,diag['rate'],diag['block'],diag['frames'],0]))
        a=np.fromfile(raw,dtype='<f4');b=np.fromfile(target,dtype='<f4')
        if a.shape!=b.shape or not np.isfinite(b).all():raise ValueError('invalid replay: '+label)
        error=float(np.max(abs(a-b)))
        if error>1e-3:raise ValueError('cross-compiler error exceeds declared bound: '+label+' '+str(error))
        verification.append(dict(label=label,max_error=error,bit_identical=np.array_equal(a,b),raw_sha256=sha(target)))
    # Restoring WAVs is lossless PCM conversion of replay, not stored sample synthesis.
    listening={}
    for profile in ('pre','post'):
        for name in ('anchors','sweeps','pattern'):
            x=np.fromfile(out/f'{profile}-{name}.f32',dtype='<f4')
            target=out/f'{profile}-{name}.wav';wav(target,x)
            listening[target.name]=dict(sha256=sha(target),identical_to_ci=(target.read_bytes()==(e/target.name).read_bytes()))
    # Supplemental audition: continuously ramp a control across repeated onsets.
    # This makes attack-only behavior audible; a late held note cannot show Punch.
    manifest=json.loads((e/'source/modules/kick-pm/playable-06/manifest.json').read_text())
    patches=json.loads((e/'source/modules/kick-pm/playable-06/patches.json').read_text())
    defaults={k:v['default'] for k,v in manifest['musical_controls'].items()}|dict(gate=0,velocity=1)
    timeline=[];events={(0,k):v for k,v in defaults.items()}
    for j,k in enumerate(patches['sweep_order']):
        start=j*4*48000+4800;lo=20. if k=='pitch_hz' else 0.;hi=160. if k=='pitch_hz' else 1.
        state=patches['sweep_baseline']|{k:lo}
        for key,value in state.items():events[start,key]=value
        for step in range(301):
            u=step/300;events[start+step*480,k]=lo*(hi/lo)**u if k=='pitch_hz' else u
        for h in range(9):
            n=start+h*18000;events[n,'gate']=1;events[n+11000,'gate']=0
        timeline.append(dict(seconds=start/48000,control=k,low=lo,high=hi))
    score=out/'retriggered-sweeps.tsv';score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(events.items())))
    for profile in ('pre','post'):
        raw=out/f'{profile}-retriggered-sweeps.f32';other=out/f'{profile}-retriggered-sweeps-127.f32'
        command([executables[profile],score,raw,48000,128,32*48000,0])
        command([executables[profile],score,other,48000,127,32*48000,0])
        x=np.fromfile(raw,dtype='<f4');y=np.fromfile(other,dtype='<f4')
        if not np.array_equal(x,y):raise ValueError('retrigger sweep block mismatch')
        wav(out/f'{profile}-retriggered-sweeps.wav',x)
        listening[f'{profile}-retriggered-sweeps.wav']=dict(raw_sha256=sha(raw),score_sha256=sha(score),block127_exact=True)
    # Interleave the four corresponding anchors, with no relative gain change.
    a=np.fromfile(out/'pre-anchors.f32',dtype='<f4');b=np.fromfile(out/'post-anchors.f32',dtype='<f4')
    combined=np.concatenate([x[j*4*48000:(j+1)*4*48000] for j in range(4) for x in (a,b)])
    wav(out/'profiles-ab.wav',combined)
    result=dict(source_commit=report['source_commit'],verification=verification,
        max_replay_error=max(x['max_error'] for x in verification),replayed=len(verification),
        exact_replays=sum(x['bit_identical'] for x in verification),listening=listening,sweep_timeline=timeline,
        processing='fixed original gain; PCM16 conversion only; profile A/B interleaves unchanged four-second segments',
        native_compiler=command([os.environ.get('CXX','c++'),'--version']),
        scope='independent generated-C++ replay; not second Faust build, device realtime or human listening approval')
    (out/'verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ('replayed','exact_replays','max_replay_error')}))

if __name__=='__main__':main()
