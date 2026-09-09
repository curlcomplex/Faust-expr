"""Compile/test color-04 with actual Faust; no recordings or network required."""
from __future__ import annotations
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import sys
import numpy as np
from body_gate_probe import Probe, DEFAULTS, ROOT, sha

SOURCE = ROOT/'modules/kick-pm/candidates/color-04.dsp'
COLORS = dict(square=0., triangle=0., mod_envelope=.7, mod_tau_s=.06,
              drive=0., drive_after_body=0., feedback_mode=0.)
PARAMETERS = DEFAULTS | COLORS

class ColorProbe(Probe):
    def build_kernel(self, name, source, vector=False):
        d=self.out/name; d.mkdir(exist_ok=True)
        faust,cxx=os.environ.get('FAUST','faust'),os.environ.get('CXX','c++')
        self.report['faust_version']=self.run([faust,'-v'])
        self.report['cxx_version']=self.run([cxx,'--version'])
        self.run([faust,'-e',source,'-o',d/'expanded.dsp'])
        flags=['-lang','cpp','-single','-cn','ModuleDSP']
        if vector: flags+=['-vec','-lv','0','-vs','32']
        self.run([faust,*flags,d/'expanded.dsp','-o',d/'generated.hpp'])
        native=['-std=c++17','-O2','-ffp-contract=off','-I'+str(d)]
        self.run([cxx,*native,ROOT/'tools/modules/render.cpp','-o',d/'render'])
        self.run([cxx,*native,'-shared','-fPIC',ROOT/'tools/modules/color_fit_api.cpp','-o',d/'kernel.so'])
        controls=self.run([d/'render','--controls']); (d/'controls.tsv').write_text(controls)
        self.report['builds'][name]=dict(source=str(source.relative_to(ROOT)),source_sha256=sha(source),
            expanded_sha256=sha(d/'expanded.dsp'),generated_sha256=sha(d/'generated.hpp'),
            binary_sha256=sha(d/'render'),library_sha256=sha(d/'kernel.so'),
            controls_sha256=sha(d/'controls.tsv'),faust_flags=flags,native_flags=native)
        return d/'render'

    def execute(self):
        scalar=self.build_kernel('scalar',SOURCE)
        vector=self.build_kernel('vector',SOURCE,True)
        body=self.build_kernel('body',ROOT/'modules/kick-pm/experiments/body-02.dsp')
        for rate in (44100,48000,96000):
            on,off,frames=101,101+round(.337*rate),rate
            ev=[(on,'gate',1),(off,'gate',0)]
            clean=self.render(f'{rate}-body',body,DEFAULTS,rate,128,frames,ev)
            neutral=self.render(f'{rate}-neutral',scalar,PARAMETERS,rate,128,frames,ev)
            self.check(f'{rate}:clean-body-preserved',np.max(abs(clean-neutral))<3e-6)
            for stage in (0,1):
                p=PARAMETERS|dict(square=.6,triangle=.75,mod_envelope=.9,mod_tau_s=.045,
                                  drive=.45,drive_after_body=stage,feedback_mode=1.)
                ref=self.render(f'{rate}-{stage}-base',scalar,p,rate,128,frames,ev)
                self.check(f'{rate}-{stage}:bounded',np.max(abs(ref))<=p['level']+1e-6)
                self.check(f'{rate}-{stage}:pre-silent',np.max(abs(ref[:on]))==0)
                for block in (1,32,64,127,256,512):
                    y=self.render(f'{rate}-{stage}-block-{block}',scalar,p,rate,block,frames,ev)
                    self.check(f'{rate}-{stage}:block-{block}',np.max(abs(y-ref))<=1e-6)
                y=self.render(f'{rate}-{stage}-vector',vector,p,rate,128,frames,ev)
                self.check(f'{rate}-{stage}:vector',np.max(abs(y-ref))<=2e-4)
                held=self.render(f'{rate}-{stage}-held',scalar,p,rate,128,frames,[(on,'gate',1)])
                self.check(f'{rate}-{stage}:release-boundary',ref[off]==held[off])
                sl=slice(off,off+round(.08*rate)); gain=np.exp(-np.arange(round(.08*rate))/(rate*p['release_tau_s']))
                self.check(f'{rate}-{stage}:release-law',np.max(abs(ref[sl]-held[sl]*gain))<2e-6)
                half=self.render(f'{rate}-{stage}-half',scalar,p|{'velocity':.5},rate,127,frames,ev)
                self.check(f'{rate}-{stage}:velocity',np.max(abs(half-.5*ref))<2e-6)
                second=round(.51*rate)
                retrig=self.render(f'{rate}-{stage}-retrigger',scalar,p,rate,127,frames,
                                    ev+[(second,'gate',1)])
                n=round(.2*rate)
                self.check(f'{rate}-{stage}:fresh-retrigger',np.max(abs(retrig[second:second+n]-held[on:on+n]))<3e-6)
            # Zero drive removes stage choice without removing PM.
            p=PARAMETERS|dict(square=.6,triangle=.7,feedback_mode=1.,drive=0.)
            a=self.render(f'{rate}-zero-drive-pre',scalar,p,rate,128,frames,ev)
            b=self.render(f'{rate}-zero-drive-post',scalar,p|{'drive_after_body':1.},rate,128,frames,ev)
            self.check(f'{rate}:drive-zero-identity',np.max(abs(a-b))<1e-6)
        events=[]
        for i in range(20):
            on=103+i*773
            events += [(on,'frequency_hz',30+i*6),(on,'square',(i%4)/3),
                       (on,'triangle',1-(i%4)/3),(on,'drive_after_body',i%2),
                       (on,'gate',1),(on+255,'gate',0)]
        p=PARAMETERS|dict(drive=.5,feedback_mode=1.)
        a=self.render('dynamic',scalar,p,48000,128,48000,events)
        for block in (1,127,512):
            b=self.render(f'dynamic-{block}',scalar,p,48000,block,48000,events)
            self.check(f'dynamic:{block}',np.max(abs(a-b))<1e-6)
        b=self.render('dynamic-vector',vector,p,48000,128,48000,events)
        self.check('dynamic:vector',np.max(abs(a-b))<2e-4)
        for i in range(16):
            p=PARAMETERS|dict(frequency_hz=200 if i&1 else 20,pitch_amount_hz=2000,
                pitch_tau_s=.002 if i&2 else .2,body_tau_s=2 if i&4 else .005,
                square=1.,triangle=1.,drive=1.,feedback_mode=1.,drive_after_body=i%2,
                mod_envelope=1. if i&8 else 0.,phase_cycles=.21)
            y=self.render(f'corner-{i}',scalar,p,48000,127,24000,[(101,'gate',1),(17777,'gate',0)])
            self.check(f'corner-{i}:bounded',np.max(abs(y))<=p['level']+1e-6)
        # Fitting interface must render the same state evolution as the score CLI.
        rows=(self.out/'scalar/controls.tsv').read_text().splitlines()[1:]
        keys=[r.split('\t')[0] for r in rows]
        f=ctypes.CDLL(str(self.out/'scalar/kernel.so')).color_render
        f.argtypes=[ctypes.POINTER(ctypes.c_double),ctypes.c_int,ctypes.c_int,ctypes.c_int,
                    ctypes.c_int,ctypes.c_int,ctypes.POINTER(ctypes.c_float)];f.restype=ctypes.c_int
        p=PARAMETERS|dict(square=.43,triangle=.62,drive=.4,drive_after_body=1.,feedback_mode=1.)
        values=np.array([(p|{'gate':0})[k] for k in keys],dtype=np.float64)
        output=np.empty(44100,dtype=np.float32)
        def call(vals=values,off=14862,block=128):
            return f(vals.ctypes.data_as(ctypes.POINTER(ctypes.c_double)),len(vals),44100,len(output),
                     off,block,output.ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
        self.check('ffi:return',call()==0)
        y=self.render('ffi-replay',scalar,p,44100,128,44100,[(0,'gate',1),(14862,'gate',0)])
        self.check('ffi:score-parity',np.max(abs(output-y))<1e-6)
        for label,key,val in [('nan','square',float('nan')),('range','square',1.1),('boolean','feedback_mode',.5)]:
            v=values.copy();v[keys.index(key)]=val
            self.check('ffi:reject-'+label,call(v)!=0)
        self.check('ffi:reject-off',call(off=0)!=0)
        self.check('ffi:reject-block',call(block=0)!=0)
        self.report['passed']=True

    def save(self):
        self.report['source_commit']=self.run(['git','rev-parse','HEAD']).strip()
        self.report['scope']='Color candidate correctness/ablations, not hardware fidelity or realtime qualification'
        self.report['source_files']={}
        for p in (SOURCE,Path(__file__),ROOT/'tools/modules/render.cpp',ROOT/'tools/modules/color_fit_api.cpp',
                  ROOT/'tools/modules/body_gate_probe.py',ROOT/'modules/kick-pm/experiments/body-02.dsp'):
            rel=p.relative_to(ROOT);dest=self.out/'source'/rel;dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(p,dest);self.report['source_files'][str(rel)]=sha(p)
        (self.out/'results.json').write_text(json.dumps(self.report,indent=2)+'\n')
        print(json.dumps(dict(passed=self.report['passed'],checks=len(self.report['checks']),
                             renders=len(self.report['renders']),failure=self.report.get('failure'))))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);args=p.parse_args()
    probe=ColorProbe(args.out)
    try:probe.execute()
    except Exception as e:probe.report['failure']=str(e)
    finally:probe.save()
    if not probe.report['passed']:raise SystemExit(1)
if __name__=='__main__':main()
