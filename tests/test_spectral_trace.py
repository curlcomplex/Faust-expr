"""#107 real Faust trend tests and independent host/report contract tests.

SPECTRAL_TRACE_INTEGRATION=1 requires explicit Faust 2.88.0 + matching libraries.
"""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("spectral_trace", ROOT / "tools/modules/spectral_trace.py")
st = importlib.util.module_from_spec(spec)
spec.loader.exec_module(st)


class Contracts(unittest.TestCase):
    def test_names_and_profile_do_not_alias_fft_metrics(self):
        self.assertTrue(all("filterbank" in x for x in st.FIELDS[1:]))
        self.assertEqual(st.CONFIG, st.profile())
        self.assertEqual(st.CONFIG["bands_per_octave"], 3)
        self.assertEqual(st.CONFIG["power_window_seconds"], 0.02)
        self.assertNotIn("power_tau_seconds", st.CONFIG)

    def test_window_preserves_single_sample_flux_peak(self):
        values = np.zeros((31, 5), dtype=np.float32)
        values[3, 3] = 0.75
        row = st.reduce_window(values, 0, 48000, 2)
        self.assertEqual(row[st.FIELDS[3]], 0.0)
        self.assertEqual(row["statistics"][st.FIELDS[3]]["max"], 0.75)
        self.assertAlmostEqual(row["statistics"][st.FIELDS[3]]["mean"], 0.75 / 31)
        self.assertEqual(row["end_frame_exclusive"], 31)
        self.assertEqual(row["last_frame"], 30)
        self.assertEqual(row["channel"], 2)

    def test_nonfinite_window_rejected(self):
        for invalid in (np.nan, np.inf, -np.inf):
            values = np.zeros((3, 5))
            values[1, 2] = invalid
            with self.assertRaisesRegex(RuntimeError, "nonfinite"):
                st.reduce_window(values, 0, 48000, 0)

    def test_invalid_dimensions_and_nyquist_rejected(self):
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / "input.f32"
            np.zeros(16, "<f4").tofile(path)
            for rate, channels, stride, block in ((8000, 1, 1, 1), (20000, 1, 1, 1),
                    (48000, 0, 1, 1), (48000, 33, 1, 1), (48000, 1, 0, 1),
                    (48000, 1, 1.5, 1), (48000, 1, True, 1), (48000, 1, 1, 0)):
                with self.assertRaises(ValueError):
                    st.validate(path, rate, channels, stride, block)

    def test_incomplete_frames_empty_and_nonfinite_input_rejected(self):
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / "input.f32"
            for data in (b"", b"bad", np.array([np.inf], "<f4").tobytes(),
                         np.array([np.nan], "<f4").tobytes()):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    st.validate(path, 48000, 1, 240, 256)
            np.zeros(3, "<f4").tofile(path)
            with self.assertRaises(ValueError):
                st.validate(path, 48000, 2, 240, 256)

    def test_resource_bound_before_execution(self):
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / "input.f32"
            with path.open("wb") as f:
                f.truncate((st.MAX_ROWS + 1) * 4)
            with self.assertRaisesRegex(ValueError, "resource"):
                st.validate(path, 48000, 1, 1, 256)

    def test_failed_rerun_has_no_success_manifest(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            source, out = root / "input.f32", root / "out"
            source.write_bytes(b"bad")
            out.mkdir()
            (out / "spectral-trace.json").write_text('{"complete":true}')
            with self.assertRaises(ValueError):
                st.analyze(source, 48000, out, root / "unused", {})
            self.assertFalse((out / "spectral-trace.json").exists())

    def test_input_output_collision_rejected_without_damage(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            source = root / "spectral-trace.json"
            source.write_bytes(b"original")
            with self.assertRaises(ValueError):
                st.analyze(source, 48000, root, root / "unused", {})
            self.assertEqual(source.read_bytes(), b"original")

    def test_explicit_libraries_required(self):
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaisesRegex(ValueError, "explicit"):
                st.build(Path(work), "unused", "unused", None)

    def test_ci_pipelines_fail_closed(self):
        workflow = (ROOT / ".github/workflows/faust-analysis-288.yml").read_text()
        self.assertIn("shell: bash", workflow)
        self.assertIn("Verify logged test failures propagate", workflow)
        p = subprocess.run(["bash", "-e", "-o", "pipefail", "-c", "exit 7 | cat"], capture_output=True)
        self.assertEqual(p.returncode, 7)


@unittest.skipUnless(os.environ.get("SPECTRAL_TRACE_INTEGRATION") == "1", "requires pinned Faust integration")
class RealDescriptors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.root = Path(cls.tmp.name)
        cls.evidence = Path(os.environ.get("SPECTRAL_TRACE_EVIDENCE", cls.root / "evidence")).resolve()
        cls.evidence.mkdir(parents=True, exist_ok=True)
        cls.runner, cls.prov = st.build(cls.evidence / "build", os.environ["FAUST"],
                os.environ.get("CXX", "c++"), os.environ["FAUST_LIBRARIES"], os.environ.get("FAUST_ARCHIVE"))
        cls.results = {}

    @classmethod
    def tearDownClass(cls):
        (cls.evidence / "trend-results.json").write_text(json.dumps(cls.results, indent=2) + "\n")

    def measure(self, name, samples, rate=48000, stride=240, block=256):
        samples = np.asarray(samples, dtype="<f4")
        channels = 1 if samples.ndim == 1 else samples.shape[1]
        src = self.root / (name + ".f32")
        samples.tofile(src)
        before = st.sha(src)
        out = self.evidence / name
        report = st.analyze(src, rate, out, self.runner, self.prov, stride, channels=channels, block=block)
        self.assertEqual(st.sha(src), before)
        self.assertEqual(report["input"]["sha256"], before)
        return report, st.read_trace(out), src

    @staticmethod
    def median(rows, field, start, end):
        return float(np.median([r[field] for r in rows if start <= r["time_seconds"] < end]))

    def test_centroid_tracks_frequency_direction(self):
        results = []
        for rate in (44100, 48000, 96000):
            t = np.arange(rate * 2) / rate
            _, low, _ = self.measure(f"low-{rate}", 0.2 * np.sin(2*np.pi*500*t), rate)
            _, high, _ = self.measure(f"high-{rate}", 0.2 * np.sin(2*np.pi*4000*t), rate)
            lc, hc = (self.median(rows, st.FIELDS[1], 1, 2) for rows in (low, high))
            self.assertGreater(hc, lc * 3)
            # A filter-bank descriptor has finite resolution, not exact pitch.
            self.assertTrue(350 < lc < 750 and 3000 < hc < 6000)
            results.append({"rate": rate, "low_centroid_hz": lc, "high_centroid_hz": hc})
        self.results["frequency_direction"] = results

    def test_spread_increases_for_two_separated_tones(self):
        t = np.arange(96000) / 48000
        _, one, _ = self.measure("one", .1*np.sin(2*np.pi*1000*t))
        _, two, _ = self.measure("two", .1*(np.sin(2*np.pi*500*t)+np.sin(2*np.pi*4000*t)))
        a, b = (self.median(rows, st.FIELDS[2], 1, 2) for rows in (one, two))
        self.assertGreater(b, a * 1.5)
        self.results["spread"] = {"single_tone_hz": a, "separated_tones_hz": b}

    def test_flux_marks_onset_more_than_steady_state(self):
        x = np.zeros(96000)
        x[24000:] = .2*np.sin(2*np.pi*1000*np.arange(72000)/48000)
        _, rows, _ = self.measure("onset", x, stride=48)
        onset = max(r["statistics"][st.FIELDS[3]]["max"] for r in rows if .49 < r["time_seconds"] < .60)
        steady = self.median(rows, st.FIELDS[3], 1.2, 1.8)
        self.assertGreater(onset, max(steady * 10, 1e-6))
        self.results["flux"] = {"onset_max": onset, "steady_median": steady}

    def test_bright_to_dark_trace_moves_down(self):
        rate = 48000
        t = np.arange(rate * 3) / rate
        phase = 2*np.pi*np.cumsum(np.where(t < 1.5, 4000., 500.))/rate
        _, rows, _ = self.measure("bright-dark", .15*np.sin(phase))
        early, late = (self.median(rows, st.FIELDS[1], a, b) for a,b in ((.7,1.3),(2.2,2.8)))
        self.assertGreater(early, late * 3)
        self.results["bright_dark"] = {"early_centroid_hz": early, "late_centroid_hz": late}

    def test_silence_and_stereo_channel_reset(self):
        n = np.arange(12000)
        x = np.column_stack((.2*np.sin(2*np.pi*997*n/48000), np.zeros(len(n))))
        report, rows, _ = self.measure("channel-reset", x)
        silent = [r for r in rows if r["channel"] == 1]
        self.assertEqual(len(silent), 50)
        for row in silent:
            self.assertFalse(row["power_above_epsilon_last"])
            for field in st.FIELDS[1:]:
                self.assertEqual(row[field], 0.0)
        self.assertEqual(report["sampling"]["channel_policy"], "independent mono, no summing/downmix")

    def test_block_invariance_and_final_partial_window(self):
        x = np.random.default_rng(107).normal(0, .1, 2053)
        all_rows = []
        for block in (1, 127, 256, 511):
            report, rows, _ = self.measure(f"block-{block}", x, stride=31, block=block)
            self.assertEqual(len(rows), 67)
            self.assertEqual((rows[-1]["start_frame"], rows[-1]["end_frame_exclusive"]), (2046, 2053))
            self.assertEqual(rows[-1]["last_frame"], 2052)
            self.assertEqual(report["sampling"]["tail_frames"], 0)
            all_rows.append(rows)
        for rows in all_rows[1:]:
            self.assertEqual(rows, all_rows[0])

    def test_stride_reduction_matches_samplewise_trace(self):
        x = np.zeros(1001)
        x[3] = .9
        _, dense, _ = self.measure("dense", x, stride=1)
        _, coarse, _ = self.measure("coarse", x, stride=37)
        for row in coarse:
            section = dense[row["start_frame"]:row["end_frame_exclusive"]]
            for field in st.FIELDS[1:]:
                v = np.array([r[field] for r in section])
                for stat, expected in (("min", v.min()), ("max", v.max()), ("mean", v.mean()), ("last", v[-1])):
                    self.assertEqual(row["statistics"][field][stat], expected)

    def test_amplitude_behavior_is_not_hidden_by_normalization(self):
        x = .1*np.sin(2*np.pi*997*np.arange(24000)/48000)
        _, first, _ = self.measure("amplitude", x)
        _, doubled, _ = self.measure("amplitude-double", 2*x)
        for a, b in zip(first[10:], doubled[10:]):
            self.assertAlmostEqual(a[st.FIELDS[1]], b[st.FIELDS[1]], delta=.001)
            self.assertAlmostEqual(a[st.FIELDS[2]], b[st.FIELDS[2]], delta=.01)
            self.assertAlmostEqual(a[st.FIELDS[3]]*2, b[st.FIELDS[3]], delta=1e-7)
            self.assertAlmostEqual(a[st.FIELDS[4]]*4, b[st.FIELDS[4]], delta=1e-7)

    def test_repeatability_and_tampered_capture_rejected(self):
        x = np.random.default_rng(17).normal(0, .05, 2048)
        a, ar, _ = self.measure("repeat-a", x)
        b, br, _ = self.measure("repeat-b", x)
        self.assertEqual(ar, br)
        self.assertEqual(a["trace"]["sha256"], b["trace"]["sha256"])
        path = self.evidence / "repeat-b" / "spectral-trace.jsonl"
        original = path.read_bytes()
        path.write_bytes(original[:-10])
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            st.read_trace(path.parent)
        path.write_bytes(original)

    def test_runner_provenance_mismatch_rejected(self):
        source = self.root / "mismatch.f32"
        np.zeros(16, "<f4").tofile(source)
        for bad in ({**self.prov, "runner_binary_sha256": "wrong"}, {**self.prov, "config": {}}):
            with self.assertRaisesRegex(ValueError, "provenance"):
                st.analyze(source, 48000, self.root / "bad", self.runner, bad)

    def test_existing_lab_report_is_linked_not_rewritten(self):
        source = self.root / "lab-input.f32"
        np.zeros(2048, "<f4").tofile(source)
        parent = self.root / "lab-results.json"
        parent.write_text(json.dumps({"source_commit": "fixture", "renders": [{"label": "test-render",
            "raw_sha256": st.sha(source), "diagnostics": {"rate": 48000, "frames": 2048, "channels": 1},
            "metrics": {"existing_fft_metric": 123}}], "passed": True}))
        old = parent.read_bytes()
        report = st.analyze(source, 48000, self.evidence / "linked", self.runner, self.prov, lab_report=parent)
        self.assertEqual(parent.read_bytes(), old)
        self.assertEqual(report["parent_lab_report"]["render_labels"], ["test-render"])
        with self.assertRaisesRegex(ValueError, "dimensions"):
            st.analyze(source, 44100, self.root / "bad-parent", self.runner, self.prov, lab_report=parent)

    def test_public_cli_from_different_cwd(self):
        source = self.root / "cli-input.f32"
        (.2*np.sin(2*np.pi*997*np.arange(4097)/48000)).astype("<f4").tofile(source)
        out = self.evidence / "cli"
        p = subprocess.run([sys.executable, str(ROOT / "tools/modules/spectral_trace.py"), str(source),
                "--rate", "48000", "--out", str(out), "--stride", "127"],
                cwd=self.root, capture_output=True, text=True, timeout=180)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertTrue(json.loads(p.stdout)["complete"])
        self.assertEqual(len(st.read_trace(out)), 33)
        report = json.loads((out / "spectral-trace.json").read_text())
        self.assertEqual(report["provenance"]["release_archive_verified"], bool(os.environ.get("FAUST_ARCHIVE")))


if __name__ == "__main__":
    unittest.main()
