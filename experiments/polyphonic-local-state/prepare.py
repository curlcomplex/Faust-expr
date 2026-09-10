#!/usr/bin/env python3
"""Additive adapter over PR42; explicit host API and nonaliasing fixes are saved."""
from pathlib import Path
import hashlib, importlib.util, json, sys
HERE=Path(__file__).resolve().parent
NEXT=HERE.parent/'persistent-state-next'
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def replace(s,a,b):
    assert s.count(a)==1,a
    return s.replace(a,b)
def main():
    load('prior_prepare',NEXT/'prepare.py').main()
    source=(NEXT/'main.generated.cpp').read_text()
    source=replace(source,'int main(int argc,char**argv)','int persistent_state_previous_main(int argc,char**argv)')
    source=source.replace('#include "live.h"','#include "../persistent-state-next/live.h"')
    anchor='    void gain(float g){auto* z=uis[0]->getParamZone("m0_gain");'
    source=replace(source,anchor,'    void parameter(const char* name,float value){auto* z=uis[0]->getParamZone(name);require(z,"independent LLVM parameter missing");*z=value;}\n'+anchor)
    (HERE/'previous.generated.inc').write_text(source)
    (HERE/'api.h').write_bytes((NEXT/'api.h').read_bytes())
    (HERE/'generate.py').write_bytes((NEXT/'generate.py').read_bytes())
    # Faust's decorator deliberately has no getDSP accessor. Record our own
    # already-owned wrappers by public gate-zone address, outside processing.
    # This registry is test instrumentation, not the audio ownership mechanism.
    native=(HERE/'main.cpp').read_text()
    native=replace(native,'std::list<GUI*> GUI::fGuiList;','// GUI::fGuiList is defined by the unchanged product FaustGraphRenderer.cpp.')
    native=replace(native,'GUI::ztimedmap GUI::gTimedZoneMap;','ztimedmap GUI::gTimedZoneMap;')
    native=replace(native,'class Voice final:public ::dsp {','class Voice final:public ::dsp {\n    inline static std::map<float*,Voice*> registry;')
    native=replace(native,'audio(engine.planes){reset();}','audio(engine.planes){reset();registry[&values[2]]=this;}\n    ~Voice() override{registry.erase(&values[2]);}\n    static Voice* lookup(float* gate){return registry.at(gate);}')
    native=replace(native,'auto* d=dynamic_cast<Voice*>(v->getDSP());','auto* d=Voice::lookup(v->getParamZone("gate"));')
    # Stock Faust does not promise in-place processing without -inpl. Keep
    # the shared effect's inputs and outputs distinct in every compared arm.
    native=replace(native,'effect->compute(n,outputs,outputs);clock+=n;','float* effected[]={temp.getWritePointer(0),temp.getWritePointer(1)};effect->compute(n,outputs,effected);for(int c=0;c<2;++c)std::copy_n(effected[c],n,outputs[c]);clock+=n;')
    import repair
    native=repair.apply(native)
    # Keep the PR45 DSP/allocator/repairs intact; replace only the online harness.
    native=replace(native,'namespace combined {','#include "job_probe.h"\nnamespace combined {')
    native=replace(native,'std::uint64_t thread=0;};','std::uint64_t thread=0;job_probe::Timing timing;};')
    old='void process(ProcessContext& pc) override{for(int i=first;i<last;++i)renderSlot(slots[i],int(pc.numSamples));}'
    new='void process(ProcessContext& pc) override{if(!job_probe::enabled){for(int i=first;i<last;++i)renderSlot(slots[i],int(pc.numSamples));return;}auto& t=slots[first].timing;t.begin=now();auto cpu=job_probe::cpuUs();for(int i=first;i<last;++i)renderSlot(slots[i],int(pc.numSamples));t.cpu=job_probe::cpuUs()-cpu;t.end=now();}'
    native=replace(native,old,new)
    start=native.index('static void liveTest(')
    end=native.index('} // namespace combined',start)
    native=native[:start]+'#include "handoff.h"\n'+native[end:]
    (HERE/'main.native.cpp').write_text(native)
    pins={'parent':'4c0dc25026e767ec74058e0fef8f4a23a28d8b5a','previous_cpp_sha256':hashlib.sha256((NEXT/'main.generated.cpp').read_bytes()).hexdigest(),
          'adapter_cpp_sha256':hashlib.sha256(source.encode()).hexdigest(),'generator_sha256':hashlib.sha256((HERE/'generate.py').read_bytes()).hexdigest(),
          'authored_main_sha256':hashlib.sha256((HERE/'main.cpp').read_bytes()).hexdigest(),'executed_main_sha256':hashlib.sha256(native.encode()).hexdigest()}
    (HERE/'adapter.json').write_text(json.dumps(pins,indent=2)+'\n')
    print('PR42_ADAPTER_OK',json.dumps(pins),flush=True)
if __name__=='__main__':main()
