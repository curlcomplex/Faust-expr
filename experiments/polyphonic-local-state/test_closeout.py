"""Regression tests for final harness ownership and single-renderer reuse.
These are source/adapter tests; native audio qualification is separate.
"""
import importlib.util
from pathlib import Path
import tempfile
import unittest

HERE=Path(__file__).resolve().parent

class CloseoutContracts(unittest.TestCase):
    def test_device_reuses_qualified_scene(self):
        text=(HERE/'device_main.cpp').read_text()
        self.assertIn('using DeviceScene = Scene;',text)
        self.assertNotIn('struct DeviceScene {',text)
        self.assertIn('stateMode(true,false,false,code->compute)',text)

    def test_device_callback_is_detached_before_timeout_throw(self):
        text=(HERE/'device_main.cpp').read_text()
        self.assertIn('detach.remove();require(finished,"device test timeout")',text)
        self.assertIn('~Detach(){remove();}',text)

    def test_device_preserves_full_evidence(self):
        text=(HERE/'device_main.cpp').read_text()
        for name in ('device-callbacks.tsv','device-counterfactual-A.f32','device-reference.f32','device-internal.f32'):
            self.assertIn(name,text)
        self.assertIn('combined::bufferContract(e,out)',text)
        self.assertIn('combined::stockControlContract(e,out)',text)

    def test_caller_policy_is_explicitly_preserved(self):
        spec=importlib.util.spec_from_file_location('patch_under_test',HERE/'patch_worker_context.py')
        m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        self.assertEqual(m.NEW.count('if (! preserveCreator) tryToUpgradeCurrentThreadToRealtime (creatorOptions);'),2)
        self.assertIn('tryToUpgradeCurrentThreadToRealtime (workerOptions);',m.NEW)
        self.assertIn('ready.wait();',m.NEW)

    def test_worker_patch_rejects_unknown_source(self):
        spec=importlib.util.spec_from_file_location('patch_under_test',HERE/'patch_worker_context.py')
        m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'pool.cpp';path.write_text('// unknown pool')
            with self.assertRaises(AssertionError):m.main(path)
            self.assertEqual(path.read_text(),'// unknown pool')

if __name__=='__main__':unittest.main()
