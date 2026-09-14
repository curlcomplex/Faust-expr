from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
from synth_selected_qualification import report_passed
class SelectedReports(unittest.TestCase):
    def test_missing_report_fails(self):self.assertFalse(report_passed({}))
    def test_empty_checks_fail(self):self.assertFalse(report_passed({'passed':True,'checks':[]}))
    def test_failed_check_fails(self):self.assertFalse(report_passed({'passed':True,'checks':[{'passed':False}]}))
    def test_failed_report_fails(self):self.assertFalse(report_passed({'passed':False,'checks':[{'passed':True}]}))
    def test_explicit_success(self):self.assertTrue(report_passed({'passed':True,'checks':[{'passed':True}]}))
if __name__=='__main__':unittest.main()
