import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "dx7_slice_cases", ROOT / "tools/modules/dx7_slice_cases.py"
)
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class DX7SliceCasesTest(unittest.TestCase):
    def test_manifest_is_bounded_and_valid(self):
        self.assertTrue(module.validate_cases())
        self.assertEqual(len(module.CASES), 3)

    def test_slice_progresses_carrier_pair_envelope(self):
        self.assertEqual(
            [c["operators"] for c in module.CASES],
            [
                {"carrier": 1, "modulators": 0},
                {"carrier": 1, "modulators": 1},
                {"carrier": 1, "modulators": 1},
            ],
        )


if __name__ == "__main__":
    unittest.main()
