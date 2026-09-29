// Research-only structured signal/partition export for pinned Faust 3d4baa1.
// This is not a stable Faust API and does not modify the partition/fusion oracle.
#pragma once
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include "exception.hh"

class PlanLLVMExporter {
    const SuperNodeGraph& sn;
    static std::string fail(const char* what) {
        throw faustexception(std::string("plan-llvm unsupported: ") + what);
    }
    template <class Range> static std::string ints(const Range& values) {
        std::ostringstream o; o << '['; bool first = true;
        for (int v : values) { if (!first) o << ','; first = false; o << v; }
        return o.str() + ']';
    }
    std::string history(Tree x, int d) const {
        if (d == 0) return expr(x);
        int m = sn.indexOf(x);
        if (d < 0 || d > 4096 || m < 0 || sn.maxDelayOf(x) < d)
            return fail("delay is not a bounded, owned materialized history");
        return "{\"op\":\"history\",\"member\":" + std::to_string(m) +
               ",\"delay\":" + std::to_string(d) + "}";
    }
    std::string expr(Tree t, bool root = false, int depth = 0) const {
        if (depth > 1024) return fail("expression nesting limit");
        const int m = sn.indexOf(t);
        if (!root && m >= 0)
            return "{\"op\":\"ref\",\"member\":" + std::to_string(m) + "}";
        int i, opcode; double r; Tree x, y;
        if (isSigInt(t, &i))
            return "{\"op\":\"int\",\"value\":" + std::to_string(i) + "}";
        if (isSigReal(t, &r)) {
            if (!std::isfinite(r)) return fail("non-finite constant");
            std::ostringstream o; o << std::setprecision(17) << std::scientific << r;
            return "{\"op\":\"real\",\"value\":" + o.str() + "}";
        }
        if (isSigInput(t, &i))
            return "{\"op\":\"input\",\"index\":" + std::to_string(i) + "}";
        if (isSigDelay1(t, x)) return history(x, 1);
        if (isSigDelay(t, x, y)) {
            if (!isSigInt(y, &i)) return fail("nonconstant delay");
            return history(x, i);
        }
        if (isSigFloatCast(t, x))
            return "{\"op\":\"float\",\"arg\":" + expr(x, false, depth + 1) + "}";
        if (isSigBinOp(t, &opcode, x, y)) {
            if (getCertifiedSigType(t)->nature() == kInt)
                return fail("runtime integer arithmetic is outside the float state ABI");
            const char* op = nullptr;
            switch (opcode) {
                case kAdd: op = "add"; break; case kSub: op = "sub"; break;
                case kMul: op = "mul"; break; case kDiv: op = "div"; break;
                default: return fail("binary operation outside arithmetic subset");
            }
            return std::string("{\"op\":\"") + op + "\",\"lhs\":" +
                   expr(x, false, depth + 1) + ",\"rhs\":" +
                   expr(y, false, depth + 1) + "}";
        }
        return fail("signal primitive (no implicit bypass or approximation)");
    }
public:
    explicit PlanLLVMExporter(const SuperNodeGraph& graph) : sn(graph) {}
    void write(Tree outputs, int ninputs, int noutputs, const char* path) const {
        // Assemble before publishing: an unsupported primitive never yields a
        // partial plan that could be mistaken for an executable success.
        std::ostringstream o;
        o << "{\"schema\":\"faust-plan-llvm-v1\",\"faust_commit\":\"3d4baa164c0dd31b7d147617ed84a626495148dd\","
          << "\"stage\":\"post-fusion-before-emission\",\"fusion_enabled\":"
          << (gGlobal->gLSFuse ? "true" : "false")
          << ",\"chunk_size\":" << gGlobal->gVecSize
          << ",\"inputs\":" << ninputs << ",\"outputs\":" << noutputs
          << ",\"members\":[";
        const auto& mat = sn.materialized();
        for (int m = 0; m < int(mat.size()); ++m) {
            if (m) o << ',';
            Tree d = SuperNodeGraph::defOf(mat[m]);
            // This special case is the same pure-ref rule as SN::build().
            bool root = !(d != mat[m] && sn.indexOf(d) >= 0);
            o << "{\"id\":" << m << ",\"block\":" << sn.blockOf(m)
              << ",\"max_delay\":" << sn.maxDelayOf(mat[m])
              << ",\"integer\":" << (getCertifiedSigType(mat[m])->nature() == kInt ? "true" : "false")
              << ",\"refs\":" << ints(sn.refs(m)) << ",\"refs0\":" << ints(sn.refs0(m))
              << ",\"expression\":" << expr(d, root) << '}';
        }
        o << "],\"blocks\":[";
        for (int b = 0; b < sn.blockCount(); ++b) {
            if (b) o << ',';
            o << "{\"id\":" << b << ",\"members\":" << ints(sn.blockMembers(b))
              << ",\"dependencies\":" << ints(sn.blockDeps(b))
              << ",\"inputs\":" << ints(sn.blockIns(b))
              << ",\"modeled_ops\":" << sn.opsEstimate(b) << '}';
        }
        o << "],\"output_expressions\":[";
        int count = 0;
        for (Tree l = outputs; isList(l); l = tl(l)) {
            if (count++) o << ',';
            o << expr(hd(l));
        }
        if (count != noutputs) fail("output count mismatch");
        o << "],\"output_tail_outside_partition\":true}\n";
        std::ofstream file(path);
        if (!file) fail("cannot open export path");
        file << o.str(); file.close();
        if (!file) fail("export write failure");
    }
};
