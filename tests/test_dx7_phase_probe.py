"""Pure contracts for the bounded DX7 phase/index probe; no render claims."""
from pathlib import Path
import sys, unittest
import numpy as np
ROOT=Path(__file__).resolve().parents[1]; sys.path.insert(0,str(ROOT/'tools/modules'))
import dx7_phase_probe as p

class PhaseProbeTests(unittest.TestCase):
    def test_ablation_changes_only_carrier_phase_connection(self):
        c={'id':'DX7-P70','family':'C02','note':57,'carrier_level':80,'modulator_level':70}
        stock=p.source_for(c,1.0); doubled=p.source_for(c,2.0)
        self.assertNotIn('phase_mod=',stock)
        self.assertIn('phase_mod=modulator*2;',doubled)
        self.assertIn('phase_mod,freq,velocity,gate);',doubled)
        self.assertEqual(stock.count('dx.operator('),2); self.assertEqual(doubled.count('dx.operator('),2)
        self.assertEqual(doubled.count('phase_mod'),2)
    def test_spectral_shape_is_gain_invariant(self):
        n=16384; t=np.arange(n)/44100; x=np.sin(2*np.pi*220*t)+.2*np.sin(2*np.pi*660*t)
        a=p.spectral_shape(x,0,n); b=p.spectral_shape(x*.25,0,n)
        self.assertLess(p.shape_distance(a,b),1e-12)
        c=p.spectral_shape(np.sin(2*np.pi*220*t)+.5*np.sin(2*np.pi*660*t),0,n)
        self.assertGreater(p.shape_distance(a,c),.1)
    def test_probe_grid_stays_small(self):
        self.assertEqual(p.LEVELS,(40,55,70,85,95)); self.assertEqual(p.SCALES,(0.5,1.0,2.0))
        self.assertEqual(len(p.LEVELS)*len(p.SCALES),15)
if __name__=='__main__': unittest.main()
