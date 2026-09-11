#define main pr45_combined_cli_main
#include "main.native.cpp"
#undef main

#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace final_device {
using namespace combined;
namespace fs = std::filesystem;

struct DeviceScene {
    Engine& e; int voices,instruments,block; std::vector<Config> configs;
    std::vector<std::unique_ptr<mydsp_poly>> synths; std::unique_ptr<::dsp> effect;
    std::vector<Slot> slots; std::unique_ptr<tg::LockFreeMultiThreadedNodePlayer> player;
    juce::AudioBuffer<float> temp{2,maxBlock}; tracktion::engine::MidiMessageArray midi; int64_t clock=0;
    DeviceScene(Engine& engine,int inst,int v,int participants,int grain,int frames,juce::AudioWorkgroup workgroup)
        : e(engine),voices(v),instruments(inst),block(frames),configs(inst) {
        require(frames>0&&frames<=maxBlock,"unsupported device callback size");
        for(int i=0;i<inst;++i){configs[i].optimized=e.a.compute;synths.push_back(std::make_unique<mydsp_poly>(new Voice(e,configs[i],0),v,true,false));synths.back()->init(48000);}
        effect.reset(e.effect.p->createDSPInstance());effect->init(48000);slots.resize(inst*v);
        for(int i=0;i<inst;++i)for(int k=0;k<v;++k)slots[i*v+k].voice=synths[i]->fVoiceTable[k];
        player=std::make_unique<tg::LockFreeMultiThreadedNodePlayer>(tg::getPoolCreatorFunction(tg::ThreadPoolStrategy::lightweightSemHybrid),workgroup);
        // Final candidate: actual processing geometry is established before workers start.
        player->setNumThreads(0);player->setNode(std::make_unique<MixNode>(slots,inst,v,grain),48000,frames);player->setNumThreads(std::size_t(participants-1));
    }
    void notes(int count){for(int i=0;i<instruments;++i)for(int k=0;k<count;++k)synths[i]->keyOn(i,40+k%72,50+k%65);}
    void stateMode(bool b,ps::Compute fresh){for(int i=0;i<instruments;++i){configs[i].b=i==0?b:false;configs[i].editable=false;configs[i].optimized=i==0&&b?(fresh?fresh:e.b.compute):e.a.compute;++configs[i].revision;}}
    void render(juce::AudioBuffer<float>& out,int n){float* outputs[]={out.getWritePointer(0),out.getWritePointer(1)};juce::AudioBuffer<float> view(outputs,2,n);midi.clear();player->process({choc::buffer::FrameCount(n),{clock,clock+n},{tg::toBufferView(view),midi}});float* effected[]={temp.getWritePointer(0),temp.getWritePointer(1)};effect->compute(n,outputs,effected);for(int c=0;c<2;++c)std::copy_n(effected[c],n,outputs[c]);clock+=n;}
};

struct Record { double begin=0,end=0; std::uint64_t host=0; int frames=0,phase=0; };

class Callback final : public juce::AudioIODeviceCallback {
public:
    Callback(Engine& engine,fs::path kernels,fs::path output,int p,bool join):e(engine),root(std::move(kernels)),out(std::move(output)),participants(p),joinGroup(join){}
    ~Callback() override{stopCompiler();}
    void audioDeviceAboutToStart(juce::AudioIODevice* d) override {
        try{
            device=d;rate=d->getCurrentSampleRate();block=d->getCurrentBufferSizeSamples();
            require(std::abs(rate-48000.0)<0.1,"device study requires 48kHz");require(block==128,"device study requires 128-frame callbacks");
            workgroup=d->getWorkgroup();workgroupAvailable=bool(workgroup);workgroupMax=workgroupAvailable?int(workgroup.getMaxParallelThreadCount()):0;require(!joinGroup||workgroupAvailable,"CoreAudio returned no AudioWorkgroup");
            records.assign(capacity,{});capture.assign(capacity*std::size_t(block)*2,0.f);scratch.setSize(2,block,false,false,true);owner=std::make_unique<HandoffOwner>();
            scene=std::make_unique<DeviceScene>(e,4,16,participants,4,block,joinGroup?workgroup:juce::AudioWorkgroup{});scene->notes(16);xrunsStart=d->getXRunCount();
            compiler=std::thread([this]{while(!request.load(std::memory_order_acquire)&&!cancel.load(std::memory_order_acquire))std::this_thread::sleep_for(std::chrono::microseconds(100));if(cancel.load(std::memory_order_acquire))return;try{owner->build=compileLive(root,out,e.pb,"device-B",false);if(owner->build.code!=0){owner->built.store(-1,std::memory_order_release);return;}prepareCode(*owner,e,owner->build.binary,block);owner->published=now();owner->ready.store(&owner->code,std::memory_order_release);owner->built.store(2,std::memory_order_release);}catch(const std::exception& x){owner->error=x.what();owner->built.store(-1,std::memory_order_release);}});
            started.store(true,std::memory_order_release);
        }catch(const std::exception& x){error=x.what();done.store(true,std::memory_order_release);}
    }
    void audioDeviceIOCallbackWithContext(const float* const*,int,float* const* output,int outputs,int samples,const juce::AudioIODeviceCallbackContext& context) override {
        for(int c=0;c<outputs;++c)if(output[c])std::fill_n(output[c],samples,0.f); // physical output always silent
        if(done.load(std::memory_order_acquire)||!scene)return;
        if(samples!=block){error="CoreAudio callback size changed";done.store(true,std::memory_order_release);return;}
        const auto index=completed.load(std::memory_order_relaxed);if(index>=capacity){error="device capture capacity exhausted";done.store(true,std::memory_order_release);return;}
        auto& row=records[index];row.begin=now();row.host=context.hostTimeNs?*context.hostTimeNs:0;row.frames=samples;
        if(index==64){requestUs=now();request.store(true,std::memory_order_release);}
        if(!adopted){if(auto* code=owner->ready.load(std::memory_order_acquire);code&&code->revision==1&&code->compute){scene->stateMode(true,code->compute);adopted=true;adoptIndex=index;readyUs=now();}}
        row.phase=index<64?0:adopted?2:1;scene->render(scratch,samples);
        auto base=index*std::size_t(samples)*2;for(int s=0;s<samples;++s)for(int c=0;c<2;++c)capture[base+std::size_t(s)*2+c]=scratch.getSample(c,s);
        row.end=now();completed.store(index+1,std::memory_order_release);if(adopted&&++postBlocks>=128)done.store(true,std::memory_order_release);
    }
    void audioDeviceStopped() override {if(device)xrunsEnd=device->getXRunCount();stopCompiler();scene.reset();workgroup.reset();device=nullptr;}
    void audioDeviceError(const juce::String& s) override {error=s.toStdString();done.store(true,std::memory_order_release);}
    bool isDone()const{return done.load(std::memory_order_acquire);} int xrunNow()const{return device?device->getXRunCount():-1;}
    void finalize(){
        stopCompiler();require(error.empty(),"audio device error: "+error);require(owner&&owner->built.load(std::memory_order_acquire)==2,"prepared device compile failed: "+(owner?owner->error:std::string("owner missing")));require(adopted&&postBlocks>=128,"prepared code was not adopted on device");
        auto n=completed.load();capture.resize(n*std::size_t(block)*2);records.resize(n);
        Scene reference(e,4,16,2,1,1,false),unchanged(e,4,16,2,1,1,false);reference.notes(16);unchanged.notes(16);juce::AudioBuffer<float> ref(2,maxBlock),neg(2,maxBlock);std::vector<float> expected,noedit;expected.reserve(capture.size());noedit.reserve(capture.size());
        for(std::size_t k=0;k<n;++k){if(k==adoptIndex)reference.stateMode(true,true);reference.render(ref,block);unchanged.render(neg,block);append(expected,unchecked(ref,block));append(noedit,unchecked(neg,block));}
        compare(capture,expected);double changed=0;for(std::size_t i=adoptIndex*std::size_t(block)*2;i<capture.size();++i)changed=std::max(changed,std::abs(double(capture[i])-noedit[i]));require(changed>1e-4,"device adoption produced no observable B audio");
        const double periodUs=block*1e6/rate;std::vector<double> times;times.reserve(n);int over=0;double maxHostMs=0;std::uint64_t prev=0;for(std::size_t k=0;k<n;++k){double t=records[k].end-records[k].begin;times.push_back(t);over+=t>periodUs;if(prev&&records[k].host)maxHostMs=std::max(maxHostMs,double(records[k].host-prev)/1e6);if(records[k].host)prev=records[k].host;}
        std::sort(times.begin(),times.end());auto percentile=[&](double q){return times[std::min(times.size()-1,std::size_t(std::ceil(q*times.size()))-1)];};int adoptionOver=(records[adoptIndex].end-records[adoptIndex].begin)>periodUs;
        V r=obj();prop(r,"passed",true);prop(r,"participants",participants);prop(r,"workgroup_requested",joinGroup);prop(r,"workgroup_available",workgroupAvailable);prop(r,"workgroup_max_parallel",workgroupMax);prop(r,"sample_rate",rate);prop(r,"block",block);prop(r,"callbacks",double(n));prop(r,"over_budget",over);prop(r,"over_budget_pct",100.0*over/n);prop(r,"median_us",times[times.size()/2]);prop(r,"p99_us",percentile(.99));prop(r,"maximum_us",times.back());prop(r,"max_host_interval_ms",maxHostMs);prop(r,"adoption_callback_us",records[adoptIndex].end-records[adoptIndex].begin);prop(r,"adoption_over_budget",adoptionOver);prop(r,"request_to_adoption_ms",(readyUs-requestUs)/1000);prop(r,"compile_ms",(owner->build.finished-owner->build.started)/1000);prop(r,"changed_vs_old",changed);prop(r,"xrun_start",xrunsStart);prop(r,"xrun_end",xrunsEnd);prop(r,"xrun_delta",(xrunsStart>=0&&xrunsEnd>=0)?xrunsEnd-xrunsStart:-1);writeJson(out/"device-result.json",r);raw(out/"device-internal.f32",capture);raw(out/"device-reference.f32",expected);
    }
    int xrunsEnd=-1;
private:
    void stopCompiler(){cancel.store(true,std::memory_order_release);if(compiler.joinable())compiler.join();}
    static constexpr std::size_t capacity=4096;Engine& e;fs::path root,out;int participants;bool joinGroup;juce::AudioIODevice* device=nullptr;double rate=0;int block=0;juce::AudioWorkgroup workgroup;bool workgroupAvailable=false;int workgroupMax=0;std::unique_ptr<HandoffOwner> owner;std::unique_ptr<DeviceScene> scene;juce::AudioBuffer<float> scratch;std::vector<Record> records;std::vector<float> capture;std::thread compiler;std::atomic<bool> request{false},cancel{false},started{false},done{false};std::atomic<std::size_t> completed{0};bool adopted=false;std::size_t adoptIndex=0;int postBlocks=0,xrunsStart=-1;double requestUs=0,readyUs=0;std::string error;
};

struct SetupRestorer {juce::AudioDeviceManager& manager;juce::AudioDeviceManager::AudioDeviceSetup setup;~SetupRestorer(){manager.setAudioDeviceSetup(setup,true);}};
static void run(const fs::path& root,const fs::path& out,const std::string& family,int stages,int participants,bool workgroup){
    require(participants==4||participants==8,"device study participants must be 4 or 8");fs::create_directories(out);Engine e(root,family,stages);juce::ScopedJuceInitialiser_GUI juce;juce::AudioDeviceManager manager;auto init=manager.initialise(0,2,nullptr,true);require(init.isEmpty(),"audio initialise: "+init.toStdString());auto* initial=manager.getCurrentAudioDevice();require(initial,"no CoreAudio output device");auto original=manager.getAudioDeviceSetup();SetupRestorer restore{manager,original};auto requested=original;requested.sampleRate=48000;requested.bufferSize=128;auto setup=manager.setAudioDeviceSetup(requested,true);require(setup.isEmpty(),"48k/128 device setup unavailable: "+setup.toStdString());
    Callback callback(e,root,out,participants,workgroup);manager.addAudioCallback(&callback);auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);while(!callback.isDone()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));require(callback.isDone(),"device test timeout");callback.xrunsEnd=callback.xrunNow();manager.removeAudioCallback(&callback);callback.finalize();
}
}

int main(int argc,char** argv){try{require(argc==7,"FinalDeviceBench family stages participants workgroup(0/1) kernels output");const std::string family=argv[1];int stages=std::stoi(argv[2]),participants=std::stoi(argv[3]);bool wg=std::stoi(argv[4])!=0;std::filesystem::path root=std::filesystem::absolute(argv[5]),out=std::filesystem::absolute(argv[6]);final_device::run(root,out,family,stages,participants,wg);std::cout<<"FINAL_DEVICE_PASS "<<family<<' '<<participants<<' '<<wg<<'\n';return 0;}catch(const std::exception& e){std::cerr<<"FINAL_DEVICE_FAILURE "<<e.what()<<'\n';return 2;}}
