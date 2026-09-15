"""#108 analytical measurement checks plus opt-in actual Faust qualification.

Numerical fixtures qualify the reducer; only the NativeQualification group
executes the real Faust compiler and native renderer.
"""
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hq', ROOT/'tools/modules/harmonic_qualification.py')
hq = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hq)
ha = hq.ha
N, RATE, K = 32768, 48000, 1365


def tone(k=K, amplitude=0.2, phase=0, frames=N):
    return amplitude*np.sin(2*np.pi*k*np.arange(frames)/N+phase)


class MeasurementTests(unittest.TestCase):
    def measure(self, x, k=K, klass='stationary-oscillator', **kwargs):
        return ha.analyze_window(x, RATE, RATE*k/N, klass, **kwargs)

    def test_pure_sine(self):
        m = self.measure(tone())
        self.assertAlmostEqual(m['fundamental_rms'], 0.2/math.sqrt(2), places=12)
        self.assertLess(m['off_harmonic_to_fundamental_db'], -140)
        self.assertIsNone(m['inband_thd_ratio'])
        self.assertIsNone(m['finite_model']['identifiable_fold_ratio'])

    def test_known_harmonics_and_stronger_second_do_not_change_fundamental(self):
        m = self.measure(tone()+tone(K*2, 0.4)+tone(K*3, 0.02), klass='sine-driven-nonlinearity')
        self.assertEqual(m['fundamental_bin'], K)
        self.assertAlmostEqual(m['inband_thd_ratio'], math.sqrt(4.01), places=10)
        self.assertAlmostEqual(m['sfdr_including_harmonics_db'], -20*math.log10(2), places=10)
        self.assertGreater(m['off_harmonic_sfdr_db'], 140)

    def test_known_offgrid_spur_is_not_automatically_aliasing(self):
        m = self.measure(tone()+tone(8761, 0.02))
        self.assertAlmostEqual(m['off_harmonic_to_fundamental_db'], -20, places=10)
        self.assertAlmostEqual(m['off_harmonic_sfdr_db'], 20, places=10)
        self.assertIsNone(m['finite_model']['identifiable_fold_ratio'])

    def test_known_fold_model(self):
        k = 6827
        m = self.measure(tone(k)+tone(3*k, 0.02), k, max_generated_order=3)
        folded = m['finite_model']
        self.assertFalse(folded['collisions'])
        self.assertEqual(folded['predicted_folds'][0]['bin'], N-3*k)
        self.assertAlmostEqual(folded['identifiable_fold_ratio'], 0.01, places=11)

    def test_alias_harmonic_collisions_are_unidentifiable(self):
        k = N//8
        m = self.measure(tone(k)+tone(7*k, 0.01), k, max_generated_order=8)
        self.assertTrue(m['finite_model']['collisions'])
        self.assertIsNone(m['finite_model']['identifiable_fold_ratio'])
        self.assertIsNone(m['finite_model']['identifiable_fold_to_fundamental_db'])

    def test_phase_and_delay_are_not_aliasing(self):
        x = tone()+tone(K*3, 0.03)
        y = np.roll(x, 101)
        a, b = self.measure(x), self.measure(y)
        self.assertGreater(np.sqrt(np.mean((x-y)**2)), 0.1)
        for field in ('inband_harmonic_ratio', 'sfdr_including_harmonics_db'):
            self.assertAlmostEqual(a[field], b[field], places=11)
        self.assertLess(b['off_harmonic_to_fundamental_db'], -140)

    def test_filter_harmonic_balance_change_not_aliasing(self):
        a = self.measure(tone()+tone(K*3, 0.03))
        b = self.measure(tone(phase=0.7)+tone(K*3, 0.01, phase=1.1))
        self.assertGreater(a['inband_harmonic_ratio'], b['inband_harmonic_ratio']*2)
        self.assertLess(b['off_harmonic_to_fundamental_db'], -140)

    def test_amplitude_scaling_and_dc(self):
        a = self.measure(tone()+tone(K*3, 0.03))
        b = self.measure(2.4*(tone()+tone(K*3, 0.03))+0.3)
        self.assertAlmostEqual(b['dc'], 0.3, places=10)
        self.assertAlmostEqual(a['inband_harmonic_ratio'], b['inband_harmonic_ratio'], places=10)

    def test_parseval_power(self):
        x = np.random.default_rng(108).normal(size=N)
        self.assertAlmostEqual(float(ha.power_spectrum(x).sum()), float(np.mean(x*x)), places=12)

    def test_missing_wrong_and_subharmonic_fundamental_rejected(self):
        for x in (tone(K*2), tone(K+1), tone(K/2)):
            with self.assertRaises(ValueError):
                self.measure(x)

    def test_noncoherent_declaration_and_actual_signal_rejected(self):
        with self.assertRaisesRegex(ValueError, 'noncoherent'):
            self.measure(tone(K+0.3), K+0.3)
        with self.assertRaisesRegex(ValueError, 'detuned|coherent|nonstationary'):
            self.measure(tone(K+0.3))

    def test_envelope_and_frequency_modulation_rejected(self):
        x = tone()
        for bad in (x*np.linspace(0.1, 1, N), x*np.exp(-np.arange(N)/8000),
                    .2*np.sin(2*np.pi*K*np.arange(N)/N+0.5*np.sin(2*np.pi*np.arange(N)/N))):
            with self.assertRaises(ValueError):
                self.measure(bad)

    def test_changing_harmonic_balance_rejected(self):
        x = tone()
        x[:N//2] += tone(K*2, .2)[:N//2]
        x[N//2:] += tone(K*3, .2)[N//2:]
        with self.assertRaisesRegex(ValueError, 'nonstationary'):
            self.measure(x)

    def test_noise_and_inharmonic_classes_rejected(self):
        for klass in ('noise', 'cymbal', 'clap', 'inharmonic', ''):
            with self.assertRaisesRegex(ValueError, 'signal class'):
                self.measure(tone(), klass=klass)
        with self.assertRaises(ValueError):
            self.measure(np.random.default_rng(108).normal(size=N))

    def test_bad_arrays_rates_and_dimensions(self):
        for x in ([], np.zeros(N), np.ones(N), np.full(N, np.nan),
                  np.full(N, np.inf), np.ones((N, 2)), np.ones(N, complex), tone()[:1001]):
            with self.assertRaises(ValueError):
                self.measure(x)
        for rate in (0, True, 48000.5, 500000):
            with self.assertRaises(ValueError):
                ha.analyze_window(tone(), rate, 1000, 'stationary-oscillator')
        for k in (1, N//2):
            with self.assertRaises(ValueError):
                self.measure(tone(), k)

    def test_bad_finite_models_rejected(self):
        for value in (0, -1, 129, 1.5, True):
            with self.assertRaises(ValueError):
                self.measure(tone(), max_generated_order=value)


class FileAndAdapterTests(unittest.TestCase):
    def test_file_hash_parent_immutability_and_stale_failure(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            src, parent, out = root/'x.f32', root/'lab.json', root/'results'
            tone().astype('<f4').tofile(src)
            parent.write_text(json.dumps({'renders': [{'raw_sha256': ha.sha(src), 'label': 'fixture',
                                                      'diagnostics': {'frames': N, 'rate': RATE, 'channels': 1}}]}))
            hashes = ha.sha(src), ha.sha(parent)
            r = ha.qualify_file(src, rate=RATE, fundamental_hz=RATE*K/N,
                                signal_class='stationary-oscillator', out=out, lab_report=parent)
            self.assertTrue(r['complete'])
            self.assertEqual((ha.sha(src), ha.sha(parent)), hashes)
            self.assertEqual(r['parent_report']['sha256'], hashes[1])
            with self.assertRaises(ValueError):
                ha.qualify_file(src, rate=RATE, fundamental_hz=333, signal_class='stationary-oscillator', out=out)
            self.assertFalse((out/'harmonic-analysis.json').exists())

    def test_mismatched_parent_and_unsafe_output_rejected(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            src, parent = root/'x.f32', root/'lab.json'
            tone().astype('<f4').tofile(src)
            parent.write_text('{"renders": []}')
            for out, report in ((root, None), (root/'out', parent)):
                with self.assertRaises(ValueError):
                    ha.qualify_file(src, rate=RATE, fundamental_hz=RATE*K/N,
                                    signal_class='stationary-oscillator', out=out, lab_report=report)

    def test_public_cli_and_nonzero_failure_from_another_directory(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            tone().astype('<f4').tofile(root/'x.f32')
            command = [sys.executable, str(ROOT/'tools/modules/harmonic_analysis.py'), 'x.f32',
                       '--out', 'result', '--rate', str(RATE), '--fundamental-hz', str(RATE*K/N),
                       '--signal-class', 'stationary-oscillator']
            result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            command[command.index('--fundamental-hz')+1] = '555.5'
            bad = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=20)
            self.assertNotEqual(bad.returncode, 0)
            self.assertFalse((root/'result/harmonic-analysis.json').exists())

    def test_faustprobe_csv_transport_only(self):
        good = 'frame,out0\n32768,0.25\n32769,-0.5\n'
        self.assertTrue(np.array_equal(hq.parse_faustprobe_csv(good, 32768, 2), [.25, -.5]))
        for bad in (good.replace('32769', '32770'), good.replace('-0.5', 'NaN'),
                    good.replace(',out0', ',out0,out1'), 'frame,out0\n32768,0.25\n'):
            with self.assertRaises(ValueError):
                hq.parse_faustprobe_csv(bad, 32768, 2)

    def test_empty_sweep_is_not_a_success(self):
        with self.assertRaises(ValueError):
            hq.sweep(Path('unused'), faust='unused', libs='unused', archive='unused', rates=())

    def test_optional_adapter_argument_transport_mock_not_backend(self):
        # Exercise command construction and parse/reduce using synthetic CSV;
        # subprocess is mocked, so this is explicitly NOT a Cranelift test.
        import hashlib
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            libs = root/'libs'
            libs.mkdir()
            for name in ('stdfaust.lib', 'analyzers.lib'):
                (libs/name).write_text('transport test only')
            x = tone().astype('<f4')
            raw = root/'native.f32'
            x.tofile(raw)
            native = {'label': 'transport', 'fixture': 'oscillator', 'params': {'shape': 0},
                      'raw_path': raw.name, 'raw_sha256': ha.sha(raw),
                      'source_sha256': ha.sha(hq.FIXTURES/'oscillator.dsp'),
                      'library_manifest_sha256': hashlib.sha256(json.dumps(hq.common.library_manifest(libs), sort_keys=True).encode()).hexdigest(),
                      'diagnostics': {'rate': RATE, 'block': 256}, 'fundamental_hz': RATE*K/N,
                      'measurement': ha.analyze_window(x, RATE, RATE*K/N, 'stationary-oscillator')}
            csv = 'frame,out0\n'+''.join(f'{N+i},{value:.17g}\n' for i, value in enumerate(x))
            response = subprocess.CompletedProcess([], 0, csv, 'mock only')
            with mock.patch.object(hq.subprocess, 'run', return_value=response) as invoked:
                result = hq.compare_faustprobe(sys.executable, 'a'*40, native, root, libs)
            argv = invoked.call_args.args[0]
            self.assertIn('--double', argv)
            self.assertEqual(argv[argv.index('--skip')+1], str(N))
            self.assertEqual(argv[argv.index('--sr')+1], str(RATE))
            self.assertEqual(result['native_case_label'], 'transport')
            self.assertAlmostEqual(result['metric_deltas_db']['sfdr_including_harmonics_db'], 0)

    def test_optional_backend_requires_revision(self):
        with self.assertRaisesRegex(ValueError, 'revision'):
            hq.compare_faustprobe('unused', '', {}, Path('.'), Path('.'))


@unittest.skipUnless(os.environ.get('HARMONIC_INTEGRATION') == '1', 'requires pinned Faust native integration')
class NativeQualification(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.out = Path(os.environ.get('HARMONIC_EVIDENCE', cls.tmp.name)).resolve()
        cls.report = hq.sweep(cls.out, faust=os.environ['FAUST'], libs=os.environ['FAUST_LIBRARIES'],
                              archive=os.environ['FAUST_ARCHIVE'], cxx=os.environ.get('CXX', 'c++'))

    def test_all_native_sweep_points_match_analytic_power(self):
        self.assertEqual(len(self.report['renders']), 72)
        self.assertTrue(self.report['passed'])
        self.assertTrue(all(c['passed'] for c in self.report['checks']))
        self.assertEqual(self.report['optional_faustprobe']['status'], 'not_run')

    def test_high_register_aliasing_does_not_hide_in_thd(self):
        rows = {r['label']: r['measurement'] for r in self.report['renders']}
        clean = rows['osc-48000-6827-1']
        aliased = rows['osc-48000-6827-2']
        self.assertLess(clean['off_harmonic_to_fundamental_db'], -110)
        self.assertGreater(aliased['off_harmonic_to_fundamental_db'], -10)
        # The in-band second partial is identical despite much more folded energy.
        self.assertAlmostEqual(clean['inband_harmonic_ratio'], aliased['inband_harmonic_ratio'], delta=1e-6)
        cubic = rows['cubic-48000-6827-4.0']
        self.assertLess(cubic['inband_thd_ratio'], 1e-6)
        self.assertGreater(cubic['finite_model']['identifiable_fold_to_fundamental_db'], -20)

    def test_drive_sweep_distortion_direction(self):
        rows = {r['label']: r['measurement'] for r in self.report['renders']}
        a = rows['cubic-48000-1365-0.25']['inband_thd_ratio']
        b = rows['cubic-48000-1365-4.0']['inband_thd_ratio']
        self.assertGreater(b, a*8)

    def test_all_saved_inputs_reports_and_scores_reconcile(self):
        for row in self.report['renders']:
            for key, filekey in (('raw_sha256', 'raw_path'), ('score_sha256', 'score_path'),
                                 ('report_sha256', 'report_path')):
                self.assertEqual(ha.sha(self.out/row[filekey]), row[key])
            report = json.loads((self.out/row['report_path']).read_text())
            self.assertEqual(report['input']['sha256'], row['raw_sha256'])
            self.assertEqual(report['measurement'], row['measurement'])

    def test_native_block_sizes_and_fresh_state(self):
        b = self.report['builds']['oscillator']
        runner = self.out/'build/oscillator/runner'
        hashes = []
        for block in (1, 127, 256, 511):
            case = hq.render_case(runner, b, self.out, f'block-{block}', 48000, 4093, {'shape': 2}, block=block)
            hashes.append(case['raw_sha256'])
        self.assertEqual(len(set(hashes)), 1)

    def test_changed_binary_rejected_before_execution(self):
        b = dict(self.report['builds']['oscillator'], binary_sha256='0'*64)
        with self.assertRaisesRegex(ValueError, 'binary hash'):
            hq.render_case(self.out/'build/oscillator/runner', b, self.out, 'never-run', RATE, K, {'shape': 0})


if __name__ == '__main__':
    unittest.main()
