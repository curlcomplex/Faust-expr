"""Metadata portability guards, not DSP renders."""
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
from faust_export_metadata import normalize_expanded
class ExportMetadata(unittest.TestCase):
    def setUp(self):self.original='declare name "Test";\ndeclare version "0.3.0";\nprocess=_;\n'
    def test_invalid_import_path_key(self):
        result=normalize_expanded('declare foo-bar_author "Person";\nprocess=_;\n',self.original)
        self.assertIn('declare foo_bar_author "Person";',result);self.assertTrue(result.endswith('process=_;\n'))
    def test_root_identity(self):
        result=normalize_expanded('declare version "2.70.3";\nprocess=_;\n',self.original)
        self.assertIn('declare version "0.3.0";',result);self.assertNotIn('2.70.3',result)
    def test_attribution_preserved(self):
        value='Copyright A-B, LGPL with exception'
        self.assertIn(value,normalize_expanded(f'declare lib_license "{value}";\nprocess=_;',self.original))
    def test_volatile_paths_removed(self):
        self.assertNotIn('/tmp/random',normalize_expanded('declare library_path0 "/tmp/random";\nprocess=_;',self.original))
    def test_collision_rejected(self):
        with self.assertRaises(ValueError):normalize_expanded('declare a-b "x";\ndeclare a_b "y";\nprocess=_;',self.original)
    def test_missing_identity_rejected(self):
        with self.assertRaises(ValueError):normalize_expanded('process=_;','process=_;')
if __name__=='__main__':unittest.main()
