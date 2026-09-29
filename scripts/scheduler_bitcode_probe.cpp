#include <faust/dsp/llvm-dsp.h>
#include <faust/dsp/libfaust.h>

#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
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

struct DSPDeleter {
    void operator()(dsp* p) const noexcept { delete p; }
};
using DSPPtr = std::unique_ptr<dsp, DSPDeleter>;

std::string target()
{
    if (const char* e = std::getenv("FAUST_JIT_TARGET"); e && *e) return e;
    return getDSPMachineTarget();
}

void timedOut(int)
{
    const char msg[] = "watchdog_timeout=30s\n";
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(124);
}

double renderChecksum(llvm_dsp_factory* factory, const char* stage)
{
    std::cout << "stage=" << stage << "_create_instance\n";
    DSPPtr instance(factory->createDSPInstance());
    if (!instance) {
        std::cerr << "instance_error=null\n";
        std::exit(5);
    }
    std::cout << "stage=" << stage << "_init\n";
    instance->init(48000);
    constexpr int frames = 256;
    std::vector<FAUSTFLOAT> left(frames), right(frames);
    FAUSTFLOAT* outputs[2] = { left.data(), right.data() };
    std::cout << "stage=" << stage << "_compute\n";
    for (int i = 0; i < 200; ++i) instance->compute(frames, nullptr, outputs);
    double checksum = 0.0;
    for (int i = 0; i < frames; ++i) checksum += left[i] + right[i];
    std::cout << "stage=" << stage << "_destroy_instance\n";
    return checksum;
}

bool writeText(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return bool(out);
}

std::string readText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

} // namespace

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::signal(SIGALRM, timedOut);
    alarm(30);

    if (argc != 3 || (std::string(argv[1]) != "write" && std::string(argv[1]) != "read")) {
        std::cerr << "usage: scheduler_bitcode_probe write|read <bitcode-file>\n";
        return 2;
    }

    const std::string mode = argv[1];
    const std::string path = argv[2];
    const std::string jitTarget = target();
    std::cout << "mode=" << mode << "\n";
    std::cout << "target=" << jitTarget << "\n";

    std::string error;

    if (mode == "write") {
        const char* schedulerModule = std::getenv("FAUST_SCHEDULER_MODULE");
        if (!schedulerModule || !*schedulerModule) {
            std::cerr << "FAUST_SCHEDULER_MODULE is required in write mode\n";
            return 3;
        }
        std::cout << "scheduler_module=" << schedulerModule << "\n";

        std::vector<std::string> storage { "-sch", "-L", schedulerModule };
        std::vector<const char*> args;
        for (const auto& s : storage) args.push_back(s.c_str());

        std::cout << "stage=create_source_factory\n";
        llvm_dsp_factory* factory = createDSPFactoryFromString(
            "SchedulerBitcodeProbe", kSource,
            static_cast<int>(args.size()), args.data(),
            jitTarget, error, -1);
        if (!factory) {
            std::cerr << "compile_error=" << error << "\n";
            return 4;
        }
        std::cout << "original_options=" << factory->getCompileOptions() << "\n";

        std::cout << "stage=write_bitcode_before_instance\n";
        const std::string bitcode = writeDSPFactoryToBitcode(factory);
        std::cout << "stage=write_bitcode_returned\n";
        if (bitcode.empty() || !writeText(path, bitcode)) {
            std::cerr << "bitcode_write_error\n";
            return 6;
        }
        std::cout << "bitcode_bytes=" << bitcode.size() << "\n";

        const double checksum = renderChecksum(factory, "source");
        std::cout << "checksum=" << checksum << "\n";
        if (!std::isfinite(checksum)) return 7;

        // Intentionally do not call deleteDSPFactory here. Curlop's process-wide
        // FaustRuntime cache also keeps hot factories alive for process lifetime,
        // and current libfaust/scheduler teardown crashes after scheduler use.
        // The OS reclaims the factory when this producer process exits.
        alarm(0);
        std::cout << "scheduler_bitcode_write=PASS\n";
        return 0;
    }

    // Fresh-process cache reload. There is deliberately no -L argument and no
    // requirement for FAUST_SCHEDULER_MODULE in this process.
    const std::string bitcode = readText(path);
    if (bitcode.empty()) {
        std::cerr << "bitcode_read_file_error\n";
        return 8;
    }
    std::cout << "bitcode_bytes=" << bitcode.size() << "\n";
    std::cout << "stage=read_bitcode_factory\n";
    llvm_dsp_factory* factory = readDSPFactoryFromBitcode(bitcode, jitTarget, error, -1);
    if (!factory) {
        std::cerr << "bitcode_reload_error=" << error << "\n";
        return 9;
    }
    std::cout << "stage=read_bitcode_returned\n";
    std::cout << "reloaded_options=" << factory->getCompileOptions() << "\n";

    const double checksum = renderChecksum(factory, "reloaded");
    std::cout << "checksum=" << checksum << "\n";
    if (!std::isfinite(checksum)) return 10;

    // Same process-lifetime cache policy as write mode.
    alarm(0);
    std::cout << "scheduler_bitcode_reload=PASS\n";
    return 0;
}
