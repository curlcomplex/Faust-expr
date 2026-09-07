// Native generated-Faust throughput benchmark. Not realtime callback acceptance.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>
#include "generated_dsp.h"

int main(int argc,char** argv){
    if(argc!=4){std::cerr<<"usage: native_scaling_bench <participants> <frames> <blocks-per-trial>\n";return 2;}
    const int participants=std::atoi(argv[1]), frames=std::atoi(argv[2]), blocks=std::atoi(argv[3]);
    if(participants<1||participants>64||frames<1||frames>4096||blocks<100)return 2;
    setenv("OMP_NUM_THREADS",std::to_string(participants).c_str(),1);
    setenv("OMP_DYN_THREAD","0",1);
    std::unique_ptr<dsp> instance(new BenchDSP());
    instance->init(48000);
    if(instance->getNumInputs()!=0||instance->getNumOutputs()!=2)return 3;
    std::vector<FAUSTFLOAT>a(frames),b(frames);FAUSTFLOAT* outs[2]={a.data(),b.data()};
    for(int i=0;i<1000;++i)instance->compute(frames,nullptr,outs);
    std::vector<double> ns;ns.reserve(9);double checksum=0;
    for(int t=0;t<9;++t){
      auto t0=std::chrono::steady_clock::now();
      for(int i=0;i<blocks;++i)instance->compute(frames,nullptr,outs);
      auto t1=std::chrono::steady_clock::now();
      ns.push_back(std::chrono::duration<double,std::nano>(t1-t0).count()/(double(blocks)*frames));
      for(int i=0;i<frames;++i)checksum+=double(a[i])+double(b[i]);
    }
    if(!std::isfinite(checksum))return 4;
    std::sort(ns.begin(),ns.end());
    std::cout<<std::setprecision(12)<<"participants="<<participants<<"\nframes="<<frames
      <<"\nmedian_ns_per_frame="<<ns[4]<<"\nbest_ns_per_frame="<<ns.front()
      <<"\nworst_ns_per_frame="<<ns.back()<<"\nchecksum="<<checksum<<"\n";
    return 0;
}
