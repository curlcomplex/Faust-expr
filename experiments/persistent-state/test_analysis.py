import unittest,tempfile
from pathlib import Path
import generate,analyze
class Tests(unittest.TestCase):
    def test_error(self):
        with self.assertRaises(ValueError):analyze.compare([.1],[.2])
    def test_length(self):
        with self.assertRaises(ValueError):analyze.compare([0],[0,0])
    def test_stats(self):self.assertEqual(analyze.stats([1,2,3])['median'],2)
    def test_nan(self):
        with self.assertRaises(ValueError):analyze.stats([float('nan')])
    def test_no_magic_state_field_mapping(self):
        source=Path(generate.__file__).read_text()
        for name in ['fRec','memcpy','fHslider']:self.assertNotIn(name,source)
    def test_fingerprint(self):self.assertNotEqual(generate.sha('A'),generate.sha('B'))
    def test_full_two_nodes(self):
        m=[dict(index=0,inputs=0,compiled_source='process=0.1,0.2;'),dict(index=1,inputs=2,compiled_source='process=_,_;')]
        g=dict(order=[0,1],edges=[dict(source=0,target=1,delay=False,key=1,gains=[1,1])]);s=generate.full_program(m,g)
        self.assertIn('in1 : e1.process',s);self.assertIn('v0=e0.process',s);self.assertEqual(s.count('environment{'),2)
    def test_schema_compiler_identity(self):
        s=Path(generate.__file__).read_text();self.assertIn('compiler',s);self.assertIn('header',s)
if __name__=='__main__':unittest.main(verbosity=2)
