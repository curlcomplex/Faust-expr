import sys
from pathlib import Path
import unittest
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
from tx81z_completion import audio_difference, folded_hz

class CompletionContracts(unittest.TestCase):
    def test_identity_is_zero(self):
        self.assertEqual(audio_difference([.5,-.5],[.5,-.5]),(0.,0.))
    def test_mismatch_not_hidden(self):
        self.assertEqual(audio_difference([.5,-.5],[.5,0.])[0],.5)
    def test_bad_audio_rejected(self):
        for x,y in (([],[]),([1],[1,2]),([float('nan')],[0]),([0],[float('inf')])):
            with self.assertRaises(ValueError):audio_difference(x,y)
    def test_declared_nyquist_fold(self):
        self.assertEqual(folded_hz(32640,44100),11460)
        self.assertEqual(folded_hz(32640,48000),15360)
        self.assertEqual(folded_hz(32640,96000),32640)
    def test_no_gain_fitting(self):
        self.assertGreater(audio_difference([.2,.3],[.4,.6])[0],.29)
if __name__=='__main__':unittest.main()
