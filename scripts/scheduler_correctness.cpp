// Correctness only. Continues the existing 64-voice probe; not a speed benchmark.
#include <faust/dsp/llvm-dsp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <execinfo.h>
#include <unistd.h>

namespace {
using Audio = std::vector<float>;
using Instance = std::unique_ptr<dsp>;
constexpr int kGuard = 16, kMax = 513;
constexpr float kSentinel = 1234567.0f;
int comparisons = 0, overlapRetire = 0;
double worst = 0;
std::string evidencePrefix;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
std::string read(const std::string& p) {
    std::ifstream f(p, std::ios::binary); require(bool(f), "open " + p);
    std::ostringstream s; s << f.rdbuf(); require(!f.bad(), "read " + p); return s.str();
}
void write(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary); f.write(s.data(), s.size()); f.close(); require(bool(f), "write " + p);
}
std::string env(const char* k) { const char* v = std::getenv(k); return v ? v : ""; }
struct Factory {
    llvm_dsp_factory* p = nullptr;
    Factory() = default;
    Factory(const Factory&) = delete;
    ~Factory() { if (p) deleteDSPFactory(p); }
    void close() { if (p) { auto* old = p; p = nullptr; require(deleteDSPFactory(old), "factory retained unexpectedly"); } }
    void load(const std::string& path) {
        require(!p, "factory already loaded"); std::string error;
        p = readDSPFactoryFromBitcode(read(path), env("FAUST_JIT_TARGET"), error, -1);
        require(p != nullptr, "bitcode load: " + error);
    }
    void build(const std::string& source, const std::string& path, const std::string& kind) {
        require(!p, "factory already loaded"); std::string error;
        std::vector<std::string> options;
        if (kind == "sch") options = {"-sch", "-L", env("FAUST_SCHEDULER_MODULE")};
        std::vector<const char*> args; for (auto& s: options) args.push_back(s.c_str());
        p = createDSPFactoryFromString("ExistingProbeCorrectness", source, int(args.size()), args.data(), env("FAUST_JIT_TARGET"), error, -1);
        require(p != nullptr, "source build: " + error);
        write(path, writeDSPFactoryToBitcode(p));
        std::cout << "options=" << p->getCompileOptions() << '\n';
    }
    Instance instance(int rate) {
        Instance d(p->createDSPInstance()); require(bool(d), "null DSP");
        require(d->getNumInputs() == 0 && d->getNumOutputs() == 2, "channel contract");
        d->init(rate); return d;
    }
};
Audio render(dsp* d, int total, bool varied) {
    const int pattern[] = {1,3,7,15,16,31,32,33,63,64,65,127,128,129,255,256,257,511,512,513};
    std::vector<float> l(kMax+2*kGuard), r(l.size());
    float* out[] = {l.data()+kGuard, r.data()+kGuard};
    Audio a; a.reserve(size_t(total)*2); int done=0, step=0;
    while (done < total) {
        int n = std::min(total-done, varied ? pattern[step++ % 20] : 256);
        std::fill(l.begin(), l.end(), kSentinel); std::fill(r.begin(), r.end(), kSentinel);
        d->compute(n, nullptr, out);
        for (auto* b: {&l,&r}) {
            for(int i=0;i<kGuard;++i) require((*b)[i]==kSentinel, "prefix overwrite");
            for(size_t i=kGuard+n;i<b->size();++i) require((*b)[i]==kSentinel, "suffix overwrite count="+std::to_string(n));
        }
        for(int i=0;i<n;++i) for(auto* b: out) {
            require(std::isfinite(b[i]) && b[i]!=kSentinel, "unwritten/nonfinite output"); a.push_back(b[i]);
        }
        done+=n;
    }
    return a;
}
void compare(const Audio& a, const Audio& b, const std::string& label) {
    require(a.size()==b.size(), "sample count " + label); double mx=0, ss=0;
    for(size_t i=0;i<a.size();++i) { double e=double(a[i])-b[i]; mx=std::max(mx,std::abs(e)); ss+=e*e; }
    ++comparisons; worst=std::max(worst,mx);
    bool exact=std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0;
    std::cout << "comparison=" << label << " samples=" << a.size() << " max_error=" << std::setprecision(17) << mx
              << " rms=" << std::sqrt(ss/a.size()) << " bit_identical=" << exact << '\n';
    if(mx>1e-7) {
        write(evidencePrefix+"-actual.f32",std::string(reinterpret_cast<const char*>(a.data()),a.size()*sizeof(float)));
        write(evidencePrefix+"-reference.f32",std::string(reinterpret_cast<const char*>(b.data()),b.size()*sizeof(float)));
        throw std::runtime_error("numerical mismatch " + label);
    }
}
Audio reference(Factory& f, int rate, int count, bool varied, int pre=0) {
    auto d=f.instance(rate); if(pre) render(d.get(),pre,false); return render(d.get(),count,varied);
}
void paired(dsp* a,dsp* b,int count,bool varied,Audio& x,Audio& y) {
    std::atomic<int> ready{0}; std::atomic<bool> go{false}; std::exception_ptr ea,eb;
    auto work=[&](dsp* d,Audio& v,std::exception_ptr& e){
        ready.fetch_add(1); while(!go.load()) std::this_thread::yield();
        try{ v=render(d,count,varied); }catch(...){e=std::current_exception();}
    };
    std::thread ta(work,a,std::ref(x),std::ref(ea)),tb(work,b,std::ref(y),std::ref(eb));
    while(ready.load()!=2) std::this_thread::yield(); go=true; ta.join(); tb.join();
    if(ea) std::rethrow_exception(ea); if(eb) std::rethrow_exception(eb);
}
void terminateTrace() noexcept {
    void* f[80]; int n=backtrace(f,80); backtrace_symbols_fd(f,n,2); std::abort();
}
}
int main(int argc,char** argv) {
    std::cout << std::unitbuf; std::cerr << std::unitbuf; std::set_terminate(terminateTrace);
    // prepare|case kind scenario threads rate source cacheA cacheB evidence-prefix
    if(argc!=10) return 2;
    const std::string mode=argv[1],kind=argv[2],scenario=argv[3];
    const int threads=std::stoi(argv[4]),rate=std::stoi(argv[5]);
    evidencePrefix=argv[9];
    try {
        require(threads>0 && threads<=64,"thread count");
        require(startMTDSPFactories(),"libfaust API lock init");
        Factory a,b;
        if(mode=="prepare") {
            auto sa=read(argv[6]),sb=sa; auto pos=sb.find("*(0.01)"); require(pos!=std::string::npos,"existing probe source changed");
            sb.replace(pos,7,"*(0.007)");
            a.build(sa,argv[7],kind); b.build(sb,argv[8],kind);
            require(a.p!=b.p,"expected distinct factories"); a.close(); b.close(); stopMTDSPFactories();
            std::cout << "stage=main_return\n"; return 0;
        }
        require(mode=="case","mode");
        require(env("FAUST_SCHEDULER_MODULE").empty(),"reader must not receive scheduler module");
        setenv("OMP_NUM_THREADS","1",1); setenv("OMP_DYN_THREAD","0",1);
        a.load(argv[7]); b.load(argv[8]); require(a.p!=b.p,"distinct factories aliased");
        constexpr int count=32768, rounds=8;
        const bool varied=scenario!="fixed";
        const bool retire=scenario=="retire";
        int expectedCount=retire?count*rounds:count;
        // References execute before participants change; all reference workers are joined.
        auto refA=reference(a,rate,expectedCount,varied);
        auto refB=reference(scenario=="overlap_same"?a:b,rate,retire?1024:count,varied,retire?0:37);
        if(scenario=="partition") {
            auto fixed=reference(a,rate,count,false); compare(refA,fixed,"variable-vs-fixed-one-participant");
        }
        setenv("OMP_NUM_THREADS",std::to_string(threads).c_str(),1);
        std::cout << "stage=exercise_begin scenario=" << scenario << " participants=" << threads << " rate=" << rate << '\n';
        if(scenario=="fixed" || scenario=="variable" || scenario=="partition") {
            auto d=a.instance(rate); compare(render(d.get(),count,varied),refA,"participants-vs-one");
        } else if(scenario=="lifetimes" || scenario=="factory_cycles") {
            if(scenario=="factory_cycles") a.close();
            for(int i=0;i<rounds;++i) {
                std::cout << "epoch=" << i << '\n';
                if(scenario=="factory_cycles") a.load(argv[7]);
                {auto d=a.instance(rate); compare(render(d.get(),count,true),refA,"lifetime-"+std::to_string(i));}
                if(scenario=="factory_cycles") a.close();
            }
        } else if(scenario=="overlap_same" || scenario=="overlap_distinct") {
            auto da=a.instance(rate),db=(scenario=="overlap_same"?a:b).instance(rate);
            render(db.get(),37,false); Audio x,y; paired(da.get(),db.get(),count,true,x,y);
            compare(x,refA,"concurrent-A"); compare(y,refB,"concurrent-B-offset37");
        } else if(retire) {
            b.close(); auto survivor=a.instance(rate); Audio all; all.reserve(refA.size());
            for(int i=0;i<rounds;++i) {
                std::cout << "epoch=" << i << '\n';
                b.load(argv[8]); auto outgoing=b.instance(rate);
                std::atomic<bool> began{false},done{false}; Audio chunk; std::exception_ptr error;
                std::thread live([&]{began=true; try {chunk=render(survivor.get(),count,true);}catch(...){error=std::current_exception();} done=true;});
                while(!began.load()) std::this_thread::yield();
                std::exception_ptr local;
                try {
                    auto tail=render(outgoing.get(),1024,true); compare(tail,refB,"outgoing-"+std::to_string(i));
                    if(!done.load()) ++overlapRetire;
                    outgoing.reset(); b.close();
                } catch(...) {local=std::current_exception();}
                live.join(); if(local) std::rethrow_exception(local); if(error) std::rethrow_exception(error);
                all.insert(all.end(),chunk.begin(),chunk.end());
            }
            compare(all,refA,"survivor-through-other-factory-retirements");
            require(overlapRetire>0,"retirement overlap not achieved");
            std::cout << "retire_while_other_render_active=" << overlapRetire << '\n';
        } else throw std::runtime_error("unknown scenario");
        std::cout << "stage=factory_cleanup\n"; a.close(); b.close(); stopMTDSPFactories();
        std::cout << "comparisons=" << comparisons << " worst_error=" << std::setprecision(17) << worst << "\nstage=main_return\n";
        return 0;
    } catch(const std::exception& e) {std::cerr << "failure=" << e.what() << '\n'; return 20;}
}
