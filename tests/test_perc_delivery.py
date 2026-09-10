"""Synthetic tests of measurements and contracts; not instrument-fidelity tests."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
import numpy as np
from scipy.io import wavfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/modules'))
import perc_delivery as p

class PercDeliveryTests(unittest.TestCase):
    def setUp(self):
        self.man=json.loads((ROOT/'modules/perc-pm/manifest.json').read_text())
        self.defaults={k:v['default'] for k,v in self.man['controls'].items()}
    def test_eight_controls(self):
        self.assertEqual(len(self.man['musical_control_order']),8)
    def test_bad_control_values(self):
        for value in (-.1,1.1,np.nan,np.inf,True):
            with self.assertRaises(ValueError):p.validate({'drive':value},self.man)
    def test_bad_name(self):
        with self.assertRaises(ValueError):p.validate({'imaginary':1},self.man)
    def test_gate_binary(self):
        with self.assertRaises(ValueError):p.validate({'gate':.5},self.man)
    def test_duplicate_events(self):
        with self.assertRaises(ValueError):p.events_to_rows(self.defaults,{},[(4,'drive',0),(4,'drive',1)],100,self.man)
    def test_bad_frames(self):
        for value in (-1,100,True,1.5):
            with self.assertRaises(ValueError):p.events_to_rows(self.defaults,{},[(value,'drive',0)],100,self.man)
    def test_zero_frame_override(self):
        rows=p.events_to_rows(self.defaults,{},[(0,'drive',.5)],100,self.man)
        self.assertEqual([r for r in rows if r[:2]==(0,'drive')],[(0,'drive',.5)])
    def test_silent_descriptor_rejected(self):
        with self.assertRaises(ValueError):p.descriptor(np.zeros(4096))
    def test_nonfinite_audio_rejected(self):
        with self.assertRaises(ValueError):p.descriptor(np.array([np.nan,1]))
    def test_gain_invariant_descriptor(self):
        t=np.arange(96000)/48000;x=np.sin(2*np.pi*220*t)*np.exp(-t/.5)
        np.testing.assert_allclose(p.descriptor(x),p.descriptor(x*.1),atol=1e-10)
    def test_late_tail_is_not_discarded(self):
        t=np.arange(480000)/48000;x=np.sin(2*np.pi*220*t)*np.exp(-t/3);y=x.copy();y[48000:]=0
        self.assertGreater(np.linalg.norm(p.descriptor(x)-p.descriptor(y)),.5)
    def test_int16_decode_preserves_scale(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'a.wav';wavfile.write(path,48000,np.array([0,16384,-16384],np.int16))
            rate,x=p.decode_audio(path);self.assertEqual(rate,48000);np.testing.assert_array_equal(x,[0,.5,-.5])
    def test_envelope_onset(self):
        y=p.envelope_oracle(48000,4096,123,.5,.5)
        self.assertFalse(np.any(y[:124]));self.assertGreater(y[124],0)
    def test_wav_writer_rejects_clipping(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(ValueError):p.base.wav(Path(d)/'a.wav',np.array([1.2]))

if __name__=='__main__':unittest.main()
