"""Pure numerical input/negative-control contracts; not Faust execution."""
import sys
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
from tx81z_wave_qualification import compare_table

class TableContracts(unittest.TestCase):
    def test_identity(self):
        self.assertTrue(compare_table(np.ones(1024),np.ones(1024))['passed'])
    def test_gain_mutant(self):
        self.assertFalse(compare_table(np.ones(1024)*.5,np.ones(1024))['passed'])
    def test_polarity_mutant(self):
        self.assertFalse(compare_table(-np.ones(1024),np.ones(1024))['passed'])
    def test_invalid_shape(self):
        with self.assertRaises(ValueError): compare_table(np.zeros(1023),np.zeros(1024))
    def test_nonfinite(self):
        for v in [np.nan,np.inf]:
            with self.assertRaises(ValueError): compare_table(np.full(1024,v),np.zeros(1024))
if __name__=='__main__': unittest.main()
