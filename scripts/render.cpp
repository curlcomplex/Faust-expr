// Offline driver for the actual Faust-generated DSP, not a substitute model.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <faust/dsp/dsp.h>
#include <faust/gui/meta.h>
#include <faust/gui/MapUI.h>
#include "probe.hpp"

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: render OUTPUT.f32");
        static_assert(sizeof(FAUSTFLOAT) == 4, "This probe requires float32 Faust output");
        const std::uint16_t endian = 1;
        if (*reinterpret_cast<const unsigned char*>(&endian) != 1)
            throw std::runtime_error("This raw float fixture requires little-endian output");
        constexpr int rate = 48000, frames = rate, block = 240;
        ProbeDSP synth;
        synth.init(rate);
        MapUI ui;
        synth.buildUserInterface(&ui);
        if (synth.getNumInputs() != 0 || synth.getNumOutputs() != 1)
            throw std::runtime_error("Unexpected DSP channel counts");
        auto* gate = ui.getParamZone("gate");
        auto* freq = ui.getParamZone("freq");
        auto* gain = ui.getParamZone("gain");
        if (!gate || !freq || !gain) throw std::runtime_error("Missing DSP parameter");
        *gate = 0; *freq = 220; *gain = 0.15f;
        std::vector<FAUSTFLOAT> audio(frames, 0);
        for (int frame = 0; frame < frames; frame += block) {
            if (frame == 2400) *gate = 1;         // 50 ms: 220 Hz starts
            if (frame == 14400) *gate = 0;       // 300 ms: release
            if (frame == 24000) { *freq = 880; *gate = 1; }
            if (frame == 36000) *gate = 0;       // 750 ms: final release
            FAUSTFLOAT* outputs[] = {audio.data() + frame};
            synth.compute(std::min(block, frames - frame), nullptr, outputs);
        }
        for (auto sample : audio)
            if (!std::isfinite(sample)) throw std::runtime_error("Non-finite DSP output");
        std::ofstream out(argv[1], std::ios::binary);
        if (!out) throw std::runtime_error("Cannot open output file");
        out.write(reinterpret_cast<const char*>(audio.data()),
                  static_cast<std::streamsize>(audio.size() * sizeof(FAUSTFLOAT)));
        out.close();
        if (!out) throw std::runtime_error("Failed to write audio");
        std::cout << "Rendered " << frames << " frames at " << rate
                  << " Hz using the Faust-generated ProbeDSP\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
