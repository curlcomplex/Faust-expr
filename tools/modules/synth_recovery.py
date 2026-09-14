"""Actual-Faust recovery qualification, separate from hardware/preset tuning."""
from pathlib import Path
import argparse
import hashlib
import itertools
import json
import os
import traceback
import numpy as np
from synth_batch import SynthLab, DEFAULTS, ROOT, phrase
from acid_batch import controls
from mini_v2_reproduction import metrics

J60 = dict(gate=0, freq=220, velocity=1, saw=.70, pulse=.30, sub=.45,
    noise=.015, pwm=.50, pwmDepth=.16, lfoRate=4.8, cutoff=1800,
    resonance=.22, hpf=25, filterEnv=.38, keyTrack=.35, attack=.012,
    decay=.30, sustain=.72, release=.50, level=.70)
SOURCES = {
    'mini-v3': (ROOT/'modules/minimoog/v3/voice.dsp', DEFAULTS['mini']),
    'juno60-pr89': (ROOT/'modules/juno-60/candidates/pr89-0b98748d/voice.dsp', J60),
}


def git_blob(path):
    data = path.read_bytes()
    return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()


def passed(checks):
    return bool(checks) and all(row.get('passed') is True for row in checks)


def tone_controls(name, base):
    p = base | dict(freq=220, gate=0, velocity=1, attack=.003, sustain=1,
                   release=.15, noise=0)
    if name.startswith('mini'):
        p.update(osc1=1, osc2=0, osc3=0, contour=0, drive=0, emphasis=.707)
    else:
        p.update(saw=1, pulse=0, sub=0, pwmDepth=0, filterEnv=0,
                 resonance=0, hpf=20, keyTrack=0)
    return p


def run(out):
    lab = SynthLab(out)
    c, r = lab.check, lab.render
    (out/'audition').mkdir(exist_ok=True)
    lab.report.update(version='synth-recovery-1', commit=os.getenv('GITHUB_SHA','local'),
        hardware_approved=False, human_approved=False, selected_for_promotion=False,
        hardware_references_fitted=False, stress_cases=[])
    try:
        identities = json.loads((ROOT/'modules/juno-60/CANDIDATES.json').read_text())
        candidate = identities['candidates'][2]
        directory = (ROOT/candidate['path']).parent
        for fn, blob in candidate['git_blobs'].items():
            c('pr89-exact-source:'+fn, git_blob(directory/fn) == blob)
        c('juno-identities-distinct', len({x['id'] for x in identities['candidates']}) == 3)
        for name, (src, base) in SOURCES.items():
            print('QUALIFY', name, flush=True)
            exe = lab.build(name, src)
            vec = lab.build(name+'-vector', src, True)
            io, ui = controls(exe)
            c(name+':io', io == (0,1))
            c(name+':controls', set(ui) == set(base))
            c(name+':defaults', all(abs(ui[k][2]-v) < 1e-5 for k,v in base.items()))
            c(name+':freq-hz', ui['freq'][:2] == [20.,8000.])
            p = tone_controls(name, base) | dict(cutoff=16000)
            events = [(4800,'gate',1), (96000,'gate',0)]
            x = r(name+'-held', exe, p, events, frames=144000)[:,0]
            m = metrics(x,48000)
            c(name+':held-tone', m['ac_rms'] > 1e-3 and m['harmonic_fraction'] > .8, **m)
            c(name+':pre-onset-silence', not np.any(x[:4800]))
            c(name+':release-settles', np.max(abs(x[-4800:])) < 1e-6)
            c(name+':never-triggered', not np.any(r(name+'-never',exe,p)))
            c(name+':zero-velocity', not np.any(r(name+'-zero',exe,p|{'velocity':0},events,frames=144000)))
            half = r(name+'-half',exe,p|{'velocity':.5},events,frames=144000)[:,0]
            c(name+':velocity-linear', np.max(abs(half-.5*x)) < 2e-6)
            for block in (1,127,512):
                y = r(name+f'-block-{block}',exe,p,events,block=block,frames=144000)[:,0]
                c(name+f':block-{block}',np.array_equal(x,y), max_error=float(np.max(abs(x-y))))
            y = r(name+'-vector',vec,p,events,frames=144000)[:,0]
            c(name+':vector-parity',np.max(abs(x-y)) < 1e-4,max_error=float(np.max(abs(x-y))))
            reskey = 'emphasis' if name.startswith('mini') else 'resonance'
            envkey = 'contour' if name.startswith('mini') else 'filterEnv'
            # The region missing from the previous candidate suite: cutoff,
            # feedback and contour together, including all three sample rates.
            for sr, cf, res, env in itertools.product((44100,48000,96000),
                    (800,12000,14000,16000), (ui[reskey][0],ui[reskey][1]), (0,1)):
                tag = f'{name}-stress-{sr}-{cf}-{res}-{env}'
                q = p | {'cutoff':cf,reskey:res,envkey:env}
                y = r(tag,exe,q,[(round(.1*sr),'gate',1),(round(2*sr),'gate',0)],
                      sr=sr,frames=round(2.5*sr))[:,0]
                measurement = metrics(y,sr)
                c(tag+':audible-bounded', measurement['ac_rms'] > 1e-4 and np.max(abs(y)) < 1,
                  **measurement)
                c(tag+':no-dc-collapse',abs(measurement['mean']) < .1*measurement['ac_rms']+1e-6)
                lab.report['stress_cases'].append(dict(instrument=name,sr=sr,cutoff=cf,
                    resonance=res,envelope=env,**measurement))
            # A persistent voice must recover after repeated high/low sweeps.
            ev=[(4800,'gate',1),(48000,'cutoff',800),(96000,'cutoff',16000),
                (144000,'cutoff',800),(192000,'cutoff',16000),(240000,'gate',0)]
            sweep = r(name+'-persistent-sweep',exe,p,ev,frames=288000)[:,0]
            for index in (0,1,2,3,4):
                w = sweep[index*48000+28800:(index+1)*48000-4800]
                c(name+f':sweep-audible-{index}',np.std(w) > 1e-4)
            lab.wav(name+'-persistent-sweep.wav',sweep)
            # Sound-shaping controls remain live after gate-off, but note pitch holds.
            tailp = p | dict(cutoff=2000,release=1.5)
            tail_events=[(4800,'gate',1),(24000,'gate',0)]
            a=r(name+'-tail',exe,tailp,tail_events,frames=96000)
            b=r(name+'-tail-cutoff',exe,tailp,tail_events+[(30000,'cutoff',80)],frames=96000)
            z=r(name+'-tail-pitch',exe,tailp,tail_events+[(30000,'freq',440)],frames=96000)
            delta=float(np.linalg.norm(a[36000:60000]-b[36000:60000])/(np.linalg.norm(a[36000:60000])+1e-20))
            c(name+':live-release-cutoff',delta > .05,relative_change=delta)
            c(name+':release-pitch-held',np.array_equal(a,z))
            # Same controls and musical score; no gain normalization or effects.
            audition=r(name+'-phrase',exe,base,phrase(root=110 if name.startswith('mini') else 220),frames=384000)
            lab.wav(name+'-phrase.wav',audition)
            print('STRESS',name,json.dumps({'cases':sum(v['instrument']==name for v in lab.report['stress_cases']),
                'min_ac_rms':min(v['ac_rms'] for v in lab.report['stress_cases'] if v['instrument']==name)}),flush=True)
        c('stress-coverage-complete',len(lab.report['stress_cases']) == 96)
    except Exception:
        lab.report['exception']=traceback.format_exc()
        c('completed-without-exception',False)
        print(lab.report['exception'],flush=True)
    lab.report['passed']=passed(lab.report['checks'])
    lab.report['failures']=[x['name'] for x in lab.report['checks'] if x.get('passed') is not True]
    (out/'results.json').write_text(json.dumps(lab.report,indent=2))
    print('RECOVERY_SUMMARY',json.dumps(dict(passed=lab.report['passed'],
        checks=len(lab.report['checks']),renders=len(lab.report['renders']),
        failures=lab.report['failures'])),flush=True)
    return 0 if lab.report['passed'] else 1


if __name__ == '__main__':
    ap=argparse.ArgumentParser()
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args()
    raise SystemExit(run(a.out))
