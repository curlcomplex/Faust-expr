from pathlib import Path
import hashlib, json, subprocess, tempfile, unittest, uuid

ROOT = Path(__file__).resolve().parents[1]

class AnalogClassicsReviewExport(unittest.TestCase):
    def test_review_set_is_explicit_portable_and_non_promoting(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "review"
            subprocess.run(["python3", "tools/modules/analog_classics_review_export.py", "--out", str(out)], cwd=ROOT, check=True)
            manifest = json.loads((out / "manifest.json").read_text())
            self.assertEqual(manifest["status"], "internal-review-only")
            self.assertEqual(manifest["schema"], "curlop-analog-classics-review/v1")
            self.assertEqual(manifest["identity"], "analog-classics-internal-review-2026-09-15.3")
            self.assertEqual(manifest["outputBoundaryContract"], {"audioInstruments": 30, "audioEffects": 5, "controlRoleModules": 2, "instrumentOutput": "stereo duplicate of the exact source mono signal", "effectOutput": "native stereo preserved", "controlOutput": "native typed CV/observation signals preserved"})
            source_tree = manifest["sourceTree"]
            self.assertEqual(source_tree["branch"], "issue-118-analog-classics-review")
            self.assertRegex(source_tree["commit"], r"^[0-9a-f]{40}$")
            self.assertIn("file-level lineage commits are informational only", source_tree["contract"])
            for dependency_fix in source_tree["dependencyFixes"]:
                subprocess.run(["git", "merge-base", "--is-ancestor", dependency_fix["commit"], source_tree["commit"]], cwd=ROOT, check=True)
            self.assertEqual(len(manifest["selected"]), 37)
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
                self.assertEqual(entry["source"]["sha256"], hashlib.sha256(export.read_bytes()).hexdigest())
                self.assertEqual(entry["source"]["sha256"], entry["export_sha256"])
                self.assertEqual(entry["upstream"]["path"], entry["source_path"])
                self.assertEqual(entry["upstream"]["sha256"], entry["source_sha256"])
                self.assertEqual(entry["upstream"]["commit"], source_tree["commit"])
                self.assertEqual(entry["upstream"]["branch"], source_tree["branch"])
                self.assertEqual(entry["source_commit"], source_tree["commit"])
                self.assertEqual(entry["source_file_last_change_commit"], entry["upstream"]["sourceFileLastChangeCommit"])
                self.assertRegex(entry["source_file_last_change_commit"], r"^[0-9a-f]{40}$")
                self.assertTrue(entry["upstream"]["lineage"]["branch"])
                self.assertRegex(entry["upstream"]["lineage"]["commit"], r"^[0-9a-f]{40}$")
                source_blob = subprocess.check_output(["git", "show", f"{source_tree['commit']}:{entry['source_path']}"], cwd=ROOT)
                self.assertEqual(hashlib.sha256(source_blob).hexdigest(), entry["source_sha256"])
                self.assertEqual(entry["lineageUuid"], str(uuid.uuid5(uuid.NAMESPACE_URL, f"curlcomplex/CURLOP/{entry['id']}")))
                self.assertIn(entry["licenseStatus"], {"declared", "mixed-reviewed", "unresolved-internal-review"})
                self.assertTrue(entry["license"])
                self.assertEqual(entry["licenseIdentifier"], entry["license"])
                self.assertTrue(entry["dependencies"])
                for dependency in entry["dependencies"]:
                    self.assertIn(dependency["kind"], {"repository-library", "standard-library"})
                    if dependency["kind"] == "repository-library":
                        self.assertRegex(dependency["sha256"], r"^[0-9a-f]{64}$")
                        self.assertTrue((ROOT / dependency["path"]).exists())
                        dependency_blob = subprocess.check_output(["git", "show", f"{source_tree['commit']}:{dependency['path']}"], cwd=ROOT)
                        self.assertEqual(hashlib.sha256(dependency_blob).hexdigest(), dependency["sha256"])
                    else:
                        self.assertEqual(dependency["compilerVersion"], entry["compiler"]["faustVersion"])
                self.assertRegex(entry["compiler"]["faustVersion"], r"^FAUST Version \d+\.\d+\.\d+$")
                self.assertTrue(entry["compiler"]["cxxVersion"])
                self.assertEqual(entry["compiler"]["compileOptions"], ["-lang", "cpp", "-single", "-cn", "ModuleDSP"])
                self.assertEqual(entry["compiler"]["runnerCompileOptions"], ["-std=c++17", "-O2", "-ffp-contract=off"])
                self.assertGreaterEqual(entry["outputEvidence"]["audioInputs"], 0)
                if entry["category"] == "instrument":
                    self.assertEqual(entry["outputEvidence"]["role"], "audio-instrument")
                    self.assertEqual(entry["outputEvidence"]["audioInputs"], 0)
                    self.assertEqual(entry["outputEvidence"]["signalOutputs"], 2)
                    self.assertIn("process = curlop_mono_process <: _, _;", export.read_text())
                    adaptation = entry["outputAdaptation"]
                    self.assertEqual((adaptation["kind"], adaptation["sourceOutputs"], adaptation["finalOutputs"]), ("mono-to-stereo-duplicate", 1, 2))
                    self.assertTrue(adaptation["eachChannelByteIdenticalToSourceMono"])
                    self.assertEqual(adaptation["monoSha256"], adaptation["leftSha256"])
                    self.assertEqual(adaptation["monoSha256"], adaptation["rightSha256"])
                    controls = entry["contractEvidence"]["capturedControls"]
                    self.assertIn("gate[curlop:input]", controls)
                    self.assertIn("velocity[curlop:input]", controls)
                    self.assertTrue(any(c.startswith("freq[") and "[unit:Hz]" in c and "[curlop:input]" in c for c in controls))
                elif entry["category"] == "effect":
                    self.assertEqual(entry["outputEvidence"]["role"], "audio-effect")
                    self.assertEqual((entry["outputEvidence"]["audioInputs"], entry["outputEvidence"]["signalOutputs"]), (2, 2))
                    self.assertEqual(entry["outputAdaptation"], {"kind": "none-native-stereo", "sourceOutputs": 2, "finalOutputs": 2})
                    self.assertNotIn("curlop_mono_process", export.read_text())
                else:
                    self.assertEqual(entry["outputEvidence"]["role"], "control")
                    self.assertEqual(entry["outputEvidence"]["audioInputs"], 0)
                    self.assertEqual(entry["outputAdaptation"]["kind"], "none-control-role")
                    self.assertEqual(entry["outputAdaptation"]["sourceOutputs"], entry["outputEvidence"]["signalOutputs"])
                    self.assertEqual(entry["outputAdaptation"]["finalOutputs"], entry["outputEvidence"]["signalOutputs"])
                    self.assertNotIn("curlop_mono_process", export.read_text())
            for identity in ("analog-kick-sharp", "analog-snare", "clap"):
                adaptation = selected[identity]["metadataAdaptation"]
                self.assertEqual(adaptation["kind"], "ui-address-only")
                self.assertEqual(adaptation["originalLabels"], ['button("gate"', 'hslider("pitch_hz"', 'hslider("velocity"'])
                self.assertEqual(adaptation["finalLabels"], ["freq[unit:Hz][scale:log][curlop:input]", "gate[curlop:input]", "velocity[curlop:input]"])
                self.assertTrue(adaptation["onlyUiLabelStringsChanged"])
                equivalence = adaptation["renderedAudioEquivalence"]
                self.assertEqual((equivalence["oldAddress"], equivalence["newAddress"]), ("pitch_hz", "freq"))
                self.assertEqual((equivalence["sampleRate"], equivalence["blockSize"], equivalence["frames"]), (48000, 64, 12000))
                self.assertRegex(equivalence["audioSha256"], r"^[0-9a-f]{64}$")
                self.assertTrue(equivalence["byteIdentical"])
                self.assertNotIn("instrumented_compute_ns", equivalence["oldDiagnostics"])
                self.assertNotIn("instrumented_compute_ns", equivalence["newDiagnostics"])
                self.assertEqual(equivalence["oldDiagnostics"], equivalence["newDiagnostics"])
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
            expected_outputs = {
                "trigger-seq": (2, {("gate", "cv"), ("step", "observation")}),
                "bassline-seq": (5, {("freq", "cv"), ("gate", "cv"), ("accent", "cv"), ("slide", "cv"), ("step", "observation")}),
            }
            for identity, (count, roles) in expected_outputs.items():
                evidence = selected[identity]["outputEvidence"]
                self.assertEqual(evidence["audioInputs"], 0)
                self.assertEqual(evidence["signalOutputs"], count)
                self.assertEqual({(item["label"].split("[", 1)[0], item["role"]) for item in evidence["namedOutputs"]}, roles)
            selections = {
                "606": json.loads((ROOT / "modules/drums-606-reference/v1/selection.json").read_text())["selections"],
                "909": json.loads((ROOT / "modules/drums-909-reference/v1/selection.json").read_text())["selections"],
            }
            tom_ids = ("606-low-tom", "606-high-tom")
            for identity in tom_ids:
                family, voice = identity.split("-", 1)
                settings = selections[family][voice]["settings"]
                self.assertEqual(selected[identity]["frozenPresetSettings"], settings)
                self.assertEqual(selected[identity]["adaptedDefaults"], {k:v for k,v in settings.items() if k not in {"gate", "velocity", "accent"}})
                self.assertEqual(selected[identity]["displayName"], family + " " + voice.replace("-", " ").title())
                self.assertTrue(selected[identity]["displayMetadataAdaptation"]["onlyUiMetadataChanged"])
            self.assertEqual(len({selected[identity]["source"]["sha256"] for identity in tom_ids}), len(tom_ids))
            tom_909 = selected["909-tom"]
            self.assertEqual(tom_909["displayName"], "909 Tom")
            self.assertEqual(tom_909["source_path"], "modules/drums-909/v1/tom.dsp")
            self.assertEqual(tom_909["canonicalDefaultState"], {"kind": "source-defaults", "settings": {"gate": 0, "freq": 105.0, "velocity": 1.0, "accent": 0.0, "decay": 0.55, "bend": 0.28, "tone": 0.5, "noise": 0.1, "drive": 0.08, "level": 0.8}})
            expected_909_presets = selections["909"]
            self.assertEqual([preset["presetId"] for preset in tom_909["namedPresets"]], ["low-tom", "mid-tom", "high-tom"])
            for preset in tom_909["namedPresets"]:
                self.assertEqual(preset["settings"], expected_909_presets[preset["presetId"]]["settings"])
                self.assertEqual(preset["reference"], expected_909_presets[preset["presetId"]]["reference"])
            for preset_id in ("low-tom", "mid-tom", "high-tom"):
                old_identity = f"analog-classics:909-{preset_id}"
                self.assertNotIn(f"909-{preset_id}", selected)
                self.assertEqual(manifest["supersededIdentities"][old_identity], {"replacement": "analog-classics:909-tom", "preset": preset_id, "status": "superseded-preset-wrapper"})
                self.assertFalse((out / "scripts" / f"909-{preset_id}.dsp").exists())
            self.assertIn("modules/juno-60/candidates/pr89-0b98748d/voice.dsp", manifest["rejected_or_unselected"])
            self.assertIn("909 sample-backed voices", manifest["rejected_or_unselected"])

if __name__ == "__main__":
    unittest.main()
