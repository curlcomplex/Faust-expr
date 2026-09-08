// Prepare a scalar libfaust LLVM bitcode factory for one coarse independent region.
#include <faust/dsp/llvm-dsp.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static bool writeFile(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), std::streamsize(s.size()));
    return bool(f);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: coarse_region_prep <voice-offset> <voice-count> <output.bc>\n";
        return 2;
    }
    const int offset = std::atoi(argv[1]);
    const int voices = std::atoi(argv[2]);
    const std::string output = argv[3];
    if (offset < 0 || voices < 1 || voices > 1024) return 2;

    std::ostringstream src;
    src << "import(\"stdfaust.lib\");\n";
    src << "voice(i)=os.osc(70+(i+" << offset << ")*3.17)"
        << ":fi.lowpass(4,900+(i+" << offset << ")*7)"
        << ":fi.highpass(2,70+(i+" << offset << ")*2)"
        << ":fi.lowpass(4,1700+(i+" << offset << ")*5)"
        << ":fi.highpass(2,110+(i+" << offset << ")*3)"
        << ":fi.lowpass(4,2600+(i+" << offset << ")*4)"
        << ":fi.lowpass(4,3600+(i+" << offset << ")*3)"
        << ":fi.highpass(2,150+(i+" << offset << ")*2)"
        << ":fi.lowpass(4,5200+(i+" << offset << ")*2)"
        << ":*(0.01);\n";
    src << "bank=par(i," << voices << ",voice(i)):>_;\nprocess=bank,bank;\n";

    std::string error;
    auto* f = createDSPFactoryFromString("CoarseRegion" + std::to_string(offset) + "x" + std::to_string(voices),
                                          src.str(), 0, nullptr, getDSPMachineTarget(), error, -1);
    if (!f) {
        std::cerr << "factory_error=" << error << "\n";
        return 3;
    }
    const auto bc = writeDSPFactoryToBitcode(f);
    if (bc.empty() || !writeFile(output, bc)) return 4;
    std::cout << "offset=" << offset << "\nvoices=" << voices << "\nbitcode_bytes=" << bc.size()
              << "\noptions=" << f->getCompileOptions() << "\n";
    if (!deleteDSPFactory(f)) return 5;
    return 0;
}
