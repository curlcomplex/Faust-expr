#pragma once

// T-466 — map a module's DECLARED control inputs (CP2 capture: every Faust
// widget in declaration order, typed by faustControlInputOf) onto the slot's
// paramBuffer rows and the paramStore schema.
//
// The paramBuffer rows stay layout-addressed (type-ordinal receiver voice rows
// for note-typed inputs, indexOf(ALL_CAPS) for value inputs) — rows feed
// ZERO_GATES / HARD_RESET / realtime bus writes / the mvm tap and the
// compiler's lock-name mapping, so the row scheme is the transport, the
// DECLARATION is the contract. The n-th declared input of a note type maps to
// the n-th layout voice slot of that type; inputs past the layout's voice
// count are unrouted (-1).
//
// Header exists for its second consumer (DeclarationRowMapTests) per
// SPEC-008 — EngineSlot.cpp is the production caller.

#include <string>
#include <vector>

#include "control/vm/machine/Delivery.h"
#include "control/vm/core/VMConstNode.h"
#include "modules/backend/FaustUiCapture.h"   // faustLabelToParamName (SF-052: stub-glue-free path)

namespace curlop {

struct DeclarationRowMapping {
    std::vector<int> rowMap;         // declaration idx → paramBuffer row (-1 = unrouted)
    std::vector<int> knobSchemaIdx;  // declaration idx → paramStore schema idx (-1 = none)
    std::vector<std::string> bindNames;  // ALL_CAPS lane-binding name ("" for note-typed)
};

// SF-083 — the canonical lane/registration name for a per-voice value input
// named "<base>.v<n>" (as expandDeclarationVoices names it): "<NORM_BASE>_V<n>".
// Returns the base name + voice index by reference. Mirrors the GATE_V<n>
// convention so the compiler's per-voice laneParams + locks bind by name.
inline std::string perVoiceValueCanonical(const std::string& rawName,
                                          std::string* baseOut = nullptr,
                                          int* voiceOut = nullptr)
{
    std::string base = rawName;
    int vn = 0;
    const auto dot = rawName.rfind(".v");
    if (dot != std::string::npos) {
        base = rawName.substr(0, dot);
        for (size_t k = dot + 2; k < rawName.size()
             && rawName[k] >= '0' && rawName[k] <= '9'; ++k)
            vn = vn * 10 + (rawName[k] - '0');
    }
    bool baseLower = false;
    for (char c : base) if (c >= 'a' && c <= 'z') { baseLower = true; break; }
    const std::string norm = baseLower ? faustLabelToParamName(base) : base;
    if (baseOut)  *baseOut = base;
    if (voiceOut) *voiceOut = vn;
    return norm + "_V" + std::to_string(vn);
}

// SF-083 — mark the base declaration's value inputs that are per-voice for this
// clip (their ALL_CAPS name is in perVoiceAllCaps), BEFORE finalize, so
// expandDeclarationVoices expands them ×voices. Match by raw label or its
// normalised form. Note-typed inputs are already per-voice by type. Empty set =
// no-op (the shared default). Called by every declaration-build site so they
// expand identically (the T-597 convergence requirement extends to per-voice).
inline void markPerVoiceParams(vm::ModuleDeclaration& base,
                               const std::vector<std::string>& perVoiceAllCaps)
{
    if (perVoiceAllCaps.empty()) return;
    auto inSet = [&] (const std::string& nm) {
        for (const auto& p : perVoiceAllCaps) if (p == nm) return true;
        return false;
    };
    for (auto& in : base.inputs) {
        if (in.type != vm::SignalType::Value) continue;
        if (inSet(in.name)) { in.perVoice = true; continue; }
        bool hasLower = false;
        for (char c : in.name) if (c >= 'a' && c <= 'z') { hasLower = true; break; }
        if (hasLower && inSet(faustLabelToParamName(in.name))) in.perVoice = true;
    }
}

// T-597 — derive a ModuleLayout from a finalized declaration: row r == input r.
// (appendStandardModuleParams + finalizeModuleDeclaration live in Delivery.h
// as curlop::vm:: free functions — pure declaration ops next to
// expandDeclarationVoices, reachable from the adapter builders too.)
// paramIndexByName carries canonical note names
// (GATE_V<g>/PITCH_V<p>/VELOCITY_V<v>/VEL_V<v>),
// mono aliases (GATE/PITCH/OUT_GAIN/IN_GAIN), and value names in both raw
// (Faust label, e.g. "cutoff" for JIT) and ALL_CAPS-normalised ("CUTOFF") forms
// so every consumer's lookup resolves. rowType pins the note-vs-value contract.
inline void computeLayoutFromDeclaration(ModuleLayout& lay,
                                         const vm::ModuleDeclaration& decl)
{
    lay.paramIndexByName.clear();
    lay.rowType.assign(decl.inputs.size(), vm::SignalType::Value);
    int gateOrd = 0, pitchOrd = 0, velOrd = 0;
    for (size_t i = 0; i < decl.inputs.size(); ++i) {
        const auto& in = decl.inputs[i];
        const uint32_t row = static_cast<uint32_t>(i);
        lay.rowType[i] = in.type;
        switch (in.type) {
            case vm::SignalType::Gate:
                lay.paramIndexByName["GATE_V" + std::to_string(gateOrd)] = row;
                if (gateOrd == 0) lay.paramIndexByName["GATE"] = row;
                ++gateOrd;
                break;
            case vm::SignalType::Pitch:
                lay.paramIndexByName["PITCH_V" + std::to_string(pitchOrd)] = row;
                if (pitchOrd == 0) lay.paramIndexByName["PITCH"] = row;
                ++pitchOrd;
                break;
            case vm::SignalType::Velocity:
                lay.paramIndexByName["VELOCITY_V" + std::to_string(velOrd)] = row;
                lay.paramIndexByName["VEL_V" + std::to_string(velOrd)] = row;
                if (velOrd == 0) {
                    lay.paramIndexByName["VELOCITY"] = row;
                    lay.paramIndexByName["VEL"] = row;
                }
                ++velOrd;
                break;
            default: {
                if (in.perVoice) {
                    // SF-083 per-voice value input "<base>.v<n>" — register the
                    // canonical <NORM_BASE>_V<n>, with the base name aliasing v0
                    // (parallel to GATE_V<n>/GATE). The shared lane/schema bind
                    // by the base name; voice n resolves <NORM>_V<n>.
                    std::string base; int vn = 0;
                    const std::string canon = perVoiceValueCanonical(in.name, &base, &vn);
                    lay.paramIndexByName[canon] = row;
                    bool baseLower = false;
                    for (char c : base) if (c >= 'a' && c <= 'z') { baseLower = true; break; }
                    const std::string norm = baseLower ? faustLabelToParamName(base) : base;
                    if (vn == 0) {
                        lay.paramIndexByName[norm] = row;   // base alias → v0
                        lay.paramIndexByName[base] = row;   // raw base too
                    }
                    break;
                }
                // Shared value input — register the raw label, plus the ALL_CAPS
                // normalised form when the label carries lowercase (JIT raw
                // "cutoff" vs factory-normalised "CUTOFF"). Already-canonical
                // contract names (PAN, IN_GAIN_0, VOICES) carry no lowercase,
                // so we skip the mangling-prone normalise for them.
                lay.paramIndexByName[in.name] = row;
                bool hasLower = false;
                for (char c : in.name) if (c >= 'a' && c <= 'z') { hasLower = true; break; }
                if (hasLower) {
                    const std::string norm = faustLabelToParamName(in.name);
                    if (norm != in.name
                        && lay.paramIndexByName.find(norm) == lay.paramIndexByName.end())
                        lay.paramIndexByName[norm] = row;
                }
                break;
            }
        }
    }
    // Mono aliases for the contract gains.
    auto alias = [&] (const char* from, const char* to) {
        auto it = lay.paramIndexByName.find(from);
        if (it != lay.paramIndexByName.end()) lay.paramIndexByName[to] = it->second;
    };
    alias("OUT_GAIN_0", "OUT_GAIN");
    alias("IN_GAIN_0", "IN_GAIN");
    lay.totalParams = static_cast<uint32_t>(decl.inputs.size());
}

// T-597 — map a finalized declaration onto its (identity) paramBuffer rows.
// Since the layout is declaration-derived (computeLayoutFromDeclaration), row i
// IS declaration input i — rowMap is the identity. This still computes
// knobSchemaIdx (declaration idx → ParamStore schema slot) and bindNames
// (ALL_CAPS lane-binding name for value inputs, "" for note-typed) which the
// EngineSlot lane binding + the ControlCore bridge need.
//
// schemaIndexOf: int(const std::string& name) — -1 on miss. A JIT module keys
// its schema by the RAW faust label ("gain", "cutoff"); a factory module by the
// faustLabelToParamName-normalised form ("FORCE_ON"). We try the raw label
// first, then the normalised form (B-331 dead-effect-knob root cause), and
// dodge faustLabelToParamName's all-caps mangling on already-canonical names.
template <typename SchemaIndexOf>
inline DeclarationRowMapping mapDeclarationToLayout(
    const vm::ModuleDeclaration& decl,
    SchemaIndexOf&& schemaIndexOf)
{
    DeclarationRowMapping out;
    const size_t n = decl.inputs.size();
    out.rowMap.reserve(n);
    out.knobSchemaIdx.reserve(n);
    out.bindNames.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& in = decl.inputs[i];
        int schemaIdx = -1;
        std::string bindName;
        if (in.type == vm::SignalType::Value) {
            if (in.perVoice) {
                // SF-083: bind by the canonical <NORM_BASE>_V<n> so the
                // compiler's per-voice laneParam + per-voice lock route to this
                // voice's row (the shared base name would collapse all voices).
                bindName = perVoiceValueCanonical(in.name);
                schemaIdx = schemaIndexOf(bindName);
            } else {
                bindName = in.name;
                schemaIdx = schemaIndexOf(bindName);
                if (schemaIdx < 0) {
                    bool hasLower = false;
                    for (char c : in.name) if (c >= 'a' && c <= 'z') { hasLower = true; break; }
                    if (hasLower) {
                        const std::string norm = faustLabelToParamName(in.name);
                        if (norm != in.name) { bindName = norm; schemaIdx = schemaIndexOf(bindName); }
                    }
                }
            }
        }
        out.rowMap.push_back((int) i);          // T-597: identity (row == decl idx)
        out.knobSchemaIdx.push_back(schemaIdx);
        out.bindNames.push_back(std::move(bindName));
    }
    return out;
}

// Synthesized shape for modules WITHOUT a source declaration (native
// effects; core.faust_jit until its per-instance unification, T-318): a mono
// gate/pitch/velocity base + the declared value params by schema name, then
// finalized (voice-expand + standard contract params). Lives here (not
// EngineSlot.cpp) so the staging-time allocations stay out of the
// RT-SAFE-DECAP-INV-1 audited file set — buildModuleVms runs on the message
// thread, but the audit is per-file.
struct SynthesizedDeclaration {
    vm::ModuleDeclaration decl;
    DeclarationRowMapping mapping;
};

// ModuleParamsT: anything with `schema` — a vector of entries carrying
// .name/.min/.max/.defaultValue (ParamStore's module shape). nullptr ok.
template <typename ModuleParamsT>
inline SynthesizedDeclaration buildSynthesizedDeclaration(
    const ModuleLayout& layout, const ModuleParamsT* mp,
    bool includeNoteBus = true)
{
    vm::ModuleDeclaration base;
    if (includeNoteBus) {
        base.inputs.push_back({ "gate",     vm::SignalType::Gate,     0.0f, 1.0f, 0.0f, vm::InputUnit::Gate });
        base.inputs.push_back({ "pitch",    vm::SignalType::Pitch,    0.0f, 0.0f, 0.0f, vm::InputUnit::Hz });
        base.inputs.push_back({ "velocity", vm::SignalType::Velocity, 0.0f, 1.0f, 0.7f, vm::InputUnit::Linear });
    }
    if (mp != nullptr) {
        for (size_t p = 0; p < mp->schema.size(); ++p) {
            const auto& pd = mp->schema[p];
            // Bus + standard-contract params are added by finalize/expand —
            // never declared twice from a schema that already lists them.
            if ((includeNoteBus && (pd.name == "GATE" || pd.name == "PITCH"
                                    || pd.name == "VEL" || pd.name == "VELOCITY"))
                || pd.name == "PAN" || pd.name == "VOICES"
                || pd.name.rfind("IN_GAIN", 0) == 0 || pd.name.rfind("OUT_GAIN", 0) == 0)
                continue;
            base.inputs.push_back({ pd.name, vm::SignalType::Value,
                                    pd.min, pd.max, pd.defaultValue,
                                    vm::InputUnit::Linear });
        }
    }
    markPerVoiceParams(base, layout.perVoiceParams);   // SF-083
    SynthesizedDeclaration out;
    out.decl = vm::finalizeModuleDeclaration(
        base, layout.voices < 1 ? 1 : (int) layout.voices,
        (int) layout.inputs, (int) layout.outputs);
    out.mapping = mapDeclarationToLayout(out.decl,
        [&] (const std::string& nm) -> int {
            if (mp != nullptr)
                for (size_t p = 0; p < mp->schema.size(); ++p)
                    if (mp->schema[p].name == nm) return (int) p;
            return -1;
        });
    return out;
}

} // namespace curlop
