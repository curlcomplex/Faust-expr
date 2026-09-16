#!/usr/bin/env python3
"""Finish #97: actual Faust recomposition, declared timing, clean cost and delivery.
Only execute through the established physical-M1 serialized queue. Reuses the
same-run v10 qualification, renderer and source; no replacement synthesis.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys
import time
import zipfile

import numpy as np
from scipy.signal import resample_poly
from tx81z_patch import decode_sysex, to_controls
from tx81z_patch_qualification import Qualification as PatchQ, ROOT, sha, rms, RENDERER_SHA

RATES=(44100,48000,55930,96000)

def folded_hz(hz,rate):
    x=hz%rate
    return min(x,rate-x)

def audio_difference(x,y):
    x=np.asarray(x); y=np.asarray(y)
    if x.shape!=y.shape or not x.size or not np.isfinite(x).all() or not np.isfinite(y).all():
        raise ValueError('matched nonempty finite arrays required')
    return float(np.max(abs(x.astype(np.float64)-y))),rms(x.astype(np.float64)-y)

def source_closure(source):
    """Only the known TX81Z source tree; standard Faust imports stay external."""
    top=(ROOT/'modules/tx81z').resolve(); seen={}
    def visit(path):
        path=path.resolve()
        if not path.is_relative_to(top): raise ValueError('dependency escapes TX81Z')
        key=str(path.relative_to(ROOT))
        if key in seen:return
        seen[key]=sha(path)
        for relative in re.findall(r'\blibrary\s*\(\s*"([^"\n]+)"\s*\)',path.read_text()):
            visit(path.parent/relative)
    visit(source)
    return seen

class Completion(PatchQ):
    def __init__(self,a):
        super().__init__(a)
        self.report.update(schema='faust-expr/tx81z-completion/v1',recomposition=[],fixed_audio=[],clock=[],cost={},rate_comparison=[])

    def pair(self,name,p,ev,rate=55930,seconds=.8,block=127):
        x=self.render(name+'-baseline',self.baseline,p,ev,rate=rate,seconds=seconds,block=block)
        y=self.render(name+'-blocks',self.recomposed,p,ev,rate=rate,seconds=seconds,block=block)
        maximum,error=audio_difference(x,y)
        exact=x.tobytes()==y.tobytes()
        self.check(name+':recomposition-exact',exact,max_abs=maximum)
        self.report['recomposition'].append({'case':name,'rate':rate,'block':block,'frames':len(x),'float_bytes_exact':exact,'max_abs':maximum,'rms_error':error})
        return y

    def reuse_baseline(self):
        base=self.a.baseline.resolve(); self.base_report=json.loads((base/'results.json').read_text())
        r=self.base_report
        self.check('baseline:completed',r['status']=='completed' and bool(r['checks']) and all(c['passed'] for c in r['checks']))
        self.check('baseline:same-commit',r['commit']==self.report['commit'])
        self.check('baseline:same-compiler',r['compiler']['faust_sha256']==sha(self.a.faust))
        for path,digest in r['source_closure'].items():
            self.check('baseline:source:'+path,sha(ROOT/path)==digest)
        self.baseline=base/'scalar/render'; self.vector=base/'vector/render'
        for kind in ('scalar','vector'):
            self.check('baseline:generated:'+kind,sha(base/kind/'generated.hpp')==r['builds'][kind]['generated_sha256'])
            self.check('baseline:renderer:'+kind,sha(base/kind/'render.cpp')==RENDERER_SHA)
        self.report['baseline']={'commit':r['commit'],'results_sha256':sha(base/'results.json'),'checks':r['check_count'],'source_closure':r['source_closure'],'execution':'same Mac job, not a historical badge'}
        self.patches={}
        for name in ('Filter1','Filter2','FilterBass'):
            raw=base/'references'/(name+'.syx')
            self.check('patch:identity:'+name,sha(raw)==r['references'][name]['sha256'])
            patch=decode_sysex(raw.read_bytes()); controls,limits=to_controls(patch)
            self.patches[name]=controls
        self.report['references']=r['references']

    def composition_cases(self):
        p=self.neutral()
        # All algorithms and all waveform assignments; more than the old one-note test.
        for algorithm in range(1,9):
            for wave in range(8):
                c=p|{'algorithm':algorithm,'feedback':wave,'velocity':.8,'freq':173.21}
                for i in range(1,5):
                    c.update({f'op{i}Wave':(wave+i-1)%8,f'op{i}TL':12+6*i,f'op{i}Coarse':i,
                              f'op{i}D1R':10+i,f'op{i}SL':3*i,f'op{i}KVS':i,f'op{i}LS':4*i})
                self.pair(f'algorithm-{algorithm}-wave-{wave}',c,[(37,'gate',1),(13001,'gate',0)],seconds=.3)
        # Real patches at each supported rate, persistent retrigger and live changes.
        for rate in RATES:
            for name,controls in self.patches.items():
                c=p|controls; ev=[]
                for i,note in enumerate((36,43,48)):
                    start=round((.05+i*.65)*rate)
                    ev.extend(((start,'freq',440*2**((note-69)/12)),(start,'velocity',(.9,.35,.7)[i]),(start,'gate',1),(start+round(.5*rate),'gate',0)))
                ev.extend(((round(.62*rate),'breath',.7),(round(1.28*rate),'breath',.2)))
                ev.sort()
                for block in (1,511):
                    self.pair(name+'-'+str(rate)+'-b'+str(block),c,ev,rate=rate,seconds=2.4,block=block)
        # All LFO waves and sync modes, controls deliberately remain live in release.
        for wave in range(4):
            for sync in range(2):
                c=p|self.patches['FilterBass']|{'lfoWave':wave,'lfoSync':sync,'lfoSpeed':90,'lfoDelay':15,
                    'pModDepth':70,'pModSens':6,'aModDepth':65,'aModSens':3,'op1AME':1,
                    'bcEGBias':65,'op1EBS':4,'op2EGShift':2,'op3Reverb':3,'op1Reverb':1}
                ev=[(31,'gate',1),(11003,'algorithm',8),(22007,'feedback',6),(28001,'gate',0),
                    (30001,'breath',1),(34003,'op2TL',9),(39001,'freq',330),(39001,'velocity',.4),
                    (39001,'gate',1),(43003,'op2Mode',1),(43003,'op2FixedCRS',24),(48001,'gate',0)]
                self.pair(f'dynamic-lfo{wave}-sync{sync}',c,ev,seconds=1.2,block=127)
        # Deliver full musical renders, not just test tones. Scores are unchanged.
        base=self.a.baseline
        for name in ('Filter1','Filter2','FilterBass'):
            entry=next(x for x in self.base_report['renders'] if x['name']==name+'-music')
            old=base/entry['raw']; score=base/entry['score']; out=self.out/(name+'-recomposed.f32')
            self.check(name+':old-score-hash',sha(score)==entry['score_sha256'])
            self.check(name+':old-audio-hash',sha(old)==entry['raw_sha256'])
            diag=json.loads(self.run([self.recomposed,score,out,entry['rate'],entry['block'],entry['frames'],0]))
            self.check(name+':full-music-exact',old.read_bytes()==out.read_bytes())
            self.report['renders'].append({'name':name+'-full-recomposition','frames':entry['frames'],'rate':entry['rate'],'channels':1,'raw':out.name,'raw_sha256':sha(out),'diagnostics':diag})
            self.listen('TX81Z_v11_'+name,np.fromfile(out,dtype='<f4'),55930,'Recomposed Faust, original tutorial patch; exact baseline audio, no hardware recording')

    def clock_and_fixed(self):
        probe=self.out/'clock-fixed-probe.dsp'
        probe.write_text('import("stdfaust.lib");\nb=library("'+str(ROOT/'modules/tx81z/v11/blocks.lib')+'");\n'
            'r=nentry("range",0,0,7,1); c=nentry("coarse",0,0,63,1); f=nentry("fine",0,0,15,1);\n'
            'step=b.phaseStep(4096,1,1,f,0,0,r,c); tick=b.egClock:(_,!); count=b.egClock:(!,_);\n'
            'process=float(b.panel.fixedHz(r,c,f)),float(step),float(tick),float(count),float(b.phase(step,0));\n')
        exe=self.build('clock-fixed',probe)
        states=[(r,c,f) for r in range(8) for c in range(64) for f in range(16)]
        for rate in RATES:
            events=[]; last=None
            for frame,state in enumerate(states):
                for k,value,old in zip(('range','coarse','fine'),state,last or (-1,-1,-1)):
                    if value!=old:events.append((frame,k,value))
                last=state
            frames=rate+37
            x=self.render('clock-fixed-'+str(rate),exe,{},events,rate=rate,frames=frames,block=127)
            expected=[]
            for r,c,f in states:
                coarse=c>>2; expected.append(((coarse*16 if coarse else 8)|f)*(1<<r))
            expected=np.asarray(expected,dtype=np.int64)
            self.check('fixed:'+str(rate)+':all-panel-values',np.array_equal(x[:len(states),0],expected),cases=len(states))
            steps=x[:,1].astype(np.int64); ideal=np.floor(expected*1048576.0/rate+.5).astype(np.int64)
            # Single-precision compiler folding can differ by one accumulator unit.
            error=np.max(abs(steps[:len(states)]-ideal))
            hzerr=float(np.max(abs(steps[:len(states)]*rate/1048576.0-expected)))
            self.check('fixed:'+str(rate)+':phase-quantization',error<=1 and hzerr<=rate/1048576.0,max_phase_units=int(error),max_hz_error=hzerr)
            i=np.arange(frames,dtype=np.int64)
            ticks=((i+1)*55930//(3*rate))-(i*55930//(3*rate))
            counts=np.cumsum(ticks)&16383
            self.check('clock:'+str(rate)+':ticks',np.array_equal(x[:,2],ticks))
            self.check('clock:'+str(rate)+':count-wrap',np.array_equal(x[:,3],counts))
            expected_phase=(np.cumsum(steps)&1048575)>>10
            self.check('phase:'+str(rate)+':state',np.array_equal(x[:,4],expected_phase))
            self.report['clock'].append({'rate':rate,'frames':frames,'ticks':int(ticks.sum()),'cadence_hz':55930/3,'fixed_cases':len(states),'max_fixed_hz_error':hzerr})
        # Actual audio verifies audible Hz (including declared Nyquist folding),
        # so a numerically self-consistent but inaudible phase-unit error cannot pass.
        for rate in RATES:
            for rang,crs,fine in ((0,0,0),(0,63,15),(3,0,0),(3,63,15),(7,0,0),(7,63,15)):
                c=self.neutral()|{'op1Mode':1,'op1Range':rang,'op1FixedCRS':crs,'op1Fine':fine}
                x=self.render(f'fixed-audio-{rate}-{rang}-{crs}',self.recomposed,c,
                    [(round(.05*rate),'gate',1),(round(1.3*rate),'gate',0)],rate=rate,seconds=1.4)
                s=x[round(.15*rate):round(1.15*rate)].astype(np.float64)
                mag=abs(np.fft.rfft((s-s.mean())*np.hanning(len(s))))
                k=int(np.argmax(mag[1:])+1); fraction=0.
                if 0<k<len(mag)-1:
                    a,b,d=np.log(np.maximum(mag[k-1:k+2],1e-30)); fraction=.5*(a-d)/(a-2*b+d)
                measured=(k+fraction)*rate/len(s)
                coarse=crs>>2; intended=((coarse*16 if coarse else 8)|fine)*(1<<rang)
                expected=folded_hz(intended,rate)
                self.check(f'fixed-audio:{rate}:{rang}:{crs}',abs(measured-expected)<1.,measured_hz=measured,expected_folded_hz=expected)
                self.report['fixed_audio'].append({'rate':rate,'range':rang,'coarse':crs,'fine':fine,'intended_hz':intended,'expected_folded_hz':expected,'measured_hz':measured,'aliases':intended>rate/2})

    def host_rate_evidence(self):
        signals={}
        c=self.neutral()|{'algorithm':1,'feedback':6,'op2TL':18,'op3TL':24,'op4TL':24,
            'op2Coarse':2,'op3Coarse':3,'op4Coarse':5,'pModDepth':30,'pModSens':3,'lfoSpeed':85}
        for rate in RATES:
            ev=[(round(.05*rate),'gate',1),(round(.65*rate),'gate',0)]
            x=self.render('rate-feedback-'+str(rate),self.recomposed,c,ev,rate=rate,seconds=1.)
            g=math.gcd(48000,rate); signals[rate]=resample_poly(x.astype(np.float64),48000//g,rate//g)
        reference=signals[55930]
        for rate,x in signals.items():
            n=min(len(x),len(reference)); maximum,error=audio_difference(x[:n],reference[:n])
            self.report['rate_comparison'].append({'rate':rate,'two_sample_feedback_us':2e6/rate,'rms_residual_vs_55930':error,'max_abs_residual':maximum,'alignment':'none; same event seconds, polyphase conversion to48k; not a fidelity pass/fail'})
        # Same musical notes across host rates. Differences remain audible, not normalized away.
        sound=[]
        for rate in RATES:sound.extend((signals[rate],np.zeros(12000)))
        self.listen('TX81Z_v11_host_rate_comparison',np.concatenate(sound),48000,'44100,48000,55930,96000Hz host synthesis in that order; equal gain; two-host-sample feedback differs')

    def clean_cost(self):
        p=self.neutral()|{'algorithm':6,'feedback':5,'pModDepth':40,'pModSens':4,'lfoSpeed':82,'aModDepth':55,'aModSens':2}
        for i in range(1,5):p.update({f'op{i}TL':12+i*4,f'op{i}Coarse':i,f'op{i}Wave':i-1,f'op{i}AME':1})
        score=self.out/'benchmark.tsv'; score.write_text(''.join(f'0\t{k}\t{v}\n' for k,v in p.items()))
        for name,directory in (('baseline',self.a.baseline/'scalar'),('recomposed',self.recomposed.parent)):
            dest=self.out/('bench-'+name); dest.mkdir()
            for filename in ('generated.hpp','render.cpp'):shutil.copy2(directory/filename,dest/filename)
            shutil.copy2(ROOT/'tools/modules/tx81z_clean_bench.cpp',dest/'bench.cpp')
            self.run(['c++','-std=c++17','-O2','-ffp-contract=off','-I'+str(dest),dest/'bench.cpp','-o',dest/'bench'])
            record=json.loads(self.run([dest/'bench',score]))
            self.check('cost:'+name+':no-new-in-compute',not any(record['operator_new_calls']))
            text=(dest/'generated.hpp').read_text(); arrays=[]
            for typ,symbol,count in re.findall(r'^(?:const\s+static|static\s+const|static)\s+(int|float|double)\s+(\w+)\[(\d+)\]',text,re.M):
                arrays.append({'symbol':symbol,'elements':int(count),'bytes':int(count)*(8 if typ=='double' else 4)})
            record['declared_static_arrays']=arrays; record['declared_static_bytes']=sum(a['bytes'] for a in arrays)
            record['median_ns_per_frame']=float(np.median(record['compute_ns']))/record['frames_per_trial']
            record['median_one_voice_cpu_fraction']=record['median_ns_per_frame']*48000/1e9
            record['scope']='clean whole-batch offline compute; instance sizeof plus declared static tables; excludes UI/host/stack and library residency; new hook is not malloc tracing'
            record['generated_sha256']=sha(dest/'generated.hpp')
            self.report['cost'][name]=record
        a=self.report['cost']['baseline']; b=self.report['cost']['recomposed']
        self.check('memory:recomposition-no-instance-growth',a['instance_bytes']==b['instance_bytes'])
        self.check('memory:recomposition-no-static-growth',a['declared_static_bytes']==b['declared_static_bytes'])

    def alternate_and_package(self):
        ev=[]
        for i,(note,depth,wave) in enumerate(zip((36,43,48,46,36,55,43,36),(.2,.35,.5,.65,.8,.95,.6,.4),(0,1,2,3,4,5,6,7))):
            start=round((.1+i*.65)*48000)
            ev.extend(((start,'freq',440*2**((note-69)/12)),(start,'index',depth),(start,'wave',wave),(start,'gate',1),(start+24000,'gate',0)))
        x=self.render('alternate-music',self.alternate,{'gate':0},ev,rate=48000,seconds=6.3)
        self.check('alternate:audible',rms(x)>.0001)
        self.listen('TX81Z_v11_two_operator_blocks',x,48000,'Original two-operator composition from extracted blocks; ascending depth, all eight modulator waveforms; not another instrument emulation')
        neutral=[(0,'freq',110),(0,'gate',1),(60000,'gate',0)]
        data=self.render('alternate-control',self.alternate,{},neutral,rate=48000,frames=96000)
        silent=self.render('alternate-index0',self.alternate,{'index':0},neutral,rate=48000,frames=96000)
        self.check('alternate:index-affects-sound',np.max(abs(data-silent))>1e-5)
        closure={}
        for source in ('modules/tx81z/v10/voice.dsp','modules/tx81z/v11/recomposed_voice.dsp','modules/tx81z/v11/alternate.dsp'):
            closure.update(source_closure(ROOT/source))
        for relative in closure:
            target=self.out/'delivery'/relative; target.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(ROOT/relative,target)
        for relative in ('modules/tx81z/YMFM-LICENSE.txt','modules/tx81z/v10/REFERENCE.md','modules/tx81z/v10/patch_sources.json','modules/tx81z/v11/README.md','tools/modules/tx81z_patch.py','tools/modules/tx81z_completion.py','tools/modules/tx81z_clean_bench.cpp'):
            target=self.out/'delivery'/relative; target.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(ROOT/relative,target)
        shutil.copy2(self.a.baseline/'delivery/TX81Z_v10.dsp',self.out/'delivery/TX81Z_v10_baseline.dsp')
        for path in (self.out/'listening').glob('*.wav'):shutil.copy2(path,self.out/'delivery'/path.name)
        self.report['source_closure']=closure
        self.report['delivery_scope']='source blocks, exact baseline and our newly synthesized recordings; excludes private controller files, third-party SysEx and firmware'

    def execute(self):
        self.check('execution:physical-M1',sys.platform=='darwin' and os.getenv('RUNNER_NAME')=='Felix-M1-Pro-CURLOP')
        self.report['commit']=self.run(['git','rev-parse','HEAD']).strip()
        self.check('renderer:unchanged',sha(self.a.renderer)==RENDERER_SHA)
        self.report['compiler']={'faust':self.run([self.a.faust,'--version']),'faust_sha256':sha(self.a.faust),'cxx':self.run(['c++','--version'])}
        self.reuse_baseline()
        print('Compile extracted voice once; reuse same-run v10 baseline',flush=True)
        self.recomposed=self.build('recomposed',ROOT/'modules/tx81z/v11/recomposed_voice.dsp')
        self.alternate=self.build('alternate',ROOT/'modules/tx81z/v11/alternate.dsp')
        print('Full-patch and dynamic recomposition',flush=True); self.composition_cases()
        print('Fixed-field grid, actual audible Hz and envelope clocks',flush=True); self.clock_and_fixed()
        self.host_rate_evidence()
        print('Clean compute/memory and playable block delivery',flush=True); self.clean_cost(); self.alternate_and_package()
        self.report['status']='completed'

    def save(self):
        self.report['elapsed_seconds']=time.monotonic()-self.start
        self.report['commands']=self.commands
        self.report['check_count']=len(self.report['checks']); self.report['render_count']=len(self.report['renders'])
        self.report['limits'].append('nonzero release extension uses per-operator native thresholds, not independently verified OP1-shared firmware trigger')
        (self.out/'results.json').write_text(json.dumps(self.report,indent=2)+'\n')
        if self.report['status']=='completed':
            shutil.copy2(self.out/'results.json',self.out/'delivery/completion-result.json')
            manifest={str(p.relative_to(self.out/'delivery')):sha(p) for p in sorted((self.out/'delivery').rglob('*')) if p.is_file()}
            (self.out/'delivery/MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
            with zipfile.ZipFile(self.out/'TX81Z_v11_completed_delivery.zip','w',zipfile.ZIP_DEFLATED) as z:
                for p in sorted((self.out/'delivery').rglob('*')):
                    if p.is_file():z.write(p,p.relative_to(self.out/'delivery'))
        manifest={str(p.relative_to(self.out)):sha(p) for p in sorted(self.out.rglob('*')) if p.is_file() and p.name!='SHA256SUMS.json'}
        (self.out/'SHA256SUMS.json').write_text(json.dumps(manifest,indent=2)+'\n')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for key in ('out','faust','renderer','ymfm','baseline'):ap.add_argument('--'+key,type=Path,required=True)
    a=ap.parse_args()
    for key in ('out','faust','renderer','ymfm','baseline'):setattr(a,key,getattr(a,key).resolve())
    q=Completion(a)
    try:q.execute()
    except Exception as e:q.report['status']='failed'; q.report['error']=str(e); raise
    finally:q.save()
    print(json.dumps({'status':q.report['status'],'checks':q.report['check_count'],'renders':q.report['render_count'],'pairs':len(q.report['recomposition'])}))
if __name__=='__main__':main()
