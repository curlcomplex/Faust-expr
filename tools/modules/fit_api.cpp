// Offline fitting adapter: reuses the checked score runner's generated-code UI.
// Not a realtime/host ABI. No hand-translated DSP equations are present here.
#define main module_score_cli_unused
#include "render.cpp"
#undef main
extern "C" int module_render(const double* values, int count, int rate, int frames, float* output) noexcept {
    try {
        if (!values || !output || rate<8000 || rate>96000 || frames<1 || frames>rate*8) return -1;
        auto s=std::make_unique<ModuleDSP>(); s->init(rate); UI ui; s->buildUserInterface(&ui);
        if (s->getNumInputs()!=0 || s->getNumOutputs()!=1 || count!=int(ui.zones.size())) return -2;
        int i=0;
        for (const auto& [name,z]:ui.zones) { double v=values[i++]; if (!std::isfinite(v)) return -3; ui.set(name,float(v)); }
        // Establish computed gate-off state. Fitting always starts a fresh instance.
        ui.set("gate",0.f); float warm[256] {}; float* wp[]={warm}; s->compute(256,nullptr,wp);
        ui.set("gate",1.f); float* op[]={output}; s->compute(1,nullptr,op); ui.set("gate",0.f);
        for (int n=1;n<frames;) { int k=std::min(128,frames-n); op[0]=output+n; s->compute(k,nullptr,op); n+=k; }
        for (int n=0;n<frames;++n) if (!std::isfinite(output[n])) return -4;
        return 0;
    } catch (...) { return -5; }
}
