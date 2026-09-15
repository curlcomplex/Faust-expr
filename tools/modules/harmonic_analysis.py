#!/usr/bin/env python3
"""#108: guarded coherent-spectrum diagnostics, not an arbitrary-audio alias detector."""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
from pathlib import Path
import tempfile

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location('_harmonic_common', ROOT / 'tools/modules/faust_analysis.py')
_common = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_common)
sha = _common.sha
SIGNAL_CLASSES = ('stationary-oscillator', 'sine-driven-nonlinearity')
MAX_FFT = 262144
DB_FLOOR = -150.0
GUARDS = {'minimum_cycles': 32, 'coherence_bin_tolerance': 1e-7,
          'fundamental_min_ac_power_fraction': 1e-6,
          'fundamental_neighbor_power_fraction': 0.999,
          'quarter_rms_relative_spread_max': 0.02,
          'quarter_spectral_relative_change_max': 0.05,
          'quarter_fundamental_complex_deviation_max': 0.02}


def db(power_ratio: float) -> float:
    return float(10 * np.log10(max(power_ratio, 10 ** (DB_FLOOR / 10))))


def power_spectrum(samples: np.ndarray) -> np.ndarray:
    """One-sided rectangular DFT bin powers, summing to original mean-square."""
    n = len(samples)
    power = np.abs(np.fft.rfft(samples) / n) ** 2
    power[1:-1] *= 2
    return power


def analyze_window(samples, rate: int, fundamental_hz: float, signal_class: str,
                   *, max_generated_order: int | None = None) -> dict:
    """Require a known coherent fundamental. Invalid measurements raise, never pass.

    Optional finite-harmonic models predict fold locations, not causality. Observed
    harmonic-grid energy is always reported. If a predicted fold lands on an
    intended harmonic bin, attributable harmonic/THD values are withheld because
    the observed energy cannot be separated into harmonic and folded components.
    """
    if signal_class not in SIGNAL_CLASSES:
        raise ValueError('unsupported signal class: use stationary components, not percussion/noise')
    if isinstance(rate, bool) or not isinstance(rate, int) or not 8000 <= rate <= 192000:
        raise ValueError('invalid sample rate')
    original = np.asarray(samples)
    if original.ndim != 1 or np.iscomplexobj(original):
        raise ValueError('one real mono window required; do not silently flatten channels')
    x = np.asarray(original, dtype=np.float64)
    n = len(x)
    if n < 1024 or n > MAX_FFT or n & (n - 1) or not np.isfinite(x).all():
        raise ValueError('window must be finite, power-of-two, 1024..262144 frames')
    if isinstance(fundamental_hz, bool) or not math.isfinite(fundamental_hz) or fundamental_hz <= 0:
        raise ValueError('invalid known fundamental')
    k_float = fundamental_hz * n / rate
    k = round(k_float)
    if abs(k_float - k) > GUARDS['coherence_bin_tolerance']:
        raise ValueError('noncoherent window: fundamental must contain an integer number of cycles')
    if not GUARDS['minimum_cycles'] <= k <= n // 2 - 32:
        raise ValueError('fundamental too near DC/Nyquist or too few cycles')
    if max_generated_order is not None and (isinstance(max_generated_order, bool) or
            not isinstance(max_generated_order, int) or not 1 <= max_generated_order <= 128):
        raise ValueError('finite model requires integer max_generated_order in 1..128')
    peak = float(np.max(np.abs(x)))
    if peak == 0 or peak > 1e10 or peak < 1e-15:
        raise ValueError('silent or unsupported numerical amplitude')
    p = power_spectrum(x)
    ac = float(np.sum(p[1:]))
    fpower = float(p[k])
    if fpower < ac * GUARDS['fundamental_min_ac_power_fraction'] or fpower <= 0:
        raise ValueError('claimed fundamental absent or too weak to qualify')
    neighborhood = float(np.sum(p[k-2:k+3]))
    concentration = fpower / neighborhood
    if concentration < GUARDS['fundamental_neighbor_power_fraction']:
        raise ValueError('fundamental is detuned, modulated or not coherent with the window')

    q = n // 4
    taper = np.hanning(q)
    z, rms, spectra = [], [], []
    for start in range(0, n, q):
        segment = x[start:start+q] / peak
        carrier = np.exp(-2j * np.pi * k * np.arange(start, start+q) / n)
        z.append(2 * np.sum(segment * taper * carrier) / np.sum(taper))
        spectra.append(power_spectrum(segment * taper))
        rms.append(math.sqrt(float(np.sum(segment * segment * taper) / np.sum(taper))))
    z = np.asarray(z)
    deviation = float(np.max(np.abs(z / np.mean(z) - 1))) if abs(np.mean(z)) else float('inf')
    rms_spread = (max(rms) - min(rms)) / float(np.mean(rms))
    spectra = np.asarray(spectra)
    average_spectrum = np.mean(spectra, axis=0)
    spectral_change = float(np.max(np.sum(np.abs(spectra-average_spectrum), axis=1)) / np.sum(average_spectrum))
    if (rms_spread > GUARDS['quarter_rms_relative_spread_max'] or
            deviation > GUARDS['quarter_fundamental_complex_deviation_max'] or
            spectral_change > GUARDS['quarter_spectral_relative_change_max']):
        raise ValueError('nonstationary amplitude/phase: select a settled window')

    orders = np.arange(1, (n // 2 - 1) // k + 1, dtype=int)
    harmonic_bins = orders * k
    harmonic_bin_set = {int(v) for v in harmonic_bins}
    off = np.ones(len(p), dtype=bool)
    off[0] = False
    off[harmonic_bins] = False
    noncarrier = np.ones(len(p), dtype=bool)
    noncarrier[[0, k]] = False
    observed_hpower = float(np.sum(p[harmonic_bins[1:]]))
    off_power = float(np.sum(p[off]))
    spur_power = float(np.max(p[noncarrier]))
    off_spur = float(np.max(p[off]))

    folded: dict[int, list[int]] = {}
    if max_generated_order:
        for order in range(2, max_generated_order + 1):
            if order * k >= n // 2:
                remainder = (order * k) % n
                folded.setdefault(min(remainder, n - remainder), []).append(order)
    collisions = [{'bin': b, 'orders': h, 'reason': 'DC/Nyquist/intended-harmonic or multiple-fold collision'}
                  for b, h in folded.items() if b in (0, n//2) or b in harmonic_bin_set or len(h) != 1]
    harmonic_collisions = [c for c in collisions if c['bin'] in harmonic_bin_set]
    harmonic_attribution_ambiguous = bool(harmonic_collisions)
    fold_ratio = None if max_generated_order is None or collisions else sum(float(p[b]) for b in folded) / fpower
    observed_harmonic_ratio = math.sqrt(observed_hpower/fpower)
    attributable_harmonic_ratio = None if harmonic_attribution_ambiguous else observed_harmonic_ratio
    attributable_harmonic_db = None if harmonic_attribution_ambiguous else db(observed_hpower/fpower)
    largest = sorted(np.flatnonzero(noncarrier), key=lambda i: p[i], reverse=True)[:12]

    return {
        'valid': True, 'signal_class': signal_class, 'frames': n, 'rate': rate,
        'fundamental_hz': float(fundamental_hz), 'fundamental_bin': k,
        'window': 'rectangular-coherent-no-padding', 'bin_width_hz': rate/n,
        'dc': float(np.mean(x)), 'rms': float(np.sqrt(np.mean(x*x))), 'sample_peak': peak,
        'fundamental_rms': math.sqrt(fpower),
        'observed_harmonic_grid_ratio': observed_harmonic_ratio,
        'observed_harmonic_grid_to_fundamental_db': db(observed_hpower/fpower),
        'harmonic_attribution_ambiguous': harmonic_attribution_ambiguous,
        'inband_harmonic_ratio': attributable_harmonic_ratio,
        'inband_thd_ratio': (attributable_harmonic_ratio
                             if signal_class == 'sine-driven-nonlinearity' else None),
        'inband_harmonic_to_fundamental_db': attributable_harmonic_db,
        'off_harmonic_to_fundamental_db': db(off_power/fpower),
        'sfdr_including_harmonics_db': -db(spur_power/fpower),
        'off_harmonic_sfdr_db': -db(off_spur/fpower), 'db_floor': DB_FLOOR,
        'harmonics': [{'order': int(h), 'hz': float(h * fundamental_hz),
                       'rms': math.sqrt(float(p[b])), 'dbc': db(float(p[b])/fpower)}
                      for h, b in zip(orders, harmonic_bins)],
        'largest_noncarrier_bins': [{'bin': int(b), 'hz': b*rate/n,
                                     'dbc': db(float(p[b])/fpower)} for b in largest],
        'finite_model': {'max_generated_order': max_generated_order,
                         'predicted_folds': [{'orders': h, 'bin': b, 'hz': b*rate/n,
                                              'dbc': db(float(p[b])/fpower)} for b, h in sorted(folded.items())],
                         'collisions': collisions,
                         'harmonic_collisions': harmonic_collisions,
                         'identifiable_fold_ratio': fold_ratio,
                         'identifiable_fold_to_fundamental_db': db(fold_ratio) if fold_ratio is not None else None},
        'guards': {**GUARDS, 'fundamental_neighborhood_fraction': concentration,
                   'quarter_rms_relative_spread': rms_spread,
                   'quarter_spectral_relative_change': spectral_change,
                   'quarter_fundamental_complex_deviation': deviation},
        'interpretation': [
            'Harmonic energy is not automatically distortion for an oscillator.',
            'Observed harmonic-grid energy is always reported; attributable harmonic/THD values are null when a predicted fold collides with a harmonic bin.',
            'Off-harmonic energy includes noise, spurs and modulation; it is not isolated alias energy.',
            'Predicted folds require an externally justified finite harmonic model; coincident energy alone does not prove aliasing.',
            'In-band THD excludes attributable folded out-of-band harmonics; SFDR includes all non-DC/non-fundamental bins.',
            'Guard tolerances qualify measurement applicability, not musical acceptance or audibility.'],
    }


def qualify_file(src, *, rate: int, fundamental_hz: float, signal_class: str,
                 out, start: int = 0, frames: int = 32768, max_generated_order=None,
                 lab_report=None) -> dict:
    src, out = Path(src).resolve(), Path(out).resolve()
    if src == out or out in src.parents:
        raise ValueError('input must be outside the report directory')
    if lab_report and (out == Path(lab_report).resolve() or out in Path(lab_report).resolve().parents):
        raise ValueError('parent report must be outside the report directory')
    out.mkdir(parents=True, exist_ok=True)
    destination = out / 'harmonic-analysis.json'
    destination.unlink(missing_ok=True)
    if any(isinstance(v, bool) or not isinstance(v, int) for v in (start, frames)) or start < 0 or frames < 1:
        raise ValueError('invalid analysis interval')
    data = _common.read_input(src, rate, 1)
    if start + frames > len(data):
        raise ValueError('analysis interval exceeds input')
    before = sha(src)
    parent = None
    if lab_report:
        path = Path(lab_report).resolve()
        parent_hash = sha(path)
        content = json.loads(path.read_text())
        matches = [r for r in content.get('renders', []) if r.get('raw_sha256') == before]
        if not matches or any(r.get('diagnostics', {}).get(k) != v for r in matches
                              for k, v in (('frames', len(data)), ('rate', rate), ('channels', 1))):
            raise ValueError('parent report raw hash/dimensions mismatch')
        parent = {'sha256': parent_hash, 'render_labels': [r.get('label', r.get('name')) for r in matches]}
    measurement = analyze_window(data[start:start+frames, 0], rate, fundamental_hz, signal_class,
                                 max_generated_order=max_generated_order)
    if sha(src) != before or (lab_report and sha(lab_report) != parent_hash):
        raise RuntimeError('input/parent report changed during measurement')
    report = {'schema': 1, 'complete': True, 'purpose': 'offline-component-diagnostic-not-release-gate',
              'input': {'sha256': before, 'frames': len(data), 'rate': rate, 'channels': 1, 'format': 'mono-f32le'},
              'interval': {'start_frame': start, 'end_frame_exclusive': start+frames},
              'parent_report': parent, 'measurement': measurement,
              'analyzer': {'sha256': sha(__file__), 'numpy_version': np.__version__}}
    with tempfile.NamedTemporaryFile('w', dir=out, delete=False) as temp:
        json.dump(report, temp, indent=2, sort_keys=True, allow_nan=False)
        temp.write('\n')
        temporary = Path(temp.name)
    temporary.replace(destination)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--rate', type=int, required=True)
    parser.add_argument('--fundamental-hz', type=float, required=True)
    parser.add_argument('--signal-class', choices=SIGNAL_CLASSES, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--start', type=int, default=0)
    parser.add_argument('--frames', type=int, default=32768)
    parser.add_argument('--max-generated-order', type=int)
    parser.add_argument('--lab-report', type=Path)
    args = parser.parse_args()
    try:
        report = qualify_file(args.input, rate=args.rate, fundamental_hz=args.fundamental_hz,
                              signal_class=args.signal_class, out=args.out, start=args.start, frames=args.frames,
                              max_generated_order=args.max_generated_order, lab_report=args.lab_report)
    except (ValueError, OSError, RuntimeError) as error:
        parser.exit(2, str(error)+'\n')
    print(json.dumps({'report': str(args.out/'harmonic-analysis.json'), 'valid': report['measurement']['valid']}))


if __name__ == '__main__':
    main()
