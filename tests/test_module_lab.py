"""Synthetic analyzer and boundary fixtures, not a Faust render."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('module_lab',ROOT/'tools/modules/lab.py')
lab=importlib.util.module_from_spec(spec); spec.loader.exec_module(lab)

class ModuleLabTests(unittest.TestCase):
    def setUp(self):
        self.manifest=json.loads((ROOT/'modules/kick-pm/manifest.json').read_text())
        self.rate=48000
        self.sine=np.sin(2*np.pi*40*np.arange(self.rate)/self.rate)*.5

    def test_subbass_is_included(self):
        self.assertGreater(lab.metrics(self.sine,self.rate)['energy_below_80_fraction'],.99)

    def test_gain_is_not_normalized_away(self):
        a=lab.metrics(self.sine,self.rate); b=lab.metrics(self.sine*.5,self.rate)
        self.assertAlmostEqual(b['rms']/a['rms'],.5)
        self.assertAlmostEqual(b['peak']/a['peak'],.5)

    def test_silence_is_explicit(self):
        self.assertIsNone(lab.metrics(np.zeros(100),self.rate)['onset_frame'])

    def test_nonfinite_audio_rejected(self):
        with self.assertRaises(ValueError): lab.metrics(np.array([np.nan]),self.rate)

    def test_unknown_control_rejected(self):
        with self.assertRaises(ValueError): lab.validate_parameters(self.manifest,{'made_up':1})

    def test_nonfinite_and_range_rejected(self):
        for x in (np.nan,np.inf,-.1,1.1):
            with self.assertRaises(ValueError): lab.validate_parameters(self.manifest,{'drive':x})

    def test_boolean_is_categorical(self):
        with self.assertRaises(ValueError): lab.validate_parameters(self.manifest,{'punch':.5})
        lab.validate_parameters(self.manifest,{'punch':1})

    def test_fullband_clean_sine_frequency(self):
        self.assertLess(abs(lab.fundamental(self.sine,self.rate)-40),.1)

    def test_multires_identity(self):
        for v in lab.multires_distance(self.sine,self.sine).values():
            self.assertEqual(v['log_magnitude_rmse_db'],0)

    def test_no_clipping_in_wav_writer(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(ValueError): lab.wav(Path(d)/'bad.wav',np.array([1.2]),48000)

    def test_reference_not_falsely_ready(self):
        r=json.loads((ROOT/'modules/kick-pm/references.json').read_text())
        self.assertFalse(r['calibration_ready']); self.assertEqual(r['captures'],[])

if __name__=='__main__': unittest.main()
