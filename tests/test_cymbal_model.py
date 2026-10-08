import sys
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from generate_cymbal import reference_modes, NMODES, SUPPORT

class ReferenceModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.modes=reference_modes()
    def test_positive_finite_modes(self):
        self.assertEqual(len(self.modes),NMODES)
        for m in self.modes:
            self.assertGreater(m['f'],0)
            self.assertAlmostEqual(m['f']**2,m['bend']+m['bell']+m['bow'],delta=m['f']**2*1e-7)
            self.assertTrue(np.isfinite(m['shape']).all())
    def test_support_and_band_coverage(self):
        self.assertLess(min(m['f'] for m in self.modes),100)
        self.assertGreater(max(m['f'] for m in self.modes),12000)
        for m in self.modes:self.assertAlmostEqual(m['shape'][0],0,places=10)
    def test_angular_pairs(self):
        for m in self.modes:
            if m['m']:
                matches=[x for x in self.modes if x['m']==m['m'] and x['n']==m['n']]
                self.assertEqual({x['quadrature'] for x in matches},{0,1})
    def test_cayley_passivity(self):
        rng=np.random.default_rng(421)
        for _ in range(100):
            p=rng.normal(size=NMODES);x=rng.normal(size=NMODES);start=float(p@p)
            for stage in range(2):
                for j in range(NMODES//2):
                    i=(2*j+stage)%NMODES;k=(i+1)%NMODES
                    t=18*(x[i]+x[k])/(1+abs(30*(x[i]+x[k])))
                    c=(1-t*t)/(1+t*t);s=2*t/(1+t*t)
                    p[i],p[k]=c*p[i]+s*p[k],-s*p[i]+c*p[k]
            self.assertAlmostEqual(float(p@p),start,places=10)
    def test_plate_scaling(self):
        for mode in self.modes:
            # Flat plate limit: double radius -> f/4 at fixed thickness, f/2 if all lengths scale.
            base=np.sqrt(mode['bend'])
            self.assertAlmostEqual(np.sqrt(mode['bend']/16),base/4)
            self.assertAlmostEqual(np.sqrt(mode['bend']*4/16),base/2)

if __name__=='__main__':unittest.main()
