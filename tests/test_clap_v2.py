"""Arithmetic/schema tests; the DSP suite runs separately."""
import json,sys,unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
import clap_v2_delivery as c
class TestClap(unittest.TestCase):
 def setUp(self):self.m=json.loads((c.MOD/'manifest.json').read_text())
 def test_seven_controls(self):self.assertEqual(len(self.m['musical_control_order'][1:]),7)
 def test_body_pair(self):self.assertEqual(self.m['tracker_columns'][4],'body/body_env')
 def test_decay_column(self):self.assertEqual(self.m['tracker_columns'][2],'decay')
 def test_valid_anchors(self):
  for p in json.loads((c.MOD/'patches.json').read_text())['anchors'].values():c.validate(p,self.m)
 def test_unknown_control(self):
  with self.assertRaises(ValueError):c.validate({'wrong':1},self.m)
 def test_nonfinite(self):
  for v in (float('inf'),float('nan')):
   with self.assertRaises(ValueError):c.validate({'drive':v},self.m)
 def test_bounds(self):
  with self.assertRaises(ValueError):c.validate({'body':1.1},self.m)
 def test_gate(self):
  with self.assertRaises(ValueError):c.validate({'gate':.5},self.m)
 def test_envelope_origin(self):
  p=dict(spacing=.5,punch=.5,decay=.5,body_env=.5)
  self.assertTrue(all(x[0]==0 for x in c.envelope_oracle(np.array([0.]),p)))
 def test_envelope_retired(self):
  p=dict(spacing=1,punch=1,decay=1,body_env=1)
  self.assertTrue(all(x[0]==0 for x in c.envelope_oracle(np.array([30.]),p)))
 def test_negative_bursts(self):
  p=dict(spacing=1,punch=.5,decay=.5,body_env=.5)
  self.assertEqual(c.envelope_oracle(np.array([0.]),p)[0][0],0)
 def test_gain_native_json(self):
  for dtype in (np.float32,np.float64):
   v=c.safe_gain(np.array([.2,-.3],dtype));self.assertIs(type(v),float);json.dumps(v,allow_nan=False)
 def test_descriptor_gain(self):
  x=np.random.default_rng(1).normal(size=10000)*np.exp(-np.arange(10000)/1000)
  np.testing.assert_allclose(c.features(x,48000),c.features(x*.4,48000),atol=1e-12)
 def test_silent_descriptor(self):
  with self.assertRaises(ValueError):c.features(np.zeros(100),48000)
 def test_ratio_wrap_mutation(self):
  n=np.arange(1000);phase=np.mod(n*.01,1);bad=np.mod(1.7*phase,1)
  self.assertGreater(np.max(abs(np.mod(np.diff(bad),1)-.017)),.1)
if __name__=='__main__':unittest.main()
