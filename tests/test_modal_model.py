"""Tests of model construction, not a substitute for compiled audio tests."""
import sys,pathlib,unittest
import numpy as np
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
from modal_model import make_grid

class ModalModelTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls): cls.grid=make_grid()
 def test_positive_frequencies_and_finite_spatial_modes(self):
  for geometry in self.grid:
   for row in geometry:
    self.assertTrue(np.all(row['bend']>0));self.assertTrue(np.all(row['membrane']>=0))
    self.assertTrue(np.isfinite(row['shape']).all())
 def test_mass_normalisation(self):
  for geometry in self.grid:
   for row in geometry:
    np.testing.assert_allclose(row['q'].T@row['mass']@row['q'],np.eye(len(row['bend'])),atol=3e-6)
 def test_mounting_hole_is_clamped_not_an_edge_impulse(self):
  for geometry in self.grid:
   for row in geometry: self.assertLess(np.max(abs(row['shape'][0])),1e-5)
 def test_bell_geometry_changes_eigenvalues(self):
  a=self.grid[0][0]; b=self.grid[8][0]
  self.assertGreater(np.linalg.norm(a['bend']+a['membrane']-b['bend']-b['membrane']),100)
 def test_bending_and_membrane_size_laws(self):
  for row in self.grid[4]:
   f=np.sqrt(row['bend']+row['membrane'])
   bigger=np.sqrt(row['bend']/16+row['membrane']/4)
   self.assertTrue(np.all(bigger<=f/2+1e-9));self.assertTrue(np.all(bigger>=f/4-1e-9))
 def test_quartic_discrete_gradient_work_identity(self):
  for a,b in [(-.04,.03),(.01,.011),(0,.4),(-.2,-.1)]:
   force=.25*(a+b)*(a*a+b*b)
   self.assertAlmostEqual(force*(b-a),.25*(b**4-a**4),places=12)
 def test_modal_basis_has_spatially_different_excitation(self):
  geometry=self.grid[4]
  bell=np.concatenate([row['shape'][5] for row in geometry])
  rim=np.concatenate([row['shape'][-1] for row in geometry])
  self.assertGreater(np.linalg.norm(bell-rim),1)

if __name__=='__main__':unittest.main()
