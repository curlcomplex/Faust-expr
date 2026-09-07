// Generic offline reference driver; actual generated Faust header supplies DSP.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#define FAUSTFLOAT float
struct dsp { virtual ~dsp() = default; };
struct Meta {void declare(const char*,const char*){}};
struct UI {
 struct C {float* z;float lo,hi;bool write;}; std::map<std::string,C> c;
 void openTabBox(const char*){} void openHorizontalBox(const char*){} void openVerticalBox(const char*){} void closeBox(){}
 void declare(float*,const char*,const char*){}
 void add(const char*l,float*z,float lo,float hi,bool w=true){if(!c.emplace(l,C{z,lo,hi,w}).second)throw std::runtime_error("Duplicate parameter");}
 void addButton(const char*l,float*z){add(l,z,0,1);} void addCheckButton(const char*l,float*z){add(l,z,0,1);}
 void addVerticalSlider(const char*l,float*z,float,float a,float b,float){add(l,z,a,b);}
 void addHorizontalSlider(const char*l,float*z,float,float a,float b,float){add(l,z,a,b);}
 void addNumEntry(const char*l,float*z,float,float a,float b,float){add(l,z,a,b);}
 void addVerticalBargraph(const char*l,float*z,float a,float b){add(l,z,a,b,false);}
 void addHorizontalBargraph(const char*l,float*z,float a,float b){add(l,z,a,b,false);}
 void set(std::string s){auto p=s.find('=');if(p==std::string::npos)throw std::runtime_error("Expected key=value");
  auto name=s.substr(0,p);auto v=s.substr(p+1);size_t used=0;float x=std::stof(v,&used);auto i=c.find(name);
  if(i==c.end()||!i->second.write||used!=v.size()||!std::isfinite(x)||x<i->second.lo||x>i->second.hi)throw std::runtime_error("Invalid parameter "+s);
  *i->second.z=x;}
};
#include "cymbal.hpp"
int main(int argc,char**argv){try{
 auto s=std::make_unique<CymbalDSP>(); int rate=48000; s->init(rate);UI ui;s->buildUserInterface(&ui);
 if(argc==2 && std::string(argv[1])=="--controls"){for(auto &[n,c]:ui.c)std::cout<<n<<" "<<*c.z<<" "<<c.lo<<" "<<c.hi<<" "<<c.write<<"\n";return 0;}
 if(argc<3)throw std::runtime_error("usage: renderer OUT.f32 SECONDS [key=value ...]");
 size_t parsed=0;double duration=std::stod(argv[2],&parsed);if(parsed!=std::string(argv[2]).size()||!std::isfinite(duration)||duration<.01||duration>30)throw std::runtime_error("Bad duration");
 for(int i=3;i<argc;++i)ui.set(argv[i]);*ui.c.at("gate").z=0;
 if(s->getNumInputs()!=0||s->getNumOutputs()!=2)throw std::runtime_error("Unexpected channel contract");
 constexpr int block=128;int frames=int(std::llround(duration*rate));
 std::vector<float> l(block),r(block),out(2*frames);float*ch[]={l.data(),r.data()};
 for(int n=0;n<16800;){int k=std::min(block,16800-n);s->compute(k,nullptr,ch);for(int j=0;j<k;++j)for(int c=0;c<2;++c)if(!std::isfinite(ch[c][j])||std::abs(ch[c][j])>1e-8)throw std::runtime_error("Pre-strike activity");n+=k;}
 double peak=0,energyPeak=0,residual=0;auto start=std::chrono::steady_clock::now();
 for(int n=0;n<frames;){int k=n==0?1:std::min(block,frames-n);*ui.c.at("gate").z=(n==0?1:0);s->compute(k,nullptr,ch);
  for(int j=0;j<k;++j)for(int c=0;c<2;++c){float x=ch[c][j];if(!std::isfinite(x))throw std::runtime_error("Nonfinite audio");out[(n+j)*2+c]=x;peak=std::max(peak,double(std::abs(x)));}
  for(auto &[name,c]:ui.c)if(!c.write){if(!std::isfinite(*c.z))throw std::runtime_error("Nonfinite diagnostic");if(name.find("energy")!=std::string::npos)energyPeak=std::max(energyPeak,double(*c.z));if(name.find("residual")!=std::string::npos)residual=std::max(residual,double(*c.z));}
  n+=k;
 }
 std::ofstream f(argv[1],std::ios::binary);f.write(reinterpret_cast<const char*>(out.data()),out.size()*sizeof(float));f.close();if(!f)throw std::runtime_error("Write failed");
 std::cerr<<std::setprecision(12)<<"{\"peak\":"<<peak<<",\"energy_peak_sampled\":"<<energyPeak<<",\"solver_residual_sampled\":"<<residual<<",\"seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<"}\n";
 return 0;}catch(std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
