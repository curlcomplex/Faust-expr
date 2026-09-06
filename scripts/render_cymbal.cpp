// Standalone, sample-exact test driver for Faust-generated code. No host code.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#define FAUSTFLOAT float
// Minimal implementations of the public Faust architecture interface.
struct dsp { virtual ~dsp() = default; };
struct Meta { void declare(const char*, const char*) {} };
struct UI {
    struct Control { float* zone; double lo, hi; bool writable; };
    std::map<std::string, Control> controls;
    void openTabBox(const char*) {} void openHorizontalBox(const char*) {}
    void openVerticalBox(const char*) {} void closeBox() {}
    void declare(float*, const char*, const char*) {}
    void add(const char* label,float* zone,double lo,double hi,bool write=true) {
        if (!controls.emplace(label,Control{zone,lo,hi,write}).second)
            throw std::runtime_error("Duplicate control name");
    }
    void addButton(const char* l,float* z){ add(l,z,0,1); }
    void addCheckButton(const char* l,float* z){ add(l,z,0,1); }
    void addVerticalSlider(const char*l,float*z,float,float a,float b,float){add(l,z,a,b);}
    void addHorizontalSlider(const char*l,float*z,float,float a,float b,float){add(l,z,a,b);}
    void addNumEntry(const char*l,float*z,float,float a,float b,float){add(l,z,a,b);}
    void addHorizontalBargraph(const char*l,float*z,float a,float b){add(l,z,a,b,false);}
    void addVerticalBargraph(const char*l,float*z,float a,float b){add(l,z,a,b,false);}
    void set(const std::string& assignment) {
        const auto pos=assignment.find('=');
        if(pos==std::string::npos) throw std::runtime_error("Expected name=value");
        const auto name=assignment.substr(0,pos);
        size_t used=0; const auto tail=assignment.substr(pos+1);
        const double value=std::stod(tail,&used);
        const auto it=controls.find(name);
        if(it==controls.end() || !it->second.writable) throw std::runtime_error("Unknown/wrong control: "+name);
        if(used!=tail.size() || !std::isfinite(value) || value<it->second.lo || value>it->second.hi)
            throw std::runtime_error("Invalid control value: "+assignment);
        if((name=="beater" || name=="hammer_pattern" || name=="proportional_thickness") && std::floor(value)!=value)
            throw std::runtime_error("Integer control required: "+assignment);
        *it->second.zone=static_cast<float>(value);
    }
};
#include "cymbal.hpp"
struct Event { std::int64_t frame; std::string assignment; };
int main(int argc,char**argv) {
    try {
        if(argc<2) throw std::runtime_error("usage: render_cymbal OUT.f32 [--rate N --seconds T --block N --set key=value --event seconds:key=value]");
        int rate=48000,block=128; double duration=5;
        std::vector<std::string> initial,events;
        for(int i=2;i<argc;i+=2) {
            if(i+1>=argc) throw std::runtime_error("Missing argument value");
            const std::string key=argv[i],value=argv[i+1];
            if(key=="--rate")rate=std::stoi(value); else if(key=="--block")block=std::stoi(value);
            else if(key=="--seconds")duration=std::stod(value); else if(key=="--set")initial.push_back(value);
            else if(key=="--event")events.push_back(value); else throw std::runtime_error("Unknown option: "+key);
        }
        if(rate<22050 || rate>192000 || block<1 || block>4096 || !std::isfinite(duration) || duration<=0 || duration>60)
            throw std::runtime_error("Unsafe render dimensions");
        const auto frames=static_cast<std::int64_t>(std::llround(rate*duration));
        auto synth=std::make_unique<CymbalDSP>(); synth->init(rate); UI ui; synth->buildUserInterface(&ui);
        if(synth->getNumInputs()!=0 || synth->getNumOutputs()!=2) throw std::runtime_error("Wrong channel contract");
        for(const auto& v:initial)ui.set(v);
        std::vector<Event> schedule;
        for(const auto& e:events) {
            const auto split=e.find(':'); if(split==std::string::npos)throw std::runtime_error("Event requires time:key=value");
            const double t=std::stod(e.substr(0,split));
            if(!std::isfinite(t) || t<0 || t>=duration)throw std::runtime_error("Event outside render");
            schedule.push_back({static_cast<std::int64_t>(std::llround(t*rate)),e.substr(split+1)});
        }
        std::stable_sort(schedule.begin(),schedule.end(),[](const auto&a,const auto&b){return a.frame<b.frame;});
        // Validate controls before rendering, then restore initial state.
        for(const auto&e:schedule)ui.set(e.assignment);
        synth->instanceResetUserInterface(); for(const auto&v:initial)ui.set(v);
        std::vector<float> left(block),right(block),output(static_cast<size_t>(frames)*2);
        std::int64_t frame=0; size_t next=0; double peak=0,maxEnergy=0;
        const auto start=std::chrono::steady_clock::now();
        while(frame<frames) {
            while(next<schedule.size() && schedule[next].frame<=frame)ui.set(schedule[next++].assignment);
            auto count=std::min<std::int64_t>(block,frames-frame);
            if(next<schedule.size())count=std::min(count,schedule[next].frame-frame);
            if(count<=0)throw std::runtime_error("Invalid event schedule");
            float* channels[]={left.data(),right.data()}; synth->compute(static_cast<int>(count),nullptr,channels);
            for(int i=0;i<count;i++) for(int ch=0;ch<2;ch++) {
                const float v=channels[ch][i]; if(!std::isfinite(v))throw std::runtime_error("Non-finite output");
                peak=std::max(peak,std::abs(static_cast<double>(v)));output[static_cast<size_t>(frame+i)*2+ch]=v;
            }
            const double energy=*ui.controls.at("energy").zone;
            if(!std::isfinite(energy) || energy<0 || energy>1e4)throw std::runtime_error("Invalid/unbounded internal energy");
            maxEnergy=std::max(maxEnergy,energy); frame+=count;
        }
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        const std::uint16_t endian=1;
        if(*reinterpret_cast<const unsigned char*>(&endian)!=1)throw std::runtime_error("Raw fixture requires little endian");
        std::ofstream out(argv[1],std::ios::binary); if(!out)throw std::runtime_error("Cannot open output");
        out.write(reinterpret_cast<const char*>(output.data()),output.size()*sizeof(float));out.close();
        if(!out)throw std::runtime_error("Audio write failed");
        std::cout<<std::setprecision(12)<<"{\"rate\":"<<rate<<",\"frames\":"<<frames
                 <<",\"channels\":2,\"peak\":"<<peak<<",\"max_energy_sampled\":"<<maxEnergy
                 <<",\"final_energy\":"<<*ui.controls.at("energy").zone<<",\"render_seconds\":"<<seconds
                 <<",\"realtime_ratio\":"<<seconds/duration<<"}\n";
    } catch(const std::exception&e) {std::cerr<<e.what()<<'\n';return 1;}
}
