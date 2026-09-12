"""Analog Classics #63: actual Faust/C++ qualification and auditions for Vintage Tape + Retro Ensemble."""
from pathlib import Path
import argparse, json, os
import numpy as np
from scipy.io import wavfile
from hats_v2_delivery import Lab, command, digest
ROOT=Path(__file__).resolve().parents[2]
SOURCES={
    'tape':ROOT/'modules/vintage-tape/v1/tape.dsp',
    'ensemble':ROOT/'modules/retro-ensemble/v1/ensemble.dsp',
}
DEFAULTS={
    'tape':dict(input=0.,speed=16.35,output=0.),
    'ensemble':dict(voices=25.,fullness=0.,brighten=1.,mix=1.),
}
PRESETS={
    'tape':{
        'Clean15':dict(input=0.,speed=15.,output=0.),
        'SlowWarm':dict(input=3.,speed=7.5,output=-2.),
        'FastOpen':dict(input=1.5,speed=30.,output=-1.),
        'Slam':dict(input=12.,speed=15.,output=-8.),
    },
    'ensemble':{
        'Light':dict(voices=6,fullness=.18,brighten=.7,mix=.45),
        'Classic':dict(voices=18,fullness=.38,brighten=.72,mix=.72),
        'Wide':dict(voices=32,fullness=.56,brighten=.9,mix=.82),
        'StringWash':dict(voices=48,fullness=.76,brighten=.62,mix=1.),
    },
}

def controls(exe):
    rows=command([exe,'--controls']).splitlines()
    io=tuple(map(int,rows[0].split('\t')[1:])); p={}
    for line in rows[1:]:
        s=line.split('\t'); p[s[0]]=tuple(map(float,s[1:4]))
    return io,p

class PairLab(Lab):
    def __init__(self,out):
        super().__init__(out)
        self.report.update(version='airwindows-pair-0.1.0-experiment',commit=os.environ.get('GITHUB_SHA','local-snapshot'),human_approved=False,host_integrated=False,hardware_approved=False,oracle_approved=False,presets=PRESETS)
    def render_audio(self,name,exe,values,x,sr=48000,block=128,events=None):
        x=np.asarray(x,np.float32); assert x.ndim==2
        score=self.out/(name+'.tsv'); rawin=self.out/(name+'-input.f32'); raw=self.out/(name+'.f32')
        rows={(0,k):float(v) for k,v in values.items()}
        for n,k,v in (events or []): rows[(int(n),k)]=float(v)
        score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
        x.tofile(rawin)
        dg=json.loads(command([exe,score,raw,sr,block,len(x),0,rawin]))
        y=np.fromfile(raw,'<f4').reshape(-1,dg['channels'])
        self.check(name+':finite',len(y)==len(x) and np.isfinite(y).all(),peak=float(abs(y).max()))
        self.report['renders'].append(dict(name=name,rate=sr,block=block,input_sha256=digest(rawin),raw_sha256=digest(raw),score_sha256=digest(score),diagnostics=dg))
        return y

def program(sr,seconds=8):
    n=round(sr*seconds); t=np.arange(n)/sr; x=np.zeros((n,2),np.float32)
    # harmonically rich deterministic synth-like stimulus plus transient bursts
    x[:,0]=(.09*np.sin(2*np.pi*110*t)+.055*np.sin(2*np.pi*220*t)+.032*np.sin(2*np.pi*440*t)).astype(np.float32)
    x[:,1]=(.09*np.sin(2*np.pi*110*t+.12)+.055*np.sin(2*np.pi*220*t+.21)+.032*np.sin(2*np.pi*440*t+.37)).astype(np.float32)
    for beat in np.arange(.25,seconds-.25,.5):
        i=int(beat*sr); m=min(n-i,int(.08*sr)); tt=np.arange(m)/sr
        burst=(.18*np.sin(2*np.pi*(70+40*np.exp(-tt*25))*tt)*np.exp(-tt*30)).astype(np.float32)
        x[i:i+m,0]+=burst; x[i:i+m,1]+=burst*.94
    return np.clip(x,-.7,.7)

def impulse(sr,seconds=2):
    x=np.zeros((round(sr*seconds),2),np.float32); x[16,:]=.35; return x

def run(out):
    L=PairLab(out); c=L.check; r=L.render_audio; (L.out/'audition').mkdir(exist_ok=True)
    try:
        exes={}; vecs={}; meta={}
        for name,src in SOURCES.items():
            exes[name]=L.build(name+'-scalar',src); vecs[name]=L.build(name+'-vector',src,True)
            io,p=controls(exes[name]); meta[name]=p
            c(name+':stereo-io',io==(2,2),io=io)
            c(name+':controls',set(p)==set(DEFAULTS[name]),actual=sorted(p))
            c(name+':defaults',all(abs(p[k][2]-v)<1e-5 for k,v in DEFAULTS[name].items()),actual={k:p[k][2] for k in p})
        for name in SOURCES:
            z=np.zeros((48000,2),np.float32); y=r(name+'-silence',exes[name],DEFAULTS[name],z)
            c(name+':silence',float(abs(y).max())<1e-6,peak=float(abs(y).max()))
            imp=impulse(48000); base=r(name+'-impulse',exes[name],DEFAULTS[name],imp)
            c(name+':audible',float(abs(base).max())>1e-6,peak=float(abs(base).max()))
            for b in (1,32,64,127,256,512):
                y=r(name+'-block-'+str(b),exes[name],DEFAULTS[name],imp,block=b)
                c(name+':block-'+str(b),float(abs(y-base).max())<3e-5,residual=float(abs(y-base).max()))
            vy=r(name+'-vector',vecs[name],DEFAULTS[name],imp)
            c(name+':vector',float(abs(vy-base).max())<5e-4,residual=float(abs(vy-base).max()))
            for sr in (44100,96000): r(name+'-rate-'+str(sr),exes[name],DEFAULTS[name],impulse(sr),sr=sr)
            test=program(48000)
            live=[]
            for i,k in enumerate(DEFAULTS[name]):
                lo,hi,_=meta[name][k]; live.append((24000+i*3600,k,lo)); live.append((48000+i*3600,k,hi))
            y=r(name+'-live-controls',exes[name],DEFAULTS[name],test,events=live)
            c(name+':live-bounded',float(abs(y).max())<12,peak=float(abs(y).max()))
            banks=[]
            for preset,changes in PRESETS[name].items():
                vals=DEFAULTS[name]|changes
                y=r(name+'-preset-'+preset,exes[name],vals,test)
                wavfile.write(L.out/'audition'/f'{name}_{preset}.wav',48000,y.astype(np.float32)); banks.append(y)
            wavfile.write(L.out/'audition'/f'{name}_four_presets.wav',48000,np.concatenate(banks).astype(np.float32))
        # Tape must be level/nonlinearity dependent and speed must alter the transfer/history response.
        test=program(48000,4)
        clean=r('tape-transfer-clean',exes['tape'],DEFAULTS['tape'],test)
        slam=r('tape-transfer-slam',exes['tape'],DEFAULTS['tape']|PRESETS['tape']['Slam'],test)
        slow=r('tape-speed-slow',exes['tape'],DEFAULTS['tape']|{'speed':7.5},test)
        fast=r('tape-speed-fast',exes['tape'],DEFAULTS['tape']|{'speed':30},test)
        c('tape-drive-effective',np.linalg.norm(slam-clean)/(np.linalg.norm(clean)+1e-20)>.05)
        c('tape-speed-effective',np.linalg.norm(slow-fast)/(np.linalg.norm(fast)+1e-20)>.01)
        # Ensemble dry/wet and voice count must materially alter a sustained signal.
        e0=r('ensemble-dry',exes['ensemble'],DEFAULTS['ensemble']|{'mix':0},test)
        e1=r('ensemble-wet',exes['ensemble'],DEFAULTS['ensemble']|{'mix':1},test)
        e2=r('ensemble-six',exes['ensemble'],DEFAULTS['ensemble']|{'voices':6,'mix':1},test)
        e48=r('ensemble-48',exes['ensemble'],DEFAULTS['ensemble']|{'voices':48,'mix':1},test)
        c('ensemble-mix-effective',np.linalg.norm(e1-e0)/(np.linalg.norm(e0)+1e-20)>.05)
        c('ensemble-voices-effective',np.linalg.norm(e48-e2)/(np.linalg.norm(e2)+1e-20)>.01)
        L.report['source_sha256']={str(p.relative_to(ROOT)):digest(p) for p in SOURCES.values()}
        L.report['reference_boundary']='Airwindows IronOxideClassic2/Ensemble source topology used as MIT-licensed oracle direction; this batch has not yet executed direct C++ oracle residual comparison.'
        L.report['passed']=all(x['passed'] for x in L.report['checks'])
    except Exception as e:
        L.report.update(passed=False,error=repr(e))
    finally:
        (L.out/'results.json').write_text(json.dumps(L.report,indent=2)+'\n')
    print(json.dumps({'passed':L.report.get('passed',False),'checks':len(L.report['checks']),'renders':len(L.report['renders']),'error':L.report.get('error')}))
    if not L.report.get('passed'): raise SystemExit(1)

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--out',required=True); run(p.parse_args().out)
