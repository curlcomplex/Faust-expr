"""Qualification for the Analog Classics Juno-60 single-note candidate.
This is implementation QA plus oracle-facing measurements, not hardware approval.
"""
from pathlib import Path
import json, os, platform, time
import numpy as np
from scipy.io import wavfile
from synth_batch import SynthLab, controls, command, digest

ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'modules/juno-60/v1/voice.dsp'
OUT_DEFAULT=ROOT/'build/juno60-reference'
DEFAULT=dict(gate=0,freq=220,velocity=1,saw=.72,pulse=.32,sub=.42,noise=.015,
             pwm=.50,pwmDepth=.14,lfoRate=4.5,cutoff=2200,resonance=.22,hpf=0,
             filterEnv=.36,keyTrack=.35,attack=.018,decay=.36,sustain=.70,
             release=.60,level=.68)
PRESETS={
 'DrySaw':dict(saw=1,pulse=0,sub=0,noise=0,pwmDepth=0,cutoff=16000,resonance=0,hpf=0,filterEnv=0,keyTrack=0,attack=.001,decay=.05,sustain=1,release=.05),
 'DryPulse':dict(saw=0,pulse=1,sub=0,noise=0,pwm=.5,pwmDepth=0,cutoff=16000,resonance=0,hpf=0,filterEnv=0,keyTrack=0,attack=.001,decay=.05,sustain=1,release=.05),
 'Sub':dict(saw=0,pulse=0,sub=1,noise=0,pwmDepth=0,cutoff=16000,resonance=0,hpf=0,filterEnv=0,keyTrack=0,attack=.001,decay=.05,sustain=1,release=.05),
 'Brass':dict(saw=.85,pulse=.18,sub=.18,noise=.01,cutoff=1250,resonance=.16,filterEnv=.62,keyTrack=.35,attack=.035,decay=.42,sustain=.62,release=.35),
 'Bass':dict(freq=110,saw=.82,pulse=.18,sub=.72,noise=0,cutoff=420,resonance=.22,filterEnv=.58,keyTrack=.25,attack=.003,decay=.21,sustain=.28,release=.14),
 'PWM':dict(saw=.22,pulse=1,sub=.18,noise=0,pwm=.48,pwmDepth=.30,lfoRate=.65,cutoff=2400,resonance=.10,filterEnv=.12,attack=.02,decay=.35,sustain=.78,release=.55),
}

def phrase(root=110,sr=48000):
    seq=(0,7,12,3,7,10,5,0)
    ev=[]
    for i,st in enumerate(seq):
        n=round((.10+i*.62)*sr)
        ev += [(n,'freq',root*2**(st/12)),(n,'velocity',1 if i%4==0 else .76),(n,'gate',1),(n+round(.42*sr),'gate',0)]
    return ev

def main(out=OUT_DEFAULT):
    out=Path(out); out.mkdir(parents=True,exist_ok=True); (out/'audition').mkdir(exist_ok=True)
    L=SynthLab(out); c=L.check; r=L.render
    L.report.update(version='juno60-reference-0.1.0',commit=os.environ.get('GITHUB_SHA','local'),
                    human_approved=False,hardware_approved=False,host_integrated=False,
                    defaults=DEFAULT,presets=PRESETS,
                    oracle_note='Hera is structural oracle; hardware/sample corpus remains sonic authority.')
    L.report['environment']=dict(platform=platform.platform(),faust=command([os.getenv('FAUST','faust'),'--version']),cxx=command([os.getenv('CXX','c++'),'--version']))
    start=time.perf_counter()
    exe=L.build('juno60',SRC); vec=L.build('juno60-vector',SRC,True)
    io,ui=controls(exe)
    c('contract:io',io==(0,1),io=io)
    c('contract:controls',set(ui)==set(DEFAULT),controls=sorted(ui))
    c('contract:freq-hz',ui['freq'][:3]==[20.,8000.,220.])
    c('contract:no-polyphony',all(k not in ui for k in ('voices','polyphony','chord','unison')))
    c('contract:no-internal-chorus',all('chorus' not in k.lower() for k in ui))
    events=[(480,'gate',1),(24480,'gate',0)]
    x=r('base-note',exe,DEFAULT,events,frames=144000)[:,0]
    c('note:audible',.003<float(abs(x).max())<1,peak=float(abs(x).max()))
    c('note:pre-silent',not np.any(x[:480]))
    c('note:release-settles',float(abs(x[-4800:]).max())<1e-6)
    z=r('never-triggered',exe,DEFAULT,[],frames=48000)
    c('note:never-triggered-silent',not np.any(z))
    h=r('half-velocity',exe,DEFAULT|{'velocity':.5},events,frames=144000)[:,0]
    c('note:velocity-linear',float(abs(h-.5*x).max())<3e-6,max_error=float(abs(h-.5*x).max()))
    for block in (1,32,127,512):
        y=r('block-'+str(block),exe,DEFAULT,events,frames=144000,block=block)[:,0]
        c('block:'+str(block),np.array_equal(y,x),max_error=float(abs(y-x).max()))
    y=r('vector',vec,DEFAULT,events,frames=144000)[:,0]
    c('vector:parity',float(abs(y-x).max())<1e-4,max_error=float(abs(y-x).max()))
    for sr in (44100,48000,96000):
        y=r('rate-'+str(sr),exe,DEFAULT,[(round(.01*sr),'gate',1),(round(.61*sr),'gate',0)],sr=sr,frames=sr*3)
        c('rate:'+str(sr),np.isfinite(y).all() and float(abs(y).max())<2,peak=float(abs(y).max()))
    # Verify HPF detents change the low-frequency output monotonically for a bass note.
    hp=[]
    base=DEFAULT|PRESETS['DrySaw']|{'freq':82.4069}
    for i,v in enumerate((0,.25,.50,.75,1.0)):
        y=r('hpf-'+str(i),exe,base|{'hpf':v},[(0,'gate',1)],frames=96000)[:,0]
        rms=float(np.sqrt(np.mean(y[48000:]**2))); hp.append(rms)
    c('hpf:detents-attenuate-bass',all(hp[i+1]<hp[i] for i in range(len(hp)-1)),rms=hp)
    # Resonance should produce a progressively stronger narrow-band peak without instability.
    res=[]
    rb=DEFAULT|{'saw':1,'pulse':0,'sub':0,'noise':0,'freq':110,'cutoff':1000,'filterEnv':0,'keyTrack':0,'attack':.001,'sustain':1,'release':.05}
    for i,v in enumerate((0,.25,.5,.75,.95)):
        y=r('res-'+str(i),exe,rb|{'resonance':v},[(0,'gate',1)],frames=96000)[:,0]
        seg=y[48000:]
        spec=np.abs(np.fft.rfft(seg*np.hanning(len(seg))))
        freqs=np.fft.rfftfreq(len(seg),1/48000)
        band=spec[(freqs>850)&(freqs<1150)]
        peak=float(np.max(band)); res.append(peak)
        c('res:bounded-'+str(i),np.isfinite(y).all() and float(abs(y).max())<2,peak=float(abs(y).max()))
    c('res:peak-grows',res[-1]>res[0]*1.2,peaks=res)
    # Key tracking must brighten higher notes when enabled relative to disabled tracking.
    kb=DEFAULT|{'saw':1,'pulse':0,'sub':0,'noise':0,'cutoff':500,'filterEnv':0,'resonance':0,'attack':.001,'sustain':1,'release':.05}
    def hf_ratio(freq,kt):
        y=r('kt-'+str(freq)+'-'+str(kt),exe,kb|{'freq':freq,'keyTrack':kt},[(0,'gate',1)],frames=72000)[:,0][24000:]
        sp=np.abs(np.fft.rfft(y*np.hanning(len(y))))**2; fr=np.fft.rfftfreq(len(y),1/48000)
        return float(sp[fr>1200].sum()/(sp.sum()+1e-20))
    low0,high0=hf_ratio(110,0),hf_ratio(880,0)
    low1,high1=hf_ratio(110,1),hf_ratio(880,1)
    c('filter:keytrack-effective',(high1-low1)>(high0-low0),values=[low0,high0,low1,high1])
    # Auditions: dry components, musical presets and common phrase.
    for name,p in PRESETS.items():
        root=(DEFAULT|p).get('freq',220)
        y=r('preset-'+name,exe,DEFAULT|p,phrase(root=root),frames=384000)
        L.wav('juno60_'+name+'.wav',y)
    y=r('common-phrase',exe,DEFAULT,phrase(root=110),frames=384000)
    L.wav('juno60_common_phrase.wav',y)
    L.report['elapsed_seconds']=time.perf_counter()-start
    L.finish()
    print(json.dumps({'checks':len(L.report.get('checks',[])),'failures':L.report.get('failures',[]),'out':str(out)},indent=2))

if __name__=='__main__':
    import argparse
    ap=argparse.ArgumentParser(); ap.add_argument('--out',default=str(OUT_DEFAULT)); a=ap.parse_args(); main(Path(a.out))
