// Output-only native test harness for the exact full-detail engine.
// No synthesis, recording, incoming MIDI, or model reduction is implemented here.
// Device tests are opt-in and muted unless --audible is explicitly supplied.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <os/workgroup.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif
namespace lab {
constexpr uint32_t Rate=48000, Stride=21, Controls=23, MaxBlock=1024;
struct Event {uint32_t frame,type,index;};
struct Hit {std::vector<double> low,high;double strength;};
struct Packet {
 uint32_t nl,nh,nk,ks,ne,total;
 std::vector<double> low,high,knots,times,weights;
 std::vector<std::array<double,Controls>> controls;
 std::vector<Hit> hits;std::vector<Event> events;
};
struct Reader {
 std::ifstream f;uint64_t remaining;
 explicit Reader(const std::string&path):f(path,std::ios::binary|std::ios::ate){
  if(!f)throw std::runtime_error("cannot open packet");
  auto n=f.tellg();
  if(n<0||n>256ll*1024*1024)throw std::runtime_error("packet size limit");
  remaining=uint64_t(n);f.seekg(0);
 }
 void bytes(void*p,size_t n){if(n>remaining||!f.read(static_cast<char*>(p),n))throw std::runtime_error("truncated packet");remaining-=n;}
 uint32_t u32(){uint32_t v;bytes(&v,4);return v;}
 double number(){double v;bytes(&v,8);if(!std::isfinite(v))throw std::runtime_error("nonfinite number");return v;}
 std::vector<double> vec(uint64_t n){if(n>remaining/8)throw std::runtime_error("invalid array length");std::vector<double>v(n);bytes(v.data(),n*8);for(double x:v)if(!std::isfinite(x))throw std::runtime_error("nonfinite array");return v;}
};
Packet read_packet(const std::string&path){
 static_assert(std::endian::native==std::endian::little);
 Reader r(path);char magic[8];r.bytes(magic,8);
 if(std::memcmp(magic,"CYM21P01",8)||r.u32()!=Rate)throw std::runtime_error("packet format/rate mismatch");
 Packet p{};p.nl=r.u32();p.nh=r.u32();p.nk=r.u32();p.ks=r.u32();p.ne=r.u32();auto ns=r.u32(),ni=r.u32(),nev=r.u32();p.total=r.u32();
 if(!p.nl||p.nl>10000||!p.nh||p.nh>10000||p.nk<2||p.nk>8192||!p.ks||p.ks>48000||p.ne<2||p.ne>20000||!ns||ns>128||!ni||ni>64||nev>65536||!p.total||p.total>Rate*60)throw std::runtime_error("packet dimensions out of bounds");
 p.low=r.vec(uint64_t(p.nl)*Stride);p.high=r.vec(uint64_t(p.nh)*Stride);p.knots=r.vec(uint64_t(p.nh)*p.nk);p.times=r.vec(p.ne);p.weights=r.vec(p.ne);
 for(uint32_t i=0;i<p.ne;i++)if(p.times[i]<0||(i&&p.times[i]<=p.times[i-1])||p.weights[i]<0||p.weights[i]>1)throw std::runtime_error("invalid envelope");
 for(uint32_t i=0;i<ns;i++){std::array<double,Controls>a;for(auto&v:a)v=r.number();p.controls.push_back(a);}
 for(uint32_t i=0;i<ni;i++){Hit h{};h.strength=r.number();if(h.strength<0||h.strength>1)throw std::runtime_error("invalid strength");h.low=r.vec(uint64_t(p.nl)*4);h.high=r.vec(uint64_t(p.nh)*4);p.hits.push_back(std::move(h));}
 unsigned strikes=0;bool initial=false;
 for(uint32_t i=0;i<nev;i++){Event e{r.u32(),r.u32(),r.u32()};
  if(e.frame>=p.total||e.type>1||(i&&e.frame<p.events.back().frame)||e.index>=(e.type?ni:ns))throw std::runtime_error("invalid event");
  if(e.type){if(!initial)throw std::runtime_error("strike before snapshot");++strikes;}else if(e.frame==0)initial=true;p.events.push_back(e);
 }
 if(!initial||strikes>64||r.remaining)throw std::runtime_error("invalid initial state/history count/trailing bytes");
 return p;
}
struct Api {
 void*lib=nullptr;
 void*(*body_create)(const double*,int,const double*,int,const double*,int,int,const double*,const double*,int);
 void(*body_destroy)(void*);void*(*snapshot_prepare)(void*,const double*);void(*snapshot_destroy)(void*);
 void*(*impact_prepare)(void*,const double*,const double*,double);void(*impact_destroy)(void*);
 void*(*scene_create)(void*,int);void(*scene_destroy)(void*);
 int(*workers_hooks)(void*,int,void*,int(*)(void*,int),void(*)(void*,int));int(*scheduler)(void*,int,int,int);
 int(*install)(void*,void*);int(*strike)(void*,void*);int(*process)(void*,int,double*);int(*reset)(void*);
 void(*stats)(void*,uint64_t*);uint64_t(*worker_allocations)(void*);const char*(*error)();const char*(*backend)();
 template<class T>void sym(T&f,const char*name){void*p=dlsym(lib,name);if(!p)throw std::runtime_error(std::string("missing API: ")+name);static_assert(sizeof(p)==sizeof(f));std::memcpy(&f,&p,sizeof(p));}
 explicit Api(const std::string&path){lib=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);if(!lib)throw std::runtime_error(dlerror());try{
#define S(f,n) sym(f,n)
 S(body_create,"cym19_body_create");S(body_destroy,"cym19_body_destroy");S(snapshot_prepare,"cym19_snapshot_prepare");S(snapshot_destroy,"cym19_snapshot_destroy");S(impact_prepare,"cym19_impact_prepare");S(impact_destroy,"cym19_impact_destroy");S(scene_create,"cym19_scene_create");S(scene_destroy,"cym19_scene_destroy");S(workers_hooks,"cym21_workers_prepare_hooks");S(scheduler,"cym21_scheduler_prepare");S(install,"cym19_install");S(strike,"cym19_strike");S(process,"cym19_process");S(reset,"cym19_reset");S(stats,"cym19_stats");S(worker_allocations,"cym19_worker_allocations");S(error,"interaction_error");S(backend,"cym20_backend_name");
#undef S
 }catch(...){dlclose(lib);lib=nullptr;throw;}}
 ~Api(){if(lib)dlclose(lib);}Api(const Api&)=delete;
};
struct Trace{uint32_t frame,frames;double ms;};
struct Engine {
 Api&a;const Packet&p;void*body=nullptr;void*scene=nullptr;
 std::vector<void*>snaps,impacts;uint32_t at=0;size_t event=0,nt=0;
 std::vector<double>capture;std::vector<Trace>trace;
 std::atomic<bool>done{false};std::atomic<int>failure{0};
 Engine(Api&api,const Packet&packet):a(api),p(packet),capture(size_t(p.total)*2),trace(p.total){try{
  body=a.body_create(p.low.data(),p.nl,p.high.data(),p.nh,p.knots.data(),p.nk,p.ks,p.times.data(),p.weights.data(),p.ne);if(!body)fail("body");
  for(auto&v:p.controls){void*q=a.snapshot_prepare(body,v.data());if(!q)fail("snapshot");snaps.push_back(q);}
  for(auto&h:p.hits){void*q=a.impact_prepare(body,h.low.data(),h.high.data(),h.strength);if(!q)fail("impact");impacts.push_back(q);}
  int capacity=0;for(auto&e:p.events)capacity+=e.type==1;scene=a.scene_create(body,std::max(1,capacity));if(!scene)fail("scene");
 }catch(...){close();throw;}}
 void fail(const char*where){const char*s=a.error();throw std::runtime_error(std::string(where)+": "+(s?s:"unknown error"));}
 void close(){if(scene){a.scene_destroy(scene);scene=nullptr;}for(void*q:impacts)a.impact_destroy(q);impacts.clear();for(void*q:snaps)a.snapshot_destroy(q);snaps.clear();if(body){a.body_destroy(body);body=nullptr;}}
 ~Engine(){close();}
 void configure(int workers,int mode,void*ctx=nullptr,int(*enter)(void*,int)=nullptr,void(*leave)(void*,int)=nullptr){if(a.workers_hooks(scene,workers,ctx,enter,leave))fail("workers");if(a.scheduler(scene,mode,128,1))fail("scheduler");}
 // Core callback processing: no allocation, file I/O, snapshot/impact preparation.
 int render(uint32_t n,double*out)noexcept{
  std::fill(out,out+size_t(n)*2,0.);if(failure.load()||done.load())return failure.load();
  if(!n||n>MaxBlock){failure.store(20);done.store(true);return 20;}
  uint32_t origin=at,end=std::min(p.total,at+n);auto start=std::chrono::steady_clock::now();int rc=0;
  while(at<end&&!rc){while(event<p.events.size()&&p.events[event].frame==at){auto&e=p.events[event++];rc=e.type?a.strike(scene,impacts[e.index]):a.install(scene,snaps[e.index]);if(rc)break;}if(rc)break;
   uint32_t next=end;if(event<p.events.size())next=std::min(next,p.events[event].frame);if(next<=at){rc=21;break;}
   rc=a.process(scene,next-at,out+size_t(at-origin)*2);if(!rc)at=next;
  }
  for(uint32_t j=0;j<(at-origin)*2;j++)capture[size_t(origin)*2+j]=out[j];
  trace[nt++]={origin,n,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()};
  if(rc)failure.store(rc);
  if(rc||at==p.total)done.store(true,std::memory_order_release);
  return rc;
 }
 void report(const std::string&path,int workers,int mode,bool device,int joins,int policyFailures,int gaps,int overloads){
  std::vector<double>ms;ms.reserve(nt);unsigned misses=0;double sum=0;for(size_t i=0;i<nt;i++){auto&t=trace[i];ms.push_back(t.ms);sum+=t.ms;misses+=t.ms>1000.*t.frames/Rate;}
  std::sort(ms.begin(),ms.end());auto pct=[&](double q){return ms.empty()?0:ms[std::min(ms.size()-1,size_t(q*(ms.size()-1)))];};uint64_t st[5]{};a.stats(scene,st);
  std::ofstream j(path);if(!j)throw std::runtime_error("cannot write report");j.precision(12);
  j<<"{\n\"device_clocked\":"<<(device?"true":"false")<<",\"rate\":48000,\"frames\":"<<at<<",\"callbacks\":"<<nt<<",\n\"workers\":"<<workers<<",\"scheduler_mode\":"<<mode<<",\"backend\":\""<<a.backend()<<"\",\n\"processing_seconds\":"<<sum/1000<<",\"missed_budgets\":"<<misses<<",\"p50_ms\":"<<pct(.5)<<",\"p99_ms\":"<<pct(.99)<<",\"max_ms\":"<<pct(1)<<",\n\"worker_joins\":"<<joins<<",\"worker_policy_failures\":"<<policyFailures<<",\"timestamp_gaps\":"<<gaps<<",\"device_overloads\":"<<overloads<<",\n\"engine_failure\":"<<failure.load()<<",\"audio_cpp_allocations\":"<<st[1]<<",\"worker_cpp_allocations\":"<<a.worker_allocations(scene)<<",\"rejected_hits\":"<<st[2]<<",\"active_histories\":"<<st[3]<<",\n\"scope\":\"Prepared scripted packet, finite histories, no incoming MIDI preparation or realtime guarantee.\"\n}\n";
  std::ofstream raw(path+".f64",std::ios::binary);raw.write(reinterpret_cast<const char*>(capture.data()),size_t(at)*16);
  std::ofstream csv(path+".callbacks.csv");csv<<"frame,frames,processing_ms,budget_ms\n";for(size_t i=0;i<nt;i++)csv<<trace[i].frame<<','<<trace[i].frames<<','<<trace[i].ms<<','<<1000.*trace[i].frames/Rate<<'\n';
 }
};
#ifdef __APPLE__
struct Device {
 AudioUnit unit=nullptr;AudioDeviceID id=0;os_workgroup_t wg=nullptr;Engine&e;UInt32 block=0;bool audible=false;
 std::atomic<int>joins{0},leaves{0},policyFailures{0},overloads{0},changed{0};int gaps=0;bool timestampSeen=false;double expected=0;
 alignas(32)double buffer[MaxBlock*2]{};
 static thread_local os_workgroup_join_token_s token;static thread_local bool joined;
 static constexpr std::array<AudioObjectPropertySelector,3>selectors={kAudioDevicePropertyNominalSampleRate,kAudioDevicePropertyBufferFrameSize,kAudioDeviceProcessorOverload};
 static int enter(void*ctx,int)noexcept{auto&d=*static_cast<Device*>(ctx);mach_timebase_info_data_t tb;mach_timebase_info(&tb);double ticks=(1e9*double(d.block)/Rate)*tb.denom/tb.numer;
  thread_time_constraint_policy_data_t p{};p.period=uint32_t(ticks);p.computation=uint32_t(ticks*.5);p.constraint=uint32_t(ticks);p.preemptible=1;
  auto kr=thread_policy_set(pthread_mach_thread_np(pthread_self()),THREAD_TIME_CONSTRAINT_POLICY,reinterpret_cast<thread_policy_t>(&p),THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  if(kr!=KERN_SUCCESS){d.policyFailures++;return int(kr);}int rc=os_workgroup_join(d.wg,&token);joined=rc==0;if(joined)d.joins++;return rc;
 }
 static void leave(void*ctx,int)noexcept{auto&d=*static_cast<Device*>(ctx);if(joined){os_workgroup_leave(d.wg,&token);joined=false;d.leaves++;}}
 static OSStatus change(AudioObjectID,UInt32 n,const AudioObjectPropertyAddress*a,void*ctx){auto&d=*static_cast<Device*>(ctx);for(UInt32 i=0;i<n;i++)if(a[i].mSelector==kAudioDeviceProcessorOverload)d.overloads++;else d.changed.store(1);return noErr;}
 static OSStatus callback(void*ctx,AudioUnitRenderActionFlags*,const AudioTimeStamp*ts,UInt32,UInt32 n,AudioBufferList*out){auto&d=*static_cast<Device*>(ctx);auto start=std::chrono::steady_clock::now();
  if(out)for(UInt32 i=0;i<out->mNumberBuffers;i++)if(out->mBuffers[i].mData)std::memset(out->mBuffers[i].mData,0,out->mBuffers[i].mDataByteSize);
  if(d.e.done.load())return noErr;
  if(d.changed.load()||!out||out->mNumberBuffers!=2||!n||n>MaxBlock){d.e.failure.store(30);d.e.done.store(true);return kAudioUnitErr_CannotDoInCurrentContext;}
  for(int c=0;c<2;c++)if(!out->mBuffers[c].mData||out->mBuffers[c].mDataByteSize<n*4){d.e.failure.store(31);d.e.done.store(true);return kAudioUnitErr_CannotDoInCurrentContext;}
  if(ts&&(ts->mFlags&kAudioTimeStampSampleTimeValid)){if(d.timestampSeen&&std::abs(ts->mSampleTime-d.expected)>.5)d.gaps++;d.expected=ts->mSampleTime+n;d.timestampSeen=true;}
  int rc=d.e.render(n,d.buffer);if(rc)return kAudioUnitErr_CannotDoInCurrentContext;
  if(d.audible){for(UInt32 j=0;j<n;j++)for(int c=0;c<2;c++){double v=d.buffer[2*j+c]*.25;if(!std::isfinite(v)||std::abs(v)>.98){d.e.failure.store(32);d.e.done.store(true);for(UInt32 i=0;i<out->mNumberBuffers;i++)std::memset(out->mBuffers[i].mData,0,out->mBuffers[i].mDataByteSize);return noErr;}static_cast<float*>(out->mBuffers[c].mData)[j]=float(v);}}
  // Hardware measurement includes timestamp checks, clear, capture, and output copy.
  if(d.e.nt)d.e.trace[d.e.nt-1].ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return noErr;
 }
 void check(OSStatus s,const char*name){if(s)throw std::runtime_error(std::string(name)+" OSStatus="+std::to_string(s));}
 explicit Device(Engine&engine):e(engine){try{
  AudioObjectPropertyAddress a{kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};UInt32 size=sizeof(id);check(AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,0,nullptr,&size,&id),"default output");if(!id)throw std::runtime_error("no output device");
  a.mSelector=kAudioDevicePropertyNominalSampleRate;double rate=0;size=sizeof(rate);check(AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&rate),"rate");if(rate!=Rate)throw std::runtime_error("Output must already be 48000 Hz; this host does not change device settings.");
  a.mSelector=kAudioDevicePropertyBufferFrameSize;size=sizeof(block);check(AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&block),"block size");if(!block||block>MaxBlock)throw std::runtime_error("device buffer must be 1..1024 frames");
  AudioComponentDescription desc{kAudioUnitType_Output,kAudioUnitSubType_HALOutput,kAudioUnitManufacturer_Apple,0,0};auto comp=AudioComponentFindNext(nullptr,&desc);if(!comp)throw std::runtime_error("AUHAL missing");check(AudioComponentInstanceNew(comp,&unit),"create");UInt32 off=0,on=1;
  check(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Input,1,&off,sizeof(off)),"disable input");check(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Output,0,&on,sizeof(on)),"enable output");check(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&id,sizeof(id)),"device");
  AudioStreamBasicDescription fmt{};fmt.mSampleRate=Rate;fmt.mFormatID=kAudioFormatLinearPCM;fmt.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked|kAudioFormatFlagIsNonInterleaved|kAudioFormatFlagsNativeEndian;fmt.mBytesPerPacket=fmt.mBytesPerFrame=4;fmt.mFramesPerPacket=1;fmt.mChannelsPerFrame=2;fmt.mBitsPerChannel=32;
  check(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&fmt,sizeof(fmt)),"format");UInt32 max=MaxBlock;check(AudioUnitSetProperty(unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&max,sizeof(max)),"maximum block");
  AURenderCallbackStruct cb{callback,this};check(AudioUnitSetProperty(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&cb,sizeof(cb)),"callback");check(AudioUnitInitialize(unit),"initialize");
  os_workgroup_t borrowed=nullptr;size=sizeof(borrowed);OSStatus gs=AudioUnitGetProperty(unit,kAudioOutputUnitProperty_OSWorkgroup,kAudioUnitScope_Global,0,&borrowed,&size);
  if(gs==noErr&&borrowed){wg=os_workgroup_create_with_workgroup("cymbal full-detail workers",borrowed);}
  for(auto selector:selectors){a.mSelector=selector;check(AudioObjectAddPropertyListener(id,&a,change,this),"device observer");}
 }catch(...){close();throw;}}
 void close(){if(unit)AudioOutputUnitStop(unit);if(id){AudioObjectPropertyAddress a{0,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};for(auto sel:selectors){a.mSelector=sel;AudioObjectRemovePropertyListener(id,&a,change,this);}}if(wg){os_release(wg);wg=nullptr;}if(unit){AudioUnitUninitialize(unit);AudioComponentInstanceDispose(unit);unit=nullptr;}}
 ~Device(){close();}
};
thread_local os_workgroup_join_token_s Device::token{};thread_local bool Device::joined=false;
#endif
int selftest(){static_assert(sizeof(double)==8&&sizeof(uint32_t)==4);
#ifdef __APPLE__
 AudioComponentDescription d{kAudioUnitType_Output,kAudioUnitSubType_HALOutput,kAudioUnitManufacturer_Apple,0,0};if(!AudioComponentFindNext(nullptr,&d))throw std::runtime_error("AUHAL component unavailable");
#endif
 std::cout<<"{\"host_smoke\":true,\"audio_device_opened\":false,\"full_engine_run\":false}\n";return 0;}
}
int main(int argc,char**argv){try{
 if(argc==2&&std::string(argv[1])=="--selftest")return lab::selftest();
 std::string packet,library,report="native-results.json";int workers=4,block=128,mode=3;bool device=false,audible=false;
 for(int i=1;i<argc;i++){std::string k=argv[i];if(k=="--device")device=true;else if(k=="--audible")audible=true;else{if(i+1==argc)throw std::runtime_error("missing option value");std::string v=argv[++i];if(k=="--packet")packet=v;else if(k=="--library")library=v;else if(k=="--report")report=v;else if(k=="--workers")workers=std::stoi(v);else if(k=="--block")block=std::stoi(v);else if(k=="--scheduler")mode=std::stoi(v);else throw std::runtime_error("unknown option "+k);}}
 if(packet.empty()||library.empty()||workers<1||workers>8||block<1||block>1024||(mode!=0&&mode!=3)||(audible&&!device))throw std::runtime_error("usage: --packet FILE --library FILE [--workers 1..8 --block 1..1024 --scheduler 0|3 --report FILE] [--device [--audible]]");
 auto p=lab::read_packet(packet);lab::Api a(library);lab::Engine e(a,p);
 if(!device){e.configure(workers,mode);std::array<double,lab::MaxBlock*2>b{};while(!e.done.load())if(e.render(std::min<uint32_t>(block,p.total-e.at),b.data()))break;e.report(report,workers,mode,false,0,0,0,0);}
 else{
#ifdef __APPLE__
  lab::Device d(e);d.audible=audible;try{
   if(workers>1&&!d.wg)throw std::runtime_error("No device workgroup: use --workers 1 for an explicit serial test.");
   if(workers>1)std::cerr<<"Workgroup recommended total workers="<<os_workgroup_max_parallel_threads(d.wg,nullptr)<<"; requested="<<workers<<'\n';
   e.configure(workers,mode,&d,workers>1?lab::Device::enter:nullptr,workers>1?lab::Device::leave:nullptr);d.check(AudioOutputUnitStart(d.unit),"start");auto start=std::chrono::steady_clock::now();
   while(!e.done.load(std::memory_order_acquire)){std::this_thread::sleep_for(std::chrono::milliseconds(10));if(std::chrono::steady_clock::now()-start>std::chrono::seconds(90)){e.failure.store(33);e.done.store(true);break;}}
   d.check(AudioOutputUnitStop(d.unit),"stop");e.report(report,workers,mode,true,d.joins,d.policyFailures,d.gaps,d.overloads);e.close();
  }catch(...){AudioOutputUnitStop(d.unit);e.close();throw;}
#else
  throw std::runtime_error("--device requires macOS; Linux supports offline exactness tests only");
#endif
 }
 std::cout<<"Wrote "<<report<<" and exact float64 capture.\n";return e.failure.load()?2:0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
