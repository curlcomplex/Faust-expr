"""Verify and package the bounded #96 phase experiment, without changing DSP.

Consumes the existing H1 and phase-probe outputs. Adds cold/block repeat tests,
a gain-only counterexample and the zero-modulation carrier identity check.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import time
import traceback
import numpy as np
import lab
import dx7_slice1 as h1
import dx7_phase_probe as probe

VERSION = 'dx7-phase-evidence-1'
START = probe.GATE_ON + 4096
END = probe.GATE_OFF - 2048


def read_audio(path: Path) -> np.ndarray:
    x = np.fromfile(path, dtype='<f4')
    if len(x) != probe.FRAMES or not np.isfinite(x).all():
        raise ValueError('invalid audio: ' + str(path))
    if np.linalg.norm(x[START:END].astype(np.float64)) <= 1e-12:
        raise ValueError('silent measurement window: ' + str(path))
    return x


def gain_only_distance(x: np.ndarray, reference: np.ndarray) -> float:
    return probe.shape_distance(probe.spectral_shape(x * 2, START, END),
                                probe.spectral_shape(reference, START, END))


def raw_stats(x: np.ndarray, reference: np.ndarray) -> dict:
    rms = lambda a: float(np.sqrt(np.mean(np.asarray(a, dtype=np.float64) ** 2)))
    ratio = rms(x[START:END]) / rms(reference[START:END])
    return {'steady_over_msfa_db': float(20 * np.log10(ratio)),
            'pre_gate_peak': float(np.max(np.abs(x[:probe.GATE_ON]))),
            'raw_residual_rms': rms(x - reference)}


def file_index(root: Path) -> dict:
    return {str(p.relative_to(root)): lab.digest(p)
            for p in sorted(root.rglob('*')) if p.is_file()
            and 'deps' not in p.relative_to(root).parts
            and '.git' not in p.relative_to(root).parts
            and p.name != 'SHA256SUMS.json'}


def execute(phase: Path, baseline: Path, out: Path, faust: Path,
            libraries: Path, archive: Path) -> None:
    phase, baseline, out = phase.resolve(), baseline.resolve(), out.resolve()
    faust, libraries, archive = faust.resolve(), libraries.resolve(), archive.resolve()
    if out.exists() and any(out.iterdir()):
        raise ValueError('use a fresh evidence directory')
    out.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    report = {'schema': 1, 'version': VERSION, 'status': 'running',
              'checks': [], 'renders': [], 'rows': [],
              'scope': 'Related-software phase/index identification only; no shipping DSP edit',
              'held_out_note': '40/55/85/95 extend the 50/70/90 study in PR117; 70 overlaps. Same oracle lineage, not independent hardware evidence.'}
    def check(name, ok, **details):
        report['checks'].append({'name': name, 'passed': bool(ok), **details})
        if not ok:
            raise AssertionError(name + ': ' + str(details))
    def render(name, command, dest, expected_patch=None):
        diag = json.loads(lab.run(command))
        x = read_audio(dest)
        if expected_patch is not None:
            check(name + ':native-patch', diag['unpacked_patch'] == expected_patch)
        check(name + ':mono', diag['channels'] == 1)
        report['renders'].append({'name': name, 'command': command,
                                 'raw_sha256': lab.digest(dest), 'diagnostics': diag})
        return x
    try:
        previous = json.loads((phase / 'results.json').read_text())
        base_report = json.loads((baseline / 'results.json').read_text())
        check('probe-executed', previous['status'] == 'passed-probe')
        check('h1-executed', base_report['status'].startswith('passed-infrastructure'))
        check('same-commit', base_report['commit'] == lab.run(['git', 'rev-parse', 'HEAD']).strip())
        for rel, digest in json.loads((baseline / 'SHA256SUMS.json').read_text()).items():
            target = (baseline / rel).resolve()
            if baseline not in target.parents or lab.digest(target) != digest:
                raise ValueError('H1 evidence hash mismatch: ' + rel)
        check('h1-evidence-hashes', True)
        original_phase = file_index(phase)
        lib_hashes = h1.verify_release_libraries(libraries, archive)
        native = phase / 'deps/msfa'
        check('msfa-commit', lab.run(['git', '-C', str(native), 'rev-parse', 'HEAD']).strip() == h1.MSFA)
        report['provenance'] = {'commit': base_report['commit'], 'execution_lane': os.environ.get('EXECUTION_LANE', 'unspecified'),
            'faust_version': lab.run([str(faust), '-v']).splitlines()[0],
            'faust_binary_sha256': lab.digest(faust), 'faust_archive_sha256': lab.digest(archive),
            'libraries_sha256': lib_hashes, 'msfa_commit': h1.MSFA,
            'msfa_binary_sha256': lab.digest(phase / 'msfa-render'),
            'msfa_source_sha256': {s: lab.digest(native / 'app/src/main/jni' / s) for s in h1.SOURCES},
            'oracle_adapter_sha256': lab.digest(lab.ROOT / 'tools/modules/dx7_msfa_oracle.cpp'),
            'probe_script_sha256': lab.digest(lab.ROOT / 'tools/modules/dx7_phase_probe.py'),
            'verification_script_sha256': lab.digest(Path(__file__)),
            'cxx_version': lab.run([os.environ.get('CXX', 'c++'), '--version']).splitlines()[0],
            'rate': probe.RATE, 'frames': probe.FRAMES, 'note': probe.NOTE,
            'gate_on': probe.GATE_ON, 'gate_off': probe.GATE_OFF,
            'measurement_frames': [START, END], 'oracle_quantum_frames': 64,
            'msfa_compile_flags': h1.CPP_FLAGS, 'candidate_compile_flags': ['-std=c++17', '-O2', '-ffp-contract=off'],
            'raw_processing': 'none; no alignment, gain fitting or startup mute',
            'audition_processing': 'one fixed 0.2 gain for all engines/files; MSFA, stock Faust, phase-times-two Faust; 0.1 s gaps'}
        check('compiler-2.88', report['provenance']['faust_version'] == 'FAUST Version 2.88.0')
        repeats = out / 'repeats'; repeats.mkdir()
        listen = out / 'listening'; listen.mkdir()
        for row in previous['rows']:
            level = row['modulator_level']; d = phase / 'cases' / f'level-{level}'
            c = {'id': f'DX7-P{level}', 'family': 'C02', 'note': probe.NOTE, 'carrier_level': 80, 'modulator_level': level}
            reference = read_audio(d / 'msfa.f32')
            waves = {scale: read_audio(d / f'faust-scale-{str(scale).replace(".", "p")}.f32') for scale in probe.SCALES}
            distances = {str(scale): probe.shape_distance(probe.spectral_shape(x, START, END), probe.spectral_shape(reference, START, END)) for scale, x in waves.items()}
            check(f'P{level}:recomputed-metrics', all(abs(distances[s] - row['shape_l2_by_phase_scale'][s]) < 1e-12 for s in distances))
            final_gain_only = gain_only_distance(waves[1.0], reference)
            check(f'P{level}:final-gain-does-not-repair-shape', abs(final_gain_only - distances['1.0']) < 1e-10)
            dest = repeats / f'P{level}-faust-2-block127.f32'
            exe = phase / 'faust-builds' / f'P{level}-S2p0' / 'render'
            replay = render(f'P{level}:faust-repeat127', [str(exe), str(d / 'faust-scale-2p0.tsv'), str(dest), str(probe.RATE), '127', str(probe.FRAMES), '0'], dest)
            check(f'P{level}:faust-cold-block-identity', np.array_equal(replay, waves[2.0]))
            dest = repeats / f'P{level}-msfa-block127.f32'
            replay = render(f'P{level}:msfa-repeat127', [str(phase / 'msfa-render'), str(d / 'patch128.bin'), str(d / 'msfa.tsv'), str(dest), str(probe.RATE), '127', str(probe.FRAMES), '0'], dest, h1.patch_data(c)[1])
            check(f'P{level}:msfa-cold-block-identity', np.array_equal(replay, reference))
            if level == 70:
                check('P70:stock-faust-equals-H1-C02', np.array_equal(waves[1.0], read_audio(baseline / 'cases/DX7-C02/faust.f32')))
                check('P70:oracle-equals-H1-C02', np.array_equal(reference, read_audio(baseline / 'cases/DX7-C02/msfa.f32')))
            winner = min(distances, key=distances.get)
            report['rows'].append({'level': level, 'shape_l2': distances, 'best_of_tested_scales': float(winner),
                'gain_only_shape_l2': final_gain_only,
                'raw_stock': raw_stats(waves[1.0], reference), 'raw_phase_times_two': raw_stats(waves[2.0], reference),
                'reference_pre_gate_peak': float(np.max(np.abs(reference[:probe.GATE_ON])))})
            if level in (55, 85, 95):
                gap = np.zeros(4410)
                audition = np.concatenate([reference, gap, waves[1.0], gap, waves[2.0]]) * .2
                lab.wav(listen / f'P{level}-MSFA-stock-phase2.wav', audition, probe.RATE)
                for name, data in [('MSFA', reference), ('stock', waves[1.0]), ('phase2', waves[2.0])]:
                    h1.float_wav(listen / f'P{level}-{name}-raw.wav', data, probe.RATE)
                (listen / f'P{level}-patch128.bin').write_bytes((d / 'patch128.bin').read_bytes())
                h1.write_json(listen / f'P{level}-case.json', c)
        # Zero modulation must remain exactly the original carrier, including startup.
        carrier = {'id': 'DX7-C01', 'family': 'C01', 'note': probe.NOTE, 'carrier_level': 80, 'modulator_level': 0}
        src = out / 'carrier-phase2.dsp'; src.write_text(probe.source_for(carrier, 2.0))
        worker = lab.Lab(out / 'carrier-build'); worker.faust = str(phase / 'faust-pinned')
        exe = worker.build('carrier', src); dest = out / 'carrier-phase2.f32'
        replay = render('C01:phase2', [str(exe), str(baseline / 'cases/DX7-C01/faust.tsv'), str(dest), str(probe.RATE), '128', str(probe.FRAMES), '0'], dest)
        check('C01:complete-carrier-identity', np.array_equal(replay, read_audio(baseline / 'cases/DX7-C01/faust.f32')))
        report['carrier_build'] = worker.builds
        check('phase-evidence-not-modified', file_index(phase) == original_phase)
        check('msfa-source-unchanged', lab.run(['git', '-C', str(native), 'status', '--porcelain']).strip() == '')
        check('faust-libraries-unchanged', h1.verify_release_libraries(libraries, archive) == lib_hashes)
        report['phase_evidence_sha256'] = original_phase
        winners = [r['best_of_tested_scales'] for r in report['rows']]
        report['consistent_best_of_tested_scales'] = winners[0] if len(set(winners)) == 1 else None
        report['status'] = 'passed-evidence-checks; no hardware-fidelity-gate'
    except Exception as exc:
        report['status'] = 'failed'; report['error'] = str(exc)
        (out / 'failure.txt').write_text(traceback.format_exc())
        raise
    finally:
        report['wall_seconds'] = time.monotonic() - started
        report['counts'] = {'checks': len(report['checks']), 'passed': sum(c['passed'] for c in report['checks']),
                            'additional_actual_renders': len(report['renders'])}
        h1.write_json(out / 'verification.json', report)
        lines = ['# DX7 phase/index evidence', '', f"Status: {report['status']}", f"Counts: {report['counts']}", '',
            '| Modulator level | stock | phase x2 | final gain x2 only |', '| ---: | ---: | ---: | ---: |']
        for row in report['rows']:
            lines.append(f"| {row['level']} | {row['shape_l2']['1.0']:.8f} | {row['shape_l2']['2.0']:.8f} | {row['gain_only_shape_l2']:.8f} |")
        lines += ['', 'Metric: L2 distance of unit-norm steady FFT magnitudes, not an authenticity percentage.',
            'The phase probe is unchanged. This adds verification, repeat/block tests and audition material only.',
            'Auditions: MSFA first, stock Faust second, phase-times-two diagnostic third. The same 0.2 gain applies everywhere.',
            'Raw WAVs preserve absolute engine gain and pre-gate material. No EQ, alignment, limiter or per-engine normalization.',
            'The single-carrier identity and gain-only counterexample separate internal modulation from final output scaling.',
            'Only one 2:1 pair at 220 Hz / 44.1 kHz is covered. No full-algorithm, feedback, hardware or startup fix is claimed.',
            'Source interpretation: stock Faust returns envelope*sine*0.5; MSFA adds Q24 operator output directly to downstream phase. A twofold connection conversion is therefore source-supported, not a fitted final-volume correction.',
            'Faust/Dexed/MSFA share lineage. PR117 is corroborating software evidence, not a second independent oracle.']
        (out / 'REPORT.md').write_text('\n'.join(lines) + '\n')
        h1.write_json(out / 'SHA256SUMS.json', file_index(out))
        print('\n'.join(lines), flush=True)
        print('PHASE_EVIDENCE_SUMMARY ' + json.dumps({'status': report['status'], 'counts': report['counts'], 'rows': report['rows']}, allow_nan=False), flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    for field in ('phase', 'baseline', 'out', 'faust', 'libraries', 'archive'):
        p.add_argument('--' + field, type=Path, required=True)
    execute(**vars(p.parse_args()))
