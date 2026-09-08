"""Synthetic validator/metric tests. These do NOT substitute for Faust renders."""
from array import array
import copy
import math
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from plan_llvm_codegen import FAUST_COMMIT, SCHEMA, compile_specs, dependencies, emit_header, validate
from plan_llvm_checkpoint import FRAMES, metrics, read_samples


def fixture():
    """Explicitly synthetic delayed-input plan, not an upstream export."""
    return {
        "schema": SCHEMA, "faust_commit": FAUST_COMMIT,
        "stage": "post-fusion-before-emission", "fusion_enabled": True,
        "chunk_size": 32, "inputs": 1, "outputs": 1,
        "members": [{"id": 0, "block": 0, "max_delay": 7, "integer": False,
                     "refs": [], "refs0": [], "expression": {"op": "input", "index": 0}}],
        "blocks": [{"id": 0, "members": [0], "dependencies": [], "inputs": [], "modeled_ops": 0}],
        "output_expressions": [{"op": "history", "member": 0, "delay": 7}],
        "output_tail_outside_partition": True,
    }


class PlanValidation(unittest.TestCase):
    def test_valid_synthetic_plan(self):
        self.assertEqual(validate(fixture())["blocks"], 1)

    def test_wrong_schema(self):
        p = fixture(); p["schema"] = "unknown"
        with self.assertRaises(ValueError): validate(p)

    def test_wrong_compiler_identity(self):
        p = fixture(); p["faust_commit"] = "unversioned"
        with self.assertRaises(ValueError): validate(p)

    def test_missing_tail_is_rejected(self):
        p = fixture(); p["output_tail_outside_partition"] = False
        with self.assertRaises(ValueError): validate(p)

    def test_missing_output_expression(self):
        p = fixture(); p["output_expressions"] = []
        with self.assertRaises(ValueError): validate(p)

    def test_integer_materialized_state(self):
        p = fixture(); p["members"][0]["integer"] = True
        with self.assertRaises(ValueError): validate(p)

    def test_output_history_capacity(self):
        p = fixture(); p["output_expressions"][0]["delay"] = 8
        with self.assertRaises(ValueError): validate(p)

    def test_negative_delay(self):
        p = fixture(); p["output_expressions"][0]["delay"] = -1
        with self.assertRaises(ValueError): validate(p)

    def test_boolean_is_not_numeric_delay(self):
        p = fixture(); p["output_expressions"][0]["delay"] = True
        with self.assertRaises(ValueError): validate(p)

    def test_unknown_primitive(self):
        p = fixture(); p["members"][0]["expression"] = {"op": "sin", "arg": {"op": "input", "index": 0}}
        with self.assertRaises(ValueError): validate(p)

    def test_nonfinite_constant(self):
        for v in (math.nan, math.inf, -math.inf):
            p = fixture(); p["members"][0]["expression"] = {"op": "real", "value": v}
            with self.assertRaises(ValueError): validate(p)

    def test_extra_expression_field(self):
        p = fixture(); p["members"][0]["expression"]["hidden_state"] = 1
        with self.assertRaises(ValueError): validate(p)

    def test_invalid_input(self):
        p = fixture(); p["members"][0]["expression"]["index"] = 1
        with self.assertRaises(ValueError): validate(p)

    def test_duplicate_ownership(self):
        p = fixture(); p["blocks"][0]["members"] = [0, 0]
        with self.assertRaises(ValueError): validate(p)

    def test_ast_dependency_mismatch(self):
        p = fixture(); p["members"][0]["refs"] = [0]
        with self.assertRaises(ValueError): validate(p)

    def test_instantaneous_cycle(self):
        p = fixture(); p["members"][0].update(refs=[0], refs0=[0], expression={"op": "ref", "member": 0})
        with self.assertRaises(ValueError): validate(p)

    def test_history_is_not_discarded_from_constraint_graph(self):
        p = fixture(); p["members"][0]["expression"] = {"op": "history", "member": 0, "delay": 7}
        with self.assertRaises(ValueError): validate(p)
        p["members"][0]["refs"] = [0]
        validate(p)  # A local delayed cycle is causal and must retain its state.

    def test_chunk_free_feedback_is_still_a_history_input(self):
        p = fixture(); p["members"][0].update(max_delay=64, expression={"op": "history", "member": 0, "delay": 64})
        validate(p)
        self.assertIn(("history", 0, 64), compile_specs(p, False)[0]["inputs"])

    def test_stateful_lowering_uses_recursion_and_retains_output_tail(self):
        p = fixture(); p["members"][0].update(refs=[0], expression={"op": "history", "member": 0, "delay": 7})
        code = emit_header(p, "process = _;\n")
        self.assertIn("sigDelay(sigSelfN(0),sigInt(6))", code)
        self.assertIn("sigRecursionN({s0})", code)
        specs = compile_specs(p)
        self.assertEqual(len(specs), 2)
        self.assertEqual(specs[0]["inputs"], [])
        self.assertEqual(specs[1]["inputs"], [("history", 0, 7)])
        self.assertTrue(specs[1]["tail"])

    def test_one_sample_oracle_keeps_local_history_in_host(self):
        p = fixture(); p["members"][0].update(refs=[0], expression={"op": "history", "member": 0, "delay": 7})
        code = emit_header(p, "process = _;\n", False)
        self.assertIn("plan_chunk_size = 1", code)
        self.assertNotIn("sigRecursionN", code)
        self.assertIn(("history", 0, 7), compile_specs(p, False)[0]["inputs"])

    def test_source_literal_delimiter_rejected(self):
        with self.assertRaises(ValueError): emit_header(fixture(), ')FAUSTSOURCE"')

    def test_reader_excludes_source_and_signal_construction(self):
        header = emit_header(fixture(), "process = _;\n")
        before, after = header.split("#ifndef PLAN_READER")
        self.assertNotIn("whole_source", before)
        self.assertNotIn("construct_region", before)
        self.assertIn("whole_source", after)


class NumericEvidence(unittest.TestCase):
    def test_first_sample_spike_is_visible(self):
        m = metrics(array("f", [0, 0, 0]), array("f", [0.5, 0, 0]))
        self.assertEqual(m["max_abs"], 0.5)
        self.assertEqual(m["max_index"], 0)
        self.assertGreater(m["rms"], 0)

    def test_no_hidden_gain_fit(self):
        m = metrics(array("f", [0.1, -0.2]), array("f", [0.2, -0.4]))
        self.assertGreater(m["max_abs"], 0.19)

    def test_exact_lengths_and_nonempty(self):
        for a, b in (([], []), ([1], [1, 2])):
            with self.assertRaises(ValueError): metrics(array("f", a), array("f", b))

    def test_bitwise_identity(self):
        a = array("f", [0.5, -0.125])
        self.assertTrue(metrics(a, a)["bit_identical"])

    def test_signed_zero_not_bit_identical(self):
        m = metrics(array("f", [0.0]), array("f", [-0.0]))
        self.assertFalse(m["bit_identical"])
        self.assertEqual(m["max_abs"], 0)

    def test_reject_truncated_and_extra_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "capture.f32"
            for n in (FRAMES - 1, FRAMES + 1):
                p.write_bytes(array("f", [0]*n).tobytes())
                with self.assertRaises(ValueError): read_samples(p, 1)

    def test_reject_nan_and_infinity_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "capture.f32"
            for bad in (math.nan, math.inf, -math.inf):
                a = array("f", [0]*FRAMES); a[0] = bad; p.write_bytes(a.tobytes())
                with self.assertRaises(ValueError): read_samples(p, 1)

    def test_exact_valid_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "capture.f32"; a = array("f", [0.25]*FRAMES); p.write_bytes(a.tobytes())
            self.assertEqual(read_samples(p, 1), a)


if __name__ == "__main__":
    unittest.main()
