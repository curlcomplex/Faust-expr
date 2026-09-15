from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
RENDER=(ROOT/'tools/modules/render.cpp').read_text()
FIXTURE=(ROOT/'tests/fixtures/probe_debug.dsp').read_text()

class ProbeRendererContracts(unittest.TestCase):
    def test_metadata_and_hierarchy_are_preserved(self):
        self.assertIn('pending[p][k]=v', RENDER)
        self.assertIn('groups.emplace_back', RENDER)
        self.assertIn('meta.find("probe")', RENDER)
    def test_controls_exclude_read_only_zones(self):
        self.assertIn('if(z.writable)', RENDER)
        self.assertIn('!z.writable&&z.meta.count("probe")', RENDER)
    def test_probe_capture_is_sample_stepped(self):
        self.assertIn('count=std::min(count,1)', RENDER)
        self.assertIn('n%stride==0', RENDER)
        self.assertIn('time_seconds', RENDER)
    def test_fixture_uses_real_debug_library_metadata(self):
        self.assertIn('db.probe_rms_lin(1060, 1)', FIXTURE)
        self.assertIn('db.probe_dc(1061, 1)', FIXTURE)

if __name__=='__main__': unittest.main()
