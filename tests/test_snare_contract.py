import json,math,sys,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools/modules'))
from snare_batch import validate
class SnareContractTests(unittest.TestCase):
    def setUp(self): self.m=json.loads((ROOT/'modules/snare-pm/manifest.json').read_text())
    def test_eight_controls_plus_events(self):
        o=self.m['musical_control_order'];self.assertEqual(len(o),8);self.assertEqual(set(self.m['controls'])-set(o),{'gate','velocity'})
    def test_defaults(self): validate({k:v['default'] for k,v in self.m['controls'].items()},self.m)
    def test_invalids(self):
        for p in ({'shape':-1},{'drive':2},{'gate':.5},{'pitch_hz':float('nan')},{'madeup':0}):
            with self.assertRaises(ValueError): validate(p,self.m)
    def test_architectures_are_explicit(self): self.assertIn('candidate_a',self.m);self.assertIn('candidate_b',self.m)
    def test_drive_zero_is_declared_neutral(self): self.assertEqual(self.m['controls']['drive']['min'],0)
if __name__=='__main__':unittest.main()
