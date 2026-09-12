"""Actual-Faust qualification for Analog Classics exploratory 808 drum quartet.
These are architecture-led studies, not hardware-approved clones.
"""
from pathlib import Path
import argparse, json
import numpy as np
from scipy.io import wavfile
from hats_v2_delivery import Lab, digest

ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'modules/analog-classics/drums-v1'
DEFAULTS={
 'kick808':dict(tone=.32,decay=.62,punch=.58,click=.16,drive=.08,freq=52.,velocity=1.,gate=0.),
 'snare808':dict(tone=.48,snappy=.62,decay=.42,noise_color=.56,drive=.05,freq=180.,velocity=1.,gate=0.),
 'clap808':dict(spacing=.52,snap=.66,decay=.44,tone=.54,tail=.36,drive=.04,freq=1200.,velocity=1.,gate=0.),
 'cymbal808':dict(metal=.96,tone=.52,decay=.68,shape=.28,drive=.05,freq=440.,velocity=1.,gate=0.),
}


def controls(exe):
    lines=__import__('subprocess').check_output([str(exe),'--controls'],text=True).splitlines()
    io=tuple(map(int,lines[0].split('\t')[1:]))
    ui={p[0]:tuple(map(float,p[1:])) for p in (x.split('\t') for x in lines[1:])}
    return io,ui


def run(out):
    L=Lab(out); c=L.check
    L.report.update(version='analog-classics-drums-0.1.0-experiment',human_approved=False,hardware_reference_approved=False,
                    note='architecture-led exploratory studies; not TR-808 clone claims')
    exes={}; vecs={}
    try:
        for name in DEFAULTS:
            exes[name]=L.build(name+'-scalar',SRC/(name+'.dsp'))
            vecs[name]=L.build(name+'-vector',SRC/(name+'.dsp'),True)
            io,ui=controls(exes[name])
            c(name+':single-note-io',io==(0,1),io=io)
            c(name+':canonical-note-controls',{'gate','freq','velocity'} <= set(ui),controls=sorted(ui))
            c(name+':no-internal-poly-control',not any(k in ui for k in ('voices','chord','stack_notes','polyphony')))
        rendered={}
        for name,exe in exes.items():
            d=DEFAULTS[name]
            x=L.render(name+'-default',exe,d,events=[(480,'gate',1),(481,'gate',0)],seconds=4.)
            rendered[name]=x
            c(name+':audible',float(np.max(np.abs(x)))>.005,peak=float(np.max(np.abs(x))))
            c(name+':initial-silence',not np.any(x[:480]))
            z=L.render(name+'-never',exe,d,events=[],seconds=1.)
            c(name+':never-triggered-silent',not np.any(z))
            zv=L.render(name+'-zero-velocity',exe,d|{'velocity':0},events=[(480,'gate',1),(481,'gate',0)],seconds=1.)
            c(name+':zero-velocity-silent',not np.any(zv))
            half=L.render(name+'-half',exe,d|{'velocity':.5},events=[(480,'gate',1),(481,'gate',0)],seconds=4.)
            c(name+':velocity-linear',float(np.max(np.abs(half-.5*x)))<2e-6)
            held=L.render(name+'-held',exe,d,events=[(480,'gate',1)],seconds=4.)
            c(name+':noteoff-does-not-choke',np.array_equal(held,x))
            for b in (1,32,64,127,128,256,512):
                y=L.render(f'{name}-block-{b}',exe,d,events=[(480,'gate',1),(481,'gate',0)],block=b,seconds=2.)
                ref=x[:len(y)]
                c(f'{name}:block-{b}',np.array_equal(y,ref))
            y=L.render(name+'-vector',vecs[name],d,events=[(480,'gate',1),(481,'gate',0)],seconds=2.)
            c(name+':vector-parity',float(np.max(np.abs(y-x[:len(y)])))<3e-5)
            for sr in (44100,96000):
                q=L.render(f'{name}-rate-{sr}',exe,d,events=[(round(.01*sr),'gate',1),(round(.01*sr)+1,'gate',0)],sr=sr,seconds=2.)
                c(f'{name}:rate-{sr}-finite',np.isfinite(q).all() and np.max(np.abs(q))>.005)
            lo=d['freq']; hi=d['freq']
            # Exercise real pitch input rather than a hidden ratio control.
            if name=='kick808': lo,hi=36.,80.
            elif name=='snare808': lo,hi=120.,280.
            elif name=='clap808': lo,hi=800.,2200.
            elif name=='cymbal808': lo,hi=300.,700.
            a=L.render(name+'-freq-low',exe,d|{'freq':lo},events=[(480,'gate',1),(481,'gate',0)],seconds=1.)
            b=L.render(name+'-freq-high',exe,d|{'freq':hi},events=[(480,'gate',1),(481,'gate',0)],seconds=1.)
            c(name+':freq-affects-sound',float(np.linalg.norm(a-b)/(np.linalg.norm(a)+1e-20))>.05)

        # Fixed-gain audition: isolated quartet then a simple 120-BPM loop.
        sr=48000; gap=np.zeros(sr//2,np.float32)
        parts=[]
        for name in ('kick808','snare808','clap808','cymbal808'):
            parts += [rendered[name][:sr*2].astype(np.float32),gap]
        isolated=np.concatenate(parts)
        wavfile.write(L.out/'01_808_study_isolated.wav',sr,isolated)
        seconds=8; mix=np.zeros(sr*seconds,np.float32)
        step=sr//4
        pattern={
          'kick808':[0,4,8,10,12,16,20,24,26,28],
          'snare808':[4,12,20,28],
          'clap808':[12,28],
          'cymbal808':[0,16],
        }
        for name,steps in pattern.items():
            hit=rendered[name]
            for st in steps:
                n=st*step
                m=min(len(hit),len(mix)-n)
                if m>0: mix[n:n+m]+=hit[:m]
        peak=float(np.max(np.abs(mix)))
        gain=1.0 if peak<=.95 else .95/peak
        wavfile.write(L.out/'02_808_study_pattern.wav',sr,(mix*gain).astype(np.float32))
        L.report['audition']={'gain':gain,'peak_before_gain':peak,'files':['01_808_study_isolated.wav','02_808_study_pattern.wav']}
        L.report['source_sha256']={name:digest(SRC/(name+'.dsp')) for name in DEFAULTS}
        L.report['passed']=all(x['passed'] for x in L.report['checks'])
    except Exception as e:
        L.report['passed']=False; L.report['error']=repr(e)
        (L.out/'results.json').write_text(json.dumps(L.report,indent=2))
        print(json.dumps({'passed':False,'checks':len(L.report['checks']),'renders':len(L.report['renders']),'error':repr(e)}))
        raise
    (L.out/'results.json').write_text(json.dumps(L.report,indent=2))
    print(json.dumps({'passed':L.report['passed'],'checks':len(L.report['checks']),'renders':len(L.report['renders'])}))

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--out',required=True); run(p.parse_args().out)
