// Offline throughput proof-of-concept for coarse scalar Faust regions.
// NOT a realtime callback acceptance test.
#include <faust/dsp/llvm-dsp.h>
#include <faust/dsp/dsp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary); std::ostringstream s; s << f.rdbuf(); return s.str();
}

struct Region {
    llvm_dsp_factory* factory = nullptr;
    std::unique_ptr<dsp> instance;
    std::vector<FAUSTFLOAT> a, b;
    FAUSTFLOAT* outs[2] = {nullptr, nullptr};
};

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "usage: coarse_region_bench <frames> <blocks-per-trial> <regions> <bc1> ... <bcN>\n";
        return 2;
    }
    const int frames = std::atoi(argv[1]);
    const int blocks = std::atoi(argv[2]);
    const int n = std::atoi(argv[3]);
    if (frames < 1 || frames > 4096 || blocks < 100 || n < 1 || n > 8 || argc != 4 + n) return 2;

    std::vector<Region> regions(n);
    const std::string target = getDSPMachineTarget();
    for (int i=0; i<n; ++i) {
        std::string error;
        regions[i].factory = readDSPFactoryFromBitcode(readFile(argv[4+i]), target, error, -1);
        if (!regions[i].factory) { std::cerr << "factory_error=" << error << "\n"; return 3; }
        regions[i].instance.reset(regions[i].factory->createDSPInstance());
        if (!regions[i].instance || regions[i].instance->getNumInputs()!=0 || regions[i].instance->getNumOutputs()!=2) return 4;
        regions[i].instance->init(48000);
        regions[i].a.resize(frames); regions[i].b.resize(frames);
        regions[i].outs[0] = regions[i].a.data(); regions[i].outs[1] = regions[i].b.data();
    }
    std::vector<FAUSTFLOAT> mixA(frames), mixB(frames);

    std::atomic<uint64_t> epoch{0};
    std::atomic<int> done{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int r=1; r<n; ++r) {
        workers.emplace_back([&, r] {
            uint64_t seen = 0;
            while (!stop.load(std::memory_order_acquire)) {
                uint64_t e;
                while ((e = epoch.load(std::memory_order_acquire)) == seen) {
                    if (stop.load(std::memory_order_acquire)) return;
                    std::this_thread::yield();
                }
                seen = e;
                regions[r].instance->compute(frames, nullptr, regions[r].outs);
                done.fetch_add(1, std::memory_order_release);
            }
        });
    }

    auto renderBlock = [&] {
        done.store(0, std::memory_order_relaxed);
        if (n > 1) epoch.fetch_add(1, std::memory_order_release);
        regions[0].instance->compute(frames, nullptr, regions[0].outs);
        while (done.load(std::memory_order_acquire) != n-1) std::this_thread::yield();
        for (int i=0; i<frames; ++i) {
            double a = 0.0, b = 0.0;
            for (int r=0; r<n; ++r) { a += regions[r].a[i]; b += regions[r].b[i]; }
            mixA[i] = FAUSTFLOAT(a); mixB[i] = FAUSTFLOAT(b);
        }
    };

    for (int i=0; i<1000; ++i) renderBlock();
    std::vector<double> nsPerFrame; nsPerFrame.reserve(9);
    double checksum=0.0;
    for (int t=0; t<9; ++t) {
        auto t0=std::chrono::steady_clock::now();
        for (int i=0; i<blocks; ++i) renderBlock();
        auto t1=std::chrono::steady_clock::now();
        nsPerFrame.push_back(std::chrono::duration<double,std::nano>(t1-t0).count() / (double(blocks)*frames));
        for (int i=0;i<frames;++i) checksum += double(mixA[i])+double(mixB[i]);
    }
    stop.store(true, std::memory_order_release);
    epoch.fetch_add(1, std::memory_order_release);
    for (auto& w:workers) w.join();
    if (!std::isfinite(checksum)) return 5;
    std::sort(nsPerFrame.begin(),nsPerFrame.end());
    std::cout<<std::setprecision(12)
             <<"regions="<<n<<"\nframes="<<frames<<"\nblocks_per_trial="<<blocks
             <<"\nmedian_ns_per_frame="<<nsPerFrame[4]
             <<"\nbest_ns_per_frame="<<nsPerFrame.front()
             <<"\nworst_ns_per_frame="<<nsPerFrame.back()
             <<"\nmedian_realtime_factor="<<(1e9/48000.0/nsPerFrame[4])
             <<"\nchecksum="<<checksum<<"\n";
    for (auto& r:regions) {
        r.instance.reset();
        if (!deleteDSPFactory(r.factory)) return 6;
    }
    return 0;
}
