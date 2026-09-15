// Thin offline adapter for the pinned MSFA DX7 core used by #96/#112.
// Compile this file against google/music-synthesizer-for-android at the pinned commit.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "synth.h"
#include "freqlut.h"
#include "sin.h"
#include "exp2.h"
#include "pitchenv.h"
#include "controllers.h"
#include "dx7note.h"

static void set_op(char* p, int op, int out, int coarse,
                   int r1=99,int r2=99,int r3=99,int r4=99,
                   int l1=99,int l2=99,int l3=99,int l4=0) {
    const int o=op*21;
    p[o+0]=r1; p[o+1]=r2; p[o+2]=r3; p[o+3]=r4;
    p[o+4]=l1; p[o+5]=l2; p[o+6]=l3; p[o+7]=l4;
    p[o+8]=50; p[o+9]=0; p[o+10]=0; p[o+11]=0; p[o+12]=0;
    p[o+13]=0; p[o+14]=0; p[o+15]=0; p[o+16]=out;
    p[o+17]=0; p[o+18]=coarse; p[o+19]=0; p[o+20]=7; // neutral DX detune encoding
}

static void make_patch(const std::string& id, char* p) {
    std::fill(p,p+156,0);
    for (int op=0;op<6;++op) set_op(p,op,0,1);
    // Algorithm 5 (zero-based 4): op2 -> op1, with the other pairs silent.
    p[134]=4; p[135]=0;
    for(int i=0;i<4;++i){p[126+i]=99; p[130+i]=50;}
    p[139]=0; p[143]=0;
    if(id=="DX7-C01") {
        set_op(p,0,85,1);
    } else if(id=="DX7-C02") {
        set_op(p,0,85,1); set_op(p,1,75,2);
    } else if(id=="DX7-C03") {
        set_op(p,0,85,1); set_op(p,1,90,2,80,60,50,65,99,70,55,0);
    } else throw std::runtime_error("unknown case");
}

int main(int argc,char** argv) try {
    if(argc!=6) throw std::runtime_error("usage: oracle CASE OUT.f32 RATE FRAMES NOTE_OFF_FRAME");
    const std::string id=argv[1]; const int rate=std::stoi(argv[3]);
    const int frames=std::stoi(argv[4]), noteoff=std::stoi(argv[5]);
    if(rate!=44100 || frames<64 || noteoff<0 || noteoff>=frames || noteoff%64)
        throw std::runtime_error("H1 oracle requires 44.1 kHz and 64-frame-aligned note-off");
    Freqlut::init(rate); Exp2::init(); Sin::init(); PitchEnv::init(rate);
    Controllers ctrls{}; ctrls.values_[kControllerPitch]=0x2000;
    char patch[156]; make_patch(id,patch);
    Dx7Note note; note.init(patch,60,127);
    std::vector<float> out(frames,0.f); bool released=false;
    for(int n=0;n<frames;n+=64) {
        if(!released && n>=noteoff){note.keyup(); released=true;}
        int32_t block[64]{}; note.compute(block,1<<23,0,&ctrls);
        const int count=std::min(64,frames-n);
        for(int j=0;j<count;++j) out[n+j]=float(block[j])/float(1<<24);
    }
    if(!std::all_of(out.begin(),out.end(),[](float x){return std::isfinite(x);}))
        throw std::runtime_error("nonfinite oracle output");
    std::ofstream f(argv[2],std::ios::binary); f.write(reinterpret_cast<const char*>(out.data()),out.size()*sizeof(float));
    if(!f) throw std::runtime_error("write failed");
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<"\n"; return 1;}
