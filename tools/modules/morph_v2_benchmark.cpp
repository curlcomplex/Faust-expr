#include <atomic>
#include <cstdlib>
#include <new>
static thread_local bool allocationGuard=false;
static std::atomic<unsigned long long> allocations{0};
void* operator new(std::size_t n){if(allocationGuard)++allocations;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);} void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);} void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
#define main rendererMain
#include "render.cpp"
#undef main
int main(int argc,char** argv){try{
 int b=argc>1?int(integer(argv[1])):128;int stack=argc>2?int(integer(argv[2])):4;
 if(b<1||b>2048||stack<1||stack>4)throw std::runtime_error("dimensions");
 constexpr int count=400,warm=64,voices=4;
 std::vector<std::unique_ptr<ModuleDSP>> dsps;std::vector<std::unique_ptr<UI>> uis;
 for(int i=0;i<voices;++i){dsps.push_back(std::make_unique<ModuleDSP>());dsps.back()->init(48000);uis.push_back(std::make_unique<UI>());dsps.back()->buildUserInterface(uis.back().get());
  uis.back()->set("pitch_hz",55.0f*std::pow(2.0f,i*7.0f/12));uis.back()->set("stack",float(stack));uis.back()->set("morph",.9f);uis.back()->set("shape",.8f);uis.back()->set("detune",.7f);uis.back()->set("drive",.7f);}
 std::vector<float> l(b),r(b),d;float* p[]={l.data(),r.data()};d.reserve(count);double checksum=0;
 for(int j=-warm;j<count;++j){for(auto&u:uis)u->set("gate",j%17?1.f:0.f);allocationGuard=true;auto t=std::chrono::steady_clock::now();
  for(auto&s:dsps){s->compute(b,nullptr,p);checksum+=l[b/2]+r[b/2];}
  double us=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t).count()/1000.;allocationGuard=false;if(j>=0)d.push_back(float(us));}
 if(!std::isfinite(checksum))throw std::runtime_error("nonfinite");std::sort(d.begin(),d.end());
 std::cout<<std::setprecision(12)<<"{\"frames\":"<<b<<",\"host_voices\":4,\"stack\":"<<stack<<",\"rate\":48000,\"object_bytes\":"<<sizeof(ModuleDSP)<<",\"ordinary_new_in_compute\":"<<allocations.load()<<",\"p50_us\":"<<d[count/2]<<",\"p95_us\":"<<d[int(count*.95)]<<",\"p99_us\":"<<d[int(count*.99)]<<",\"max_us\":"<<d.back()<<",\"checksum\":"<<checksum<<"}\n";
 return allocations?1:0;
 }catch(const std::exception&e){allocationGuard=false;std::cerr<<e.what()<<'\n';return 1;}}
