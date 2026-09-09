"""Synthetic tests of color-study diagnostics, not DSP or hardware validation."""
import importlib.util, pathlib, unittest, numpy as np
spec=importlib.util.spec_from_file_location('fit_color',pathlib.Path(__file__).resolve().parents[1]/'tools/modules/fit_color.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Diagnostics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rate=44100;cls.t=np.arange(44100)/44100
        cls.x=np.sin(2*np.pi*65.4*cls.t)*np.exp(-cls.t/.2)
        cls.obj=m.Objective(cls.x,cls.rate,(.22,.36))
    def test_identical(self):
        r=self.obj.metrics(self.x)
        self.assertLess(r['spectral_rmse_db'],1e-10);self.assertLess(r['envelope_rmse_db'],1e-10)
        self.assertLess(abs(r['pitch_error_cents']),1e-6)
    def test_level_is_recorded(self):
        r=self.obj.metrics(self.x*.25)
        self.assertAlmostEqual(r['rms_gain'],4)
        self.assertLess(r['spectral_rmse_db'],1e-10)
        self.assertAlmostEqual(r['raw_rms'],np.sqrt(np.mean(self.x**2))*.25)
    def test_wrong_pitch_rejected(self):
        y=np.sin(2*np.pi*70*self.t)*np.exp(-self.t/.2)
        self.assertFalse(self.obj.metrics(y)['pitch_acceptable'])
    def test_wrong_decay_detected(self):
        y=np.sin(2*np.pi*65.4*self.t)*np.exp(-self.t/.45)
        r=self.obj.metrics(y)
        self.assertGreater(r['envelope_rmse_db'],3)
        self.assertGreater(r['energy_quantiles']['0.9']['error_s'],.1)
    def test_nonfinite_and_silence_fail(self):
        for v in [np.zeros_like(self.x),np.full_like(self.x,np.nan)]:
            with self.assertRaises(ValueError):self.obj.metrics(v)
    def test_boundaries_are_encoded(self):
        names,bounds=m.transformed_bounds(44100,44100,False)
        v=np.mean(bounds,axis=1)
        p,off=m.decode(v,names,1,65.4)
        self.assertNotIn('gate_off_s',p);self.assertGreater(off,0)
        self.assertEqual(p['frequency_hz'],65.4);self.assertEqual(p['square'],0)
        self.assertEqual(p['drive_after_body'],1)
if __name__=='__main__':unittest.main()
