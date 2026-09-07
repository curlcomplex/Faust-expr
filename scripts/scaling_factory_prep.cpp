// Prepare cached libfaust LLVM factories for the scheduler scaling benchmark.
#include <faust/dsp/llvm-dsp.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static bool writeFile(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc); f.write(s.data(), std::streamsize(s.size())); return bool(f);
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: scaling_factory_prep scalar|vec|sch <voices> <output.bc> <scheduler.ll-or-dash>\n";
        return 2;
    }
    const std::string mode = argv[1], output = argv[3], scheduler = argv[4];
    const int voices = std::atoi(argv[2]);
    if ((mode != "scalar" && mode != "vec" && mode != "sch") || voices < 8 || voices > 1024) return 2;
    std::ostringstream src;
    src << "import(\"stdfaust.lib\");\n"
        << "voice(i)=os.osc(70+i*3.17):fi.lowpass(2,1200+i*41):*(0.01);\n"
        << "bank=par(i," << voices << ",voice(i)):>_;\nprocess=bank,bank;\n";
    std::vector<std::string> optStorage;
    if (mode == "vec") optStorage = {"-vec", "-vs", "32"};
    if (mode == "sch") {
        if (scheduler == "-") return 2;
        optStorage = {"-sch", "-L", scheduler};
    }
    std::vector<const char*> opts;
    for (const auto& s : optStorage) opts.push_back(s.c_str());
    std::string error;
    auto* f = createDSPFactoryFromString("Scaling" + mode + std::to_string(voices), src.str(),
        int(opts.size()), opts.data(), getDSPMachineTarget(), error, -1);
    if (!f) { std::cerr << "factory_error=" << error << "\n"; return 3; }
    const auto bc = writeDSPFactoryToBitcode(f);
    if (bc.empty() || !writeFile(output, bc)) return 4;
    std::cout << "mode=" << mode << "\nvoices=" << voices << "\nbitcode_bytes=" << bc.size()
              << "\noptions=" << f->getCompileOptions() << "\n";
    if (!deleteDSPFactory(f)) return 5;
    return 0;
}
