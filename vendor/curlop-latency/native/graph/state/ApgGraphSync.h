#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "graph/state/GraphPublicationTypes.h"
#include "modules/contract/ModuleTypes.h"  // ParamSchemaEntry — T-318 step 5a
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace curlop {

struct EngineSlot;
struct ApgBundle;  // audio/EngineSlot.h
namespace transport {
class PreparedAudioRenderer;
}

// Caller-supplied desired state of the APG. Manager owns this; C++ diffs and applies.
// Index is the authoritative module index (matches bytecode module index).
struct DesiredModule {
    int index = -1;
    std::string dslName;
    std::string nodeId;
    // T-480: per-instance physical voice ceiling (0 = registry default).
    // Sourced from GraphState.modules[].physicalVoices; drives the adapter
    // sub-graph's instance count + expanded channel contract.
    int physicalVoices = 0;
    ModuleProcessingMode processingMode = ModuleProcessingMode::Native;
    ModuleOversamplingFactor oversamplingFactor = ModuleOversamplingFactor::X1;
    std::uint32_t audioInputChannelIndex = 0;
    // B-253: stable dotted registry key (e.g. "core.fx.density"), sourced from
    // GraphState.modules[].lineageId. seedApgModuleSchema resolves the factory
    // schema by THIS (factoryModuleSchema), not the per-instance dslName — the
    // 2nd instance's dslName carries a dedup suffix ("density2") that matches no
    // factory entry, which dropped every declared param (silent + locks ignored).
    // Identity-first per MODULE-IDENTITY-SPEC.
    std::string lineageId;
    // Per-module persisted user knob values (paramName → value). Sourced
    // from GraphState.modules[].paramValues at apply time. When set, the
    // seed loop overrides the freshly-built ControlCore's schema default
    // and pushes the persisted value through SET_PARAM. Without this,
    // `set_param` BEFORE play_script gets clobbered when BYTE drains
    // because the fresh ControlCore carries only schema defaults.
    std::unordered_map<std::string, float> paramOverrides;
    // Per-instance source text. Currently consumed by core.faust_jit so its
    // saved Faust .dsp travels with the project state into FaustNode's
    // constructor (and, post hot-swap, into setSource). Empty = use the
    // module type's placeholder/default. Sourced from ModuleEntry::code,
    // populated by USER_ADD_MODULE / clip-state restore.
    std::string code;
    std::string bufferAssetPath;
    std::string midiInputRouteId;
    std::string midiOutputRouteId;
    // Named native source sockets are graph-state declarations. Renderer-only
    // publication carries them into the bundle because it has no APG wrapper
    // from which to recover the source lane names and signal types.
    std::vector<std::string> controlOutputs;
    std::vector<vm::SignalType> controlOutputTypes;
    // Script V2 CTRL_INPUT uses its source-declared input ordering as the
    // renderer/VM previous-row channel contract.
    std::vector<std::string> controlInputs;
    // T-318 step 5a: per-instance param schema. Sourced from
    // GraphState.modules[].params. Currently consumed by core.faust_jit —
    // when non-empty, the buildFaustJit dispatcher converts these entries
    // into ControlCore ParamDef[] instead of the placeholder {FREQ, GAIN}.
    // Empty = use module type default (i.e. placeholder schema for a
    // freshly added faust_jit before its first schemaChangedCallback fires).
    std::vector<ParamSchemaEntry> params;
};

struct DesiredGraphState {
    std::vector<DesiredModule> modules;
    std::vector<ApgEdge> edges;
    bool reset = false;  // true = clear()+rebuild (clip switch); false = incremental diff
    bool deferColdFaustCompile = false;
};

struct ApgGraphSyncResult {
    int modulesAdded = 0;
    int modulesRemoved = 0;
    int edgesAdded = 0;
    int edgesRemoved = 0;
    double buildClipApgUs = 0.0;
    double apgTopGraphInitUs = 0.0;
    double apgModuleBuildUs = 0.0;
    double apgEdgeWireUs = 0.0;
    double apgMeterTapUs = 0.0;
    double apgFinalRebuildUs = 0.0;
    double bundleAssembleUs = 0.0;
    double schemaSeedUs = 0.0;
    double publishUs = 0.0;
    double retireUs = 0.0;
    std::vector<ApgModuleBuildTiming> apgModuleBuildTimings;
    bool usedReset = false;
    bool valid = false;
    std::string diagnostic;
};

// Called once per new module core to seed paramBuffer with SET_PARAM commands.
// (moduleIdx, paramIdx, defaultValue). Caller wires this to CurlopProcessor::paramCmdQueue_
// (requires friend access). Pass an empty std::function to skip seeding (not recommended
// for live playback — CORE_LOOP will stomp new module params to 0 each block).
using ParamSeedFn = std::function<void(int moduleIdx, int paramIdx, float defaultValue)>;

// T-375 (D-APG-1): bundle-swap retire callback. Both reset and incremental
// paths build a fresh ApgBundle off-thread, atomically exchange it into
// the slot, and pass the displaced old bundle here for deferred deletion
// via the processor's retire queue (CurlopProcessor::retireApgBundle).
// The audio thread may still be mid-block on the old pointer; the
// safeAtSeq=+2 fence in the queue defers the actual delete until two
// processBlock cycles later. Pass an empty std::function for test paths
// (slot is single-threaded; the reset path falls back to inline delete).
using RetireGraphFn = std::function<void(ApgBundle*)>;

// Apply desired graph state to the slot's APG. Idempotent.
// Runs on message thread. Edge-only live rewires use JUCE's async render-
// sequence rebuild so playback keeps the previous sequence until the new one is
// ready.
//
// reset=true  → buildClipApg from scratch, publish a fresh ApgBundle atomically.
//               Caches (apgScriptNodes/apgMeters/apgNodeIDs/apgAudioOutNodeID),
//               layouts, ParamStore, keyToParamSlot, and bytecodeToGraphIdx
//               are rebuilt from the same desired state before publish.
//
// reset=false → if module set matches, reconcile only edges in the live bundle
//               without callback-lock rebuild. Otherwise falls back to reset.
//
// Cut 1: wired into EventRouter.cpp APG_GRAPH_STATE handler. This is the sole
// graph-mutation path — the legacy APG_ADD_MODULE / APG_REMOVE_MODULE / APG_EDGES
// EventRouter handlers were deleted (SF-053 / T-336, drift-map P1 / D-APG-2).
ApgGraphSyncResult applyGraphState(
    EngineSlot& slot,
    const DesiredGraphState& desired,
    double sampleRate,
    int blockSize,
    const juce::CriticalSection& callbackLock,
    const ParamSeedFn& seedParam = {},
    const RetireGraphFn& retireGraph = {},
    bool deferColdFaustCompile = false,
    double launchMasterBpm = 120.0,
    std::unique_ptr<transport::PreparedAudioRenderer> preparedAudioRenderer
        = {},
    std::string unifiedPublicationRejection = {}); // T-374 — reset path only

// T-375 (D-APG-1): rebuildSingleModule removed from the public surface.
// Faust-schema-change callers (EventRouter USER_SET_FAUST_SOURCE handler)
// now mutate the clip's authoritative GraphState (set code + params on
// the changed module) and call applyFromGraphState(reset=true) — same
// rebuild-and-swap path as a clip switch. The legacy seven-step in-place
// rewrite (capture-edges → removeNode → registerSingleModule → restore
// edges → re-apply overrides → graph.rebuild) is gone with the helpers
// it depended on.

} // namespace curlop
