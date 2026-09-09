// Extend the exact existing fixture/build/render code rather than a toy graph.
#define main baseline_checkpoint_main
#include "main.cpp"
#undef main
#include "RetainedFaustGraph.h"
#include <random>

namespace re {
using retained::Plan;
using retained::Graph;
static constexpr double absTolerance=1e-5,relTolerance=1e-5;
static void compare(const std::vector<float>& a,const std::vector<float>& b){
    require(a.size()==b.size(),"comparison length");
    for(std::size_t i=0;i<a.size();++i)
        require(std::isfinite(a[i])&&std::isfinite(b[i])&&std::abs(double(a[i])-b[i])<=absTolerance+relTolerance*std::abs(b[i]),"full sample comparison failed at "+std::to_string(i));
}
static void append(std::vector<float>& a,const std::vector<float>& b){a.insert(a.end(),b.begin(),b.end());}
static std::vector<float> source(const Plan& p,int count){
    std::vector<float> out(std::size_t(count)*2);
    for(int s=0;s<count;++s)for(int c=0;c<2;++c)out[2*s+c]=p.nodes[0]->out[c][s];return out;
}
static void render(const Plan& p,juce::AudioBuffer<float>& b){require(Graph::render(p,b),"retained render rejected");}
static void compareTaps(const Plan& p,Renderer& reference,int frames,bool ignoreMarker=false){
    for(std::size_t t=0;t<reference.tapCount();++t){
        auto id=reference.tapModuleId(t);
        for(auto& n:p.nodes)if(n&&n->id==id&&!n->output&&!(ignoreMarker&&n->id=="latency-marker"))
            for(int c=0;c<2;++c)for(int s=0;s<frames;++s){
                double v=reference.tapSample(t,c,s),x=n->out[c][s];
                require(std::isfinite(x)&&std::abs(x-v)<=absTolerance+relTolerance*std::abs(v),"retained tap mismatch: "+n->id);
            }
    }
}
static void cost(const fs::path& out,Plan& p,Renderer& fused,int frames){
    juce::AudioBuffer<float>b(2,frames);juce::MidiBuffer midi;
    std::ofstream f(out/"throughput.tsv");f<<"trial\tbackend\tns_per_frame\n"<<std::setprecision(14);
    std::mt19937 random(20260908);
    for(int trial=0;trial<7;++trial){
        const bool first=random()%2;
        for(int order=0;order<2;++order){
            const bool candidate=order==int(first);double begin=now();
            for(int k=0;k<512;++k){if(candidate)render(p,b);else process(fused,b,midi);}
            double elapsed=now()-begin;
            auto x=interleave(b);require(!x.empty(),"missing timed output");
            f<<trial<<'\t'<<(candidate?"retained":"fused")<<'\t'<<elapsed*1000/(512.0*frames)<<'\n';
        }
    }
}
static void matrix(const fs::path& out,const std::string& family,int n,int frames){
    auto f=fixture(family,n);auto mono=build(f.graph,frames);
    Graph graph(48000,frames,(out/"private-store-unused").string());
    auto start=now();auto p=graph.prepare(f.graph,nullptr,1);auto initialEnd=now();
    std::ofstream meta(out/"initial.tsv");meta<<"prepare_ms\tcreated\tacquired\n"<<std::setprecision(14)<<(initialEnd-start)/1000<<'\t'<<p->created<<'\t'<<p->acquired<<'\n';
    juce::AudioBuffer<float>b(2,frames),ref(2,frames);juce::MidiBuffer midi;
    std::vector<float> actual,reference;
    std::ofstream blocks(out/"blocks.tsv");blocks<<"block\tmarker_connected\n";int block=0;
    auto blockCheck=[&](bool connected){
        render(*p,b);process(*mono.renderer,ref,midi);
        auto x=interleave(b),y=interleave(ref),expected=y;
        for(std::size_t i=0;i<y.size();++i)expected[i]+=connected?float(markerExpected(i)):0;
        compare(x,expected);compareTaps(*p,*mono.renderer,frames);
        append(actual,x);append(reference,y);blocks<<block++<<'\t'<<connected<<'\n';
    };
    for(int k=0;k<4096/frames;++k)blockCheck(false);
    std::ofstream edits(out/"edits.tsv");edits<<"trial\tconnected\tprepare_us\tedit_to_changed_compute_us\tcreated\treused\tacquired\n"<<std::setprecision(14);
    for(int k=0;k<32;++k){
        auto t0=now();marker(f,k%2==0);auto t1=now();auto next=graph.prepare(f.graph,p.get(),k+2);auto t2=now();
        require(next->created==0&&next->acquired==0&&next->reused==f.graph.modules().size(),"wiring edit recreated DSP");
        for(std::size_t i=0;i<p->nodes.size();++i)require(next->nodes[i]==p->nodes[i],"instance not retained");
        p.swap(next);render(*p,b);auto t3=now();process(*mono.renderer,ref,midi);
        auto x=interleave(b),y=interleave(ref),expected=y;
        for(std::size_t i=0;i<y.size();++i)expected[i]+=k%2==0?float(markerExpected(i)):0;
        compare(x,expected);compareTaps(*p,*mono.renderer,frames);
        append(actual,x);append(reference,y);blocks<<block++<<'\t'<<(k%2==0)<<'\n';
        edits<<k<<'\t'<<(k%2==0)<<'\t'<<t2-t1<<'\t'<<t3-t0<<'\t'<<p->created<<'\t'<<p->reused<<'\t'<<p->acquired<<'\n';
        for(int k2=0;k2<3;++k2)blockCheck(k%2==0);
    }
    raw(out/"edited.f32",actual);raw(out/"continued-fused.f32",reference);
    cost(out,*p,*mono.renderer,frames);
}
// Reference has a scripted zero->constant source at frame 4096. Candidate does
// not predeclare the target connection: it creates a new routing plan at 4096.
static void novel(const fs::path& out,const std::string& family,int n,int frames,bool nonlinear=false){
    auto f=fixture(family,n);if(nonlinear)f.graph.modulesMutable()[1].code="sat(x)=x/(1.0+abs(x)); process=_,_ : *(3),*(3) : sat,sat;";
    auto oracle=f;oracle.graph.modulesMutable().back().code=
        "time=+(1)~_; step=float(time>4096); process=0.02*step,0.01*step;";
    int target=std::max(1,n/2);require(oracle.graph.addEdge(edge(f.marker,target)),"oracle edge");
    auto mono=build(oracle.graph,frames);Graph graph(48000,frames,(out/"store").string());
    auto p=graph.prepare(f.graph,nullptr,1);
    juce::AudioBuffer<float>b(2,frames),ref(2,frames);juce::MidiBuffer midi;std::vector<float>x,y;
    for(int offset=0;offset<8192;offset+=frames){
        if(offset==4096){
            auto t0=now();require(f.graph.addEdge(edge(f.marker,target)),"novel edge rejected");
            auto next=graph.prepare(f.graph,p.get(),2);auto t1=now();
            require(next->created==0&&next->acquired==0,"novel route caused compile");p.swap(next);
            std::ofstream evidence(out/"novel.tsv");evidence<<"source\ttarget\tprepare_plus_mutation_us\tcreated\tacquired\n"<<std::setprecision(14)<<f.marker<<'\t'<<target<<'\t'<<t1-t0<<"\t0\t0\n";
        }
        render(*p,b);process(*mono.renderer,ref,midi);auto a=interleave(b),r=interleave(ref);
        compare(a,r);compareTaps(*p,*mono.renderer,frames,true);append(x,a);append(y,r);
    }
    raw(out/"novel-edited.f32",x);raw(out/"scripted-fused-oracle.f32",y);
}
static void continuity(const fs::path& out,int frames){
    auto f=fixture("serial",4,true);Graph graph(48000,frames,(out/"store").string());
    auto p=graph.prepare(f.graph,nullptr,1);auto mono=build(f.graph,frames);
    juce::AudioBuffer<float>b(2,frames),ref(2,frames);juce::MidiBuffer midi;
    for(int n=0;n<4096/frames;++n){render(*p,b);process(*mono.renderer,ref,midi);}
    marker(f,true);auto next=graph.prepare(f.graph,p.get(),2);
    require(next->created==0&&next->nodes[0]==p->nodes[0],"continuity reset");p.swap(next);
    auto reset=graph.prepare(f.graph,nullptr,3);std::vector<float>x,y,z;
    const auto tap=sourceTap(*mono.renderer);
    for(int n=0;n<8192/frames;++n){
        render(*p,b);process(*mono.renderer,ref,midi);append(x,source(*p,frames));
        for(int s=0;s<frames;++s)for(int c=0;c<2;++c)y.push_back(mono.renderer->tapSample(tap,c,s));
        render(*reset,b);append(z,source(*reset,frames));
    }
    compare(x,y);require(delta(x,z)>0.01,"reset negative control not detected");
    raw(out/"preserved-source.f32",x);raw(out/"continued-source.f32",y);raw(out/"reset-negative.f32",z);
}
static void changeSource(const fs::path& out,int frames){
    auto f=fixture("serial",4,true);Graph graph(48000,frames,(out/"store").string());
    auto p=graph.prepare(f.graph,nullptr,1);auto mono=build(f.graph,frames);
    juce::AudioBuffer<float>b(2,frames),ref(2,frames);juce::MidiBuffer midi;
    for(int k=0;k<4096/frames;++k){render(*p,b);process(*mono.renderer,ref,midi);}
    f.graph.modulesMutable()[2].code="process=_,_;";double begin=now();auto next=graph.prepare(f.graph,p.get(),2);double elapsed=now()-begin;
    require(next->created==1&&next->acquired==1&&next->reused+1==f.graph.modules().size(),"source edit rebuilt unaffected nodes");
    for(std::size_t i=0;i<p->nodes.size();++i)require((next->nodes[i]==p->nodes[i])==(i!=2),"source identity mismatch");
    p.swap(next);std::vector<float>x,y;auto tap=sourceTap(*mono.renderer);
    for(int k=0;k<8192/frames;++k){render(*p,b);process(*mono.renderer,ref,midi);append(x,source(*p,frames));
        for(int s=0;s<frames;++s)for(int c=0;c<2;++c)y.push_back(mono.renderer->tapSample(tap,c,s));}
    compare(x,y);raw(out/"unaffected-source.f32",x);raw(out/"continued-source.f32",y);
    std::ofstream e(out/"source-edit.tsv");e<<"prepare_us\tcreated\treused\tacquired\n"<<std::setprecision(14)<<elapsed<<"\t1\t"<<p->reused<<"\t1\n";
    // Fail-closed controls must leave the installed plan untouched.
    auto bad=f;bad.graph.modulesMutable()[1].physicalVoices=4;bool rejected=false;
    try{graph.prepare(bad.graph,p.get(),4);}catch(const std::exception&){rejected=true;}require(rejected,"unsupported polyphony accepted");
    bad=f;bad.graph.modulesMutable()[1].code="process = ;";rejected=false;
    try{graph.prepare(bad.graph,p.get(),5);}catch(const std::exception&){rejected=true;}require(rejected,"invalid source accepted");
    std::ofstream(out/"negative-controls.txt")<<"unsupported-polyphony: rejected\ninvalid-source: rejected\n";
}
struct Mailbox {
    std::atomic<std::uint64_t> requested{1};std::atomic<const Plan*> pending{nullptr};
    bool publish(const Plan* p){if(p->revision!=requested.load(std::memory_order_acquire))return false;pending.store(p,std::memory_order_release);return true;}
    const Plan* adopt(const Plan* active) const noexcept {auto* p=pending.load(std::memory_order_acquire);
        return p&&p->revision==requested.load(std::memory_order_acquire)?p:active;}
};
struct Block {double start=0,end=0,scheduled=0;std::uint64_t revision=0;};
static void paced(const fs::path& out,const std::string& family,int n,int frames){
    auto f=fixture(family,n);auto mono=build(f.graph,frames);Graph graph(48000,frames,(out/"store").string());
    std::vector<std::unique_ptr<Plan>> owners;owners.push_back(graph.prepare(f.graph,nullptr,1));
    Mailbox mailbox;require(mailbox.publish(owners[0].get()),"initial publication");
    constexpr std::size_t capacity=8192;
    std::vector<Block> records(capacity);std::vector<float> audio(capacity*frames*2);
    std::atomic<bool> stop{false},started{false},failure{false};std::size_t completed=0;
    std::thread worker([&]{
        juce::AudioBuffer<float>b(2,frames);auto* active=owners[0].get();
        auto scheduled=Clock::now();auto period=std::chrono::nanoseconds(std::llround(1e9*frames/48000.0));
        started.store(true,std::memory_order_release);
        while(!stop.load(std::memory_order_acquire)&&completed<capacity){
            std::this_thread::sleep_until(scheduled);active=const_cast<Plan*>(mailbox.adopt(active));
            const auto begin=now();bool ok=Graph::render(*active,b);auto end=now();
            if(!ok){failure.store(true);break;}
            records[completed]={begin,end,us(scheduled),active->revision};
            for(int s=0;s<frames;++s)for(int c=0;c<2;++c)audio[(completed*frames+s)*2+c]=b.getSample(c,s);
            ++completed;scheduled+=period;
        }
    });
    ThreadGuard guard{stop,worker};while(!started.load(std::memory_order_acquire))std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::ofstream events(out/"events.tsv");events<<"revision\trequest_us\tprepared_us\tpublished_us\tcreated\tacquired\tstale_rejected\n"<<std::setprecision(16);
    for(std::uint64_t revision=2;revision<=17;++revision){
        double t0=now();mailbox.requested.store(revision,std::memory_order_release);marker(f,revision%2==0);
        auto next=graph.prepare(f.graph,owners.back().get(),revision);double prepared=now();
        require(next->created==0&&next->acquired==0,"paced edit compiled");
        bool rejected=!mailbox.publish(owners[0].get());require(rejected,"stale result accepted");
        owners.push_back(std::move(next));require(mailbox.publish(owners.back().get()),"publication failed");double published=now();
        events<<revision<<'\t'<<t0<<'\t'<<prepared<<'\t'<<published<<"\t0\t0\t1\n";events.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    stop.store(true,std::memory_order_release);worker.join();require(!failure.load()&&completed<capacity,"paced capacity/render failure");
    audio.resize(completed*frames*2);std::vector<float> oracle;oracle.reserve(audio.size());
    juce::AudioBuffer<float>b(2,frames);juce::MidiBuffer midi;
    std::ofstream rows(out/"callbacks.tsv");rows<<"block\trevision\tstart_us\tend_us\tscheduled_us\n"<<std::setprecision(16);
    for(std::size_t k=0;k<completed;++k){
        process(*mono.renderer,b,midi);auto x=interleave(b);append(oracle,x);
        rows<<k<<'\t'<<records[k].revision<<'\t'<<records[k].start<<'\t'<<records[k].end<<'\t'<<records[k].scheduled<<'\n';
        bool connected=records[k].revision>=2&&records[k].revision%2==0;
        for(std::size_t i=0;i<x.size();++i)x[i]+=connected?float(markerExpected(i)):0;
        std::vector<float> actual(audio.begin()+k*x.size(),audio.begin()+(k+1)*x.size());compare(actual,x);
    }
    raw(out/"paced-edited.f32",audio);raw(out/"paced-continued-fused.f32",oracle);
}
}
int main(int argc,char**argv){
    try{require(argc==6,"usage: RetainedPatchingBench mode family size frames output");
        std::string mode=argv[1],family=argv[2];int n=std::stoi(argv[3]),frames=std::stoi(argv[4]);
        require(n>=0&&n<=64&&(frames==64||frames==128),"bounded arguments");
        fs::path out=fs::absolute(argv[5]);fs::create_directories(out);
        if(mode=="matrix")re::matrix(out,family,n,frames);
        else if(mode=="novel")re::novel(out,family,n,frames);
        else if(mode=="nonlinear")re::novel(out,"serial",n,frames,true);
        else if(mode=="continuity")re::continuity(out,frames);
        else if(mode=="source")re::changeSource(out,frames);
        else if(mode=="paced")re::paced(out,family,n,frames);
        else throw std::runtime_error("unknown mode");
        curlop::FaustRuntime::instance().releaseAllFactories();
        std::cout<<"PASS "<<mode<<' '<<family<<' '<<n<<' '<<frames<<'\n';return 0;
    }catch(const std::exception&e){std::cerr<<"RETAINED_CHECKPOINT_FAILURE: "<<e.what()<<'\n';return 2;}
}
