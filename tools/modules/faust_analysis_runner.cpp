// Bounded, offline mono analysis host. No instrument/device integration.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct dsp { virtual ~dsp() = default; };
struct Meta { void declare(const char*, const char*) {} };
struct UI {
    void openTabBox(const char*) {} void openHorizontalBox(const char*) {}
    void openVerticalBox(const char*) {} void closeBox() {}
    void declare(float*, const char*, const char*) {}
    void addButton(const char*, float*) {} void addCheckButton(const char*, float*) {}
    void addVerticalSlider(const char*, float*, float, float, float, float) {}
    void addHorizontalSlider(const char*, float*, float, float, float, float) {}
    void addNumEntry(const char*, float*, float, float, float, float) {}
    void addVerticalBargraph(const char*, float*, float, float) {}
    void addHorizontalBargraph(const char*, float*, float, float) {}
};
#include "analysis_generated.hpp"

// The default #105 meter ABI is unchanged. Other OFFLINE analyzers explicitly
// select their output count at compilation; a mismatched DSP still fails closed.
#ifndef FAUST_ANALYSIS_OUTPUTS
#define FAUST_ANALYSIS_OUTPUTS 6
#endif
static_assert(FAUST_ANALYSIS_OUTPUTS >= 1 && FAUST_ANALYSIS_OUTPUTS <= 64);

static long long integer(const char* s) {
    const std::string value(s);
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("invalid nonnegative integer");
    return std::stoll(value);
}

int main(int argc, char** argv) {
    try {
        if (argc != 5 && argc != 7)
            throw std::runtime_error("usage: runner INPUT.f32 OUTPUT.f32 RATE FRAMES [BLOCK TAIL]");
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
        const uint16_t endian = 1;
        if (*reinterpret_cast<const unsigned char*>(&endian) != 1)
            throw std::runtime_error("raw format requires a little-endian host");
        const auto rate_arg = integer(argv[3]);
        const auto frames = integer(argv[4]);
        const auto block_arg = argc == 7 ? integer(argv[5]) : 256;
        const auto tail = argc == 7 ? integer(argv[6]) : 11;
        if (rate_arg < 8000 || rate_arg > 192000 || frames < 1 ||
            frames > rate_arg * 600 || block_arg < 1 || block_arg > 65536 || tail > 4096)
            throw std::runtime_error("invalid dimensions");
        if (std::filesystem::exists(argv[2]) && std::filesystem::equivalent(argv[1], argv[2]))
            throw std::runtime_error("input and output must differ");
        if (std::filesystem::file_size(argv[1]) != static_cast<uintmax_t>(frames) * sizeof(float))
            throw std::runtime_error("input size mismatch");
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("input open failed");
        auto module = std::make_unique<ModuleDSP>();
        module->init(static_cast<int>(rate_arg));
        constexpr int outputs = FAUST_ANALYSIS_OUTPUTS;
        if (module->getNumInputs() != 1 || module->getNumOutputs() != outputs)
            throw std::runtime_error("analysis DSP IO contract changed");
        std::ofstream output(argv[2], std::ios::binary);
        if (!output) throw std::runtime_error("output open failed");
        const auto block = static_cast<int>(block_arg);
        std::vector<float> x(static_cast<size_t>(block), 0.0f);
        std::vector<float> planar(static_cast<size_t>(block) * outputs);
        std::vector<float> interleaved(static_cast<size_t>(block) * outputs);
        std::array<float*, outputs> ptrs{};
        for (int c = 0; c < outputs; ++c) ptrs[c] = planar.data() + c * block;
        float* inputs[] = {x.data()};
        for (long long base = 0; base < frames + tail; base += block) {
            const int n = static_cast<int>(std::min<long long>(block, frames + tail - base));
            const int real = static_cast<int>(std::max(0LL, std::min<long long>(n, frames - base)));
            std::fill(x.begin(), x.end(), 0.0f);
            if (real) {
                input.read(reinterpret_cast<char*>(x.data()), real * sizeof(float));
                if (input.gcount() != static_cast<std::streamsize>(real * sizeof(float)))
                    throw std::runtime_error("short input read");
            }
            for (int i = 0; i < real; ++i)
                if (!std::isfinite(x[i])) throw std::runtime_error("nonfinite input");
            module->compute(n, inputs, ptrs.data());
            for (int i = 0; i < n; ++i) {
                for (int c = 0; c < outputs; ++c) {
                    const float value = ptrs[c][i];
                    if (!std::isfinite(value)) throw std::runtime_error("nonfinite analyzer output");
                    interleaved[static_cast<size_t>(i) * outputs + c] = value;
                }
            }
            output.write(reinterpret_cast<const char*>(interleaved.data()), n * outputs * sizeof(float));
            if (!output) throw std::runtime_error("output write failed");
        }
        output.close();
        if (!output) throw std::runtime_error("output close failed");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
