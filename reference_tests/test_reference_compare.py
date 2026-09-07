import importlib.util
from pathlib import Path
import unittest
import numpy as np
path=Path(__file__).resolve().parents[1]/'scripts/reference_compare.py'
spec=importlib.util.spec_from_file_location('reference_compare',path)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class ReferenceMetricsTests(unittest.TestCase):
 def setUp(self):
  t=np.arange(96000)/48000
  y=np.sin(2*np.pi*1500*t)*np.exp(-2*t)
  self.x=np.column_stack([y,y])
 def test_identity(self):
  f=m.features(self.x,2);self.assertEqual(m.distance(f,f),0)
 def test_constant_gain_is_not_timbre(self):
  self.assertLess(m.distance(m.features(self.x,2),m.features(self.x*.13,2)),1e-9)
 def test_stereo_antiphase_does_not_cancel(self):
  y=self.x*np.array([1,-1]);self.assertEqual(m.features(self.x,2),m.features(y,2))
 def test_silence_rejected(self):
  with self.assertRaises(ValueError):m.features(np.zeros_like(self.x),2)
 def test_nonfinite_rejected(self):
  y=self.x.copy();y[3,0]=np.nan
  with self.assertRaises(ValueError):m.features(y,2)
 def test_pitch_change_is_detected(self):
  y=np.sin(2*np.pi*4000*np.arange(96000)/48000)
  self.assertGreater(m.distance(m.features(self.x,2),m.features(np.column_stack([y,y]),2)),5)
 def test_decay_change_is_detected(self):
  y=self.x*np.exp(-3*np.arange(96000)/48000)[:,None]
  self.assertGreater(m.distance(m.features(self.x,2),m.features(y,2)),1)
 def test_coherent_object_configuration(self):
  import json
  p=json.loads((path.parents[1]/'references/calibration-ref1.json').read_text())
  self.assertEqual(set(p['velocities']),set(p['optimizer_training']+p['excluded_from_optimizer']))
  self.assertNotIn('velocity',p['body_and_stick']);self.assertNotIn('strike_radius',p['body_and_stick'])
if __name__=='__main__':unittest.main()
