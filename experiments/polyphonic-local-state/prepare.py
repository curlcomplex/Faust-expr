#!/usr/bin/env python3
"""Additive adapter over PR42. Keep original fixtures, generated state ABI and cost reference."""
from pathlib import Path
import hashlib, importlib.util, json, sys
HERE=Path(__file__).resolve().parent
NEXT=HERE.parent/'persistent-state-next'
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    load('prior_prepare',NEXT/'prepare.py').main()
    source=(NEXT/'main.generated.cpp').read_text()
    # Keep the actual product builder, graph/order/route and library functions.
    old='int main(int argc,char**argv)'
    assert source.count(old)==1
    source=source.replace(old,'int persistent_state_previous_main(int argc,char**argv)')
    source=source.replace('#include "live.h"','#include "../persistent-state-next/live.h"')
    anchor='    void gain(float g){auto* z=uis[0]->getParamZone("m0_gain");'
    assert source.count(anchor)==1
    source=source.replace(anchor,'    void parameter(const char* name,float value){auto* z=uis[0]->getParamZone(name);require(z,"independent LLVM parameter missing");*z=value;}\n'+anchor)
    (HERE/'previous.generated.inc').write_text(source)
    (HERE/'api.h').write_bytes((NEXT/'api.h').read_bytes())
    # Reuse the stricter output-taps-only kernel generator, not the first diagnostic.
    (HERE/'generate.py').write_bytes((NEXT/'generate.py').read_bytes())
    pins={'parent':'4c0dc25026e767ec74058e0fef8f4a23a28d8b5a','previous_cpp_sha256':hashlib.sha256((NEXT/'main.generated.cpp').read_bytes()).hexdigest(),
          'adapter_cpp_sha256':hashlib.sha256(source.encode()).hexdigest(),'generator_sha256':hashlib.sha256((HERE/'generate.py').read_bytes()).hexdigest()}
    (HERE/'adapter.json').write_text(json.dumps(pins,indent=2)+'\n')
    print('PR42_ADAPTER_OK',json.dumps(pins),flush=True)
if __name__=='__main__':main()
