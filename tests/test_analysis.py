"""Synthetic fixtures test the analyser only; these are NOT Faust build evidence."""
import sys
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from analyse import RATE, measure


def fixture():
    x = np.zeros(RATE)
    for start, end, frequency in ((2400, 14400, 220), (24000, 36000, 880)):
        x[start:end] = 0.15 * np.sin(2 * np.pi * frequency * np.arange(end - start) / RATE)
    return x


class AnalysisTests(unittest.TestCase):
    def test_controlled_fixture_passes(self):
        self.assertTrue(measure(fixture())["pass"])

    def test_silence_fails(self):
        self.assertFalse(measure(np.zeros(RATE))["pass"])

    def test_incorrect_pitch_fails(self):
        x = fixture()
        x[24000:36000] = 0.15 * np.sin(2 * np.pi * 440 * np.arange(12000) / RATE)
        self.assertFalse(measure(x)["pass"])

    def test_excessive_level_fails(self):
        self.assertFalse(measure(fixture() * 10)["pass"])

    def test_nonfinite_rejected(self):
        x = fixture()
        x[700] = np.nan
        with self.assertRaises(ValueError):
            measure(x)

    def test_incorrect_length_rejected(self):
        with self.assertRaises(ValueError):
            measure(fixture()[:-1])

    def test_tail_noise_fails(self):
        x = fixture()
        x[45600:] = 0.01
        self.assertFalse(measure(x)["pass"])


if __name__ == "__main__":
    unittest.main()
