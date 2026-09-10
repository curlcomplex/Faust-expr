// Additive combined experiment. The actual earlier graph builder and state ABI
// remain the source of fixtures and generated DSP, not rewritten audio math.
#include "previous.generated.inc"
#include <faust/dsp/poly-dsp.h>
#include <tracktion_graph/tracktion_graph.h>
#include <atomic>
#include <future>
#include <unordered_set>

std::list<GUI*> GUI::fGuiList;
GUI::ztimedmap GUI::gTimedZoneMap;

namespace combined {
using namespace ps_test;
namespace tg=tracktion::graph;
static constexpr int maxBlock=512;

static Fixture voiceFixture(const std::string& family,int n){
    auto f=make(family,n);auto& source=f.graph.modulesMutable()[0];
    source.code="import(\"stdfaust.lib\"); freq=hslider(\"freq\",440,20,20000,0.01); gain=hslider(\"gain\",0,0,1,0.001); gate=button(\"gate\"); env=en.adsr(0.002,0.006,0.65,0.09,gate); process=os.osc(freq)*env*gain*0.12,os.osc(freq*1.007)*env*gain*0.10;";
    source.params.clear();source.paramValues.clear();
    for(auto name:{"freq","gain","gate"}){curlop::ParamSchemaEntry p;p.name=name;p.sourceId=name;p.min=name==std::string("freq")?20:0;p.max=name==std::string("freq")?20000:1;p.defaultValue=name==std::string("freq")?440:0;source.params.push_back(p);source.paramValues[name]=p.defaultValue;}
    return f;
}
static GraphState editedGraph(const Fixture& f){
    auto g=f.graph;const int target=f.output-1;
    // New internal route, not a cached topology toggle. Parallel has source->all,
    // so use branch1->last; serial/feedback use source->last.
    bool already=false;for(auto& e:g.edges())already|=e.srcIndex==0&&e.tgtIndex==target;
    require(g.addEdge(edge(already?1:0,target,0.25f)),"new instrument connection rejected");return g;
}
static void exportVoice(const fs::path& out,const std::string& family,int n){
    auto f=voiceFixture(family,n);V m=obj();prop(m,"family",family);prop(m,"size",n);prop(m,"output",f.output);juce::Array<V> modules;
    for(auto entry:f.graph.modules()){
        if(entry.lineageId=="core.output"){
            entry.code="process=_,_;";entry.params.clear();
        }
        V r=obj();prop(r,"index",entry.index);prop(r,"id",entry.moduleId);prop(r,"source",entry.code);prop(r,"inputs",int(entry.audioInputs.size())*2);juce::Array<V> params;
        for(auto& p:entry.params){V x=obj();prop(x,"label",p.sourceId);prop(x,"value",p.defaultValue);params.add(x);}prop(r,"params",params);modules.add(r);
    }
    prop(m,"modules",modules);V graphs=obj();prop(graphs,"A",graphJson(f.graph));prop(graphs,"B",graphJson(editedGraph(f)));prop(m,"graphs",graphs);writeJson(out/"graph.json",m);
}
struct Factory {
    llvm_dsp_factory* p=nullptr;
    Factory(const std::string& name,const std::string& code){std::string error;p=createDSPFactoryFromString(name,code,0,nullptr,"",error,-1);require(p,"LLVM factory: "+error);}
    ~Factory(){if(p)deleteDSPFactory(p);}Factory(const Factory&)=delete;
};
struct Config {bool b=false;bool editable=false;ps::Compute optimized=nullptr;std::uint64_t revision=0;};
struct Engine {
    Fixture fixture;V manifest;Library a,b;Plan pa,pb;Factory stockA,stockB,effect;int planes,output;
    static std::string file(const fs::path& p){std::ifstream f(p);std::ostringstream o;o<<f.rdbuf();require(bool(f),"source unavailable");return o.str();}
    Engine(const fs::path& root,const std::string& family,int n):fixture(voiceFixture(family,n)),manifest(read(root/"manifest.json")),a(root/"kernel-A.dylib"),b(root/"kernel-B.dylib"),pa(plan(fixture.graph)),pb(plan(editedGraph(fixture))),stockA("poly-whole-A",file(root/"whole-A.dsp")),stockB("poly-whole-B",file(root/"whole-B.dsp")),effect("shared-post-voice-effect","import(\"stdfaust.lib\"); fx(x)=x*0.8+(x : de.delay(16384,257))*0.2; process=fi.lowpass(2,8300),fi.lowpass(2,8700) : fx,fx;"),planes(4*int(fixture.graph.modules().size())),output(fixture.output){require(std::string(a.schema())==b.schema()&&a.bytes()==b.bytes(),"state schema mismatch");}
};
// One object is one real note voice. It owns private state and output/tap storage.
// Backend0=persistent state,1=ordinary full-Faust LLVM,2=independent module LLVM.
class Voice final:public ::dsp {
    Engine& e;Config& config;int backend;int sr=48000;
    std::unique_ptr<State> state;std::unique_ptr<::dsp> stock;MapUI stockUI;
    std::unique_ptr<ModuleLLVM> independent;Audio audio;float values[3]={440,0,0};float* zones[3]={};double meter=0;
    static const char* label(int i){return std::array<const char*,3>{"m0_freq","m0_gain","m0_gate"}[i];}
    void reset(){
        if(backend==0){state=std::make_unique<State>(e.a);for(int i=0;i<3;++i){zones[i]=e.a.zone(state->ptr,0,label(i));require(zones[i],"state control absent");}}
        if(backend==1){stock.reset((config.b?e.stockB:e.stockA).p->createDSPInstance());stock->init(sr);stock->buildUserInterface(&stockUI);for(int i=0;i<3;++i){zones[i]=stockUI.getParamZone(label(i));require(zones[i],"whole control absent");}}
        if(backend==2)independent=std::make_unique<ModuleLLVM>(e.manifest);
    }
public:
    std::uint64_t framesProcessed=0;
    Voice(Engine& engine,Config& c,int kind):e(engine),config(c),backend(kind),audio(engine.planes){reset();}
    int getNumInputs() override{return 0;}int getNumOutputs() override{return 2;}int getSampleRate() override{return sr;}
    void metadata(Meta*) override{}
    void buildUserInterface(UI* ui) override{ui->openVerticalBox("voice");ui->addHorizontalSlider("freq",&values[0],440,20,20000,.01f);ui->addHorizontalSlider("gain",&values[1],0,0,1,.001f);ui->addButton("gate",&values[2]);ui->closeBox();}
    void init(int rate) override{require(rate==48000,"fixed-rate experiment");sr=rate;instanceResetUserInterface();reset();framesProcessed=0;}
    void instanceInit(int rate) override{init(rate);}void instanceConstants(int rate) override{require(rate==sr,"sample-rate change unsupported");}
    void instanceResetUserInterface() override{values[0]=440;values[1]=values[2]=0;}
    void instanceClear() override{reset();framesProcessed=0;}
    ::dsp* clone() override{return new Voice(e,config,backend);}
    const void* stateIdentity()const{return state?state->ptr:stock.get();}
    void compute(int count,FAUSTFLOAT**,FAUSTFLOAT** outputs) override{
        if(count==0)return;require(count>0&&count<=maxBlock,"bounded compute");
        if(backend==2){for(int i=0;i<3;++i)independent->parameter(label(i),values[i]);independent->render(config.b?e.pb:e.pa,audio,count);}
        else {for(int i=0;i<3;++i)*zones[i]=values[i];if(backend==1)stock->compute(count,nullptr,audio.outputPointers.data());else if(config.editable)dynamic(e.a,state->ptr,config.b?e.pb:e.pa,audio,count);else config.optimized(state->ptr,count,audio.outputPointers.data());}
        // Same required per-voice taps and peak/RMS work in every arm. Private sink:
        // the previous shared volatile benchmark sink must not race across workers.
        double sum=0;for(auto* ch:audio.outputPointers){double squares=0;float peak=0;for(int s=0;s<count;++s){float x=ch[s];squares+=double(x)*x;peak=std::max(peak,std::abs(x));}sum+=peak+std::sqrt(squares/count);}meter=sum;
        for(int c=0;c<2;++c)std::copy_n(audio.outputPointers[2*e.output+c],count,outputs[c]);framesProcessed+=count;
    }
};
struct Slot {dsp_voice* voice=nullptr;std::array<std::array<float,maxBlock>,2> data{};bool active=false,legato=false;std::uint64_t thread=0;};
static void renderSlot(Slot& slot,int count){
    auto& v=*slot.voice;slot.active=v.fCurNote!=kFreeVoice;slot.legato=v.fCurNote==kLegatoVoice;
    if(!slot.active)return;slot.thread=std::hash<std::thread::id>{}(std::this_thread::get_id());float* out[]={slot.data[0].data(),slot.data[1].data()};
    if(slot.legato){
        v.computeLegato(count,nullptr,out);int half=count/2;
        for(int c=0;c<2;++c){double factor=1,step=half?1.0/half:0;for(int s=0;s<half;++s){out[c][s]*=factor;factor-=step;}}
#if POLY_HAS_FADE_IN
        for(int c=0;c<2;++c){double factor=0,step=half?1.0/half:0;for(int s=0;s<half;++s){out[c][half+s]*=factor;factor+=step;}}
#endif
    }else v.compute(count,nullptr,out);
}
class BatchNode final:public tg::Node{
    std::vector<Slot>& slots;int first,last;
public:
    BatchNode(std::vector<Slot>& s,int begin,int end):slots(s),first(begin),last(end){setOptimisations({tg::ClearBuffers::no,tg::AllocateAudioBuffer::no});}
    tg::NodeProperties getNodeProperties() override{return {false,false,0,0,std::size_t(first+1)};}
    bool isReadyToProcess() override{return true;}
    void process(ProcessContext& pc) override{for(int i=first;i<last;++i)renderSlot(slots[i],int(pc.numSamples));}
};
class MixNode final:public tg::Node{
    std::vector<std::unique_ptr<tg::Node>> jobs;std::vector<Slot>& slots;int instruments,voices;
public:
    MixNode(std::vector<Slot>& s,int inst,int v,int grain):slots(s),instruments(inst),voices(v){for(int i=0;i<int(s.size());i+=grain)jobs.push_back(std::make_unique<BatchNode>(s,i,std::min(i+grain,int(s.size()))));}
    tg::NodeProperties getNodeProperties() override{return {true,false,2,0,100000};}
    std::vector<tg::Node*> getDirectInputNodes() override{std::vector<tg::Node*> result;for(auto& p:jobs)result.push_back(p.get());return result;}
    bool isReadyToProcess() override{for(auto& p:jobs)if(!p->hasProcessed())return false;return true;}
    void process(ProcessContext& pc) override{
        auto out=tg::toAudioBuffer(pc.buffers.audio);int count=int(pc.numSamples);out.clear();
        std::array<std::array<float,maxBlock>,2> instrument{};
        for(int inst=0;inst<instruments;++inst){for(auto& ch:instrument)std::fill_n(ch.data(),count,0.f);
            for(int index=0;index<voices;++index){auto& s=slots[inst*voices+index];if(!s.active)continue;float squares=0;
                for(int c=0;c<2;++c)for(int k=0;k<count;++k){float sample=s.data[c][k];squares+=sample*sample;instrument[c][k]+=sample;}
                s.voice->fLevel=std::sqrt(squares/(count*2));
                if(!s.legato&&s.voice->fCurNote==kReleaseVoice&&s.voice->fLevel<VOICE_STOP_LEVEL)s.voice->fCurNote=kFreeVoice;
            }
            for(int c=0;c<2;++c)for(int k=0;k<count;++k)out.addSample(c,k,instrument[c][k]/float(instruments));
        }
    }
};
struct Scene {
    Engine& e;int voices,instruments;std::vector<Config> configs;
    std::vector<std::unique_ptr<mydsp_poly>> synths;std::unique_ptr<::dsp> effect;
    std::vector<Slot> slots;std::unique_ptr<tg::LockFreeMultiThreadedNodePlayer> player;
    juce::AudioBuffer<float> temp{2,maxBlock};tracktion::engine::MidiMessageArray midi;int64_t clock=0;
    Scene(Engine& engine,int inst,int v,int backend,int participants,int grain,bool usePool):e(engine),voices(v),instruments(inst),configs(inst){
        for(int i=0;i<inst;++i){configs[i].optimized=e.a.compute;synths.push_back(std::make_unique<mydsp_poly>(new Voice(e,configs[i],backend),v,true,false));synths.back()->init(48000);}
        effect.reset(e.effect.p->createDSPInstance());effect->init(48000);
        if(usePool){slots.resize(inst*v);for(int i=0;i<inst;++i)for(int k=0;k<v;++k)slots[i*v+k].voice=synths[i]->fVoiceTable[k];
            player=std::make_unique<tg::LockFreeMultiThreadedNodePlayer>(tg::getPoolCreatorFunction(tg::ThreadPoolStrategy::lightweightSemHybrid));
            player->setNumThreads(std::size_t(participants-1));player->setNode(std::make_unique<MixNode>(slots,inst,v,grain),48000,maxBlock);
        }
    }
    void notes(int count){for(int i=0;i<instruments;++i)for(int k=0;k<count;++k)synths[i]->keyOn(i,40+k%72,50+k%65);}
    void stateMode(bool b,bool edit,bool whole=false,ps::Compute fresh=nullptr){for(int i=0;i<instruments;++i){configs[i].b=i==0?b:false;configs[i].editable=(i==0||whole)?edit:false;configs[i].optimized=i==0&&b?(fresh?fresh:e.b.compute):e.a.compute;++configs[i].revision;}}
    void render(juce::AudioBuffer<float>& out,int n){
        float* outputs[]={out.getWritePointer(0),out.getWritePointer(1)};
        if(player){juce::AudioBuffer<float> view(outputs,2,n);midi.clear();player->process({choc::buffer::FrameCount(n),{clock,clock+n},{tg::toBufferView(view),midi}});}
        else {out.clear();float* tmp[]={temp.getWritePointer(0),temp.getWritePointer(1)};for(auto& poly:synths){poly->compute(n,nullptr,tmp);for(int c=0;c<2;++c)for(int k=0;k<n;++k)outputs[c][k]+=tmp[c][k]/float(instruments);}}
        // Exactly one shared post-mix effect, not one reverb/delay per voice.
        effect->compute(n,outputs,outputs);clock+=n;
    }
    int active()const{int result=0;for(auto& p:synths)for(auto* v:p->fVoiceTable)result+=v->fCurNote!=kFreeVoice;return result;}
    std::vector<const void*> identities()const{std::vector<const void*> r;for(auto& p:synths)for(auto* v:p->fVoiceTable){auto* d=dynamic_cast<Voice*>(v->getDSP());require(d,"voice adapter identity");r.push_back(d->stateIdentity());}return r;}
    std::size_t observedThreads()const{std::set<std::uint64_t> ids;for(auto& s:slots)if(s.thread)ids.insert(s.thread);return ids.size();}
};
static void compareVoices(const Scene& a,const Scene& b){for(int i=0;i<a.instruments;++i)for(int k=0;k<a.voices;++k){auto* x=a.synths[i]->fVoiceTable[k];auto* y=b.synths[i]->fVoiceTable[k];require(x->fCurNote==y->fCurNote&&x->fNextNote==y->fNextNote&&x->fDate==y->fDate,"voice allocation/lifecycle diverged");}}
static void event(Scene& s,int sample){
    if(sample==0)s.notes(std::max(1,s.voices/2));
    if(sample==13)for(auto& p:s.synths)p->keyOn(0,91,100);
    if(sample==1249)for(auto& p:s.synths)p->keyOff(0,40,0);
    if(sample==2049)for(auto& p:s.synths)for(int k=0;k<s.voices+3;++k)p->keyOn(0,60+k,90);
    if(sample==3001)for(auto& p:s.synths)for(auto* v:p->fVoiceTable)if(v->fCurNote>=0)v->setParamValue("freq",dsp_voice::midiToFreq(v->fCurNote)*1.012);
    if(sample==4096)s.stateMode(true,true);
    if(sample==5120)s.stateMode(true,false);
    if(sample==8192)s.stateMode(false,true);
    if(sample==9216)s.stateMode(false,false);
    if(sample==10003)for(auto& p:s.synths)p->allNotesOff(false);
    if(sample==24000)s.notes(s.voices);
    if(sample==30007)for(auto& p:s.synths)p->allNotesOff(true);
}
static void conformance(Engine& e,const fs::path& out,int participants,int grain,int block){
    Scene candidate(e,2,8,0,participants,grain,true),reference(e,2,8,2,1,1,false);
    auto identities=candidate.identities();juce::AudioBuffer<float>a(2,maxBlock),b(2,maxBlock);std::vector<float>x,y;int offset=0;std::vector<int> boundaries={0,13,1249,2049,3001,4096,5120,8192,9216,10003,24000,30007,48000};
    std::ofstream trace(out/"voice-states.tsv");trace<<"sample\tframes\tactive\n";
    while(offset<48000){if(std::find(boundaries.begin(),boundaries.end(),offset)!=boundaries.end()){event(candidate,offset);event(reference,offset);}
        auto next=*std::upper_bound(boundaries.begin(),boundaries.end(),offset);int n=std::min(block,next-offset);
        candidate.render(a,n);reference.render(b,n);
        juce::AudioBuffer<float> av(a.getArrayOfWritePointers(),2,n),bv(b.getArrayOfWritePointers(),2,n);auto aa=interleave(av),bb=interleave(bv);compare(aa,bb);compareVoices(candidate,reference);append(x,aa);append(y,bb);
        require(candidate.identities()==identities,"wiring change replaced a note state");trace<<offset<<'\t'<<n<<'\t'<<candidate.active()<<'\n';offset+=n;
        if(offset==24000)require(candidate.active()==0,"release voices failed to retire");
    }
    raw(out/"candidate.f32",x);raw(out/"independent-llvm.f32",y);V r=obj();prop(r,"frames",48000);prop(r,"workers_observed",double(candidate.observedThreads()));prop(r,"allocations_unchanged",true);prop(r,"all_notes_retired",candidate.active()==0);writeJson(out/"result.json",r);
}
static void benchmark(Engine& e,const fs::path& out,int participants,int grain,int block,int voices){
    // Keep the genuine stock mydsp_poly + whole-LLVM baseline; compare host cost
    // and shared-state cost separately, on the exact same notes and observers.
    Scene llvm(e,4,voices,1,1,1,false),serial(e,4,voices,0,1,1,false),pool(e,4,voices,0,participants,grain,true);
    std::array<Scene*,3> scenes={&llvm,&serial,&pool};for(auto* s:scenes)s->notes(voices);
    juce::AudioBuffer<float>a(2,maxBlock),b(2,maxBlock);for(int k=0;k<24;++k){llvm.render(a,block);for(auto* s:{&serial,&pool}){s->render(b,block);juce::AudioBuffer<float> av(a.getArrayOfWritePointers(),2,block),bv(b.getArrayOfWritePointers(),2,block);compare(interleave(av),interleave(bv));compareVoices(*s,llvm);}}
    std::ofstream rows(out/"timing.tsv");rows<<"trial\tarm\tmode\tvoices\tns_per_frame\n"<<std::setprecision(15);std::mt19937 rng(91713);
    for(std::string mode:{"optimized-A","whole-fallback-B","local-fallback-B","optimized-B"}){
        bool bMode=mode!="optimized-A",editable=mode.find("fallback")!=std::string::npos,whole=mode=="whole-fallback-B";
        for(auto* s:{&serial,&pool})s->stateMode(bMode,editable,whole);
        // Standard LLVM A is timed only for A; it is not silently used as a B oracle.
        for(int trial=0;trial<9;++trial){std::vector<int> order=mode=="optimized-A"?std::vector<int>{0,1,2}:std::vector<int>{1,2};std::shuffle(order.begin(),order.end(),rng);
            for(int arm:order){auto start=now();for(int k=0;k<64;++k)scenes[arm]->render(a,block);double end=now();rows<<trial<<'\t'<<std::array<const char*,3>{"stock-poly-whole-LLVM","stock-poly-shared-state","tracktion-pool-shared-state"}[arm]<<'\t'<<mode<<'\t'<<4*voices<<'\t'<<(end-start)*1000/(64.0*block)<<'\n';rows.flush();}}
    }
    V r=obj();prop(r,"voices_per_instrument",voices);prop(r,"instruments",4);prop(r,"participants",participants);prop(r,"grain",grain);prop(r,"frames_per_block",block);prop(r,"workers_observed",double(pool.observedThreads()));prop(r,"shared_effect_instances",1);writeJson(out/"result.json",r);
}
static void liveTest(Engine& e,const fs::path& root,const fs::path& out,int participants,int grain,int block,int voices,const std::string& policy){
    Scene scene(e,4,voices,0,participants,grain,true);scene.notes(voices);auto ids=scene.identities();
    juce::AudioBuffer<float> audio(2,maxBlock);std::vector<float> capture;capture.reserve(48000*8*2);
    std::vector<int> acceptedB;std::ofstream trace(out/"callbacks.tsv");trace<<"sample\tframes\tphase\tbegin_us\tend_us\n"<<std::setprecision(16);
    auto start=Clock::now();auto deadline=start+std::chrono::seconds(10);auto next=start;auto period=std::chrono::nanoseconds(std::llround(1e9*block/48000.0));
    std::future<CompileResult> compiling;std::unique_ptr<Library> compiled;double request=0,ready=0;bool sent=false;int optimizedBlocks=0;CompileResult evidence;int offset=0;
    while(Clock::now()<deadline&&offset<48000*8){
        if(!sent&&offset>=4096){request=now();if(policy!="deferred")scene.stateMode(true,true,policy=="whole");compiling=std::async(std::launch::async,[&]{return compileLive(root,out,e.pb,"new-instrument-B");});sent=true;}
        if(sent&&!compiled&&compiling.wait_for(std::chrono::seconds(0))==std::future_status::ready){evidence=compiling.get();require(evidence.code==0,"background instrument compile failed");compiled=std::make_unique<Library>(evidence.binary);require(std::string(compiled->schema())==e.a.schema(),"schema incompatibility");scene.stateMode(true,false,false,compiled->compute);ready=now();}
        std::this_thread::sleep_until(next);auto begin=now();scene.render(audio,block);auto end=now();std::string phase=!sent?"before":compiled?"optimized-B":policy=="deferred"?"pending-A":"editable-B";
        trace<<offset<<'\t'<<block<<'\t'<<phase<<'\t'<<begin<<'\t'<<end<<'\n';acceptedB.push_back(scene.configs[0].b);juce::AudioBuffer<float> view(audio.getArrayOfWritePointers(),2,block);append(capture,interleave(view));offset+=block;next+=period;
        require(scene.identities()==ids,"live code change reset a note state");if(compiled&&++optimizedBlocks==64)break;
    }
    require(compiled&&optimizedBlocks==64,"live benchmark bound reached before adoption");trace.flush();
    // Match every recorded sample with an independent continuing module-LLVM
    // oracle, after timing ends. No doubling of real-time work for verification.
    Scene reference(e,4,voices,2,1,1,false);reference.notes(voices);std::vector<float> expected;expected.reserve(capture.size());
    for(std::size_t k=0;k<acceptedB.size();++k){reference.stateMode(acceptedB[k],true);reference.render(audio,block);juce::AudioBuffer<float> view(audio.getArrayOfWritePointers(),2,block);auto y=interleave(view);std::vector<float>x(capture.begin()+k*block*2,capture.begin()+(k+1)*block*2);compare(x,y);append(expected,y);}
    raw(out/"live.f32",capture);raw(out/"reference.f32",expected);V r=obj();prop(r,"policy",policy);prop(r,"request_us",request);prop(r,"ready_us",ready);prop(r,"compile_begin_us",evidence.started);prop(r,"compile_end_us",evidence.finished);prop(r,"voices",4*voices);prop(r,"participants",participants);prop(r,"grain",grain);prop(r,"sample_frames",offset);prop(r,"compiled_instruments",1);prop(r,"unaffected_optimized_instruments",policy=="whole"?V(0):V(3));prop(r,"state_objects_replaced",V(0));prop(r,"workers_observed",double(scene.observedThreads()));writeJson(out/"result.json",r);
}
} // namespace combined
int main(int argc,char** argv){try{require(argc==10,"CombinedPolyBench mode family stages block voices participants grain kernels output");std::string mode=argv[1],family=argv[2];int n=std::stoi(argv[3]),block=std::stoi(argv[4]),voices=std::stoi(argv[5]),participants=std::stoi(argv[6]),grain=std::stoi(argv[7]);fs::path root=fs::absolute(argv[8]),out=fs::absolute(argv[9]);fs::create_directories(out);require(n>=4&&n<=32&&block>=1&&block<=512&&voices>=1&&voices<=32&&participants>=1&&participants<=8&&(grain==1||grain==4||grain==8),"bounded experiment inputs");
    if(mode=="export")combined::exportVoice(out,family,n);else{combined::Engine engine(root,family,n);if(mode=="conformance")combined::conformance(engine,out,participants,grain,block);else if(mode=="benchmark")combined::benchmark(engine,out,participants,grain,block,voices);else if(mode=="local"||mode=="whole"||mode=="deferred")combined::liveTest(engine,root,out,participants,grain,block,voices,mode);else throw std::runtime_error("unknown experiment");}
    std::cout<<"COMBINED_NATIVE_PASS "<<mode<<' '<<family<<' '<<n<<' '<<block<<' '<<voices<<' '<<participants<<' '<<grain<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"COMBINED_NATIVE_FAILURE "<<e.what()<<'\n';return 2;}}
