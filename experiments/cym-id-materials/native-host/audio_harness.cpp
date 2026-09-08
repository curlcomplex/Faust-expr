// Native execution test host, not a plug-in or a new synthesis implementation.
// Loads the preserved numerical engine through its C API. No reference audio is read.
// --device is explicit; defaults to muted even on the hardware device.
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
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <os/workgroup.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif
namespace lab {
constexpr uint32_t Rate=48000, Stride=21, ControlCount=23, MaxFrames=1024;
struct Event {uint32_t frame,type,index;};
struct Hit {std::vector<double>low,high;double strength;};
struct Packet {
 uint32_t nl=0,nh=0,nk=0,ks=0,ne=0,ns=0,ni=0,total=0;
 std::vector<double>low,high,knots,times,weights;
 std::vector<std::array<double,ControlCount>>controls;std::vector<Hit>hits;std::vector<Event>events;
};
struct Reader {
 std::ifstream f;uint64_t remaining;
 explicit Reader(const std::string&path):f(path,std::ios::binary|std::ios::ate){if(!f)throw std::runtime_error("cannot open numerical packet");auto n=f.tellg();if(n<0||n>256ll*1024*1024)throw std::runtime_error("packet size limit");remaining=n;f.seekg(0);}
 void bytes(void*p,size_t n){if(n>remaining||!f.read(static_cast<char*>(p),n))throw std::runtime_error("truncated packet");remaining-=n;}
 uint32_t u32(){uint32_t x;bytes(&x,4);return x;}
 double number(){double x;bytes(&x,8);if(!std::isfinite(x))throw std::runtime_error("nonfinite packet number");return x;}
 std::vector<double>vec(uint64_t n){if(n>remaining/8)throw std::runtime_error("invalid packet length");std::vector<double>x(n);bytes(x.data(),n*8);for(double v:x)if(!std::isfinite(v))throw std::runtime_error("nonfinite packet vector");return x;}
};
Packet read_packet(const std::string&path){
 static_assert(std::endian::native==std::endian::little,"packet decoder requires little endian");
 Reader r(path);char magic[8];r.bytes(magic,8);if(std::memcmp(magic,"CYM21P01",8))throw std::runtime_error("unknown packet format");
 if(r.u32()!=Rate)throw std::runtime_error("engine/packet requires 48000 Hz");
 Packet p;
 p.nl=r.u32();p.nh=r.u32();p.nk=r.u32();p.ks=r.u32();p.ne=r.u32();p.ns=r.u32();p.ni=r.u32();auto nev=r.u32();p.total=r.u32();
 if(p.nl<1||p.nl>10000||p.nh<1||p.nh>10000||p.nk<2||p.nk>8192||!p.ks||p.ks>48000||p.ne<2||p.ne>20000||!p.ns||p.ns>128||!p.ni||p.ni>64||nev>65536||!p.total||p.total>Rate*60)throw std::runtime_error("packet dimensions out of bounds");
 p.low=r.vec(uint64_t(p.nl)*Stride);p.high=r.vec(uint64_t(p.nh)*Stride);p.knots=r.vec(uint64_t(p.nh)*p.nk);p.times=r.vec(p.ne);p.weights=r.vec(p.ne);
 for(uint32_t i=0;i<p.ne;i++)if(p.times[i]<0||(i&&p.times[i]<=p.times[i-1])||p.weights[i]<0||p.weights[i]>1)throw std::runtime_error("invalid mixture envelope");
 for(uint32_t i=0;i<p.ns;i++){std::array<double,ControlCount>a;for(auto&v:a)v=r.number();p.controls.push_back(a);}
 for(uint32_t i=0;i<p.ni;i++){double s=r.number();if(s<0||s>1)throw std::runtime_error("invalid mixture strength");Hit h;h.strength=s;h.low=r.vec(uint64_t(p.nl)*4);h.high=r.vec(uint64_t(p.nh)*4);p.hits.push_back(std::move(h));}
 unsigned strikes=0;bool initial=false;
 for(uint32_t i=0;i<nev;i++){Event e{r.u32(),r.u32(),r.u32()};if(e.frame>=p.total||e.type>1||(i&&e.frame<p.events.back().frame)||e.index>=(e.type?p.ni:p.ns))throw std::runtime_error("invalid event");if(e.type==1){if(!initial)throw std::runtime_error("strike before initial control");strikes++;}else if(e.frame==0)initial=true;p.events.push_back(e);}
 if(!initial||strikes>64||r.remaining)throw std::runtime_error("missing initial snapshot, too many hits, or trailing packet bytes");
 return p;
}
struct Api {
 void*lib=nullptr;
 decltype((void*(*)(const double*,int,const double*,int,const double*,int,int,const double*,const double*,int))nullptr) body_create;
 void(*body_destroy)(void*);void*(*snapshot_prepare)(void*,const double*);void(*snapshot_destroy)(void*);
 void*(*impact_prepare)(void*,const double*,const double*,double);void(*impact_destroy)(void*);
 void*(*scene_create)(void*,int);void(*scene_destroy)(void*);int(*workers_prepare)(void*,int);
 int(*workers_hooks)(void*,int,void*,int(*)(void*,int),void(*)(void*,int));
 int(*scheduler)(void*,int,int,int);int(*install)(void*,void*);int(*strike)(void*,void*);int(*process)(void*,int,double*);int(*reset)(void*);
 void(*stats)(void*,uint64_t*);uint64_t(*worker_allocations)(void*);const char*(*error)();const char*(*backend)();
 template<class T>void sym(T&fn,const char*name){void*p=dlsym(lib,name);if(!p)throw std::runtime_error(std::string("missing engine API: ")+name);static_assert(sizeof(fn)==sizeof(p));std::memcpy(&fn,&p,sizeof(p));}
 explicit Api(const std::string&path){lib=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);if(!lib)throw std::runtime_error(dlerror());try{
 #define S(field,name) sym(field,name)
 S(body_create,"cym19_body_create");S(body_destroy,"cym19_body_destroy");S(snapshot_prepare,"cym19_snapshot_prepare");S(snapshot_destroy,"cym19_snapshot_destroy");S(impact_prepare,"cym19_impact_prepare");S(impact_destroy,"cym19_impact_destroy");S(scene_create,"cym19_scene_create");S(scene_destroy,"cym19_scene_destroy");S(workers_prepare,"cym19_workers_prepare");S(workers_hooks,"cym21_workers_prepare_hooks");S(scheduler,"cym21_scheduler_prepare");S(install,"cym19_install");S(strike,"cym19_strike");S(process,"cym19_process");S(reset,"cym19_reset");S(stats,"cym19_stats");S(worker_allocations,"cym19_worker_allocations");S(error,"interaction_error");S(backend,"cym20_backend_name");
 #undef S
 }catch(...){dlclose(lib);lib=nullptr;throw;}}
 ~Api(){if(lib)dlclose(lib);}Api(const Api&)=delete;
};
struct Trace {uint32_t frame,n;double ms;};
struct Engine {
 Api&a;const Packet&p;void*body=nullptr;void*scene=nullptr;std::vector<void*>snaps,impacts;uint32_t at=0;size_t event=0,nt=0;
 std::vector<double>capture;std::vector<Trace>trace;std::atomic<bool>done{false};std::atomic<int>failure{0};
 Engine(Api&api,const Packet&packet):a(api),p(packet),capture(size_t(p.total)*2),trace(p.total){try{
 body=a.body_create(p.low.data(),p.nl,p.high.data(),p.nh,p.knots.data(),p.nk,p.ks,p.times.data(),p.weights.data(),p.ne);if(!body)fail("body");
 for(auto&v:p.controls){void*q=a.snapshot_prepare(body,v.data());if(!q)fail("snapshot");snaps.push_back(q);}for(auto&h:p.hits){void*q=a.impact_prepare(body,h.low.data(),h.high.data(),h.strength);if(!q)fail("impact");impacts.push_back(q);}
 int capacity=0;for(auto&e:p.events)capacity+=e.type==1;scene=a.scene_create(body,std::max(1,capacity));if(!scene)fail("scene");
 }catch(...){close();throw;}}
 void fail(const char*where){const char*e=a.error();throw std::runtime_error(std::string(where)+": "+(e?e:"unknown error"));}
 void close(){if(scene){a.scene_destroy(scene);scene=nullptr;}for(void*q:impacts)a.impact_destroy(q);impacts.clear();for(void*q:snaps)a.snapshot_destroy(q);snaps.clear();if(body){a.body_destroy(body);body=nullptr;}}
 ~Engine(){close();}
 void configure(int w,int mode,void*ctx=nullptr,int(*enter)(void*,int)=nullptr,void(*leave)(void*,int)=nullptr){if(a.workers_hooks(scene,w,ctx,enter,leave))fail("workers");if(a.scheduler(scene,mode,128,1))fail("scheduler");}
 // No allocations, file I/O or preparation in this function. capture and trace are resident.
 int render(uint32_t n,double*out)noexcept{
 if(n>MaxFrames||n==0||!out){failure.store(20);done.store(true);return 20;}
 std::fill(out,out+size_t(n)*2,0.);if(failure.load()||done.load())return failure.load();
 uint32_t origin=at,end=std::min(p.total,at+n);auto start=std::chrono::steady_clock::now();int rc=0;
 while(at<end&&!rc){while(event<p.events.size()&&p.events[event].frame==at){auto&e=p.events[event++];rc=e.type?a.strike(scene,impacts[e.index]):a.install(scene,snaps[e.index]);if(rc)break;}if(rc)break;
 uint32_t next=end;if(event<p.events.size())next=std::min(next,p.events[event].frame);if(next<=at){rc=21;break;}
 rc=a.process(scene,next-at,out+size_t(at-origin)*2);if(!rc)at=next;}
 for(uint32_t j=0;j<(at-origin)*2;j++)capture[size_t(origin)*2+j]=out[j];
 double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();trace[nt++]={origin,n,ms};
 if(rc)failure.store(rc);
 if(rc||at==p.total)done.store(true,std::memory_order_release);
 return rc;
 }
 void report(const std::string&path,int workers,int mode,bool device,int joins,int policyFailures,int timestampGaps,int overloads){
 std::vector<double>ms;ms.reserve(nt);unsigned misses=0;double sum=0;for(size_t i=0;i<nt;i++){auto&t=trace[i];ms.push_back(t.ms);sum+=t.ms;misses+=t.ms>1000.*t.n/Rate;}
 std::sort(ms.begin(),ms.end());auto pct=[&](double q){return ms.empty()?0:ms[std::min(ms.size()-1,size_t(q*(ms.size()-1)))];};uint64_t stats0[5]{};a.stats(scene,stats0);
 std::ofstream j(path);if(!j)throw std::runtime_error("cannot write report");j.precision(12);
 j<<"{\n  \"device_clocked\": "<<(device?"true":"false")<<", \"rate\": 48000, \"frames\": "<<at<<", \"callbacks\": "<<nt<<",\n  \"workers\": "<<workers<<", \"scheduler_mode\": "<<mode<<", \"backend\": \""<<a.backend()<<"\",\n  \"processing_seconds\": "<<sum/1000<<", \"missed_budgets\": "<<misses<<", \"p50_ms\": "<<pct(.5)<<", \"p99_ms\": "<<pct(.99)<<", \"max_ms\": "<<pct(1)<<",\n  \"worker_joins\": "<<joins<<", \"worker_policy_failures\": "<<policyFailures<<", \"timestamp_gaps\": "<<timestampGaps<<", \"device_overload_notifications\": "<<overloads<<",\n  \"engine_failure\": "<<failure.load()<<", \"audio_cpp_allocations\": "<<stats0[1]<<", \"worker_cpp_allocations\": "<<a.worker_allocations(scene)<<", \"rejected_hits\": "<<stats0[2]<<", \"active_histories\": "<<stats0[3]<<",\n  \"scope\": \"Prepared scripted event packet; no incoming MIDI preparation, automatic history retirement, or real-time guarantee.\"\n}\n";
 std::ofstream raw(path+".f64",std::ios::binary);raw.write(reinterpret_cast<const char*>(capture.data()),size_t(at)*2*8);
 std::ofstream csv(path+".callbacks.csv");csv<<"frame,frames,processing_ms,budget_ms\n";for(size_t i=0;i<nt;i++)csv<<trace[i].frame<<','<<trace[i].n<<','<<trace[i].ms<<','<<1000.*trace[i].n/Rate<<'\n';
 }
};
#ifdef __APPLE__
struct Device {
 AudioUnit unit=nullptr;AudioDeviceID id=0;os_workgroup_t wg=nullptr;Engine&e;UInt32 nominalFrames=0;bool audible=false;double gain=.25;
 std::atomic<int>joins{0},leaves{0},policyFailures{0},overloads{0},changed{0};int gaps=0;bool timestampSeen=false;double expected=0;
 alignas(32)double buffer[MaxFrames*2]{};
 static thread_local os_workgroup_join_token_s token;static thread_local bool joined;
 static int enter(void*ctx,int)noexcept{auto&d=*static_cast<Device*>(ctx);mach_timebase_info_data_t tb;mach_timebase_info(&tb);
 double ticks=(1e9*double(d.nominalFrames)/Rate)*tb.denom/tb.numer;thread_time_constraint_policy_data_t p{};p.period=uint32_t(ticks);p.computation=uint32_t(ticks*.5);p.constraint=uint32_t(ticks);p.preemptible=1;
 auto kr=thread_policy_set(pthread_mach_thread_np(pthread_self()),THREAD_TIME_CONSTRAINT_POLICY,reinterpret_cast<thread_policy_t>(&p),THREAD_TIME_CONSTRAINT_POLICY_COUNT);if(kr!=KERN_SUCCESS){d.policyFailures++;return int(kr);}
 int rc=os_workgroup_join(d.wg,&token);joined=rc==0;if(joined)d.joins++;return rc;}
 static void leave(void*ctx,int)noexcept{auto&d=*static_cast<Device*>(ctx);if(joined){os_workgroup_leave(d.wg,&token);joined=false;d.leaves++;}}
 static OSStatus change(AudioObjectID,UInt32 n,const AudioObjectPropertyAddress*a,void*ctx){auto&d=*static_cast<Device*>(ctx);for(UInt32 i=0;i<n;i++)if(a[i].mSelector==kAudioDeviceProcessorOverload)d.overloads++;else d.changed.store(1);return noErr;}
 static OSStatus callback(void*ctx,AudioUnitRenderActionFlags*,const AudioTimeStamp*ts,UInt32,UInt32 n,AudioBufferList*out){auto&d=*static_cast<Device*>(ctx);
 if(out)for(UInt32 i=0;i<out->mNumberBuffers;i++)if(out->mBuffers[i].mData)std::memset(out->mBuffers[i].mData,0,out->mBuffers[i].mDataByteSize);
 if(d.e.done.load())return noErr;
 if(d.changed.load()||!out||out->mNumberBuffers!=2||n>MaxFrames||n==0){d.e.failure.store(30);d.e.done.store(true);return kAudioUnitErr_CannotDoInCurrentContext;}
 for(int c=0;c<2;c++)if(!out->mBuffers[c].mData||out->mBuffers[c].mDataByteSize<n*4){d.e.failure.store(31);d.e.done.store(true);return kAudioUnitErr_CannotDoInCurrentContext;}
 if(ts&&(ts->mFlags&kAudioTimeStampSampleTimeValid)){if(d.timestampSeen&&std::abs(ts->mSampleTime-d.expected)>.5)d.gaps++;d.expected=ts->mSampleTime+n;d.timestampSeen=true;}
 int rc=d.e.render(n,d.buffer);if(rc)return kAudioUnitErr_CannotDoInCurrentContext;
 if(d.audible){for(UInt32 j=0;j<n;j++)for(int c=0;c<2;c++){double v=d.buffer[2*j+c]*d.gain;if(!std::isfinite(v)||std::abs(v)>.98){d.e.failure.store(32);d.e.done.store(true);for(UInt32 i=0;i<out->mNumberBuffers;i++)std::memset(out->mBuffers[i].mData,0,out->mBuffers[i].mDataByteSize);return noErr;}static_cast<float*>(out->mBuffers[c].mData)[j]=float(v);}}
 return noErr;}
 void check(OSStatus s,const char*name){if(s)throw std::runtime_error(std::string(name)+" OSStatus="+std::to_string(s));}
 explicit Device(Engine&engine):e(engine){try{
 AudioObjectPropertyAddress a{kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};UInt32 size=sizeof(id);check(AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,0,nullptr,&size,&id),"default output");if(!id)throw std::runtime_error("no output device");
 a.mSelector=kAudioDevicePropertyNominalSampleRate;double rate=0;size=sizeof(rate);check(AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&rate),"sample rate");if(rate!=Rate)throw std::runtime_error("Output device must already be at 48000 Hz; this host does not change your device rate.");
 a.mSelector=kAudioDevicePropertyBufferFrameSize;size=sizeof(nominalFrames);check(AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&nominalFrames),"buffer size");if(!nominalFrames||nominalFrames>MaxFrames)throw std::runtime_error("unsupported device buffer (need 1..1024)");
 AudioComponentDescription desc{kAudioUnitType_Output,kAudioUnitSubType_HALOutput,kAudioUnitManufacturer_Apple,0,0};auto comp=AudioComponentFindNext(nullptr,&desc);if(!comp)throw std::runtime_error("AUHAL missing");check(AudioComponentInstanceNew(comp,&unit),"AUHAL create");UInt32 off=0,on=1;
 check(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Input,1,&off,sizeof(off)),"disable input");check(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Output,0,&on,sizeof(on)),"enable output");check(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&id,sizeof(id)),"set output");
 AudioStreamBasicDescription fmt{};fmt.mSampleRate=Rate;fmt.mFormatID=kAudioFormatLinearPCM;fmt.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked|kAudioFormatFlagIsNonInterleaved|kAudioFormatFlagsNativeEndian;fmt.mBytesPerPacket=fmt.mBytesPerFrame=4;fmt.mFramesPerPacket=1;fmt.mChannelsPerFrame=2;fmt.mBitsPerChannel=32;
 check(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&fmt,sizeof(fmt)),"client stream format");UInt32 max=MaxFrames;check(AudioUnitSetProperty(unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&max,sizeof(max)),"maximum block");
 AURenderCallbackStruct cb{callback,this};check(AudioUnitSetProperty(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&cb,sizeof(cb)),"render callback");check(AudioUnitInitialize(unit),"initialize");
 os_workgroup_t borrowed=nullptr;size=sizeof(borrowed);OSStatus gs=AudioUnitGetProperty(unit,kAudioOutputUnitProperty_OSWorkgroup,kAudioUnitScope_Global,0,&borrowed,&size);if(gs==noErr&&borrowed){wg=borrowed;os_retain(wg);} // explicit lifetime through all worker exits; AU also remains alive
 for(auto selector:{kAudioDevicePropertyNominalSampleRate,kAudioDevicePropertyBufferFrameSize,kAudioDeviceProcessorOverload}){a.mSelector=selector;check(AudioObjectAddPropertyListener(id,&a,change,this),"device observer");}
 }catch(...){close();throw;}}
 void close(){if(unit)AudioOutputUnitStop(unit);if(id){AudioObjectPropertyAddress a{0,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};for(auto sel:{kAudioDevicePropertyNominalSampleRate,kAudioDevicePropertyBufferFrameSize,kAudioDeviceProcessorOverload}){a.mSelector=sel;AudioObjectRemovePropertyListener(id,&a,change,this);}}if(wg){os_release(wg);wg=nullptr;}if(unit){AudioUnitUninitialize(unit);AudioComponentInstanceDispose(unit);unit=nullptr;}}
 ~Device(){close();}
};
thread_local os_workgroup_join_token_s Device::token{};thread_local bool Device::joined=false;
#endif
int selftest(){ // No audio device or engine library required; CLI sanity fixture for target compiler.
 static_assert(sizeof(double)==8&&sizeof(uint32_t)==4);std::vector<int>x{4,1,3,2};std::sort(x.begin(),x.end());if(x!=std::vector<int>({1,2,3,4}))return 1;
#ifdef __APPLE__
 AudioComponentDescription d{kAudioUnitType_Output,kAudioUnitSubType_HALOutput,kAudioUnitManufacturer_Apple,0,0};if(!AudioComponentFindNext(nullptr,&d))throw std::runtime_error("AUHAL component unavailable");
#endif
 std::cout<<"{\"host_smoke\":true,\"audio_device_opened\":false,\"full_engine_run\":false}\n";return 0;}
}
int main(int argc,char**argv){try{
 if(argc==2&&std::string(argv[1])=="--selftest")return lab::selftest();
 std::string packet,library,report="native-results.json";int workers=4,block=128,mode=3;bool device=false,audible=false;
 for(int i=1;i<argc;i++){std::string k=argv[i];if(k=="--device")device=true;else if(k=="--audible")audible=true;else{if(i+1==argc)throw std::runtime_error("missing option value");std::string v=argv[++i];if(k=="--packet")packet=v;else if(k=="--library")library=v;else if(k=="--report")report=v;else if(k=="--workers")workers=std::stoi(v);else if(k=="--block")block=std::stoi(v);else if(k=="--scheduler")mode=std::stoi(v);else throw std::runtime_error("unknown option "+k);}}
 if(packet.empty()||library.empty()||workers<1||workers>8||block<1||block>1024||mode<0||mode>3||(audible&&!device))throw std::runtime_error("usage: --packet FILE --library FILE [--workers 1..8 --block 1..1024 --scheduler 0..3 --report FILE] [--device [--audible]]");
 auto p=lab::read_packet(packet);lab::Api a(library);lab::Engine e(a,p);
 if(!device){e.configure(workers,mode);std::array<double,lab::MaxFrames*2>b{};while(!e.done.load()){if(e.render(std::min<uint32_t>(block,p.total-e.at),b.data()))break;}e.report(report,workers,mode,false,0,0,0,0);}
 else{
#ifdef __APPLE__
 lab::Device d(e);d.audible=audible;try{
 if(workers>1&&!d.wg)throw std::runtime_error("No device workgroup; use --workers 1 to test serial explicitly.");
 if(workers>1){int rec=os_workgroup_max_parallel_threads(d.wg,nullptr);std::cerr<<"Audio Workgroup recommended total workers="<<rec<<"; requested="<<workers<<'\n';}
 e.configure(workers,mode,&d,workers>1?lab::Device::enter:nullptr,workers>1?lab::Device::leave:nullptr);d.check(AudioOutputUnitStart(d.unit),"start");
 auto start=std::chrono::steady_clock::now();while(!e.done.load(std::memory_order_acquire)){std::this_thread::sleep_for(std::chrono::milliseconds(10));if(std::chrono::steady_clock::now()-start>std::chrono::seconds(90)){e.failure.store(33);e.done.store(true);break;}}
 d.check(AudioOutputUnitStop(d.unit),"stop");e.report(report,workers,mode,true,d.joins,d.policyFailures,d.gaps,d.overloads);e.close(); // joins must leave before d releases the workgroup/AU
 }catch(...){AudioOutputUnitStop(d.unit);e.close();throw;}
#else
 throw std::runtime_error("--device requires macOS; this Linux build supports offline exactness tests only");
#endif
 }
 std::cout<<"Wrote "<<report<<" and exact float64 capture.\n";return e.failure.load()?2:0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
