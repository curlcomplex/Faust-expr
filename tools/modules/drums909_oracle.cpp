// Copyright 2016 Emilie Gillet. MIT permission/notice retained in
// modules/drums-909/v1/LICENSES.md. Functions below extracted verbatim from
// pichenettes/eurorack 08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4:
// synthetic_snare_drum.h::DistortedSine and synthetic_bass_drum.h::TransistorVCA.
// This harness compares these TWO COMPONENTS, not full voices or hardware.
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

inline float DistortedSine(float phase) {
    float triangle = (phase < 0.5f ? phase : 1.0f - phase) * 4.0f - 1.3f;
    return 2.0f * triangle / (1.0f + fabsf(triangle));
}
inline float TransistorVCA(float s, float gain) {
    s = (s - 0.6f) * gain;
    return 3.0f * s / (2.0f + fabsf(s)) + gain * 0.3f;
}
int main(int argc,char** argv) { try {
    if(argc!=3) throw std::runtime_error("usage: oracle input.f32 output.f32");
    const uint32_t end=1;
    if(*reinterpret_cast<const char*>(&end)!=1) throw std::runtime_error("little endian required");
    std::ifstream in(argv[1],std::ios::binary|std::ios::ate);
    if(!in) throw std::runtime_error("input open");
    const auto bytes=in.tellg();
    if(bytes<=0 || bytes%(3*sizeof(float)) || bytes>3000000) throw std::runtime_error("input dimensions");
    std::vector<float> x(static_cast<size_t>(bytes)/sizeof(float));in.seekg(0);
    in.read(reinterpret_cast<char*>(x.data()),bytes);
    if(!in) throw std::runtime_error("input read");
    std::vector<float> y(x.size()/3*2);
    for(size_t i=0;i<x.size()/3;i++) {
      const float p=x[3*i],s=x[3*i+1],g=x[3*i+2];
      if(!std::isfinite(p)||!std::isfinite(s)||!std::isfinite(g)) throw std::runtime_error("nonfinite input");
      y[2*i]=DistortedSine(p);y[2*i+1]=TransistorVCA(s,g);
    }
    std::ofstream out(argv[2],std::ios::binary);
    out.write(reinterpret_cast<const char*>(y.data()),y.size()*sizeof(float));out.close();
    if(!out) throw std::runtime_error("output write");
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
