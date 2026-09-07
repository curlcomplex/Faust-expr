#include <faust/dsp/llvm-dsp.h>
#include <faust/dsp/libfaust.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

const char* kSource = R"FAUST(
import("stdfaust.lib");
voice(i) = os.osc(70 + i * 3.17)
         : fi.lowpass(2, 1200 + i * 41)
         : *(0.01);
bank = par(i, 64, voice(i)) :> _;
process = bank, bank;
)FAUST";

struct FactoryDeleter {
    void operator()(llvm_dsp_factory* p) const noexcept { if (p) deleteDSPFactory(p); }
};
using FactoryPtr = std::unique_ptr<llvm_dsp_factory, FactoryDeleter>;

struct DSPDeleter {
    void operator()(dsp* p) const noexcept { delete p; }
};
using DSPPtr = std::unique_ptr<dsp, DSPDeleter>;

std::string target()
{
    if (const char* e = std::getenv("FAUST_JIT_TARGET"); e && *e) return e;
    return getDSPMachineTarget();
}

double renderChecksum(llvm_dsp_factory* factory)
{
    DSPPtr instance(factory->createDSPInstance());
    if (!instance) {
        std::cerr << "instance_error=null\n";
        std::exit(5);
    }
    instance->init(48000);
    constexpr int frames = 256;
    std::vector<FAUSTFLOAT> left(frames), right(frames);
    FAUSTFLOAT* outputs[2] = { left.data(), right.data() };
    for (int i = 0; i < 200; ++i) instance->compute(frames, nullptr, outputs);
    double checksum = 0.0;
    for (int i = 0; i < frames; ++i) checksum += left[i] + right[i];
    return checksum;
}

} // namespace

int main()
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    const char* schedulerModule = std::getenv("FAUST_SCHEDULER_MODULE");
    if (!schedulerModule || !*schedulerModule) {
        std::cerr << "FAUST_SCHEDULER_MODULE is required\n";
        return 2;
    }

    const std::string jitTarget = target();
    std::cout << "target=" << jitTarget << "\n";
    std::cout << "scheduler_module=" << schedulerModule << "\n";

    std::vector<std::string> storage { "-sch", "-L", schedulerModule };
    std::vector<const char*> args;
    for (const auto& s : storage) args.push_back(s.c_str());

    std::string error;
    FactoryPtr original(createDSPFactoryFromString(
        "SchedulerBitcodeProbe", kSource,
        static_cast<int>(args.size()), args.data(),
        jitTarget, error, -1));
    if (!original) {
        std::cerr << "compile_error=" << error << "\n";
        return 3;
    }

    std::cout << "original_options=" << original->getCompileOptions() << "\n";
    const double before = renderChecksum(original.get());
    std::cout << "checksum_before=" << before << "\n";

    const std::string bitcode = writeDSPFactoryToBitcode(original.get());
    if (bitcode.empty()) {
        std::cerr << "bitcode_write_error=empty\n";
        return 4;
    }
    std::cout << "bitcode_bytes=" << bitcode.size() << "\n";

    // This is the compatibility gate: destroy the scheduler-enabled source
    // factory, then reconstruct from only its serialized factory bitcode.
    // readDSPFactoryFromBitcode has no -L argument, so a successful reload
    // proves that the scheduler linkage needed by this factory survived the
    // persisted build artifact rather than depending on the source compile call.
    original.reset();

    error.clear();
    FactoryPtr reloaded(readDSPFactoryFromBitcode(bitcode, jitTarget, error, -1));
    if (!reloaded) {
        std::cerr << "bitcode_reload_error=" << error << "\n";
        return 6;
    }
    std::cout << "reloaded_options=" << reloaded->getCompileOptions() << "\n";

    const double after = renderChecksum(reloaded.get());
    std::cout << "checksum_after=" << after << "\n";
    const double delta = std::abs(before - after);
    std::cout << "checksum_delta=" << delta << "\n";
    if (!std::isfinite(after) || delta > 1.0e-5) return 7;

    std::cout << "scheduler_bitcode_roundtrip=PASS\n";
    return 0;
}
