// Serial semantic bridge: real upstream blocks -> LLVM signal factories.
// Stateful mode preserves local delays in each LLVM instance. Cross-kernel
// history is committed after all blocks and the output tail finish a subchunk.
// The same harness can run the earlier one-sample host-state semantic oracle.
// There is deliberately no worker pool, performance claim or live-state reload.
#include <faust/dsp/llvm-dsp.h>
#ifndef PLAN_READER
#include <faust/dsp/libfaust-signal.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

struct InputSpec { int kind, id, delay; };
struct RegionSpec { std::vector<InputSpec> inputs; std::vector<int> members; int outputs; bool tail; };
#include "plan_generated.hpp"

namespace fs = std::filesystem;
constexpr int frames = 16384;
constexpr float guard = 12345.5f;
constexpr float unwritten = std::numeric_limits<float>::quiet_NaN();
static_assert(sizeof(FAUSTFLOAT) == 4, "The research ABI currently requires float32 I/O");
using Samples = std::vector<FAUSTFLOAT>;
using Audio = std::vector<Samples>;
struct FactoryDelete { void operator()(llvm_dsp_factory* f) const { if (f) deleteDSPFactory(f); } };
using Factory = std::unique_ptr<llvm_dsp_factory, FactoryDelete>;
int source_compiles = 0, signal_compiles = 0, bitcode_reads = 0;
int smallest_kernel_call = 4096, largest_kernel_call = 0;
std::uint64_t kernel_calls = 0;

void require(bool test, const std::string& message) {
    if (!test) throw std::runtime_error(message);
}
std::string target() { return getDSPMachineTarget(); }

std::unique_ptr<dsp> instance(llvm_dsp_factory* f, int sr, int ni, int no) {
    std::unique_ptr<dsp> d(f->createDSPInstance());
    require(bool(d), "cannot create instance");
    require(d->getNumInputs() == ni && d->getNumOutputs() == no, "factory interface mismatch");
    d->init(sr);
    return d;
}

Audio stimulus() {
    Audio x(plan_inputs, Samples(frames));
    std::uint32_t seed = 0x91ad0457U;
    for (int n = 0; n < frames; ++n) {
        seed = 1664525U * seed + 1013904223U;
        const float noise = float(int(seed >> 9) - 4194304) * (1.0f / 33554432.0f);
        for (int ch = 0; ch < plan_inputs; ++ch) {
            float value = (ch == 0) ? noise : float(1 + ((n / (137 + ch * 2)) % 3)) * 0.25f;
            if (ch == 0 && (n == 64 || n == 1537 || n == 4099)) value += 0.5f;
            if (n < 64 || n >= frames - 1024) value = 0.0f;
            x[ch][n] = value;
        }
    }
    return x;
}

std::vector<int> partitions(int type) {
    static const int irregular[] = {1, 7, 63, 2, 129, 17, 511, 3, 64, 5, 257, 31, 128, 9, 513, 4};
    std::vector<int> result;
    for (int pos = 0, k = 0; pos < frames; ++k) {
        const int n = std::min(frames - pos, type == 0 ? 1 : type == 1 ? 128 : irregular[k % 16]);
        result.push_back(n); pos += n;
    }
    return result;
}

Audio whole(llvm_dsp_factory* f, const Audio& input, int sr, int type) {
    auto d = instance(f, sr, plan_inputs, plan_outputs);
    Audio result(plan_outputs, Samples(frames));
    int pos = 0;
    for (int n : partitions(type)) {
        Audio in(plan_inputs, Samples(n + 2, guard)), out(plan_outputs, Samples(n + 2, guard));
        std::vector<FAUSTFLOAT*> ip, op;
        for (int ch = 0; ch < plan_inputs; ++ch) {
            std::copy_n(input[ch].data() + pos, n, in[ch].data() + 1); ip.push_back(in[ch].data() + 1);
        }
        for (auto& c : out) { std::fill(c.begin() + 1, c.end() - 1, unwritten); op.push_back(c.data() + 1); }
        d->compute(n, ip.data(), op.data());
        for (int ch = 0; ch < plan_inputs; ++ch) {
            require(in[ch].front() == guard && in[ch].back() == guard, "reference input guard overwritten");
            require(std::equal(in[ch].begin() + 1, in[ch].end() - 1, input[ch].begin() + pos), "reference mutated input");
        }
        for (int ch = 0; ch < plan_outputs; ++ch) {
            require(out[ch].front() == guard && out[ch].back() == guard, "reference output guard overwritten");
            for (int i = 0; i < n; ++i) require(std::isfinite(out[ch][i + 1]), "reference unwritten/nonfinite sample");
            std::copy_n(out[ch].data() + 1, n, result[ch].data() + pos);
        }
        pos += n;
    }
    return result;
}

struct Kernel {
    std::unique_ptr<dsp> d;
    Audio in, out;
    std::vector<FAUSTFLOAT*> ip, op;
    Kernel(llvm_dsp_factory* f, int sr, const RegionSpec& r)
        : d(instance(f, sr, int(r.inputs.size()), r.outputs)),
          in(r.inputs.size(), Samples(plan_chunk_size + 2, guard)),
          out(r.outputs, Samples(plan_chunk_size + 2, guard)) {
        for (auto& c : in) ip.push_back(c.data() + 1);
        for (auto& c : out) op.push_back(c.data() + 1);
    }
};

Audio regions(const std::vector<Factory>& factories, const Audio& input, int sr, int type, int corrupt = 0) {
    // corrupt=1 resets state at host-block boundaries; corrupt=2 reads external
    // history at d+1. Both are actual renders required to violate the audio gate.
    const int nm = int(history_capacity.size());
    std::vector<Samples> history(nm);
    std::vector<int> head(nm, 0);
    for (int m = 0; m < nm; ++m) if (history_capacity[m] > 0)
        history[m].assign(history_capacity[m] + 1, 0.0f);
    std::vector<std::unique_ptr<Kernel>> kernels;
    for (std::size_t k = 0; k < region_specs.size(); ++k)
        kernels.push_back(std::make_unique<Kernel>(factories[k + 1].get(), sr, region_specs[k]));
    Audio result(plan_outputs, Samples(frames, unwritten));
    Audio current(nm, Samples(plan_chunk_size, unwritten));
    int pos = 0;
    for (int count : partitions(type)) {
        if (corrupt == 1) {
            for (auto& h : history) std::fill(h.begin(), h.end(), 0.0f);
            std::fill(head.begin(), head.end(), 0);
            for (auto& kernel : kernels) kernel->d->instanceClear();
        }
        for (int start = 0; start < count; ) {
            // The upstream planner certified its partition at this chunk size.
            // Splitting a host call is legal; increasing that size is NOT assumed.
            const int n = std::min(plan_chunk_size, count - start);
            for (auto& values : current) std::fill(values.begin(), values.end(), unwritten);
            for (std::size_t k = 0; k < region_specs.size(); ++k) {
                const auto& r = region_specs[k]; auto& kernel = *kernels[k];
                for (auto& c : kernel.in) std::fill(c.begin(), c.end(), guard);
                for (auto& c : kernel.out) {
                    std::fill(c.begin(), c.end(), guard);
                    std::fill_n(c.begin() + 1, n, unwritten);
                }
                for (std::size_t j = 0; j < r.inputs.size(); ++j) {
                    const auto& spec = r.inputs[j];
                    for (int i = 0; i < n; ++i) {
                        float v = unwritten;
                        if (spec.kind == 0) v = input.at(spec.id).at(pos + start + i);
                        else if (spec.kind == 1) v = current.at(spec.id).at(i);
                        else if (spec.kind == 2) {
                            const int d = spec.delay + (corrupt == 2 ? 1 : 0);
                            if (i >= d) {
                                // A short-delay producer must already have run
                                // in the upstream all-constrained-edge ordering.
                                v = current.at(spec.id).at(i - d);
                            } else {
                                const auto& h = history.at(spec.id);
                                const int behind = d - i;
                                require(!h.empty() && behind > 0 && behind <= int(h.size()), "history read out of range");
                                v = h[(head[spec.id] + int(h.size()) - behind) % h.size()];
                            }
                        }
                        require(std::isfinite(v), "upstream dependency not ready or invalid input");
                        kernel.in[j][i + 1] = v;
                    }
                }
                const Audio before = kernel.in;
                kernel.d->compute(n, kernel.ip.data(), kernel.op.data());
                ++kernel_calls;
                smallest_kernel_call = std::min(smallest_kernel_call, n);
                largest_kernel_call = std::max(largest_kernel_call, n);
                require(kernel.in == before, "kernel mutated input or input guards");
                for (int j = 0; j < r.outputs; ++j) {
                    const auto& c = kernel.out[j];
                    require(c.front() == guard && std::all_of(c.begin() + n + 1, c.end(),
                            [](float v) { return v == guard; }), "kernel output guard overwritten");
                    for (int i = 0; i < n; ++i) {
                        require(std::isfinite(c[i + 1]), "kernel output not written or nonfinite");
                        if (r.tail) result[j][pos + start + i] = c[i + 1];
                        else current.at(r.members.at(j))[i] = c[i + 1];
                    }
                }
            }
            // Commit only externally visible history, once per member/sample,
            // after all readers finish. Intra-region histories live in LLVM.
            for (int m = 0; m < nm; ++m) for (int i = 0; i < n; ++i) {
                require(std::isfinite(current[m][i]), "materialized member omitted");
                if (!history[m].empty()) {
                    history[m][head[m]] = current[m][i];
                    head[m] = (head[m] + 1) % int(history[m].size());
                }
            }
            start += n;
        }
        pos += count;
    }
    return result;
}

void write_audio(const fs::path& path, const Audio& audio) {
    std::ofstream file(path, std::ios::binary);
    require(bool(file), "cannot open capture");
    // channel-major, exact float32; no normalization, alignment or filtering.
    for (const auto& channel : audio)
        file.write(reinterpret_cast<const char*>(channel.data()), std::streamsize(channel.size() * sizeof(FAUSTFLOAT)));
    file.close(); require(bool(file), "capture write failed");
}

std::vector<Factory> load(const fs::path& cache) {
    std::vector<Factory> result;
    for (std::size_t i = 0; i <= region_specs.size(); ++i) {
        std::string error;
        auto* f = readDSPFactoryFromBitcodeFile((cache / (std::to_string(i) + ".bc")).string(), target(), error, -1);
        ++bitcode_reads;
        require(f != nullptr, "bitcode read failed: " + error);
        result.emplace_back(f);
    }
    return result;
}

#ifndef PLAN_READER
std::vector<Factory> prepare(const fs::path& cache) {
    const char* args[] = {"-scal"};
    std::string error;
    std::vector<Factory> result;
    ++source_compiles;
    auto* wf = createDSPFactoryFromString("plan_whole", whole_source, 1, args, target(), error, -1);
    require(wf != nullptr, "whole-source compilation failed: " + error);
    result.emplace_back(wf);
    for (std::size_t k = 0; k < region_specs.size(); ++k) {
        createLibContext();
        llvm_dsp_factory* f = nullptr;
        try {
            tvec signals = construct_region(int(k));
            ++signal_compiles;
            f = createDSPFactoryFromSignals("plan_region_" + std::to_string(k), signals, 1, args, target(), error, -1);
        } catch (...) { destroyLibContext(); throw; }
        destroyLibContext();
        require(f != nullptr, "region signal compilation failed: " + error);
        result.emplace_back(f);
    }
    fs::create_directories(cache);
    for (std::size_t i = 0; i < result.size(); ++i)
        require(writeDSPFactoryToBitcodeFile(result[i].get(), (cache / (std::to_string(i) + ".bc")).string()), "bitcode serialization failed");
    return result;
}
#endif

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: executable CACHE_DIRECTORY OUTPUT_DIRECTORY");
        const fs::path cache(argv[1]), output(argv[2]); fs::create_directories(output);
#ifdef PLAN_READER
        require(!fs::exists(cache / "fixture.dsp"), "reader unexpectedly has a source file");
        auto factories = load(cache);
        const char* mode = "read_only_fresh_process";
#else
        auto factories = prepare(cache);
        const char* mode = "source_and_signals";
#endif
        const Audio x = stimulus(); write_audio(output / "stimulus.f32", x);
        for (int sr : {44100, 48000, 96000}) for (int p : {0, 1, 2}) {
            const std::string suffix = "-" + std::to_string(sr) + "-p" + std::to_string(p) + ".f32";
            write_audio(output / ("whole" + suffix), whole(factories[0].get(), x, sr, p));
            write_audio(output / ("regions" + suffix), regions(factories, x, sr, p));
        }
#ifndef PLAN_READER
        write_audio(output / "negative-reset.f32", regions(factories, x, 48000, 1, 1));
        write_audio(output / "negative-delay.f32", regions(factories, x, 48000, 1, 2));
#endif
        std::ofstream report(output / "process.json");
        report << "{\"pid\":" << getpid() << ",\"mode\":\"" << mode
               << "\",\"plan_sha256\":\"" << plan_identity << "\",\"frames\":" << frames
               << ",\"source_compiles\":" << source_compiles << ",\"signal_compiles\":" << signal_compiles
               << ",\"bitcode_reads\":" << bitcode_reads << ",\"execution_abi\":\"" << execution_abi
               << "\",\"kernel_min_frames\":" << smallest_kernel_call << ",\"kernel_max_frames\":" << largest_kernel_call
               << ",\"kernel_calls\":" << kernel_calls << ",\"workers\":1,\"live_instance_state_reload\":false}\n";
        report.close(); require(bool(report), "process report write failed");
        std::cout << "completed mode=" << mode << " pid=" << getpid() << " plan=" << plan_identity << std::endl;
        // All instances have already died. Factory RAII precedes process exit.
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "checkpoint error: " << e.what() << std::endl;
        return 1;
    }
}
