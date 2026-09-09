"""Compile and verify the isolated gate-release body using actual Faust.

No hardware files or network are required. The independent analytic equation
is a numerical oracle, never the source of delivered listening audio.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'modules/kick-pm/experiments/body-02.dsp'
DEFAULTS = dict(frequency_hz=65.4, pitch_amount_hz=350., pitch_tau_s=.025,
                body_tau_s=.18, release_tau_s=.04, attack_tau_s=.0001,
                phase_cycles=.13, level=.65, velocity=1.)
BLOCKS = (1, 32, 64, 127, 128, 256, 512)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def expected(p, rate, frames, onset, note_off):
    """Discrete phase sum, reset sample=zero. Constant-parameter oracle only."""
    n = np.arange(frames, dtype=np.float64) - onset
    t = np.maximum(n, 0) / rate
    q = 1. / (rate * p['pitch_tau_s'])
    cycles = (p['frequency_hz'] * t
              + p['pitch_amount_hz'] / rate
              * (-np.expm1(-t / p['pitch_tau_s'])) / np.expm1(q))
    u = np.maximum(np.arange(frames) - note_off, 0) / rate
    env = (-np.expm1(-t / p['attack_tau_s']) * np.exp(-t / p['body_tau_s'])
           * np.exp(-u / p['release_tau_s']))
    return np.where(n >= 0, p['level'] * p['velocity'] * env
                    * np.sin(2*np.pi*(cycles+p['phase_cycles'])), 0.)


class Probe:
    def __init__(self, out):
        self.out = Path(out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.report = dict(scope='actual Faust gate-body correctness; not hardware match or realtime qualification',
                           passed=False, builds={}, checks=[], renders=[])

    def run(self, cmd):
        cmd = list(map(str, cmd))
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=120)
        with (self.out/'build.log').open('a') as f:
            f.write(json.dumps(cmd)+'\n'+p.stdout+p.stderr+'\n')
        if p.returncode:
            raise RuntimeError('command failed: '+repr(cmd)+'\n'+p.stderr)
        return p.stdout

    def check(self, name, ok, **details):
        self.report['checks'].append(dict(name=name, passed=bool(ok), **details))
        if not ok:
            raise AssertionError(name+': '+str(details))

    def build(self, vector):
        name = 'vector' if vector else 'scalar'
        d = self.out/name
        d.mkdir(exist_ok=True)
        faust, cxx = os.environ.get('FAUST', 'faust'), os.environ.get('CXX', 'c++')
        self.report['faust_version'] = self.run([faust, '-v'])
        self.report['cxx_version'] = self.run([cxx, '--version'])
        self.run([faust, '-e', SOURCE, '-o', d/'expanded.dsp'])
        flags = ['-lang', 'cpp', '-single', '-cn', 'ModuleDSP']
        if vector:
            flags += ['-vec', '-lv', '0', '-vs', '32']
        self.run([faust, *flags, d/'expanded.dsp', '-o', d/'generated.hpp'])
        native = ['-std=c++17', '-O2', '-ffp-contract=off', '-I'+str(d)]
        self.run([cxx, *native, ROOT/'tools/modules/render.cpp', '-o', d/'render'])
        controls = self.run([d/'render', '--controls'])
        (d/'controls.tsv').write_text(controls)
        self.check(name+':io', controls.splitlines()[0] == 'io\t0\t1')
        self.check(name+':controls', {r.split('\t')[0] for r in controls.splitlines()[1:]}
                   == set(DEFAULTS)|{'gate'})
        self.report['builds'][name] = dict(source_sha256=sha(SOURCE),
            expanded_sha256=sha(d/'expanded.dsp'), generated_sha256=sha(d/'generated.hpp'),
            runner_sha256=sha(d/'render'), faust_flags=flags, native_flags=native)
        return d/'render'

    def render(self, name, exe, p, rate, block, frames, events):
        score, raw = self.out/(name+'.tsv'), self.out/(name+'.f32')
        rows = [(0, k, v) for k, v in p.items()] + list(events)
        rows.sort(key=lambda r: r[0])
        score.write_text(''.join(f'{n}\t{k}\t{v:.12g}\n' for n, k, v in rows))
        diag = json.loads(self.run([exe, score, raw, rate, block, frames, 0]))
        x = np.fromfile(raw, dtype='<f4')
        self.check(name+':finite-length', len(x) == frames and np.isfinite(x).all())
        self.report['renders'].append(dict(name=name, score=score.name,
            score_sha256=sha(score), raw=raw.name, raw_sha256=sha(raw), diagnostics=diag))
        return x

    def execute(self):
        scalar, vector = self.build(False), self.build(True)
        for rate in (44100, 48000, 96000):
            onset, note_off, frames = 101, 101+round(.337*rate), rate
            events = [(onset, 'gate', 1), (note_off, 'gate', 0)]
            p = DEFAULTS.copy()
            baseline = self.render(f'{rate}-base', scalar, p, rate, 128, frames, events)
            self.check(f'{rate}:pre-silence', np.max(abs(baseline[:onset])) == 0)
            self.check(f'{rate}:headroom', np.max(abs(baseline)) <= p['level'])
            self.check(f'{rate}:tail', np.max(abs(baseline[-100:])) < 1e-7)
            oracle = expected(p, rate, frames, onset, note_off)
            error = float(np.max(abs(baseline-oracle)))
            self.check(f'{rate}:discrete-oracle', error < 5e-4, max_error=error)
            # A one-sample-early release must not satisfy the boundary oracle.
            held = self.render(f'{rate}-held', scalar, p, rate, 128, frames, [(onset, 'gate', 1)])
            self.check(f'{rate}:release-boundary', baseline[note_off] == held[note_off])
            span = slice(note_off, note_off+round(.08*rate))
            gain = np.exp(-np.arange(round(.08*rate))/(rate*p['release_tau_s']))
            self.check(f'{rate}:release-law', np.max(abs(baseline[span]-held[span]*gain)) < 2e-6)
            for block in BLOCKS:
                x = self.render(f'{rate}-block-{block}', scalar, p, rate, block, frames, events)
                self.check(f'{rate}:block-{block}', np.max(abs(x-baseline)) <= 1e-6)
            x = self.render(f'{rate}-vector', vector, p, rate, 128, frames, events)
            self.check(f'{rate}:vector', np.max(abs(x-baseline)) <= 2e-4)
            x = self.render(f'{rate}-latched', scalar, p, rate, 127, frames,
                            events+[(onset+1000, 'velocity', .1)])
            self.check(f'{rate}:latched-velocity', np.array_equal(x, baseline))
            x = self.render(f'{rate}-half', scalar, p|{'velocity':.5}, rate, 127, frames, events)
            self.check(f'{rate}:velocity-linearity', np.max(abs(x-.5*baseline)) < 2e-6)
            # A second onset during the old release resets the same persistent body.
            second = round(.51*rate)
            ev = events+[(second, 'gate', 1), (second+round(.1*rate), 'gate', 0)]
            x = self.render(f'{rate}-retrigger', scalar, p, rate, 127, frames, ev)
            fresh = self.render(f'{rate}-fresh', scalar, p, rate, 128, frames,
                [(101, 'gate', 1), (101+round(.1*rate), 'gate', 0)])
            n = round(.2*rate)
            self.check(f'{rate}:persistent-retrigger', np.max(abs(x[second:second+n]-fresh[101:101+n])) < 1e-6)
        # Exercise event fragments and repeated gate edges in both compiled modes.
        for rate in (44100, 48000):
            events = []
            for i in range(24):
                n = 131+i*601
                events += [(n, 'frequency_hz', 32.+i*5), (n, 'pitch_amount_hz', (i%4)*150.),
                           (n, 'velocity', .25+.25*(i%4)), (n, 'gate', 1), (n+251, 'gate', 0)]
            ref = self.render(f'{rate}-dynamic-ref', scalar, DEFAULTS, rate, 128, rate, events)
            for block in (1, 127, 512):
                y = self.render(f'{rate}-dynamic-{block}', scalar, DEFAULTS, rate, block, rate, events)
                self.check(f'{rate}:dynamic-{block}', np.max(abs(y-ref)) < 1e-6)
            y = self.render(f'{rate}-dynamic-vector', vector, DEFAULTS, rate, 128, rate, events)
            self.check(f'{rate}:dynamic-vector', np.max(abs(y-ref)) < 2e-4)
        self.report['passed'] = True

    def save(self):
        try:
            self.report['source_commit'] = self.run(['git', 'rev-parse', 'HEAD']).strip()
        except Exception:
            self.report['source_commit'] = None
        self.report['source_files'] = {}
        for p in (SOURCE, Path(__file__), ROOT/'tools/modules/render.cpp'):
            rel = p.relative_to(ROOT)
            dst = self.out/'source'/rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, dst)
            self.report['source_files'][str(rel)] = sha(p)
        (self.out/'results.json').write_text(json.dumps(self.report, indent=2)+'\n')
        print(json.dumps(dict(passed=self.report['passed'], checks=len(self.report['checks']),
                             renders=len(self.report['renders']), failure=self.report.get('failure'))))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    probe = Probe(args.out)
    try:
        probe.execute()
    except Exception as e:
        probe.report['failure'] = str(e)
    finally:
        probe.save()
    if not probe.report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
