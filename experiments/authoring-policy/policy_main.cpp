// #38 implementation and test helpers, with only its initial grouping policy selected.
#include "policy_support.generated.h"
namespace policy_test {
using namespace next_slice;
constexpr float wireScale=1.4142135623730951f;
void wire(GraphState& g,int a,int b,float factor=1.f){auto e=edge(a,b,wireScale*factor);require(g.addEdge(e),"policy trace wire rejected");}
std::string policyName(int p){switch(p){case 1:return "individual";case 0:return "feedback-only";case 2:return "cap2";case 4:return "cap4";case 8:return "cap8";}throw std::runtime_error("bad policy");}
// Event targets depend only on authored fixture identities. They never inspect
// a policy's layout. Two predetermined targets expose different member ports.
void trace(const std::string& family,int count,int mode,int frames,int variant,const fs::path& out){
    experiment::Config cfg{family,count,4};auto f=experiment::makeFixture(cfg);auto& g=f.graph;
    auto id=[&](int i){return family=="serialshuffled"&&i>=1&&i<=count?count+1-i:i;};
    if(family=="serialshuffled"){
        auto ms=g.modules();auto es=g.edges();for(auto& m:ms)m.index=id(m.index);for(auto& e:es){e.srcIndex=id(e.srcIndex);e.tgtIndex=id(e.tgtIndex);}
        require(g.replace(std::move(ms),std::move(es),{}),"permuted graph rejected");
    }
    auto calibrated=g.edges();for(auto& e:calibrated)if(e.feedbackBoundary==curlop::FeedbackBoundary::None)e.gain*=wireScale;
    require(g.replace(g.modules(),std::move(calibrated),{}),"calibrated fixture rejected");
    int monitor=f.marker+4,inserted=f.marker+7;addNode(g,module(monitor,"monitor","process=_,_;",true));wire(g,monitor,f.output);
    const bool parallel=family=="parallel";int first=id(count-7),member=id(count-(variant==0?4:5)),last=id(count),prev=id(count-1);
    int before=id(count-(variant==0?4:5)-1),after=id(count-(variant==0?4:5)+1);
    auto originalMember=entry(g,member);auto originalSource=entry(g,last).code;
    adaptive::Engine engine(mode,frames,(out/"candidate-cache").string());auto initial=engine.apply(g);
    // Compile references AFTER recording candidate's initial preparation.
    FlatReference reference(frames,out);reference.apply(g);coldOracle(g,{},mode,frames,out,"initial");
    graphRecord(g,engine,out/"graph-initial.json");
    std::ofstream init(out/"initial.tsv");init<<"prepare_us\tcreated\tacquired\tgroups\n"<<std::setprecision(16)<<initial.totalUs<<'\t'<<initial.created<<'\t'<<initial.acquired<<'\t'<<engine.layout.units.size()<<'\n';
    std::ofstream edits(out/"edits.tsv");edits<<"event\tname\tmutation_us\tplan_us\tprepare_us\tedit_compute_us\tcreated\treused\tacquired\tsource_hits\tsource_misses\treset_members\tgroups\tsample_mode\n"<<std::setprecision(16);
    std::ofstream blocks(out/"blocks.tsv");blocks<<"block\tevent\tframe\n";
    std::ofstream perf(out/"throughput.tsv");perf<<"phase\ttrial\tbackend\tns_per_frame\n"<<std::setprecision(16);
    juce::AudioBuffer<float> xb(2,frames),rb(2,frames);std::vector<float>x,y;std::uint64_t at=0;int block=0;double eventPeak=0;
    auto capture=[&](int event,bool already=false){if(!already)engine.render(xb);reference.render(rb);auto a=interleave(xb),b=interleave(rb);compare(a,b,"policy live "+std::to_string(event));
        for(float v:b)eventPeak=std::max(eventPeak,std::abs(double(v)));append(x,a);append(y,b);blocks<<block++<<'\t'<<event<<'\t'<<at<<'\n';at+=frames;};
    std::mt19937 rng(2901+variant*101+frames);
    auto measure=[&](const std::string& phase){
        for(int trial=0;trial<9;++trial){bool swapped=rng()%2;
            for(int j=0;j<2;++j){bool candidate=j==int(swapped);auto t=now();
                for(int k=0;k<256;++k){if(candidate)engine.render(xb);else reference.render(rb);}
                auto end=now();perf<<phase<<'\t'<<trial<<'\t'<<(candidate?"policy":"individual-reference")<<'\t'<<(end-t)*1000/(256.0*frames)<<'\n';}
            compare(interleave(xb),interleave(rb),"matched timed blocks");at+=256*frames;
        }perf.flush();
    };
    for(int offset=0;offset<4096;offset+=frames)capture(-1);measure("initial");
    Pins pins;std::vector<EdgeEntry> deletedEdges;GraphState undoEndpoint,undoInsertion;
    const std::vector<std::string> names={"external-connect","external-disconnect","expose-member-input","connect-member-input","disconnect-member-input",
        "expose-member-output","connect-member-output","disconnect-member-output","endpoint-rewire","undo-endpoint-rewire","insert-node","remove-inserted-node",
        "member-source-change","undo-member-source","delete-member","restore-member","explicit-compact","repeat-after-compact"};
    for(int k=0;k<int(names.size());++k){eventPeak=0;auto begin=now();bool compact=false;
        switch(k){
        case 0:wire(g,f.marker,f.output);break;
        case 1:removeWire(g,f.marker,f.output);break;
        case 2:pins.inputs.insert(member);break;
        case 3:wire(g,f.marker,member);break;
        case 4:removeWire(g,f.marker,member);break;
        case 5:pins.outputs.insert(member);break;
        case 6:wire(g,member,monitor);break;
        case 7:removeWire(g,member,monitor);break;
        case 8:undoEndpoint=g;removeWire(g,parallel?0:prev,last);wire(g,parallel?f.marker:first,last);break;
        // Undo restores delay identity too; addEdge may auto-insert z^-1 in a cycle.
        case 9:g=undoEndpoint;break;
        case 10:undoInsertion=g;addNode(g,module(inserted,"inserted","process=*(0.6),*(0.6);",true));removeWire(g,parallel?0:prev,last);wire(g,parallel?0:prev,inserted);wire(g,inserted,last);break;
        case 11:g=undoInsertion;break;
        case 12:{auto& m=mutableEntry(g,last);auto pos=m.code.find("process");require(pos!=std::string::npos,"source anchor");m.code.replace(pos,7,"priorprocess");m.code+="\nprocess=priorprocess : *(0.75),*(0.75);\n";break;}
        case 13:mutableEntry(g,last).code=originalSource;break;
        case 14:deletedEdges=g.edges();pins.inputs.erase(member);pins.outputs.erase(member);removeNode(g,member);if(!parallel)wire(g,before,after);break;
        case 15:{auto ms=g.modules();ms.push_back(originalMember);require(g.replace(std::move(ms),deletedEdges,{}),"restore original member edges");break;}
        case 16:pins={};compact=true;break;
        case 17:break;
        }
        auto mutated=now();auto impact=engine.apply(g,pins,compact);engine.render(xb);auto done=now();
        if(mode==1&&k<=9)require(impact.created==0&&impact.acquired==0,"individual policy rebuilt for wiring/exposure");
        if(k==0||k==1||k==3||k==4||k==6||k==7||k==17)require(impact.created==0&&impact.acquired==0,"exposed route rebuilt");
        reference.apply(g,&impact);capture(k,true);for(int n=0;n<3;++n)capture(k);require(eventPeak>1e-4,"silent edit reference");
        eventRow(edits,k,names[k],impact,mutated-begin,done-begin,engine);graphRecord(g,engine,out/("graph-"+std::to_string(k)+".json"));
        if(k==7)measure("after-exposure");if(k==15)measure("before-compact");if(k==16)measure("after-compact");
    }
    raw(out/"live-policy.f32",x);raw(out/"live-selective-reference.f32",y);coldOracle(g,pins,mode,frames,out,"final");
    std::ofstream meta(out/"trace.json");meta<<"{\"policy\":"<<quote(policyName(mode))<<",\"family\":"<<quote(family)<<",\"size\":"<<count<<",\"frames\":"<<frames<<",\"variant\":"<<variant<<",\"target\":"<<member<<",\"processed_frames\":"<<at<<"}\n";
}
void state(int mode,int frames,int variant,const fs::path& out){
    auto f=stateFixture();adaptive::Engine engine(mode,frames,(out/"cache").string());engine.apply(f.graph);graphRecord(f.graph,engine,out/"graph-initial.json");
    auto mono=build(f.graph,frames);std::size_t tap1=0,tap2=0;bool found1=false,found2=false;
    for(std::size_t t=0;t<mono.renderer->tapCount();++t){auto id=mono.renderer->tapModuleId(t);if(id=="latency-fx4"){tap1=t;found1=true;}if(id=="latency-fx8"){tap2=t;found2=true;}}
    require(found1&&found2,"state taps unavailable");juce::AudioBuffer<float>b(2,frames),ref(2,frames);juce::MidiBuffer midi;
    for(int i=0;i<4096/frames;++i){engine.render(b);process(*mono.renderer,ref,midi);}
    int untouchedOwner=adaptive::owner(engine.layout,4);auto old=engine.plan->nodes[untouchedOwner];Pins pins;pins.inputs.insert(variant==0?6:7);
    auto impact=engine.apply(f.graph,pins);require(engine.plan->nodes[untouchedOwner]==old,"unrelated state object replaced");
    graphRecord(f.graph,engine,out/"graph-0.json");int affectedOwner=adaptive::owner(engine.layout,8);
    adaptive::Engine fresh(mode,frames,(out/"negative-cache").string());fresh.apply(f.graph,pins);
    std::vector<float>x,y,affected,continuous,negative;
    for(int i=0;i<8192/frames;++i){engine.render(b);process(*mono.renderer,ref,midi);auto a=experiment::tap(*engine.plan,untouchedOwner,frames);append(x,a);append(affected,experiment::tap(*engine.plan,affectedOwner,frames));
        std::vector<float>u,v;for(int s=0;s<frames;++s)for(int c=0;c<2;++c){u.push_back(mono.renderer->tapSample(tap1,c,s));v.push_back(mono.renderer->tapSample(tap2,c,s));}append(y,u);append(continuous,v);compare(a,u,"untouched memory");
        fresh.render(b);append(negative,experiment::tap(*fresh.plan,adaptive::owner(fresh.layout,4),frames));}
    auto impulse=[](const std::vector<float>& a){int found=-1;for(std::size_t i=1;i<a.size();i+=2)if(std::abs(a[i])>.01){require(found<0,"multiple impulse peaks");found=int(i/2);}require(found>=0,"missing impulse");return found;};
    require(impulse(x)==1904&&impulse(negative)==6000,"memory negative control failed");
    const bool resetsMemory=impact.resetMembers.count(5);require(impulse(affected)==(resetsMemory?6000:1904),"actual memory disagrees with reset report");
    if(!resetsMemory)compare(affected,continuous,"affected memory retained");else require(delta(affected,continuous)>1e-3,"reset hidden by comparison");
    raw(out/"untouched.f32",x);raw(out/"untouched-continuous.f32",y);raw(out/"affected.f32",affected);raw(out/"affected-continuous.f32",continuous);raw(out/"reset-negative.f32",negative);
    std::ofstream o(out/"state.json");o<<"{\"reset_members\":"<<integers(impact.resetMembers)<<",\"created\":"<<impact.created<<",\"acquired\":"<<impact.acquired<<",\"affected_impulse\":"<<impulse(affected)<<",\"untouched_impulse\":"<<impulse(x)<<",\"negative_impulse\":"<<impulse(negative)<<"}\n";
}
}
int main(int argc,char**argv){try{
    require(argc==8,"usage: AuthoringPolicyBench mode family count policy frames variant output");std::string mode=argv[1],family=argv[2];int n=std::stoi(argv[3]),policy=std::stoi(argv[4]),frames=std::stoi(argv[5]),variant=std::stoi(argv[6]);
    require((frames==64||frames==128)&&n>=8&&n<=32&&(variant==0||variant==1),"bounded arguments");policy_test::policyName(policy);fs::path out=fs::absolute(argv[7]);fs::create_directories(out);
    if(mode=="trace")policy_test::trace(family,n,policy,frames,variant,out);else if(mode=="state")policy_test::state(policy,frames,variant,out);else throw std::runtime_error("unknown mode");
    curlop::FaustRuntime::instance().releaseAllFactories();std::cout<<"PASS "<<mode<<' '<<family<<' '<<policy<<' '<<frames<<' '<<variant<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"POLICY_CHECKPOINT_FAILURE: "<<e.what()<<'\n';return 2;}}
