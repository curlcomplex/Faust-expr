// Offline throughput benchmark for the existing Faust-expr scheduler research.
// NOT a realtime callback acceptance test. Loads an already-created bitcode factory.
#include <faust/dsp/llvm-dsp.h>
#include <faust/dsp/dsp.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary); std::ostringstream s; s << f.rdbuf(); return s.str();
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: scheduler_scaling_bench <bitcode> <participants> <frames> <blocks-per-trial>\n";
        return 2;
    }
    const std::string bcPath = argv[1];
    const int participants = std::atoi(argv[2]);
    const int frames = std::atoi(argv[3]);
    const int blocks = std::atoi(argv[4]);
    if (participants < 1 || participants > 64 || frames < 1 || frames > 4096 || blocks < 100) return 2;
    setenv("OMP_NUM_THREADS", std::to_string(participants).c_str(), 1);
    setenv("OMP_DYN_THREAD", "0", 1);

    std::string error;
    const std::string target = getDSPMachineTarget();
    auto* factory = readDSPFactoryFromBitcode(readFile(bcPath), target, error, -1);
    if (!factory) { std::cerr << "factory_error=" << error << "\n"; return 3; }
    std::unique_ptr<dsp> instance(factory->createDSPInstance());
    if (!instance) return 4;
    instance->init(48000);
    if (instance->getNumInputs() != 0 || instance->getNumOutputs() != 2) return 5;
    std::vector<FAUSTFLOAT> a(frames), b(frames); FAUSTFLOAT* outs[2] = {a.data(), b.data()};

    // Warm scheduler, JIT code, and caches before measuring.
    for (int i=0; i<1000; ++i) instance->compute(frames, nullptr, outs);

    std::vector<double> nsPerFrame;
    nsPerFrame.reserve(9);
    double checksum = 0.0;
    for (int t=0; t<9; ++t) {
        auto t0 = std::chrono::steady_clock::now();
        for (int i=0; i<blocks; ++i) instance->compute(frames, nullptr, outs);
        auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1-t0).count();
        nsPerFrame.push_back(ns / (double(blocks) * frames));
        for (int i=0; i<frames; ++i) checksum += double(a[i]) + double(b[i]);
    }
    if (!std::isfinite(checksum)) return 6;
    std::sort(nsPerFrame.begin(), nsPerFrame.end());
    const double median = nsPerFrame[4];
    const double best = nsPerFrame.front();
    const double worst = nsPerFrame.back();
    std::cout << std::setprecision(12)
              << "participants=" << participants << "\nframes=" << frames
              << "\nblocks_per_trial=" << blocks << "\ntrials=9"
              << "\nmedian_ns_per_frame=" << median
              << "\nbest_ns_per_frame=" << best
              << "\nworst_ns_per_frame=" << worst
              << "\nmedian_realtime_factor=" << (1e9 / 48000.0 / median)
              << "\nchecksum=" << checksum << "\n";
    instance.reset();
    if (!deleteDSPFactory(factory)) return 7;
    return 0;
}
