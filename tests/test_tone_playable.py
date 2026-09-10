"""Independent contract/measurement fixtures, not a Faust render."""
import importlib.util,json,unittest
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('tone_playable',ROOT/'tools/modules/tone_playable.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class TonePlayableTests(unittest.TestCase):
 def setUp(self):self.man=json.loads((ROOT/'modules/tone-pm/playable/manifest.json').read_text())
 def test_eight_knobs(self):self.assertEqual(len(self.man['musical_control_order']),8)
 def test_distinct_sound_version(self):self.assertEqual(self.man['sound_version'],'0.2.0-experiment')
 def test_invalid_values(self):
  for k,v in [('gate_mode',.5),('ratio',float('nan')),('drive',2),('bogus',0)]:
   with self.assertRaises(ValueError):m.validate({k:v},self.man)
 def test_duplicate_events(self):
  with self.assertRaises(ValueError):m.score_rows({},[(1,'gate',1),(1,'gate',0)],self.man,20)
 def test_event_boundaries(self):
  for n in (-1,20,1.5):
   with self.assertRaises(ValueError):m.score_rows({},[(n,'gate',1)],self.man,20)
 def test_same_sample_distinct_controls(self):self.assertIn('1\tgate\t1',m.score_rows({},[(1,'ratio',.6),(1,'gate',1)],self.man,20))
 def test_silent_descriptor(self):
  with self.assertRaises(ValueError):m.descriptor(np.zeros(5000),48000)
 def test_descriptor_gain_invariance(self):
  x=np.sin(2*np.pi*220*np.arange(48000)/48000)*np.exp(-np.arange(48000)/5000)
  self.assertTrue(np.allclose(m.descriptor(x,48000),m.descriptor(x*.1,48000),atol=1e-10))
 def test_ratio_landmarks(self):
  for p,r in self.man['ratio_landmarks'].items():self.assertAlmostEqual(.25*32**float(p),r,places=12)
 def test_decay_range(self):self.assertAlmostEqual(.03*200,6)
if __name__=='__main__':unittest.main()
