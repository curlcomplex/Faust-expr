"""Evidence-helper tests; these synthetic fixtures do not certify a DX7 render."""
from pathlib import Path
import sys
import tempfile
import unittest
import numpy as np
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/modules'))
import dx7_phase_probe as p
import dx7_phase_evidence as e


class PhaseEvidenceTests(unittest.TestCase):
    def test_gain_only_cannot_change_shape_distance(self):
        t = np.arange(p.FRAMES) / p.RATE
        a = np.sin(2*np.pi*220*t) + .2*np.sin(2*np.pi*660*t)
        b = np.sin(2*np.pi*220*t) + .6*np.sin(2*np.pi*660*t)
        original = p.shape_distance(p.spectral_shape(a, e.START, e.END), p.spectral_shape(b, e.START, e.END))
        self.assertGreater(original, .1)
        self.assertAlmostEqual(e.gain_only_distance(a, b), original, places=12)

    def test_audio_validation_rejects_missing_information(self):
        with tempfile.TemporaryDirectory() as directory:
            f = Path(directory) / 'test.f32'
            for x in (np.zeros(p.FRAMES), np.ones(p.FRAMES-1), np.full(p.FRAMES, np.nan)):
                np.asarray(x, dtype='<f4').tofile(f)
                with self.assertRaises(ValueError):
                    e.read_audio(f)
            x = np.sin(np.arange(p.FRAMES) * .01).astype('<f4')
            x.tofile(f)
            self.assertTrue(np.array_equal(e.read_audio(f), x))

    def test_raw_gain_is_not_normalized_away(self):
        t = np.arange(p.FRAMES) / p.RATE
        reference = np.sin(2*np.pi*220*t)
        stats = e.raw_stats(reference * .5, reference)
        self.assertAlmostEqual(stats['steady_over_msfa_db'], -6.020599913, places=8)
        self.assertGreater(stats['pre_gate_peak'], .49)


if __name__ == '__main__':
    unittest.main()
