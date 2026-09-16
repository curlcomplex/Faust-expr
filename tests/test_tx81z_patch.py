import importlib.util
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('tx81z_patch',ROOT/'tools/modules/tx81z_patch.py')
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class TX81ZPatchTests(unittest.TestCase):
    def test_vced_aced_direct_mapping_and_unresolved_policy(self):
        v=bytearray(93); a=bytearray(23); v[77:87]=b'TESTVOICE '
        v[52]=7; v[53]=6; v[54:62]=bytes([91,42,73,66,1,3,5,2])
        b=39; v[b:b+13]=bytes([31,22,13,9,7,44,3,5,1,6,88,37,4]); a[15:20]=bytes([1,6,12,7,0])
        b=13; v[b:b+13]=bytes([30,21,12,8,6,43,2,4,1,5,77,31,3]); a[5:10]=bytes([0,4,9,6,3])
        p=m.decode(bytes(v),bytes(a)); c,u=m.to_controls(p)
        self.assertEqual(p['name'],'TESTVOICE')
        self.assertEqual((c['algorithm'],c['feedback']),(8,6))
        self.assertEqual((c['lfoSpeed'],c['lfoWave'],c['pModSens'],c['aModSens']),(91,3,5,2))
        self.assertEqual((c['op1AR'],c['op1Mode'],c['op1Range'],c['op1Fine'],c['op1Wave'],c['op1FixedCRS']),(31,1,6,12,7,37))
        self.assertNotIn('op1EGShift',c); self.assertEqual(c['op2EGShift'],3)
        self.assertEqual(u['op1']['output_level'],88); self.assertEqual(u['op1']['ratio_coarse'],37); self.assertEqual(u['op1']['key_velocity_sensitivity'],6)
    def test_rejects_wrong_sizes(self):
        with self.assertRaises(ValueError): m.decode(bytes(92),bytes(23))
if __name__=='__main__': unittest.main()
