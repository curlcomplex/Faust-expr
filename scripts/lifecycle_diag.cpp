// Compatibility/lifecycle diagnostic, NOT an audio performance benchmark.
// The DSP input is extracted from the existing scheduler_bitcode_probe.cpp.
#include <algorithm>
#include <cmath>
#include <cstdio>
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
#include <vector>
#include <execinfo.h>
#include <unistd.h>
#include <faust/dsp/dsp.h>
#include <faust/gui/UI.h>
#include <faust/gui/meta.h>

#ifdef DIAG_AOT
#include "diag_dsp.h"
#else
#include <faust/dsp/llvm-dsp.h>
#endif

namespace {
void traceTerminate() noexcept
{
    // Diagnostic-only: this handler is not used in the realtime product.
    std::fputs("diagnostic_terminate_handler\n", stderr);
    if (auto ep = std::current_exception()) {
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { std::fprintf(stderr, "exception=%s\n", e.what()); }
        catch (...) { std::fputs("exception=non_std\n", stderr); }
    }
    void* frames[96];
    const int n = backtrace(frames, 96);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    std::abort();
}
std::string readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::ostringstream s; s << f.rdbuf();
    if (f.bad()) throw std::runtime_error("failed reading " + path);
    return s.str();
}
void writeFile(const std::string& path, const std::string& data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.close();
    if (!f) throw std::runtime_error("failed writing " + path);
}
std::string env(const char* key)
{
    const char* p = std::getenv(key);
    return p ? p : "";
}
void render(dsp* instance, const std::string& outputPath)
{
    if (instance->getNumInputs() != 0 || instance->getNumOutputs() != 2)
        throw std::runtime_error("unexpected DSP channel contract");
    std::cout << "stage=instance_init_begin\n";
    instance->init(48000);
    std::cout << "stage=instance_init_end\n";
    constexpr int frames = 256, blocks = 200;
    std::vector<FAUSTFLOAT> left(frames), right(frames);
    FAUSTFLOAT* outputs[2] = {left.data(), right.data()};
    std::vector<float> capture;
    capture.reserve(frames * blocks * 2);
    double checksum = 0, energy = 0, peak = 0;
    std::cout << "stage=compute_begin\n";
    for (int b = 0; b < blocks; ++b) {
        instance->compute(frames, nullptr, outputs);
        for (int i = 0; i < frames; ++i) {
            for (auto v : {left[i], right[i]}) {
                if (!std::isfinite(v)) throw std::runtime_error("nonfinite output");
                capture.push_back(static_cast<float>(v));
                checksum += v;
                energy += double(v) * v;
                peak = std::max(peak, std::abs(double(v)));
            }
        }
    }
    std::cout << "stage=compute_end\n";
    if (!(energy > 1e-12)) throw std::runtime_error("silent output cannot validate DSP");
    writeFile(outputPath, std::string(reinterpret_cast<const char*>(capture.data()),
                                    capture.size() * sizeof(float)));
    std::cout << std::setprecision(17)
              << "checksum=" << checksum << "\nenergy=" << energy
              << "\npeak=" << peak << "\nsamples=" << capture.size() << '\n';
}
}

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::set_terminate(traceTerminate);
    // Timeout/backtrace capture belongs to the parent. No success via _exit().
    // args: scalar|sch source|read explicit|retain source.dsp cache.bc.txt audio.f32
    if (argc != 7) {
        std::cerr << "usage: lifecycle_diag scalar|sch source|read explicit|retain source.dsp cache.bc.txt audio.f32\n";
        return 2;
    }
    const std::string shape = argv[1], mode = argv[2], cleanup = argv[3];
    const std::string sourcePath = argv[4], cachePath = argv[5], outputPath = argv[6];
    if ((shape != "scalar" && shape != "sch") || (mode != "source" && mode != "read") ||
        (cleanup != "explicit" && cleanup != "retain")) return 2;
    std::cout << "pid=" << getpid() << "\nshape=" << shape << "\nmode=" << mode
              << "\ncleanup=" << cleanup << "\nOMP_NUM_THREADS=" << env("OMP_NUM_THREADS") << '\n';
    try {
#ifdef DIAG_AOT
        if (mode != "source" || cleanup != "explicit") return 2;
        std::cout << "backend=cpp_aot\nstage=instance_create_begin\n";
        std::unique_ptr<dsp> instance(new ProbeDSP());
        std::cout << "stage=instance_create_end\n";
        render(instance.get(), outputPath);
        std::cout << "stage=instance_destroy_begin\n";
        instance.reset();
        std::cout << "stage=instance_destroy_end\nfactory_cleanup=not_applicable\n";
#else
        std::cout << "backend=llvm_jit\n";
        const std::string target = env("FAUST_JIT_TARGET").empty()
                                 ? getDSPMachineTarget() : env("FAUST_JIT_TARGET");
        std::string error;
        llvm_dsp_factory* factory = nullptr;
        std::cout << "target=" << target << "\nstage=factory_create_begin\n";
        if (mode == "source") {
            std::vector<std::string> opts;
            if (shape == "sch") {
                const auto module = env("FAUST_SCHEDULER_MODULE");
                if (module.empty()) throw std::runtime_error("missing scheduler IR path");
                opts = {"-sch", "-L", module};
            }
            std::vector<const char*> args;
            for (const auto& s : opts) args.push_back(s.c_str());
            factory = createDSPFactoryFromString("LifecycleDiagnostic", readFile(sourcePath),
                       int(args.size()), args.data(), target, error, -1);
        } else {
            // The reader deliberately does not receive -L or a scheduler path.
            if (!env("FAUST_SCHEDULER_MODULE").empty())
                throw std::runtime_error("reader unexpectedly received scheduler module");
            factory = readDSPFactoryFromBitcode(readFile(cachePath), target, error, -1);
        }
        if (!factory) throw std::runtime_error("factory error: " + error);
        std::cout << "stage=factory_create_end\noptions=" << factory->getCompileOptions() << '\n';
        if (mode == "source") {
            std::cout << "stage=bitcode_write_begin\n";
            const auto bitcode = writeDSPFactoryToBitcode(factory);
            if (bitcode.empty()) throw std::runtime_error("empty bitcode");
            writeFile(cachePath, bitcode);
            std::cout << "stage=bitcode_write_end\nbitcode_string_bytes=" << bitcode.size() << '\n';
        }
        std::cout << "stage=instance_create_begin\n";
        std::unique_ptr<dsp> instance(factory->createDSPInstance());
        if (!instance) throw std::runtime_error("null instance");
        std::cout << "stage=instance_create_end\n";
        render(instance.get(), outputPath);
        std::cout << "stage=instance_destroy_begin\n";
        instance.reset();
        std::cout << "stage=instance_destroy_end\n";
        if (cleanup == "explicit") {
            std::cout << "stage=factory_destroy_begin\n";
            const bool removed = deleteDSPFactory(factory);
            std::cout << "stage=factory_destroy_end\nfactory_removed=" << removed << '\n';
            if (!removed) throw std::runtime_error("factory still retained after explicit delete");
        } else {
            // Deliberate negative control reproducing the old probe. Not a
            // proposed cache policy and not a production acceptance condition.
            std::cout << "factory_intentionally_retained=1\n";
        }
#endif
        std::cout << "stage=main_return\n";
        return 0; // Only the parent may declare PASS after observing clean exit.
    } catch (const std::exception& e) {
        std::cerr << "diagnostic_error=" << e.what() << '\n';
        void* frames[64];
        backtrace_symbols_fd(frames, backtrace(frames, 64), STDERR_FILENO);
        return 20;
    }
}
