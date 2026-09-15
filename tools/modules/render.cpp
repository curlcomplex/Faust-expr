// Offline actual-Faust runner. No audio device, clipping, normalization or FX.
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
    struct Zone { float* p; float lo, hi, def; bool boolean, writable; std::string label,path; std::map<std::string,std::string> meta; };
    std::map<std::string, Zone> zones; std::vector<std::string> groups; std::map<float*,std::map<std::string,std::string>> pending;
    static std::string esc(const std::string& s) { std::string o; for(char c:s){if(c=='\\'||c=='"')o+='\\';o+=c;} return o; }
    std::string path(const char* label) const { std::string p; for(auto& g:groups)p+="/"+g; return p+"/"+label; }
    void openTabBox(const char* n) { groups.emplace_back(n); } void openHorizontalBox(const char* n) { groups.emplace_back(n); }
    void openVerticalBox(const char* n) { groups.emplace_back(n); } void closeBox() { if(groups.empty()) throw std::runtime_error("UI group underflow"); groups.pop_back(); }
    void declare(float* p,const char* k,const char* v) { pending[p][k]=v; }
    void add(const char* n,float* p,float lo,float hi,bool b=false,bool w=true) {
        auto meta=pending[p]; pending.erase(p); const auto full=path(n); std::string key=full;
        if (auto i=meta.find("probe"); !w && i!=meta.end()) key="probe:"+i->second;
        if (!zones.emplace(key, Zone{p,lo,hi,*p,b,w,n,full,std::move(meta)}).second) throw std::runtime_error("duplicate UI identity: "+key);
    }
    void addButton(const char* n,float* p){add(n,p,0,1,true);} void addCheckButton(const char* n,float* p){add(n,p,0,1,true);}
    void addVerticalSlider(const char* n,float* p,float,float l,float h,float){add(n,p,l,h);} void addHorizontalSlider(const char* n,float* p,float,float l,float h,float){add(n,p,l,h);}
    void addNumEntry(const char* n,float* p,float,float l,float h,float){add(n,p,l,h);} void addVerticalBargraph(const char* n,float* p,float l,float h){add(n,p,l,h,false,false);}
    void addHorizontalBargraph(const char* n,float* p,float l,float h){add(n,p,l,h,false,false);}
    const Zone* writable(const std::string& name) const { const Zone* found=nullptr; for(auto& [k,z]:zones) if(z.writable&&(k==name||z.label==name||z.path==name)){if(found)throw std::runtime_error("ambiguous control: "+name);found=&z;} return found; }
    Zone* writable(const std::string& name) { return const_cast<Zone*>(std::as_const(*this).writable(name)); }
    void validate(const std::string& name,float x) const { auto z=writable(name); if(!z||!std::isfinite(x)||x<z->lo||x>z->hi||(z->boolean&&x!=0&&x!=1))throw std::runtime_error("invalid control: "+name); }
    void set(const std::string& name,float x){validate(name,x);*writable(name)->p=x;}
    void printControls() const { for(auto& [k,z]:zones) if(z.writable) std::cout<<z.label<<"\t"<<z.lo<<"\t"<<z.hi<<"\t"<<z.def<<"\t"<<z.boolean<<"\n"; }
    void printUI() const { std::cout<<"["; bool first=true; for(auto& [k,z]:zones){if(!first)std::cout<<",";first=false;std::cout<<"{\"id\":\""<<esc(k)<<"\",\"label\":\""<<esc(z.label)<<"\",\"path\":\""<<esc(z.path)<<"\",\"writable\":"<<(z.writable?"true":"false")<<",\"metadata\":{";bool fm=true;for(auto&[mk,mv]:z.meta){if(!fm)std::cout<<",";fm=false;std::cout<<"\""<<esc(mk)<<"\":\""<<esc(mv)<<"\"";}std::cout<<"}}";}std::cout<<"]\n"; }
    std::vector<std::pair<std::string,const Zone*>> probes() const { std::vector<std::pair<std::string,const Zone*>> out; for(auto&[k,z]:zones)if(!z.writable&&z.meta.count("probe"))out.push_back({k,&z});return out; }
};
#include "generated.hpp"
long long integer(const std::string&s){size_t u=0;auto x=std::stoll(s,&u);if(u!=s.size())throw std::runtime_error("invalid integer");return x;}
float real(const std::string&s){size_t u=0;float x=std::stof(s,&u);if(u!=s.size()||!std::isfinite(x))throw std::runtime_error("invalid float");return x;}
struct Event{int frame;std::string name;float value;};
int main(int argc,char**argv){try{
 const uint32_t endian=1;if(*reinterpret_cast<const char*>(&endian)!=1)throw std::runtime_error("requires little endian");auto module=std::make_unique<ModuleDSP>();
 if(argc==2&&(std::string(argv[1])=="--controls"||std::string(argv[1])=="--ui")){module->init(48000);UI ui;module->buildUserInterface(&ui);if(std::string(argv[1])=="--ui")ui.printUI();else{std::cout<<std::setprecision(9)<<"io\t"<<module->getNumInputs()<<"\t"<<module->getNumOutputs()<<"\n";ui.printControls();}return 0;}
 if(argc<7||argc>10)throw std::runtime_error("usage: runner SCORE OUT.f32 RATE BLOCK FRAMES RESERVED_ZERO [INPUT.f32] [--probes FILE.tsv] [--probe-stride N]");
 const auto r=integer(argv[3]),b=integer(argv[4]),total=integer(argv[5]);if(r<8000||r>96000||b<1||b>8192||total<1||total>r*60||integer(argv[6])!=0)throw std::runtime_error("invalid dimensions");const int rate=int(r),block=int(b),frames=int(total);
 module->init(rate);UI ui;module->buildUserInterface(&ui);const int ni=module->getNumInputs(),no=module->getNumOutputs();if(ni<0||ni>8||no<1||no>8)throw std::runtime_error("I/O contract");
 int arg=7;std::string inputPath,probePath;int stride=1;if(ni){if(arg>=argc||std::string(argv[arg]).rfind("--",0)==0)throw std::runtime_error("input required");inputPath=argv[arg++];}
 while(arg<argc){std::string opt=argv[arg++];if(opt=="--probes"&&arg<argc)probePath=argv[arg++];else if(opt=="--probe-stride"&&arg<argc){auto s=integer(argv[arg++]);if(s<1||s>frames)throw std::runtime_error("invalid probe stride");stride=int(s);}else throw std::runtime_error("bad option");}
 auto probes=ui.probes();if(!probePath.empty()&&probes.empty())throw std::runtime_error("no [probe:ID] zones");std::ofstream probeOut;if(!probePath.empty()){probeOut.open(probePath);if(!probeOut)throw std::runtime_error("probe output open failed");probeOut<<"frame\ttime_seconds";for(auto&[k,z]:probes)probeOut<<"\t"<<k;probeOut<<"\n";}
 std::ifstream score(argv[1]);if(!score)throw std::runtime_error("score open failed");std::vector<Event>events;std::set<std::pair<int,std::string>>keys;std::string line;int previous=-1;while(std::getline(score,line)){if(line.empty()||line[0]=='#')continue;std::istringstream in(line);std::string f,n,v,extra;if(!(in>>f>>n>>v)||(in>>extra))throw std::runtime_error("bad score row");auto frame=integer(f);float value=real(v);if(frame<previous||frame<0||frame>=frames||events.size()>=100000)throw std::runtime_error("event ordering/range");ui.validate(n,value);if(!keys.emplace(int(frame),n).second)throw std::runtime_error("duplicate event");events.push_back({int(frame),n,value});previous=int(frame);}
 std::vector<float>input(size_t(frames)*ni),output(size_t(frames)*no);if(ni){std::ifstream f(inputPath,std::ios::binary|std::ios::ate);if(!f||f.tellg()!=std::streamoff(input.size()*sizeof(float)))throw std::runtime_error("input size");f.seekg(0);f.read(reinterpret_cast<char*>(input.data()),std::streamsize(input.size()*sizeof(float)));if(!f||!std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);}))throw std::runtime_error("invalid input");}
 std::vector<std::vector<float>>ins(ni,std::vector<float>(block)),outs(no,std::vector<float>(block));std::vector<float*>ip(ni),op(no);for(int c=0;c<ni;++c)ip[c]=ins[c].data();for(int c=0;c<no;++c)op[c]=outs[c].data();size_t event=0;double peak=0;long long computeNs=0;int calls=0;
 for(int n=0;n<frames;){while(event<events.size()&&events[event].frame==n){ui.set(events[event].name,events[event].value);++event;}int count=std::min(block,frames-n);if(event<events.size())count=std::min(count,events[event].frame-n);if(!probePath.empty())count=std::min(count,1);if(count<=0)throw std::runtime_error("nonprogressing score");for(int c=0;c<ni;++c)for(int j=0;j<count;++j)ins[c][j]=input[size_t(n+j)*ni+c];auto start=std::chrono::steady_clock::now();module->compute(count,ni?ip.data():nullptr,op.data());computeNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();++calls;for(int j=0;j<count;++j)for(int c=0;c<no;++c){float x=outs[c][j];if(!std::isfinite(x))throw std::runtime_error("nonfinite output");peak=std::max(peak,double(std::abs(x)));output[size_t(n+j)*no+c]=x;}if(!probePath.empty()&&n%stride==0){probeOut<<n<<"\t"<<std::setprecision(12)<<double(n)/rate;for(auto&[k,z]:probes){if(!std::isfinite(*z->p))throw std::runtime_error("nonfinite probe");probeOut<<"\t"<<*z->p;}probeOut<<"\n";}n+=count;}
 std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<const char*>(output.data()),std::streamsize(output.size()*sizeof(float)));out.close();if(!out)throw std::runtime_error("output write failed");if(probeOut){probeOut.close();if(!probeOut)throw std::runtime_error("probe output write failed");}std::cout<<std::setprecision(12)<<"{\"frames\":"<<frames<<",\"rate\":"<<rate<<",\"channels\":"<<no<<",\"block\":"<<block<<",\"compute_calls\":"<<calls<<",\"peak\":"<<peak<<",\"instrumented_compute_ns\":"<<computeNs<<",\"diagnostic_probe_capture\":"<<(!probePath.empty()?"true":"false")<<"}\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
