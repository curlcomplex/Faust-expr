"""Synthetic mapping-oracle checks; actual mapping is compiled/tested separately."""
import importlib.util
import itertools
import json
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("control_probe",ROOT/"tools/modules/control_surface_probe.py")
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class CompactMapping(unittest.TestCase):
    def setUp(self):
        self.defs=json.loads((ROOT/"modules/kick-pm/playable-06/manifest.json").read_text())["musical_controls"]
        self.p={k:v["default"] for k,v in self.defs.items()}
    def test_eight_controls(self):
        self.assertEqual(len(self.defs),8)
    def test_zero_color_and_drive(self):
        p=m.map_static(self.p|dict(color=0,shape=0,drive=0,sweep=0),0)
        self.assertEqual((p["square"],p["triangle"],p["drive"],p["pitch_amount_hz"]),(0,0,0,0))
    def test_decay_monotonic(self):
        ps=[m.map_static(self.p|dict(decay=i/100),0) for i in range(101)]
        self.assertTrue(all(a["body_tau_s"]<b["body_tau_s"] and a["release_tau_s"]<b["release_tau_s"] for a,b in zip(ps,ps[1:])))
    def test_sweep_is_pitch_relative(self):
        a=m.map_static(self.p|dict(pitch_hz=40),0);b=m.map_static(self.p|dict(pitch_hz=80),0)
        self.assertAlmostEqual(b["pitch_amount_hz"],2*a["pitch_amount_hz"])
    def test_punch_changes_timing_not_level(self):
        a=m.map_static(self.p|dict(punch=0),0);b=m.map_static(self.p|dict(punch=1),0)
        self.assertLess(b["attack_tau_s"],a["attack_tau_s"]);self.assertLess(b["pitch_tau_s"],a["pitch_tau_s"])
        self.assertEqual(a["level"],b["level"])
    def test_profiles_change_only_placement(self):
        a=m.map_static(self.p,0);b=m.map_static(self.p,1)
        self.assertEqual([k for k in a if a[k]!=b[k]],["drive_after_body"])
    def test_corners_in_kernel_domain(self):
        for values in itertools.product((0.,1.),repeat=7):
            p=self.p|dict(zip([k for k in self.p if k!="pitch_hz"],values))
            for pitch in (20,160):
                x=m.map_static(p|dict(pitch_hz=pitch),0)
                self.assertTrue(0<=x["pitch_amount_hz"]<=2000)
                self.assertTrue(.005<=x["body_tau_s"]<=2)
                self.assertTrue(.002<=x["release_tau_s"]<=2)
    def test_contour_changes_depth_and_time(self):
        a=m.map_static(self.p|dict(contour=0),0);b=m.map_static(self.p|dict(contour=1),0)
        self.assertGreater(b["mod_envelope"],a["mod_envelope"]);self.assertLess(b["mod_tau_s"],a["mod_tau_s"])

if __name__=="__main__":unittest.main()
