// Diagnostic transport around unchanged, pinned BSD-3-Clause ymfm sources.
// NOT a chip-clock, envelope or complete TX81Z audio renderer.
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include "ymfm_opz.h"
#include "ymfm_fm.ipp"
int main() {
    try {
        ymfm::opz_registers regs;
        regs.reset();
        std::cout << "wave,index,log_word,linear\n" << std::setprecision(17);
        for (uint32_t wave = 0; wave < 8; ++wave) {
            uint32_t channel = 0, mask = 0;
            regs.write(0x40, 0x80 | (wave << 4), channel, mask);
            if (regs.op_waveform(0) != wave) throw std::runtime_error("wave register decode");
            ymfm::opdata_cache cache{};
            regs.cache_operator_data(0, 0, cache);
            for (uint32_t i = 0; i < regs.WAVEFORM_LENGTH; ++i) {
                const uint16_t word = cache.waveform[i];
                const double magnitude = ymfm::attenuation_to_volume(word & 0x7fff) / 8192.0;
                std::cout << wave << ',' << i << ',' << word << ','
                          << ((word & 0x8000) ? -magnitude : magnitude) << '\n';
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
