#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// ModuleContract.h — ADR-0008 concrete declaration surface.
//
// ADR-0008 (Accepted s454) pinned the *model*: one contract for native C++,
// Faust JIT/interpreter, and VM bytecode. Its Scope clause
// explicitly deferred the concrete C++ surface to a follow-up task — that
// task is T-385, this is its deliverable.
//
// SCOPE — declarations + lifecycle only. Per ADR-0008 §Consequences: "the
// contract must not become a god-interface — declarations + lifecycle only,
// no scheduling / drawing / DSP behaviour." Two surfaces (§Decision):
//   1. DSP surface          — audio-thread processor (lives in native/dsp/*)
//   2. Declaration surface  — ports / params / voice policy / topology
//                             (THIS HEADER)
//
// WHAT THIS HEADER PROVIDES:
//
//   ParamAcceptance      — per SPEC-014 §5 — input-port acceptance flag
//                          { AudioOnly, ControlValue, ControlSet }
//   StealPolicy          — per SPEC-014 §6 — voice-steal at physical_voices
//                          ceiling (substrate is uncapped — receiver decides)
//   VoicePolicy          — { physical_voices, steal_policy } — module-author
//                          declaration; substrate delivers, receiver decides
//   LineageId            — juce::Uuid — the model's identity per
//                          MODULE-IDENTITY-SPEC (already wire-shipped as the
//                          schema's lineageUuid field; this is the type)
//   Flavour              — { Audio, Control } — manifesto's two flavours,
//                          declared per-module as a contract field (ADR-0008
//                          §Decision: "flavour is a declared property of the
//                          contract, not a separate type")
//   InputPortDecl        — { name, acceptance } — one entry per input port
//   ModuleContract       — pure-virtual interface every backend implements
//   NativeModuleContract — concrete projection from curlop::ModuleSchema
//                          (the existing factory schema registry, the
//                          D-MOD-1 single source of truth — see
//                          ApgClipBuilder_modules.h::schemaToParamDefs)
//
// WHAT THIS HEADER DOES NOT DO:
//   - No DSP behaviour. ModuleContract does not own a processor.
//   - No graph scheduling. ApgClipBuilder/AudioProcessorGraph still own that.
//   - No persistence. paramStore is unchanged.
//   - No GUI drawing. Faceplate renderer (ADR-0010) reads the contract; the
//     contract does not draw.
//
// CONSUMERS (current):
//   - T-382 (SPEC-014 §5 wire-time acceptance check) reads
//     inputPorts()[i].acceptance against the source emit-shape.
//   - GUI MODULE_REGISTRY_STATE emitters can read flavour / voicePolicy /
//     topologyVersion for browser + faceplate.
//
// CONSUMERS (future):
//   - SF-052 voice stacking reads voicePolicy.physical_voices to size per-
//     voice storage; reads steal_policy at note-on overflow.
//   - Faust JIT/interpreter and VM bytecode backends each ship their own
//     ModuleContract implementation. The interface is backend-agnostic by
//     design.
//
// D-MOD RESOLUTION:
//   - D-MOD-1 (param-set fragmentation): contract::inputPorts() projects
//     from the schema — single source.
//   - D-MOD-3 (module identity 3-keyed): contract::lineageId() returns
//     the schema's lineageUuid as a typed juce::Uuid — single source.
//   - P4 (module-spec gap): this header + ADR-0008 + ADR-0010 are the
//     governing spec for the module declaration layer.
// ═══════════════════════════════════════════════════════════════════════════

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "modules/contract/ModuleTypes.h"

namespace curlop {

// ── ParamAcceptance and StealPolicy ────────────────────────────────────────
// These two enums live in ModuleTypes.h (the schema layer owns the data;
// this header is the projection). Re-exposed here so consumers including
// only ModuleContract.h see them in scope without an extra include.
// (No re-definition — the using-declarations bind to ModuleTypes.h.)
using curlop::ParamAcceptance;
using curlop::StealPolicy;

// ── Voice policy ──────────────────────────────────────────────────────────
// SPEC-014 §6 — substrate has no voice cap (voice_idx is uint32_t, per-voice
// storage grows via T-374/T-375 atomic-handoff). A module's *physical*
// polyphony is a module-author declaration; steal_policy decides who renders
// when voice_idx arrivals exceed physical_voices.
struct VoicePolicy {
    std::uint32_t physical_voices = 1;
    StealPolicy   steal_policy    = StealPolicy::LastStolen;
};

// ── Identity ──────────────────────────────────────────────────────────────
// MODULE-IDENTITY-SPEC: lineage_id is a UUID v4, stable across rename / move.
// The schema's lineageUuid (std::string of the dashed form) is the wire
// shape; LineageId is the typed in-process form.
using LineageId = juce::Uuid;

// ── Flavour (manifesto axis) ──────────────────────────────────────────────
// Per the manifesto: two flavours (audio / control). Soft categories per
// ADR-0008 §Decision — a declared property of the contract, not a separate
// type. core.script and other control-data hosts return Flavour::Control;
// every DSP module returns Flavour::Audio.
enum class Flavour : std::uint8_t {
    Audio,
    Control,
};

// ── Input port declaration ────────────────────────────────────────────────
// One entry per input port — covers both audio bus inputs and control-param
// inputs. The wire-time check (T-382) reads `acceptance`.
struct InputPortDecl {
    std::string     name;
    ParamAcceptance acceptance = ParamAcceptance::ControlValue;
};

// ── Output port declaration (F-075) ─────────────────────────────────────────
// One entry per output socket on the module's right edge. The audio bus is the
// "OUT" port (isControl=false); a module that emits internal control data
// (LFO / envelope / derived value) declares one additional port per control
// output (isControl=true), carried on a sample-rate signal channel. The canvas
// reads these to draw right-edge sockets; the graph addresses an edge's source
// by port name (superseding the hardcoded "OUT"). min/max give a control
// output's range (for meters / display scaling); they are unit-range for audio.
struct OutputPortDecl {
    std::string name;               // "OUT" audio bus, or a control-output name
    bool        isControl = false;  // true = control-data channel (sample-rate)
    float       min = 0.0f;         // control range (meaningful when isControl)
    float       max = 1.0f;
};

// ══════════════════════════════════════════════════════════════════════════
// ModuleContract — the abstract declaration surface.
// ══════════════════════════════════════════════════════════════════════════
//
// Pure-virtual: every backend ships its own implementation. The graph and
// the GUI consume ONLY this interface — they do not know the backend type.
//
// Lifecycle: topology_version is monotonic. When a JIT recompile changes
// the declaration surface (ports / params / flavour), the implementation
// bumps the version; consumers re-read declarations only when they observe
// a higher value. FaustNode's schema-changed callback is the prototype.
//
// Thread model: declarations are message-thread-owned. topology_version
// load/store is atomic so the audio thread can cheaply detect a change
// (e.g. to invalidate cached per-port routing) without locking.
class ModuleContract
{
public:
    virtual ~ModuleContract() = default;

    virtual LineageId                       lineageId()       const = 0;
    virtual juce::String                    displayName()     const = 0;
    virtual const std::vector<InputPortDecl>& inputPorts()    const = 0;
    virtual VoicePolicy                     voicePolicy()     const = 0;
    virtual Flavour                         flavour()         const = 0;
    virtual std::uint64_t                   topologyVersion() const = 0;
    virtual ModuleProcessingCapabilities processingCapabilities() const
    {
        return {};
    }

    // F-075: output sockets. Default = a single audio "OUT" bus (every existing
    // backend). A module that emits control data (Faust JIT with [curlop:cvout]
    // channels) overrides this to append one control output port per channel.
    // Non-pure so existing implementations need no change.
    virtual const std::vector<OutputPortDecl>& outputPorts() const
    {
        static const std::vector<OutputPortDecl> audioOnly{ { "OUT", false, 0.0f, 1.0f } };
        return audioOnly;
    }
};

// ══════════════════════════════════════════════════════════════════════════
// NativeModuleContract — projects a curlop::ModuleSchema into a contract.
// ══════════════════════════════════════════════════════════════════════════
//
// Native-C++ backends (the 29 factory modules in ModuleRegistryGen.h) all
// share a single declaration source: the curlop::ModuleSchema populated by
// populateFactoryModuleRegistry. This class is the projection — it takes a
// schema, computes the contract surface once, and serves it.
//
// Why a separate type rather than putting the contract on the processor?
//   - Separation of surfaces (ADR-0008 §Decision). The audio-thread
//     processor is the DSP surface; this is the declaration surface.
//     Coupling them would re-create the entanglement the contract exists
//     to dissolve.
//   - One projection rule for all 29 modules: audio bus inputs project as
//     AudioOnly (their channels carry sample-stream data by definition);
//     control params project with their schema-declared acceptance. The
//     rule is the same for every native-C++ module — encoding it once here
//     avoids 29 hand-maintained per-module declarations (the D-MOD-1
//     fragmentation pattern).
//
// Topology version: starts at 1 (so consumers can distinguish "never
// observed" = 0 from "first declaration" = 1). bumpTopologyVersion()
// increments atomically; intended for the hot-swap case (e.g. when a
// user-authored ModuleSchema is re-registered with a new param set).
// Native-C++ schemas are immutable at construction in practice, so most
// contracts hold at version 1 for their lifetime.
class NativeModuleContract final : public ModuleContract
{
public:
    explicit NativeModuleContract(const ModuleSchema& schema)
        : displayName_(schema.name)
        , lineageId_(juce::Uuid(juce::String(schema.lineageUuid)))
        , voicePolicy_{ static_cast<std::uint32_t>(schema.voices > 0 ? schema.voices : 1),
                        schema.stealPolicy }
        , processingCapabilities_(schema.processingCapabilities)
        , flavour_(schema.flavour == "control" ? Flavour::Control : Flavour::Audio)
        , topology_(1)
    {
        // Project audio bus inputs first (always AudioOnly — structural).
        ports_.reserve(schema.inputs.size() + schema.params.size());
        for (const auto& in : schema.inputs) {
            ports_.push_back({ in, ParamAcceptance::AudioOnly });
        }
        // Then control params (acceptance per schema). ParamType::Display
        // entries are GUI visualisation widgets (XY pads / FFT / goniometer)
        // — they bind to display state, not signal inputs, so they are not
        // input ports per the contract. This matches the schemaToParamDefs
        // filter in graph/ApgClipBuilder_modules.h: Display entries are
        // excluded from the ControlCore set and never receive a port.
        for (const auto& p : schema.params) {
            if (p.type == ParamType::Display) continue;
            ports_.push_back({ p.name, p.acceptance });
        }
    }

    LineageId                       lineageId()       const override { return lineageId_; }
    juce::String                    displayName()     const override { return displayName_; }
    const std::vector<InputPortDecl>& inputPorts()    const override { return ports_; }
    VoicePolicy                     voicePolicy()     const override { return voicePolicy_; }
    Flavour                         flavour()         const override { return flavour_; }
    ModuleProcessingCapabilities processingCapabilities() const override
    {
        return processingCapabilities_;
    }
    std::uint64_t                   topologyVersion() const override { return topology_.load(std::memory_order_acquire); }

    // Message-thread-only: bump when the declaration surface changes
    // (re-registration with a new param set, etc.). The next consumer that
    // observes a higher value re-reads declarations.
    void bumpTopologyVersion() { topology_.fetch_add(1, std::memory_order_release); }

private:
    juce::String                  displayName_;
    LineageId                     lineageId_;
    VoicePolicy                   voicePolicy_;
    ModuleProcessingCapabilities processingCapabilities_;
    Flavour                       flavour_;
    std::vector<InputPortDecl>    ports_;
    std::atomic<std::uint64_t>    topology_;
};

} // namespace curlop
