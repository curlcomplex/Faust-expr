// Offline, sample-checked driver for the actual Faust-generated cymbal.
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
#include <faust/dsp/dsp.h>
#include <faust/gui/meta.h>
#include <faust/gui/MapUI.h>
#include "cymbal.hpp"

int main(int argc, char** argv) {
 try {
  if(argc<2) throw std::runtime_error("usage: cymbal-render OUTPUT.f32 [--param key=value] [--seconds N] [--rate N] [--sweep key,start,end] [--roll]");
  int rate=48000; double seconds=3; bool roll=false; std::map<std::string,double> params;
  std::string sweep; double begin=0,end=0;
  auto number=[](const std::string& text) { size_t used=0; double v=std::stod(text,&used); if(used!=text.size()||!std::isfinite(v)) throw std::runtime_error("invalid number: "+text); return v; };
  for(int i=2;i<argc;++i) {
   std::string a=argv[i];
   if(a=="--roll") {roll=true;continue;}
   if(++i==argc) throw std::runtime_error("missing argument");
   std::string v=argv[i];
   if(a=="--seconds") seconds=number(v);
   else if(a=="--rate") rate=int(number(v));
   else if(a=="--param") {auto j=v.find('='); if(j==std::string::npos) throw std::runtime_error("expected name=value"); params[v.substr(0,j)]=number(v.substr(j+1));}
   else if(a=="--sweep") {auto j=v.find(',');auto k=v.find(',',j+1); if(j==std::string::npos||k==std::string::npos)throw std::runtime_error("expected name,start,end");sweep=v.substr(0,j);begin=number(v.substr(j+1,k-j-1));end=number(v.substr(k+1));}
   else throw std::runtime_error("unknown option: "+a);
  }
  if(seconds<0.5||seconds>15||(rate!=44100&&rate!=48000&&rate!=96000))throw std::runtime_error("unsupported render bounds");
  std::uint16_t endian=1; if(*reinterpret_cast<unsigned char*>(&endian)!=1)throw std::runtime_error("requires little endian");
  auto synth=std::make_unique<CymbalDSP>(); synth->init(rate); MapUI ui; synth->buildUserInterface(&ui);
  if(synth->getNumInputs()!=0||synth->getNumOutputs()!=2)throw std::runtime_error("wrong channel count");
  auto zone=[&](const std::string& key) { auto p=ui.getParamZone(key); if(!p)throw std::runtime_error("missing parameter: "+key);return p; };
  for(const auto& kv:params)*zone(kv.first)=FAUSTFLOAT(kv.second);
  if(!sweep.empty())*zone(sweep)=FAUSTFLOAT(begin);
  auto gate=zone("gate"); auto resid=zone("solver_residual_peak"); auto energy=zone("energy"); auto epeak=zone("energy_peak");
  auto fpeak=zone("contact_force_peak");auto contacts=zone("contact_samples");
  const int frames=int(seconds*rate),block=32,first=(int(0.35*rate)/block)*block,period=(int(0.25*rate)/block)*block;
  std::vector<FAUSTFLOAT> l(block),r(block);std::vector<float> out(size_t(frames)*2);
  double peak=0,previousEnergy=0,energyIncrease=0;int strikes=0;
  auto start=std::chrono::steady_clock::now();
  for(int f=0;f<frames;f+=block) {
   bool hit=(f==first)||(roll&&f>=first&&((f-first)%period==0));*gate=hit?1:0;strikes+=hit;
   if(!sweep.empty()&&f>=int(0.7*rate)) {double t=std::clamp((double(f)/rate-0.7)/std::max(0.1,seconds-1.0),0.0,1.0);*zone(sweep)=FAUSTFLOAT(begin+(end-begin)*t);}
   FAUSTFLOAT* outputs[]={l.data(),r.data()};int n=std::min(block,frames-f);synth->compute(n,nullptr,outputs);
   for(int j=0;j<n;++j) {if(!std::isfinite(l[j])||!std::isfinite(r[j]))throw std::runtime_error("non-finite audio");peak=std::max({peak,double(std::abs(l[j])),double(std::abs(r[j]))});out[2*(f+j)]=float(l[j]);out[2*(f+j)+1]=float(r[j]);}
   if(!std::isfinite(*energy)||!std::isfinite(*resid))throw std::runtime_error("non-finite physical diagnostic");
   if(f>first+int(0.15*rate)&&!hit)energyIncrease=std::max(energyIncrease,double(*energy)-previousEnergy);
   previousEnergy=*energy;
  }
  double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
  std::ofstream file(argv[1],std::ios::binary);if(!file)throw std::runtime_error("cannot open output");file.write(reinterpret_cast<char*>(out.data()),std::streamsize(out.size()*sizeof(float)));file.close();if(!file)throw std::runtime_error("write failed");
  std::cout<<std::setprecision(12)<<"{\"rate\":"<<rate<<",\"frames\":"<<frames<<",\"channels\":2,\"strikes\":"<<strikes<<",\"raw_peak\":"<<peak<<",\"energy_peak\":"<<*epeak<<",\"final_energy\":"<<*energy<<",\"max_post_contact_energy_increase\":"<<energyIncrease<<",\"solver_residual_peak\":"<<*resid<<",\"contact_force_peak\":"<<*fpeak<<",\"contact_samples\":"<<*contacts<<",\"render_seconds\":"<<elapsed<<",\"audio_seconds_per_compute_second\":"<<seconds/elapsed<<"}\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
