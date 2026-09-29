// Proof-of-concept coarse multicore executor: three persistent workers run
// three independent scalar Faust DSP instances in parallel. Offline throughput
// only; this is not a realtime/audio-workgroup acceptance test.
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

struct Worker {
    dsp* instance = nullptr;
    int frames = 0;
    std::vector<FAUSTFLOAT> a, b;
    FAUSTFLOAT* outs[2] = {nullptr, nullptr};
    std::atomic<uint64_t> requested{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<bool> stop{false};
    std::thread thread;

    Worker() = default;
    void setup(dsp* d, int n) {
        instance=d; frames=n; a.resize(n); b.resize(n); outs[0]=a.data(); outs[1]=b.data();
    }
    void start() {
        thread=std::thread([this]{
            uint64_t seen=0;
            for (;;) {
                if (stop.load(std::memory_order_acquire)) return;
                uint64_t r=requested.load(std::memory_order_acquire);
                if (r==seen) { std::this_thread::yield(); continue; }
                seen=r;
                instance->compute(frames, nullptr, outs);
                completed.store(seen, std::memory_order_release);
            }
        });
    }
    void shutdown() {
        stop.store(true, std::memory_order_release);
        requested.fetch_add(1, std::memory_order_release);
        if (thread.joinable()) thread.join();
    }
};

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: coarse_scalar_bench <region-bitcode> <frames> <blocks-per-trial>\n";
        return 2;
    }
    const int frames=std::atoi(argv[2]), blocks=std::atoi(argv[3]);
    if (frames < 1 || frames > 4096 || blocks < 100) return 2;
    std::string error;
    auto* factory=readDSPFactoryFromBitcode(readFile(argv[1]), getDSPMachineTarget(), error, -1);
    if (!factory) { std::cerr << "factory_error=" << error << "\n"; return 3; }
    std::unique_ptr<dsp> d0(factory->createDSPInstance()), d1(factory->createDSPInstance()), d2(factory->createDSPInstance());
    if (!d0 || !d1 || !d2) return 4;
    for (auto* d : {d0.get(),d1.get(),d2.get()}) {
        d->init(48000);
        if (d->getNumInputs()!=0 || d->getNumOutputs()!=2) return 5;
    }
    std::vector<FAUSTFLOAT> a0(frames), b0(frames); FAUSTFLOAT* out0[2]={a0.data(),b0.data()};
    Worker w1,w2; w1.setup(d1.get(),frames); w2.setup(d2.get(),frames); w1.start(); w2.start();
    uint64_t epoch=0;
    auto block=[&]{
        ++epoch;
        w1.requested.store(epoch,std::memory_order_release);
        w2.requested.store(epoch,std::memory_order_release);
        d0->compute(frames,nullptr,out0);
        while (w1.completed.load(std::memory_order_acquire)<epoch || w2.completed.load(std::memory_order_acquire)<epoch) {
            std::this_thread::yield();
        }
    };
    for (int i=0;i<1000;++i) block();
    std::vector<double> nsPerFrame; nsPerFrame.reserve(9); double checksum=0.0;
    for (int t=0;t<9;++t) {
        auto t0=std::chrono::steady_clock::now();
        for (int i=0;i<blocks;++i) block();
        auto t1=std::chrono::steady_clock::now();
        nsPerFrame.push_back(std::chrono::duration<double,std::nano>(t1-t0).count()/(double(blocks)*frames));
        for (int i=0;i<frames;++i) checksum += double(a0[i])+double(b0[i])+double(w1.a[i])+double(w1.b[i])+double(w2.a[i])+double(w2.b[i]);
    }
    w1.shutdown(); w2.shutdown();
    if (!std::isfinite(checksum)) return 6;
    std::sort(nsPerFrame.begin(),nsPerFrame.end());
    std::cout << std::setprecision(12)
              << "regions=3\nframes=" << frames << "\nblocks_per_trial=" << blocks << "\ntrials=9"
              << "\nmedian_ns_per_frame=" << nsPerFrame[4]
              << "\nbest_ns_per_frame=" << nsPerFrame.front()
              << "\nworst_ns_per_frame=" << nsPerFrame.back()
              << "\nchecksum=" << checksum << "\n";
    d0.reset(); d1.reset(); d2.reset();
    if (!deleteDSPFactory(factory)) return 7;
    return 0;
}
