"""Consolidated 606/909 tom equivalence and retained 808 auxiliary delivery.
All audio is compiled Faust/C++; Python supplies scores, comparisons and summing.
"""
from pathlib import Path
import argparse
import itertools
import json
import os
import platform
import time
import numpy as np
from synth_batch import SynthLab
from acid_batch import controls
from hats_v2_delivery import command, digest

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / 'modules/analog-classics/percussion-finish/manifest.json'
PLAN = json.loads(MANIFEST.read_text())
NOTE = [(480, 'gate', 1), (481, 'gate', 0)]

def equal(lab, name, a, b):
    error = float(np.max(np.abs(a - b)))
    lab.check(name, np.array_equal(a, b), max_absolute=error)

def pulse_score(rate):
    """Persistent retriggers and in-tail edits, within all legacy slot ranges."""
    return [(round(.01*rate), 'gate', 1), (round(.01*rate)+1, 'gate', 0),
            (round(.04*rate), 'tone', .9), (round(.08*rate), 'noise', .6),
            (round(.17*rate), 'velocity', .43), (round(.17*rate), 'gate', 1),
            (round(.17*rate)+1, 'gate', 0), (round(.31*rate), 'gate', 1),
            (round(.31*rate)+1, 'gate', 0)]

def run(out):
    lab = SynthLab(out)
    lab.out.joinpath('audition').mkdir(exist_ok=True)
    check, render = lab.check, lab.render
    lab.report.update(version='percussion-finish-0.1.0',
        commit=os.environ.get('GITHUB_SHA', 'local-snapshot'),
        source_candidate=True, hardware_approved=False, human_approved=False,
        host_integrated=False, device_qualified=False, selections=PLAN,
        reference_comparison='Legacy source equivalence only; hardware/full-oracle comparison pending')
    lab.report['environment'] = dict(platform=platform.platform(), machine=platform.machine(),
        faust=command([os.getenv('FAUST', 'faust'), '--version']),
        cxx=command([os.getenv('CXX', 'c++'), '--version']))
    start = time.perf_counter()
    exes, captured = {}, {}
    try:
        # One executable for each family, not a hidden low/mid/high selector bank.
        for name in ('tom606', 'tom909'):
            print('CONSOLIDATE', name, flush=True)
            item = PLAN['selections'][name]
            src = ROOT / item['source']
            p = item['defaults']
            exe = lab.build(name, src)
            exes[name] = exe
            vec = lab.build(name+'-vector', src, True)
            io, ui = controls(exe)
            captured[name] = ui
            check(name+':single-note-io', io == (0, 1))
            check(name+':canonical-controls', set(ui) == set(p) and {'gate','freq','velocity'} <= set(ui))
            check(name+':defaults', all(abs(ui[k][2]-v) < 1e-5 for k,v in p.items()))
            legacy_exes = {slot: lab.build(name+'-'+slot, ROOT/path)
                           for slot,path in item['legacy_sources'].items()}
            for slot, old in legacy_exes.items():
                old_io, old_ui = controls(old)
                old_p = {k:v[2] for k,v in old_ui.items()}
                check(name+':range-union-'+slot, old_io == io and all(
                    ui[k][0] <= v[0] and ui[k][1] >= v[1] for k,v in old_ui.items()))
                # Default settings at three rates, including real persistent edits.
                for rate in (44100,48000,96000):
                    ev = pulse_score(rate)
                    a = render(f'{name}-{slot}-{rate}-legacy', old, old_p, ev, sr=rate, frames=rate)
                    b = render(f'{name}-{slot}-{rate}-unified', exe, old_p, ev, sr=rate, frames=rate)
                    equal(lab, f'{name}:legacy-trace-{slot}-{rate}', a, b)
                # Every previously auditioned slot preset, not just defaults.
                for title, pre in item['presets'].items():
                    if not title.startswith(slot+'-'):
                        continue
                    a = render(name+'-'+title+'-old', old, pre, NOTE)
                    b = render(name+'-'+title+'-new', exe, pre, NOTE)
                    equal(lab, name+':legacy-preset-'+title, a, b)
            baseline = render(name+'-baseline', exe, p, NOTE)
            for block in (1,32,127,512):
                y = render(name+'-block-'+str(block), exe, p, NOTE, block=block)
                equal(lab,name+':block-'+str(block),baseline,y)
            y = render(name+'-vector',vec,p,NOTE)
            check(name+':vector-parity', np.max(abs(y-baseline)) < 3e-5,
                  max_absolute=float(np.max(abs(y-baseline))))
            check(name+':never-triggered',not np.any(render(name+'-never',exe,p,frames=12000)))
            check(name+':zero-velocity',not np.any(render(name+'-zero',exe,p|{'velocity':0},NOTE)))
            long = render(name+'-long-tail',exe,p|{'decay':ui['decay'][1]},NOTE,frames=576000)
            check(name+':tail-settles',float(abs(long[-24000:]).max())<1e-7,
                  tail_peak=float(abs(long[-24000:]).max()))
            ev=[]
            for i,(frequency,decay,noise) in enumerate(itertools.product(ui['freq'][:2],ui['decay'][:2],(0.,1.))):
                n=480+i*2500
                ev += [(n,'freq',frequency),(n,'decay',decay),(n,'noise',noise),
                       (n,'gate',1),(n+1,'gate',0)]
            z=render(name+'-union-corners',exe,p,ev,frames=96000)
            check(name+':union-corners-bounded',float(abs(z).max())<1.5)
            bank=[]
            for slot in item['legacy_sources']:
                pre=item['presets'][slot+'-Classic']
                z=render(name+'-'+slot+'-audition',exe,pre,[(4800,'gate',1),(4801,'gate',0),
                    (67200,'velocity',.65),(67200,'gate',1),(67201,'gate',0)],frames=144000)[:,0]
                bank.append(z)
            lab.wav(name+'_classic_presets.wav',np.concatenate(bank))
        # A real mutated compiled source proves the equivalence check detects revoicing.
        src=ROOT/PLAN['selections']['tom909']['source']
        mutant=src.read_text().replace('process=u.finish(raw,','process=.99*u.finish(raw,')
        assert mutant != src.read_text()
        mpath=lab.out/'tom909-revoiced.dsp'
        mpath.write_text(mutant.replace('library("drums909.lib")',f'library("{src.parent / "drums909.lib"}")'))
        bad=lab.build('tom909-revoiced',mpath)
        p=PLAN['selections']['tom909']['defaults']
        a=render('equivalence-good',exes['tom909'],p,NOTE)
        b=render('equivalence-revoiced',bad,p,NOTE)
        check('negative-control:detect-one-percent-revoicing',not np.array_equal(a,b))
        lab.report['negative_controls']={'tom_equivalence':{'mutation':'0.99 output gain','rejected':not np.array_equal(a,b)}}
        # Inspect each mode independently: the earlier suite only tested freq in Rim mode.
        item=PLAN['selections']['rim-claves'];p=item['defaults']
        exe=lab.build('rim-claves',ROOT/item['source']);exes['rim-claves']=exe
        vec=lab.build('rim-claves-vector',ROOT/item['source'],True)
        old=lab.build('rim-claves-v1',ROOT/'modules/drums-808-aux/v1/rim-claves.dsp')
        io,ui=controls(exe);captured['rim-claves']=ui
        check('rim-claves:io-and-contract',io==(0,1) and set(ui)==set(p))
        for title in ('Rim','DryRim','Claves'):
            pre=item['presets'][title]
            a=render('rim-v1-'+title,old,pre,NOTE)
            b=render('rim-v2-'+title,exe,pre,NOTE)
            equal(lab,'rim-claves:preserved-'+title,a,b)
        fixed=[];broken=[]
        for frequency in (1500.,2500.,3000.):
            pre=p|{'mode':1,'freq':frequency,'decay':.12,'drive':0}
            a=render('claves-v1-'+str(int(frequency)),old,pre,NOTE)[:,0];broken.append(a)
            b=render('claves-v2-'+str(int(frequency)),exe,pre,NOTE)[:,0];fixed.append(b)
            spectrum=abs(np.fft.rfft(b))
            peak=float(np.argmax(spectrum[600:4000])+600)
            check('claves:tuning-'+str(int(frequency)),abs(peak-frequency)<=3,measured_hz=peak,target_hz=frequency)
        old_fault=np.array_equal(broken[0],broken[1]) and np.array_equal(broken[1],broken[2])
        check('negative-control:old-claves-ignores-frequency',old_fault)
        check('claves:new-frequency-changes-audio',not np.array_equal(fixed[0],fixed[1]))
        lab.report['negative_controls']['claves_v1']={'fault_reproduced':old_fault,'fixed_frequencies':[1500,2500,3000]}
        for mode in (0.,1.):
            pre=p|{'mode':mode,'freq':455 if mode==0 else 1900}
            base=render('rim-mode-'+str(int(mode)),exe,pre,NOTE)
            y=render('rim-latch-'+str(int(mode)),exe,pre,NOTE+[(4000,'freq',3000),(4000,'mode',1-mode)])
            equal(lab,'rim-claves:onset-latching-'+str(int(mode)),base,y)
            y=render('rim-vector-'+str(int(mode)),vec,pre,NOTE)
            check('rim-claves:vector-'+str(int(mode)),np.max(abs(y-base))<3e-5)
            for rate in (44100,96000):
                n=round(.01*rate)
                y=render(f'rim-rate-{mode}-{rate}',exe,pre,[(n,'gate',1),(n+1,'gate',0)],sr=rate,frames=rate)
                check(f'rim-claves:rate-bounded-{mode}-{rate}',np.isfinite(y).all() and abs(y).max()<1)
        # Audition exact selected revisions, retaining the three unchanged 808 sources.
        first={}
        for name in ('tom-conga','rim-claves','maracas','cowbell'):
            item=PLAN['selections'][name]
            if name not in exes:exes[name]=lab.build(name,ROOT/item['source'])
            bank=[]
            for title,pre in item['presets'].items():
                z=render(name+'-'+title,exes[name],pre,[(4800,'gate',1),(4801,'gate',0),
                    (67200,'velocity',.65),(67200,'gate',1),(67201,'gate',0)],frames=144000)[:,0]
                lab.wav(name+'_'+title+'.wav',z);bank.append(z)
                first.setdefault(name,z)
            lab.wav(name+'_preset_bank.wav',np.concatenate(bank))
        seq=lab.build('trigger-seq',ROOT/'modules/trigger-seq/v1/trigger.dsp')
        sd=dict(run=1,length=16,clock=0,reset=0)|{f'step{i+1:02d}':0 for i in range(32)}
        pats={'tom-conga':[3,7,11,15],'rim-claves':[4,12],'maracas':[2,6,10,14],'cowbell':[0,7,8,15]}
        clocks=[(4800+i*6000+j,'clock',1-j) for i in range(64) for j in (0,1)]
        stems=[]
        for name,pat in pats.items():
            lane=render(name+'-triggers',seq,sd|{f'step{i+1:02d}':1 for i in pat},clocks,frames=576000)[:,0]
            onsets=np.flatnonzero(lane>.5).tolist()
            expected=[4800+i*6000 for i in range(64) if i%16 in pat]
            check(name+':exact-sequencer-events',onsets==expected)
            ev=[];item=PLAN['selections'][name]
            for n in onsets:ev += [(n,'gate',1),(n+1,'gate',0)]
            z=render(name+'-groove',exes[name],item['defaults'],ev,frames=576000)[:,0]
            lab.wav(name+'_stem.wav',z);stems.append(z)
        lab.wav('00_808_aux_groove.wav',sum(stems)*.5)
        lab.wav('01_808_aux_isolated.wav',np.concatenate(list(first.values())))
        lab.report['audition']={'groove_gain':.5,'other_gains':1,'seconds':12,'bpm':120,'bars':4,
            'processing':'No external effects, normalization or limiter; offline adapter not CURLOP.'}
        paths={MANIFEST,Path(__file__),ROOT/'tools/modules/render.cpp'}
        paths.update(ROOT/item['source'] for item in PLAN['selections'].values())
        paths.update(ROOT/p for item in PLAN['selections'].values() for p in item.get('legacy_sources',{}).values())
        paths.update(ROOT/f'modules/drums-{f}/v1/{lib}' for f,lib in [('606','drums606.lib'),('909','drums909.lib'),('808-aux','drums808aux.lib')])
        lab.report['sources']={str(path.relative_to(ROOT)):digest(path) for path in sorted(paths)}
        lab.report['passed']=all(x['passed'] for x in lab.report['checks'])
    except Exception as exc:
        lab.report.update(passed=False,error=repr(exc),compiler_output=getattr(exc,'output',None))
        raise
    finally:
        lab.report['wall_seconds']=time.perf_counter()-start
        lab.report['instrumented_compute_seconds']=sum(r['diagnostics']['instrumented_compute_ns'] for r in lab.report['renders'])/1e9
        (lab.out/'report.json').write_text(json.dumps(lab.report,indent=2))
        print(json.dumps({'passed':lab.report.get('passed'), 'checks':len(lab.report['checks']),
            'renders':len(lab.report['renders']), 'builds':len(lab.report['builds'])}),flush=True)
    if not lab.report['passed']:raise SystemExit(1)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--out',required=True)
    run(parser.parse_args().out)
