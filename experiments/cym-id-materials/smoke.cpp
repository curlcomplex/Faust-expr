// Compile/finite-output fixture only. Does not load fitted cymbal data.
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <faust/dsp/dsp.h>
#include <faust/gui/meta.h>
#include <faust/gui/MapUI.h>
#include "probe.hpp"
int main() {
  try {
    for (int lock=0;lock<2;++lock) for(int material=0;material<12;++material) {
      MaterialProbe synth; synth.init(48000); MapUI ui;
      synth.buildUserInterface(&ui);
      auto set=[&](const char* name,float v) {
        auto* p=ui.getParamZone(name);
        if(!p) throw std::runtime_error(std::string("Missing control: ")+name);
        *p=v;
      };
      set("material_a",material);set("material_b",material);
      set("pitch_lock",lock);set("gate",1);
      std::vector<float> l(128),r(128);float* out[]={l.data(),r.data()};
      synth.compute(1,nullptr,out);set("gate",0);
      double peak=std::abs(l[0]);
      for(int b=0;b<375;++b) {
        if(b==120)set("grip",1);
        if(b==240)set("grip",0);
        synth.compute(128,nullptr,out);
        for(int i=0;i<128;++i) {
          if(!std::isfinite(l[i])||!std::isfinite(r[i]))throw std::runtime_error("Nonfinite fixture");
          peak=std::max(peak,double(std::abs(l[i])));
        }
      }
      if(peak<=0||peak>1)throw std::runtime_error("Invalid fixture peak");
    }
    std::cout<<"24 single-resonance material/lock fixtures passed; NOT full cymbal validation.\n";
  } catch(const std::exception&e) {std::cerr<<e.what()<<"\n";return 1;}
}
