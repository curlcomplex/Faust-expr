"""Independent arithmetic/schema fixtures, not actual-Faust audio tests."""
import importlib.util,json,sys,tempfile,unittest
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools/modules'))
import morph_v2_batch as lab
import generate_morph_bank as bank
class MorphTests(unittest.TestCase):
 def setUp(self):self.man=json.loads((ROOT/'modules/morph-wavetable/v2/manifest.json').read_text());self.c=self.man['controls']
 def test_six_controls(self):self.assertEqual(len(self.man['musical_control_order']),6);self.assertEqual(self.man['musical_control_order'][2],'decay')
 def test_unknown(self):
  with self.assertRaises(ValueError):lab.validate({'invented':0},self.c)
 def test_nonfinite(self):
  for x in (float('nan'),float('inf'),-1,2):
   with self.assertRaises(ValueError):lab.validate({'morph':x},self.c)
 def test_integer_stack(self):
  with self.assertRaises(ValueError):lab.validate({'stack':1.5},self.c)
 def test_duplicate(self):
  with self.assertRaises(ValueError):lab.make_events({}, {},[(1,'gate',1),(1,'gate',0)],10,self.c)
 def test_invalid_sample(self):
  for n in (-1,10,.5,True):
   with self.assertRaises(ValueError):lab.make_events({}, {},[(n,'gate',1)],10,self.c)
 def test_valid_same_sample(self):self.assertEqual(len(lab.make_events({}, {},[(1,'gate',1),(1,'morph',.5)],10,self.c)),2)
 def test_unison_symmetry(self):
  for n in range(1,5):
   offsets=(2*np.arange(n)/max(1,n-1)-1)*(n>1);self.assertAlmostEqual(float(offsets.sum()),0,places=12)
 def test_interpolation_partition(self):
  for x in np.linspace(0,7,101):
   u=np.maximum(0,1-np.abs(x-np.arange(8)));self.assertAlmostEqual(float(np.sum(u*u*(3-2*u))),1,places=12)
 def test_bank_cycle_zero_mean(self):
  clean,driven,cycles=bank.generate();self.assertLess(float(np.abs(cycles.mean(axis=1)).max()),1e-12);self.assertTrue(np.isfinite(cycles).all())
 def test_frame_peak_calibration(self):
  _,_,cycles=bank.generate();self.assertLess(float(np.abs(np.max(abs(cycles),axis=1)-.75).max()),1e-12)
 def test_bank_reproducibility_same_environment(self):
  with tempfile.TemporaryDirectory() as d:
   a=bank.write(Path(d)/'a');b=bank.write(Path(d)/'b');self.assertEqual(a['bank_sha256'],b['bank_sha256']);self.assertEqual(a['cycles_sha256'],b['cycles_sha256'])
 def test_wav_rejects_clipping(self):
  with tempfile.TemporaryDirectory() as d:
   with self.assertRaises(ValueError):lab.wav(Path(d)/'x.wav',np.array([1.1]))
if __name__=='__main__':unittest.main()
