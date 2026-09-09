"""Arithmetic oracle tests, separate from actual-Faust tests."""
from pathlib import Path
import sys, unittest
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/modules'))
from metal_acceptance import expected_envelope, relative_error

class AcceptanceTests(unittest.TestCase):
    def test_exact_onset(self):
        x=expected_envelope(4800,48000,137,.2,.8)
        np.testing.assert_array_equal(x[:138],np.zeros(138));self.assertGreater(x[138],0)
    def test_time_across_rates(self):
        a=expected_envelope(48000,48000,2400,1,1)
        b=expected_envelope(96000,96000,4800,1,1)
        np.testing.assert_array_equal(a,b[::2])
    def test_drift_is_detected(self):
        x=expected_envelope(4800,48000,137,.2,.8)
        self.assertGreater(relative_error(x*1.02,x),.015)
    def test_nonfinite_rejected(self):
        with self.assertRaises(ValueError):relative_error(np.array([np.nan]),np.array([1.]))
    def test_silence_rejected(self):
        with self.assertRaises(ValueError):relative_error(np.zeros(10),np.zeros(10))
    def test_invalid_dimensions(self):
        for args in [(0,48000,0,.5,.5),(100,0,0,.5,.5),(100,48000,100,.5,.5)]:
            with self.assertRaises(ValueError):expected_envelope(*args)

if __name__=='__main__':unittest.main()
