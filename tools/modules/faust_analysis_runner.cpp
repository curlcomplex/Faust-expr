#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

struct dsp { virtual ~dsp() = default; };
struct Meta { void declare(const char*, const char*) {} };
struct UI {
    void openTabBox(const char*) {} void openHorizontalBox(const char*) {}
    void openVerticalBox(const char*) {} void closeBox() {}
    void declare(float*, const char*, const char*) {}
    void addButton(const char*,float*) {} void addCheckButton(const char*,float*) {}
    void addVerticalSlider(const char*,float*,float,float,float,float) {}
    void addHorizontalSlider(const char*,float*,float,float,float,float) {}
    void addNumEntry(const char*,float*,float,float,float,float) {}
    void addVerticalBargraph(const char*,float*,float,float) {}
    void addHorizontalBargraph(const char*,float*,float,float) {}
};
#include "analysis_generated.hpp"

static long long integer(const char* s) {
    size_t used=0; auto x=std::stoll(s,&used);
    if (used!=std::string(s).size()) throw std::runtime_error("invalid integer");
    return x;
}
int main(int argc,char** argv) { try {
    if (argc!=5) throw std::runtime_error("usage: runner INPUT.f32 OUTPUT.f32 RATE FRAMES");
    const int rate=int(integer(argv[3])); const long long frames=integer(argv[4]);
    if (rate<8000 || rate>192000 || frames<1 || frames>1LL*rate*600) throw std::runtime_error("invalid dimensions");
    std::ifstream in(argv[1],std::ios::binary); if(!in) throw std::runtime_error("input open failed");
    std::vector<float> x(size_t(frames)); in.read(reinterpret_cast<char*>(x.data()),std::streamsize(x.size()*sizeof(float)));
    if (in.gcount()!=std::streamsize(x.size()*sizeof(float))) throw std::runtime_error("input size mismatch");
    char extra; if(in.read(&extra,1)) throw std::runtime_error("input has trailing data");
    for(float v:x) if(!std::isfinite(v)) throw std::runtime_error("nonfinite input");
    ModuleDSP module; module.init(rate);
    const int outputs=module.getNumOutputs(); if(module.getNumInputs()!=1 || outputs!=6) throw std::runtime_error("analysis DSP IO contract changed");
    std::vector<float> out(size_t(frames)*size_t(outputs));
    const int block=256; std::vector<float> scratch(size_t(block)*size_t(outputs));
    for(long long base=0;base<frames;base+=block) {
        int n=int(std::min<long long>(block,frames-base)); float* inputs[1]={x.data()+base};
        float* ptrs[6]; for(int c=0;c<outputs;c++) ptrs[c]=scratch.data()+size_t(c)*block;
        module.compute(n,inputs,ptrs);
        for(int c=0;c<outputs;c++) for(int i=0;i<n;i++) out[size_t(base+i)*outputs+c]=ptrs[c][i];
    }
    std::ofstream f(argv[2],std::ios::binary); if(!f) throw std::runtime_error("output open failed");
    f.write(reinterpret_cast<const char*>(out.data()),std::streamsize(out.size()*sizeof(float))); if(!f) throw std::runtime_error("output write failed");
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 2; } }
