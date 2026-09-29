#include <faust/dsp/llvm-dsp.h>
#include <faust/dsp/libfaust.h>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int kSampleRate = 48000;

struct FactoryDeleter { void operator()(llvm_dsp_factory* p) const noexcept { if (p) deleteDSPFactory(p); } };
using FactoryPtr = std::unique_ptr<llvm_dsp_factory, FactoryDeleter>;

std::string target()
{
    if (const char* e = std::getenv("FAUST_JIT_TARGET"); e && *e) return e;
    return getDSPMachineTarget();
}

double usSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

FactoryPtr compileSource(const std::string& name, const std::string& source, double& us)
{
    std::string err;
    const auto t0 = Clock::now();
    auto* raw = createDSPFactoryFromString(name, source, 0, nullptr, target(), err, -1);
    us = usSince(t0);
    if (!raw) { std::cerr << "compile_error name=" << name << " error=" << err << "\n"; std::exit(3); }
    return FactoryPtr(raw);
}

std::string bitcodeFor(llvm_dsp_factory* f, double& us)
{
    const auto t0 = Clock::now();
    auto bc = writeDSPFactoryToBitcode(f);
    us = usSince(t0);
    if (bc.empty()) { std::cerr << "empty_bitcode\n"; std::exit(4); }
    return bc;
}

FactoryPtr loadBitcode(const std::string& bc, double& us)
{
    std::string err;
    const auto t0 = Clock::now();
    auto* raw = readDSPFactoryFromBitcode(bc, target(), err, -1);
    us = usSince(t0);
    if (!raw) { std::cerr << "bitcode_error " << err << "\n"; std::exit(5); }
    return FactoryPtr(raw);
}

double instanceInitUs(llvm_dsp_factory* f)
{
    const auto t0 = Clock::now();
    std::unique_ptr<dsp> d(f->createDSPInstance());
    if (!d) { std::cerr << "instance_error\n"; std::exit(6); }
    d->init(kSampleRate);
    return usSince(t0);
}

std::string moduleSource(int i, bool edited = false)
{
    std::ostringstream s;
    const int cutoff = 650 + i * 137 + (edited ? 73 : 0);
    s << "import(\"stdfaust.lib\"); process = fi.lowpass(2," << cutoff << ");\n";
    return s.str();
}

std::string fusedSource(int stages, int bypassIndex = -1, int editedModule = -1)
{
    std::ostringstream s;
    s << "import(\"stdfaust.lib\");\n"
      << "src = par(i,24,os.osc(80+i*7.13)) :> _;\n"
      << "process = src";
    for (int i = 0; i < stages; ++i) {
        if (i == bypassIndex) continue;
        const int cutoff = 650 + i * 137 + (i == editedModule ? 73 : 0);
        s << " : fi.lowpass(2," << cutoff << ")";
    }
    s << " : *(0.01);\n";
    return s.str();
}

struct Artifact { std::string bitcode; };
using Cache = std::map<std::string, Artifact>;

std::string hash(const std::string& s) { return generateSHA1(s); }

struct BuildResult {
    bool cacheHit = false;
    double compileUs = 0;
    double writeUs = 0;
    double loadUs = 0;
    double initUs = 0;
    size_t bytes = 0;
};

BuildResult acquireGraph(Cache& cache, const std::string& name, const std::string& source, bool forceBitcodeReload)
{
    BuildResult r;
    const auto key = hash(source);
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto f = compileSource(name, source, r.compileUs);
        auto bc = bitcodeFor(f.get(), r.writeUs);
        r.bytes = bc.size();
        cache.emplace(key, Artifact{bc});
        if (!forceBitcodeReload) {
            r.initUs = instanceInitUs(f.get());
            return r;
        }
        f.reset();
        it = cache.find(key);
    } else {
        r.cacheHit = true;
        r.bytes = it->second.bitcode.size();
    }
    auto loaded = loadBitcode(it->second.bitcode, r.loadUs);
    r.initUs = instanceInitUs(loaded.get());
    return r;
}

void printResult(const char* phase, int stages, const BuildResult& r, int moduleMisses)
{
    std::cout << "phase=" << phase
              << " stages=" << stages
              << " graph_cache_hit=" << (r.cacheHit ? 1 : 0)
              << " module_cache_misses=" << moduleMisses
              << " compile_us=" << r.compileUs
              << " bitcode_write_us=" << r.writeUs
              << " bitcode_load_us=" << r.loadUs
              << " instance_init_us=" << r.initUs
              << " bitcode_bytes=" << r.bytes
              << " total_prepare_us=" << (r.compileUs+r.writeUs+r.loadUs+r.initUs)
              << "\n";
}

void run(int stages)
{
    Cache moduleCache;
    Cache graphCache;

    // Existing Curlop idea: authored modules get source-hash bitcode artifacts.
    double moduleColdUs = 0;
    for (int i = 0; i < stages; ++i) {
        auto src = moduleSource(i);
        auto r = acquireGraph(moduleCache, "module-" + std::to_string(i), src, true);
        moduleColdUs += r.compileUs + r.writeUs + r.loadUs + r.initUs;
    }
    std::cout << "phase=module_cache_warm stages=" << stages
              << " module_cache_entries=" << moduleCache.size()
              << " aggregate_prepare_us=" << moduleColdUs << "\n";

    const auto a = fusedSource(stages);
    const auto b = fusedSource(stages, stages/2);
    const auto edited = fusedSource(stages, -1, stages/2);

    // First-time whole-graph build.
    auto ra = acquireGraph(graphCache, "graph-A", a, true);
    printResult("graph_A_cold", stages, ra, 0);

    // Same topology again: content-addressed whole-graph bitcode hit.
    auto raWarm = acquireGraph(graphCache, "graph-A", a, true);
    printResult("graph_A_cached", stages, raWarm, 0);

    // Cable disconnect / bypass: module sources are all cached, but topology is new.
    auto rb = acquireGraph(graphCache, "graph-B", b, true);
    printResult("wire_edit_new_topology", stages, rb, 0);

    // Undo/reconnect back to an already-seen topology.
    auto raReturn = acquireGraph(graphCache, "graph-A", a, true);
    printResult("wire_edit_return_cached", stages, raReturn, 0);

    // One user module source changes. Compile/cache just that authored module first.
    int misses = 0;
    {
        auto src = moduleSource(stages/2, true);
        const auto key = hash(src);
        if (moduleCache.find(key) == moduleCache.end()) ++misses;
        auto mr = acquireGraph(moduleCache, "edited-module", src, true);
        std::cout << "phase=edited_module_build stages=" << stages
                  << " module_cache_miss=" << misses
                  << " compile_us=" << mr.compileUs
                  << " bitcode_write_us=" << mr.writeUs
                  << " bitcode_load_us=" << mr.loadUs
                  << " instance_init_us=" << mr.initUs << "\n";
    }
    auto re = acquireGraph(graphCache, "graph-edited", edited, true);
    printResult("module_edit_fused_rebuild", stages, re, misses);
}

} // namespace

int main()
{
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "target=" << target() << " sample_rate=" << kSampleRate << "\n";
    for (int stages : {4, 8, 16, 32}) run(stages);
    return 0;
}
