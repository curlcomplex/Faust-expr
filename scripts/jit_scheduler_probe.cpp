#include <faust/dsp/llvm-dsp.h>
#include <faust/dsp/libfaust.h>

#include <algorithm>
#include <chrono>
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
    void operator()(llvm_dsp_factory* p) const noexcept
    {
        if (p) deleteDSPFactory(p);
    }
};

struct DSPDeleter {
    void operator()(dsp* p) const noexcept { delete p; }
};

std::vector<std::string> argsFor(const std::string& mode)
{
    if (mode == "scalar") return {};
    if (mode == "vec32") return {"-vec", "-vs", "32"};
    if (mode == "vec64") return {"-vec", "-vs", "64"};
    if (mode == "sch") return {"-sch"};
    std::cerr << "unknown mode: " << mode << "\n";
    std::exit(2);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: jit_scheduler_probe scalar|vec32|vec64|sch\n";
        return 2;
    }
    const std::string mode = argv[1];
    auto storage = argsFor(mode);
    std::vector<const char*> faustArgs;
    for (const auto& arg : storage) faustArgs.push_back(arg.c_str());

    std::string error;
    const auto buildStart = std::chrono::steady_clock::now();
    std::unique_ptr<llvm_dsp_factory, FactoryDeleter> factory(
        createDSPFactoryFromString("SchedulerProbe", kSource,
                                   static_cast<int>(faustArgs.size()),
                                   faustArgs.empty() ? nullptr : faustArgs.data(),
                                   "", error, -1));
    const auto buildEnd = std::chrono::steady_clock::now();
    if (!factory) {
        std::cerr << "factory_error=" << error << "\n";
        return 3;
    }

    const std::string ir = writeDSPFactoryToIR(factory.get());
    std::cout << "mode=" << mode << "\n";
    std::cout << "compile_options=" << factory->getCompileOptions() << "\n";
    std::cout << "ir_bytes=" << ir.size() << "\n";
    std::cout << "ir_has_createScheduler=" << (ir.find("createScheduler") != std::string::npos) << "\n";
    std::cout << "ir_has_computeThreadExternal=" << (ir.find("computeThreadExternal") != std::string::npos) << "\n";
    std::cout << "build_us=" << std::chrono::duration_cast<std::chrono::microseconds>(buildEnd - buildStart).count() << "\n";

    std::unique_ptr<dsp, DSPDeleter> instance(factory->createDSPInstance());
    if (!instance) {
        std::cerr << "instance_error=null\n";
        return 4;
    }
    instance->init(48000);

    constexpr int frames = 256;
    constexpr int warmup = 1000;
    constexpr int iterations = 20000;
    std::vector<FAUSTFLOAT> left(frames), right(frames);
    FAUSTFLOAT* outputs[2] = {left.data(), right.data()};

    for (int i = 0; i < warmup; ++i) instance->compute(frames, nullptr, outputs);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) instance->compute(frames, nullptr, outputs);
    const auto end = std::chrono::steady_clock::now();

    volatile double checksum = 0.0;
    for (int i = 0; i < frames; ++i) checksum += left[i] + right[i];
    const double seconds = std::chrono::duration<double>(end - start).count();
    const double nsPerFrame = seconds * 1e9 / (static_cast<double>(frames) * iterations);
    std::cout << "bench_seconds=" << seconds << "\n";
    std::cout << "ns_per_frame=" << nsPerFrame << "\n";
    std::cout << "realtime_x=" << ((static_cast<double>(frames) * iterations / 48000.0) / seconds) << "\n";
    std::cout << "checksum=" << checksum << "\n";
    return std::isfinite(checksum) ? 0 : 5;
}
