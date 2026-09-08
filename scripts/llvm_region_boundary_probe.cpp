#include <faust/dsp/llvm-dsp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kFrames = 8192;
constexpr double kRegionVsWholeMaxAbsGate = 3.0e-5;
constexpr double kRegionVsWholeRmsGate = 5.0e-6;
constexpr double kReloadMaxAbsGate = 1.0e-7;

struct FactoryDeleter {
    void operator()(llvm_dsp_factory* p) const noexcept { if (p) deleteDSPFactory(p); }
};
using FactoryPtr = std::unique_ptr<llvm_dsp_factory, FactoryDeleter>;

struct Metrics {
    double maxAbs = 0.0;
    double rms = 0.0;
    size_t maxIndex = 0;
};

std::string target()
{
    if (const char* env = std::getenv("FAUST_JIT_TARGET"); env && *env) return env;
    return getDSPMachineTarget();
}

FactoryPtr compile(const std::string& name, const std::string& source)
{
    std::string error;
    auto* raw = createDSPFactoryFromString(name, source, 0, nullptr, target(), error, -1);
    if (!raw) {
        std::cerr << "compile_error name=" << name << " error=" << error << "\n";
        std::exit(2);
    }
    return FactoryPtr(raw);
}

std::string bitcode(llvm_dsp_factory* factory)
{
    auto bc = writeDSPFactoryToBitcode(factory);
    if (bc.empty()) {
        std::cerr << "empty_bitcode\n";
        std::exit(3);
    }
    return bc;
}

FactoryPtr reload(const std::string& bc)
{
    std::string error;
    auto* raw = readDSPFactoryFromBitcode(bc, target(), error, -1);
    if (!raw) {
        std::cerr << "reload_error " << error << "\n";
        std::exit(4);
    }
    return FactoryPtr(raw);
}

std::unique_ptr<dsp> instance(llvm_dsp_factory* factory)
{
    std::unique_ptr<dsp> result(factory->createDSPInstance());
    if (!result) {
        std::cerr << "instance_error\n";
        std::exit(5);
    }
    result->init(kSampleRate);
    return result;
}

std::vector<FAUSTFLOAT> stimulus()
{
    std::vector<FAUSTFLOAT> x(kFrames, FAUSTFLOAT(0));
    // Deterministic, broadband-ish input with impulses and low-level tones. The
    // deliberately non-block-aligned impulses make boundary mistakes visible.
    for (int i = 0; i < kFrames; ++i) {
        const double t = double(i) / kSampleRate;
        double v = 0.035 * std::sin(2.0 * M_PI * 173.0 * t)
                 + 0.019 * std::sin(2.0 * M_PI * 997.0 * t);
        if (i == 0) v += 0.8;
        if (i == 1537) v -= 0.47;
        if (i == 4099) v += 0.31;
        x[i] = FAUSTFLOAT(v);
    }
    return x;
}

std::vector<int> fixedBlocks(int n, int block)
{
    std::vector<int> result;
    while (n > 0) {
        const int take = std::min(n, block);
        result.push_back(take);
        n -= take;
    }
    return result;
}

std::vector<int> irregularBlocks(int n)
{
    static const int pattern[] = {1, 7, 63, 2, 129, 17, 511, 3, 64, 5, 257, 31, 128, 9, 513, 4};
    std::vector<int> result;
    size_t i = 0;
    while (n > 0) {
        const int take = std::min(n, pattern[i++ % (sizeof(pattern) / sizeof(pattern[0]))]);
        result.push_back(take);
        n -= take;
    }
    return result;
}

std::vector<FAUSTFLOAT> runWhole(llvm_dsp_factory* factory,
                                 const std::vector<FAUSTFLOAT>& input,
                                 const std::vector<int>& blocks)
{
    auto d = instance(factory);
    std::vector<FAUSTFLOAT> output(input.size(), FAUSTFLOAT(0));
    size_t offset = 0;
    for (int n : blocks) {
        FAUSTFLOAT* in[] = {const_cast<FAUSTFLOAT*>(input.data() + offset)};
        FAUSTFLOAT* out[] = {output.data() + offset};
        d->compute(n, in, out);
        offset += size_t(n);
    }
    if (offset != input.size()) std::exit(6);
    return output;
}

std::vector<FAUSTFLOAT> runRegions(llvm_dsp_factory* aFactory,
                                   llvm_dsp_factory* bFactory,
                                   const std::vector<FAUSTFLOAT>& input,
                                   const std::vector<int>& blocks)
{
    // Deliberately one worker: A then host edge materialization then B, all on
    // this calling thread. The delayed edge has explicit host state (`carry`)
    // that persists across arbitrary compute block boundaries.
    auto a = instance(aFactory);
    auto b = instance(bFactory);
    std::vector<FAUSTFLOAT> output(input.size(), FAUSTFLOAT(0));
    std::vector<FAUSTFLOAT> aCurrent;
    std::vector<FAUSTFLOAT> aDelayed;
    FAUSTFLOAT carry = FAUSTFLOAT(0);
    size_t offset = 0;
    for (int n : blocks) {
        aCurrent.assign(size_t(n), FAUSTFLOAT(0));
        aDelayed.assign(size_t(n), FAUSTFLOAT(0));
        FAUSTFLOAT* aIn[] = {const_cast<FAUSTFLOAT*>(input.data() + offset)};
        FAUSTFLOAT* aOut[] = {aCurrent.data()};
        a->compute(n, aIn, aOut);

        for (int i = 0; i < n; ++i) {
            aDelayed[size_t(i)] = carry;
            carry = aCurrent[size_t(i)];
        }

        FAUSTFLOAT* bIn[] = {aCurrent.data(), aDelayed.data()};
        FAUSTFLOAT* bOut[] = {output.data() + offset};
        b->compute(n, bIn, bOut);
        offset += size_t(n);
    }
    if (offset != input.size()) std::exit(7);
    return output;
}

Metrics compare(const std::vector<FAUSTFLOAT>& a, const std::vector<FAUSTFLOAT>& b)
{
    if (a.size() != b.size()) std::exit(8);
    long double sumSq = 0.0L;
    Metrics m;
    for (size_t i = 0; i < a.size(); ++i) {
        const double av = double(a[i]);
        const double bv = double(b[i]);
        if (!std::isfinite(av) || !std::isfinite(bv)) {
            std::cerr << "nonfinite index=" << i << "\n";
            std::exit(9);
        }
        const double d = std::abs(av - bv);
        if (d > m.maxAbs) { m.maxAbs = d; m.maxIndex = i; }
        sumSq += static_cast<long double>(d) * static_cast<long double>(d);
    }
    m.rms = std::sqrt(double(sumSq / std::max<size_t>(1, a.size())));
    return m;
}

void report(const char* label, const Metrics& m)
{
    std::cout << label
              << " max_abs=" << m.maxAbs
              << " rms=" << m.rms
              << " max_index=" << m.maxIndex << "\n";
}

bool within(const Metrics& m, double maxAbs, double rms)
{
    return m.maxAbs <= maxAbs && m.rms <= rms;
}

} // namespace

int main()
{
    std::cout << std::scientific << std::setprecision(10);
    std::cout << "target=" << target() << " sample_rate=" << kSampleRate
              << " frames=" << kFrames << "\n";
    std::cout << "gate_region_vs_whole_max_abs=" << kRegionVsWholeMaxAbsGate
              << " gate_region_vs_whole_rms=" << kRegionVsWholeRmsGate
              << " gate_reload_max_abs=" << kReloadMaxAbsGate << "\n";

    const std::string sourceA = R"FAUST(import("stdfaust.lib");
process = fi.lowpass(2, 1200.0);
)FAUST";
    const std::string sourceB = R"FAUST(import("stdfaust.lib");
process = + : fi.lowpass(1, 3400.0);
)FAUST";
    // The reference is deliberately the same decomposition expressed as one
    // Faust program. `mem` is a one-sample delay of A's current output.
    const std::string sourceWhole = R"FAUST(import("stdfaust.lib");
a = fi.lowpass(2, 1200.0);
process = _ : a <: _, mem : + : fi.lowpass(1, 3400.0);
)FAUST";

    auto whole = compile("whole", sourceWhole);
    auto regionA = compile("region-a", sourceA);
    auto regionB = compile("region-b", sourceB);

    const std::string wholeBC = bitcode(whole.get());
    const std::string aBC = bitcode(regionA.get());
    const std::string bBC = bitcode(regionB.get());
    std::cout << "bitcode_bytes whole=" << wholeBC.size()
              << " region_a=" << aBC.size() << " region_b=" << bBC.size() << "\n";

    const auto x = stimulus();
    const auto fixed = fixedBlocks(kFrames, 128);
    const auto irregular = irregularBlocks(kFrames);

    const auto wholeFixed = runWhole(whole.get(), x, fixed);
    const auto wholeIrregular = runWhole(whole.get(), x, irregular);
    const auto regionFixed = runRegions(regionA.get(), regionB.get(), x, fixed);
    const auto regionIrregular = runRegions(regionA.get(), regionB.get(), x, irregular);

    const Metrics wholeBlockInvariant = compare(wholeFixed, wholeIrregular);
    const Metrics regionBlockInvariant = compare(regionFixed, regionIrregular);
    const Metrics regionVsWholeFixed = compare(wholeFixed, regionFixed);
    const Metrics regionVsWholeIrregular = compare(wholeIrregular, regionIrregular);
    report("whole_block_invariance", wholeBlockInvariant);
    report("region_block_invariance", regionBlockInvariant);
    report("region_vs_whole_fixed", regionVsWholeFixed);
    report("region_vs_whole_irregular", regionVsWholeIrregular);

    // Retire every source-created factory before loading the bitcode. This tests
    // fresh factory reconstruction, not instance-state serialization. Runtime
    // state is then exercised continuously across the irregular compute calls.
    whole.reset();
    regionA.reset();
    regionB.reset();

    auto wholeReloaded = reload(wholeBC);
    auto aReloaded = reload(aBC);
    auto bReloaded = reload(bBC);
    const auto wholeReload = runWhole(wholeReloaded.get(), x, irregular);
    const auto regionReload = runRegions(aReloaded.get(), bReloaded.get(), x, irregular);

    const Metrics wholeReloadMatch = compare(wholeIrregular, wholeReload);
    const Metrics regionReloadMatch = compare(regionIrregular, regionReload);
    const Metrics reloadRegionVsWhole = compare(wholeReload, regionReload);
    report("whole_fresh_bitcode_reload", wholeReloadMatch);
    report("region_fresh_bitcode_reload", regionReloadMatch);
    report("reload_region_vs_whole", reloadRegionVsWhole);

    bool ok = true;
    ok = ok && within(wholeBlockInvariant, kReloadMaxAbsGate, kReloadMaxAbsGate);
    ok = ok && within(regionBlockInvariant, kReloadMaxAbsGate, kReloadMaxAbsGate);
    ok = ok && within(regionVsWholeFixed, kRegionVsWholeMaxAbsGate, kRegionVsWholeRmsGate);
    ok = ok && within(regionVsWholeIrregular, kRegionVsWholeMaxAbsGate, kRegionVsWholeRmsGate);
    ok = ok && within(wholeReloadMatch, kReloadMaxAbsGate, kReloadMaxAbsGate);
    ok = ok && within(regionReloadMatch, kReloadMaxAbsGate, kReloadMaxAbsGate);
    ok = ok && within(reloadRegionVsWhole, kRegionVsWholeMaxAbsGate, kRegionVsWholeRmsGate);

    std::cout << "cross_region_delayed_edge=explicit_host_carry\n";
    std::cout << "execution_workers=1 execution_order=A_then_B\n";
    std::cout << "instance_state_serialization_tested=0\n";
    std::cout << "checkpoint_gate=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 10;
}
