#!/usr/bin/env python3
"""Apply only an opt-in export hook to the exact pinned upstream compiler.

No partition rule, fusion budget, generated C++ body or LLVM lowering is patched.
"""
from pathlib import Path
import argparse
import hashlib
import shutil

EXPECTED = "5e8bf3713f14d2cc2f28bab6b748124042e98f67239f9515a3e9543b98f0e46f"
INCLUDE = '#include "global.hh"\n'
ANCHOR = '    if (gGlobal->gDrawSuperNodes) {\n'
HOOK = '''    // Opt-in research export: use the final upstream partition and signal
    // definitions, never infer an executable plan from DOT or generated C++.
    if (const char* p = std::getenv("FAUST_PLAN_LLVM_EXPORT")) {
        PlanLLVMExporter(fSN).write(L, fC->fClass->inputs(), nouts, p);
    }
'''

def patch(root: Path) -> None:
    source = root / "compiler/generator/compile_scal.cpp"
    original = source.read_bytes()
    if hashlib.sha256(original).hexdigest() != EXPECTED:
        raise ValueError("upstream compile_scal.cpp does not match the pinned revision")
    text = original.decode()
    if text.count(INCLUDE) != 1 or text.count(ANCHOR) != 1:
        raise ValueError("export hook anchors are not unique")
    text = text.replace(INCLUDE, INCLUDE + '#include "plan_llvm_export.hh"\n')
    text = text.replace(ANCHOR, HOOK + ANCHOR)
    shutil.copyfile(Path(__file__).with_name("plan_llvm_export.hh"), source.with_name("plan_llvm_export.hh"))
    source.write_text(text)
    print("export_only_patch_sha256=" + hashlib.sha256(source.read_bytes()).hexdigest())

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("faust_source", type=Path)
    patch(p.parse_args().faust_source)
