// #254: actual unified Faust builder + rendered-sample checkpoint.
// Headless, with an optional normally scheduled paced thread. Not UI/Core Audio
// latency, not a new product executor, not incremental compiler-plan reuse.
#include <juce_core/juce_core.h>
#include <faust/dsp/llvm-dsp.h>
#include "graph/state/GraphState.h"
#include "graph/transport/FaustGraphRenderer.h"
#include "modules/backend/FaustRuntime.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using Renderer = curlop::transport::PreparedAudioRenderer;
using Build = curlop::transport::FaustGraphRendererBuild;
namespace fs = std::filesystem;
static double us(Clock::time_point p) {
    return std::chrono::duration<double, std::micro>(p.time_since_epoch()).count();
}
static double now() { return us(Clock::now()); }
// Native stereo wire pan=0 uses equal-power centre gains; never fit gain to audio.
static double markerExpected(std::size_t i) { return (i%2 ? 0.01 : 0.02)*0.7071067811865476; }
static void require(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
}
static void raw(const fs::path& p, const std::vector<float>& x) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(x.data()), std::streamsize(x.size()*sizeof(float)));
    require(bool(f), "raw write failed: " + p.filename().string());
}
static std::vector<float> interleave(const juce::AudioBuffer<float>& b) {
    std::vector<float> x(std::size_t(b.getNumSamples())*2);
    for (int i=0; i<b.getNumSamples(); ++i)
        for (int c=0; c<2; ++c) {
            x[std::size_t(i)*2+c] = b.getSample(c,i);
            require(std::isfinite(x[std::size_t(i)*2+c]), "nonfinite audio");
        }
    return x;
}
static void process(Renderer& r, juce::AudioBuffer<float>& b, juce::MidiBuffer& m) {
    b.clear(); m.clear(); require(r.processAudio(b,m), "prepared renderer rejected audio");
}
static Build build(const curlop::GraphState& g, int frames) {
    auto r = curlop::transport::buildFaustGraphRenderer(g, 48000, frames);
    require(bool(r.renderer), "actual graph build failed: " + r.diagnostic);
    require(r.renderer->rendererId().find("faust") != std::string_view::npos,
            "not the expected Faust renderer");
    return r;
}
static curlop::ModuleEntry module(int id, const std::string& name,
                                  const std::string& code, bool input) {
    curlop::ModuleEntry m;
    m.index=id; m.dslName=name; m.moduleId="latency-"+name;
    m.lineageId="core.faust_jit"; m.code=code;
    if (input) m.audioInputs={"IN"};
    m.audioOutputs={"OUT"}; return m;
}
static curlop::EdgeEntry edge(int a, int b, float gain=1) {
    curlop::EdgeEntry e; e.srcIndex=a; e.tgtIndex=b; e.gain=gain; return e;
}
struct Fixture {
    curlop::GraphState graph;
    int marker=0, output=0;
};
static Fixture fixture(const std::string& family, int count, bool memory=false) {
    Fixture f; f.output=count+1; f.marker=count+2;
    std::vector<curlop::ModuleEntry> modules;
    std::vector<curlop::EdgeEntry> edges;
    const std::string source = memory
      ? "import(\"stdfaust.lib\"); process = os.osc(173)*0.1, ((1-1') : de.delay(8192,6000))*0.05;"
      : "import(\"stdfaust.lib\"); process = os.osc(91)*0.08, os.osc(137)*0.07;";
    modules.push_back(module(0,"source",source,false));
    for (int i=1; i<=count; ++i) {
        const std::string frequency=std::to_string(1400+i*31);
        std::string code="import(\"stdfaust.lib\"); process = fi.lowpass(4,"+frequency+
                         "), fi.lowpass(4,"+std::to_string(1600+i*29)+");";
        auto m=module(i,"fx"+std::to_string(i),code,true);
        if (family=="control") {
            m.code="import(\"stdfaust.lib\"); cutoff=hslider(\"cutoff[unit:Hz]\","+
              frequency+",100,18000,1); process=fi.lowpass(4,cutoff),fi.lowpass(4,cutoff);";
            curlop::ParamSchemaEntry p;
            p.name="cutoff"; p.sourceId="cutoff"; p.min=100; p.max=18000;
            p.defaultValue=float(1400+i*31); p.unit="Hz";
            m.params={p}; m.paramValues["cutoff"]=p.defaultValue;
        }
        modules.push_back(std::move(m));
        edges.push_back(edge(family=="parallel" ? 0 : i-1, i));
        if (family=="parallel") edges.push_back(edge(i,f.output,1.0f/count));
    }
    if (family!="parallel") edges.push_back(edge(count,f.output));
    curlop::ModuleEntry output;
    output.index=f.output; output.dslName="output"; output.moduleId="latency-output";
    output.lineageId="core.output"; output.audioInputs={"IN"};
    output.paramValues["LEVEL"]=1;
    modules.push_back(output);
    modules.push_back(module(f.marker,"marker","process = 0.02, 0.01;",false));
    require(f.graph.replace(std::move(modules),std::move(edges),{}),"fixture graph rejected");
    if (family=="feedback") {
        auto feedback=edge(count,1,0.05f);
        feedback.feedbackBoundary=curlop::FeedbackBoundary::OneSample;
        require(f.graph.addEdge(feedback),"feedback edge rejected");
    }
    return f;
}
static void marker(Fixture& f, bool connected) {
    if (connected) require(f.graph.addEdge(edge(f.marker,f.output)),"marker connect rejected");
    else {
        const auto it=std::find_if(f.graph.edges().begin(),f.graph.edges().end(),[&](auto& e){
            return e.srcIndex==f.marker && e.tgtIndex==f.output;
        });
        require(it!=f.graph.edges().end(),"marker edge missing");
        const auto e=*it;
        f.graph.removeEdge(e.srcIndex,e.tgtIndex,e.srcPort,e.tgtPort);
    }
}
static double delta(const std::vector<float>& a, const std::vector<float>& b) {
    require(a.size()==b.size(),"audio length mismatch"); double d=0;
    for (std::size_t i=0;i<a.size();++i) d=std::max(d,std::abs(double(a[i])-b[i]));
    return d;
}
static std::vector<std::string> factoryKeys() {
    std::lock_guard<std::mutex> lock(curlop::FaustRuntime::compileMutex());
    auto keys=getAllDSPFactories(); std::sort(keys.begin(),keys.end()); return keys;
}
static std::string joinKeys(const std::vector<std::string>& keys) {
    std::string out;
    for(const auto& key:keys) {if(!out.empty())out+=",";out+=key;}
    return out;
}
static void matrix(const fs::path& out, const std::string& family, int n, int frames) {
    auto f=fixture(family,n);
    for (const auto& m:f.graph.modules()) if (!m.code.empty()) {
        std::ofstream file(out/(m.dslName+".dsp")); file<<m.code;
    }
    juce::AudioBuffer<float> b(2,frames); juce::MidiBuffer midi;
    std::ofstream rows(out/"edits.tsv");
    rows<<"action\tphase\ttrial\tmutation_us\tprepare_wall_us\tgraph_plan_us\tfactory_us\tinstance_init_us\tprepare_unattributed_us\tedit_to_first_render_complete_us\tfirst_render_us\tmax_reference_error\n";
    rows<<std::setprecision(14);
    std::ofstream cache(out/"factory-cache.tsv");
    cache<<"trial\tbefore_keys\tafter_keys\n";
    std::vector<std::unique_ptr<Renderer>> resident;
    curlop::FaustRuntime moduleCache;
    moduleCache.setStoreDirectory((out/"module-store-unused").string());
    std::vector<curlop::FaustRuntime::FactoryPtr> handles;
    std::vector<float> a,bAnchor;
    for (int k=-1;k<10;++k) {
        const bool connected=k>=0 && k%2==0;
        const auto beforeKeys=factoryKeys(); // Outside the edit timing interval.
        const auto start=now();
        if (k>=0) marker(f,connected);
        const auto mutationEnd=now();
        auto candidate=build(f.graph,frames);
        const auto prepared=now();
        const auto renderStart=now();
        process(*candidate.renderer,b,midi);
        const auto rendered=now();
        auto samples=interleave(b);
        cache<<k<<'\t'<<joinKeys(beforeKeys)<<'\t'<<joinKeys(factoryKeys())<<'\n';cache.flush();
        if (k==-1) a=samples;
        if (k==0) {
            bAnchor=samples;
            double maxMarkerError=0;
            for(std::size_t i=0;i<a.size();++i)
                maxMarkerError=std::max(maxMarkerError,std::abs(double(samples[i])-a[i]-markerExpected(i)));
            std::ofstream check(out/"marker-check.txt"); check<<std::setprecision(14)<<maxMarkerError<<"\n";
            require(maxMarkerError<=1e-5,"new connection failed independent additive marker oracle");
        }
        const auto err=delta(samples,connected?bAnchor:a);
        require(err<=1e-5,"repeat first block differs from frozen same-topology reference");
        const auto& t=candidate.timing;
        rows<<(k==-1?"initial":connected?"connect":"disconnect")<<'\t'
            <<(k==-1?"fresh_process_initial":k==0?"module_cache_warm_novel_topology":"graph_factory_resident_revisit")<<'\t'
            <<k<<'\t'<<mutationEnd-start<<'\t'<<prepared-mutationEnd<<'\t'
            <<t.graphPlanUs<<'\t'<<t.factoryUs<<'\t'<<t.instanceInitUs<<'\t'
            <<(prepared-mutationEnd-t.graphPlanUs-t.factoryUs-t.instanceInitUs)<<'\t'
            <<rendered-start<<'\t'<<rendered-renderStart<<'\t'<<err<<'\n'; rows.flush();
        raw(out/("first-"+std::to_string(k)+".f32"),samples);
        if (k<1) resident.push_back(std::move(candidate.renderer));
        // Warm authored factories only after timing A, before novel topology B.
        if(k==-1) {
            const auto warmStart=now();
            for(const auto& m:f.graph.modules()) if(!m.code.empty()) {
                std::string error;
                auto factory=moduleCache.acquire(m.dslName,m.code,error);
                require(bool(factory),"module warm failed: "+error);
                handles.push_back(std::move(factory));
            }
            std::ofstream warm(out/"module-warm.txt");
            warm<<std::setprecision(14)<<"wall_us="<<now()-warmStart<<"\ncache_size="<<moduleCache.cacheSize()<<"\n";
        }
    }
    resident.clear(); handles.clear(); moduleCache.releaseAllFactories();
}
static std::size_t sourceTap(Renderer& r) {
    for(std::size_t i=0;i<r.tapCount();++i) if(r.tapModuleId(i)=="latency-source") return i;
    throw std::runtime_error("source tap missing");
}
static std::vector<float> captureTap(Renderer& r, int total, int frames) {
    require(total%frames==0,"capture must have whole blocks");
    const auto tap=sourceTap(r); std::vector<float> x; x.reserve(std::size_t(total)*2);
    juce::AudioBuffer<float> b(2,frames); juce::MidiBuffer m;
    for(int offset=0;offset<total;offset+=frames) {
        process(r,b,m);
        for(int s=0;s<frames;++s) for(int c=0;c<2;++c) {
            const float v=r.tapSample(tap,c,s); require(std::isfinite(v),"nonfinite tap"); x.push_back(v);
        }
    }
    return x;
}
static void continuity(const fs::path& out, int frames) {
    auto f=fixture("serial",4,true);
    {std::ofstream source(out/"source.dsp");source<<f.graph.modules()[0].code;}
    auto fresh=build(f.graph,frames); auto anchor=captureTap(*fresh.renderer,8192,frames);
    auto ongoing=build(f.graph,frames); auto warm=captureTap(*ongoing.renderer,4096,frames);
    marker(f,true); auto replaced=build(f.graph,frames);
    auto continued=captureTap(*ongoing.renderer,8192,frames);
    auto replacement=captureTap(*replaced.renderer,8192,frames);
    raw(out/"fresh-anchor.f32",anchor); raw(out/"before-edit.f32",warm);
    raw(out/"continued-source.f32",continued); raw(out/"replaced-source.f32",replacement);
    std::ofstream result(out/"continuity.tsv");
    result<<"replacement_vs_continued_max_error\treplacement_vs_fresh_max_error\n"
          <<std::setprecision(14)<<delta(replacement,continued)<<'\t'<<delta(replacement,anchor)<<'\n';
    // Do not fail numerical validity merely because the product resets state.
    // Acceptance of seamless live patching is deliberately a separate verdict.
}
static void parameter(const fs::path& out, int frames) {
    auto f=fixture("serial",0);
    auto& s=f.graph.modulesMutable()[0];
    s.code="level=hslider(\"level\",0.1,0,1,0.001); process=level,level;";
    curlop::ParamSchemaEntry p; p.name="level";p.sourceId="level";p.defaultValue=.1f;p.min=0;p.max=1;
    s.params={p};s.paramValues["level"]=.1f;
    auto prepared=build(f.graph,frames);
    std::vector<float> controls(std::size_t(frames),.1f); const int rowMap[]={0};
    require(prepared.renderer->bindModuleControls(0,controls.data(),frames,rowMap,1,frames),"control binding failed");
    juce::AudioBuffer<float> b(2,frames); juce::MidiBuffer midi;
    process(*prepared.renderer,b,midi);
    std::ofstream result(out/"parameter.tsv");
    result<<"trial\tvalue\trow_update_to_first_render_complete_us\tfirst_sample\n"<<std::setprecision(14);
    for(int k=0;k<20;++k) {
        const float value=k%2?.1f:.2f; auto start=now();
        std::fill(controls.begin(),controls.end(),value);
        // The product deliberately clears borrowed control-row bindings after each block.
        require(prepared.renderer->bindModuleControls(0,controls.data(),frames,rowMap,1,frames),"per-block control binding failed");
        process(*prepared.renderer,b,midi);
        auto done=now(); auto samples=interleave(b);
        raw(out/("parameter-"+std::to_string(k)+".f32"),samples);
        for(float x:samples) require(std::abs(x-value*0.7071067811865476)<=1e-6,"parameter audio oracle failed");
        result<<k<<'\t'<<value<<'\t'<<done-start<<'\t'<<samples[0]<<'\n';
    }
}
struct CallbackRecord { double start,end,scheduled; bool replacement; };
struct ThreadGuard {
    std::atomic<bool>& stop; std::thread& t;
    ~ThreadGuard(){stop.store(true,std::memory_order_release);if(t.joinable())t.join();}
};
static void paced(const fs::path& out,const std::string& family,int count,int frames) {
    auto f=fixture(family,count); auto initial=build(f.graph,frames);
    juce::AudioBuffer<float> oracleBuffer(2,frames);juce::MidiBuffer oracleMidi;
    process(*initial.renderer,oracleBuffer,oracleMidi);
    const auto initialAnchor=interleave(oracleBuffer);
    auto active=build(f.graph,frames);
    std::atomic<Renderer*> published{active.renderer.get()};
    std::atomic<bool> stop{false},started{false},seen{false},failed{false};
    std::vector<CallbackRecord> history;history.reserve(20000);
    std::vector<float> firstAfter(std::size_t(frames)*2);
    double firstStart=0,firstComplete=0;
    std::thread audio([&]{
        juce::AudioBuffer<float> buffer(2,frames);juce::MidiBuffer midi;
        auto next=Clock::now();
        const auto period=std::chrono::nanoseconds(std::llround(1e9*frames/48000.0));
        Renderer* original=published.load(std::memory_order_acquire);
        started.store(true,std::memory_order_release);
        while(!stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_until(next);
            auto* r=published.load(std::memory_order_acquire);
            buffer.clear();midi.clear();const auto t0=Clock::now();
            bool valid=r->processAudio(buffer,midi); const auto t1=Clock::now();
            if(!valid || history.size()==history.capacity()) {failed.store(true);break;}
            history.push_back({us(t0),us(t1),us(next),r!=original});
            if(r!=original&&!seen.load(std::memory_order_relaxed)) {
                for(int i=0;i<frames;++i)for(int c=0;c<2;++c)
                    firstAfter[std::size_t(i)*2+c]=buffer.getSample(c,i);
                firstStart=us(t0);firstComplete=us(t1);seen.store(true,std::memory_order_release);
            }
            next+=period;
        }
    });
    Build candidate;
    ThreadGuard guard{stop,audio};
    while(!started.load(std::memory_order_acquire))std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto edit=now(); marker(f,true);const auto afterMutation=now();
    candidate=build(f.graph,frames);const auto prepared=now();
    // This pointer handoff is a measurement seam, NOT EngineSlot publication.
    // Both owners remain alive until after the rendering thread has joined.
    published.store(candidate.renderer.get(),std::memory_order_release);
    const auto publish=now();
    const auto limit=Clock::now()+std::chrono::seconds(3);
    while(!seen.load(std::memory_order_acquire)&&!failed.load()&&Clock::now()<limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true,std::memory_order_release);audio.join();
    require(seen.load()&&!failed.load(),"paced rendering failed or no changed block observed");
    double markerError=0;
    for(std::size_t i=0;i<firstAfter.size();++i) {
        require(std::isfinite(firstAfter[i]),"nonfinite published block");
        markerError=std::max(markerError,std::abs(double(firstAfter[i])-initialAnchor[i]-markerExpected(i)));
    }
    require(markerError<=1e-5,"paced first changed block failed additive audio oracle");
    raw(out/"first-published.f32",firstAfter); raw(out/"initial-anchor.f32",initialAnchor);
    std::ofstream times(out/"paced.tsv"); times<<std::setprecision(16);
    times<<"edit_us\tmutation_end_us\tprepared_us\tpublication_us\tfirst_changed_compute_start_us\tfirst_changed_compute_complete_us\tfactory_us\tgraph_plan_us\tinstance_init_us\tmarker_max_error\n"
         <<edit<<'\t'<<afterMutation<<'\t'<<prepared<<'\t'<<publish<<'\t'<<firstStart<<'\t'<<firstComplete<<'\t'
         <<candidate.timing.factoryUs<<'\t'<<candidate.timing.graphPlanUs<<'\t'<<candidate.timing.instanceInitUs<<'\t'<<markerError<<'\n';
    std::ofstream callbacks(out/"callbacks.tsv"); callbacks<<std::setprecision(16);
    callbacks<<"compute_start_us\tcompute_end_us\tscheduled_us\treplacement\n";
    for(const auto& r:history) callbacks<<r.start<<'\t'<<r.end<<'\t'<<r.scheduled<<'\t'<<r.replacement<<'\n';
}
int main(int argc,char**argv) {
    try {
        require(argc==6,"usage: PatchingLatencyBench mode family size frames output-directory");
        std::string mode=argv[1],family=argv[2];int n=std::stoi(argv[3]),frames=std::stoi(argv[4]);
        require(n>=0&&n<=64&&(frames==64||frames==128),"invalid bounded arguments");
        require(family=="serial"||family=="parallel"||family=="feedback"||family=="control","invalid family");
        fs::path out=fs::absolute(argv[5]); fs::create_directories(out);
        if(mode=="matrix")matrix(out,family,n,frames);
        else if(mode=="paced")paced(out,family,n,frames);
        else if(mode=="continuity")continuity(out,frames);
        else if(mode=="parameter")parameter(out,frames);
        else throw std::runtime_error("invalid mode");
        curlop::FaustRuntime::instance().releaseAllFactories();
        std::cout<<"completed="<<mode<<" family="<<family<<" size="<<n<<" frames="<<frames<<"\n";
        return 0;
    } catch(const std::exception&e) {std::cerr<<"CHECKPOINT_FAILURE: "<<e.what()<<'\n';return 2;}
}
