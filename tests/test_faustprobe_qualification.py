"""#130: real optional-runner qualification; contracts are not backend execution."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hq130', ROOT/'tools/modules/harmonic_qualification.py')
hq = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hq)
REVISION = 'fbe0e282343cca2980642aa6dc81e2e9681bdc65'
# These belong to our finite deterministic fixtures, NOT arbitrary instruments.
POWER_TOLERANCE = 2e-5  # existing #108 analytical qualification tolerance
SAMPLE_TOLERANCE = 2e-6  # float32 interchange / independent compiler rounding


def verify_build(binary, record):
    """Check the CI build record, not merely a caller-provided revision string."""
    if (record.get('source_revision') != REVISION or
            record.get('source_tracked_clean') is not True or
            record.get('cargo_locked') is not True or
            record.get('binary_sha256') != hq.ha.sha(binary)):
        raise ValueError('faustprobe build provenance mismatch')
    for key in ('source_tree', 'cargo_lock_sha256', 'rustc_version', 'cargo_version', 'build_command', 'version'):
        if not record.get(key):
            raise ValueError('incomplete faustprobe build provenance: '+key)


class AdapterContracts(unittest.TestCase):
    def test_build_record_rejects_forged_binary_and_missing_toolchain(self):
        with self.assertRaises(ValueError):
            verify_build(sys.executable, {'source_revision': REVISION})
        record = {'source_revision': REVISION, 'source_tracked_clean': True,
                  'cargo_locked': True, 'binary_sha256': hq.ha.sha(sys.executable)}
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            verify_build(sys.executable, record)

    def test_csv_requires_complete_unique_finite_frames(self):
        for data in ('frame,out0\n0,1\n0,1\n', 'frame,out0\n0,nan\n1,1\n',
                     'frame,out0\n0,1\n', 'frame,out0\n0,1\n1,1\n2,1\n'):
            with self.assertRaises(ValueError):
                hq.parse_faustprobe_csv(data, 0, 2)

    def test_failure_preserves_raw_output_and_removes_stale_success(self):
        # Mocked transport failure, explicitly not a backend execution test.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            libs = root/'libs'
            libs.mkdir()
            for name in ('stdfaust.lib', 'analyzers.lib'):
                (libs/name).write_text('mock only')
            raw, score = root/'native.f32', root/'native.tsv'
            np.zeros(2*hq.FRAMES, '<f4').tofile(raw)
            score.write_text('0\tshape\t0\n')
            native = {'label': 'failure', 'fixture': 'oscillator', 'params': {'shape': 0},
                      'raw_path': raw.name, 'raw_sha256': hq.ha.sha(raw),
                      'score_path': score.name, 'score_sha256': hq.ha.sha(score),
                      'source_sha256': hq.ha.sha(hq.FIXTURES/'oscillator.dsp'), 'input_sha256': None,
                      'library_manifest_sha256': hashlib.sha256(json.dumps(hq.common.library_manifest(libs), sort_keys=True).encode()).hexdigest(),
                      'diagnostics': {'rate': 48000, 'block': 256}}
            evidence = root/'failure-faustprobe-execution.json'
            for response in (subprocess.CompletedProcess([], 9, 'partial CSV', 'compiler error'),
                             subprocess.CompletedProcess([], 0, 'invalid CSV', 'bad output')):
                evidence.write_text('{"status":"executed"}')
                with mock.patch.object(hq.subprocess, 'run', return_value=response):
                    with self.assertRaises((RuntimeError, ValueError)):
                        hq.compare_faustprobe(sys.executable, REVISION, native, root, libs)
                report = json.loads(evidence.read_text())
                self.assertEqual(report['status'], 'failed')
                self.assertEqual(report['exit_code'], response.returncode)
                self.assertEqual((root/'failure-faustprobe.csv').read_text(), response.stdout)
                self.assertEqual((root/'failure-faustprobe.stderr').read_text(), response.stderr)
                self.assertEqual(report['csv_sha256'], hq.ha.sha(root/'failure-faustprobe.csv'))


@unittest.skipUnless(os.environ.get('FAUSTPROBE_INTEGRATION') == '1',
                     'requires pinned faustprobe integration environment')
class RealFaustprobe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = Path(os.environ['FAUSTPROBE_EVIDENCE']).resolve()
        cls.out.mkdir(parents=True, exist_ok=True)
        cls.binary = Path(os.environ['FAUSTPROBE']).resolve()
        cls.build = json.loads(Path(os.environ['FAUSTPROBE_BUILD_RECORD']).read_text())
        verify_build(cls.binary, cls.build)
        cls.report = hq.sweep(cls.out, faust=os.environ['FAUST'], libs=os.environ['FAUST_LIBRARIES'],
                              archive=os.environ['FAUST_ARCHIVE'], rates=(48000,),
                              faustprobe=cls.binary, revision=REVISION)
        cls.native = {c['label']: c for c in cls.report['renders']}
        cls.cases = cls.report['optional_faustprobe']['cases']
        (cls.out/'faustprobe-build.json').write_text(json.dumps(cls.build, indent=2, sort_keys=True)+'\n')

    def test_actual_three_cases_and_configuration(self):
        self.assertTrue(self.report['passed'])
        self.assertEqual(self.report['optional_faustprobe']['status'], 'executed')
        self.assertEqual({c['native_case_label'] for c in self.cases},
                         {'osc-48000-6827-1', 'osc-48000-6827-2', 'cubic-48000-6827-4.0'})
        for c in self.cases:
            native = self.native[c['native_case_label']]
            self.assertEqual(c['binary_sha256'], self.build['binary_sha256'])
            self.assertEqual(c['revision_declared_by_caller'], REVISION)
            self.assertEqual(c['source_sha256'], native['source_sha256'])
            self.assertEqual(c['input_sha256'], native['input_sha256'])
            self.assertEqual(c['score_sha256'], native['score_sha256'])
            self.assertEqual(c['configuration']['controls'], native['params'])
            self.assertEqual(c['configuration']['frames_compared'], hq.FRAMES)

    def test_analytical_harmonic_and_fold_power(self):
        # A third reference: the finite signal formulas, not either compiler.
        expected = {r['label']: r for r in self.report['checks']}
        for c in self.cases:
            m, e = c['measurement'], expected[c['native_case_label']]
            self.assertAlmostEqual(m['observed_harmonic_grid_ratio']**2,
                                   e['expected_inband_harmonic_power_ratio'], delta=POWER_TOLERANCE)
            self.assertAlmostEqual(m['finite_model']['identifiable_fold_ratio'],
                                   e['expected_fold_power_ratio'], delta=POWER_TOLERANCE)

    def test_original_samples_agree_without_normalization(self):
        for c in self.cases:
            self.assertLessEqual(c['sample_difference']['maximum_absolute'], SAMPLE_TOLERANCE, c['native_case_label'])
        summary = {'scope': 'three controlled 48-kHz fixtures, not general compiler equivalence',
                   'source_revision': REVISION, 'build_record_sha256': hq.ha.sha(self.out/'faustprobe-build.json'),
                   'sample_tolerance': SAMPLE_TOLERANCE, 'absolute_power_ratio_tolerance': POWER_TOLERANCE,
                   'cases': [{'label': c['native_case_label'], 'sample_difference': c['sample_difference'],
                              'metric_deltas_db': c['metric_deltas_db'], 'csv_sha256': c['csv_sha256']}
                             for c in self.cases]}
        (self.out/'faustprobe-parity-summary.json').write_text(json.dumps(summary, indent=2)+'\n')

    def test_saved_output_recomputes_same_measurement(self):
        for c in self.cases:
            path = self.out/(c['native_case_label']+'-faustprobe.csv')
            self.assertEqual(hq.ha.sha(path), c['csv_sha256'])
            x = hq.parse_faustprobe_csv(path.read_text(), hq.FRAMES, hq.FRAMES)
            m = hq.ha.analyze_window(x, 48000, c['measurement']['fundamental_hz'],
                                     c['measurement']['signal_class'],
                                     max_generated_order=c['measurement']['finite_model']['max_generated_order'])
            self.assertEqual(m, c['measurement'])

    def test_block_sizes_and_repeatability(self):
        checks = []
        for baseline in self.cases:
            original = self.native[baseline['native_case_label']]
            previous = hq.parse_faustprobe_csv((self.out/(original['label']+'-faustprobe.csv')).read_text(), hq.FRAMES, hq.FRAMES)
            fixture = original['fixture']
            build = self.report['builds'][fixture]
            runner = self.out/'build'/fixture/'runner'
            for block in (1, 127, 256, 511):
                label = original['label']+'-block-'+str(block)
                native = hq.render_case(runner, build, self.out, label, 48000, original['bin'], original['params'], block=block)
                external = hq.compare_faustprobe(self.binary, REVISION, native, self.out, os.environ['FAUST_LIBRARIES'])
                x = hq.parse_faustprobe_csv((self.out/(label+'-faustprobe.csv')).read_text(), hq.FRAMES, hq.FRAMES)
                np.testing.assert_array_equal(x, previous)
                self.assertEqual(native['raw_sha256'], original['raw_sha256'])
                self.assertLessEqual(external['sample_difference']['maximum_absolute'], SAMPLE_TOLERANCE)
                checks.append({'case': original['label'], 'block': block, 'native_bit_identical': True,
                               'external_float32_samples_identical': True, 'comparison': external})
        (self.out/'block-checks.json').write_text(json.dumps(checks, indent=2, sort_keys=True)+'\n')


if __name__ == '__main__':
    unittest.main()
