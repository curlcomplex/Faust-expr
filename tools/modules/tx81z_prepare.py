#!/usr/bin/env python3
"""Generate small OPZ lookup tables from hash-verified, unchanged BSD ymfm source.
No audio synthesis is performed here. The Faust functions own all DSP state.
"""
from pathlib import Path
import argparse,hashlib,json,re
PIN = "81aec25ccbb98f4873a255f7551ac4dadac59b4a"
IPP_SHA = "4fb7fe59d4494a19e9f9c7888eb644a63d86e109fdefb27a8117bf6a5f6ab4b9"
def prepare(source: Path, out: Path):
    path=source/"src/ymfm_fm.ipp"
    if hashlib.sha256(path.read_bytes()).hexdigest()!=IPP_SHA:
        raise ValueError("ymfm source hash differs from the reviewed pin")
    text=path.read_text()
    def numbers(name):
        m=re.search(r"\b"+name+r"\s*\[[^;=]*?=\s*\{(.*?)\};",text,re.S)
        if not m: raise ValueError("missing table "+name)
        s=re.sub(r"//[^\n]*", "", m[1])
        return [int(v,0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+",s)]
    sin=numbers("s_sin_table"); power=[(v|0x400)<<2 for v in numbers("s_power_table")]
    inc=[(v>>(4*i))&15 for v in numbers("s_increment_table") for i in range(8)]
    phase=numbers("s_phase_step"); detune=numbers("s_detune_adjustment")
    tables={"sineLog":sin,"power":power,"increment":inc,"keyStep":phase,"detune":detune}
    if [len(x) for x in tables.values()]!=[256,256,512,768,128]:
        raise ValueError("unexpected upstream table dimensions")
    header="// Generated from unchanged ymfm@"+PIN+". BSD-3-Clause.\n// Copyright (c) 2021 Aaron Giles. See ../YMFM-LICENSE.txt.\n"
    result=header
    for name,values in tables.items():
        result+=name+"(i) = (waveform{"+",".join(map(str,values))+"}, int(i)) : rdtable;\n"
    out.parent.mkdir(parents=True,exist_ok=True);out.write_text(result)
    return {"commit":PIN,"source_sha256":IPP_SHA,"tables":{k:len(v) for k,v in tables.items()},
            "generated_sha256":hashlib.sha256(out.read_bytes()).hexdigest()}


def standalone_source(source: Path):
    """Embed the bounded local dependency closure, preserving written expressions.

    Same scoped-environment approach as synth_script_exports, extended here only
    for the known nested OPZ helpers. Standard libraries remain external. Avoid
    Faust -e's 518-MiB textual duplication of the four operator state equations.
    """
    source = source.resolve()
    root = source.parent.parent
    allowed = {p.resolve() for p in (
        root/'v5/opz_core.lib', root/'v5/opz_tables.lib',
        root/'v4/opz_routes.lib', root/'v2/opz_core.lib')}
    embedded = {}
    pattern = re.compile(r'^(\w+)\s*=\s*library\("([^"\n]+)"\);\s*$', re.M)

    def expand(path, stack=()):
        if path in stack:
            raise ValueError('cyclic OPZ source dependency')
        text = path.read_text()
        if path != source:
            text = re.sub(r'^import\("stdfaust.lib"\);\s*$', '', text, flags=re.M)
        def replace(match):
            alias, relative = match.groups()
            dependency = (path.parent/relative).resolve()
            if dependency not in allowed:
                raise ValueError('unexpected OPZ dependency: '+str(dependency))
            embedded[str(dependency.relative_to(root))] = hashlib.sha256(dependency.read_bytes()).hexdigest()
            return alias+'=environment {\n'+expand(dependency,stack+(path,))+'\n};'
        return pattern.sub(replace,text)

    result = expand(source)
    if re.search(r'\blibrary\s*\(',result):
        raise ValueError('unresolved repository library')
    if re.findall(r'\bimport\s*\("([^"\n]+)"\)',result) != ['stdfaust.lib']:
        raise ValueError('unexpected imports in OPZ export')
    license_text=(root/"YMFM-LICENSE.txt").read_text()
    result="// Standalone OPZ research voice. Standard Faust libraries required.\n"+"\n".join("// "+line for line in license_text.splitlines())+"\n"+result
    return result, embedded


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--ymfm",type=Path,required=True);p.add_argument("--out",type=Path,required=True)
    a=p.parse_args();print(json.dumps(prepare(a.ymfm,a.out)))

