// Independent procedural cross-check of the numerical scheme in Mini v3.
// This is NOT a hardware oracle. No Faust-generated expressions are imported.
// Adapted from Stefano D'Angelo's ImprovedModel.h, Git blob
// e83f5398b6b9e8bd7a123689017efbbaf2be973b in ddiakopoulos/MoogLadders.
// Changes: four integration substeps; cubic saturator; 2*r feedback; file CLI.
// Copyright 2012 Stefano D'Angelo <zanga.mail@gmail.com>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static double number(const char* text) {
    std::size_t used=0;
    double x=std::stod(text,&used);
    if (used!=std::string(text).size() || !std::isfinite(x))
        throw std::runtime_error("invalid number");
    return x;
}
static double saturate(double x) {
    x=std::clamp(x,-1.0,1.0);
    return x-x*x*x/3.0;
}
int main(int argc,char** argv) { try {
    if (argc!=6) throw std::runtime_error("usage: reference INPUT OUTPUT RATE CUTOFF RESONANCE");
    const double rate=number(argv[3]), cutoff=number(argv[4]), r=number(argv[5]);
    if (rate<8000 || rate>96000 || cutoff<20 || cutoff>44000 || r<0 || r>1)
        throw std::runtime_error("out of domain");
    std::ifstream in(argv[1],std::ios::binary|std::ios::ate);
    if (!in) throw std::runtime_error("input unavailable");
    auto bytes=in.tellg();
    if (bytes<=0 || bytes>100000000 || bytes%sizeof(float)) throw std::runtime_error("input size");
    std::vector<float> audio(static_cast<std::size_t>(bytes)/sizeof(float));
    in.seekg(0); in.read(reinterpret_cast<char*>(audio.data()),bytes);
    if (!in) throw std::runtime_error("input read failed");
    const double internalRate=4.0*rate, vt=.312, inv=1.0/(2.0*vt);
    const double fc=std::clamp(cutoff,20.0,.45*rate), pi=std::acos(-1.0);
    const double warp=pi*fc/internalRate;
    const double g=4.0*pi*vt*fc*(1.0-warp)/(1.0+warp);
    const double h=.5/internalRate;
    std::array<double,4> voltage{},previousDerivative{};
    for (float& sample:audio) {
        if (!std::isfinite(sample)) throw std::runtime_error("nonfinite input");
        const double drive=sample;
        for (int substep=0;substep<4;++substep) {
            for (int stage=0;stage<4;++stage) {
                const double derivative=stage==0
                    ? -g*(saturate((drive+2.0*r*voltage[3])*inv)+saturate(voltage[0]*inv))
                    : g*(saturate(voltage[stage-1]*inv)-saturate(voltage[stage]*inv));
                voltage[stage]+=h*(derivative+previousDerivative[stage]);
                previousDerivative[stage]=derivative;
            }
        }
        sample=static_cast<float>(voltage[3]);
        if (!std::isfinite(sample)) throw std::runtime_error("nonfinite output");
    }
    std::ofstream out(argv[2],std::ios::binary);
    out.write(reinterpret_cast<const char*>(audio.data()),bytes);
    if (!out) throw std::runtime_error("output write failed");
    return 0;
} catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
