"""Prevent large physical constants silently overflowing Faust int32 literals."""
import pathlib,re,unittest
TOKEN=re.compile(r"(?<![A-Za-z0-9_.])([0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?)(?![A-Za-z0-9_.])")
def oversized(source):
    return [v for v in TOKEN.findall(source) if not any(c in v.lower() for c in '.e') and int(v)>2147483647]
class FaustLiteralTests(unittest.TestCase):
    def test_catches_truncated_young_modulus(self):
        self.assertEqual(oversized('kappa=bloom*100000000000*er;'),['100000000000'])
    def test_allows_explicit_floating_point_constant(self):
        self.assertEqual(oversized('kappa=bloom*1.0e11*er;'),[])
    def test_instrument_constants_are_safe(self):
        root=pathlib.Path(__file__).resolve().parents[1]
        for path in (root/'dsp').glob('*.dsp'):
            self.assertEqual(oversized(path.read_text()),[],str(path))
