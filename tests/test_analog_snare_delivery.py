"""Synthetic arithmetic / boundary tests, separate from actual DSP tests."""
import sys,json,unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
import analog_snare_delivery as a
class TestAnalogSnare(unittest.TestCase):
 def setUp(self):self.m=json.loads((a.MOD/'manifest.json').read_text())
 def test_six_columns(self):self.assertEqual(len(self.m['visible_columns']),6);self.assertEqual(self.m['visible_columns'][2],'decay')
 def test_presets_valid(self):
  for p in json.loads((a.MOD/'patches.json').read_text())['anchors'].values():a.validate(p,self.m)
 def test_unknown(self):
  with self.assertRaises(ValueError):a.validate({'oops':0},self.m)
 def test_nonfinite(self):
  for x in (float('nan'),float('inf')):
   with self.assertRaises(ValueError):a.validate({'decay':x},self.m)
 def test_out_of_range(self):
  with self.assertRaises(ValueError):a.validate({'pitch_hz':999},self.m)
 def test_gate_binary(self):
  with self.assertRaises(ValueError):a.validate({'gate':.5},self.m)
 def test_envelope_start(self):np.testing.assert_array_equal(a.envelopes(np.array([0.]),.5,.5,.5),[[0.,0.]])
 def test_envelope_tail(self):self.assertTrue(np.all(a.envelopes(np.array([30.]),1,1,1)==0))
 def test_longer_decay(self):self.assertTrue(np.all(a.envelopes(np.array([.2]),1,.5,.5)>a.envelopes(np.array([.2]),0,.5,.5)))
 def test_fullband_pitch(self):
  r=48000;x=np.sin(2*np.pi*183*np.arange(r)/r);self.assertLess(abs(a.pitch_estimate(x,r)-183),.1)
 def test_descriptor_gain(self):
  x=np.random.default_rng(1).normal(size=48000)*np.exp(-np.arange(48000)/4000);np.testing.assert_allclose(a.descriptors(x,48000),a.descriptors(x*.4,48000),atol=1e-9)
 def test_silence_rejected(self):
  with self.assertRaises(ValueError):a.descriptors(np.zeros(100),48000)
 def test_gain_native_float(self):
  for dtype in (np.float32,np.float64):
   x=np.array([.8,-.3,.1],dtype=dtype);g=a.phrase_gain(x)
   self.assertIs(type(g),float);json.dumps({'gain':g},allow_nan=False)
 def test_gain_does_not_clip(self):
  x=np.zeros(1000,np.float32);x[0]=.8;g=a.phrase_gain(x);self.assertLessEqual(float(abs(x*g).max()),.9000001)
 def test_tail_zero_padding(self):
  x=np.sin(2*np.pi*.1*np.arange(4096))*np.exp(-np.arange(4096)/800);d=a.descriptors(x,48000);z=a.descriptors(np.r_[x,np.zeros(48000)],48000);np.testing.assert_allclose(d[:2],z[:2]);self.assertLess(abs(d[2]-z[2]),.02)
if __name__=='__main__':unittest.main()
