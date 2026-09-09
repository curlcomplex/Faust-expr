// Same complete instrument; offline four-voice benchmark, not phone audio proof.
#include <atomic>
#include <cstdlib>
#include <new>
static thread_local bool guarded=false;
static std::atomic<unsigned long long> allocations{0};
void* operator new(std::size_t n) { if(guarded)++allocations; if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc(); }
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
#define main rendererMain
#include "render.cpp"
#undef main
int main(int argc,char** argv){try{
 const int block=argc>1?int(integer(argv[1])):128;
 if(block<1||block>2048)throw std::runtime_error("block");
 constexpr int count=2400,warmup=256,voices=4;
 std::vector<std::unique_ptr<ModuleDSP>> dsps;std::vector<std::unique_ptr<UI>> uis;
 for(int v=0;v<voices;++v){
  dsps.push_back(std::make_unique<ModuleDSP>());dsps.back()->init(48000);
  uis.push_back(std::make_unique<UI>());dsps.back()->buildUserInterface(uis.back().get());
  uis.back()->set("pitch_hz",220+v*490);uis.back()->set("shape",.85);
  uis.back()->set("decay",.74);uis.back()->set("drive",.6);
 }
 std::vector<float> buffer(block),timings;timings.reserve(count);float* output=buffer.data();double checksum=0;
 for(int j=-warmup;j<count;++j){
  for(auto& ui:uis)ui->set("gate",j%13==0?1:0);
  guarded=true;const auto start=std::chrono::steady_clock::now();
  for(auto& dsp:dsps){dsp->compute(block,nullptr,&output);checksum+=buffer[block/2];}
  auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();guarded=false;
  if(j>=0)timings.push_back(float(ns)/1000);
 }
 if(!std::isfinite(checksum))throw std::runtime_error("nonfinite output");
 std::sort(timings.begin(),timings.end());
 std::cout<<std::setprecision(12)<<"{\"voices\":4,\"rate\":48000,\"block\":"<<block<<",\"iterations\":"<<count
 <<",\"sizeof_dsp_bytes\":"<<sizeof(ModuleDSP)<<",\"ordinary_new_allocations\":"<<allocations.load()
 <<",\"p50_us\":"<<timings[count/2]<<",\"p95_us\":"<<timings[int(count*.95)]
 <<",\"p99_us\":"<<timings[int(count*.99)]<<",\"max_us\":"<<timings.back()<<",\"checksum\":"<<checksum<<"}\n";
 return allocations?1:0;
}catch(const std::exception& e){guarded=false;std::cerr<<e.what()<<'\n';return 1;}}
