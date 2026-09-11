// Same-sound, warmed four-voice benchmark; NOT a device callback/malloc proof.
#include <atomic>
#include <cstdlib>
#include <new>
static thread_local bool allocationGuard = false;
static std::atomic<unsigned long long> allocationCount {0};
void* operator new(std::size_t n) { if (allocationGuard) ++allocationCount; if (void* p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
#define main offlineRendererMain
#include "render.cpp"
#undef main
int main(int argc,char** argv) { try {
 const int block=argc>1?int(integer(argv[1])):128;
 if(block<1 || block>2048) throw std::runtime_error("block size");
 constexpr int voices=4,iterations=2400,warmup=256;
 std::vector<std::unique_ptr<ModuleDSP>> dsps;
 std::vector<std::unique_ptr<UI>> uis;
 ModuleDSP::classInit(48000);
 for(int v=0;v<voices;++v) {
  dsps.push_back(std::make_unique<ModuleDSP>());dsps.back()->instanceInit(48000);
  uis.push_back(std::make_unique<UI>());dsps.back()->buildUserInterface(uis.back().get());
  uis.back()->set("pitch_hz",105.0f+v*80.0f);uis.back()->set("balance",.65f);
  uis.back()->set("crack",.8f);uis.back()->set("decay",.7f);uis.back()->set("drive",.6f);
 }
 std::vector<float> output(block),timings;timings.reserve(iterations);float* ptr=output.data();double checksum=0;
 for(int j=-warmup;j<iterations;++j) {
  for(auto& ui:uis) ui->set("gate",j%13==0?1.0f:0.0f);
  allocationGuard=true;const auto start=std::chrono::steady_clock::now();
  for(auto& dsp:dsps) {dsp->compute(block,nullptr,&ptr);checksum+=output[block/2];}
  const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
  allocationGuard=false;if(j>=0)timings.push_back(float(ns)/1000.0f);
 }
 if(!std::isfinite(checksum))throw std::runtime_error("nonfinite compute");
 std::sort(timings.begin(),timings.end());
 std::cout<<std::setprecision(12)<<"{\"voices\":4,\"rate\":48000,\"block\":"<<block
 <<",\"iterations\":"<<iterations<<",\"sizeof_dsp_bytes\":"<<sizeof(ModuleDSP)
 <<",\"ordinary_new_allocations\":"<<allocationCount.load()<<",\"p50_us\":"<<timings[iterations/2]
 <<",\"p95_us\":"<<timings[int(iterations*.95)]<<",\"p99_us\":"<<timings[int(iterations*.99)]
 <<",\"max_us\":"<<timings.back()<<",\"checksum\":"<<checksum<<"}\n";
 return allocationCount?1:0;
 } catch(const std::exception& e) {allocationGuard=false;std::cerr<<e.what()<<'\n';return 1;}
}
