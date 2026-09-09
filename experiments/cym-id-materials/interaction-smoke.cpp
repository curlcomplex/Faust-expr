// Actual Faust synthetic fixture. No measured cymbal data or realism claim.
#include <algorithm>
#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <faust/dsp/dsp.h>
#include <faust/gui/meta.h>
#include <faust/gui/MapUI.h>
#include "interaction-probe.hpp"
std::vector<std::complex<double>> render(float h,int lock,int material,float pressure,bool release=false) {
 InteractionProbe dsp;dsp.init(48000);MapUI ui;dsp.buildUserInterface(&ui);
 auto set=[&](const char* key,float value){auto* z=ui.getParamZone(key);if(!z)throw std::runtime_error(std::string("Missing ")+key);*z=value;};
 set("relative_thickness",h);set("thickness_pitch_lock",lock);set("contact_material",material);set("contact_pressure",pressure);set("gate",1);
 std::vector<std::complex<double>> result;float l,r;float* outputs[]={&l,&r};
 for(int n=0;n<9600;n++) {if(n==1)set("gate",0);if(release&&n==4800)set("contact_pressure",0);dsp.compute(1,nullptr,outputs);if(!std::isfinite(l)||!std::isfinite(r))throw std::runtime_error("Nonfinite");result.emplace_back(l,r);}
 return result;
}
int main(){try{
 const double pi=std::acos(-1.0);int checked=0;
 const double k0[]={3e5,1e5,1.5e6,8e5,2e7,4e7},k1[]={5e6,3e6,6e6,8e6,3e6,4e6};
 const double tau[]={.0007,.00012,.0002,.0001,.000008,.000004},visc[]={18,12,6,3,1.5,.8};
 auto free=render(1,0,0,0);
 for(int m=0;m<6;m++){
  if(render(1,0,m,0)!=free)throw std::runtime_error("Inactive contact changed baseline");
  for(float pressure:{.25f,.5f,1.f}){
   auto a=render(1,0,m,pressure);std::complex<double> num=0;double den=0;
   for(size_t n=1;n<a.size();n++){num+=std::conj(a[n-1])*a[n];den+=std::norm(a[n-1]);if(std::norm(a[n])>std::norm(a[n-1])+1e-8)throw std::runtime_error("State energy increased");}
   auto q=num/den;double f=std::abs(std::arg(q))*48000/(2*pi),d=-std::log(std::abs(q))*48000,z=2*pi*1000*tau[m],load=pressure*pressure;
   double expected_f=std::sqrt(1e6+load*(k0[m]+k1[m]*z*z/(1+z*z))/(4*pi*pi));
   double expected_d=2+load*(visc[m]+k1[m]*tau[m]/(2*(1+z*z)));
   if(std::abs(f-expected_f)>.05||std::abs(d-expected_d)>.05)throw std::runtime_error("Load formula mismatch");++checked;
  }
  auto released=render(1,0,m,1,true);
  for(size_t n=1;n<released.size();n++)if(std::norm(released[n])>std::norm(released[n-1])+1e-8)throw std::runtime_error("Released energy increased");
 }
 for(float h:{.25f,.6f,1.f,1.6f,4.f}){
  auto a=render(h,1,0,0);std::complex<double> num=0;double den=0;
  for(size_t n=1;n<a.size();n++){num+=std::conj(a[n-1])*a[n];den+=std::norm(a[n-1]);}
  double f=std::abs(std::arg(num/den))*48000/(2*pi);
  if(std::abs(f-1000)>.05)throw std::runtime_error("Pivot lock failed");++checked;
 }
 std::cout<<checked<<" numeric load/pivot fixtures passed, plus inactive-contact and release-passivity checks. NOT full-bank or real-material validation.\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
