"""CLI inclusion regression, separate from all native DSP/equivalence tests."""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import tempfile
import unittest

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('entry_prepare',HERE/'prepare.py')
prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
ENTRY='int main(int argc,char** argv)'
RENAMED='int pr45_combined_cli_main(int argc,char** argv)'

class EntryPointTests(unittest.TestCase):
    def test_only_cli_symbol_changes(self):
        source='#include "previous.generated.inc"\nnamespace combined { struct Scene {}; }\n'+ENTRY+'{return 0;}\n'
        patched=prepare.device_include(source)
        self.assertEqual(patched.replace(RENAMED,ENTRY),source)
        self.assertEqual(patched.count(RENAMED),1)
    def test_missing_entry_rejected(self):
        with self.assertRaises(AssertionError):prepare.device_include('struct Scene {};')
    def test_ambiguous_entry_rejected(self):
        with self.assertRaises(AssertionError):prepare.device_include(ENTRY+'{}\n'+ENTRY+'{}')
    def test_unchanged_generation_preserves_mtime(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'same.inc';path.write_text('same')
            before=path.stat().st_mtime_ns
            prepare.write_text_if_changed(path,'same')
            self.assertEqual(path.stat().st_mtime_ns,before)
            prepare.write_text_if_changed(path,'changed')
            self.assertEqual(path.read_text(),'changed')
    def test_old_macro_collision_reproduced_and_explicit_entry_compiles(self):
        compiler=shutil.which('clang++')
        self.assertIsNotNone(compiler,'native closeout requires clang++')
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            (root/'previous.generated.inc').write_text('#define main inherited_fixture_main\nint main(){return 0;}\n#undef main\n')
            native='#include "previous.generated.inc"\n'+ENTRY+'{return argc==0;}\n'
            tail=ENTRY+'{return pr45_combined_cli_main(argc,argv);}\n'
            (root/'broken.cpp').write_text('#define main pr45_combined_cli_main\n'+native+'#undef main\n'+tail)
            broken=subprocess.run([compiler,'-fsyntax-only',str(root/'broken.cpp')],capture_output=True,text=True,timeout=20)
            self.assertNotEqual(broken.returncode,0)
            self.assertIn('redefinition of',broken.stderr)
            (root/'fixed.cpp').write_text(prepare.device_include(native)+tail)
            fixed=subprocess.run([compiler,'-fsyntax-only',str(root/'fixed.cpp')],capture_output=True,text=True,timeout=20)
            self.assertEqual(fixed.returncode,0,fixed.stderr)

if __name__=='__main__':unittest.main()
