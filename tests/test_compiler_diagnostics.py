"""#109 parser/process contracts and opt-in actual Faust/interpreter qualification."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("compiler_diagnostics", ROOT / "tools/modules/compiler_diagnostics.py")
cd = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cd)
FIX = ROOT / "tests/fixtures/diagnostics"


def trace_record(counts=None, banner="", *, rc=0):
    # Synthetic parser input, explicitly not a Faust execution result.
    values = dict.fromkeys(cd.COUNTERS, 0)
    values.update(counts or {})
    text = ("Libfaust version : 2.88.0\nUsing interpreter backend\n"
            "-------- Test impulse --------\n-------- Test noise ----------\n" + banner +
            "\nInterpreter statistics\n" + "".join(f"{k}: {v}\n" for k, v in values.items()) + "---\n")
    return {"stdout": text, "stderr": "", "returncode": rc, "status": "completed"}


class Contracts(unittest.TestCase):
    def test_compile_policy(self):
        self.assertFalse(cd.classify_compile({"returncode": 0, "stdout": "WARNING : interval", "stderr": ""})["hard_failure"])
        self.assertTrue(cd.classify_compile({"returncode": 1, "stdout": "", "stderr": "error"})["hard_failure"])

    def test_zero_counters_do_not_indicate_faults(self):
        result = cd.classify_trace(trace_record())
        self.assertFalse(result["hard_failure"])
        self.assertEqual(result["numerical_faults"], [])

    def test_positive_faults_override_zero_exit_code(self):
        for key in cd.HARD_COUNTERS:
            with self.subTest(counter=key):
                result = cd.classify_trace(trace_record({key: 1}))
                self.assertTrue(result["hard_failure"])
                self.assertIn(key, result["numerical_faults"])

    def test_fault_banner_without_statistics_is_hard(self):
        result = cd.classify_trace({"returncode": 0, "stdout": "-------- Interpreter 'Inf' trace start --------\n", "stderr": ""})
        self.assertIn("FP_INFINITE", result["numerical_faults"])
        self.assertTrue(result["hard_failure"])

    def test_later_zero_block_cannot_erase_earlier_fault(self):
        r = trace_record({"FP_NAN": 3})
        r["stdout"] += trace_record()["stdout"]
        self.assertIn("FP_NAN", cd.classify_trace(r)["numerical_faults"])

    def test_advisory_counters_remain_informational(self):
        result = cd.classify_trace(trace_record({"FP_SUBNORMAL": 10, "INTEGER_OVERFLOW": 6, "NEGATIVE_BITSHIFT": 1}))
        self.assertFalse(result["hard_failure"])
        self.assertEqual(len(result["advisories"]), 3)

    def test_missing_or_incomplete_instrumentation_fails(self):
        for text in ("", "Using interpreter backend\n", trace_record()["stdout"].replace("FP_NAN: 0\n", "")):
            result = cd.classify_trace({"returncode": 0, "stdout": text, "stderr": ""})
            self.assertTrue(result["hard_failure"])
            self.assertFalse(result["coverage_complete"])

    def test_wrong_runtime_version_fails(self):
        r = trace_record()
        r["stdout"] = r["stdout"].replace("2.88.0", "2.70.3")
        self.assertTrue(cd.classify_trace(r)["hard_failure"])

    def test_incomplete_input_suite_fails(self):
        r = trace_record()
        r["stdout"] = r["stdout"].replace("-------- Test noise ----------", "")
        self.assertTrue(cd.classify_trace(r)["hard_failure"])

    def test_unknown_trace_or_process_error_fails(self):
        for r in (trace_record(banner="Interpreter 'Load' trace start\n"), trace_record(rc=-6),
                  dict(trace_record(), status="timeout")):
            self.assertTrue(cd.classify_trace(r)["hard_failure"])

    def test_process_timeout_preserves_logs(self):
        with tempfile.TemporaryDirectory() as work:
            r = cd.run(["/bin/sh", "-c", "printf before; sleep 5"], work, "timeout", timeout=1)
            self.assertEqual(r["status"], "timeout")
            self.assertIn("before", r["stdout"])
            self.assertTrue(Path(r["logs"]["stdout"]["path"]).is_file())

    def test_source_output_collision_rejected(self):
        with tempfile.TemporaryDirectory() as work:
            p = Path(work) / "input.dsp"
            p.write_text("process = _;")
            with self.assertRaises(ValueError):
                cd.diagnose(p, "unused", work, work)
            self.assertEqual(p.read_text(), "process = _;")

    def test_wrong_archive_and_missing_libraries_rejected(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            with self.assertRaises(ValueError):
                cd.libraries_identity(root)
            (root / "stdfaust.lib").write_text("fake")
            archive = root / "archive"
            archive.write_text("wrong")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                cd.libraries_identity(root, archive)

    def test_failed_preflight_removes_stale_report(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            source = root / "missing.dsp"
            out = root / "evidence"
            out.mkdir()
            (out / "diagnostics.json").write_text('{"status": "completed"}')
            with self.assertRaises(ValueError):
                cd.diagnose(source, "definitely-no-faust-109", root / "libs", out)
            self.assertFalse((out / "diagnostics.json").exists())
            self.assertFalse((out / ".diagnostics-lock").exists())


@unittest.skipUnless(os.environ.get("COMPILER_DIAGNOSTICS_INTEGRATION") == "1", "requires pinned Faust and trace-capable interpreter")
class RealDiagnostics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.evidence = Path(os.environ.get("COMPILER_DIAGNOSTICS_EVIDENCE", cls.root / "evidence")).resolve()
        cls.evidence.mkdir(parents=True, exist_ok=True)
        cls.faust, cls.libs, cls.tracer = (os.environ[k] for k in ("FAUST", "FAUST_LIBRARIES", "INTERP_TRACER"))
        cls.archive = os.environ.get("FAUST_ARCHIVE")

    def diagnose(self, name, *, tracer=True, structural=False, suffix=""):
        r = cd.diagnose(FIX / (name + ".dsp"), self.faust, self.libs, self.evidence / (name + suffix),
                        self.tracer if tracer else None, structural=structural, archive=self.archive)
        self.assertTrue(r["input_and_dependencies_unchanged"])
        return r

    def test_safe_compiler_and_real_instrumentation(self):
        r = self.diagnose("safe")
        self.assertFalse(r["hard_failure"])
        tr = r["interp_tracer"]
        self.assertTrue(tr["classification"]["coverage_complete"])
        self.assertGreaterEqual(len(tr["classification"]["reported_statistics"]), 3)
        self.assertIn("FAUST_LIB_PATH", tr["environment"])
        self.assertTrue(r["provenance"]["dependencies"])

    def test_generator_uses_bounded_control_profile(self):
        r = self.diagnose("generator")
        self.assertFalse(r["hard_failure"])
        self.assertEqual(r["interp_tracer"]["profile"], "controls")

    def test_compile_domain_error_is_not_called_runtime_fault(self):
        r = self.diagnose("domain_error")
        self.assertTrue(r["compiler_classification"]["hard_failure"])
        self.assertEqual(r["interp_tracer"]["status"], "not_run_compile_failed")
        self.assertIn("log", r["compiler"]["stderr"])

    def test_math_warning_is_not_automatically_hard(self):
        r = self.diagnose("divzero_output", tracer=False, suffix="-compile-only")
        self.assertTrue(r["compiler_classification"]["warning_present"])
        self.assertFalse(r["hard_failure"])
        self.assertEqual(r["interp_tracer"]["status"], "not_requested")

    def test_runtime_division_has_positive_fault_not_just_counter_label(self):
        r = self.diagnose("divzero_output")
        self.assertFalse(r["compiler_classification"]["hard_failure"])
        self.assertIn("DIV_BY_ZERO_REAL", r["interp_tracer"]["classification"]["numerical_faults"])
        self.assertTrue(r["hard_failure"])

    def test_runtime_domain_and_feedback_overflow(self):
        for name, expected in (("runtime_domain", "FP_NAN"), ("overflow_state", "FP_INFINITE")):
            r = self.diagnose(name)
            self.assertFalse(r["compiler_classification"]["hard_failure"])
            self.assertIn(expected, r["interp_tracer"]["classification"]["numerical_faults"])
            self.assertTrue(r["hard_failure"])

    def test_hidden_fault_survives_finite_native_output(self):
        import numpy as np
        before = set(FIX.iterdir())
        r = self.diagnose("divzero_hidden")
        self.assertFalse(r["compiler_classification"]["hard_failure"])
        tr = r["interp_tracer"]
        self.assertTrue(tr["classification"]["numerical_faults"])
        self.assertTrue(r["hard_failure"])
        self.assertEqual(set(FIX.iterdir()), before, "upstream rc side effects escaped isolation")
        build = Path(r["compiler"]["cwd"])
        # Reuse the bounded native host, not the interpreter, to check actual final audio.
        (build / "analysis_generated.hpp").write_bytes((build / "diagnostic.cpp").read_bytes())
        command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-DFAUST_ANALYSIS_OUTPUTS=1",
                   "-I" + str(build), str(ROOT / "tools/modules/faust_analysis_runner.cpp"), "-o", str(build / "native")]
        subprocess.run(command, check=True, capture_output=True, timeout=90)
        x = np.zeros(1024, dtype="<f4")
        x[0] = 1
        x.tofile(build / "input.f32")
        subprocess.run([str(build / "native"), str(build / "input.f32"), str(build / "output.f32"), "44100", "1024", "16", "0"], check=True, timeout=20)
        y = np.fromfile(build / "output.f32", dtype="<f4")
        self.assertEqual(len(y), 1024)
        self.assertTrue(np.isfinite(y).all())
        self.assertTrue(np.array_equal(y, np.ones(1024, dtype="<f4")))
        cd.write_json(build / "native-comparison.json", {"command": command, "frames": 1024,
                      "all_finite": True, "min": float(y.min()), "max": float(y.max()),
                      "input_sha256": cd.sha(build / "input.f32"), "output_sha256": cd.sha(build / "output.f32")})

    def test_structural_contains_actual_ocpp_signature(self):
        r = self.diagnose("safe", tracer=False, structural=True, suffix="-structure")
        self.assertFalse(r["hard_failure"])
        self.assertEqual(r["structural"]["status"], "completed")
        self.assertEqual(r["structural"]["backend"], "ocpp")
        self.assertTrue(r["structural"]["signatures"][0].startswith("SS_SIG nodes="))
        self.assertIn("never runtime-performance", r["structural"]["interpretation"])
        self.assertIn("cpp", r["compiler"]["command"])

    def test_repeatable_fault_classification_and_input_hash(self):
        a = self.diagnose("divzero_hidden", suffix="-repeat-a")
        b = self.diagnose("divzero_hidden", suffix="-repeat-b")
        self.assertEqual(a["input"], b["input"])
        self.assertEqual(a["interp_tracer"]["classification"], b["interp_tracer"]["classification"])

    def test_public_cli_returns_failure_for_runtime_fault(self):
        for name, expected in (("safe", 0), ("divzero_hidden", 2)):
            output = self.evidence / ("cli-" + name)
            command = [sys.executable, str(ROOT / "tools/modules/compiler_diagnostics.py"), str(FIX / (name + ".dsp")),
                       "--faust", self.faust, "--faust-libraries", self.libs, "--interp-tracer", self.tracer, "--out", str(output)]
            if self.archive:
                command += ["--faust-archive", self.archive]
            p = subprocess.run(command, cwd=self.root, capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, expected, p.stderr)
            self.assertEqual(json.loads(p.stdout)["hard_failure"], expected != 0)
            self.assertEqual(json.loads((output / "diagnostics.json").read_text())["hard_failure"], expected != 0)


if __name__ == "__main__":
    unittest.main()
