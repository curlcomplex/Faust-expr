"""Test-runner guardrails; these fixtures are not Faust audio tests."""
from pathlib import Path
import importlib.util
import json
import sys
import tempfile
import unittest

TOOLS=Path(__file__).resolve().parents[1]/'tools/modules'
sys.path.insert(0,str(TOOLS))
spec=importlib.util.spec_from_file_location('synth_reference_checkpoint',TOOLS/'synth_reference_pass.py')
checkpoint=importlib.util.module_from_spec(spec)
spec.loader.exec_module(checkpoint)

class SynthReferenceGuardTests(unittest.TestCase):
 def test_failed_check_is_nonzero_and_retained(self):
  with tempfile.TemporaryDirectory() as out:
   report={'checks':[{'name':'intentional-negative','passed':False}]}
   self.assertEqual(checkpoint.finalize_report(report,out),1)
   saved=json.loads((Path(out)/'results.json').read_text())
   self.assertFalse(saved['passed'])
   self.assertEqual(saved['failures'],['intentional-negative'])

 def test_empty_checks_cannot_pass(self):
  with tempfile.TemporaryDirectory() as out:
   self.assertEqual(checkpoint.finalize_report({'checks':[]},out),1)

 def test_success_does_not_approve_hardware(self):
  with tempfile.TemporaryDirectory() as out:
   report={'checks':[{'name':'equation','passed':True}],
           'hardware_approved':False,'selected_for_promotion':False}
   self.assertEqual(checkpoint.finalize_report(report,out),0)
   self.assertFalse(report['hardware_approved'])
   self.assertFalse(report['selected_for_promotion'])

 def test_prediction_has_no_zero_resonance_change(self):
  self.assertEqual(checkpoint.gain_prediction(0),1)
  self.assertAlmostEqual(checkpoint.gain_prediction(.3),1/1.231)
