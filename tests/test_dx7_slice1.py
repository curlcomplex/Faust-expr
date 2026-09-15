"""Pure adapter/measurement tests; these are not actual Faust/MSFA renders."""
import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/modules'))
import dx7_slice1 as dx


class DX7Slice1Tests(unittest.TestCase):
    def setUp(self):
        self.suite=json.loads((ROOT/'modules/dx7-reference-slice/cases.json').read_text())

    def test_schema_and_family_counts(self):
        dx.validate_suite(self.suite)
        self.assertEqual([sum(c['family']==f for c in self.suite['cases']) for f in ['C01','C02','C03']],[1,1,1])

    def test_reject_bad_cases(self):
        for field,value in [('sample_rate',48000),('gate_on',2049),('frames',-1),('velocity',0),('schema',True)]:
            bad=copy.deepcopy(self.suite); bad[field]=value
            with self.subTest(field=field), self.assertRaises(ValueError): dx.validate_suite(bad)
        for field,value in [('note',True),('carrier_level',float('nan')),('modulator_level',-1),('id','../x'),('family','guess')]:
            bad=copy.deepcopy(self.suite); bad['cases'][0][field]=value
            with self.subTest(field=field), self.assertRaises(ValueError): dx.validate_suite(bad)
        bad=copy.deepcopy(self.suite); bad['cases'].append(bad['cases'][0])
        with self.assertRaises(ValueError): dx.validate_suite(bad)

    def test_patch_fields_and_sysex(self):
        c=self.suite['cases'][1]
        packed,expected,sysex=dx.patch_data(c)
        self.assertEqual(len(packed),128); self.assertEqual(len(expected),156)
        self.assertEqual(packed[5*17+14],80) # operator 1 is last in storage
        self.assertEqual(packed[4*17+14],70) # operator 2 modulates it
        self.assertEqual(packed[4*17+15],4)  # ratio 2, ratio mode
        self.assertEqual(expected[5*21+16],80)
        self.assertEqual(expected[4*21+18],2)
        self.assertEqual(expected[134:137],[0,0,1]) # algo 1, no feedback, sync
        self.assertEqual(expected[144],24)
        self.assertEqual(len(sysex),163)
        self.assertEqual(sysex[:6],bytes([240,67,0,0,1,27]))
        self.assertEqual(sysex[6:161],bytes(expected[:155]))
        self.assertEqual(sum(sysex[6:-1])%128,0)
        self.assertEqual(sysex[-1],247)

    def test_faust_uses_actual_function_order_not_stale_usage_comment(self):
        s=dx.faust_source(self.suite['cases'][1],Path('/pinned'))
        self.assertEqual(s.count('dx.operator('),2)
        self.assertIn('dx7/dx7.lib',s)
        self.assertIn('modulator,freq,velocity,gate',s)
        self.assertNotIn('sin(',s) # no replacement oscillator
        self.assertNotIn('pow(',s)
        c01=dx.faust_source(self.suite['cases'][0],Path('/pinned'))
        self.assertIn('modulator=0;',c01)
        self.assertEqual(c01.count('dx.operator('),1)

    def test_articulation_is_in_both_encodings(self):
        c=self.suite['cases'][-1]
        _,p,_=dx.patch_data(c)
        self.assertEqual(p[105:113],[85,55,45,65,99,80,70,0])
        self.assertIn('85,55,45,65,99,80,70,0',dx.faust_source(c,Path('/pinned')))
        self.assertEqual(p[84:92],[90,60,50,65,99,50,30,0])

    def test_metrics_detect_gain_and_onset_without_alignment(self):
        n=self.suite['frames']; x=np.zeros(n)
        on=self.suite['gate_on']; off=self.suite['gate_off']
        x[on:off]=.2*np.sin(2*np.pi*220*np.arange(off-on)/44100)
        same=dx.compare(x,x,self.suite,'C01')
        self.assertEqual(same['raw_residual_rms'],0)
        gain=dx.compare(x*.5,x,self.suite,'C01')
        self.assertAlmostEqual(gain['steady_faust_over_msfa_db'],-6.020599913,places=6)
        self.assertLess(gain['steady_normalized_magnitude_l2'],1e-12)
        delayed=np.r_[np.zeros(64),x[:-64]]
        delay=dx.compare(delayed,x,self.suite,'C01')
        self.assertEqual(delay['faust']['onset_frame']-delay['msfa_core']['onset_frame'],64)
        self.assertGreater(delay['raw_residual_rms'],.01)
        self.assertAlmostEqual(same['carrier_peak_hz']['faust'],220,places=2)
        with self.assertRaises(ValueError): dx.compare(x[:-1],x,self.suite,'C01')
        x[0]=np.nan
        with self.assertRaises(ValueError): dx.compare(x,x,self.suite,'C01')

    def test_float_wav_preserves_above_full_scale(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'raw.wav'; dx.float_wav(p,np.array([0,2,-2,.25]),44100)
            b=p.read_bytes(); self.assertEqual(b[:4],b'RIFF')
            self.assertEqual(struct.unpack('<H',b[20:22])[0],3)
            pos=b.index(b'data')+8
            np.testing.assert_array_equal(np.frombuffer(b[pos:],dtype='<f4'),[0,2,-2,.25])


if __name__=='__main__': unittest.main()
