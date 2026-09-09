"""Synthetic/contract tests, not actual-Faust or hardware acceptance."""
import importlib.util,json,unittest
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('metal_batch',ROOT/'tools/modules/metal_batch.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class MetalTests(unittest.TestCase):
 def setUp(self):self.man=json.loads((ROOT/'modules/metal-pm/manifest.json').read_text());self.defaults={k:v['default'] for k,v in self.man['controls'].items()}
 def test_default_contract(self):m.validate(self.defaults,self.man)
 def test_invalid_values(self):
  for d in ({'bogus':1},{'drive':float('nan')},{'drive':2},{'gate':.5},{'choke':.5},{'pitch_hz':0}):
   with self.assertRaises(ValueError):m.validate(d,self.man)
 def test_duplicate_event(self):
  with self.assertRaises(ValueError):m.score_rows(self.defaults,{},[(1,'gate',1),(1,'gate',0)],100,self.man)
 def test_frames(self):
  for n in (-1,100,.5):
   with self.assertRaises(ValueError):m.score_rows(self.defaults,{},[(n,'gate',1)],100,self.man)
 def test_silent_descriptor(self):
  with self.assertRaises(ValueError):m.descriptor(np.zeros(100))
 def test_descriptor_level_invariant(self):
  x=np.sin(2*np.pi*500*np.arange(48000)/48000)*np.exp(-np.arange(48000)/12000)
  np.testing.assert_allclose(m.descriptor(x),m.descriptor(x*.5),rtol=1e-7,atol=1e-7)
 def test_finite_descriptor(self):
  with self.assertRaises(ValueError):m.descriptor(np.array([np.nan]))
 def test_disjoint_reference_split(self):self.assertFalse(set(m.TRAIN)&set(m.TEST));self.assertEqual(len(m.TRAIN)+len(m.TEST),8)
 def test_eight_musical_controls(self):self.assertEqual(len(self.man['musical_control_order']),8);self.assertNotIn('choke',self.man['musical_control_order'])
if __name__=='__main__':unittest.main()
