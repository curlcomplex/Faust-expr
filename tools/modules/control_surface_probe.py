"""Actual-Faust compact-control tests and listening evidence. No external audio.
Canonical mappings are Faust. map_static is an independent test oracle only.
"""
from __future__ import annotations
import argparse
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import wave
import numpy as np

ROOT=Path(__file__).resolve().parents[2]
MODULE=ROOT/'modules/kick-pm/playable-06'
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def map_static(p,stage):
    return dict(frequency_hz=p['pitch_hz'],pitch_amount_hz=p['pitch_hz']*(2**(3.5*p['sweep'])-1),
        pitch_tau_s=.055*.1**p['punch'],attack_tau_s=.0015*.1**p['punch'],
        body_tau_s=.018*100**p['decay'],release_tau_s=.012*25**p['decay'],
        phase_cycles=0.,level=.6,velocity=p.get('velocity',1.),gate=0.,
        square=p['color']**2,triangle=p['shape']**2,mod_envelope=p['contour'],
        mod_tau_s=.012*16**(1-p['contour']),drive=p['drive']**2,
        drive_after_body=float(stage),feedback_mode=1.)

def write_wav(path,x,rate=48000):
    if not np.isfinite(x).all() or np.max(abs(x))>=1: raise ValueError('refusing clipping/nonfinite WAV')
    with wave.open(str(path),'wb') as f:
        f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate)
        f.writeframes(np.rint(x*32767).astype('<i2').tobytes())

class Study:
    def __init__(self,out):
        self.out=out.resolve();self.out.mkdir(parents=True,exist_ok=True)
        self.manifest=json.loads((MODULE/'manifest.json').read_text())
        self.patches=json.loads((MODULE/'patches.json').read_text())
        self.defaults={k:v['default'] for k,v in self.manifest['musical_controls'].items()}|dict(gate=0.,velocity=1.)
        self.report=dict(experiment='compact-controls-06',passed=False,builds={},checks=[],renders=[],listening={},
            scope='authored compact-control behavior; no reference fit, human approval or target realtime claim')
    def run(self,cmd):
        cmd=list(map(str,cmd));p=subprocess.run(cmd,cwd=ROOT,text=True,capture_output=True,timeout=120)
        with (self.out/'commands.log').open('a') as f: f.write(json.dumps(cmd)+'\n'+p.stdout+p.stderr+'\n')
        if p.returncode: raise RuntimeError(p.stderr or p.stdout or 'command failed')
        return p.stdout
    def check(self,name,passed,**data):
        self.report['checks'].append(dict(name=name,passed=bool(passed),**data))
        if not passed: raise AssertionError(name+': '+str(data))
    def build(self,label,source,vector=False):
        d=self.out/label;d.mkdir(exist_ok=True)
        faust=os.environ.get('FAUST','faust');cxx=os.environ.get('CXX','c++')
        self.run([faust,'-I',MODULE,'-I',MODULE.parent/'candidates','-e',source,'-o',d/'expanded.dsp'])
        flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vector else [])
        self.run([faust,*flags,d/'expanded.dsp','-o',d/'generated.hpp'])
        self.run([cxx,'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render'])
        controls=self.run([d/'render','--controls']);(d/'controls.tsv').write_text(controls)
        self.report['builds'][label]=dict(source=str(source.relative_to(ROOT)),source_sha256=sha(source),
            expanded_sha256=sha(d/'expanded.dsp'),generated_sha256=sha(d/'generated.hpp'),
            binary_sha256=sha(d/'render'),controls_sha256=sha(d/'controls.tsv'),faust_flags=flags)
        return d/'render'
    def render(self,label,exe,params,events,rate=48000,block=128,frames=48000):
        # Explicit initial state then sample-exact events; duplicate authored events fail.
        values={(0,k):v for k,v in params.items()};seen=set()
        for n,k,v in events:
            if (n,k) in seen: raise ValueError('duplicate authored event')
            seen.add((n,k));values[n,k]=v
        ev=sorted((n,k,v) for (n,k),v in values.items())
        score=self.out/(label+'.tsv');raw=self.out/(label+'.f32')
        score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for n,k,v in ev))
        diag=json.loads(self.run([exe,score,raw,rate,block,frames,0]))
        x=np.fromfile(raw,dtype='<f4').reshape(frames,diag['channels'])
        self.report['renders'].append(dict(label=label,build=exe.parent.name,score_sha256=sha(score),raw_sha256=sha(raw),
            diagnostics=diag,rms=float(np.sqrt(np.mean(x.astype(float)**2))),dc=float(np.mean(x)),
            max_sample_jump=float(np.max(abs(np.diff(x,axis=0))))))
        self.check(label+':finite-size',len(x)==frames and np.isfinite(x).all())
        return x[:,0] if x.shape[1]==1 else x
    def execute(self):
        self.report['source_commit']=self.run(['git','rev-parse','HEAD']).strip()
        self.report['faust_version']=self.run([os.environ.get('FAUST','faust'),'-v'])
        self.report['cxx_version']=self.run([os.environ.get('CXX','c++'),'--version'])
        engines={};vectors={}
        for profile in ('pre','post'):
            engines[profile]=self.build(profile,MODULE/(profile+'.dsp'))
            vectors[profile]=self.build(profile+'-vector',MODULE/(profile+'.dsp'),True)
            rows=(engines[profile].parent/'controls.tsv').read_text().splitlines()
            controls={r.split('\t')[0]:list(map(float,r.split('\t')[1:])) for r in rows[1:]}
            self.check(profile+':eight-musical-plus-gate-velocity',set(controls)==set(self.defaults) and rows[0]=='io\t0\t1')
            for k,c in self.manifest['musical_controls'].items():
                self.check(profile+':manifest-'+k,np.allclose(controls[k][:3],[c['min'],c['max'],c['default']],atol=1e-6,rtol=0))
        kernel=self.build('frozen-kernel',MODULE.parent/'candidates/color-04.dsp')
        diagnostic=self.build('smoothing-probe',MODULE/'control-probe.dsp')
        for rate in (44100,48000,96000):
            change=round(.02*rate);trigger=change+round(.004*rate)
            y=self.render(f'smoother-{rate}',diagnostic,dict(color=0.,gate=0.),[(change,'color',1.),(trigger,'gate',1.)],rate=rate,frames=round(.06*rate))
            c=math.exp(-1/(.003*rate));n=np.arange(1,trigger-change+1)
            self.check(f'smoother-{rate}:time-law',np.max(abs(y[change:trigger,0]-(1-c**n)))<1e-4)
            self.check(f'smoother-{rate}:onset-snap',y[trigger,0]==1. and y[change,1]==1.)
            self.check(f'smoother-{rate}:not-a-step',0<y[change,0]<.02)
            for profile,exe in engines.items():
                p=self.defaults|self.patches['anchors']['colored'];on=101;off=on+round(.23*rate)
                ev=[(on,'gate',1.),(off,'gate',0.)]
                x=self.render(f'{rate}-{profile}-base',exe,p,ev,rate=rate,frames=rate)
                self.check(f'{rate}-{profile}:silence-bound',np.max(abs(x[:on]))==0 and np.max(abs(x))<=.60001)
                raw=self.render(f'{rate}-{profile}-mapped',kernel,map_static(p,int(profile=='post')),ev,rate=rate,frames=rate)
                err=float(np.max(abs(raw-x)))
                self.check(f'{rate}-{profile}:kernel-map-parity',err<.001,max_error=err)
                for block in (1,32,127,512):
                    z=self.render(f'{rate}-{profile}-block{block}',exe,p,ev,rate=rate,frames=rate,block=block)
                    self.check(f'{rate}-{profile}:block{block}',np.max(abs(z-x))<1e-6)
                z=self.render(f'{rate}-{profile}-vector',vectors[profile],p,ev,rate=rate,frames=rate)
                self.check(f'{rate}-{profile}:vector',np.max(abs(z-x))<2e-4)
                z=self.render(f'{rate}-{profile}-half',exe,p|dict(velocity=.5),ev,rate=rate,frames=rate)
                self.check(f'{rate}-{profile}:velocity',np.max(abs(z-x*.5))<2e-6)
                z=self.render(f'{rate}-{profile}-latched',exe,p,ev+[(off+100,'velocity',0)],rate=rate,frames=rate)
                self.check(f'{rate}-{profile}:velocity-latch',np.array_equal(z,x))
                held=self.render(f'{rate}-{profile}-held',exe,p,[(on,'gate',1.)],rate=rate,frames=rate)
                self.check(f'{rate}-{profile}:release-boundary',x[off]==held[off])
                rt=map_static(p,0)['release_tau_s'];size=round(.06*rate)
                self.check(f'{rate}-{profile}:release-law',np.max(abs(x[off:off+size]-held[off:off+size]*np.exp(-np.arange(size)/(rate*rt))))<2e-5)
                second=round(.57*rate)
                z=self.render(f'{rate}-{profile}-retrigger',exe,p,ev+[(second,'gate',1.)],rate=rate,frames=rate)
                size=round(.15*rate)
                self.check(f'{rate}-{profile}:persistent-retrigger',np.max(abs(z[second:second+size]-held[on:on+size]))<2e-4)
        # Same-onset parameter locks must not lag behind a pre-set patch.
        target=self.defaults|self.patches['anchors']['long']
        for profile,exe in engines.items():
            ev=[(1001,'gate',1.),(10000,'gate',0.)]
            a=self.render(profile+'-preset-at-start',exe,target,ev,frames=24000)
            locks=ev+[(1001,k,v) for k,v in self.patches['anchors']['long'].items()]
            b=self.render(profile+'-preset-at-onset',exe,self.defaults,locks,frames=24000)
            self.check(profile+':locks-no-lag',np.max(abs(a-b))<1e-6)
        # Full 2^7 macro endpoint matrix, with pitch alternating between endpoints.
        # Short persistent runs are corner safety probes, not exhaustive musical acceptance.
        keys=[k for k in self.manifest['musical_controls'] if k!='pitch_hz']
        for profile,exe in engines.items():
            events=[]
            for i,bits in enumerate(itertools.product((0.,1.),repeat=7)):
                n=101+i*2048
                events += [(n,k,v) for k,v in zip(keys,bits)]
                events += [(n,'pitch_hz',20. if i%2==0 else 160.),(n,'gate',1.),(n+1700,'gate',0.)]
            x=self.render(profile+'-corners',exe,self.defaults,events,frames=101+128*2048+48000)
            self.check(profile+':corners-bound',np.max(abs(x))<=.60001)
        # Musical evidence: identical anchored phrases for each fixed topology.
        for profile,exe in engines.items():
            events=[];timeline=[]
            for j,(name,p) in enumerate(self.patches['anchors'].items()):
                start=j*4*48000+4800
                for h,(delta,vel,gate_seconds) in enumerate(((0,1.,.8),(60000,.6,.16),(96000,.85,1.4))):
                    n=start+delta
                    events += [(n,k,v) for k,v in p.items()]+[(n,'velocity',vel),(n,'gate',1.),(n+round(gate_seconds*48000),'gate',0.)]
                timeline.append(dict(start_seconds=start/48000,anchor=name))
            x=self.render(profile+'-anchors',exe,self.defaults,events,frames=16*48000)
            write_wav(self.out/(profile+'-anchors.wav'),x)
            self.report['listening'][profile+'-anchors']=timeline
            # Continuous 100 Hz gesture ramps in the underlying smoothed Faust path.
            events=[];timeline=[]
            for j,k in enumerate(self.patches['sweep_order']):
                n=j*4*48000+4800;p=self.patches['sweep_baseline'].copy()
                lo=20. if k=='pitch_hz' else 0.;hi=160. if k=='pitch_hz' else 1.
                p[k]=lo
                events += [(n,key,v) for key,v in p.items()]+[(n,'gate',1.),(n+163200,'gate',0.)]
                for step in range(1,301):
                    u=step/300
                    v=lo*(hi/lo)**u if k=='pitch_hz' else u
                    events.append((n+step*480,k,v))
                timeline.append(dict(start_seconds=n/48000,control=k,from_value=lo,to_value=hi))
            x=self.render(profile+'-sweeps',exe,self.defaults,events,frames=32*48000)
            write_wav(self.out/(profile+'-sweeps.wav'),x)
            self.report['listening'][profile+'-sweeps']=timeline
            events=[]
            anchors=list(self.patches['anchors'].values())
            for step in range(48):
                if step%4==0 or step%16 in (7,10,14,15):
                    n=4800+step*6000;p=anchors[(step//12)%4].copy()
                    p['shape']=min(1.,p['shape']+.25*(step%3)/2)
                    events += [(n,k,v) for k,v in p.items()]+[(n,'velocity',1. if step%4==0 else .5),
                        (n,'gate',1.),(n+int((.10 if step%4 else .19)*48000),'gate',0.)]
            x=self.render(profile+'-pattern',exe,self.defaults,events,frames=7*48000)
            write_wav(self.out/(profile+'-pattern.wav'),x)
            z=self.render(profile+'-pattern-127',exe,self.defaults,events,frames=7*48000,block=127)
            self.check(profile+':pattern-block',np.max(abs(x-z))<1e-6)
        # No secret hold or selectable topology control leaks through to the UI.
        invalid=['0 body_hold_s 1\n','0 drive_after_body 1\n','0 pitch_hz 200\n','0 drive nan\n','0 gate 0.5\n','0 shape -0.1\n']
        for i,text in enumerate(invalid):
            p=self.out/f'invalid-{i}.tsv';p.write_text(text)
            r=subprocess.run([str(engines['pre']),str(p),str(self.out/f'invalid-{i}.f32'),'48000','128','4800','0'],capture_output=True,text=True,timeout=5)
            self.check(f'invalid-{i}:rejected',r.returncode!=0)
        self.report['passed']=True
    def save(self,failure=None):
        self.report['failure']=failure
        if failure:self.report['passed']=False
        sources={}
        files=list(MODULE.glob('*'))+[MODULE.parent/'candidates/color-04.dsp',ROOT/'tools/modules/control_surface_probe.py',ROOT/'tools/modules/render.cpp']
        for p in files:
            if p.is_file():
                rel=p.relative_to(ROOT);d=self.out/'source'/rel;d.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,d);sources[str(rel)]=sha(p)
        self.report['source_files']=sources
        (self.out/'results.json').write_text(json.dumps(self.report,indent=2)+'\n')
        print(json.dumps(dict(passed=self.report['passed'],checks=len(self.report['checks']),renders=len(self.report['renders']),failure=failure)))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,default=ROOT/'build/kick-controls-06');args=p.parse_args()
    study=Study(args.out);failure=None
    try:study.execute()
    except Exception as e:failure=str(e)
    finally:study.save(failure)
    if failure:raise SystemExit(failure)
if __name__=='__main__':main()
