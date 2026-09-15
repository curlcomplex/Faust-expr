#!/usr/bin/env python3
"""#108 bounded actual-Faust oscillator/waveshaper sweeps and optional second backend."""
from __future__ import annotations

import argparse
import csv
import importlib.util
import io
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import subprocess

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location('_harmonic_metrics', ROOT / 'tools/modules/harmonic_analysis.py')
ha = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ha)
common = ha._common
FIXTURES = ROOT / 'tests/fixtures/harmonics'
RENDERER = ROOT / 'tools/modules/render.cpp'
FRAMES = 32768
BINS = (137, 1365, 4093, 6827)
RATES = (44100, 48000, 96000)
DRIVES = (0.25, 1.0, 4.0)


def environment():
    return {'system': platform.system(), 'architecture': platform.machine(),
            'execution_lane': os.environ.get('EXECUTION_LANE', 'assistant-sandbox-or-local'),
            'github_sha': os.environ.get('GITHUB_SHA'), 'github_run_id': os.environ.get('GITHUB_RUN_ID')}


def build_fixture(name, out, *, faust, libs, archive, cxx='c++'):
    if name not in ('oscillator', 'cubic'):
        raise ValueError('unknown qualification fixture')
    libraries = Path(libs).resolve()
    manifest = common.library_manifest(libraries)
    archive_hash = common.verify_release_archive(Path(archive), manifest)
    compiler, cpp = common.executable(faust), common.executable(cxx)
    version = common.run([compiler, '--version'])
    if not re.search(r'^FAUST Version 2\.88\.0\s*$', version, re.M):
        raise ValueError('qualification requires exactly Faust 2.88.0')
    source = FIXTURES / (name + '.dsp')
    out = Path(out).resolve()
    if any(out == p or out in p.parents for p in (source, RENDERER, libraries, compiler, Path(archive).resolve())):
        raise ValueError('build output must not contain source or toolchain inputs')
    out.mkdir(parents=True, exist_ok=True)
    generated, expanded, runner = out/'generated.hpp', out/'expanded.dsp', out/'runner'
    runner.unlink(missing_ok=True)
    commands = [[compiler, '-I', libraries, '-e', source, '-o', expanded],
                [compiler, '-I', libraries, '-lang', 'cpp', '-double', '-cn', 'ModuleDSP', source, '-o', generated],
                [cpp, '-std=c++17', '-O2', '-ffp-contract=off', '-I'+str(out), RENDERER, '-o', runner]]
    for index, command in enumerate(commands):
        try:
            log = common.run(command)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            (out/f'build-{index}.log').write_text(str(error)+'\n')
            raise
        (out/f'build-{index}.log').write_text(log+'\n')
    provenance = {'fixture': name, 'source_sha256': ha.sha(source),
                  'expanded_sha256': ha.sha(expanded), 'generated_sha256': ha.sha(generated),
                  'binary_sha256': ha.sha(runner), 'renderer_sha256': ha.sha(RENDERER),
                  'faust_version': version, 'faust_binary_sha256': ha.sha(compiler),
                  'release_archive_sha256': archive_hash, 'library_manifest': manifest,
                  'cxx_version': common.run([cpp, '--version']),
                  'commands': [[str(x) for x in command] for command in commands],
                  'internal_precision': 'double', 'io_precision': 'float32', 'build_environment': environment()}
    (out/'build.json').write_text(json.dumps(provenance, indent=2, sort_keys=True)+'\n')
    return runner, provenance


def render_case(runner, provenance, out, label, rate, bin_index, params, *, block=256):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+', label) or label in ('.', '..'):
        raise ValueError('invalid render label')
    if ha.sha(runner) != provenance['binary_sha256']:
        raise ValueError('native binary hash mismatch')
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    raw, score = out/(label+'.f32'), out/(label+'.tsv')
    freq = rate*bin_index/FRAMES
    values = dict(params)
    if provenance['fixture'] == 'oscillator':
        values['frequency_hz'] = freq
    score.write_text(''.join(f'0\t{k}\t{v:.17g}\n' for k, v in sorted(values.items())))
    args = [runner, score, raw, rate, block, FRAMES*2, 0]
    excitation = None
    if provenance['fixture'] == 'cubic':
        excitation = out/(label+'-input.f32')
        x = 0.4*np.sin(2*np.pi*bin_index*np.arange(FRAMES*2)/FRAMES)
        x.astype('<f4').tofile(excitation)
        args.append(excitation)
    diagnostics = json.loads(common.run(args))
    if diagnostics['frames'] != FRAMES*2 or diagnostics['channels'] != 1:
        raise RuntimeError('native render dimensions mismatch')
    if raw.stat().st_size != FRAMES*2*4:
        raise RuntimeError('native raw length mismatch')
    klass = 'stationary-oscillator' if excitation is None else 'sine-driven-nonlinearity'
    order = 8 if excitation is None else 3
    report = ha.qualify_file(raw, rate=rate, fundamental_hz=freq, signal_class=klass,
                            out=out/label, start=FRAMES, frames=FRAMES, max_generated_order=order)
    return {'label': label, 'fixture': provenance['fixture'], 'params': values,
            'source_sha256': provenance['source_sha256'],
            'library_manifest_sha256': hashlib.sha256(json.dumps(provenance['library_manifest'], sort_keys=True).encode()).hexdigest(),
            'fundamental_hz': freq, 'bin': bin_index, 'raw_path': raw.name,
            'raw_sha256': ha.sha(raw), 'score_path': score.name, 'score_sha256': ha.sha(score),
            'input_sha256': ha.sha(excitation) if excitation else None,
            'diagnostics': diagnostics, 'build': provenance['fixture'],
            'report_path': label+'/harmonic-analysis.json',
            'report_sha256': ha.sha(out/label/'harmonic-analysis.json'),
            'measurement': report['measurement']}


def parse_faustprobe_csv(text: str, start: int, frames: int) -> np.ndarray:
    """Strict mono full-cadence CSV contract. This parser is not a backend test."""
    rows = csv.reader(io.StringIO(text))
    if next(rows, None) != ['frame', 'out0']:
        raise ValueError('faustprobe CSV must be mono frame,out0')
    result = []
    for index, row in enumerate(rows):
        if len(row) != 2 or int(row[0]) != start+index or index >= frames:
            raise ValueError('faustprobe frame coverage mismatch')
        value = float(row[1])
        if not math.isfinite(value) or abs(value) > np.finfo(np.float32).max:
            raise ValueError('faustprobe nonfinite/out-of-range sample')
        result.append(value)
    if len(result) != frames:
        raise ValueError('faustprobe truncated frame coverage')
    return np.asarray(result, dtype='<f4')


def compare_faustprobe(binary, revision: str, native: dict, out, libs):
    """Explicit optional adapter. Neither backend is presumed to be an oracle.

    Use CSV and the SAME reducer: faustprobe's built-in SFDR excludes harmonics
    and uses Blackman-Harris, unlike our standard inclusive coherent SFDR.
    """
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('supply full upstream faust-rs source revision for optional evidence')
    executable = common.executable(str(binary))
    out = Path(out).resolve()
    case = native
    label = case['label']
    source = FIXTURES/(case['fixture']+'.dsp')
    if ha.sha(source) != case['source_sha256'] or ha.sha(out/case['raw_path']) != case['raw_sha256']:
        raise ValueError('native source/output changed before optional comparison')
    manifest_hash = hashlib.sha256(json.dumps(common.library_manifest(Path(libs).resolve()), sort_keys=True).encode()).hexdigest()
    if manifest_hash != case['library_manifest_sha256']:
        raise ValueError('optional backend libraries differ from native libraries')
    if case['fixture'] == 'cubic' and ha.sha(out/(label+'-input.f32')) != case['input_sha256']:
        raise ValueError('optional backend excitation differs from native input')
    excitation = 'zero' if case['fixture'] == 'oscillator' else 'file:'+str(out/(label+'-input.f32'))
    args = [executable, '-I', Path(libs).resolve(), '--double', '--sr', case['diagnostics']['rate'],
            '--block', case['diagnostics']['block'], '-n', FRAMES*2, '--skip', FRAMES,
            '--every', 1, '--in', excitation, '--format', 'csv']
    for key, value in sorted(case['params'].items()):
        args += ['--set', f'{key}={value:.17g}']
    args.append(source)
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=180)
    (out/(label+'-faustprobe.stderr')).write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(f'optional faustprobe failed ({result.returncode}): {result.stderr}')
    data = parse_faustprobe_csv(result.stdout, FRAMES, FRAMES)
    csv_path = out/(label+'-faustprobe.csv')
    csv_path.write_text(result.stdout)
    measured = ha.analyze_window(data, case['diagnostics']['rate'], case['fundamental_hz'],
                                case['measurement']['signal_class'],
                                max_generated_order=case['measurement']['finite_model']['max_generated_order'])
    return {'status': 'executed', 'backend': 'faust-rs/Cranelift',
            'revision_declared_by_caller': revision, 'binary_sha256': ha.sha(executable),
            'commands': [str(a) for a in args], 'csv_sha256': ha.sha(csv_path),
            'native_case_label': label, 'source_sha256': case['source_sha256'],
            'metric_deltas_db': {key: measured[key]-case['measurement'][key] for key in
                                 ('sfdr_including_harmonics_db', 'off_harmonic_sfdr_db', 'inband_harmonic_to_fundamental_db')},
            'measurement': measured, 'execution_environment': environment(),
            'interpretation': 'investigation only; no automatic winner, tolerance or sonic acceptance'}


def sweep(out, *, faust, libs, archive, cxx='c++', rates=RATES, faustprobe=None, revision=None):
    out = Path(out).resolve()
    if not rates or len(set(rates)) != len(rates) or any(r not in RATES for r in rates):
        raise ValueError('qualification rates must be a nonempty unique subset of RATES')
    if faustprobe and not re.fullmatch(r'[0-9a-f]{40}', revision or ''):
        raise ValueError('optional faustprobe requires a full source revision')
    out.mkdir(parents=True, exist_ok=True)
    destination = out/'qualification.json'
    destination.unlink(missing_ok=True)
    builds = {name: build_fixture(name, out/'build'/name, faust=faust, libs=libs, archive=archive, cxx=cxx)
              for name in ('oscillator', 'cubic')}
    rows = []
    checks = []
    for rate in rates:
        for k in BINS:
            # frequency slider tops at 24 kHz; bins/rates here never exceed it.
            for shape in (0, 1, 2):
                rows.append(render_case(*builds['oscillator'], out, f'osc-{rate}-{k}-{shape}', rate, k, {'shape': shape}))
            for drive in DRIVES:
                rows.append(render_case(*builds['cubic'], out, f'cubic-{rate}-{k}-{drive}', rate, k, {'drive': drive}))
    for row in rows:
        m, k = row['measurement'], row['bin']
        if row['fixture'] == 'oscillator':
            shape = row['params']['shape']
            expected_h = sum(1/h**2 for h in range(2, 9) if h*k < FRAMES/2) if shape else 0.0
            expected_fold = sum(1/h**2 for h in range(2, 9) if h*k > FRAMES/2) if shape == 2 else 0.0
        else:
            d, a = row['params']['drive'], 0.4
            ratio = (d*a**3/4)/(a+3*d*a**3/4)
            expected_h = ratio**2 if 3*k < FRAMES/2 else 0.0
            expected_fold = ratio**2 if 3*k > FRAMES/2 else 0.0
        actual_fold = m['finite_model']['identifiable_fold_ratio']
        passed = (abs(m['inband_harmonic_ratio']**2 - expected_h) < 2e-5 and
                  actual_fold is not None and abs(actual_fold-expected_fold) < 2e-5)
        checks.append({'label': row['label'], 'passed': passed,
                       'expected_inband_harmonic_power_ratio': expected_h,
                       'expected_fold_power_ratio': expected_fold,
                       'measured_fold_power_ratio': actual_fold, 'absolute_power_ratio_tolerance': 2e-5})
    report = {'schema': 1, 'complete': True, 'passed': all(c['passed'] for c in checks),
              'purpose': 'measurement-fixture-qualification-not-instrument-acceptance',
              'profile': {'fft_frames': FRAMES, 'warmup_frames': FRAMES, 'bins': list(BINS),
                          'rates': list(rates), 'shapes': [0, 1, 2], 'drives': list(DRIVES)},
              'builds': {name: b[1] for name, b in builds.items()}, 'renders': rows, 'checks': checks,
              'analyzer_sha256': ha.sha(ha.__file__), 'sweep_sha256': ha.sha(__file__),
              'execution_environment': environment(),
              'optional_faustprobe': {'status': 'not_run', 'required_for_native_qualification': False}}
    if faustprobe:
        chosen = [r for r in rows if r['diagnostics']['rate'] == 48000 and
                  ((r['fixture'] == 'oscillator' and r['bin'] == 6827 and r['params']['shape'] in (1, 2)) or
                   (r['fixture'] == 'cubic' and r['bin'] == 6827 and r['params']['drive'] == 4.0))]
        if not chosen:
            raise ValueError('optional comparison requires the 48000-Hz sweep')
        report['optional_faustprobe'] = {'status': 'executed', 'required_for_native_qualification': False,
            'cases': [compare_faustprobe(faustprobe, revision or '', r, out, libs) for r in chosen]}
    destination.write_text(json.dumps(report, indent=2, sort_keys=True, allow_nan=False)+'\n')
    if not report['passed']:
        raise AssertionError('analytic harmonic/fold qualification failed; see qualification.json')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--faust', default=os.environ.get('FAUST', 'faust'))
    p.add_argument('--faust-libraries', default=os.environ.get('FAUST_LIBRARIES'), required=not os.environ.get('FAUST_LIBRARIES'))
    p.add_argument('--faust-archive', default=os.environ.get('FAUST_ARCHIVE'), required=not os.environ.get('FAUST_ARCHIVE'))
    p.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    p.add_argument('--faustprobe', type=Path)
    p.add_argument('--faustprobe-revision')
    a = p.parse_args()
    try:
        r = sweep(a.out, faust=a.faust, libs=a.faust_libraries, archive=a.faust_archive, cxx=a.cxx,
                  faustprobe=a.faustprobe, revision=a.faustprobe_revision)
    except (ValueError, OSError, RuntimeError, AssertionError, subprocess.TimeoutExpired) as error:
        p.exit(2, str(error)+'\n')
    print(json.dumps({'report': str(a.out/'qualification.json'), 'renders': len(r['renders']), 'passed': r['passed']}))


if __name__ == '__main__':
    main()
