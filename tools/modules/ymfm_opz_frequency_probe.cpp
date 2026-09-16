// TX81Z/OPZ frequency-law characterization against unchanged ymfm.
// This is a diagnostic adapter, not a replacement oscillator implementation.
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <string>
#include "ymfm_opz.h"
#include "ymfm_fm.ipp"

static void write_reg(ymfm::opz_registers &r, uint16_t a, uint8_t d) {
    uint32_t c=0,m=0; r.write(a,d,c,m);
}

static uint32_t block_freq(unsigned block,unsigned code,unsigned frac=0) {
    return ((block & 7u)<<10) | ((code & 15u)<<6) | (frac & 63u);
}

static void set_key(ymfm::opz_registers &r,uint32_t bf) {
    write_reg(r,0x28,(bf>>6)&0x7f);
    write_reg(r,0x30,(bf&0x3f)<<2);
}

static void set_ratio_op(ymfm::opz_registers &r,unsigned dt1,unsigned coarse,unsigned fine,unsigned dt2) {
    write_reg(r,0x40,((dt1&7)<<4)|(coarse&15));
    write_reg(r,0x40,0x80|(fine&15));
    write_reg(r,0x80,0x00); // ratio mode, AR irrelevant here
    write_reg(r,0xc0,(dt2&3)<<6);
}

static void set_fixed_op(ymfm::opz_registers &r,unsigned range,unsigned ff,unsigned fine) {
    write_reg(r,0x40,((range&7)<<4)|(ff&15));
    write_reg(r,0x40,0x80|(fine&15));
    write_reg(r,0x80,0x20); // fixed mode
}

static uint32_t ratio_step(ymfm::opz_registers &r) {
    ymfm::opdata_cache c{}; r.cache_operator_data(0,0,c);
    return r.compute_phase_step(0,0,c,0);
}

static double fixed_mean_step(ymfm::opz_registers &r) {
    ymfm::opdata_cache c{}; r.cache_operator_data(0,0,c);
    uint64_t total=0;
    constexpr unsigned N=65536;
    for(unsigned i=0;i<N;i++) total += r.compute_phase_step(0,0,c,0);
    return double(total)/N;
}

int main() {
    const unsigned codes[12]={0,1,2,4,5,6,8,9,10,12,13,14};
    std::cout<<std::setprecision(17);
    std::cout<<"kind,block,code,dt1,coarse,fine,dt2,range,fixed,step,baseline,ratio\n";
    for(unsigned block: {2u,4u,6u}) for(unsigned code: codes) {
        const uint32_t bf=block_freq(block,code,0);
        ymfm::opz_registers base; base.reset(); set_key(base,bf); set_ratio_op(base,0,1,0,0);
        const double b=ratio_step(base);
        for(unsigned coarse: {0u,1u,2u,7u,15u}) for(unsigned fine: {0u,1u,7u,8u,15u}) {
            ymfm::opz_registers r; r.reset(); set_key(r,bf); set_ratio_op(r,0,coarse,fine,0);
            double s=ratio_step(r); std::cout<<"ratio,"<<block<<','<<code<<",0,"<<coarse<<','<<fine<<",0,0,0,"<<s<<','<<b<<','<<s/b<<"\n";
        }
        for(unsigned dt2=0;dt2<4;dt2++) {
            ymfm::opz_registers r; r.reset(); set_key(r,bf); set_ratio_op(r,0,1,0,dt2);
            double s=ratio_step(r); std::cout<<"dt2,"<<block<<','<<code<<",0,1,0,"<<dt2<<",0,0,"<<s<<','<<b<<','<<s/b<<"\n";
        }
        for(unsigned dt1=0;dt1<8;dt1++) {
            ymfm::opz_registers r; r.reset(); set_key(r,bf); set_ratio_op(r,dt1,1,0,0);
            double s=ratio_step(r); std::cout<<"dt1,"<<block<<','<<code<<','<<dt1<<",1,0,0,0,0,"<<s<<','<<b<<','<<s/b<<"\n";
        }
        for(unsigned range: {0u,1u,3u,7u}) for(unsigned ff: {0u,1u,8u,15u}) for(unsigned fine: {0u,7u,15u}) {
            ymfm::opz_registers r; r.reset(); set_key(r,bf); set_fixed_op(r,range,ff,fine);
            double s=fixed_mean_step(r); std::cout<<"fixed,"<<block<<','<<code<<",0,0,"<<fine<<",0,"<<range<<','<<ff<<','<<s<<','<<b<<','<<s/b<<"\n";
        }
    }
}
