#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// ModuleTypes.h — C++-side module schema (UI concern, not pipeline).
//
// Introduced Cut 0.B-closure-B. Lets C++ own authoritative module schema
// (inputs/outputs/params with types, ranges, units, widget + layout hints)
// without touching ParamDescriptor — ParamDescriptor is the audio-pipeline
// struct (paramStore/ControlCore) and stays pipeline-pure.
//
// Populated via USER_REGISTER_MODULE (JS CORE_MODULES → C++ moduleRegistry_
// at startup) + USER_EDIT_MODULE (user-authored schema edits). Consumed by
// EventRouter::kUserAddModule for paramValues seed + BridgeMessageHandler
// MODULE_REGISTRY_STATE emit. Message-thread-only.
//
// [-r DUMB-VIEW-INV-1]
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>

#include "graph/transport/SignalDescriptor.h"
#include "modules/contract/ParameterValue.h"

namespace curlop {

enum class ParamType : uint8_t { Continuous, Integer, Boolean, Select, Display };
using Scale = param::Scale;

// ── ADR-008 ModuleContract declaration-surface enums (T-385) ──────────────
// Located here (schema layer) rather than in ModuleContract.h so the schema
// can directly carry them — the contract is a projection of the schema, so
// the schema owns the data. ModuleContract.h re-uses these via include.

// SPEC-014 §5 — input-port acceptance. Wire-time check in ApgClipBuilder
// reads source emit-shape × destination acceptance. Flag values pinned by
// SPEC-014 (do not renumber).
enum class ParamAcceptance : uint8_t {
    AudioOnly    = 0x1,  // accepts Samples only (DSP-rate signal flow)
    ControlValue = 0x2,  // accepts Set + Offset + Samples (the common case)
    ControlSet   = 0x4,  // accepts Set + Samples (rejects Offset) — rare
};

// SPEC-014 §6 — voice-steal policy at physical_voices ceiling. Substrate is
// uncapped (voice_idx uint32_t); steal_policy decides which voice renders
// when arrivals exceed physical_voices.
enum class StealPolicy : uint8_t {
    LastStolen,       // newest note steals the oldest still-sounding voice
    RoundRobin,       // rotate steals across voice slots
    OldestReleased,   // prefer to steal voices already in release stage
};

enum class ModuleProcessingMode : uint8_t {
    Native = 0,
    AutomaticMultiMono = 1,
};

// FF-032 Section 5: a discrete structural DSP quality choice.  It is not a
// parameter/automation domain; changing it replaces the prepared DSP node.
enum class ModuleOversamplingFactor : uint8_t {
    X1 = 1,
    X2 = 2,
    X4 = 4,
};

inline constexpr int moduleOversamplingMultiplier(ModuleOversamplingFactor factor)
{
    return static_cast<int>(factor);
}

// Host-sample latency of the selected Release 1 polyphase-IIR boundary. This
// is part of the prepared boundary contract, not an estimate of graph delay.
inline constexpr int moduleOversamplingBoundaryLatencySamples(ModuleOversamplingFactor factor)
{
    switch (factor) {
        case ModuleOversamplingFactor::X1: return 0;
        case ModuleOversamplingFactor::X2: return 4;
        case ModuleOversamplingFactor::X4: return 6;
    }
    return 0;
}

inline const char* moduleOversamplingFactorToString(ModuleOversamplingFactor factor)
{
    switch (factor) {
        case ModuleOversamplingFactor::X1: return "1x";
        case ModuleOversamplingFactor::X2: return "2x";
        case ModuleOversamplingFactor::X4: return "4x";
    }
    return "1x";
}

inline bool moduleOversamplingFactorFromString(const std::string& value,
                                                ModuleOversamplingFactor& factor)
{
    if (value.empty() || value == "1x") {
        factor = ModuleOversamplingFactor::X1;
        return true;
    }
    if (value == "2x") {
        factor = ModuleOversamplingFactor::X2;
        return true;
    }
    if (value == "4x") {
        factor = ModuleOversamplingFactor::X4;
        return true;
    }
    return false;
}

struct ModuleProcessingCapabilities {
    uint32_t version = 1;
    bool independentMono = false;
    // A source-author assertion that every physical voice owns independent DSP
    // state and may therefore be considered by the compiled renderer's
    // capability-gated voice-cohort path. It does not itself select workers.
    bool independentVoiceCohorts = false;
};

inline const char* moduleProcessingModeToString(ModuleProcessingMode mode)
{
    return mode == ModuleProcessingMode::AutomaticMultiMono
        ? "automatic-multi-mono"
        : "native";
}

inline bool moduleProcessingModeFromString(const std::string& value,
                                           ModuleProcessingMode& mode)
{
    if (value.empty() || value == "native") {
        mode = ModuleProcessingMode::Native;
        return true;
    }
    if (value == "automatic-multi-mono") {
        mode = ModuleProcessingMode::AutomaticMultiMono;
        return true;
    }
    return false;
}

struct ParamOption {
    std::string label;
    float       value = 0.0f;
};

struct ParamSchemaEntry {
    std::string sourceId;             // backend-stable id, e.g. Faust "/Group/Cutoff"
    std::string name;
    ParamType   type         = ParamType::Continuous;
    float       min          = 0.0f;
    float       max          = 1.0f;
    float       defaultValue = 0.0f;
    std::string unit;
    Scale       scale        = Scale::Linear;
    std::string widget;
    std::string tooltip;
    float       order        = -1.0f;
    std::vector<ParamOption> options;
    float       column       = -1.0f;
    float       row          = -1.0f;
    float       width        = 1.0f;
    float       height       = 1.0f;
    // ── FF-001 cut 1.1: 8 ui fields forwarded for native widget hydration ─
    std::string orientation;            // "horizontal" | "vertical" | ""
    bool        hidden       = false;
    bool        graphInput   = false;    // [curlop:input] exposes a graph socket.
    std::string mode;                   // "bipolar" | ""
    bool        interactive  = true;
    std::string accent;                 // "#RRGGBB" | ""
    float       size         = 1.0f;
    std::string skin;                   // authored control variant ("outline" | "")
    float       fontSize     = 0.0f;     // label font size; 0 = renderer default
    std::string labelPosition;          // top | bottom | left | right | hidden | ""
    std::string xParam;
    std::string yParam;
    // ── ADR-008 ModuleContract (T-385): per-port acceptance flag ──────────
    // ControlValue is the structural default for every native-C++ control
    // param (knob/MCP/seed all push Set/Offset/Samples). ControlSet is the
    // rare opt-in for params that must reject additive offsets. AudioOnly
    // is never set here — audio-rate inputs live in ModuleSchema::inputs,
    // not in params.
    ParamAcceptance acceptance = ParamAcceptance::ControlValue;

    param::ParameterDeclaration valueDeclaration() const
    {
        return { min, max, unit, scale };
    }
};

// T-833: one versioned transport declaration for every non-visual module
// socket. The legacy string arrays remain as compatibility projections, but
// this descriptor is the authority for width, ordering, rate and semantics.
struct SignalPortDeclaration {
    std::string name;
    transport::SignalDescriptor descriptor = transport::SignalDescriptor::legacyScalar();
};

struct ModuleSchema {
    std::string                    lineageId;           // dotted registry key — the identity model's `id`
    std::string                    lineageUuid;         // UUID v4 — stable identity (the model's `lineage_id`).
                                                        // Seeded for factory modules; empty for user-registered
                                                        // modules until Layer 4 assigns one. See
                                                        // .planning/specs/MODULE-IDENTITY-SPEC.md.
    std::string                    name;
    std::string                    moduleType;          // "synth" | "effect" | "script" — engine routing
    std::string                    flavour = "audio";   // "audio" | "control" — graph-primitive class (FF-020 W0).
                                                        // "audio": output channels carry audio. "control": output
                                                        // channels carry sequencer signals (gate/pitch/vel/CV).
    std::string                    category;            // "Synth" | "Effects" | "Drums" | "Utility" | "Debug" | "Script" — browser filter
    std::string                    tier = "sealed";     // "sealed" | "user" | "community" — origin/trust
    std::string                    description;         // free-form short description for browser
    std::vector<std::string>       inputs;
    std::vector<std::string>       outputs;
    int                            voices = 1;
    // ── ADR-008 ModuleContract (T-385): voice-steal policy ──────────────
    // `voices` is the physical_voices ceiling (substrate is uncapped per
    // SPEC-014 §6); stealPolicy decides who renders when voice arrivals
    // exceed it. LastStolen is the conventional analog-synth default and
    // matches what every existing factory module would do today (no module
    // currently implements voice stealing at all — they are mono).
    StealPolicy                    stealPolicy = StealPolicy::LastStolen;
    ModuleProcessingCapabilities   processingCapabilities;
    std::vector<ParamSchemaEntry>  params;
    std::vector<SignalPortDeclaration> signalInputs;
    std::vector<SignalPortDeclaration> signalOutputs;
};

// ── Shared JSON serialization helpers ─────────────────────────────────────
// Used by kUserRegisterModule (EventRouter) + MODULE_REGISTRY_STATE emit
// (BridgeMessageHandler). Inline so both TUs can link cleanly.

inline const char* paramTypeToStr(ParamType t)
{
    switch (t) {
        case ParamType::Integer:    return "integer";
        case ParamType::Boolean:    return "boolean";
        case ParamType::Select:     return "select";
        case ParamType::Display:    return "display";
        case ParamType::Continuous: default: return "continuous";
    }
}

inline const char* scaleToStr(Scale s)
{
    switch (s) {
        case Scale::Logarithmic: return "logarithmic";
        case Scale::Exponential: return "exponential";
        case Scale::Linear:
        default:                 return "linear";
    }
}

inline const char* paramAcceptanceToStr(ParamAcceptance a)
{
    switch (a) {
        case ParamAcceptance::AudioOnly:    return "audio_only";
        case ParamAcceptance::ControlSet:   return "control_set";
        case ParamAcceptance::ControlValue:
        default:                            return "control_value";
    }
}

inline std::string escapeJsonString(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    static constexpr char hex[] = "0123456789abcdef";
    for (char c : s) {
        const auto uc = static_cast<unsigned char>(c);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (uc < 0x20) {
                    out += "\\u00";
                    out += hex[(uc >> 4) & 0x0f];
                    out += hex[uc & 0x0f];
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

inline std::string paramSchemaEntryToJson(const ParamSchemaEntry& p)
{
    std::string s = "{";
    s += "\"sourceId\":\"" + escapeJsonString(p.sourceId) + "\",";
    s += "\"name\":\""   + escapeJsonString(p.name)   + "\",";
    s += "\"type\":\""   + std::string(paramTypeToStr(p.type)) + "\",";
    s += "\"min\":"      + std::to_string(p.min) + ",";
    s += "\"max\":"      + std::to_string(p.max) + ",";
    s += "\"default\":"  + std::to_string(p.defaultValue) + ",";
    s += "\"unit\":\""   + escapeJsonString(p.unit)   + "\",";
    s += "\"scale\":\""  + std::string(scaleToStr(p.scale))    + "\",";
    s += "\"widget\":\"" + escapeJsonString(p.widget) + "\",";
    s += "\"tooltip\":\"" + escapeJsonString(p.tooltip) + "\",";
    s += "\"order\":"    + std::to_string(p.order) + ",";
    s += "\"column\":"   + std::to_string(p.column) + ",";
    s += "\"row\":"      + std::to_string(p.row)    + ",";
    s += "\"width\":"    + std::to_string(p.width)  + ",";
    s += "\"height\":"   + std::to_string(p.height) + ",";
    // ── FF-001 cut 1.1: 8 ui fields forwarded for native widget hydration ─
    s += "\"orientation\":\"" + escapeJsonString(p.orientation) + "\",";
    s += "\"hidden\":"        + std::string(p.hidden ? "true" : "false") + ",";
    s += "\"graphInput\":"    + std::string(p.graphInput ? "true" : "false") + ",";
    s += "\"mode\":\""        + escapeJsonString(p.mode) + "\",";
    s += "\"interactive\":"   + std::string(p.interactive ? "true" : "false") + ",";
    s += "\"accent\":\""      + escapeJsonString(p.accent) + "\",";
    s += "\"size\":"          + std::to_string(p.size) + ",";
    s += "\"skin\":\""        + escapeJsonString(p.skin) + "\",";
    s += "\"fontSize\":"      + std::to_string(p.fontSize) + ",";
    s += "\"labelPosition\":\"" + escapeJsonString(p.labelPosition) + "\",";
    s += "\"x_param\":\""     + escapeJsonString(p.xParam) + "\",";
    s += "\"y_param\":\""     + escapeJsonString(p.yParam) + "\",";
    s += "\"acceptance\":\""  + std::string(paramAcceptanceToStr(p.acceptance)) + "\",";
    s += "\"options\":[";
    for (size_t i = 0; i < p.options.size(); ++i) {
        if (i > 0) s += ",";
        s += "{\"label\":\"" + escapeJsonString(p.options[i].label) + "\",";
        s += "\"value\":" + std::to_string(p.options[i].value) + "}";
    }
    s += "]";
    s += "}";
    return s;
}

// FF-001 cut 1.6: build the full MODULE_REGISTRY_STATE JSON envelope from
// a moduleRegistry map. Extracted from BridgeMessageHandler.cpp so the
// USER_REGISTER_MODULE handler in EventRouter can also emit, ensuring
// the registry ships before any GRAPH_STATE arrives; otherwise Web scene
// hydration can render empty stubs before the first BYTE drain.
inline bool hideFromModuleRegistryState(const std::string& lineageId)
{
    return lineageId != "core.faust_jit"
        && lineageId != "core.audio_input"
        && lineageId != "core.script"
        && lineageId != "core.script_v2"
        && lineageId != "core.stepseq"
        && lineageId != "core.trigger_seq"
        && lineageId != "core.pitch_seq"
        && lineageId != "core.transport_clock"
        && lineageId != "core.midi_in"
        && lineageId != "core.midi_out"
        && lineageId != "core.host_automation"
        && lineageId != "core.buffer"
        && lineageId != "core.visual_shader"
        && lineageId != "core.visual_output"
        && lineageId != "curlop.synth.bell"
        && lineageId != "faust.synth.djembe"
        && lineageId != "faust.test.tone"
        && lineageId != "faust.test.saw"
        && lineageId != "output";
}

inline bool isRetiredPrecompiledFaustLineage(const std::string& lineageId)
{
    return lineageId == "curlop.synth.bell"
        || lineageId == "faust.synth.djembe"
        || lineageId == "faust.test.tone"
        || lineageId == "faust.test.saw";
}

inline bool isTc1ExcludedFromAuthoring(const std::string& lineageId)
{
    return lineageId == "core.script"
        || lineageId == "core.mixer"
        || lineageId == "core.buffer"
        || isRetiredPrecompiledFaustLineage(lineageId);
}

inline const char* tc1ExclusionDiagnosticForLineage(
    const std::string& lineageId)
{
    return lineageId == "core.script"
        ? "Script V1 is retired; replace it with Script V2 before publishing this graph"
        : lineageId == "core.mixer"
        ? "Mixer is retired; remove it before publishing this graph"
        : lineageId == "core.buffer"
        ? "core.buffer is excluded from TC1; project state and media are preserved, but remove Buffer to publish audio until the post-TC1 host-media/Faust migration lands"
        : lineageId == "curlop.synth.bell"
            || lineageId == "faust.synth.djembe"
            || lineageId == "faust.test.tone"
            || lineageId == "faust.test.saw"
        ? "legacy precompiled Faust module is retired; remove it and use a source-backed Faust library module to publish audio"
        : "module is excluded from TC1";
}

// Runtime registration is intentionally broader than the user-facing module
// browser: these retired schemas must still load older projects, but must not
// be offered as new modules.
inline bool showInModuleBrowser(const std::string& lineageId)
{
    return ! hideFromModuleRegistryState(lineageId)
        && ! isTc1ExcludedFromAuthoring(lineageId)
        && lineageId != "core.trigger_seq"
        && lineageId != "core.pitch_seq";
}

inline bool isStepSequencerLineage(const std::string& lineageId)
{
    return lineageId == "core.stepseq"
        || lineageId == "core.trigger_seq"
        || lineageId == "core.pitch_seq";
}

inline std::vector<std::string> defaultExposedParamInputsForLineage(
    const std::string& lineageId)
{
    if (isStepSequencerLineage(lineageId))
        return { "RATE", "INC", "DEC", "RESET", "SELECT" };

    if (lineageId == "core.buffer")
        return { "PLAY_GATE", "REC_GATE", "POSITION", "RATE", "START", "END",
                 "LOOP", "INTERP", "GAIN", "FADE_IN_MS", "FADE_OUT_MS" };

    if (lineageId == "core.midi_out")
        return { "GATE", "PITCH", "VELOCITY", "CHANNEL", "CC", "CC_VALUE" };

    return {};
}

template <typename ModuleMap>
inline std::string buildModuleRegistryStateJson(const ModuleMap& moduleRegistry)
{
    std::string json = "{\"contractVersion\":1,\"type\":\"MODULE_REGISTRY_STATE\",\"modules\":[";
    bool first = true;
    for (const auto& kv : moduleRegistry) {
        const auto& schema = kv.second;
        if (hideFromModuleRegistryState(schema.lineageId)) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"lineageId\":\""  + escapeJsonString(schema.lineageId)   + "\","
                "\"name\":\""        + escapeJsonString(schema.name)        + "\","
                "\"moduleType\":\""  + escapeJsonString(schema.moduleType)  + "\","
                "\"flavour\":\""     + escapeJsonString(schema.flavour)     + "\","
                "\"category\":\""    + escapeJsonString(schema.category)    + "\","
                "\"tier\":\""        + escapeJsonString(schema.tier)        + "\","
                "\"description\":\"" + escapeJsonString(schema.description) + "\","
                "\"voices\":"        + std::to_string(schema.voices)        + ","
                "\"browserVisible\":" + std::string(showInModuleBrowser(schema.lineageId)
                                      ? "true" : "false") + ","
                "\"processingCapabilities\":{\"version\":"
                    + std::to_string(schema.processingCapabilities.version)
                    + ",\"independentMono\":"
                    + std::string(schema.processingCapabilities.independentMono
                                      ? "true" : "false")
                    + ",\"independentVoiceCohorts\":"
                    + std::string(schema.processingCapabilities.independentVoiceCohorts
                                      ? "true" : "false")
                    + "},"
                "\"inputs\":[";
        bool ifirst = true;
        for (const auto& in : schema.inputs) {
            if (!ifirst) json += ",";
            ifirst = false;
            json += "\"" + escapeJsonString(in) + "\"";
        }
        json += "],\"outputs\":[";
        bool ofirst = true;
        for (const auto& out : schema.outputs) {
            if (!ofirst) json += ",";
            ofirst = false;
            json += "\"" + escapeJsonString(out) + "\"";
        }
        json += "],\"params\":[";
        bool pfirst = true;
        for (const auto& p : schema.params) {
            if (!pfirst) json += ",";
            pfirst = false;
            json += paramSchemaEntryToJson(p);
        }
        json += "]}";
    }
    json += "]}";
    return json;
}

} // namespace curlop
