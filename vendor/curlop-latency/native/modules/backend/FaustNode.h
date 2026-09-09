#pragma once
//
// FaustNode (renamed from FaustJitNode, T-405 / SF-058 P2) — DspNode
// wrapping a libfaust-compiled `.dsp` program; factories come from the
// shared FaustRuntime (JIT today; the interpreter backend lands via the
// same runtime in P3).
//
// Originally: DspNode wrapping a libfaust LLVM-JIT-compiled `.dsp`
// program. Phase 1 (s438): construction-time compile from a source string;
// fixed (3 + N) input channel layout matching FaustSynthAdapter (gate/pitch/
// velocity + param cores); stereo output via deinterleave→compute→interleave.
//
// Header is pimpl-only: libfaust types (`UI`, `dsp`, `llvm_dsp*`) collide
// with the local stubs in audio/faust/Faust{Djembe,Bell}Adapter.h if both
// land in the same TU. Keeping libfaust includes confined to FaustNode.cpp
// keeps the existing pre-compiled-Faust adapters working unmodified.
//
// Phase 2 (deferred — handover): atomic source-string hot-swap, editor-window
// triggered recompile, per-instance schema regen, state persistence. The
// node shape here is the substrate Phase 2 wraps with double-buffered
// factories + atomic pointer swap (analog of AtomicProgramSwap in
// audio/SequencerVM.h:19).
//
#include "modules/backend/CurlopDspNode.h"
#include "modules/backend/FaustUiCapture.h" // ControlOutputDecl — F-075 socket model (libfaust-free)
#include "modules/contract/ModuleContract.h" // ADR-0008 declaration surface (T-405)
#include "modules/contract/ModuleTypes.h"   // ParamSchemaEntry — faceplate schema (T-318)
#include "modules/contract/FaceplateTypes.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace curlop {

namespace transport { class RealtimeWorkerTeam; }

class FaustRuntime; // shared factory cache + backend policy (T-404, SF-058 P1)

class FaustNode : public DspNode
{
public:
    // paramNames must match the source's slider labels in declaration order.
    // T-564 (adr-modules §"How modules receive control"): the input bus has
    // one channel per DECLARED control input — channel i writes the zone
    // captured for paramNames[i]. No reserved gate/pitch/vel slots; gate/
    // freq/gain are ordinary declared inputs on their declaration-ordinal
    // channel, typed by name at the VM delivery seam.
    //
    // T-405 (SF-058 P2): the node no longer compiles inline — it acquires
    // its factory from `runtime` (default: the app-wide
    // FaustRuntime::instance()), so N instances of one program share one
    // compile and the P6 warm cache serves clip switches. Tests inject
    // their own runtime for isolation.
    // voices (T-617): per-instance voice count — the node renders one DSP
    // instance per voice from ONE shared factory, raw-summed (adr-modules
    // "one Faust module type"; gate/pitch expand per voice, value inputs
    // shared). 1 = mono. build_faust_jit threads the clip/schema voice count.
    // perVoiceParams (SF-052): ALL_CAPS names of value inputs this clip drives
    // PER-VOICE (a {} voice-stack / per-voice modulator). They expand ×voices
    // into their own channels like gate/pitch; absent value inputs stay shared.
    // Must equal the VM-side set (slot.layouts[].perVoiceParams) so the feeding
    // ScriptNode and this node agree channel-for-channel.
    // automaticMultiMonoLanes > 1 is the explicit prepared wrapper width for
    // an author-declared independent mono processor. It is distinct from
    // physical voice polyphony: controls are shared, DSP state is not.
    FaustNode(const char* name,
                 const std::string& source,
                 const std::vector<std::string>& paramNames,
                 FaustRuntime* runtime = nullptr,
                 int voices = 1,
                 const std::vector<std::string>& perVoiceParams = {},
                 bool deferColdCompile = false,
                 int automaticMultiMonoLanes = 1,
                 ModuleOversamplingFactor oversamplingFactor =
                     ModuleOversamplingFactor::X1);

    ~FaustNode() override;

    const juce::String getName() const override;

    void prepareToPlay(double sr, int blockSize) override;
    void reset() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    struct PrepareTiming {
        double totalUs = 0.0;
        double controlStateUs = 0.0;
        double sampleRateInitUs = 0.0;
        double scratchUs = 0.0;
    };

    PrepareTiming lastPrepareTiming() const;

    struct ConstructionTiming {
        double totalUs = 0.0;
        double factoryAcquireUs = 0.0;
        double compileLockWaitUs = 0.0;
        double instanceCreateInitUs = 0.0;
        double instanceCreateUs = 0.0;
        double instanceInitUs = 0.0;
        double uiZoneResolveUs = 0.0;
        double ioControlOutputUs = 0.0;
        double inputContractUs = 0.0;
        double schemaUs = 0.0;
        double faceplateUs = 0.0;
        double debugSchemaUs = 0.0;
    };

    ConstructionTiming lastConstructionTiming() const;

    // Default placeholder source: continuous stereo sine + GAIN — audible
    // on add, two sliders so the faceplate renders something. Per-instance
    // source from persisted state / editor edits supersedes this.
    static const char* placeholderSource();

    // Async hot-swap. Message- (or any non-audio-) thread call. Schedules a
    // libfaust recompile on the internal worker; on success, atomically
    // swaps the factory+instance pointer that processBlock reads. The
    // previous program is held in a single-slot graveyard until the *next*
    // successful swap, guaranteeing the audio thread has moved past its
    // last load() of it before delete (mirror of AtomicProgramSwap in
    // SequencerVM.h:19 — same acquire/release/exchange pattern). On
    // compile failure the previous factory stays live and lastCompileError()
    // returns the libfaust diagnostic.
    void setSource(const std::string& source);

    // Host-validated live-authoring path. The host has already compiled this
    // exact source and accepted `validatedSchema` as name-stable with the
    // current ControlCore contract. The worker still recompiles/acquires the
    // program and independently rejects audio/control-output arity changes,
    // but compares the result with this validated schema rather than a stale
    // installed presentation schema. This prevents a range/widget-only edit
    // from being misclassified as a structural rebuild after the host has
    // already persisted it.
    void setSource(const std::string& source,
                   const std::vector<ParamSchemaEntry>& validatedSchema);

    // Returns the last libfaust diagnostic (empty when the last compile
    // succeeded). Cleared at the start of each compile attempt. Safe to
    // call from the message thread.
    std::string lastCompileError() const;

    // True until the worker has serviced the most recent setSource() call.
    bool isCompilePending() const;

    // T-572: the active program's audio I/O arity, straight from the compiled
    // Faust `dsp` (getNumInputs/getNumOutputs). numAudioInputs() > 0 ⇒ this
    // source is an EFFECT — its `process` consumes upstream audio, delivered
    // on input channels [numParams .. numParams+numAudioInputs-1] (the control
    // channels occupy [0 .. numParams-1], T-564). A synth reports 0. The
    // sub-graph builder reads this to wire an audioInputNode and to set the
    // module's outer audio-input bus (hasInput). 0 if no active program.
    int numAudioInputs() const;
    // F-075: the REAL source-declared audio output channel count —
    // `getNumOutputs()` minus trailing cvout and meterout channels.
    int numAudioOutputs() const;

    // F-075: the module's declared control OUTPUTS — named, sample-rate signal
    // channels emitted on the trailing output channels of `process` (the
    // channels after the numAudioOutputs() audio channels), declared by
    // `[curlop:cvout]`-tagged bargraphs via the cv() helper. Order = Faust
    // declaration order = trailing-channel order. Empty when none declared.
    int numControlOutputs() const;
    std::vector<ControlOutputDecl> controlOutputs() const;

    ModuleOversamplingFactor oversamplingFactor() const noexcept;
    // The first production boundary is intentionally audio-only: modules
    // exposing control-output ports or automatic multi-mono processors need a
    // separately specified rate crossing before they can opt in.
    bool isOversamplingEligible() const noexcept;
    double effectiveSampleRate(double hostSampleRate) const noexcept;
    int oversamplingLatencySamples() const noexcept;
    // Smoothed fraction of the most recent host block period spent inside the
    // prepared upsample -> Faust DSP -> downsample boundary. It is a measured
    // boundary load, not a claim about whole-graph or whole-core CPU load.
    float oversamplingBoundaryLoad() const noexcept;
    bool hasOversamplingBoundaryLoadMeasurement() const noexcept;

    // A1R.5: source capability plus the exact runtime shape required before a
    // compiled plan may offer this node voice cohorts. Worker selection and
    // execution remain plan-owned; this query deliberately makes no claim
    // about current worker participation.
    bool isVoiceCohortEligible() const noexcept;
    // Called only by the owning compiled render plan before that immutable plan
    // is published. The team is shared with the plan; a node never creates a
    // per-module thread pool.
    void setVoiceCohortWorkerTeam(transport::RealtimeWorkerTeam* team) noexcept;

    // T-318: synthesised faceplate schema for the currently-active program,
    // in Faust slider declaration order. Empty if construction-time compile
    // failed and no later setSource has succeeded. Safe to call from the
    // message thread.
    std::vector<ParamSchemaEntry> currentSchema() const;

    std::vector<std::pair<std::string, float>> faceplateMeterValues() const;

    // Diagnostic/readback for graph-driven param sockets: last real-unit value
    // written from the control-input bus into the named Faust zone.
    float lastControlInputValue(const std::string& paramName) const;

    // T-318 strategic fix: synchronously compile `source` JUST to extract
    // its faceplate schema. No FaustNode instance is created or kept
    // alive. Used by USER_SET_FAUST_SOURCE / USER_ADD_MODULE handlers and
    // by the clip-switch backfill so `GraphState.module.params` is always
    // authoritative-derived from the source at write time — the runtime
    // schemaChangedFn callback then reverts to a safety-net role rather
    // than the primary persistence mechanism. On compile failure `errOut`
    // is populated and the returned vector is empty. Message-thread only
    // (libfaust compile is bounded but not RT-safe).
    //
    // T-405: routes through `runtime` (default FaustRuntime::instance()) —
    // the compiled factory lands in the cache, so the node build that
    // typically follows a schema probe cache-hits instead of recompiling.
    static std::vector<ParamSchemaEntry> compileSchemaOnly(const std::string& source,
                                                           std::string& errOut,
                                                           FaustRuntime* runtime = nullptr,
                                                           bool liveAuthoring = false);

    // F-075: compile `source` JUST to read its declared control outputs (the
    // [curlop:cvout] bargraph markers), in declaration order. Used at
    // construction to size the output bus (source-declared audio + one channel
    // per control output) before the body compile runs. Routes through `runtime`
    // (default FaustRuntime::instance()), so the compiled factory lands in the
    // cache and the node's own compileSync that follows is a cache hit — net
    // cost is one compile, not two. Empty on compile failure or when none
    // declared. Message-thread only (libfaust compile is bounded, not RT-safe).
    static std::vector<ControlOutputDecl> probeControlOutputs(const std::string& source,
                                                             FaustRuntime* runtime = nullptr);

    // Exact compiled Faust audio channel counts. Control-output bargraphs are
    // excluded from the output count. Message/background thread only.
    static std::pair<int, int> probeAudioIo(const std::string& source,
                                            std::string& errOut,
                                            FaustRuntime* runtime = nullptr);

    // GUI-only faceplate elements from `declare curlop_gui` records in the
    // source. These are not DSP params and must not affect ControlCore/schema
    // wiring.
    static std::vector<FaceplateElement> compileFaceplateElementsOnly(
        const std::string& source,
        FaustRuntime* runtime = nullptr);

    // GUI-only socket records from source `declare curlop_gui "socket ..."`.
    // They annotate existing ports; they never alter the module contract.
    static std::vector<SocketPresentation> compileSocketPresentationsOnly(
        const std::string& source,
        FaustRuntime* runtime = nullptr);

    static FaceplateSchema compileFaceplateSchemaOnly(const std::string& source,
                                                      std::string& errOut,
                                                      FaustRuntime* runtime = nullptr);

    // T-405 (ADR-0008): the node's declaration surface. Ports = the fixed
    // note trio (gate/pitch/velocity, ch0-2, signal streams) + the installed
    // program's params in Faust slider declaration order (ch3+i). Re-projected
    // on every install that lands a different schema; consumers detect that
    // via topologyVersion() (monotonic, atomic) and re-read. Valid for the
    // node's lifetime.
    const ModuleContract& contract() const;

    // T-318: invoked on the worker thread when a successful recompile produces
    // a schema that differs from the currently-installed program's schema.
    // The new program is *not* installed in this node; the callback is
    // expected to drive a sub-graph rebuild (ApgGraphSync::rebuildSingleModule)
    // which constructs a fresh FaustNode with the new ControlCore set and
    // atomically swaps it in. The arguments are the source text that produced
    // the new schema and the schema itself; the callback owner is responsible
    // for marshalling onto whatever thread it needs (typically posts to the
    // EventRouter via the bridge command queue). Set once at construction
    // time; not safe to mutate concurrently with setSource calls.
    using SchemaChangedFn =
        std::function<void(const std::string& newSource,
                           std::vector<ParamSchemaEntry> newSchema)>;
    void setSchemaChangedCallback(SchemaChangedFn cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace curlop
