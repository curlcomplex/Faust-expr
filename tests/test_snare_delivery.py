"""Score-boundary fixtures only, not actual DSP or fidelity tests."""
import importlib.util
import json
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/modules'))
from snare_delivery import validate_events
class SnareDeliveryTests(unittest.TestCase):
    def setUp(self):self.man=json.loads((ROOT/'modules/snare-pm/manifest.json').read_text())
    def test_same_sample_different_controls(self):
        validate_events([(10,'shape',.3),(10,'gate',1)],100,self.man)
    def test_duplicate_is_not_silently_overwritten(self):
        with self.assertRaises(ValueError):validate_events([(10,'shape',.3),(10,'shape',.5)],100,self.man)
    def test_invalid_sample_offsets(self):
        for offset in (-1,100,1.5,True):
            with self.assertRaises(ValueError):validate_events([(offset,'gate',1)],100,self.man)
    def test_invalid_control_value(self):
        for value in (float('nan'),float('inf'),-1,2,True):
            with self.assertRaises(ValueError):validate_events([(10,'shape',value)],100,self.man)
    def test_bad_control_and_gate(self):
        for key,value in (('unknown',.5),('gate',.5)):
            with self.assertRaises(ValueError):validate_events([(10,key,value)],100,self.man)
if __name__=='__main__':unittest.main()
