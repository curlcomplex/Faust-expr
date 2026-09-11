import json,math,sys,unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
import clap_batch as c
class T(unittest.TestCase):
 def setUp(self):self.m=json.loads((c.MOD/'manifest.json').read_text())
 def test_six_columns(self):self.assertEqual(self.m['visible_columns'],['spacing','punch','decay','color','body','drive'])
 def test_decay_column3(self):self.assertEqual(self.m['visible_columns'][2],'decay')
 def test_presets(self):
  for p in json.loads((c.MOD/'patches.json').read_text())['anchors'].values():c.validate(p,self.m)
 def test_bad_controls(self):
  for d in ({'oops':0},{'pitch_hz':99},{'gate':.5},{'decay':float('nan')}):
   with self.assertRaises(ValueError):c.validate(d,self.m)
 def test_descriptors_gain_invariant(self):
  x=np.random.default_rng(2).normal(size=20000)*np.exp(-np.arange(20000)/3000);np.testing.assert_allclose(c.descriptors(x),c.descriptors(x*.25),atol=1e-9)
 def test_spacing_contract(self):
  for x in np.linspace(0,1,20):
   gap=.0015+.0155*x*x;self.assertGreaterEqual(gap,.0015);self.assertLessEqual(gap,.017)
 def test_decay_monotonic(self):
  vals=[.025*70**x for x in np.linspace(0,1,20)];self.assertTrue(all(a<b for a,b in zip(vals,vals[1:])))
if __name__=='__main__':unittest.main()
