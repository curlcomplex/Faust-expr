"""Analyzer/exit guards only; synthetic fixtures are NOT Faust renders."""
from pathlib import Path
import sys
import unittest
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
from synth_recovery import passed
from synth_recovery_verification import tone_ok


class RecoveryGuards(unittest.TestCase):
    def setUp(self):
        self.sr=48000
        self.t=np.arange(self.sr)/self.sr
        self.note=.1*np.sin(2*np.pi*220*self.t)

    def test_empty_suite_fails(self):
        self.assertFalse(passed([]))

    def test_failed_check_fails(self):
        self.assertFalse(passed([{'passed':True},{'passed':False}]))

    def test_missing_status_fails(self):
        self.assertFalse(passed([{}]))

    def test_explicit_success(self):
        self.assertTrue(passed([{'passed':True}]))

    def test_fixture_tone_passes(self):
        self.assertTrue(tone_ok(self.note,self.sr,220))

    def test_wrong_octave_fails(self):
        self.assertFalse(tone_ok(.1*np.sin(2*np.pi*440*self.t),self.sr,220))

    def test_silence_fails(self):
        self.assertFalse(tone_ok(np.zeros(self.sr),self.sr,220))

    def test_dc_fails(self):
        self.assertFalse(tone_ok(np.ones(self.sr)*.1,self.sr,220))

    def test_dc_dominated_tone_fails(self):
        self.assertFalse(tone_ok(self.note+.5,self.sr,220))

    def test_nonfinite_fails(self):
        y=self.note.copy();y[8]=np.nan
        self.assertFalse(tone_ok(y,self.sr,220))

    def test_short_input_fails(self):
        self.assertFalse(tone_ok(self.note[:64],self.sr,220))

    def test_wrong_shape_fails(self):
        self.assertFalse(tone_ok(self.note[:,None],self.sr,220))


if __name__=='__main__':
    unittest.main()
