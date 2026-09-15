"""Regression tests for issue #128 review findings."""
import importlib.util
import math
from pathlib import Path
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


st = load("spectral_trace_review", "tools/modules/spectral_trace.py")
ha = load("harmonic_analysis_review", "tools/modules/harmonic_analysis.py")


class SpectralMetadataReview(unittest.TestCase):
    def test_band_centers_match_pinned_faust_288_formula(self):
        """Faust 2.88.0 libraries commit f1d729e uses this exact band_center formula."""
        rate = 48000
        m = st.CONFIG["bands_per_octave"]
        top = st.CONFIG["top_hz"]
        n = st.CONFIG["bands"]
        expected = [math.sqrt(top * rate / 2)]
        expected.extend(top * 2 ** ((1 - 2*i) / (2*m)) for i in range(1, n-1))
        expected.append(0.5 * top * 2 ** ((2-n) / m))
        self.assertEqual(st.band_centers(rate), expected)
        self.assertAlmostEqual(expected[0], math.sqrt(10000 * 24000))


class HarmonicAttributionReview(unittest.TestCase):
    def test_alias_collision_with_harmonic_nulls_attributable_thd(self):
        n = 32768
        rate = 48000
        k = n // 8
        t = np.arange(n)
        # Order 7 folds to bin k, while order 9 folds to the same fundamental/harmonic grid.
        # Add a visible second harmonic so observed harmonic-grid energy is nonzero.
        x = (0.2*np.sin(2*np.pi*k*t/n) +
             0.05*np.sin(2*np.pi*2*k*t/n))
        m = ha.analyze_window(x, rate, rate*k/n, "sine-driven-nonlinearity",
                              max_generated_order=9)
        self.assertTrue(m["finite_model"]["harmonic_collisions"])
        self.assertTrue(m["harmonic_attribution_ambiguous"])
        self.assertGreater(m["observed_harmonic_grid_ratio"], 0)
        self.assertIsNone(m["inband_harmonic_ratio"])
        self.assertIsNone(m["inband_harmonic_to_fundamental_db"])
        self.assertIsNone(m["inband_thd_ratio"])

    def test_no_harmonic_collision_preserves_attributable_metrics(self):
        n = 32768
        rate = 48000
        k = 1365
        t = np.arange(n)
        x = 0.2*np.sin(2*np.pi*k*t/n) + 0.02*np.sin(2*np.pi*3*k*t/n)
        m = ha.analyze_window(x, rate, rate*k/n, "sine-driven-nonlinearity",
                              max_generated_order=3)
        self.assertFalse(m["harmonic_attribution_ambiguous"])
        self.assertAlmostEqual(m["observed_harmonic_grid_ratio"], m["inband_harmonic_ratio"])
        self.assertAlmostEqual(m["inband_harmonic_ratio"], m["inband_thd_ratio"])


if __name__ == "__main__":
    unittest.main()
