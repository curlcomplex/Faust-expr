from pathlib import Path
import json, subprocess, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]

class AnalogClassicsReviewExport(unittest.TestCase):
    def test_review_set_is_explicit_portable_and_non_promoting(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "review"
            subprocess.run(["python3", "tools/modules/analog_classics_review_export.py", "--out", str(out)], cwd=ROOT, check=True)
            manifest = json.loads((out / "manifest.json").read_text())
            self.assertEqual(manifest["status"], "internal-review-only")
            self.assertEqual(len(manifest["selected"]), 39)
            self.assertEqual(manifest["modules"], manifest["selected"])
            selected = {entry["identity"]: entry for entry in manifest["modules"]}
            self.assertEqual(selected["mini"]["source_path"], "modules/minimoog/v1/voice.dsp")
            self.assertEqual(selected["juno-60"]["source_path"], "modules/juno-60/v3/voice.dsp")
            for entry in manifest["selected"]:
                export = out / entry["export_path"]
                self.assertTrue(export.exists())
                self.assertNotIn('import("', export.read_text())
                self.assertNotIn('library("', export.read_text())
                self.assertTrue(export.with_suffix(".hpp").exists())
                self.assertEqual(entry["status"], "internal-review")
                self.assertEqual(entry["source"]["path"], entry["export_path"])
                if entry["category"] == "instrument":
                    controls = entry["contractEvidence"]["capturedControls"]
                    self.assertIn("gate[curlop:input]", controls)
                    self.assertIn("velocity[curlop:input]", controls)
                    self.assertTrue(any(c.startswith("freq[") and "[unit:Hz]" in c and "[curlop:input]" in c for c in controls))
            for identity in ("analog-kick-sharp", "analog-snare", "clap"):
                adaptation = selected[identity]["metadataAdaptation"]
                self.assertEqual(adaptation["kind"], "ui-address-only")
                self.assertEqual(adaptation["originalLabels"], ['button("gate"', 'hslider("pitch_hz"', 'hslider("velocity"'])
                self.assertEqual(adaptation["finalLabels"], ["freq[unit:Hz][scale:log][curlop:input]", "gate[curlop:input]", "velocity[curlop:input]"])
                self.assertTrue(adaptation["onlyUiLabelStringsChanged"])
                self.assertTrue(selected[identity]["contractEvidence"]["metadataAdapted"])
            for identity, entry in selected.items():
                if identity not in {"analog-kick-sharp", "analog-snare", "clap"}:
                    self.assertFalse(entry["contractEvidence"]["metadataAdapted"])
                    self.assertIsInstance(entry["metadataAdaptation"], str)
            self.assertIn("accent", selected["acid-voice"]["contractEvidence"]["specialEvents"])
            self.assertIn("slide", selected["acid-voice"]["contractEvidence"]["specialEvents"])
            self.assertIn("accent", selected["606-kick"]["contractEvidence"]["specialEvents"])
            self.assertIn("chokeGate", selected["606-open-hat"]["contractEvidence"]["specialEvents"])
            for event in ("clock", "reset", "run"):
                self.assertIn(event, selected["trigger-seq"]["contractEvidence"]["specialEvents"])
            self.assertIn("modules/juno-60/candidates/pr89-0b98748d/voice.dsp", manifest["rejected_or_unselected"])
            self.assertIn("909 sample-backed voices", manifest["rejected_or_unselected"])

if __name__ == "__main__":
    unittest.main()
