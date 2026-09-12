// Offline actual-Faust runner. No audio device, clipping, normalization or FX.
// Minimal generated-code architecture, deliberately independent of any host.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#define FAUSTFLOAT float
struct dsp { virtual ~dsp() = default; };
struct Meta { void declare(const char*, const char*) {} };
struct UI {
    struct Zone { float* p; float lo, hi, def; bool boolean, writable; };
    std::map<std::string, Zone> zones;
    void openTabBox(const char*) {} void openHorizontalBox(const char*) {}
    void openVerticalBox(const char*) {} void closeBox() {}
    void declare(float*, const char*, const char*) {}
    void add(const char* n,float* p,float lo,float hi,bool b=false,bool w=true) {
        if (!zones.emplace(n, Zone{p,lo,hi,*p,b,w}).second)
            throw std::runtime_error("duplicate control");
    }
    void addButton(const char* n,float* p) { add(n,p,0,1,true); }
    void addCheckButton(const char* n,float* p) { add(n,p,0,1,true); }
    void addVerticalSlider(const char* n,float* p,float,float l,float h,float) { add(n,p,l,h); }
    void addHorizontalSlider(const char* n,float* p,float,float l,float h,float) { add(n,p,l,h); }
    void addNumEntry(const char* n,float* p,float,float l,float h,float) { add(n,p,l,h); }
    void addVerticalBargraph(const char* n,float* p,float l,float h) { add(n,p,l,h,false,false); }
    void addHorizontalBargraph(const char* n,float* p,float l,float h) { add(n,p,l,h,false,false); }
    void validate(const std::string& name,float x) const {
        const auto i=zones.find(name);
        if (i==zones.end() || !i->second.writable || !std::isfinite(x)
            || x<i->second.lo || x>i->second.hi || (i->second.boolean && x!=0 && x!=1))
            throw std::runtime_error("invalid control: "+name);
    }
    void set(const std::string& name,float x) { validate(name,x); *zones.at(name).p=x; }
};
#include "generated.hpp"

long long integer(const std::string& s) {
    size_t used=0; auto x=std::stoll(s,&used);
    if (used!=s.size()) throw std::runtime_error("invalid integer"); return x;
}
float real(const std::string& s) {
    size_t used=0; float x=std::stof(s,&used);
    if (used!=s.size() || !std::isfinite(x)) throw std::runtime_error("invalid float"); return x;
}
struct Event { int frame; std::string name; float value; };
int main(int argc,char** argv) { try {
    const uint32_t endian=1;
    if (*reinterpret_cast<const char*>(&endian)!=1) throw std::runtime_error("requires little endian");
    auto module=std::make_unique<ModuleDSP>();
    if (argc==2 && std::string(argv[1])=="--controls") {
        module->init(48000); UI ui; module->buildUserInterface(&ui);
        std::cout<<std::setprecision(9);
        std::cout<<"io\t"<<module->getNumInputs()<<"\t"<<module->getNumOutputs()<<"\n";
        for (const auto& [n,z]:ui.zones)
            std::cout<<n<<"\t"<<z.lo<<"\t"<<z.hi<<"\t"<<z.def<<"\t"<<z.boolean<<"\n";
        return 0;
    }
    if (argc!=7 && argc!=8) throw std::runtime_error("usage: runner SCORE OUT.f32 RATE BLOCK FRAMES RESERVED_ZERO [INPUT.f32]");
    const auto r=integer(argv[3]), b=integer(argv[4]), total=integer(argv[5]);
    if (r<8000 || r>96000 || b<1 || b>8192 || total<1 || total>r*60 || integer(argv[6])!=0)
        throw std::runtime_error("invalid dimensions");
    const int rate=int(r), block=int(b), frames=int(total);
    module->init(rate); UI ui; module->buildUserInterface(&ui);
    const int ni=module->getNumInputs(), no=module->getNumOutputs();
    // Eight bounded channels allow typed control-lane probes; not voice allocation.
    if (ni<0 || ni>8 || no<1 || no>8 || (ni!=0)!=(argc==8)) throw std::runtime_error("I/O contract");
    std::ifstream score(argv[1]); if (!score) throw std::runtime_error("score open failed");
    std::vector<Event> events; std::set<std::pair<int,std::string>> keys;
    std::string line; int previous=-1;
    while (std::getline(score,line)) {
        if (line.empty() || line[0]=='#') continue;
        std::istringstream in(line); std::string f,n,v,extra;
        if (!(in>>f>>n>>v) || (in>>extra)) throw std::runtime_error("bad score row");
        auto frame=integer(f); float value=real(v);
        if (frame<previous || frame<0 || frame>=frames || events.size()>=100000)
            throw std::runtime_error("event ordering/range");
        ui.validate(n,value);
        if (!keys.emplace(int(frame),n).second) throw std::runtime_error("duplicate event");
        events.push_back({int(frame),n,value}); previous=int(frame);
    }
    std::vector<float> input(size_t(frames)*ni), output(size_t(frames)*no);
    if (ni) {
        std::ifstream f(argv[7],std::ios::binary|std::ios::ate);
        if (!f || f.tellg()!=std::streamoff(input.size()*sizeof(float))) throw std::runtime_error("input size");
        f.seekg(0); f.read(reinterpret_cast<char*>(input.data()),std::streamsize(input.size()*sizeof(float)));
        if (!f || !std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);})) throw std::runtime_error("invalid input");
    }
    std::vector<std::vector<float>> ins(ni,std::vector<float>(block)), outs(no,std::vector<float>(block));
    std::vector<float*> ip(ni),op(no);
    for (int c=0;c<ni;++c) ip[c]=ins[c].data(); for (int c=0;c<no;++c) op[c]=outs[c].data();
    size_t event=0; double peak=0; long long computeNs=0; int calls=0;
    for (int n=0;n<frames;) {
        while (event<events.size() && events[event].frame==n) { ui.set(events[event].name,events[event].value); ++event; }
        int count=std::min(block,frames-n);
        if (event<events.size()) count=std::min(count,events[event].frame-n);
        if (count<=0) throw std::runtime_error("nonprogressing score");
        for (int c=0;c<ni;++c) for (int j=0;j<count;++j) ins[c][j]=input[size_t(n+j)*ni+c];
        const auto start=std::chrono::steady_clock::now();
        module->compute(count,ni?ip.data():nullptr,op.data());
        computeNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count(); ++calls;
        for (int j=0;j<count;++j) for (int c=0;c<no;++c) {
            float x=outs[c][j]; if (!std::isfinite(x)) throw std::runtime_error("nonfinite output");
            peak=std::max(peak,double(std::abs(x))); output[size_t(n+j)*no+c]=x;
        }
        n+=count;
    }
    std::ofstream out(argv[2],std::ios::binary); out.write(reinterpret_cast<const char*>(output.data()),std::streamsize(output.size()*sizeof(float))); out.close();
    if (!out) throw std::runtime_error("output write failed");
    std::cout<<std::setprecision(12)<<"{\"frames\":"<<frames<<",\"rate\":"<<rate<<",\"channels\":"<<no
        <<",\"block\":"<<block<<",\"compute_calls\":"<<calls<<",\"peak\":"<<peak<<",\"instrumented_compute_ns\":"<<computeNs<<"}\n";
    return 0;
} catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; } }
