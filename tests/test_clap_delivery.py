import json,sys,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];M=ROOT/'modules/clap/v1';sys.path.insert(0,str(ROOT/'tools/modules'))
import clap_delivery as c
class T(unittest.TestCase):
 def setUp(self):self.m=json.loads((M/'manifest.json').read_text())
 def test_identity(self):self.assertEqual(self.m['module_id'],'clap');self.assertEqual(len(self.m['musical_control_order']),8)
 def test_seven_nonpitch(self):self.assertEqual(self.m['musical_control_order'][1:],['spacing','punch','decay','balance','body','body_env','drive'])
 def test_decay_column3(self):self.assertEqual(self.m['tracker_columns'][2],'decay')
 def test_pair(self):self.assertEqual(self.m['tracker_columns'][4],'body/body_env')
 def test_refs(self):self.assertEqual(len(self.m['references']),2)
 def test_patches(self):
  p=json.loads((M/'patches.json').read_text())['anchors'];self.assertEqual(len(p),8)
  for x in p.values():
   for k,v in x.items():self.assertTrue(self.m['controls'][k]['min']<=v<=self.m['controls'][k]['max'])
 def test_descriptor_gain_invariant(self):
  import numpy as np
  x=np.random.default_rng(1).normal(size=48000)*np.exp(-np.arange(48000)/4000);a=np.array(c.descriptor(x));b=np.array(c.descriptor(x*.3));np.testing.assert_allclose(a,b,rtol=0,atol=1e-8)
if __name__=='__main__':unittest.main()
