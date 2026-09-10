"""Arithmetic/schema tests. Actual Faust audio is checked by the batch runner."""
from pathlib import Path
import unittest,sys
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
from generate_morph_tables import BAND_HZ,build_arrays
class TableTests(unittest.TestCase):
 def test_sorted_unique_knots(self):
  self.assertEqual(len(BAND_HZ),124);self.assertTrue(np.all(np.diff(BAND_HZ)>0))
 def test_all_taper_kinks_present(self):
  for n in range(1,65):
   for c in (16000,19000):
    if 20<c/n<9000:self.assertIn(c/n,BAND_HZ)
 def test_linear_band_taper_exact(self):
  n=np.arange(1,65)
  for a,b in zip(BAND_HZ[:-1],BAND_HZ[1:]):
   for u in (.01,.27,.51,.93):
    f=a+(b-a)*u;expected=np.clip((19000-n*f)/3000,0,1);actual=(1-u)*np.clip((19000-n*a)/3000,0,1)+u*np.clip((19000-n*b)/3000,0,1)
    np.testing.assert_allclose(expected,actual,atol=1e-13,rtol=0)
 def test_binary_search_has_valid_bounds(self):
  for f in list(BAND_HZ)+list(np.linspace(20,9000,4097)):
   k=0
   for step in (64,32,16,8,4,2,1):
    if k+step<len(BAND_HZ) and f>=BAND_HZ[k+step]:k+=step
   k=min(len(BAND_HZ)-2,k);self.assertLessEqual(BAND_HZ[k],f);self.assertGreaterEqual(BAND_HZ[k+1],f)
 def test_lookup_index_two_corrections(self):
  f=np.array(BAND_HZ);index=np.clip(np.searchsorted(f,np.arange(9001),side='right')-1,0,len(f)-2)
  self.assertLessEqual(np.bincount(f.astype(int)).max(),2)
  probes=np.r_[np.linspace(20,9000,10001),f,f+1e-7,f-1e-7];probes=np.clip(probes,20,9000)
  for hz in probes:
   k=int(index[int(hz)])
   for _ in range(2):k+=int(k+1<len(f)-1 and hz>=f[k+1])
   self.assertLessEqual(f[k],hz);self.assertGreaterEqual(f[k+1],hz)
 def test_sine_deduplicates_and_is_periodic(self):
  c=np.zeros((8,64));c[:,0]=.75;samples,offsets,lengths=build_arrays(c,c)
  self.assertEqual(len(set(offsets)),1);self.assertEqual(len(samples),65);self.assertTrue(np.all(lengths==128));self.assertAlmostEqual(float(samples[32]),.75);self.assertAlmostEqual(float(-samples[128-96]),-.75)
 def test_cubic_wrap_matches_sine(self):
  n=128;data=.75*np.sin(2*np.pi*np.arange(n)/n);p=np.array([0.,1e-6,.999999,.15,.75]);x=p*n;i=x.astype(int);u=x-i
  a,b,c,d=[data[(i+j)%n] for j in (-1,0,1,2)];y=b+.5*u*(c-a+u*(2*a-5*b+4*c-d+u*(3*(b-c)+d-a)))
  np.testing.assert_allclose(y,.75*np.sin(2*np.pi*p),atol=2e-6,rtol=0)
 def test_half_storage_reflection_for_mixed_harmonics(self):
  n=256;p=np.arange(n)/n;full=.5*np.sin(2*np.pi*p)+.2*np.sin(4*np.pi*p)+.1*np.sin(14*np.pi*p);half=full[:n//2+1]
  i=np.arange(n);rebuild=half[np.minimum(i,n-i)]*(1-2*(i>n//2));np.testing.assert_allclose(full,rebuild,atol=2e-15,rtol=0)
 def test_no_dedicated_chord_or_host_dependency(self):
  s=(Path(__file__).resolve().parents[1]/'modules/morph-wavetable/v3/engine.lib').read_text();self.assertNotIn('soundfile(',s);self.assertIn('readEndpoint',s);self.assertIn('rdtable(bandIndex',s);self.assertIn('voices=max(1,int(latch(stackCtl)))',s)
if __name__=='__main__':unittest.main()
