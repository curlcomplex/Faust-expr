// Additive experiment over the existing real GraphState/Faust-renderer fixture.
#define main inherited_fixture_main
#include "../../vendor/curlop-latency/scripts/bench/patching_latency/main.cpp"
#undef main
#include "graph/transport/PreparedSignalSchedule.h"
#include <faust/dsp/llvm-dsp.h>
#include <faust/gui/MapUI.h>
#include <dlfcn.h>
#include <map>
#include <set>
#include <random>
#include <numeric>
#include "api.h"

namespace ps_test {
using curlop::GraphState;
using curlop::transport::PreparedSignalSchedule;
using curlop::transport::PreparedSignalRouteDefinition;
using V=juce::var;using O=juce::DynamicObject;
static V obj(){return V(new O);}
static void prop(V& x,const char* k,V v){x.getDynamicObject()->setProperty(k,std::move(v));}
static void prop(V& x,const char* k,const std::string& v){prop(x,k,V(juce::String(v)));}
static void prop(V& x,const char* k,const char* v){prop(x,k,V(v));}
static V read(const fs::path& p){return juce::JSON::parse(juce::File(p.string()));}
static void writeJson(const fs::path& p,const V& v){std::ofstream(p)<<juce::JSON::toString(v,true).toStdString()<<'\n';}
static std::string text(const V& v){return v.toString().toStdString();}
static Fixture make(const std::string& family,int n){
    auto f=fixture(family=="parallel"?"parallel":family=="feedback"?"feedback":family=="control"?"control":"serial",n);
    auto& modules=f.graph.modulesMutable();
    auto& s=modules[0];
    if(family=="ben"){
        s.code="import(\"stdfaust.lib\"); gain=hslider(\"gain\",1,0,1,0.001); process=os.osc(110)*0.125*gain,os.osc(173)*0.1*gain;";
        const std::array<std::string,4> effects={"process = _ * 0.73, _ * 0.61;", "process = (_ * 0.17) + 0.02, (_ * 0.11) - 0.03;", "import(\"stdfaust.lib\"); process=fi.lowpass(2,4200),fi.lowpass(2,3100);", "import(\"stdfaust.lib\"); process=de.delay(8,1),de.delay(8,1);"};
        for(int i=1;i<=n;++i)modules[i].code=effects[(i-1)%4];
    }else{
        s.code="import(\"stdfaust.lib\"); gain=hslider(\"gain\",1,0,1,0.001); process=os.osc(91)*0.08*gain,os.osc(137)*0.07*gain;";
    }
    curlop::ParamSchemaEntry gain;gain.name="gain";gain.sourceId="gain";gain.min=0;gain.max=1;gain.defaultValue=1;
    s.params={gain};s.paramValues["gain"]=1;
    if(family=="nonlinear")modules[1].code="sat(x)=x/(1.0+abs(x)); process=_,_ : *(3),*(3) : sat,sat;";
    if(family=="memory"){
        s.code="import(\"stdfaust.lib\"); gain=hslider(\"gain\",1,0,1,0.001); process=os.osc(173)*0.1*gain,(1-1')*0.1*gain;";
        for(int i=1;i<=n;++i)modules[i].code=i==3?"import(\"stdfaust.lib\"); process=_,de.delay(32768,6000);":"process=_,_;";
        auto edges=f.graph.edges();for(auto& e:edges)e.gain=1.4142135623730951f;
        auto mods=modules;require(f.graph.replace(std::move(mods),std::move(edges),{}),"memory graph rejected");
    }
    return f;
}
struct Edge {int s,t,key;float l,r;bool delay;};
struct Plan {std::vector<int> order;std::vector<Edge> edges;std::vector<std::vector<Edge>> incoming;bool single=false;};
static Plan plan(const GraphState& g){
    Plan p;const int n=int(g.modules().size());p.incoming.resize(n);std::vector<PreparedSignalRouteDefinition> definitions;
    std::map<int,int> slot;for(int k=0;k<n;++k)slot[g.modules()[k].index]=k;
    for(auto& e:g.computeEffectiveEdges()){
        float gain=e.audible?std::clamp(e.gain,0.f,2.f):0.f;float left=gain,right=gain;
        bool delayed=e.feedbackBoundary==curlop::FeedbackBoundary::OneSample;
        if(!delayed&&e.signalDescriptor.layout()&&*e.signalDescriptor.layout()=="stereo"){
            auto a=(std::clamp(e.pan,-1.f,1.f)+1.f)*.25f*3.14159265358979323846f;left*=std::cos(a);right*=std::sin(a);
        }
        int s=slot.at(e.srcIndex),t=slot.at(e.tgtIndex);Edge route{s,t,s*n+t,left,right,delayed};p.edges.push_back(route);p.incoming[t].push_back(route);p.single|=delayed;
        PreparedSignalRouteDefinition d;d.sourceGraphIdx=s;d.destinationGraphIdx=t;d.sourceChannel=0;d.destinationChannel=0;d.descriptor=e.signalDescriptor;d.feedbackBoundary=delayed;definitions.push_back(d);
    }
    auto schedule=PreparedSignalSchedule::build(n,std::move(definitions),512);require(schedule&&!schedule->hasDeferredCycles(),"unsupported causal plan");
    p.order=schedule->preparedModuleOrder();return p;
}
static GraphState graphB(const Fixture& f){auto b=f.graph;require(b.addEdge(edge(f.marker,f.output-1)),"novel route rejected");return b;}
static V graphJson(const GraphState& g){
    auto p=plan(g);V out=obj();juce::Array<V> order,edges;
    for(int i:p.order)order.add(g.modules()[i].index);
    for(auto& e:p.edges){V v=obj();prop(v,"source",g.modules()[e.s].index);prop(v,"target",g.modules()[e.t].index);prop(v,"delay",e.delay);prop(v,"key",e.key);juce::Array<V> gs;gs.add(e.l);gs.add(e.r);prop(v,"gains",gs);edges.add(v);}
    prop(out,"order",order);prop(out,"edges",edges);return out;
}
static void exportGraph(const fs::path& out,const std::string& family,int n){
    auto f=make(family,n);V manifest=obj();prop(manifest,"family",family);prop(manifest,"size",n);prop(manifest,"output",f.output);juce::Array<V> modules;
    for(auto m:f.graph.modules()){
        if(m.lineageId=="core.output"){
            m.code="level=hslider(\"LEVEL\",1,0,1.5,0.001); clip(x)=min(1.0,max(-1.0,x)); process=_,_ : *(level),*(level) : clip,clip;";
            curlop::ParamSchemaEntry p;p.name="LEVEL";p.sourceId="LEVEL";p.min=0;p.max=1.5;p.defaultValue=1;m.params={p};
        }
        V row=obj();prop(row,"index",m.index);prop(row,"id",m.moduleId);prop(row,"source",m.code);prop(row,"inputs",int(m.audioInputs.size())*2);
        juce::Array<V> params;for(auto& p:m.params){V v=obj();prop(v,"label",p.sourceId.empty()?p.name:p.sourceId);prop(v,"value",p.defaultValue);params.add(v);}prop(row,"params",params);modules.add(row);
    }
    prop(manifest,"modules",modules);V graphs=obj();prop(graphs,"A",graphJson(f.graph));prop(graphs,"B",graphJson(graphB(f)));prop(manifest,"graphs",graphs);writeJson(out/"graph.json",manifest);
}
class Library {
    void* handle=nullptr;
public:
    ps::Compute compute=nullptr;ps::Create create=nullptr;ps::Free destroy=nullptr;ps::Zone zone=nullptr;ps::Control controls=nullptr;ps::Tick tick=nullptr;ps::Block block=nullptr;
    using FB=float*(*)(void*);FB feedback=nullptr;ps::Schema schema=nullptr;ps::Bytes bytes=nullptr;ps::InitTables tables=nullptr;
    template<class T>T load(const char* name,bool necessary=true){auto* ptr=dlsym(handle,name);require(ptr||!necessary,std::string("missing symbol ")+name);return reinterpret_cast<T>(ptr);}
    explicit Library(const fs::path& path,bool bank=true){handle=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);require(handle!=nullptr,"library load failed: "+path.string());compute=load<ps::Compute>("ps_compute");create=load<ps::Create>("ps_create");destroy=load<ps::Free>("ps_free");zone=load<ps::Zone>("ps_zone");if(bank){controls=load<ps::Control>("ps_control");tick=load<ps::Tick>("ps_tick");block=load<ps::Block>("ps_block");feedback=load<FB>("ps_feedback");schema=load<ps::Schema>("ps_schema");bytes=load<ps::Bytes>("ps_bytes");tables=load<ps::InitTables>("ps_tables");tables(48000);}}
    ~Library(){if(handle)dlclose(handle);}Library(const Library&)=delete;
};
struct State {Library& owner;void* ptr;State(Library& l):owner(l),ptr(l.create(48000)){require(ptr,"allocation failed");}~State(){owner.destroy(ptr);}State(const State&)=delete;};
struct Audio {
    std::vector<std::vector<float>> data;std::vector<float*> pointers;
    explicit Audio(int channels,int max=512):data(channels,std::vector<float>(max)),pointers(channels){for(int c=0;c<channels;++c)pointers[c]=data[c].data();}
    void clear(int n){for(auto& ch:data)std::fill_n(ch.begin(),n,0.f);}
    std::vector<float> capture(int n)const{std::vector<float> a(std::size_t(n)*data.size());for(int i=0;i<n;++i)for(std::size_t c=0;c<data.size();++c)a[std::size_t(i)*data.size()+c]=data[c][i];return a;}
};
static void append(std::vector<float>& a,const std::vector<float>& b){a.insert(a.end(),b.begin(),b.end());}
static void compare(const std::vector<float>& a,const std::vector<float>& b,double abs=1e-5){require(a.size()==b.size(),"length mismatch");for(std::size_t i=0;i<a.size();++i)require(std::isfinite(a[i])&&std::isfinite(b[i])&&std::abs(double(a[i])-b[i])<=abs+1e-5*std::abs(b[i]),"sample mismatch at "+std::to_string(i)+": "+std::to_string(a[i])+" vs "+std::to_string(b[i]));}
static volatile double meterSink=0;
static void observe(const Audio& a,int frames){double total=0;for(auto& ch:a.data){double sum=0;float peak=0;for(int s=0;s<frames;++s){float x=ch[s];sum+=double(x)*x;peak=std::max(peak,std::abs(x));}total+=peak+std::sqrt(sum/frames);}meterSink=total;}
static void dynamic(Library& l,void* state,const Plan& p,Audio& audio,int count){
    l.controls(state);auto* prev=l.feedback(state);int step=p.single?1:count;
    for(int offset=0;offset<count;offset+=step){
        for(int i:p.order){
            auto* il=audio.pointers[4*i+2]+offset;auto* ir=audio.pointers[4*i+3]+offset;std::fill_n(il,step,0.f);std::fill_n(ir,step,0.f);
            for(auto& e:p.incoming[i])for(int s=0;s<step;++s){il[s]+=(e.delay?prev[2*e.key]:audio.data[4*e.s][offset+s])*e.l;ir[s]+=(e.delay?prev[2*e.key+1]:audio.data[4*e.s+1][offset+s])*e.r;}
            float* inputs[]={il,ir};float* outputs[]={audio.pointers[4*i]+offset,audio.pointers[4*i+1]+offset};l.block(state,i,step,inputs,outputs);
        }
        for(auto& e:p.edges)if(e.delay){prev[e.key*2]=audio.data[4*e.s][offset];prev[e.key*2+1]=audio.data[4*e.s+1][offset];}
    }
}
class WholeLLVM {
    llvm_dsp_factory* f=nullptr;std::unique_ptr<::dsp> d;MapUI ui;
public:
    explicit WholeLLVM(const fs::path& path){std::ifstream in(path);std::ostringstream code;code<<in.rdbuf();std::string error;f=createDSPFactoryFromString("whole-reference",code.str(),0,nullptr,"",error,-1);require(f!=nullptr,"whole LLVM compile failed: "+error);d.reset(f->createDSPInstance());require(bool(d),"LLVM instance");d->init(48000);d->buildUserInterface(&ui);}
    void render(Audio& a,int n){d->compute(n,nullptr,a.pointers.data());}
    int channels(){return d->getNumOutputs();}
    void gain(float value){auto* z=ui.getParamZone("m0_gain");require(z,"LLVM gain zone missing");*z=value;}
    ~WholeLLVM(){d.reset();if(f)deleteDSPFactory(f);}
};
// Independent live oracle: ordinary separately compiled LLVM instances. No
// persistent-state sample-function ABI or generated native kernel is used here.
class ModuleLLVM {
    std::vector<llvm_dsp_factory*> factories;std::vector<std::unique_ptr<::dsp>> dsps;std::vector<std::unique_ptr<MapUI>> uis;std::vector<float> previous;
public:
    explicit ModuleLLVM(const V& manifest){auto* mods=manifest["modules"].getArray();require(mods,"manifest modules");previous.resize(mods->size()*mods->size()*2);
        for(auto& m:*mods){std::string error;auto* f=createDSPFactoryFromString("independent-module",text(m["compiled_source"]),0,nullptr,"",error,-1);require(f,"reference module: "+error);factories.push_back(f);std::unique_ptr<::dsp>d(f->createDSPInstance());d->init(48000);auto ui=std::make_unique<MapUI>();d->buildUserInterface(ui.get());uis.push_back(std::move(ui));dsps.push_back(std::move(d));}}
    void gain(float g){auto* z=uis[0]->getParamZone("m0_gain");require(z,"module gain zone");*z=g;}
    void render(const Plan& p,Audio& a,int count){int quantum=p.single?1:count;for(int offset=0;offset<count;offset+=quantum){for(int i:p.order){auto* il=a.pointers[4*i+2]+offset;auto* ir=a.pointers[4*i+3]+offset;std::fill_n(il,quantum,0.f);std::fill_n(ir,quantum,0.f);for(auto& e:p.incoming[i])for(int s=0;s<quantum;++s){il[s]+=(e.delay?previous[2*e.key]:a.data[4*e.s][offset+s])*e.l;ir[s]+=(e.delay?previous[2*e.key+1]:a.data[4*e.s+1][offset+s])*e.r;}float* ins[]={il,ir};float* outs[]={a.pointers[4*i]+offset,a.pointers[4*i+1]+offset};dsps[i]->compute(quantum,dsps[i]->getNumInputs()?ins:nullptr,outs);}for(auto& e:p.edges)if(e.delay){previous[2*e.key]=a.data[4*e.s][offset];previous[2*e.key+1]=a.data[4*e.s+1][offset];}}}
    ~ModuleLLVM(){dsps.clear();for(auto* f:factories)deleteDSPFactory(f);}
};
static void matchProduct(Renderer& reference,const GraphState& g,const Audio& a,int count){
    for(std::size_t slot=0;slot<g.modules().size();++slot){auto& m=g.modules()[slot];bool found=false;for(std::size_t t=0;t<reference.tapCount();++t)if(reference.tapModuleId(t)==m.moduleId){found=true;for(int s=0;s<count;++s)for(int c=0;c<2;++c){double y=reference.tapSample(t,c,s);require(std::isfinite(y)&&std::abs(a.data[slot*4+c][s]-y)<=1e-5+1e-5*std::abs(y),"actual product tap mismatch "+m.moduleId);}}require(found,"product tap missing");}
}
static void staticGate(const fs::path& root,const fs::path& out,const std::string& family,int n,int frames){
    auto fixture=make(family,n);auto manifest=read(root/"manifest.json");const int channels=int(fixture.graph.modules().size())*4;
    Library kernel(root/"kernel-A.dylib"),wholecpp(root/"whole-A.dylib",false);State bank(kernel),modular(kernel),cpp(wholecpp);WholeLLVM llvm(root/"whole-A.dsp");require(llvm.channels()==channels,"whole plane count");
    auto product=build(fixture.graph,frames);auto p=plan(fixture.graph);Audio a(channels),b(channels),c(channels),d(channels);juce::AudioBuffer<float>host(2,frames);juce::MidiBuffer midi;
    std::vector<float> ca,cb,cc,cd;for(int offset=0;offset<8192;offset+=frames){kernel.compute(bank.ptr,frames,a.pointers.data());llvm.render(b,frames);wholecpp.compute(cpp.ptr,frames,c.pointers.data());dynamic(kernel,modular.ptr,p,d,frames);process(*product.renderer,host,midi);compare(a.capture(frames),b.capture(frames));compare(a.capture(frames),c.capture(frames));compare(a.capture(frames),d.capture(frames));matchProduct(*product.renderer,fixture.graph,a,frames);append(ca,a.capture(frames));append(cb,b.capture(frames));append(cc,c.capture(frames));append(cd,d.capture(frames));}
    raw(out/"shared-optimized.f32",ca);raw(out/"whole-llvm.f32",cb);raw(out/"whole-cpp.f32",cc);raw(out/"shared-modular.f32",cd);
    std::ofstream perf(out/"throughput.tsv");perf<<"trial\tbackend\tmeasurement\tns_per_frame\n"<<std::setprecision(16);std::mt19937 rng(907);
    for(int trial=0;trial<11;++trial){std::array<int,4>order={0,1,2,3};std::shuffle(order.begin(),order.end(),rng);for(int arm:order)for(int metered=0;metered<2;++metered){auto start=now();for(int k=0;k<256;++k){if(arm==0)kernel.compute(bank.ptr,frames,a.pointers.data());if(arm==1)llvm.render(a,frames);if(arm==2)wholecpp.compute(cpp.ptr,frames,a.pointers.data());if(arm==3)dynamic(kernel,modular.ptr,p,a,frames);if(metered)observe(a,frames);}double elapsed=now()-start;perf<<trial<<'\t'<<std::array<const char*,4>{"shared-optimized","whole-llvm","whole-cpp","shared-modular"}[arm]<<'\t'<<(metered?"with-observers":"dsp")<<'\t'<<elapsed*1000/(256.0*frames)<<'\n';perf.flush();}}
    V result=obj();prop(result,"schema",kernel.schema());prop(result,"state_bytes",double(kernel.bytes()));prop(result,"channels",channels);prop(result,"captured_frames",8192);prop(result,"actual_product_taps_checked",true);prop(result,"phase","static-gate");writeJson(out/"result.json",result);
}
static void transition(const fs::path& root,const fs::path& out,const std::string& family,int n,int frames){
    auto f=make(family,n);auto bGraph=graphB(f);auto pa=plan(f.graph),pb=plan(bGraph);auto manifest=read(root/"manifest.json");int channels=int(f.graph.modules().size())*4;
    Library aLib(root/"kernel-A.dylib"),bLib(root/"kernel-B.dylib");require(std::string(aLib.schema())==bLib.schema()&&aLib.bytes()==bLib.bytes(),"state ABI mismatch");State live(aLib),reset(aLib);ModuleLLVM reference(manifest);
    auto* gain=aLib.zone(live.ptr,0,"m0_gain");require(gain,"gain pointer");Audio audio(channels),ref(channels),negative(channels);
    std::vector<float> actual,expected,resetAudio;std::ofstream rows(out/"blocks.tsv");rows<<"offset\tframes\tmode\ttopology\tgain\tcompute_us\n"<<std::setprecision(16);
    const int total=24576;int offset=0;std::array<int,9>irregular={1,7,17,64,65,127,128,257,511};int k=0;
    while(offset<total){
        int stage=offset<4096?0:offset<5120?1:offset<12288?2:offset<14336?3:4;
        int end=std::array<int,5>{4096,5120,12288,14336,total}[stage];int count=std::min({frames,irregular[k++%irregular.size()],end-offset});
        // Sample-offset control change, split exactly rather than quantized.
        int controlBoundary=offset<3001?3001:offset<10003?10003:total;if(controlBoundary>offset)count=std::min(count,controlBoundary-offset);
        float value=offset<3001?1.f:offset<10003?.73f:.91f;*gain=value;reference.gain(value);
        bool topologyB=stage==1||stage==2;std::string mode=stage==1||stage==3?"editable":"optimized";
        auto start=now();if(mode=="editable")dynamic(aLib,live.ptr,topologyB?pb:pa,audio,count);else(topologyB?bLib:aLib).compute(live.ptr,count,audio.pointers.data());auto elapsed=now()-start;
        reference.render(topologyB?pb:pa,ref,count);compare(audio.capture(count),ref.capture(count));
        append(actual,audio.capture(count));append(expected,ref.capture(count));rows<<offset<<'\t'<<count<<'\t'<<mode<<'\t'<<(topologyB?"B":"A")<<'\t'<<value<<'\t'<<elapsed<<'\n';
        if(offset>=4096&&offset<12288){aLib.compute(reset.ptr,count,negative.pointers.data());append(resetAudio,negative.capture(count));}
        offset+=count;
    }
    raw(out/"transition.f32",actual);raw(out/"independent-llvm-reference.f32",expected);raw(out/"reset-negative.f32",resetAudio);
    V result=obj();prop(result,"schema",aLib.schema());prop(result,"state_address_unchanged",true);prop(result,"new_states_on_transition",0);prop(result,"state_bytes_copied",0);prop(result,"frames",total);prop(result,"channels",channels);prop(result,"phase","transition");writeJson(out/"result.json",result);
}
} // namespace
int main(int argc,char**argv){try{require(argc==7,"usage: PersistentStateBench mode family size frames kernel-directory output-directory");std::string mode=argv[1],family=argv[2];int n=std::stoi(argv[3]),frames=std::stoi(argv[4]);require(n>=4&&n<=64&&(frames==64||frames==128||frames==512),"bounded size");fs::path root=fs::absolute(argv[5]),out=fs::absolute(argv[6]);fs::create_directories(out);if(mode=="export")ps_test::exportGraph(out,family,n);else if(mode=="gate")ps_test::staticGate(root,out,family,n,frames);else if(mode=="transition")ps_test::transition(root,out,family,n,frames);else throw std::runtime_error("unknown mode");std::cout<<"PASS "<<mode<<' '<<family<<' '<<n<<' '<<frames<<'\n';return 0;}catch(const std::exception&e){std::cerr<<"PERSISTENT_STATE_FAILURE: "<<e.what()<<'\n';return 2;}}
