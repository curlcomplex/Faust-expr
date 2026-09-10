"""Perc end-to-end qualification, controlled ablation, and offline replay.

The instrument/patches are preserved. Measurements do not establish a hardware
clone. --replay recompiles unchanged generated C++; it does not invoke Faust.
"""
from __future__ import annotations
import argparse
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import wave

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
import perc_batch as base

ROOT = Path(__file__).resolve().parents[2]
MOD = ROOT / 'modules/perc-pm'
CXX_FLAGS = ['-std=c++17', '-O2', '-ffp-contract=off', '-fstack-usage']


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(values: dict, manifest: dict) -> None:
    for key, value in values.items():
        if key not in manifest['controls'] or isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            raise ValueError('invalid control: ' + str(key))
        c = manifest['controls'][key]
        if not c['min'] <= value <= c['max'] or (key == 'gate' and value not in (0, 1)):
            raise ValueError('out-of-range control: ' + key)


def events_to_rows(defaults, params, events, frames, manifest):
    validate(params, manifest)
    rows = {(0, k): v for k, v in (defaults | params).items()}
    seen = set()
    for n, k, v in events:
        if isinstance(n, bool) or not isinstance(n, int) or n < 0 or n >= frames:
            raise ValueError('invalid frame')
        validate({k: v}, manifest)
        if (n, k) in seen:
            raise ValueError('duplicate control event')
        seen.add((n, k))
        rows[n, k] = v
    return sorted((n, k, v) for (n, k), v in rows.items())


def decode_audio(path: Path):
    rate, x = wavfile.read(path)
    dtype = x.dtype
    if np.issubdtype(dtype, np.integer):
        if np.issubdtype(dtype, np.unsignedinteger):
            half = (np.iinfo(dtype).max + 1) / 2
            x = (x.astype(float) - half) / half
        else:
            x = x.astype(float) / max(abs(np.iinfo(dtype).min), np.iinfo(dtype).max)
    else:
        x = x.astype(float)
    if x.ndim == 2:
        x = x.mean(axis=1)
    if x.ndim != 1 or not len(x) or not np.isfinite(x).all():
        raise ValueError('invalid reference audio')
    return int(rate), x


def descriptor(x, rate=48000):
    """Full 10-second horizon; actual energy weights spectral windows.

    A silent or truncated candidate cannot gain a free match by throwing away
    the later part of a reference. Level is measured separately, not fitted.
    """
    x = np.asarray(x, float).reshape(-1)
    if not len(x) or not np.isfinite(x).all():
        raise ValueError('invalid descriptor audio')
    peak = float(np.max(abs(x)))
    if peak < 1e-12:
        raise ValueError('silent descriptor')
    onset = int(np.flatnonzero(abs(x) > peak * .005)[0])
    x = x[onset:onset + round(10 * rate)]
    energy = x*x
    total = max(float(energy.sum()), 1e-30)
    cumulative = np.cumsum(energy) / total
    result = [math.log1p(np.searchsorted(cumulative, q) / rate) for q in (.5, .9)]
    for lo, hi in ((0, .04), (.04, .3), (.3, 1), (1, 3), (3, 10)):
        y = x[round(lo * rate):round(hi * rate)]
        fraction = float(np.dot(y, y) / total)
        if len(y) < 16 or fraction < 1e-8:
            result.extend([0.] * 6)
            continue
        n = max(4096, 1 << (len(y)-1).bit_length())
        s = abs(np.fft.rfft(y*np.hanning(len(y)), n=n))**2
        f = np.fft.rfftfreq(n, 1/rate)
        s /= max(float(s.sum()), 1e-30)
        weight = math.sqrt(fraction)
        result.extend([fraction, weight * math.log10(max(1, float((s*f).sum())))])
        result.extend(weight * float(s[(f >= a) & (f < b)].sum()) for a, b in ((20, 400), (400, 2000), (2000, 8000), (8000, 20000)))
    return np.asarray(result)


def envelope_oracle(rate, frames, onset, decay, punch):
    # Independent float64 formula, not a generated graph calling itself.
    time = np.maximum(0, (np.arange(frames, dtype=float)-onset)/rate)
    body_tau = .018 * 85.**decay
    attack_tau = .00016 + .0009*(1-punch)**2
    env = -np.expm1(-time/attack_tau) * np.exp(-time/body_tau)
    env[:onset] = 0
    return env


class Complete(base.Study):
    def __init__(self, out, references, replay=None):
        super().__init__(out, None)
        self.references = references
        self.replay = replay
        self.actual_controls = {}
        self.r.update(schema=2, builds={}, source_files={}, platform=platform.platform(),
                      reference_limit='Exploratory descriptors of lossy unknown-settings PC Carbon previews. Prior comparisons already inspected all eight; no untouched holdout claim.')
        if replay:
            self.old = json.loads((replay/'report.json').read_text())
            for path, digest in self.old['source_files'].items():
                if sha(ROOT/path) != digest:
                    raise ValueError('replay source hash: ' + path)

    def build(self, label, vec=False, source='perc.dsp', diagnostic=False):
        d = self.out/label
        d.mkdir(exist_ok=True)
        flags = ['-lang', 'cpp', '-single', '-cn', 'ModuleDSP'] + (['-vec', '-lv', '0', '-vs', '32'] if vec else [])
        if self.replay:
            h = self.replay/label/'generated.hpp'
            if sha(h) != self.old['builds'][label]['generated_sha256']:
                raise ValueError('generated header checksum: ' + label)
            shutil.copyfile(h, d/'generated.hpp')
        else:
            base.cmd([os.getenv('FAUST', 'faust'), '-I', MOD, *flags, MOD/source, '-o', d/'generated.hpp'])
            base.cmd([os.getenv('FAUST', 'faust'), '-I', MOD, '-e', MOD/source, '-o', d/'expanded.dsp'])
        base.cmd([os.getenv('CXX', 'c++'), *CXX_FLAGS, '-I'+str(d), ROOT/'tools/modules/render.cpp', '-o', d/'render'])
        rows = base.cmd([d/'render', '--controls']).splitlines()
        (d/'controls.tsv').write_text('\n'.join(rows)+'\n')
        self.actual_controls[label] = {r.split('\t')[0] for r in rows[1:]}
        self.check(label+':mono-contract', rows[0] == 'io\t0\t1')
        expected = set(self.defaults)
        self.check(label+':control-ids', self.actual_controls[label] <= expected if diagnostic else self.actual_controls[label] == expected)
        values = {r.split('\t')[0]: list(map(float, r.split('\t')[1:4])) for r in rows[1:]}
        self.check(label+':control-ranges-defaults', all(np.allclose(v, [self.man['controls'][k][a] for a in ('min', 'max', 'default')], rtol=0, atol=1e-6) for k, v in values.items()))
        if not diagnostic:
            base.cmd([os.getenv('CXX', 'c++'), *CXX_FLAGS, '-I'+str(d), ROOT/'tools/modules/perc_benchmark.cpp', '-o', d/'benchmark'])
        self.r['builds'][label] = dict(source=source, diagnostic=diagnostic, flags=flags, cxx_flags=CXX_FLAGS,
            source_sha256=sha(MOD/source), generated_sha256=sha(d/'generated.hpp'), binary_sha256=sha(d/'render'))
        return d/'render'

    def render(self, label, exe, p=None, events=None, rate=48000, block=128, seconds=1.2):
        frames = round(seconds*rate)
        rows = events_to_rows(self.defaults, p or {}, events or [], frames, self.man)
        rows = [row for row in rows if row[1] in self.actual_controls[exe.parent.name]]
        score = self.out/(label+'.tsv')
        raw = self.out/(label+'.f32')
        score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for n, k, v in rows))
        diag = json.loads(base.cmd([exe, score, raw, rate, block, frames, 0]))
        x = np.fromfile(raw, '<f4')
        is_envelope = exe.parent.name == 'envelope'
        self.check(label+':finite-headroom', len(x) == frames and np.isfinite(x).all() and np.max(abs(x)) < (1.001 if is_envelope else .9), peak=float(np.max(abs(x))))
        self.r['renders'].append(dict(label=label, build=exe.parent.name, rate=rate, block=block, frames=frames,
            diagnostic=is_envelope, score_sha256=sha(score), raw_sha256=sha(raw), metrics=base.describe(x, rate), diagnostics=diag))
        return x

    def execute(self):
        # Preserve all original sound/score assertions, but skip obsolete 0.9-s
        # descriptor search: full-duration controlled ablation replaces it.
        super().execute()
        self.r['passed'] = False
        scalar = self.out/'scalar/render'
        vector = self.out/'vector/render'
        env = self.build('envelope', source='envelope-diagnostic.dsp', diagnostic=True)
        additive = self.build('additive', source='additive-diagnostic.dsp', diagnostic=True)
        self.r['compilers'] = dict(cxx=base.cmd([os.getenv('CXX', 'c++'), '--version']).strip(),
            faust='replay of unchanged generated C++; Faust not invoked' if self.replay else base.cmd([os.getenv('FAUST', 'faust'), '-v']).strip())
        # Onset writes versus one-sample-prepared controls with old tail present.
        for rate in (44100, 48000, 96000):
            n = round(.28*rate)
            target = self.patches['Industrial']
            early = self.hit(101) + [(n-1, k, v) for k, v in target.items()] + self.hit(n)
            on = self.hit(101) + [(n, k, v) for k, v in target.items()] + self.hit(n)
            a = self.render(f'lock-pre-{rate}', scalar, events=early, rate=rate)
            b = self.render(f'lock-on-{rate}', scalar, events=on, rate=rate)
            self.check('same-sample-lock:'+str(rate), np.array_equal(a, b))
            for de, pu in itertools.product((0., .5, 1.), (0., 1.)):
                frames = round(5.2*rate)
                at = rate//20
                x = self.render(f'envelope-{rate}-{de}-{pu}', env, {'decay': de, 'punch': pu}, self.hit(at), rate=rate, seconds=5.2)
                oracle = envelope_oracle(rate, frames, at, de, pu)
                valid = oracle > 1e-3
                error = float(np.max(abs(x[valid]-oracle[valid])/oracle[valid]))
                self.check(f'envelope-closed-form:{rate}:{de}:{pu}', error < 1e-4, max_relative_error=error)
        zero = self.render('velocity-zero', scalar, {'velocity': 0}, self.hit(101))
        self.check('velocity-zero-exact', not np.any(zero))
        a = self.render('onset-baseline', scalar, events=self.hit(101, 1))
        changes = [(5001, k, c['max']) for k, c in self.man['controls'].items() if k != 'gate']
        b = self.render('all-controls-latched', scalar, events=self.hit(101, 1)+changes)
        self.check('all-controls-latched-including-velocity', np.array_equal(a, b))
        # Original source visits 128 alternating-pitch points, NOT 512 corners.
        # This adds 256 real endpoints: every 7-D corner at both pitch endpoints.
        keys = [k for k in self.man['musical_control_order'] if k != 'pitch_hz']
        ev = []
        count = 0
        for hz, bits in itertools.product((35., 1800.), itertools.product((0., 1.), repeat=7)):
            n = 101 + count*1024
            count += 1
            ev += [(n, k, v) for k, v in zip(keys, bits)] + [(n, 'pitch_hz', hz)] + self.hit(n, 96)
        x = self.render('full-pitch-endpoints', scalar, events=ev, block=127, seconds=(101+count*1024+24000)/48000)
        self.check('full-endpoints-dc', abs(float(x.mean())) < .02, settings=count)
        # Defined equal event times, no noisy exciter in this rate diagnostic.
        rate_checks = []
        for name, p in [('clean', dict(pitch_hz=220, modulation=0, inharmonicity=0, drive=0)),
                        ('bell', self.patches['Bell']), ('upper-driven', dict(pitch_hz=1700, modulation=.95, inharmonicity=.9, drive=1))]:
            p = p | {'punch': 0, 'sweep': 0, 'decay': .9}
            lo = self.render('rate48-'+name, scalar, p, self.hit(2400), seconds=1.5)
            hi = self.render('rate96-'+name, scalar, p, self.hit(4800, 128), rate=96000, seconds=1.5)
            down = resample_poly(hi.astype(float), 1, 2, window=('kaiser', 10.))
            sl = slice(9600, 57600)
            error = lo[sl]-down[sl]
            rate_checks.append(dict(name=name, relative_rms_db=float(20*np.log10(max(1e-15, np.linalg.norm(error))/max(1e-15, np.linalg.norm(down[sl])))), max_abs=float(max(abs(error)))))
        self.r['rate_consistency_not_isolated_alias_energy'] = rate_checks
        self.coverage(scalar, additive)
        # Paired full-synth benchmarks alternate order; never force a speed win.
        perf = []
        for block in (32, 64, 128, 512):
            pairs = []
            for rep in range(4):
                order = ('scalar', 'vector') if rep % 2 == 0 else ('vector', 'scalar')
                pair = {label: json.loads(base.cmd([self.out/label/'benchmark', block])) for label in order}
                self.check(f'ordinary-new-guard:{block}:{rep}', all(p['ordinary_new_allocations_in_compute'] == 0 for p in pair.values()))
                pairs.append(pair)
            perf.append(dict(block=block, pairs=pairs, median_paired_scalar_over_vector=statistics.median(p['scalar']['p50_us']/p['vector']['p50_us'] for p in pairs)))
        self.r['performance'] = perf
        self.r['performance_scope'] = 'Four-voice hosted/container offline DSP-only microbenchmark, warm, alternating build order. Ordinary new/new[] guard is not a malloc/aligned/host allocation proof; target device and thermal/deadline acceptance remain open.'
        # Verify complete listening containers, not merely file existence.
        for path in sorted(self.out.glob('*.wav')):
            with wave.open(str(path), 'rb') as f:
                n, c, width = f.getnframes(), f.getnchannels(), f.getsampwidth()
                raw = f.readframes(n)
                self.check('wav-container:'+path.name, f.getframerate() == 48000 and n > 0 and len(raw) == n*c*width, frames=n, sha256=sha(path))
        self.r['passed'] = True

    def coverage(self, scalar, additive):
        manifest = json.loads((self.references/'manifest.json').read_text())
        self.r['reference_manifest_sha256'] = sha(self.references/'manifest.json')
        refs = {}
        for record in manifest['records']:
            name = record['id']
            for ext, field in [('mp3', 'mp3_sha256'), ('wav', 'wav_sha256')]:
                if sha(self.references/(name+'.'+ext)) != record[field]:
                    raise ValueError('reference checksum: ' + name)
            rate, x = decode_audio(self.references/(name+'.wav'))
            if rate != 48000 or len(x)/rate > 10:
                raise ValueError('reference horizon/rate')
            refs[name] = x
        # Predeclared grid, not fitted to individual references. Same settings for
        # both candidates. Ten-second renders include the full observed tails.
        grid = [dict(pitch_hz=p, decay=d, inharmonicity=i, modulation=m, mod_envelope=.55, sweep=.25, punch=.35, drive=.08)
                for p, d, i, m in itertools.product((90, 260, 720), (.25, .55, .85), (.1, .5, .9), (.2, .8))]
        labels = {}; all_desc = {}
        for name, exe in [('pm', scalar), ('additive', additive)]:
            pool = []
            for i, p in enumerate(grid):
                label = f'full-coverage-{name}-{i}'
                x = self.render(label, exe, p, self.hit(101), seconds=10.05)
                pool.append(descriptor(x))
                labels[name, i] = label
            all_desc[name] = np.vstack(pool)
        R = np.vstack([descriptor(x) for x in refs.values()])
        # Frozen metric in this experiment; all clips were already inspected by
        # the original short-window search, so none is labelled untouched here.
        combined = np.vstack([R, *all_desc.values()])
        mu, scale = combined.mean(0), np.maximum(combined.std(0), .02)
        coverage = {}
        for name, P in all_desc.items():
            distances = np.sqrt(np.mean(((R[:, None, :]-P[None, :, :])/scale)**2, axis=2))
            nearest = []
            for j, ref_name in enumerate(refs):
                k = int(distances[j].argmin())
                nearest.append(dict(reference=ref_name, candidate_index=k, render=labels[name, k], distance=float(distances[j, k]), parameters=grid[k]))
            coverage[name] = dict(mean_nearest=float(np.mean(distances.min(1))), median_nearest=float(np.median(distances.min(1))), nearest=nearest)
        self.r['full_duration_coverage'] = dict(method='32 energy-weighted time/spectral descriptors over 10 s, fixed 54-setting pool for each architecture; not a hardware-fit score', center=mu.tolist(), scale=scale.tolist(), candidates=coverage)
        # Audible controlled A/B for authored patches; fixed gain in both.
        ev = []
        for i, p in enumerate(self.patches.values()):
            n = round((.05+i*2.2)*48000)
            ev += [(n, k, v) for k, v in p.items()] + [(n, 'velocity', 1)] + self.hit(n)
            ev += [(n+36000, 'velocity', .55)] + self.hit(n+36000)
        alt = self.render('anchors-additive', additive, events=ev, seconds=18)
        base.wav(self.out/'perc-anchors-additive.wav', alt)
        # Reference/candidate listening derivative only: crops are explicit, no
        # fitted EQ or timing alignment, one gain per excerpt and shared attenuation.
        pieces, timeline = [], []
        cursor = 0
        for entry in coverage['pm']['nearest']:
            r = refs[entry['reference']][:72000]
            x = np.fromfile(self.out/(entry['render']+'.f32'), '<f4')[:len(r)]
            gain = float(np.sqrt(np.mean(r*r))/max(1e-12, np.sqrt(np.mean(x.astype(float)**2))))
            pause = np.zeros(7200)
            timeline.append(dict(reference=entry['reference'], source_render=entry['render'], start_seconds=cursor/48000, excerpt_frames=len(r), candidate_gain=gain))
            chunk = np.concatenate([r, pause, x*gain, pause])
            pieces.append(chunk);cursor += len(chunk)
        audio = np.concatenate(pieces)
        attenuation = min(1., .85/max(1e-12, float(max(abs(audio)))))
        base.wav(self.out/'perc-reference-nearest.wav', audio*attenuation)
        self.r['reference_audition'] = dict(timeline=timeline, shared_attenuation=attenuation, note='First up-to-1.5-second excerpts; one whole-excerpt candidate RMS gain. Descriptor analysis above uses complete 10-second horizon. Not individually fitted replicas.')

    def save(self, error=None):
        self.r['failure'] = error
        self.r['passed'] = bool(self.r.get('passed')) and error is None
        self.r['source_files'] = {}
        paths = list(MOD.glob('*')) + [ROOT/'tools/modules'/n for n in ('render.cpp', 'perc_batch.py', 'perc_delivery.py', 'perc_benchmark.cpp', 'fetch_perc_references.py', 'fetch_snare_references.py')]
        paths += list((ROOT/'tests').glob('test_perc_delivery.py'))
        for p in paths:
            if p.is_file():
                rel = p.relative_to(ROOT);self.r['source_files'][str(rel)] = sha(p)
                dest = self.out/'source'/rel;dest.parent.mkdir(parents=True, exist_ok=True);shutil.copyfile(p, dest)
        try:
            self.r['source_commit'] = base.cmd(['git', 'rev-parse', 'HEAD']).strip()
        except RuntimeError:
            self.r['source_commit'] = None
        (self.out/'report.json').write_text(json.dumps(self.r, indent=2)+'\n')
        print(json.dumps(dict(passed=self.r['passed'], renders=len(self.r['renders']), checks=len(self.r['checks']), failure=error)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--references', type=Path, required=True)
    parser.add_argument('--replay', type=Path)
    args = parser.parse_args()
    study = Complete(args.out.resolve(), args.references.resolve(), args.replay.resolve() if args.replay else None)
    error = None
    try:
        study.execute()
    except Exception as exc:
        error = str(exc)
    finally:
        study.save(error)
    if error:
        raise SystemExit(error)

if __name__ == '__main__':
    main()
