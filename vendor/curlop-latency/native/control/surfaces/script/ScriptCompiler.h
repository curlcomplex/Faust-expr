#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// ScriptCompiler.h — C++ DSL bytecode compiler.
//
// Consumes a ScriptAst produced by ScriptParser and emits the bytecode the
// SequencerVM executes.
//
// This is the canonical, authoritative bytecode emitter. compileClipNative
// (NativeCompileAdapter.cpp) runs ScriptParser + ScriptCompiler::compile and
// returns the envelope the runtime installs — its output IS load-bearing.
// The earlier cross-check oracle and the JS compiler it compared against
// were decommissioned (s373); there is no second authority. The full DSL
// opcode set ships — the historical "0.D-3a observational-only / unported-op"
// scope limit is long gone (DR-7, drift-map P4).
//
// [-r DUMB-VIEW-INV-1]
// ═══════════════════════════════════════════════════════════════════════════

#include "control/surfaces/script/ScriptParser.h"
#include "control/vm/core/VMConstNode.h"
#include "control/vm/machine/Signal.h"
#include "control/vm/machine/Machine.h"
#include <juce_core/juce_core.h>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace curlop::script {

struct ModuleLayoutEntry {
    juce::String         dslName;
    curlop::ModuleLayout layout;
};

// Per-module schema handed to the compiler by the oracle caller.
// Populated from proc_.moduleRegistry_ at USER_REQUEST_COMPILE time.
struct ModuleSchemaLite {
    int                       voices  = 1;
    // T-480: receiver steal policy (vm::Delivery::StealPolicy numeric) —
    // rides the layout envelope to EngineSlot::buildModuleVms.
    int                       stealPolicy = 0;
    int                       inputs  = 0;
    int                       outputs = 1;
    std::vector<juce::String> declaredParamNames;   // filtered: no display params
    // Cut 0.D-3c-pre: per-param min/max parallel to declaredParamNames.
    // Needed by T-053 CTRL_MIDI offset-mode (~midi with % args) to scale
    // % → param units. Matches JS paramSchemas meta shape `{min, max}`.
    struct ParamDesc {
        juce::String name;   // canonical (UPPERCASE) param name
        float        min = 0.0f;
        float        max = 1.0f;
        // T-468 — the param's real-world unit ("ms" | "s" | "" ...) from the
        // source-derived schema. Time-typed targets convert BARE lock values
        // as step time (Rule 6 universal); empty = raw param units as before.
        juce::String unit;
        // B-304 — the param's declared scale ("log"/"logarithmic" = curved).
        // Absolute lock values invert through the curve so they mean real
        // units after Delivery's Exp edge conversion; empty/linear = as before.
        juce::String scale;
    };
    std::vector<ParamDesc>    paramDescs;
};

struct CompileResult {
    std::vector<uint8_t>                            bytecode;
    std::vector<std::pair<juce::String, int>>       moduleIndices;   // insertion order
    std::vector<ModuleLayoutEntry>                  layouts;
    double                                          bpm         = 120.0;
    bool                                            bpmExplicit = false;  // true iff @bpm:<value> fired in source
    double                                          loopLength  = 0.0;
    uint32_t                                        globalSeed  = 42; // from @seed; threaded
                                                                       // to VMState.config.global_seed
                                                                       // (Phase 1 of runtime-VM
                                                                       // redesign)
    uint16_t                                        numGenerators = 0; // FF-020 W1 (T-278):
                                                                       // count from GeneratorRegistry.
                                                                       // Threaded into Program for
                                                                       // future per-instance sizing.
    std::vector<uint64_t>                           genKeys;         // T-279 slice-1
                                                                     // (SPEC-013 §5.5): parallel to
                                                                     // gen_id; genKeys[i] = stable
                                                                     // (gen_type, args_hash) key for
                                                                     // gen_id == i. Empty when
                                                                     // numGenerators == 0. Threaded
                                                                     // into Program::genKeys via
                                                                     // BytecodeMessage; slice-2 uses
                                                                     // it at install time to migrate
                                                                     // matching state across
                                                                     // live recompiles.
    juce::String                                    compileError;    // empty on success
};

struct CompileOptions {
    // Seeded from graphContainer_.forClip(clipId).modules() iteration order.
    std::vector<std::pair<juce::String, int>> moduleIndicesSeed;
    // Keyed by dslName.
    std::unordered_map<juce::String, ModuleSchemaLite> schemas;
    juce::String localOutputTarget;   // empty = do not emit owner-local output program
    std::vector<juce::String> inputSocketNames; // V2 `<name` input bus order
    double bpmOverride = -1.0;   // -1 = use @bpm from AST or default 120
    double seed        = 0.0;
};

// ═══════════════════════════════════════════════════════════════════════════
// F-066 Phase 5 (T-462) — machine-ISA emission.
//
// compileV2 projects the script into ONE vm::Program PER REFERENCED MODULE
// (the Machine is module-blind; emissions carry voice + lane only). Shared
// random verdicts across projections come from the roll-key contract: rolls
// key on (seed operand, iteration) and compileV2 stamps each source
// processor ONE unique seed, reused in every projection that emits it.
//
// Values on Value lanes are emitted in NORMALIZED wire units (±1, per
// adr-vm "How signals move"): absolute locks/gen ranges map through the
// module schema's [min,max] → ±1; `%` offsets map pct → fraction-of-range
// → ±2-span delta. Delivery converts back to real units at the edge.
//
// Generator routes are MODULATION (they sum over the knob base — adr-vm
// composition rule); the old CTRL_ROUTE_SET "replace the base" layer does
// not exist in the new model. `~midi` is a machine op like every other
// route (CTRL_MIDI — B-275): the host feeds CC values into the machine,
// the route renders span-clipped to its scope.
// Generator args to articulations / vel / arp-speed are a compile
// ERROR ("arrives with the operand-kind work" — B-186/B-187 disposition,
// Neo s489).
// ═══════════════════════════════════════════════════════════════════════════

struct ModuleProgramV2 {
    juce::String dslName;
    int          moduleIdx = -1;   // graph index (moduleIndices contract)
    int          minVoices = 1;    // V2 stack(n) can request a physical voice floor
    curlop::vm::Program program;
    bool         localOutputOwner = false;
    // lane → declared param: Delivery's LaneBinding source. Index in this
    // vector == the lane stamped into ParamLock / Ctrl* operands.
    std::vector<juce::String> laneParams;
    // Same index as laneParams. Internal lanes are VM scratch lanes for
    // dynamic expression arguments: they count for lane namespacing but do not
    // bind to module inputs or surface as user-addressable params.
    std::vector<uint8_t>       laneInternal;
    // SF-059 FOLLOW: source-script top-level step ordinal for the nth Step
    // instruction emitted into this program (parallel to emission order).
    std::vector<int>          stepSourceOrdinals;
    // SF-052 — ALL_CAPS base names of params this module voice-stacks ({}),
    // so the declaration build (markPerVoiceParams) expands their per-voice
    // rows. Empty = no per-voice params (shared default).
    std::vector<std::string>  perVoiceParams;
    struct LocalOutputDecl {
        juce::String name;
        curlop::vm::SignalType type = curlop::vm::SignalType::Value;
        float min = 0.0f;
        float max = 1.0f;
        float def = 0.0f;
    };
    std::vector<LocalOutputDecl> localOutputs;
};

// T-519 — a modulated per-script tempo: `bpm:~lfo(...)` makes the script's own
// clock (T-467) a continuously-evaluated value. The LFO is clocked by the MASTER
// tempo (CON-027: separate the modulator's clock from its target -> no feedback);
// the modulated value drives this script's sourceBpm per block.
struct BpmModulator {
    uint8_t  kind     = 0;     // 0 = none (fixed bpm), 1 = lfo
    uint8_t  waveform = 0;     // lfo: 0 sine 1 tri 2 saw 3 square (getLfoWaveformId)
    double   rateBeats = 1.0;  // master-synced: beats per cycle
    double   minBpm   = 120.0;
    double   maxBpm   = 120.0;
    bool     active() const { return kind != 0; }
};

// T-569 — a top-level (root-scope) `timescale:~gen` modulates the per-script
// TIME SCALE: a tempo MULTIPLIER on srcBpm, riding the per-script clock exactly
// like BpmModulator (master-clocked LFO; B-324's anchored beat clock keeps the
// per-block slope change smooth). min/max are MULTIPLIERS (1.0 = unchanged),
// not BPM — SPEC-018 §4.6 (effective rate = bpm × Πtimescale). Static
// (non-modulated) timescale keeps its compile-time loop-length path untouched.
struct TimescaleModulator {
    uint8_t  kind     = 0;     // 0 = none (static), 1 = lfo
    uint8_t  waveform = 0;     // 0 sine 1 tri 2 saw 3 square (getLfoWaveformId)
    double   rateBeats = 1.0;  // master-synced: beats per cycle
    double   minMul   = 1.0;
    double   maxMul   = 1.0;
    bool     active() const { return kind != 0; }
};

struct CompileResultV2 {
    std::vector<ModuleProgramV2>              modulePrograms;
    std::vector<std::pair<juce::String, int>> moduleIndices;  // insertion order
    double       bpm         = 120.0;
    bool         bpmExplicit = false;
    BpmModulator bpmMod;                                       // T-519 — empty = fixed bpm
    TimescaleModulator tsMod;                                  // T-569 — empty = static timescale
    double       loopLength  = 0.0;
    uint32_t     globalSeed  = 42;
    juce::String compileError;     // empty on success
};

CompileResultV2 compileV2(const ScriptAst& ast, const CompileOptions& opts = {});

} // namespace curlop::script
