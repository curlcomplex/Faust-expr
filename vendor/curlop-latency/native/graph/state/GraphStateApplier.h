#pragma once

#include "graph/state/GraphState.h"
#include "graph/state/ApgGraphSync.h"

#include <unordered_map>
#include <cstdint>

namespace curlop {

struct EngineSlot;  // audio/EngineSlot.h

struct ModuleProcessingModeValidation {
    bool valid = true;
    std::string diagnostic;
};

// Prepared on the message thread from authoritative authored state. The audio
// thread only supplies the current master BPM to resolve the target-unit base;
// it never parses Script source or touches GraphState.
struct PreparedAuthoredParameterBinding {
    enum class RuntimeKind : std::uint8_t { Fixed, Time, Frequency };
    int moduleIndex = -1;
    std::string parameterName;
    param::ParameterDeclaration target;
    param::AuthoredValue authored;
    double stepDurationBeats = 1.0;
    double sourceBpmRatio = 1.0;
    RuntimeKind runtimeKind = RuntimeKind::Fixed;
    double fixedImplementationValue = 0.0;
    // Seconds are numeratorSeconds / currentBpm for tempo/script-relative
    // values. A zero numerator means the binding is fixed at its prepared
    // implementation value and needs no unit parsing in the callback.
    double numeratorSeconds = 0.0;
    double targetUnitsPerSecond = 1.0;
};

struct PreparedAuthoredParameterValue {
    bool ok = false;
    float value = 0.0f;
    const char* diagnostic = "";
};

struct AuthoredParameterResolution {
    bool valid = true;
    std::string diagnostic;
    std::unordered_map<int, std::unordered_map<std::string, float>> paramOverrides;
    std::vector<PreparedAuthoredParameterBinding> bindings;
};

// Resolves authored values against the exact persisted schema sourceId. Script
// timing is compiled from the identified source module on the message thread.
AuthoredParameterResolution resolveAuthoredParameterValues(
    const GraphState& state, double masterBpm);

// Allocation-free per-block evaluation of an already prepared binding.
PreparedAuthoredParameterValue resolvePreparedAuthoredParameterValue(
    const PreparedAuthoredParameterBinding& binding, double masterBpm);

// Pure structural preflight used by the processing-mode mutation boundary.
// It applies the same eligibility and incoming-width rules as buildClipApg
// without allocating or publishing an APG bundle.
ModuleProcessingModeValidation validateModuleProcessingModes(
    const GraphState& state);

// FF-032 Section 5: validate the persisted quality choice before an inactive
// clip commits it, and before an active clip attempts its candidate build.
ModuleProcessingModeValidation validateModuleOversamplingFactors(
    const GraphState& state);

// ═══════════════════════════════════════════════════════════════════════════
// GraphStateApplier — Cut 2 thin adapter.
//
// Translates the authoritative curlop::GraphState (per-clip, owned by
// CurlopProcessor::graphContainer_) into the DesiredGraphState shape
// consumed by ApgGraphSync::applyGraphState. Exists so USER_* handlers
// and the BYTE-handler insertion don't need to know DesiredGraphState
// internals.
//
// reset=true  → full rebuild on the slot (clip switch path).
// reset=false → incremental diff/apply (live edit — used sparingly; Cut 2
//               USER_* handlers do NOT touch slots, so reset=false callers
//               arrive only from future stops).
// ═══════════════════════════════════════════════════════════════════════════

ApgGraphSyncResult applyFromGraphState(
    EngineSlot& slot,
    const GraphState& state,
    bool reset,
    double sampleRate,
    int blockSize,
    const juce::CriticalSection& callbackLock,
    const ParamSeedFn& seedParam = {},
    const RetireGraphFn& retireGraph = {},
    bool deferColdFaustCompile = false,
    double launchMasterBpm = 120.0);   // T-374 — reset path only

// B-263 step 1 — push live smart-wire gain/pan/mute/solo to the slot's live
// wire ControlCores (setKnobPosition) WITHOUT a topology rebuild. Use this
// instead of applyFromGraphState for USER_SET_EDGE_PARAM: gain/pan/mute/solo
// only change effective wire gains, never topology, so no buildClipApg /
// bundle swap / bytecode reinstall is needed (that path cut the audio + retriggered
// the VM — the T-375 regression). No-op if the slot has no live bundle yet, or
// for edges whose wire isn't in the live graph (persisted value rides in on the
// next genuine build). Caller guards active-slot + clip match.
void applyLiveWireParams(EngineSlot& slot, const GraphState& state);

} // namespace curlop
