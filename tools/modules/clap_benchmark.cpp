#include <chrono>
#include <iostream>
#include <memory>
#include <vector>
#define main offlineRendererMain
#include "render.cpp"
#undef main
int main(int argc,char**argv){int block=argc>1?std::stoi(argv[1]):128;constexpr int voices=4,N=2500,W=200;std::vector<std::unique_ptr<ModuleDSP>> d;std::vector<std::unique_ptr<UI>> u;for(int i=0;i<voices;i++){d.emplace_back(new ModuleDSP());d.back()->init(48000);u.emplace_back(new UI());d.back()->buildUserInterface(u.back().get());u.back()->set("pitch_hz",150+70*i);u.back()->set("spacing",.7f);u.back()->set("decay",.7f);u.back()->set("body",.65f);u.back()->set("drive",.55f);}std::vector<float>o(block),ts;float*p=o.data();double sum=0;for(int j=-W;j<N;j++){for(auto&q:u)q->set("gate",j%17==0?1.f:0.f);auto a=std::chrono::steady_clock::now();for(auto&q:d){q->compute(block,nullptr,&p);sum+=o[block/2];}auto b=std::chrono::steady_clock::now();if(j>=0)ts.push_back(std::chrono::duration<float,std::micro>(b-a).count());}std::sort(ts.begin(),ts.end());std::cout<<"{\"block\":"<<block<<",\"voices\":4,\"p50_us\":"<<ts[N/2]<<",\"p95_us\":"<<ts[int(N*.95)]<<",\"p99_us\":"<<ts[int(N*.99)]<<",\"checksum\":"<<sum<<"}\n";}
