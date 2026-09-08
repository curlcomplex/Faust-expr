"""Compare the actual-Faust gated body with the frozen BDM-01 preview.

No search/refitting or Python-generated listening audio. The prior study's
estimates are frozen. This is one inspected, lossy, unknown-settings recording,
not held-out device validation or a recovered hardware control map.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import numpy as np
from scipy.io import wavfile
from scipy.signal import stft

ROOT = Path(__file__).resolve().parents[2]
PRESET = ROOT/'modules/kick-pm/experiments/body-gate-study-03.json'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def phase_depth(continuous_depth, tau, rate):
    """Convert continuous integral depth to the reset phasor's right-end sum."""
    if not (np.isfinite([continuous_depth, tau, rate]).all()
            and continuous_depth >= 0 and tau > 0 and rate > 0):
        raise ValueError('invalid phase conversion')
    return float(continuous_depth * rate * tau * np.expm1(1/(rate*tau)))


def compare(x, y, rate):
    x, y = np.asarray(x, dtype=np.float64), np.asarray(y, dtype=np.float64)
    if x.ndim != 1 or x.shape != y.shape or not np.isfinite(x).all() or not np.isfinite(y).all():
        raise ValueError('comparison shape or values')
    xx, yy = float(x@x), float(y@y)
    if xx < 1e-20 or yy < 1e-20:
        raise ValueError('silent comparison')
    gain = math.sqrt(xx/yy)
    per_resolution = []
    for n in (256, 1024, 4096):
        def spectrum(z):
            return abs(stft(z, fs=rate, nperseg=n, noverlap=n*3//4,
                            boundary='zeros', padded=True)[2])
        reference, candidate = spectrum(x), spectrum(y*gain)
        floor = max(float(reference.max())*1e-3, 1e-12)
        diff = 20*np.log10(np.maximum(candidate, floor)/np.maximum(reference, floor))
        per_resolution.append(float(np.sqrt(np.mean(diff*diff))))
    # No phase/time alignment. One positive whole-hit gain for all metrics.
    residual = x-gain*y
    bands = {}
    for name, a, b in [('attack', 0, .006), ('body', .006, .30), ('late_tail', .337, .45)]:
        sl = slice(round(a*rate), round(b*rate))
        bands[name] = float(20*np.log10(max(np.linalg.norm(residual[sl]), 1e-20)
                                         / max(np.linalg.norm(x[sl]), 1e-20)))
    return dict(rms_match_gain=gain, spectral_rmse_db=float(np.mean(per_resolution)),
                spectral_by_resolution_db=per_resolution,
                residual_relative_db=float(20*np.log10(np.linalg.norm(residual)/math.sqrt(xx))),
                residual_by_time_region_db=bands, candidate_peak=float(abs(y).max()),
                raw_rms_ratio=math.sqrt(yy/xx))


def crossing_error(x, y, rate):
    """Diagnostic only for this near-sinusoidal hit, not a general f0 tracker."""
    def points(z):
        k = np.flatnonzero((z[:-1] <= 0) & (z[1:] > 0))
        t = (k-z[k]/(z[k+1]-z[k]))/rate
        mids, freq = (t[1:]+t[:-1])/2, 1/np.diff(t)
        keep = (mids > .005) & (mids < .30) & (freq > 40) & (freq < 1200)
        return mids[keep], freq[keep]
    tx, fx = points(x)
    ty, fy = points(y)
    good = (tx >= ty.min()) & (tx <= ty.max())
    e = 1200*np.log2(np.interp(tx[good], ty, fy)/fx[good])
    return dict(point_count=int(good.sum()), rmse_cents=float(np.sqrt(np.mean(e*e))),
                error_cents=e.tolist(), reference_times=tx[good].tolist())


def render(exe, out, label, parameters, rate, frames, note_off, block=128, extra=None):
    score, raw = out/(label+'.tsv'), out/(label+'.f32')
    events = [(0, k, v) for k, v in parameters.items()]+[(0, 'gate', 1)]
    if note_off is not None:
        events += [(note_off, 'gate', 0)]
    events += list(extra or [])
    events.sort(key=lambda e: e[0])
    score.write_text(''.join(f'{t}\t{k}\t{v:.12g}\n' for t, k, v in events))
    p = subprocess.run([str(exe), str(score), str(raw), str(rate), str(block), str(frames), '0'],
                       capture_output=True, text=True, timeout=20)
    if p.returncode:
        raise RuntimeError(p.stderr)
    y = np.fromfile(raw, dtype='<f4').astype(np.float64)
    if len(y) != frames or not np.isfinite(y).all():
        raise ValueError('invalid native output')
    return y, dict(score_sha256=sha(score), raw_sha256=sha(raw), parameters=parameters,
                   gate_off_frame=note_off, diagnostics=json.loads(p.stdout), runner_sha256=sha(exe))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--references', type=Path, required=True)
    ap.add_argument('--body-runner', type=Path, required=True)
    ap.add_argument('--baseline-runner', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    refs, out = args.references.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    protocol = json.loads(PRESET.read_text())
    manifest = json.loads((refs/'manifest.json').read_text())
    record = next(r for r in manifest['records'] if r['id'] == 'BDM-01')
    ref = refs/record['decoded_file']
    if ref.parent.resolve() != refs or sha(ref) != protocol['reference_wav_sha256']:
        raise ValueError('reference identity/path mismatch')
    rate, x = wavfile.read(ref)
    if rate != 44100 or x.ndim != 1 or not np.isfinite(x).all():
        raise ValueError('reference format')
    x = x.astype(np.float64)
    params = protocol['prior_continuous_estimates'].copy()
    off_seconds = params.pop('gate_off_s')
    off = round(off_seconds*rate)
    corrected = params|{'pitch_amount_hz':phase_depth(params['pitch_amount_hz'], params['pitch_tau_s'], rate)}
    report = dict(protocol=protocol, protocol_sha256=sha(PRESET), script_sha256=sha(__file__),
                  reference_manifest_sha256=sha(refs/'manifest.json'), renders={},
                  scope='Frozen-estimate same-recording comparison; no search, blind validation, hardware knob mapping or sonic approval')
    sounds = {}
    variants = [
        ('old_fitted', args.baseline_runner.resolve(), protocol['old_fitted_parameters'], 1),
        ('body_direct_estimates', args.body_runner.resolve(), params, off),
        ('body_discrete_phase', args.body_runner.resolve(), corrected, off),
        ('body_without_release', args.body_runner.resolve(), corrected, None)]
    for name, exe, p, release in variants:
        y, evidence = render(exe, out, name, p, rate, len(x), release)
        again, replay = render(exe, out, name+'-127', p, rate, len(x), release, block=127)
        error = float(np.max(abs(again-y)))
        if error > 1e-6:
            raise AssertionError('score segmentation changed output')
        evidence.update(metrics=compare(x, y, rate), crossing_diagnostic=crossing_error(x, y, rate),
                        replay_127=replay, replay_max_error=error)
        report['renders'][name] = evidence
        sounds[name] = y
    # Repeated three-way comparison, all transformations explicit.
    audio, timeline, pos = [], [], 0
    gap = np.zeros(round(.3*rate))
    for repetition in range(3):
        for name in ('reference', 'old_fitted', 'body_discrete_phase'):
            y = x if name == 'reference' else sounds[name]*report['renders'][name]['metrics']['rms_match_gain']
            timeline.append(dict(seconds=pos/rate, item=name, repetition=repetition+1))
            audio += [y, gap]
            pos += len(y)+len(gap)
    audio = np.concatenate(audio)
    gain = min(.7, .9/max(float(abs(audio).max()), 1e-20))
    audio *= gain
    if abs(audio).max() >= 1:
        raise ValueError('listening headroom')
    listen = out/'reference-old-new.wav'
    wavfile.write(listen, rate, np.rint(audio*32767).astype('<i2'))
    report['listening'] = dict(file=listen.name, global_gain=gain, timeline=timeline,
        transforms='Untrimmed reference; one whole-hit RMS match for each synth, then one common attenuation, silence gaps and PCM16. No EQ, time/phase alignment, limiter or reverb.', sha256=sha(listen))
    (out/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    (out/'ATTRIBUTION.md').write_text(
        '# Reference attribution\n\nBDM-01 from Syntakt Designer Drums by Winston Edwards / Particles Into Waves.\n'
        'https://particlesintowaves.bandcamp.com/album/syntakt-designer-drums\n'
        'CC BY 4.0: https://creativecommons.org/licenses/by/4.0/\n\n'
        'The public MP3 preview was decoded to WAV upstream; it is not the lossless original. '
        'This comparison interleaves our synthesized audio, adjusts whole-hit levels, inserts silence, '
        'and converts to PCM16 as recorded in results.json. No endorsement is implied.\n')
    print(json.dumps({name: r['metrics'] for name, r in report['renders'].items()}, indent=2))


if __name__ == '__main__':
    main()
