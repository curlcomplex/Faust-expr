// #112 phase/index diagnostic adapter. Original MSFA synthesis sources remain unchanged.
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

static void set_op(char* p, int op, int out, int coarse) {
    const int o=op*21;
    p[o+0]=99; p[o+1]=99; p[o+2]=99; p[o+3]=99;
    p[o+4]=99; p[o+5]=99; p[o+6]=99; p[o+7]=0;
    p[o+8]=50; p[o+9]=0; p[o+10]=0; p[o+11]=0; p[o+12]=0;
    p[o+13]=0; p[o+14]=0; p[o+15]=0; p[o+16]=out;
    p[o+17]=0; p[o+18]=coarse; p[o+19]=0; p[o+20]=7;
}

int main(int argc,char** argv) try {
    if(argc!=6) throw std::runtime_error("usage: oracle OUT.f32 MOD_LEVEL FRAMES GATE_ON GATE_OFF");
    const int level=std::stoi(argv[2]), frames=std::stoi(argv[3]);
    const int gate_on=std::stoi(argv[4]), gate_off=std::stoi(argv[5]);
    if(level<0 || level>99 || frames<=gate_off || gate_on<0 || gate_on>=gate_off || gate_on%64 || gate_off%64 || frames%64)
        throw std::runtime_error("invalid bounded probe arguments");
    const int rate=44100;
    Freqlut::init(rate); Exp2::init(); Sin::init(); PitchEnv::init(rate);
    Controllers ctrls{}; ctrls.values_[kControllerPitch]=0x2000;
    char p[156]{};
    for(int op=0;op<6;++op) set_op(p,op,0,1);
    // MSFA storage index 0 is DX operator 6; index 5 is operator 1.
    set_op(p,5,80,1); set_op(p,4,level,2);
    p[134]=0; p[135]=0; // algorithm 1, no feedback
    for(int i=0;i<4;++i){p[126+i]=99; p[130+i]=50;}
    p[139]=0; p[143]=0;
    Dx7Note note; bool started=false, released=false;
    std::vector<float> out(frames,0.f);
    for(int n=0;n<frames;n+=64) {
        if(!started && n>=gate_on){note.init(p,57,100); started=true;}
        if(started && !released && n>=gate_off){note.keyup(); released=true;}
        if(started){int32_t block[64]{}; note.compute(block,1<<23,0,&ctrls); for(int j=0;j<64;++j) out[n+j]=float(block[j])/float(1<<24);}
    }
    std::ofstream f(argv[1],std::ios::binary); f.write(reinterpret_cast<const char*>(out.data()),out.size()*sizeof(float));
    if(!f) throw std::runtime_error("write failed");
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<"\n"; return 1;}
