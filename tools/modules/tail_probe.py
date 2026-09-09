"""Actual-Faust hold-stage ablation; no hardware audio or fitting in CI."""
from __future__ import annotations
import argparse, json, shutil
from pathlib import Path
import numpy as np
from color_probe import ColorProbe, PARAMETERS
from body_gate_probe import ROOT, sha

SOURCE=ROOT/'modules/kick-pm/candidates/tail-05.dsp'

class TailProbe(ColorProbe):
    def execute(self):
        scalar=self.build_kernel('scalar',SOURCE)
        vector=self.build_kernel('vector',SOURCE,True)
        prior=self.build_kernel('color04',ROOT/'modules/kick-pm/candidates/color-04.dsp')
        for rate in (44100,48000,96000):
            on,off,frames=101,101+round(.337*rate),rate
            ev=[(on,'gate',1),(off,'gate',0)]
            for stage in (0,1):
                p=PARAMETERS|dict(square=.6,triangle=.7,drive=.45,feedback_mode=1.,drive_after_body=stage)
                a=self.render(f'{rate}-{stage}-prior',prior,p,rate,128,frames,ev)
                b=self.render(f'{rate}-{stage}-zero',scalar,p|{'body_hold_s':0.},rate,128,frames,ev)
                self.check(f'{rate}-{stage}:zero-hold-preserves-prior',np.max(abs(a-b))<3e-6)
                p=p|{'body_hold_s':.47}
                ref=self.render(f'{rate}-{stage}-held',scalar,p,rate,128,frames,ev)
                self.check(f'{rate}-{stage}:bounded',np.max(abs(ref))<=p['level']+1e-6)
                self.check(f'{rate}-{stage}:initial-silence',np.max(abs(ref[:on]))==0)
                for block in (1,127,512):
                    x=self.render(f'{rate}-{stage}-block{block}',scalar,p,rate,block,frames,ev)
                    self.check(f'{rate}-{stage}:block{block}',np.max(abs(x-ref))<1e-6)
                x=self.render(f'{rate}-{stage}-vector',vector,p,rate,128,frames,ev)
                self.check(f'{rate}-{stage}:vector',np.max(abs(x-ref))<2e-4)
                sustain=self.render(f'{rate}-{stage}-sustain',scalar,p,rate,128,frames,[(on,'gate',1)])
                self.check(f'{rate}-{stage}:release-at-zero',ref[off]==sustain[off])
                n=round(.08*rate);g=np.exp(-np.arange(n)/(rate*p['release_tau_s']))
                self.check(f'{rate}-{stage}:release-during-hold',np.max(abs(ref[off:off+n]-sustain[off:off+n]*g))<2e-6)
                second=round(.6*rate)
                x=self.render(f'{rate}-{stage}-retrigger',scalar,p,rate,127,frames,ev+[(second,'gate',1)])
                self.check(f'{rate}-{stage}:restart',np.max(abs(x[second:second+n]-sustain[on:on+n]))<3e-6)
                x=self.render(f'{rate}-{stage}-half',scalar,p|{'velocity':.5},rate,127,frames,ev)
                self.check(f'{rate}-{stage}:velocity',np.max(abs(x-ref*.5))<2e-6)
            # Closed-form RELATIVE envelope oracle; oscillator phase cancels.
            # Pre-body shaping is unchanged by hold, so a scalar envelope ratio
            # must explain the whole difference, including with PM and drive.
            p=PARAMETERS|dict(body_tau_s=.4,square=.5,triangle=.6,drive=.4,feedback_mode=1.,drive_after_body=0.)
            a=self.render(f'{rate}-oracle-zero',scalar,p|{'body_hold_s':0.},rate,128,frames,[(on,'gate',1)])
            b=self.render(f'{rate}-oracle-hold',scalar,p|{'body_hold_s':.23},rate,127,frames,[(on,'gate',1)])
            t=np.maximum(0,(np.arange(frames)-on)/rate)
            expected=a*np.exp(np.minimum(t,.23)/p['body_tau_s'])
            self.check(f'{rate}:relative-envelope-law',np.max(abs(b-expected))<4e-6)
            # Zero-hold, no-color path must still match the old clean regression.
        events=[]
        for i in range(12):
            n=101+i*1703
            events += [(n,'body_hold_s',(i%4)*.1),(n,'frequency_hz',40+i*4),
                       (n,'gate',1),(n+833,'gate',0)]
        p=PARAMETERS|dict(body_hold_s=.4,square=.5,triangle=.6,drive=.5,feedback_mode=1.)
        a=self.render('locks',scalar,p,48000,128,48000,events)
        for block in (1,127,512):
            b=self.render(f'locks-{block}',scalar,p,48000,block,48000,events)
            self.check(f'locks:{block}',np.max(abs(a-b))<1e-6)
        for i in range(8):
            p=PARAMETERS|dict(body_hold_s=2. if i&1 else 0.,body_tau_s=2. if i&2 else .005,
                frequency_hz=200. if i&4 else 20.,pitch_amount_hz=2000.,square=1.,triangle=1.,
                drive=1.,feedback_mode=1.,drive_after_body=i%2)
            x=self.render(f'corner{i}',scalar,p,48000,127,144000,[(101,'gate',1),(105001,'gate',0)])
            self.check(f'corner{i}:bounded',np.max(abs(x))<=p['level']+1e-6)
        self.report['passed']=True

    def save(self):
        self.report['source_commit']=self.run(['git','rev-parse','HEAD']).strip()
        self.report['scope']='Hold hypothesis correctness; no hardware fidelity, listening or realtime acceptance'
        self.report['source_files']={}
        for p in (SOURCE,Path(__file__),ROOT/'tools/modules/color_probe.py',ROOT/'tools/modules/body_gate_probe.py',
                  ROOT/'tools/modules/render.cpp',ROOT/'tools/modules/color_fit_api.cpp',ROOT/'modules/kick-pm/candidates/color-04.dsp'):
            rel=p.relative_to(ROOT); dest=self.out/'source'/rel;dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(p,dest);self.report['source_files'][str(rel)]=sha(p)
        (self.out/'results.json').write_text(json.dumps(self.report,indent=2)+'\n')
        print(json.dumps(dict(passed=self.report['passed'],checks=len(self.report['checks']),renders=len(self.report['renders']),failure=self.report.get('failure'))))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    probe=TailProbe(a.out)
    try:probe.execute()
    except Exception as e:probe.report['failure']=str(e)
    finally:probe.save()
    if not probe.report['passed']:raise SystemExit(1)
if __name__=='__main__':main()
