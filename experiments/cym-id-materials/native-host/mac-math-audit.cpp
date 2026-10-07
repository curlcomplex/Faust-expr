// Apple Silicon phase-math diagnostic. No cymbal/model data are substituted.
// Full-audio/model regression is required before promoting any candidate.
#include <Accelerate/Accelerate.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

using Clock = std::chrono::steady_clock;
static uint64_t bits(double v) { uint64_t u; std::memcpy(&u,&v,sizeof(u)); return u; }
static uint64_t ordered(double v) { uint64_t u=bits(v); return (u>>63)?~u:(u|(uint64_t(1)<<63)); }
static uint64_t ulps(double a,double b) { auto x=ordered(a),y=ordered(b); return x>y?x-y:y-x; }
// This distinguishes separate platform routines from compiler-emitted DSP code.
static double (*volatile ref_sin)(double) = static_cast<double(*)(double)>(std::sin);
static double (*volatile ref_cos)(double) = static_cast<double(*)(double)>(std::cos);
static void ordinary(int n,const double*x,double*s,double*c) { for(int i=0;i<n;i++){s[i]=ref_sin(x[i]);c[i]=ref_cos(x[i]);} }
__attribute__((noinline)) static void compiled(int n,const double*x,double*s,double*c) { for(int i=0;i<n;i++){s[i]=std::sin(x[i]);c[i]=std::cos(x[i]);} }
static void paired(int n,const double*x,double*s,double*c) { for(int i=0;i<n;i++) __sincos(x[i],s+i,c+i); }
static void force(int n,const double*x,double*s,double*c) { vvsincos(s,c,x,&n); }
using Fn=void(*)(int,const double*,double*,double*);
struct Check { uint64_t pairs=0,mismatch=0,max_ulp=0;double max_absolute=0; };
static uint64_t rng=0x62835192c0ffeedULL;
static uint64_t random64(){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
static double uniform(){return (random64()>>11)*0x1p-53;}
static void compare(Check&ch,const std::vector<double>&x,Fn reference,Fn candidate){
 const int n=int(x.size());std::vector<double>rs(n),rc(n),s(n),c(n);reference(n,x.data(),rs.data(),rc.data());candidate(n,x.data(),s.data(),c.data());
 for(int i=0;i<n;i++) {ch.pairs++;if(bits(rs[i])!=bits(s[i])||bits(rc[i])!=bits(c[i]))ch.mismatch++;
  ch.max_ulp=std::max({ch.max_ulp,ulps(rs[i],s[i]),ulps(rc[i],c[i])});ch.max_absolute=std::max({ch.max_absolute,std::abs(rs[i]-s[i]),std::abs(rc[i]-c[i])});}
}
static volatile double consume=0;
static double timed(Fn fn,const std::vector<double>&x,int block,int reps){
 std::vector<double>s(x.size()),c(x.size());auto start=Clock::now();
 for(int r=0;r<reps;r++)for(int i=0;i<int(x.size());i+=block){int n=std::min(block,int(x.size())-i);fn(n,x.data()+i,s.data()+i,c.data()+i);}
 double elapsed=std::chrono::duration<double>(Clock::now()-start).count();consume=s[x.size()/3]+c[x.size()/2];return elapsed;
}
static void printcheck(const char*n,const Check&c){std::cout<<'"'<<n<<"\":{\"pairs\":"<<c.pairs<<",\"mismatched_pairs\":"<<c.mismatch<<",\"max_ulp\":"<<c.max_ulp<<",\"max_absolute\":"<<c.max_absolute<<'}';}
int main(){
 std::cout<<std::setprecision(17);Check dc,pc,vc,dp,dv;
 auto checks=[&](const std::vector<double>&x){compare(dc,x,ordinary,compiled);compare(pc,x,ordinary,paired);compare(vc,x,ordinary,force);compare(dp,x,compiled,paired);compare(dv,x,compiled,force);};
 // Frozen processor phase increments are nonnegative and at most 0.96*pi.
 for(int batch=0;batch<32;batch++) {std::vector<double>x(65536);for(double&v:x)v=uniform()*(0.96*M_PI);checks(x);}
 std::vector<double>bound;
 for(double x:{0.,0x1p-30,0x1p-27,0.125,0.5,0.855469,M_PI/4,M_PI/2,2.426265,0.96*M_PI})for(int side:{-1,1}){double v=x;for(int i=0;i<128;i++){if(v>=0)bound.push_back(v);v=std::nextafter(v,side<0?-INFINITY:INFINITY);}}
 checks(bound);
 std::vector<double>x(1856*128);for(size_t i=0;i<x.size();i++){double f=200.+21800.*double(i/128)/1856.;x[i]=(2*M_PI*f/48000.)*(1.+0.004*std::sin(double(i%128)*0.007));}
 std::vector<double>warmS(x.size()),warmC(x.size());compiled(int(x.size()),x.data(),warmS.data(),warmC.data());
 std::cout<<"{\"scope\":\"Mac platform double-precision phase-math diagnostic; not full model or DAW timing\",\"range\":[0,"<<0.96*M_PI<<"],\"checks\":{";
 printcheck("separate_vs_compiled",dc);std::cout<<',';printcheck("separate_vs_paired",pc);std::cout<<',';printcheck("separate_vs_vforce",vc);std::cout<<',';printcheck("compiled_vs_paired",dp);std::cout<<',';printcheck("compiled_vs_vforce",dv);std::cout<<"},\"timings\":[";
 bool first=true;for(int block:{64,128,256})for(int repeat=0;repeat<4;repeat++){
  double a,b,c,d;if(repeat%2){c=timed(force,x,block,80);b=timed(paired,x,block,80);d=timed(compiled,x,block,80);a=timed(ordinary,x,block,80);}else{a=timed(ordinary,x,block,80);d=timed(compiled,x,block,80);b=timed(paired,x,block,80);c=timed(force,x,block,80);}
  if(!first)std::cout<<',';first=false;std::cout<<"{\"block\":"<<block<<",\"repeat\":"<<repeat<<",\"pairs\":"<<x.size()*80<<",\"scalar_seconds\":"<<a<<",\"compiled_seconds\":"<<d<<",\"paired_seconds\":"<<b<<",\"vforce_seconds\":"<<c<<'}';
 }
 std::cout<<"],\"candidate_promoted\":false}"<<std::endl;return 0;
}
