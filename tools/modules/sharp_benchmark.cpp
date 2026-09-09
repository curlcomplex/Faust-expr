// Same-source, same-settings offline benchmark, not device audio acceptance.
#include <atomic>
#include <cstdlib>
#include <new>
static thread_local bool allocationGuard = false;
static std::atomic<unsigned long long> guardedAllocations {0};
void* operator new(std::size_t n) {
    if (allocationGuard) ++guardedAllocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#define main offlineRendererMain
#include "render.cpp"
#undef main
int main(int argc, char** argv) { try {
    const int block = argc > 1 ? int(integer(argv[1])) : 128;
    if (block < 1 || block > 2048) throw std::runtime_error("block");
    constexpr int voices = 4, count = 2000, warmup = 256;
    std::vector<std::unique_ptr<ModuleDSP>> dsps;
    std::vector<std::unique_ptr<UI>> uis;
    for (int v = 0; v < voices; ++v) {
        dsps.push_back(std::make_unique<ModuleDSP>());
        dsps.back()->init(48000);
        uis.push_back(std::make_unique<UI>());
        dsps.back()->buildUserInterface(uis.back().get());
        uis.back()->set("pitch_hz", 41.0f+v*31.0f);
        uis.back()->set("wave", float(v*2+4));
        uis.back()->set("drive", .7f);
        uis.back()->set("decay", .75f);
    }
    std::vector<float> output(block), durations; durations.reserve(count);
    float* ptr = output.data(); double checksum = 0.0;
    for (int j = -warmup; j < count; ++j) {
        for (auto& ui : uis) ui->set("gate", j % 13 == 0 ? 1.0f : 0.0f);
        allocationGuard = true;
        const auto start = std::chrono::steady_clock::now();
        for (auto& dsp : dsps) {
            dsp->compute(block, nullptr, &ptr);
            checksum += output[block/2];
        }
        const double us = double(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-start).count()) / 1000.0;
        allocationGuard = false;
        if (j >= 0) durations.push_back(float(us));
    }
    if (!std::isfinite(checksum)) throw std::runtime_error("nonfinite compute");
    std::sort(durations.begin(), durations.end());
    std::cout << std::setprecision(12)
        << "{\"voices\":" << voices << ",\"block\":" << block
        << ",\"rate\":48000,\"iterations\":" << count
        << ",\"sizeof_dsp_bytes\":" << sizeof(ModuleDSP)
        << ",\"ordinary_new_allocations_in_compute\":" << guardedAllocations.load()
        << ",\"p50_us\":" << durations[count/2]
        << ",\"p95_us\":" << durations[int(count*.95)]
        << ",\"p99_us\":" << durations[int(count*.99)]
        << ",\"max_us\":" << durations.back()
        << ",\"checksum\":" << checksum << "}\n";
    return guardedAllocations ? 1 : 0;
} catch (const std::exception& e) {
    allocationGuard = false; std::cerr << e.what() << '\n'; return 1;
} }
