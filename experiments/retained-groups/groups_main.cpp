// Extension of the exact executed #33 fixtures and the real fused renderer.
#define main original_patching_entrypoint
#include "main.cpp"
#undef main
#include "StableGroups.h"
#include <random>
#include <numeric>

namespace experiment {
using namespace groups;
constexpr double absoluteBound=1e-5,relativeBound=1e-5;
struct Config {std::string family;int size,width;};
const std::vector<Config> configs={{"serial",32,4},{"serial",32,8},{"parallel",32,4},{"parallel",32,8},{"feedback",8,8},{"control",16,4},{"nonlinear",16,4}};
ModuleEntry& mutableEntry(GraphState& g,int id){for(auto& m:g.modulesMutable())if(m.index==id)return m;throw std::runtime_error("module missing");}
Fixture makeFixture(const Config& c,bool memory=false){
    auto f=fixture(c.family=="nonlinear"?"serial":c.family,c.size);
    if(c.family=="nonlinear")mutableEntry(f.graph,c.size-c.width+2).code="sat(x)=x/(1+abs(x)); process=*(3),*(3) : sat,sat;";
    if(memory){
        mutableEntry(f.graph,0).code="import(\"stdfaust.lib\"); process=os.osc(173)*0.1, (1-1')*0.05;";
        for(int i=1;i<=c.size;++i)mutableEntry(f.graph,i).code=i==1?"process=_, _ @ 6000;":"process=_,_;";
    }
    return f;
}
Layout grouping(const Config& c){return layout(c.size,c.width,c.family=="parallel");}
std::vector<int> inputTargets(const Layout& l){auto& b=l.units.back();return b.parallel?b.members:std::vector<int>{b.first};}
void wire(Fixture& f,const Layout& l,bool connected){
    for(int target:inputTargets(l)){
        if(connected)require(f.graph.addEdge(edge(f.marker,target)),"new exposed-port wire rejected");
        else {
            auto it=std::find_if(f.graph.edges().begin(),f.graph.edges().end(),[&](auto& e){return e.srcIndex==f.marker&&e.tgtIndex==target;});
            require(it!=f.graph.edges().end(),"wire not installed");auto e=*it;
            f.graph.removeEdge(e.srcIndex,e.tgtIndex,e.srcPort,e.tgtPort);
        }
    }
}
void internalEdit(Fixture& f,const Config& c,bool code){
    if(code){mutableEntry(f.graph,c.width+2).code="process=*(0.75),*(0.75);";return;}
    auto it=std::find_if(f.graph.edges().begin(),f.graph.edges().end(),[&](auto& e){return e.srcIndex==c.width+1&&e.tgtIndex==c.width+2;});
    require(it!=f.graph.edges().end(),"internal wire missing");auto e=*it;
    f.graph.removeEdge(e.srcIndex,e.tgtIndex,e.srcPort,e.tgtPort);e.gain=.5f;
    require(f.graph.addEdge(e),"internal wire edit rejected");
}
void compare(const std::vector<float>& x,const std::vector<float>& y,const std::string& label){
    require(x.size()==y.size(),label+": length mismatch");
    for(std::size_t i=0;i<x.size();++i)if(!std::isfinite(x[i])||!std::isfinite(y[i])||std::abs(double(x[i])-y[i])>absoluteBound+relativeBound*std::abs(y[i]))
        throw std::runtime_error(label+": sample "+std::to_string(i)+" mismatch "+std::to_string(x[i])+" versus "+std::to_string(y[i]));
}
void append(std::vector<float>& dst,const std::vector<float>& src){dst.insert(dst.end(),src.begin(),src.end());}
std::vector<float> tap(const RPlan& p,int unit,int frames){
    std::vector<float>x(std::size_t(frames)*2);for(int s=0;s<frames;++s)for(int c=0;c<2;++c)x[2*s+c]=p.nodes[unit]->out[c][s];return x;
}
struct Engine {
    std::string name;
    bool flat=false;
    std::shared_ptr<NativeFactories> factories;
    std::unique_ptr<RGraph> graph;
    std::unique_ptr<RPlan> plan;
    double coldUs=0;
    Engine(std::string backend,const GraphState& g,const Layout& l,int frames,const fs::path& aot,const fs::path& out):name(std::move(backend)){
        flat=name=="modules";retained_units::ExternalFactory factory;
        if(name=="ocpp"||name=="ls-fuse"){
            factories=std::make_shared<NativeFactories>(aot,name);
            factory=[owner=factories](const ModuleEntry& m){return owner->create(m);};
        }
        graph=std::make_unique<RGraph>(48000,frames,(out/("unused-cache-"+name)).string(),std::move(factory));
        auto t=now();auto shaped=flat?g:collapse(g,l);plan=graph->prepare(shaped,nullptr,1);coldUs=now()-t;
    }
    double prepare(const GraphState& g,const Layout& l,std::uint64_t revision,std::size_t expectedCreated){
        auto t=now();auto shaped=flat?g:collapse(g,l);auto next=graph->prepare(shaped,plan.get(),revision);
        require(next->created==expectedCreated&&next->acquired==expectedCreated,name+": unexpected invalidation count");
        if(expectedCreated==0)for(std::size_t i=0;i<plan->nodes.size();++i)require(next->nodes[i]==plan->nodes[i],name+": instance reset on cable edit");
        plan.swap(next);return now()-t;
    }
    void render(juce::AudioBuffer<float>& b){require(RGraph::render(*plan,b),name+": rejected render");}
};
std::vector<std::unique_ptr<Engine>> engines(const GraphState& g,const Layout& l,int frames,const fs::path& aot,const fs::path& out,bool modules=true){
    std::vector<std::string>names=modules?std::vector<std::string>{"modules","group-jit"}:std::vector<std::string>{"group-jit"};
    if(aot!="none"){names.push_back("ocpp");names.push_back("ls-fuse");}
    std::vector<std::unique_ptr<Engine>> es;for(auto& n:names)es.push_back(std::make_unique<Engine>(n,g,l,frames,aot,out));return es;
}
void saveUnits(const GraphState& g,const Layout& l,const fs::path& dir){
    auto shaped=collapse(g,l);fs::create_directories(dir);
    for(auto& m:shaped.modules())if(m.lineageId=="core.faust_jit"){
        auto path=dir/(fingerprint(m.code)+".dsp");
        if(fs::exists(path)){std::ifstream in(path);std::ostringstream s;s<<in.rdbuf();require(s.str()==m.code,"source-key collision");}
        else{std::ofstream out(path);out<<m.code;require(bool(out),"source write failed");}
    }
}
void exportSources(const fs::path& root){
    for(auto& c:configs){auto f=makeFixture(c);saveUnits(f.graph,grouping(c),root/"sources");}
    Config memory{"serial",8,4};auto f=makeFixture(memory,true);auto l=grouping(memory);
    saveUnits(f.graph,l,root/"sources");
    for(bool code:{false,true}){auto edit=f;internalEdit(edit,memory,code);saveUnits(edit.graph,l,root/"sources");}
}
void throughput(const fs::path& out,std::vector<std::unique_ptr<Engine>>& es,int frames){
    juce::AudioBuffer<float>b(2,frames);std::ofstream f(out/"throughput.tsv");
    f<<"trial\tbackend\tns_per_frame\n"<<std::setprecision(15);
    std::vector<int> order(es.size());std::iota(order.begin(),order.end(),0);std::mt19937 random(20260909);
    for(auto& e:es)for(int k=0;k<32;++k)e->render(b);
    for(int trial=0;trial<9;++trial){std::shuffle(order.begin(),order.end(),random);
        for(int idx:order){auto start=now();for(int k=0;k<512;++k)es[idx]->render(b);auto end=now();
            interleave(b);f<<trial<<'\t'<<es[idx]->name<<'\t'<<(end-start)*1000/(512.0*frames)<<'\n';}
    }
}
void matrix(const Config& c,int frames,const fs::path& out,const fs::path& aot){
    auto f=makeFixture(c);auto l=grouping(c);auto oracle=f;wire(oracle,l,true);
    mutableEntry(oracle.graph,f.marker).code="time=+(1)~_; phase=int((time-4097)/"+std::to_string(4*frames)+"); active=(time>4096)&(time<="+std::to_string(4096+32*4*frames)+")&((phase%2)==0); process=0.02*float(active),0.01*float(active);";
    auto fused=build(oracle.graph,frames);auto es=engines(f.graph,l,frames,aot,out);
    Engine dry("modules",f.graph,l,frames,"none",out);
    std::ofstream initial(out/"initial.tsv");initial<<"backend\tprepare_us\tinstances\texternal_feedback_sample_mode\n"<<std::setprecision(15);
    for(auto& e:es)initial<<e->name<<'\t'<<e->coldUs<<'\t'<<e->plan->created<<'\t'<<e->plan->singleSample<<'\n';
    std::ofstream edits(out/"edits.tsv");edits<<"trial\tbackend\tconnected\tgraph_mutation_us\tgroup_and_routing_prepare_us\tprepare_and_first_compute_us\tcreated\treused\tacquired\n"<<std::setprecision(15);
    std::ofstream blocks(out/"blocks.tsv");blocks<<"block\tconnected\n";
    std::vector<std::vector<float>> captures(es.size());std::vector<float> refCapture,dryCapture;
    juce::AudioBuffer<float>b(2,frames),reference(2,frames);juce::MidiBuffer midi;std::size_t block=0;
    auto renderBlock=[&](bool connected,int trial,double mutation){
        std::vector<std::vector<float>> current(es.size());
        std::vector<int> order(es.size());std::iota(order.begin(),order.end(),0);if(trial>=0&&trial%2)std::reverse(order.begin(),order.end());
        for(int idx:order){auto& e=es[idx];auto t=now();double preparation=0;
            if(trial>=0)preparation=e->prepare(f.graph,l,trial+2,0);
            e->render(b);auto done=now();current[idx]=interleave(b);
            if(trial>=0)edits<<trial<<'\t'<<e->name<<'\t'<<connected<<'\t'<<mutation<<'\t'<<preparation<<'\t'<<done-t<<'\t'<<e->plan->created<<'\t'<<e->plan->reused<<'\t'<<e->plan->acquired<<'\n';
        }
        process(*fused.renderer,reference,midi);auto y=interleave(reference);append(refCapture,y);
        dry.render(b);append(dryCapture,interleave(b));
        for(std::size_t i=0;i<es.size();++i){append(captures[i],current[i]);compare(current[i],y,es[i]->name+" block "+std::to_string(block));}
        blocks<<block++<<'\t'<<connected<<'\n';
    };
    for(int i=0;i<4096/frames;++i)renderBlock(false,-1,0);
    for(int trial=0;trial<32;++trial){bool connected=trial%2==0;auto start=now();wire(f,l,connected);double mutation=now()-start;
        renderBlock(connected,trial,mutation);for(int k=0;k<3;++k)renderBlock(connected,-1,0);}
    for(std::size_t i=0;i<es.size();++i)raw(out/(es[i]->name+".f32"),captures[i]);raw(out/"fused-oracle.f32",refCapture);
    raw(out/"unpatched-negative.f32",dryCapture);
    require(delta(refCapture,dryCapture)>1e-4,"connection effect too small to qualify the audio comparison");
    throughput(out,es,frames);
    // Boundaries are explicit: hidden internals must not silently lose cables.
    if(c.width>1&&c.family!="parallel"){
        auto bad=f;require(bad.graph.addEdge(edge(f.marker,2)),"negative edge fixture");bool rejected=false;
        try{collapse(bad.graph,l);}catch(const std::exception&){rejected=true;}
        require(rejected,"hidden internal input accepted as exposed group port");
        std::ofstream(out/"boundary-negative.txt")<<"hidden-input: rejected\n";
    }
}
void state(int frames,const fs::path& out,const fs::path& aot){
    Config c{"serial",8,4};auto l=grouping(c);
    std::ofstream observations(out/"state.tsv");observations<<"event\tbackend\tprepare_us\tcreated\treused\tacquired\tunchanged_group_same_object\n"<<std::setprecision(15);
    for(std::string event:{"external","internal-wire","source-code"}){
        auto f=makeFixture(c,true);auto ref=build(f.graph,frames);auto es=engines(f.graph,l,frames,aot,out,false);
        auto reset=engines(f.graph,l,frames,aot,out,false);
        juce::AudioBuffer<float>b(2,frames),reference(2,frames);juce::MidiBuffer midi;
        std::size_t refTap=0;bool found=false;
        for(std::size_t t=0;t<ref.renderer->tapCount();++t)if(ref.renderer->tapModuleId(t)=="latency-fx4"){refTap=t;found=true;}
        require(found,"independent group-exit tap missing");
        for(int i=0;i<4096/frames;++i){for(auto& e:es)e->render(b);process(*ref.renderer,reference,midi);}
        if(event=="external")marker(f,true);else internalEdit(f,c,event=="source-code");
        for(auto& e:es){auto old=e->plan->nodes;auto t=e->prepare(f.graph,l,2,event=="external"?0:1);
            for(std::size_t i=0;i<old.size();++i)require((e->plan->nodes[i]==old[i])==(event=="external"||i!=5),"incorrect group replacement");
            observations<<event<<'\t'<<e->name<<'\t'<<t<<'\t'<<e->plan->created<<'\t'<<e->plan->reused<<'\t'<<e->plan->acquired<<"\t1\n";
        }
        std::vector<std::vector<float>>current(es.size()),negative(es.size());std::vector<float>oracle;
        for(int i=0;i<8192/frames;++i){
            process(*ref.renderer,reference,midi);std::vector<float>y;
            for(int s=0;s<frames;++s)for(int channel=0;channel<2;++channel)y.push_back(ref.renderer->tapSample(refTap,channel,s));append(oracle,y);
            for(std::size_t j=0;j<es.size();++j){es[j]->render(b);auto x=tap(*es[j]->plan,1,frames);append(current[j],x);compare(x,y,event+" "+es[j]->name+" retained group state");
                reset[j]->render(b);append(negative[j],tap(*reset[j]->plan,1,frames));}
        }
        raw(out/(event+"-oracle.f32"),oracle);
        for(std::size_t j=0;j<es.size();++j){raw(out/(event+"-"+es[j]->name+".f32"),current[j]);raw(out/(event+"-reset-"+es[j]->name+".f32"),negative[j]);
            require(delta(current[j],negative[j])>1e-3,"reset negative control not distinguished");}
    }
}
void partition(const Config& c,int frames,const fs::path& out,const fs::path& aot){
    auto f=makeFixture(c);auto l=grouping(c);auto fixed=engines(f.graph,l,frames,aot,out,false),irregular=engines(f.graph,l,frames,aot,out,false);
    std::vector<std::vector<float>>x(fixed.size()),y(fixed.size());
    juce::AudioBuffer<float>b(2,frames);
    for(int offset=0;offset<8192;offset+=frames)for(std::size_t j=0;j<fixed.size();++j){fixed[j]->render(b);append(x[j],interleave(b));}
    int offset=0,k=0;const int sizes[]={1,17,3,64,127,5,31};
    while(offset<8192){int n=std::min({sizes[k++%7],frames,8192-offset});juce::AudioBuffer<float>small(2,n);
        for(std::size_t j=0;j<irregular.size();++j){irregular[j]->render(small);append(y[j],interleave(small));}offset+=n;}
    for(std::size_t j=0;j<fixed.size();++j){raw(out/(fixed[j]->name+"-fixed.f32"),x[j]);raw(out/(fixed[j]->name+"-irregular.f32"),y[j]);
        require(x[j]==y[j],fixed[j]->name+": block partition changed samples");}
}
}
int main(int argc,char**argv){
    try{
        if(argc==3&&std::string(argv[1])=="export"){experiment::exportSources(fs::absolute(argv[2]));return 0;}
        require(argc==8,"usage: StableGroupBench mode family size width frames output native-directory-or-none");
        std::string mode=argv[1];experiment::Config c{argv[2],std::stoi(argv[3]),std::stoi(argv[4])};int frames=std::stoi(argv[5]);
        require((frames==64||frames==128)&&c.size>0&&c.size<=64&&c.width>0&&c.width<=c.size,"bounded arguments");
        fs::path out=fs::absolute(argv[6]);fs::create_directories(out);fs::path aot=std::string(argv[7])=="none"?fs::path("none"):fs::absolute(argv[7]);
        if(mode=="matrix")experiment::matrix(c,frames,out,aot);
        else if(mode=="state")experiment::state(frames,out,aot);
        else if(mode=="partition")experiment::partition(c,frames,out,aot);
        else throw std::runtime_error("unknown test mode");
        curlop::FaustRuntime::instance().releaseAllFactories();std::cout<<"PASS "<<mode<<" "<<c.family<<" "<<c.width<<" "<<frames<<"\n";return 0;
    }catch(const std::exception&e){std::cerr<<"STABLE_GROUP_FAILURE: "<<e.what()<<"\n";return 2;}
}
