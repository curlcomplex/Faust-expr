import unittest,numpy as np
from validation import compare,spec
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  t=np.arange(48000)/48000;cls.x=(.1*np.cos(2*np.pi*1333*t)*np.exp(-t*5))[:,None]
 def test_identity(self):
  a=compare(self.x,self.x);self.assertEqual(a['envelope_mae_db'],0);self.assertTrue(all(a['engineering_gates'].values()))
 def test_first_sample(self):
  c=self.x.copy();c[0]=.5
  for n in [256,1024,4096,16384]:
   _,_,a=spec(self.x,n);_,_,b=spec(c,n);self.assertGreater(float(np.max(abs(b-a))),1e-9)
 def test_gain_not_removed(self):self.assertAlmostEqual(compare(self.x,self.x*2)['rms_ratio_db'],6.020599913,places=6)
 def test_no_stereo_cancellation(self):
  x=np.concatenate([self.x,-self.x],axis=1);_,_,p=spec(x,1024);self.assertGreater(p.max(),1e-4)
 def test_decay_regression_fails(self):
  t=np.arange(48000)/48000;y=self.x*np.exp(t[:,None]*3);self.assertFalse(compare(self.x,y)['engineering_gates']['t90_within_15pct'])
 def test_pitch_shift_fails(self):
  t=np.arange(48000)/48000;y=(.1*np.cos(2*np.pi*1533*t)*np.exp(-t*5))[:,None]
  self.assertFalse(compare(self.x,y)['engineering_gates']['multires_active_error_under_6dB'])
 def test_truncation_fails(self):
  c=self.x.copy();c[6000:]=0;self.assertFalse(compare(self.x,c)['engineering_gates']['envelope_within_3dB'])
 def test_nonfinite(self):
  c=self.x.copy();c[0]=np.nan
  with self.assertRaises(ValueError):compare(self.x,c)
 def test_clipping(self):self.assertFalse(compare(self.x,self.x*12)['engineering_gates']['no_clipping'])
 def test_unrelated_noise_fails(self):
  y=np.random.default_rng(1).normal(size=self.x.shape)*.05*np.exp(-np.arange(48000)[:,None]/9600)
  self.assertFalse(compare(self.x,y)['engineering_gates']['multires_active_error_under_6dB'])
if __name__=='__main__':unittest.main()
