// Offline fit/replay boundary around ACTUAL generated Faust, not a host ABI.
#define main module_score_cli_unused
#include "render.cpp"
#undef main
extern "C" int color_render(const double* values, int count, int rate, int frames,
                              int off, int block, float* output) noexcept {
    try {
        if (!values || !output || rate<8000 || rate>96000 || frames<1 || frames>rate*8
            || block<1 || block>8192 || off<1 || off>frames) return -1;
        auto s=std::make_unique<ModuleDSP>(); s->init(rate); UI ui; s->buildUserInterface(&ui);
        if (s->getNumInputs()!=0 || s->getNumOutputs()!=1 || count!=int(ui.zones.size())) return -2;
        int i=0;
        for (const auto& [name,z]:ui.zones) {
            const double v=values[i++];
            if (!std::isfinite(v) || v<z.lo || v>z.hi || (z.boolean && v!=0. && v!=1.)) return -3;
            ui.set(name,float(v));
        }
        ui.set("gate",0.f); float warm[256]{}; float* op[]={warm}; s->compute(256,nullptr,op);
        ui.set("gate",1.f);
        for (int n=0;n<frames;) {
            if (n==off) ui.set("gate",0.f);
            int k=std::min(block,frames-n); if (n<off) k=std::min(k,off-n);
            op[0]=output+n; s->compute(k,nullptr,op); n+=k;
        }
        for (int n=0;n<frames;++n) if (!std::isfinite(output[n])) return -4;
        return 0;
    } catch (...) { return -5; }
}
