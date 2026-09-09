import importlib.util,unittest
from pathlib import Path
import analyze_groups as analyze
from make_runtime import generate
class EvidenceTests(unittest.TestCase):
    def test_inventory(self):self.assertEqual(len(analyze.definitions()),20);self.assertEqual(len(set(analyze.definitions())),20)
    def test_no_missing(self):
        with self.assertRaises(ValueError):analyze.validate_inventory([])
    def test_timing_negative(self):
        with self.assertRaises(ValueError):analyze.stats([-1])
    def test_timing_nan(self):
        with self.assertRaises(ValueError):analyze.stats([float('nan')])
    def test_no_gain_fit(self):
        with self.assertRaises(ValueError):analyze.compare([.1,.2],[.2,.4])
    def test_no_time_fit(self):
        with self.assertRaises(ValueError):analyze.compare([.1,.2],[0,.1])
    def test_nan_audio(self):
        with self.assertRaises(ValueError):analyze.compare([float('nan')],[0])
    def test_length(self):
        with self.assertRaises(ValueError):analyze.compare([1],[1,1])
    def test_missing_impulse(self):
        with self.assertRaises(ValueError):analyze.impulse([0]*10)
    def test_multiple_impulses(self):
        with self.assertRaises(ValueError):analyze.impulse([0,.01,0,.01])
    def test_retainer_pin(self):
        with self.assertRaises(ValueError):generate(b'wrong version')
    def test_patched_render_unchanged(self):
        p=Path(__file__).resolve().parents[2]/'vendor/curlop-latency/scripts/bench/patching_latency/RetainedFaustGraph.h'
        source=p.read_bytes();derived=generate(source)
        self.assertEqual(source.decode().split('static bool render')[1],derived.split('static bool render')[1])
        self.assertIn('externalFactory_',derived)
if __name__=='__main__':unittest.main(verbosity=2)
