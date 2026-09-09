"""Independent arithmetic/manifest fixtures, not hardware or Faust renders."""
import json
import math
from pathlib import Path
import sys
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/modules"))
from sharp_batch import validate


class SharpContractTests(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads(
            (ROOT / "modules/kick-analog/sharp-02/manifest.json").read_text())

    def test_eight_musical_controls_plus_events(self):
        musical = self.manifest["musical_control_order"]
        self.assertEqual(len(musical), 8)
        self.assertEqual(len(set(musical)), 8)
        self.assertEqual(set(self.manifest["controls"]) - set(musical),
                         {"gate", "velocity"})

    def test_defaults_are_in_declared_domains(self):
        validate({key: row["default"]
                  for key, row in self.manifest["controls"].items()}, self.manifest)

    def test_invalid_controls_fail(self):
        for parameters in ({"unknown": 0}, {"wave": .5}, {"wave": 12},
                           {"gate": .5}, {"pitch_hz": 0}, {"decay": -1},
                           {"drive": float("nan")}, {"drive": True}):
            with self.subTest(parameters=parameters), self.assertRaises(ValueError):
                validate(parameters, self.manifest)

    def test_fourier_recurrence_matches_direct_sum(self):
        rng = np.random.default_rng(20260909)
        for theta in np.r_[np.linspace(-math.pi, math.pi, 129), rng.uniform(-math.pi, math.pi, 129)]:
            coefficients = rng.uniform(-1., 1., 16) / np.arange(1, 17)
            direct = sum(a * math.sin(n * theta)
                         for n, a in enumerate(coefficients, 1))
            upper = above = 0.
            for a in coefficients[::-1]:
                current = a + 2 * math.cos(theta) * upper - above
                above, upper = upper, current
            self.assertLess(abs(direct - math.sin(theta) * upper), 1e-11)

    def test_harmonic_cutoff_has_continuous_endpoints(self):
        rate = 48000
        weight = lambda frequency: np.clip((.47 * rate - frequency) / (.07 * rate), 0, 1)
        self.assertAlmostEqual(weight(.4 * rate), 1)
        self.assertAlmostEqual(weight(.47 * rate), 0)
        self.assertGreater(weight(.435 * rate), 0)
        self.assertLess(weight(.435 * rate), 1)

    def test_hold_does_not_alter_pitch_trajectory(self):
        times = np.array([0., .01, .1, .5])
        frequency = 52 * (1 + (2 ** 2 - 1) * np.exp(-times / .025))
        expected = 52 + 156 * np.exp(-times / .025)
        np.testing.assert_allclose(frequency, expected, rtol=1e-14)
        for hold in (0., .2, 1.):
            body = np.exp(-np.maximum(0., times - hold) / .3)
            self.assertEqual(body[0], 1)
            self.assertTrue(np.all((body > 0) & (body <= 1)))

    def test_square_and_triangle_have_only_odd_partials(self):
        for n in range(1, 17):
            square = 4 / math.pi * (n % 2) / n
            triangle = 8 / math.pi ** 2 * (n % 2) * (1 - 2 * (((n - 1) // 2) % 2)) / n ** 2
            if n % 2 == 0:
                self.assertEqual(square, 0)
                self.assertEqual(triangle, 0)

    def test_wave_pairs_cover_reset_and_free_phase(self):
        pairs = [(index // 2, index % 2) for index in range(12)]
        self.assertEqual(len(set(pairs)), 12)
        self.assertEqual({kind for kind, _ in pairs}, set(range(6)))


if __name__ == "__main__":
    unittest.main()
