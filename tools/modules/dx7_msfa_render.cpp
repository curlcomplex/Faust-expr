// Narrow offline adapter for unmodified Google MSFA; no synthesis equations here.
// The original core is Apache-2.0. See the pinned source archive and license.
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "synth.h"
#include "controllers.h"
#include "dx7note.h"
#include "freqlut.h"
#include "exp2.h"
#include "sin.h"
#include "patch.h"

struct Event { int frame; std::string name; int value; };
int integer(const char* text) {
    std::size_t used = 0;
    long value = std::stol(text, &used);
    if (used != std::strlen(text) || value < 0 || value > 10000000)
        throw std::runtime_error("invalid integer argument");
    return static_cast<int>(value);
}
int main(int argc, char** argv) try {
    if (argc != 8) throw std::runtime_error("usage: render packed128 score.tsv output.f32 rate block frames seed");
    const int rate = integer(argv[4]), block = integer(argv[5]);
    const int frames = integer(argv[6]), seed = integer(argv[7]);
    // Original MSFA amplitude envelopes do not rescale their increments with SR.
    // Do not silently claim a cross-rate hardware comparison from this adapter.
    if (rate != 44100 || block < 1 || block > 4096 || frames < N || frames % N || seed != 0)
        throw std::runtime_error("slice supports 44100 Hz, whole 64-frame quanta, seed 0");
    std::array<char,128> packed{};
    std::ifstream patchfile(argv[1], std::ios::binary);
    if (!patchfile.read(packed.data(), packed.size()) || patchfile.peek() != EOF)
        throw std::runtime_error("patch must have exactly 128 bytes");
    for (unsigned char b : packed) if (b > 127) throw std::runtime_error("non-7-bit patch");
    std::array<char,156> patch{};
    UnpackPatch(packed.data(), patch.data());
    // This adapter deliberately supports only the baseline's algorithm/state.
    if (patch[134] != 0 || patch[135] != 0 || patch[136] != 1 || patch[139] != 0 || patch[140] != 0 || patch[144] != 24)
        throw std::runtime_error("unsupported algorithm/feedback/modulation/transpose");
    for (int op=0; op<6; ++op) {
        int o=op*21;
        for (int k=0; k<11; ++k) if (static_cast<unsigned char>(patch[o+k]) > 99)
            throw std::runtime_error("invalid operator data");
        if (patch[o+11] || patch[o+12] || patch[o+13] || patch[o+14] || patch[o+15] || patch[o+17] || patch[o+19] || patch[o+20] != 7)
            throw std::runtime_error("unsupported operator mode/scaling/detune");
        if (static_cast<unsigned char>(patch[o+16])>99 || patch[o+18] < 1 || patch[o+18] > 2)
            throw std::runtime_error("invalid level/coarse ratio");
    }
    std::ifstream score(argv[2]);
    if (!score) throw std::runtime_error("cannot open score");
    std::vector<Event> events;
    std::set<std::pair<int,std::string>> seen;
    std::string line;
    int last=-1, note=-1, velocity=-1, on=-1, off=-1;
    while (std::getline(score,line)) {
        std::istringstream row(line); Event e; std::string extra;
        if (!(row>>e.frame>>e.name>>e.value) || (row>>extra) || e.frame<0 || e.frame>=frames || e.frame<last || !seen.insert({e.frame,e.name}).second)
            throw std::runtime_error("malformed/duplicate/unordered score event");
        if (e.name=="note" && e.frame==0 && e.value>=0 && e.value<=127) note=e.value;
        else if (e.name=="velocity" && e.frame==0 && e.value>=1 && e.value<=127) velocity=e.value;
        else if (e.name=="gate" && e.frame%N==0 && (e.value==0 || e.value==1)) {
            if (e.value==1 && on<0 && off<0) on=e.frame;
            else if (e.value==0 && on>=0 && off<0 && e.frame>on) off=e.frame;
            else throw std::runtime_error("slice requires one ordered note-on/note-off pair");
        } else throw std::runtime_error("unsupported event or non-64-aligned gate");
        events.push_back(e); last=e.frame;
    }
    if (note<0 || velocity<0 || on<0 || off<0) throw std::runtime_error("incomplete score");
    Freqlut::init(rate); Exp2::init(); Sin::init(); PitchEnv::init(rate);
    Controllers controllers{}; controllers.values_[kControllerPitch]=0x2000;
    Dx7Note voice{}; // zero initialization includes all feedback/history state
    std::ofstream output(argv[3], std::ios::binary);
    if (!output) throw std::runtime_error("cannot open output");
    alignas(16) std::array<int32_t,N> core{};
    std::array<float,N> pending{};
    int cursor=N, generated=0, written=0, calls=0;
    double computeSeconds=0;
    bool started=false;
    // Caller chunking never changes MSFA's internal 64-frame control cadence.
    while (written<frames) {
        int budget=std::min(block,frames-written);
        while (budget>0) {
            if (cursor==N) {
                if (generated==on) { voice.init(patch.data(),note,velocity); started=true; }
                if (generated==off) voice.keyup();
                core.fill(0); // Dx7Note::compute adds to its destination
                if (started) {
                    auto begin=std::chrono::steady_clock::now();
                    voice.compute(core.data(),1<<23,0,&controllers);
                    computeSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
                    ++calls;
                }
                for(int i=0;i<N;++i) pending[i]=static_cast<float>(core[i]/16777216.0);
                generated+=N; cursor=0;
            }
            int take=std::min(budget,N-cursor);
            output.write(reinterpret_cast<const char*>(pending.data()+cursor),take*sizeof(float));
            if(!output) throw std::runtime_error("output write failed");
            cursor+=take; written+=take; budget-=take;
        }
    }
    std::cout << "{\"frames\":"<<frames<<",\"channels\":1,\"sample_rate\":"<<rate
              <<",\"host_block\":"<<block<<",\"native_quantum\":"<<N
              <<",\"compute_calls\":"<<calls<<",\"compute_seconds\":"<<computeSeconds
              <<",\"gate_on\":"<<on<<",\"gate_off\":"<<off<<",\"note\":"<<note
              <<",\"velocity\":"<<velocity<<",\"q24_divisor\":16777216,\"unpacked_patch\":[";
    for(int i=0;i<156;++i) std::cout<<(i?",":"")<<static_cast<int>(static_cast<unsigned char>(patch[i]));
    std::cout<<"]}\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
