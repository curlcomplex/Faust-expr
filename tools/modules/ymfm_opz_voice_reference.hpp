// Transport/UI only around unchanged pinned ymfm FM engine; no replacement DSP.
// Native-core output excludes ym2414's explicitly guessed roundtrip_fp DAC stage.
#include "ymfm_opz.h"
#include "ymfm_fm.ipp"
#include <array>
#ifndef TX81Z_NATIVE_PROBE
#define TX81Z_NATIVE_PROBE 0
#endif
class ModuleDSP {
    ymfm::ymfm_interface interface;
    ymfm::fm_engine_base<ymfm::opz_registers> engine{interface};
    struct Op {
        float Wave=0,Mode=0,Coarse=2,Fine=0,DT1=0,DT2=0,Range=0,TL=36;
        float AR=31,D1R=12,D2R=0,SL=8,RR=7,KS=0,Reverb=0;
    };
    std::array<Op,4> ops;
    std::array<int,512> written;
    float gate=0,freq=220,velocity=1,algorithm=1,feedback=0,level=.5,blockFreq=-1;
    float heldVelocity=0;
    bool lastGate=false;
    int sampleRate=0;
    void reg(unsigned a,int d) {
        if (written[a]!=d) { engine.write(a,static_cast<uint8_t>(d));written[a]=d; }
    }
    void controls() {
        int bf=int(blockFreq);
        if (bf<0) {
            int pitch=std::clamp(int(std::floor((69.+12.*std::log2(double(freq)/440.)-13.)*64.+.5)),0,6143);
            int oct=pitch/768,note=(pitch/64)%12;
            bf=(oct<<10)|((note+note/3)<<6)|(pitch%64);
        }
        reg(0x28,(bf>>6)&127); reg(0x30,((bf&63)<<2)|1);
        constexpr unsigned offsets[4]={24,8,16,0}; // v2/v4 public carrier-first order
        for (unsigned i=0;i<4;++i) {
            const auto& p=ops[i];unsigned o=offsets[i];
            // Overloaded fields use different backing cache slots in this adapter.
            int ratio=(int(p.Mode)?int(p.Range):int(p.DT1))*16+int(p.Coarse);
            if (written[0x40+o]!=ratio) {engine.write(0x40+o,ratio);written[0x40+o]=ratio;}
            int wave=128+(int(p.Wave)<<4)+int(p.Fine);
            if (written[0x100+o]!=wave) {engine.write(0x40+o,wave);written[0x100+o]=wave;}
            reg(0x60+o,int(p.TL));
            reg(0x80+o,(int(p.KS)<<6)|(int(p.Mode)<<5)|int(p.AR));
            reg(0xa0+o,int(p.D1R));
            int dr2=(int(p.DT2)<<6)|int(p.D2R);
            if (written[0xc0+o]!=dr2) {engine.write(0xc0+o,dr2);written[0xc0+o]=dr2;}
            int rev=32|int(p.Reverb);
            if (written[0x120+o]!=rev) {engine.write(0xc0+o,rev);written[0x120+o]=rev;}
            reg(0xe0+o,(int(p.SL)<<4)|int(p.RR));
        }
        reg(0x20,128|(gate>0?64:0)|(int(feedback)<<3)|(int(algorithm)-1));
    }
public:
    void init(int rate) {sampleRate=rate;engine.reset();written.fill(-1);ops[0].Coarse=1;ops[0].TL=0;}
    int getNumInputs() const {return 0;}
    int getNumOutputs() const {return TX81Z_NATIVE_PROBE?4:1;}
    void buildUserInterface(UI* u) {
        u->addButton("gate",&gate);
        u->addHorizontalSlider("freq",&freq,220,20,4400,.01);
        u->addHorizontalSlider("velocity",&velocity,1,0,1,.001);
        u->addNumEntry("algorithm",&algorithm,1,1,8,1);
        u->addNumEntry("feedback",&feedback,0,0,7,1);
        u->addHorizontalSlider("level",&level,.5,0,1,.001);
        u->addNumEntry("blockFreq",&blockFreq,-1,-1,8191,1);
        for(unsigned i=0;i<4;++i) {
            auto& p=ops[i];auto add=[&](const char* n,float* v,int lo,int hi){u->addNumEntry(("op"+std::to_string(i+1)+n).c_str(),v,*v,lo,hi,1);};
            add("Wave",&p.Wave,0,7);add("Mode",&p.Mode,0,1);add("Coarse",&p.Coarse,0,15);add("Fine",&p.Fine,0,15);
            add("DT1",&p.DT1,0,7);add("DT2",&p.DT2,0,3);add("Range",&p.Range,0,7);add("TL",&p.TL,0,127);
            add("AR",&p.AR,0,31);add("D1R",&p.D1R,0,31);add("D2R",&p.D2R,0,31);add("SL",&p.SL,0,15);
            add("RR",&p.RR,0,15);add("KS",&p.KS,0,3);add("Reverb",&p.Reverb,0,7);
        }
    }
    void compute(int n,float**,float** output) {
        if(sampleRate!=55930) throw std::runtime_error("OPZ reference requires native 55930-Hz tick stream");
        controls();const bool on=gate>0;if(on&&!lastGate) heldVelocity=velocity;lastGate=on;
        for(int i=0;i<n;++i) {
            engine.clock(1);
            ymfm::ymfm_output<2> out;engine.output(out.clear(),0,32767,1);
            if(TX81Z_NATIVE_PROBE) {
                auto* op=engine.debug_channel(0)->debug_operator(3);
                output[0][i]=op->debug_eg_attenuation();output[1][i]=op->debug_eg_state();
                output[2][i]=op->phase()&1023;output[3][i]=op->compute_volume(op->phase(),0);
            } else output[0][i]=float(out.data[0])*(1.f/32768.f)*heldVelocity*level;
        }
    }
};
