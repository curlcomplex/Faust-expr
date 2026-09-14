"""Independent equation cross-check, preservation and raw listening comparisons.

Only the comparison program is a procedural model. All instrument audio is
rendered from the committed Faust sources. No hardware audio is used here.
"""
from pathlib import Path
import argparse
import itertools
import json
import os
import subprocess
import traceback
import numpy as np
from synth_batch import SynthLab, ROOT, phrase
from synth_recovery import SOURCES, passed, tone_controls
from acid_batch import controls
from hats_v2_delivery import command, digest

BASELINE='91f6a9671fa13026e69b62731c1fbdd0a592d23b'


def tone_ok(audio, sr, frequency):
    """A full-band dominant-frequency guard, not a synth-fidelity score."""
    y=np.asarray(audio,dtype=float)
    if y.ndim!=1 or len(y)<sr//2 or not np.isfinite(y).all():
        return False
    centered=y-y.mean()
    rms=float(np.sqrt(np.mean(centered*centered)))
    if rms<1e-4 or abs(y.mean())>.1*rms+1e-6:
        return False
    spectrum=abs(np.fft.rfft(centered*np.hanning(len(y))))
    measured=float(np.argmax(spectrum[1:])+1)*sr/len(y)
    return abs(measured-frequency)<=max(2.0,1.5*sr/len(y))


def run(out):
    lab=SynthLab(out); c=lab.check
    (out/'audition').mkdir(exist_ok=True)
    lab.report.update(commit=os.getenv('GITHUB_SHA','local'),
        hardware_approved=False, human_approved=False, selected_for_promotion=False,
        reference_kind='Independent procedural equation cross-check; NOT hardware',
        audition_order={},filter_comparisons=[])
    try:
        # The workflow fetches this exact public baseline. No other repo is read.
        original=subprocess.check_output(['git','ls-tree','-r','--name-only',BASELINE],cwd=ROOT,text=True).splitlines()
        diff=subprocess.check_output(['git','diff','--no-renames','--name-status',BASELINE,'HEAD','--'],cwd=ROOT,text=True).splitlines()
        changed=[row for row in diff if not row.startswith('A\t')]
        c('all-baseline-files-preserved',bool(original) and not changed,
          original_file_count=len(original),unexpected_changes=changed)
        wrapper=out/'ladder-test.dsp'
        lib=ROOT/'modules/minimoog/v3/ladder.lib'
        wrapper.write_text(f'm=library("{lib}");\ncutoff=hslider("cutoff",800,20,44000,1);\nres=hslider("resonance",0,0,1,.001);\nprocess=m.vcf(cutoff,res);\n')
        exe=lab.build('ladder-component',wrapper)
        reference=out/'component-reference'
        command([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off',
                 ROOT/'tools/modules/mini_substep_reference.cpp','-o',reference])
        c('component-one-in-one-out',controls(exe)[0]==(1,1))
        frames=4096; t=np.arange(frames,dtype=float)
        stimulus=(.08*np.sin(.137*t)+.03*np.sin(.593*t)).astype(np.float32)
        stimulus[:32]=0; stimulus[32]=.2
        input_file=out/'reference-input.f32'; stimulus.astype('<f4').tofile(input_file)
        for sr in (44100,48000,96000):
            for cf,res in itertools.product((30,800,8000,16000,.4*sr),(0,.98)):
                tag=f'component-{sr}-{cf}-{res}'
                actual=lab.render(tag,exe,{'cutoff':cf,'resonance':res},sr=sr,
                                  frames=frames,inputs=stimulus[:,None])[:,0]
                raw=out/(tag+'-reference.f32')
                command([reference,input_file,raw,sr,cf,res])
                expected=np.fromfile(raw,'<f4')
                error=float(np.max(abs(actual-expected)))
                c(tag+':equation-cross-check',error<2e-4,max_absolute_error=error)
                lab.report['filter_comparisons'].append(dict(sr=sr,cutoff=cf,resonance=res,
                    max_absolute_error=error,reference_sha256=digest(raw)))
        # Pitch in the audible output, not only diagnostics/control telemetry.
        for name,(src,base) in SOURCES.items():
            exe=lab.build(name+'-pitch',src)
            for sr,f in itertools.product((44100,48000,96000),(20,55,220,1760,8000)):
                p=tone_controls(name,base)|dict(freq=f,cutoff=16000)
                y=lab.render(f'{name}-pitch-{sr}-{f}',exe,p,[(round(.1*sr),'gate',1)],
                             sr=sr,frames=2*sr)[:,0]
                c(f'{name}:audible-pitch-{sr}-{f}',tone_ok(y[sr:],sr,f))
        # Auditions compare defaults under the same phrase without normalization.
        banks={
          'mini_v1_v2_v3': [('mini-v1',ROOT/'modules/minimoog/v1/voice.dsp'),
                           ('mini-v2',ROOT/'modules/minimoog/v2/voice.dsp'),
                           ('mini-v3',ROOT/'modules/minimoog/v3/voice.dsp')],
          'juno60_reference_v1_v2_pr89': [('juno60-v1',ROOT/'modules/juno-60/v1/voice.dsp'),
                           ('juno60-v2',ROOT/'modules/juno-60/v2/voice.dsp'),
                           ('juno60-pr89',SOURCES['juno60-pr89'][0])]}
        for bank,versions in banks.items():
            audio=[]; order=[]
            for name,src in versions:
                exe=lab.build(name+'-audition',src)
                p={k:v[2] for k,v in controls(exe)[1].items()}
                y=lab.render(name+'-comparison',exe,p,
                    phrase(root=110 if bank.startswith('mini') else 220),frames=384000)
                order.append(dict(name=name,source=str(src.relative_to(ROOT)),source_sha256=digest(src),
                                  start_seconds=8*len(audio),duration_seconds=8,controls=p))
                audio.append(y)
            lab.wav(bank+'.wav',np.concatenate(audio))
            lab.report['audition_order'][bank]=order
        c('component-coverage-complete',len(lab.report['filter_comparisons'])==30)
    except Exception:
        lab.report['exception']=traceback.format_exc(); c('verification-completed',False)
        print(lab.report['exception'],flush=True)
    lab.report['passed']=passed(lab.report['checks'])
    lab.report['failures']=[q['name'] for q in lab.report['checks'] if q.get('passed') is not True]
    (out/'results.json').write_text(json.dumps(lab.report,indent=2))
    errors=[r['max_absolute_error'] for r in lab.report['filter_comparisons']]
    print('VERIFICATION_SUMMARY',json.dumps(dict(passed=lab.report['passed'],
        checks=len(lab.report['checks']),renders=len(lab.report['renders']),
        max_component_error=max(errors) if errors else None,failures=lab.report['failures'])),flush=True)
    return 0 if lab.report['passed'] else 1


if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args();raise SystemExit(run(args.out))
