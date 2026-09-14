"""Reproduce the preserved Mini v2 failure; this is not a qualification pass.

Runs the original and suspect actual Faust sources at identical settings. A
successful reproducer means it FOUND the old defect, not that v2 is acceptable.
"""
from pathlib import Path
import argparse
import json
import math
import os
import numpy as np
from synth_batch import SynthLab, DEFAULTS, ROOT


def metrics(x, sr):
    y = np.asarray(x[round(sr):round(1.9*sr)], dtype=float).reshape(-1)
    centered = y-y.mean()
    power = np.abs(np.fft.rfft(centered*np.hanning(len(y))))**2
    hz = np.fft.rfftfreq(len(y), 1/sr)
    tonal = sum(power[(hz > n*220-3) & (hz < n*220+3)].sum()
                for n in range(1, 33))
    return dict(rms=float(np.sqrt(np.mean(y*y))),
                ac_rms=float(np.sqrt(np.mean(centered*centered))),
                mean=float(y.mean()), peak=float(np.max(np.abs(y))),
                harmonic_fraction=float(tonal/max(float(power.sum()), 1e-30)))


def run(out):
    lab = SynthLab(out)
    (out/'audition').mkdir(exist_ok=True)
    report = dict(commit=os.getenv('GITHUB_SHA', 'local'),
                  purpose='Expected-failure reproduction, NOT v2 qualification',
                  cases=[], hardware_approved=False)
    sources = {v: ROOT/f'modules/minimoog/{v}/voice.dsp' for v in ('v1', 'v2')}
    exes = {v: lab.build('mini-'+v, src) for v, src in sources.items()}
    base = DEFAULTS['mini'] | dict(freq=220, osc1=1, osc2=0, osc3=0,
        noise=0, drive=0, emphasis=.707, contour=0, attack=.003,
        sustain=1, release=.15)
    for sr in (44100, 48000, 96000):
        for cutoff, contour in ((800, 0), (16000, 0), (16000, 1)):
            row = dict(sr=sr, cutoff=cutoff, contour=contour, versions={})
            effective_fc = min(.4*sr, cutoff*2**(5*contour*.2))
            a = effective_fc*math.pi/sr
            row['sustain_coefficient'] = effective_fc*4*math.pi*.312*(1-a)/(1+a)
            for version, exe in exes.items():
                tag = f'{version}-{sr}-{cutoff}-{contour}'
                x = lab.render(tag, exe, base | dict(cutoff=cutoff, contour=contour),
                    [(round(.1*sr), 'gate', 1), (round(2*sr), 'gate', 0)],
                    sr=sr, frames=round(3*sr))[:, 0]
                row['versions'][version] = metrics(x, sr)
                if sr == 48000:
                    lab.wav(tag+'.wav', x, sr)
            old = row['versions']['v1']['ac_rms']
            new = row['versions']['v2']['ac_rms']
            row['v2_collapsed_vs_v1'] = old > 1e-4 and new < .01*old
            report['cases'].append(row)
            print(json.dumps(row), flush=True)
    report['defect_reproduced'] = any(r['v2_collapsed_vs_v1'] for r in report['cases'])
    report['lab'] = lab.report
    (out/'results.json').write_text(json.dumps(report, indent=2))
    print('DEFECT_REPRODUCED', report['defect_reproduced'], flush=True)
    return 0 if report['defect_reproduced'] else 1


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True, type=Path)
    args = ap.parse_args()
    raise SystemExit(run(args.out))
