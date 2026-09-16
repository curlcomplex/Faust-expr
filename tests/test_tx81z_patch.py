import importlib.util
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('tx81z_patch',ROOT/'tools/modules/tx81z_patch.py')
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class TX81ZPatchTests(unittest.TestCase):
    def test_vced_aced_maps_panel_fields_to_working_voice(self):
        v=bytearray(93); a=bytearray(23); v[77:87]=b'TESTVOICE '
        v[52]=7; v[53]=6; v[54:62]=bytes([91,42,73,66,1,3,5,2])
        b=39; v[b:b+13]=bytes([31,22,13,9,7,44,3,5,1,6,88,37,4]); a[15:20]=bytes([1,6,12,7,0])
        b=13; v[b:b+13]=bytes([30,21,12,8,6,43,2,4,1,5,77,31,3]); a[5:10]=bytes([0,4,9,6,3])
        p=m.decode(bytes(v),bytes(a)); c,u=m.to_controls(p)
        self.assertEqual(p['name'],'TESTVOICE')
        self.assertEqual((c['algorithm'],c['feedback']),(8,6))
        self.assertEqual((c['lfoSpeed'],c['lfoWave'],c['pModSens'],c['aModSens']),(91,3,5,2))
        self.assertEqual((c['op1AR'],c['op1Mode'],c['op1Range'],c['op1Fine'],c['op1Wave'],c['op1FixedCRS']),(31,1,6,12,7,37))
        self.assertEqual((c['op1Coarse'],c['op1DT2'],c['op1DT1']),(7,3,1))
        self.assertEqual((c['op1TL'],c['op1KVS'],c['op1LS'],c['op1EBS']),(11,6,44,5))
        self.assertEqual((c['op2Coarse'],c['op2DT2'],c['op2DT1']),(10,0,0))
        self.assertEqual((c['op2LS'],c['op2EBS']),(43,4))
        self.assertNotIn('op1EGShift',c); self.assertEqual(c['op2EGShift'],3)
        self.assertNotIn('op1',u)
        self.assertIn('eg_bias_controller',u['voice'])

    def test_all_ratio_coarse_values_are_bijective(self):
        self.assertEqual(len(m.RATIO_MAP),64)
        self.assertEqual(m.RATIO_MAP[0],(0,0)); self.assertEqual(m.RATIO_MAP[1],(0,1))
        self.assertEqual(m.RATIO_MAP[2],(0,2)); self.assertEqual(m.RATIO_MAP[3],(0,3))
        self.assertEqual(m.RATIO_MAP[63],(15,3))

    def test_detune_centre_and_sign_encoding(self):
        self.assertEqual([m._dt1(x) for x in range(7)],[7,6,5,0,1,2,3])

    def test_rejects_wrong_sizes(self):
        with self.assertRaises(ValueError): m.decode(bytes(92),bytes(23))
if __name__=='__main__': unittest.main()
