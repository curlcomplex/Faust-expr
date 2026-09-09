"""Synthetic diagnostic fixtures, not Faust or hardware validation."""
import sys,unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
from replay_tail import tail_slope

class TailMetricsTests(unittest.TestCase):
    def setUp(self):
        self.rate=48000;self.t=np.arange(3*self.rate)/self.rate
        self.signal=.5*np.sin(2*np.pi*200*self.t)
    def test_constant_tail_is_flat(self):
        self.assertLess(abs(tail_slope(self.signal,self.rate)['slope_db_per_s']),1e-9)
    def test_known_exponential_slope(self):
        s=tail_slope(self.signal*np.exp(-self.t/.33),self.rate)['slope_db_per_s']
        self.assertAlmostEqual(s,-20/np.log(10)/.33,places=6)
    def test_static_gain_does_not_change_slope(self):
        x=self.signal*np.exp(-self.t/.33)
        self.assertAlmostEqual(tail_slope(x,self.rate)['slope_db_per_s'],tail_slope(x*.2,self.rate)['slope_db_per_s'],places=9)
    def test_faster_tail_is_not_equal(self):
        a=tail_slope(self.signal*np.exp(-self.t/.3),self.rate)['slope_db_per_s']
        b=tail_slope(self.signal*np.exp(-self.t/.6),self.rate)['slope_db_per_s']
        self.assertGreater(abs(a-b),10)
    def test_bad_audio_rejected(self):
        for x in [np.zeros(3*self.rate),np.array([np.nan]),np.ones((10,2))]:
            with self.assertRaises(ValueError):tail_slope(x,self.rate)
if __name__=='__main__':unittest.main()
