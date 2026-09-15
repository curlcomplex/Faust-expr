"""#130: actual faustprobe/Cranelift execution against the #108 reducer."""
import importlib.util, json, os
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('hq',ROOT/'tools/modules/harmonic_qualification.py')
hq=importlib.util.module_from_spec(spec);spec.loader.exec_module(hq)

@unittest.skipUnless(os.environ.get('FAUSTPROBE_INTEGRATION')=='1','requires pinned faustprobe integration environment')
class FaustprobeParity(unittest.TestCase):
 def test_actual_cranelift_cases(self):
  out=Path(os.environ['FAUSTPROBE_EVIDENCE'])
  report=hq.sweep(out,faust=os.environ['FAUST'],libs=os.environ['FAUST_LIBRARIES'],archive=os.environ['FAUST_ARCHIVE'],rates=(48000,),faustprobe=os.environ['FAUSTPROBE'],revision=os.environ['FAUSTPROBE_REVISION'])
  fp=report['optional_faustprobe']
  self.assertEqual(fp['status'],'executed'); self.assertEqual(len(fp['cases']),3)
  for case in fp['cases']:
   self.assertEqual(case['backend'],'faust-rs/Cranelift')
   self.assertEqual(case['revision_declared_by_caller'],os.environ['FAUSTPROBE_REVISION'])
   self.assertTrue(case['binary_sha256']); self.assertTrue(case['csv_sha256'])
   # Same reducer should give closely matching scalar results for these deterministic fixtures.
   for key,delta in case['metric_deltas_db'].items():
    if delta is not None: self.assertLess(abs(delta),0.05,(case['native_case_label'],key,delta))
  (out/'faustprobe-parity-summary.json').write_text(json.dumps({'cases':[{'label':c['native_case_label'],'binary_sha256':c['binary_sha256'],'csv_sha256':c['csv_sha256'],'metric_deltas_db':c['metric_deltas_db']} for c in fp['cases']]},indent=2)+'\n')

if __name__=='__main__': unittest.main()
