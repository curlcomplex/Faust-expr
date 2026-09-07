#include <faust/dsp/llvm-dsp.h>
#include <faust/dsp/libfaust.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kFrames = 256;
constexpr int kWarmup = 300;
constexpr int kIterations = 3000;

struct FactoryDeleter {
    void operator()(llvm_dsp_factory* p) const noexcept { if (p) deleteDSPFactory(p); }
};
struct DSPDeleter {
    void operator()(dsp* p) const noexcept { delete p; }
};
using FactoryPtr = std::unique_ptr<llvm_dsp_factory, FactoryDeleter>;
using DSPPtr = std::unique_ptr<dsp, DSPDeleter>;

struct BuiltDSP {
    FactoryPtr factory;
    DSPPtr dsp;
    long long buildUs = 0;
};

std::string jitTarget()
{
    if (const char* e = std::getenv("FAUST_JIT_TARGET"); e && *e) return e;
    return "x86_64-pc-linux-gnu:generic";
}

BuiltDSP build(const std::string& name, const std::string& source,
               const std::vector<std::string>& options = {})
{
    std::vector<const char*> args;
    for (const auto& o : options) args.push_back(o.c_str());
    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    FactoryPtr factory(createDSPFactoryFromString(name, source,
        static_cast<int>(args.size()), args.empty() ? nullptr : args.data(),
        jitTarget(), error, -1));
    const auto t1 = std::chrono::steady_clock::now();
    if (!factory) {
        std::cerr << "factory_error name=" << name << " error=" << error << "\n";
        std::exit(3);
    }
    DSPPtr instance(factory->createDSPInstance());
    if (!instance) {
        std::cerr << "instance_error name=" << name << "\n";
        std::exit(4);
    }
    instance->init(kSampleRate);
    return {std::move(factory), std::move(instance),
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()};
}

std::string oscillatorBank(int voices, double gain = 0.01)
{
    std::ostringstream s;
    s << "import(\"stdfaust.lib\");\n"
      << "voice(i) = os.osc(70 + i * 5.731);\n"
      << "bank = par(i," << voices << ",voice(i)) :> _;\n"
      << "process = bank * " << std::setprecision(12) << gain << ";\n";
    return s.str();
}

std::string serialFusedSource(int voices, int stages)
{
    std::ostringstream s;
    s << "import(\"stdfaust.lib\");\n"
      << "voice(i) = os.osc(70 + i * 5.731);\n"
      << "bank = par(i," << voices << ",voice(i)) :> _;\n"
      << "process = bank";
    for (int i = 0; i < stages; ++i) {
        s << " : fi.lowpass(2," << (700 + i * 190) << ")";
    }
    s << " : *(0.01);\n";
    return s.str();
}

std::string filterSource(int cutoff, bool finalGain)
{
    std::ostringstream s;
    s << "import(\"stdfaust.lib\");\nprocess = fi.lowpass(2," << cutoff << ")";
    if (finalGain) s << " : *(0.01)";
    s << ";\n";
    return s.str();
}

std::string voiceSource(int index)
{
    std::ostringstream s;
    s << "import(\"stdfaust.lib\");\nprocess = os.osc(" << std::setprecision(12)
      << (70.0 + index * 5.731) << ") : fi.lowpass(2," << (900 + (index % 7) * 130)
      << ") : *(0.01);\n";
    return s.str();
}

std::string wideFusedSource(int voices)
{
    std::ostringstream s;
    s << "import(\"stdfaust.lib\");\n"
      << "voice(i) = os.osc(70 + i * 5.731) : fi.lowpass(2,900 + (i % 7) * 130) : *(0.01);\n"
      << "process = par(i," << voices << ",voice(i)) :> _;\n";
    return s.str();
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i) d = std::max(d, std::abs(double(a[i]) - double(b[i])));
    return d;
}

void renderOne(dsp* unit, float* in, float* out)
{
    FAUSTFLOAT* inputs[1] = {in};
    FAUSTFLOAT* outputs[1] = {out};
    unit->compute(kFrames, in ? inputs : nullptr, outputs);
}

void runSerial(int voices, int stages)
{
    auto fused = build("serial-fused", serialFusedSource(voices, stages));
    auto source = build("serial-source", oscillatorBank(voices, 1.0));
    std::vector<BuiltDSP> filters;
    long long separateBuildUs = source.buildUs;
    for (int i = 0; i < stages; ++i) {
        filters.push_back(build("filter-" + std::to_string(i), filterSource(700 + i * 190, i == stages - 1)));
        separateBuildUs += filters.back().buildUs;
    }

    std::vector<float> fusedOut(kFrames), a(kFrames), b(kFrames);
    auto fusedRender = [&]() { renderOne(fused.dsp.get(), nullptr, fusedOut.data()); };
    auto separateRender = [&]() {
        renderOne(source.dsp.get(), nullptr, a.data());
        float* src = a.data();
        float* dst = b.data();
        for (auto& f : filters) {
            renderOne(f.dsp.get(), src, dst);
            std::swap(src, dst);
        }
        if (src != a.data()) std::copy(src, src + kFrames, a.data());
    };

    // Reinitialize so the equivalence block begins at identical phase/state.
    fused.dsp->init(kSampleRate);
    source.dsp->init(kSampleRate);
    for (auto& f : filters) f.dsp->init(kSampleRate);
    fusedRender();
    separateRender();
    const double diff = maxDiff(fusedOut, a);

    for (int i = 0; i < kWarmup; ++i) fusedRender();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) fusedRender();
    auto t1 = std::chrono::steady_clock::now();
    const double fusedNs = std::chrono::duration<double, std::nano>(t1 - t0).count() / (kIterations * kFrames);

    for (int i = 0; i < kWarmup; ++i) separateRender();
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) separateRender();
    t1 = std::chrono::steady_clock::now();
    const double separateNs = std::chrono::duration<double, std::nano>(t1 - t0).count() / (kIterations * kFrames);

    std::cout << "serial voices=" << voices << " stages=" << stages
              << " fused_ns_per_frame=" << fusedNs
              << " separate_ns_per_frame=" << separateNs
              << " separate_over_fused=" << (separateNs / fusedNs)
              << " fused_build_us=" << fused.buildUs
              << " separate_build_us=" << separateBuildUs
              << " max_abs_diff=" << diff << "\n";
}

void runWide(int voices)
{
    auto fused = build("wide-fused", wideFusedSource(voices));
    std::vector<BuiltDSP> separate;
    long long separateBuildUs = 0;
    for (int i = 0; i < voices; ++i) {
        separate.push_back(build("voice-" + std::to_string(i), voiceSource(i)));
        separateBuildUs += separate.back().buildUs;
    }

    std::vector<float> fusedOut(kFrames), sum(kFrames), tmp(kFrames);
    auto fusedRender = [&]() { renderOne(fused.dsp.get(), nullptr, fusedOut.data()); };
    auto separateRender = [&]() {
        std::fill(sum.begin(), sum.end(), 0.0f);
        for (auto& v : separate) {
            renderOne(v.dsp.get(), nullptr, tmp.data());
            for (int i = 0; i < kFrames; ++i) sum[i] += tmp[i];
        }
    };

    fused.dsp->init(kSampleRate);
    for (auto& v : separate) v.dsp->init(kSampleRate);
    fusedRender();
    separateRender();
    const double diff = maxDiff(fusedOut, sum);

    for (int i = 0; i < kWarmup; ++i) fusedRender();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) fusedRender();
    auto t1 = std::chrono::steady_clock::now();
    const double fusedNs = std::chrono::duration<double, std::nano>(t1 - t0).count() / (kIterations * kFrames);

    for (int i = 0; i < kWarmup; ++i) separateRender();
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) separateRender();
    t1 = std::chrono::steady_clock::now();
    const double separateNs = std::chrono::duration<double, std::nano>(t1 - t0).count() / (kIterations * kFrames);

    std::cout << "wide voices=" << voices
              << " fused_ns_per_frame=" << fusedNs
              << " separate_ns_per_frame=" << separateNs
              << " separate_over_fused=" << (separateNs / fusedNs)
              << " fused_build_us=" << fused.buildUs
              << " separate_build_us=" << separateBuildUs
              << " max_abs_diff=" << diff << "\n";
}

} // namespace

int main()
{
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "target=" << jitTarget() << " frames=" << kFrames
              << " iterations=" << kIterations << "\n";
    runSerial(24, 2);
    runSerial(24, 4);
    runSerial(24, 8);
    runWide(4);
    runWide(8);
    runWide(16);
    return 0;
}
