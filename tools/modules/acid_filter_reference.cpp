// Selected Open303 TB_303 filter-mode transcription for port verification.
// Copyright (c) 2009 Robin Schmidt; MIT notice in
// modules/acid-voice/v1/OPEN303-LICENSE.txt. Source pin in the batch README.
// This is NOT the complete upstream synthesizer or a hardware recording.
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <algorithm>
int main(int argc,char** argv) { try {
 if(argc!=6) throw std::runtime_error("usage: reference INPUT OUTPUT SR CUTOFF RES");
 double sr=std::stod(argv[3]),cf=std::stod(argv[4]),res=std::stod(argv[5]);
 if(!std::isfinite(sr+cf+res)||sr<8000||sr>96000||res<0||res>.920001) throw std::runtime_error("range");
 cf=std::max(200.,std::min(sr/8,cf));
 std::ifstream in(argv[1],std::ios::binary|std::ios::ate);
 const auto bytes=std::streamoff(in.tellg());
 if(!in||bytes<4||bytes>4000000||bytes%4) throw std::runtime_error("input size");
 std::vector<float> x(size_t(bytes)/4);in.seekg(0);in.read(reinterpret_cast<char*>(x.data()),x.size()*4);
 if(!in) throw std::runtime_error("read");
 double r=(1-std::exp(-3*res))/(1-std::exp(-3.));
 double fx=cf/(sr*std::sqrt(2.));
 double b=(.00045522346+6.1922189*fx)/(1+12.358354*fx+4.4156345*fx*fx);
 double kk=fx*(fx*(fx*(fx*(fx*(fx+7198.6997)-5837.7917)-476.47308)+614.95611)+213.87126)+16.998792;
 double k=kk*r,g=((kk/17-1)*r+1)*(1+r),a=std::exp(-2*3.14159265358979323846*150/sr);
 double y1=0,y2=0,y3=0,y4=0,hx=0,hy=0;
 for(auto& sample:x) {
  if(!std::isfinite(sample)) throw std::runtime_error("nonfinite input");
  double feedback=k*y4,hp=.5*(1+a)*(feedback-hx)+a*hy;
  hx=feedback;hy=hp;
  double y0=sample-hp;
  y1+=2*b*(y0-y1+y2);
  y2+=b*(y1-2*y2+y3);
  y3+=b*(y2-2*y3+y4);
  y4+=b*(y3-2*y4);
  sample=float(2*g*y4);
  if(!std::isfinite(sample)) throw std::runtime_error("nonfinite output");
 }
 std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<char*>(x.data()),x.size()*4);
 if(!out) throw std::runtime_error("write");
 return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
