"""Synthetic equation/measurement tests; actual DSP checks are body_gate_probe."""
import importlib.util
from pathlib import Path
import unittest
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT/'tools/modules'/f'{name}.py')
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m

probe = load('body_gate_probe')

class GateEquationTests(unittest.TestCase):
    def test_silent_before_onset_and_zero_attack_sample(self):
        y = probe.expected(probe.DEFAULTS, 48000, 500, 101, 400)
        self.assertTrue(np.all(y[:102] == 0))
        self.assertGreater(float(abs(y[102:200]).max()), .1)

    def test_release_starts_at_zero_elapsed(self):
        p = probe.DEFAULTS
        a = probe.expected(p, 48000, 10000, 101, 1000)
        b = probe.expected(p, 48000, 10000, 101, 10000)
        self.assertEqual(a[1000], b[1000])
        np.testing.assert_allclose(a[1000:], b[1000:]*np.exp(-np.arange(9000)/(48000*p['release_tau_s'])), atol=1e-14)

    def test_release_oracle_rejects_one_sample_early_mutation(self):
        # Ensure the sensitive gate-boundary assertion has a real discriminator.
        p = probe.DEFAULTS
        held = probe.expected(p, 48000, 2000, 101, 2000)
        early = probe.expected(p, 48000, 2000, 101, 999)
        self.assertGreater(abs(held[1000]-early[1000]), 1e-6)

    def test_discrete_phase_agrees_with_direct_frequency_sum(self):
        p, rate = probe.DEFAULTS, 48000
        t = np.arange(10000)/rate
        f = p['frequency_hz']+p['pitch_amount_hz']*np.exp(-t/p['pitch_tau_s'])
        phase = np.r_[0, np.cumsum(f[1:])/rate]
        envelope = -np.expm1(-t/p['attack_tau_s'])*np.exp(-t/p['body_tau_s'])
        direct = p['level']*p['velocity']*envelope*np.sin(2*np.pi*(phase+p['phase_cycles']))
        np.testing.assert_allclose(probe.expected(p, rate, 10000, 0, 10000), direct, atol=1e-12)

    def test_velocity_zero_and_half(self):
        p = probe.DEFAULTS
        one = probe.expected(p, 48000, 10000, 0, 5000)
        half = probe.expected(p|{'velocity':.5}, 48000, 10000, 0, 5000)
        zero = probe.expected(p|{'velocity':0}, 48000, 10000, 0, 5000)
        np.testing.assert_array_equal(half, one*.5)
        self.assertTrue(np.all(zero == 0))

if __name__ == '__main__':
    unittest.main()
