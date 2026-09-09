// This includes #34's actual workload constructors, retainer and product oracle.
#include "groups_as_library.generated.h"
#include "AdaptiveGroups.h"

namespace next_slice {
using namespace groups;
using experiment::append;using experiment::compare;using experiment::mutableEntry;
using adaptive::Pins;using adaptive::Impact;
std::string quote(const std::string& s){return juce::JSON::toString(juce::var(juce::String(s)),true).toStdString();}
std::string integers(const std::set<int>& ids){std::ostringstream s;s<<'[';bool first=true;for(int id:ids){if(!first)s<<',';first=false;s<<id;}return s.str()+"]";}
void graphRecord(const GraphState& g,const adaptive::Engine& engine,const fs::path& out){
    std::ofstream f(out);f<<"{\"modules\":[";bool first=true;
    for(auto& m:g.modules()){if(!first)f<<',';first=false;f<<"{\"id\":"<<m.index<<",\"identity\":"<<quote(m.moduleId)<<",\"code\":"<<quote(m.code)<<'}';}
    f<<"],\"edges\":[";first=true;for(auto& e:g.computeEffectiveEdges()){
        if(!first)f<<',';first=false;f<<"{\"source\":"<<e.srcIndex<<",\"target\":"<<e.tgtIndex<<",\"gain\":"<<number(e.gain)<<",\"pan\":"<<number(e.pan)
            <<",\"feedback\":"<<(e.feedbackBoundary==curlop::FeedbackBoundary::OneSample)<<'}';}
    f<<"],\"groups\":[";first=true;for(auto& b:engine.layout.units){if(!first)f<<',';first=false;
        f<<"{\"owner\":"<<b.first<<",\"parallel\":"<<b.parallel<<",\"members\":[";
        for(std::size_t i=0;i<b.members.size();++i){if(i)f<<',';f<<b.members[i];}f<<"]}";}
    f<<"],\"compiled_units\":[";first=true;for(auto& m:engine.shaped().modules()){if(!first)f<<',';first=false;
        f<<"{\"index\":"<<m.index<<",\"identity\":"<<quote(m.moduleId)<<",\"code\":"<<quote(m.code)<<'}';}
    f<<"]}\n";require(bool(f),"graph evidence write failed");
}
void removeWire(GraphState& g,int src,int dst){
    auto it=std::find_if(g.edges().begin(),g.edges().end(),[&](auto& e){return e.srcIndex==src&&e.tgtIndex==dst;});
    require(it!=g.edges().end(),"missing edited wire");auto e=*it;g.removeEdge(e.srcIndex,e.tgtIndex,e.srcPort,e.tgtPort);
}
void addWire(GraphState& g,int a,int b,bool feedback=false){auto e=edge(a,b,feedback?.025f:1.f);if(feedback)e.feedbackBoundary=curlop::FeedbackBoundary::OneSample;require(g.addEdge(e),"edited wire rejected");}
void removeNode(GraphState& g,int id){std::vector<ModuleEntry> ms;std::vector<EdgeEntry> es;
    for(auto& m:g.modules())if(m.index!=id)ms.push_back(m);for(auto& e:g.edges())if(e.srcIndex!=id&&e.tgtIndex!=id)es.push_back(e);
    require(g.replace(std::move(ms),std::move(es),{}),"node removal failed");}
void addNode(GraphState& g,ModuleEntry m){auto ms=g.modules();auto es=g.edges();ms.push_back(std::move(m));require(g.replace(std::move(ms),std::move(es),{}),"node insertion failed");}
struct FlatReference {
    RGraph graph;std::unique_ptr<RPlan> plan;std::map<int,int> epochs;
    explicit FlatReference(int frames,const fs::path& out):graph(48000,frames,(out/"reference-cache").string()){}
    void apply(GraphState g,const Impact* impact=nullptr){
        if(impact)for(int id:impact->resetMembers)++epochs[id];
        for(auto& m:g.modulesMutable())if(epochs[m.index])m.code+="\n// selective reference restart "+std::to_string(epochs[m.index])+"\n";
        auto next=graph.prepare(g,plan.get(),plan?plan->revision+1:1);
        if(impact)for(auto& r:next->routes)if(r.delay&&impact->resetFeedbackSources.count(r.source))
            r.delay=std::make_shared<curlop::transport::FeedbackBoundaryState>(r.delay->descriptor());
        plan.swap(next);
    }
    void render(juce::AudioBuffer<float>& b){require(RGraph::render(*plan,b),"flat reference failed");}
};
void coldOracle(const GraphState& g,const Pins& pins,int width,int frames,const fs::path& out,const std::string& label){
    auto mono=build(g,frames);adaptive::Engine cold(width,frames,(out/(label+"-cache")).string());cold.apply(g,pins);
    juce::AudioBuffer<float>b(2,frames),r(2,frames);juce::MidiBuffer midi;std::vector<float>x,y;
    for(int offset=0;offset<2048;offset+=frames){cold.render(b);process(*mono.renderer,r,midi);auto a=interleave(b),ref=interleave(r);append(x,a);append(y,ref);compare(a,ref,label+" fresh product oracle");}
    raw(out/(label+"-adaptive.f32"),x);raw(out/(label+"-product.f32"),y);
}
void eventRow(std::ofstream& f,int k,const std::string& label,const Impact& i,double mutation,double total,const adaptive::Engine& e){
    f<<k<<'\t'<<label<<'\t'<<mutation<<'\t'<<i.planningUs<<'\t'<<i.preparationUs<<'\t'<<total<<'\t'<<i.created<<'\t'<<i.reused<<'\t'<<i.acquired<<'\t'
     <<i.hits<<'\t'<<i.misses<<'\t'<<integers(i.resetMembers)<<'\t'<<e.layout.units.size()<<'\t'<<e.plan->singleSample<<'\n';f.flush();
}
void trace(const experiment::Config& c,int frames,const fs::path& out){
    auto fixture=experiment::makeFixture(c);auto& g=fixture.graph;Pins pins;
    if(c.family=="serialshuffled"){
        auto ms=g.modules();auto es=g.edges();auto permute=[&](int id){return id>=1&&id<=c.size?c.size+1-id:id;};
        for(auto& m:ms)m.index=permute(m.index);for(auto& e:es){e.srcIndex=permute(e.srcIndex);e.tgtIndex=permute(e.tgtIndex);}
        require(g.replace(std::move(ms),std::move(es),{}),"nonmonotone index fixture");
    }
    const int monitor=fixture.marker+4;addNode(g,module(monitor,"monitor","process=_,_;",true));addWire(g,monitor,fixture.output);
    adaptive::Engine engine(c.width,frames,(out/"candidate-cache").string());auto initial=engine.apply(g);
    require(!engine.layout.units.empty(),"automatic seed made no compiled groups");
    FlatReference reference(frames,out);reference.apply(g);coldOracle(g,pins,c.width,frames,out,"initial");
    const auto initialLayout=engine.layout;graphRecord(g,engine,out/"graph-initial.json");
    const auto target=initialLayout.units.back();const bool parallel=target.parallel;const int a=target.members.front(),b=target.members[1],last=target.members.back();
    const int inserted=fixture.marker+7;const auto originalModule=entry(g,b);const auto originalSource=entry(g,last).code;
    std::ofstream rows(out/"edits.tsv");rows<<"event\tname\tmutation_us\tplan_us\tprepare_us\tedit_compute_us\tcreated\treused\tacquired\tsource_hits\tsource_misses\treset_members\tgroups\tsample_mode\n"<<std::setprecision(16);
    std::ofstream blocks(out/"blocks.tsv");blocks<<"block\tevent\n";int block=0;
    juce::AudioBuffer<float>xbuf(2,frames),rbuf(2,frames);std::vector<float>x,y;
    auto capture=[&](int event,bool candidateAlready=false){if(!candidateAlready)engine.render(xbuf);reference.render(rbuf);auto cur=interleave(xbuf),ref=interleave(rbuf);
        append(x,cur);append(y,ref);compare(cur,ref,"live trace "+std::to_string(event));blocks<<block++<<'\t'<<event<<'\n';};
    for(int offset=0;offset<4096;offset+=frames)capture(-1);
    std::set<int> exposedTarget=parallel?std::set<int>(target.members.begin(),target.members.end()):std::set<int>{a};
    std::vector<std::string> names={"external-connect","external-disconnect","expose-hidden-input","connect-exposed-input","disconnect-exposed-input",
      "expose-hidden-output","connect-exposed-output","disconnect-exposed-output","internal-endpoint-rewire","undo-endpoint-rewire",
      "insert-node","remove-inserted-node","member-source-change","undo-member-source","delete-member","restore-member","explicit-compact","repeat-after-compact"};
    for(int k=0;k<int(names.size());++k){const auto begin=now();bool compact=false;
        switch(k){
          case 0:for(int id:exposedTarget)addWire(g,fixture.marker,id);break;
          case 1:for(int id:exposedTarget)removeWire(g,fixture.marker,id);break;
          case 2:pins.inputs.insert(b);break;
          case 3:addWire(g,fixture.marker,b);break;
          case 4:removeWire(g,fixture.marker,b);break;
          case 5:pins.outputs.insert(b);break;
          case 6:addWire(g,b,monitor);break;
          case 7:removeWire(g,b,monitor);break;
          case 8:if(parallel){removeWire(g,0,last);addWire(g,fixture.marker,last);}else{
              int from=target.members[target.members.size()-2];removeWire(g,from,last);addWire(g,a,last);}break;
          case 9:if(parallel){removeWire(g,fixture.marker,last);addWire(g,0,last);}else{
              int from=target.members[target.members.size()-2];removeWire(g,a,last);addWire(g,from,last);}break;
          case 10:{auto m=module(inserted,"inserted","process=*(0.6),*(0.6);",true);addNode(g,m);
              int src=parallel?0:target.members[target.members.size()-2];removeWire(g,src,last);addWire(g,src,inserted);addWire(g,inserted,last);break;}
          case 11:{int src=parallel?0:target.members[target.members.size()-2];removeNode(g,inserted);addWire(g,src,last);break;}
          case 12:{auto& m=mutableEntry(g,last);auto at=m.code.find("process");require(at!=std::string::npos,"source-edit anchor");
              m.code.replace(at,7,"priorprocess");m.code+="\nprocess=priorprocess : *(0.75),*(0.75);\n";break;}
          case 13:mutableEntry(g,last).code=originalSource;break;
          case 14:pins.inputs.erase(b);pins.outputs.erase(b);removeNode(g,b);if(!parallel)addWire(g,a,target.members[2]);break;
          case 15:if(!parallel)removeWire(g,a,target.members[2]);addNode(g,originalModule);
              if(parallel){addWire(g,0,b);auto route=edge(b,fixture.output,1.f/c.size);require(g.addEdge(route),"restore parallel output");}
              else{addWire(g,a,b);addWire(g,b,target.members[2]);}break;
          case 16:pins={};compact=true;break;
          case 17:break;
        }
        const double afterMutation=now();auto impact=engine.apply(g,pins,compact);engine.render(xbuf);const double rendered=now();
        // A legal routing-only edit is not permitted to recreate any DSP.
        if(k==0||k==1||k==3||k==4||k==6||k==7||k==17)require(impact.created==0&&impact.acquired==0,"routing-only edit rebuilt a processor");
        if(k==2)require(impact.created>0,"hidden input did not split its compiled group");
        if(k==17)require(impact.misses==0,"unchanged groups reassembled source");
        reference.apply(g,&impact);capture(k,true);for(int n=0;n<3;++n)capture(k);
        eventRow(rows,k,names[k],impact,afterMutation-begin,rendered-begin,engine);
        graphRecord(g,engine,out/("graph-"+std::to_string(k)+".json"));
    }
    raw(out/"live-adaptive.f32",x);raw(out/"live-selective-reference.f32",y);
    coldOracle(g,pins,c.width,frames,out,"final");
    // Same retainer, same work; this is a cost of the selected boundaries, not
    // a product observer/meter difference. No claim of universal improvement.
    std::ofstream perf(out/"throughput.tsv");perf<<"trial\tbackend\tns_per_frame\n"<<std::setprecision(16);
    std::mt19937 rng(43);for(int trial=0;trial<7;++trial){bool swap=rng()%2;for(int j=0;j<2;++j){bool grouped=(j==int(swap));auto begin=now();
        for(int n=0;n<512;++n){if(grouped)engine.render(xbuf);else reference.render(rbuf);}auto elapsed=now()-begin;
        perf<<trial<<'\t'<<(grouped?"adaptive":"individual")<<'\t'<<elapsed*1000/(512.0*frames)<<'\n';}}
}
Fixture stateFixture(){
    auto f=fixture("parallel",8);std::vector<EdgeEntry> edges;
    for(int i=1;i<=8;++i){mutableEntry(f.graph,i).code=(i==1||i==5)?
       "import(\"stdfaust.lib\"); process(x,y)=os.osc(173)*0.1+x*0.001,((1-1')@6000)*0.05+y*0.001;":"process=_,_;";
       edges.push_back(edge(i==1||i==5?0:i-1,i));if(i==4||i==8)edges.push_back(edge(i,f.output));}
    require(f.graph.replace(f.graph.modules(),edges,{}),"two-island memory fixture");return f;
}
void state(int frames,const fs::path& out,bool identity){
    auto f=stateFixture();adaptive::Engine engine(4,frames,(out/"candidate-cache").string());engine.apply(f.graph);
    require(engine.layout.units.size()==2&&engine.layout.owner.at(4)==1&&engine.layout.owner.at(8)==5,"memory fixture not automatically grouped");
    auto mono=build(f.graph,frames);juce::AudioBuffer<float>b(2,frames),ref(2,frames);juce::MidiBuffer midi;
    std::size_t tap1=0,tap2=0;bool found1=false,found2=false;
    for(std::size_t i=0;i<mono.renderer->tapCount();++i){auto id=mono.renderer->tapModuleId(i);if(id=="latency-fx4"){tap1=i;found1=true;}if(id=="latency-fx8"){tap2=i;found2=true;}}
    require(found1&&found2,"product memory taps missing");
    for(int offset=0;offset<4096;offset+=frames){engine.render(b);process(*mono.renderer,ref,midi);}
    Pins p;if(identity)mutableEntry(f.graph,6).moduleId="replacement-object-6";else p.inputs.insert(7);auto old=engine.plan->nodes[1];auto impact=engine.apply(f.graph,p);
    require(engine.plan->nodes[1]==old&&impact.resetMembers==std::set<int>({5,6,7,8}),"split touched wrong state island");
    std::vector<float> untouched,oracle,affected,uninterrupted;
    int affectedOwner=adaptive::owner(engine.layout,8);
    for(int offset=0;offset<8192;offset+=frames){engine.render(b);process(*mono.renderer,ref,midi);
        auto x=experiment::tap(*engine.plan,1,frames);append(untouched,x);append(affected,experiment::tap(*engine.plan,affectedOwner,frames));
        std::vector<float> y,z;for(int s=0;s<frames;++s)for(int c=0;c<2;++c){y.push_back(mono.renderer->tapSample(tap1,c,s));z.push_back(mono.renderer->tapSample(tap2,c,s));}
        append(oracle,y);append(uninterrupted,z);compare(x,y,"untouched island after real split");}
    raw(out/"untouched.f32",untouched);raw(out/"untouched-product.f32",oracle);raw(out/"affected-reset.f32",affected);raw(out/"affected-uninterrupted.f32",uninterrupted);
    require(delta(affected,uninterrupted)>1e-3,"affected reset was not observable");
    graphRecord(f.graph,engine,out/"split-graph.json");
}
void negative(int frames,const fs::path& out){
    auto f=stateFixture();adaptive::Engine engine(4,frames,(out/"cache").string());engine.apply(f.graph);
    FlatReference reference(frames,out);reference.apply(f.graph);juce::AudioBuffer<float>a(2,frames),b(2,frames);
    std::ofstream rows(out/"rejections.tsv");rows<<"case\trejected\tplan_unchanged\n";std::vector<float>x,y;
    for(int k=0;k<6;++k){auto bad=f.graph;Pins pins;bool preserve=false;std::string label;
        if(k==0){pins.inputs.insert(7);preserve=true;label="preserve-only-split";}
        if(k==1){
            addWire(bad,4,1);
            // GraphState normally inserts z^-1 automatically. Deliberately
            // corrupt only this private negative fixture after ingress, so
            // the validator sees an actually undeclared cycle.
            auto& malformed=const_cast<std::vector<EdgeEntry>&>(bad.edges());
            for(auto& e:malformed)if(e.srcIndex==4&&e.tgtIndex==1)e.feedbackBoundary=curlop::FeedbackBoundary::None;
            label="undeclared-cycle";
        }
        if(k==2){mutableEntry(bad,6).physicalVoices=4;label="hidden-polyphony";}
        if(k==3){mutableEntry(bad,6).code="process=;";label="bad-hidden-source";}
        if(k==4){pins.outputs.insert(210);label="absent-pin";}
        if(k==5){mutableEntry(bad,6).moduleId=entry(bad,1).moduleId;label="duplicate-identity";}
        auto* old=engine.plan.get();bool rejected=false;try{engine.apply(bad,pins,false,preserve);}catch(const std::exception&){rejected=true;}
        require(rejected&&engine.plan.get()==old,"failed edit changed active plan");rows<<label<<"\t1\t1\n";
        for(int n=0;n<8;++n){engine.render(a);reference.render(b);auto aa=interleave(a),bb=interleave(b);append(x,aa);append(y,bb);compare(aa,bb,"audio after rejected edit");}}
    raw(out/"after-reject-adaptive.f32",x);raw(out/"after-reject-reference.f32",y);
}
void feedback(int frames,const fs::path& out){
    auto f=fixture("serial",16);adaptive::Engine engine(4,frames,(out/"cache").string());engine.apply(f.graph);
    FlatReference reference(frames,out);reference.apply(f.graph);juce::AudioBuffer<float>a(2,frames),b(2,frames);std::vector<float>x,y;
    std::ofstream events(out/"feedback.tsv");events<<"event\tcreated\tacquired\tsample_mode\n";
    for(int event=0;event<4;++event){
        if(event==1)addWire(f.graph,12,5,true);if(event==2)removeWire(f.graph,12,5);
        auto impact=engine.apply(f.graph);reference.apply(f.graph,&impact);require(impact.created==0,"external feedback edit unnecessarily rebuilt a group");
        events<<event<<'\t'<<impact.created<<'\t'<<impact.acquired<<'\t'<<engine.plan->singleSample<<'\n';
        for(int n=0;n<32;++n){engine.render(a);reference.render(b);auto aa=interleave(a),bb=interleave(b);append(x,aa);append(y,bb);compare(aa,bb,"dynamic feedback");}}
    raw(out/"feedback-adaptive.f32",x);raw(out/"feedback-reference.f32",y);
}
void planner(const experiment::Config& c,int frames,const fs::path& out){
    auto f=experiment::makeFixture(c);adaptive::View view(f.graph,frames);auto initial=adaptive::seed(view,c.width);
    auto l=adaptive::maintain(view,initial,{});std::ofstream rows(out/"planner.tsv");rows<<"trial\tgroups\tcoverage\n";
    for(int trial=0;trial<128;++trial){Pins p;int id=1+(trial%c.size);p.inputs.insert(id);p.outputs.insert(id);
        l=adaptive::maintain(view,l,p);std::set<int> covered;
        for(auto& b:l.units)for(int member:b.members)require(covered.insert(member).second,"overlapping planned units");
        auto shaped=collapse(f.graph,l); // Independent existing mapper checks every port.
        require(!shaped.modules().empty(),"empty plan");rows<<trial<<'\t'<<l.units.size()<<'\t'<<covered.size()<<'\n';}
    require(l.units.empty(),"progressively opening every node must reach individual execution");
}
}
int main(int argc,char** argv){
    if(argc>1&&std::string(argv[1])=="previous-groups")return stableGroupEntrypoint(argc-1,argv+1);
    try{require(argc==7,"usage: AdaptiveGroupBench mode family count width frames output");std::string mode=argv[1];
        experiment::Config c{argv[2],std::stoi(argv[3]),std::stoi(argv[4])};int frames=std::stoi(argv[5]);require((frames==64||frames==128)&&c.size>=8&&c.size<=64&&c.width>=4&&c.width<=8,"bounded arguments");
        fs::path out=fs::absolute(argv[6]);fs::create_directories(out);
        if(mode=="trace")next_slice::trace(c,frames,out);else if(mode=="state")next_slice::state(frames,out,c.family=="identity");else if(mode=="negative")next_slice::negative(frames,out);
        else if(mode=="feedback")next_slice::feedback(frames,out);else if(mode=="planner")next_slice::planner(c,frames,out);else throw std::runtime_error("bad mode");
        curlop::FaustRuntime::instance().releaseAllFactories();std::cout<<"PASS "<<mode<<' '<<c.family<<' '<<frames<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<"ADAPTIVE_GROUP_FAILURE: "<<e.what()<<'\n';return 2;}}
