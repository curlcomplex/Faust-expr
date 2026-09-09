#pragma once

#include <juce_graphics/juce_graphics.h>  // juce::Point
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <optional>
#include <cstdint>

#include "control/vm/machine/Signal.h"
#include "graph/transport/SignalDescriptor.h"
#include "graph/state/GraphStateLimits.h"
#include "modules/contract/ModuleTypes.h"     // ParamSchemaEntry — per-instance schema (T-318)
#include "modules/contract/FaceplateTypes.h"  // FaceplateLayout — per-instance layout overrides (T357-5)

namespace curlop {

struct AuthoredParameterState {
    std::string paramId;
    param::AuthoredValue value;
    // Required for ScriptSteps; empty for wall-clock and tempo-relative bases.
    std::string timingSourceId;
};

// ═══════════════════════════════════════════════════════════════════════════
// GraphState — per-clip authoritative graph description.
//
// Stop 0.A Cut 1: declared here as the target schema. No live code references
// yet — the APG is still driven by the legacy APG_GRAPH_STATE handler in
// EventRouter.cpp. Cut 2 will migrate authority into these types.
//
// Fields mirror the React Flow data model one-for-one so the migration is a
// direct move, not a restructuring:
//   - modules[].moduleId    == React Flow node id ("string uuid")
//   - modules[].index       == session-stable int used by APG + bytecode
//   - modules[].position    == React Flow node position
//   - modules[].paramValues == per-clip param values — live knob positions
//                               written by USER_SET_PARAM + BYTE overlay; schema
//                               defaults populated by USER_ADD_MODULE (Tier 3
//                               migrates schema source to C++). Authoritative
//                               source for clip-switch seeding + save/load
//                               persistence (Tier 1, Cut 0.C-1).
//   - edges[]               == React Flow edges, resolved via index not nodeId
//   - viewport              == React Flow viewport (pan + zoom) per Q2
// ═══════════════════════════════════════════════════════════════════════════

struct ModuleEntry {
    int         index      = -1;                // session-stable int, matches bytecode module index
    std::string dslName;                         // e.g. "kick", "reverb2"
    std::string moduleId;                        // React Flow nodeId (uuid string)
    std::string lineageId;                       // registry key, e.g. "core.gen.kick" — join key for CORE_MODULES lookup

    // T-364 (F-069, MODULE-IDENTITY-SPEC §Identity-Fields + Layer 4): stable
    // per-instance lineage_id (UUID v4) for an AUTHORED module. Factory modules
    // leave this empty and resolve their UUID from the registry by lineageId;
    // an authored Faust module (lineageId == "core.faust_jit") has no unique
    // registry UUID, so its identity must live on the instance. Assigned a fresh
    // UUID when the module is first saved as a version; the version store keys on
    // it (modules/<lineageUuid>/vN.{fdsp,fbc}). Never changes once set (forking
    // mints a new one). Empty for every factory module — registry is authority there.
    std::string lineageUuid;

    // T-364: which saved version of this authored module the instance is pinned
    // to. 0 = unversioned — use the embedded `code` (factory modules, and
    // authored modules that have never been version-saved). >0 = load the source
    // from modules/<lineageUuid>/v<moduleVersion>.fdsp at project load.
    int moduleVersion = 0;

    // SF-077: the current preset's name (the moduleVersion-th preset's declared
    // name), cached for the faceplate strip. Display-only — re-resolved from the
    // version store by the engine on save/step/recall/load; the store manifest
    // is the authority. Empty → the strip shows "preset <N>".
    std::string presetName;

    // T-685: read-only authored-module store status for the faceplate:
    // session | project | library | diverged. Empty means "not computed"; the
    // GUI falls back to session for unversioned authored modules.
    std::string moduleStoreStatus;

    juce::Point<float> position{ 0.0f, 0.0f };
    // T-292: user-set node body size in graph px (.x = width, .y = height).
    // {0,0} = no override — the GUI lays the node out to fit its widget grid.
    // Editor-only metadata, like position; persisted in the clip state so a
    // resized node (e.g. an enlarged core.script editor) survives save/load.
    juce::Point<float> nodeSize{ 0.0f, 0.0f };

    // T-425 (SF-060): in-memory hint — true when this node was auto-placed by
    // the spawn default (a programmatic/MCP add with no explicit position, see
    // EventRouter::autoPlaceModule) and the user hasn't deliberately moved it
    // since. The "unplaced" layout mode tidies exactly this set, leaving
    // hand-placed nodes alone. NOT serialized: a loaded project's nodes are all
    // considered deliberately placed (they carry saved positions), so this
    // resets to false on load — only fresh in-session auto-adds are "unplaced".
    bool autoPlaced = false;
    std::unordered_map<std::string, float> paramValues;
    // Optional authored semantic values. Legacy projects contain only
    // paramValues; absence is their explicit migration to raw target units.
    std::vector<AuthoredParameterState> authoredParamValues;
    std::string code;                            // Module source text (0.A-closure)

    // SF-110: render-domain assignment for a Visual Shader. V1 accepts only
    // graph-background and native graph authority enforces one owner.
    std::string visualTarget;
    std::string authoredPackageId;
    std::string sourceUnitId;
    std::string sourceKey;
    std::string sourceLanguage;

    // T-733: per-instance MIDI route metadata. Empty = host/default aggregate
    // route. Hardware route ids are stable JUCE MidiDeviceInfo identifiers,
    // not list positions, and are only meaningful in standalone.
    std::string midiInputRouteId;
    std::string midiOutputRouteId;

    // FF-029 buffer persistence: project-relative WAV path for a core.buffer
    // capture, e.g. takes/buffer-1-core_buffer_abc.wav. Empty = runtime-only
    // or no captured media saved yet.
    std::string bufferAssetPath;

    // T-318 (CON-007 Phase 2b): per-instance param schema. Empty = use the
    // registry default for `lineageId` (the normal case for static-schema
    // modules). Populated for runtime-authored modules whose schema is
    // determined by per-instance source — currently `core.faust_jit`, where
    // the .dsp `hslider/vslider` declarations define the faceplate. Written
    // by FaustNode's hot-swap completion path; read by Web faceplate hydration
    // and ApgGraphSync (sub-graph rebuild on schema change).
    std::vector<ParamSchemaEntry> params;

    // GUI-only source-authored faceplate elements. Separate from `params` so
    // labels/dividers/spacers/meters never create ControlCores or sockets.
    std::vector<FaceplateElement> faceplateElements;
    std::vector<FaceplateMeter>   faceplateMeters;
    FaceplateGroup                faceplateGroup;

    // T-572: per-instance audio socket names. Empty = use the registry default
    // (static modules). Populated for core.faust_jit because its source decides
    // whether it is a synth (no audio input) or an effect (IN → OUT).
    std::vector<std::string> audioInputs;
    std::vector<std::string> audioOutputs;
    // F-099: render-domain texture sockets. These are deliberately separate
    // from audio/control sockets: they are executed by the WebGL visual graph
    // and never become APG channels.
    std::vector<std::string> visualInputs;
    std::vector<std::string> visualOutputs;

    // F-075: the module's declared control-output names ([curlop:cvout]), in
    // declaration order. Empty for modules with none. Like `params`, this is
    // per-instance (a faust_jit's outputs come from its source, not the
    // registry) and travels through GRAPH_STATE to the canvas, where each name
    // becomes a right-edge output socket a control wire can start from. The
    // engine pairs name[k] with sub-graph output channel 2+k.
    std::vector<std::string> controlOutputs;
    std::vector<vm::SignalType> controlOutputTypes;

    // Source-derived control input socket names. Unlike exposedParamInputs,
    // these are not user-selected parameter inputs; they are script/module
    // processor buses that the graph can wire into.
    std::vector<std::string> controlInputs;

    // T-833: canonical non-visual port declarations. The historical
    // audio*/control* name vectors above remain compatibility projections.
    std::vector<SignalPortDeclaration> signalInputs;
    std::vector<SignalPortDeclaration> signalOutputs;
    bool signalInputsExplicit = false;
    bool signalOutputsExplicit = false;

    // User-selected params exposed as normal left-edge input sockets. The schema
    // (`params`) says what exists; this list says what is visible/connectable.
    std::vector<std::string> exposedParamInputs;
    bool exposedParamInputsExplicit = false;

    // GUI-only socket presentation metadata. Socket existence remains governed
    // by audioInputs/audioOutputs/controlOutputs/exposedParamInputs above.
    std::vector<SocketPresentation> socketPresentation;

    // T-480 (SF-065, SPEC-014 §6 + ADR-0019): per-instance voice policy.
    // physicalVoices = the allocated voice ceiling (structural — changing it
    // rebuilds the adapter sub-graph + channel contract); 0 = use the
    // registry default for `lineageId`. stealPolicy maps onto
    // vm::Delivery::StealPolicy (0 LastStolen / 1 RoundRobin /
    // 2 OldestReleased). User-facing on EVERY module, never a fixed
    // constant; persisted with the clip. The registry holds defaults only.
    int     physicalVoices = 0;
    uint8_t stealPolicy    = 0;
    ModuleProcessingMode processingMode = ModuleProcessingMode::Native;
    ModuleOversamplingFactor oversamplingFactor = ModuleOversamplingFactor::X1;

    // T-836: dedicated Audio Input endpoint state. The selected zero-based
    // negotiated input-bus ordinal is persisted. The bus count is ephemeral
    // runtime authority used only for GRAPH_STATE resolution/readback.
    std::uint32_t audioInputChannelIndex = 0;
    std::uint32_t audioInputBusChannelCount = 0;

    // T357-5 (ADR-009): per-instance faceplate layout overrides. The ADR-009
    // model is `runtime faceplate = FaceplateSchema ⊕ FaceplateLayout`: the
    // schema re-derives from source on load (faust_jit) or from the factory
    // registry (native); the *layout* — user-edited positions, sizes, widget
    // picks, panel style — is the only half that cannot re-derive, so it lives
    // here and persists with the clip state. Empty = no overrides → the merge
    // (T357-6) uses pure schema defaults. Per-instance because faust_jit has no
    // registered module definition (the `core.faust_jit` registry entry is a
    // generic stub); a faust_jit module's identity travels on the instance,
    // alongside `code` + `params`.
    FaceplateLayout faceplate;
};

// Graph-level feedback is deliberately not same-sample recursion. A boundary
// is inferred when an ordinary signal edge closes a cycle and is then persisted
// as part of that edge's contract.
enum class FeedbackBoundary : std::uint8_t {
    None,
    OneSample
};

enum class ModulationPolarity : std::uint8_t {
    Unipolar,
    Bipolar,
};

// Semantic metadata for a wireless modulation relationship. The relationship
// remains one ordinary graph edge; these stable public IDs survive module index
// reassignment, while depth is deliberately distinct from audio-wire gain.
struct ModulationMapping {
    std::string routeId;
    std::string sourceId;
    std::string targetParamId;
    float depth = 0.0f; // signed normalized target-range offset, -1 .. +1
    ModulationPolarity polarity = ModulationPolarity::Unipolar;
};

struct ResolvedModulationMapping {
    std::size_t sourceOutput = 0;
    std::size_t targetParameter = 0;
};

// Mapping IDs are module-local: source declaration name (legacy source ID),
// target schema sourceId. Display labels and exposed sockets are not targets.
std::optional<ResolvedModulationMapping> resolveModulationMapping(
    const ModuleEntry& source, const ModuleEntry& target,
    const ModulationMapping& mapping);

struct EdgeEntry {
    int         srcIndex = -1;
    int         tgtIndex = -1;
    std::string srcPort;  // defaults to "" (primary output)
    std::string tgtPort;  // defaults to "" (primary input)
    // Empty means the legacy audio/control contract. `visual-texture` is the
    // persisted render-domain role and is never routed through APG.
    std::string signalRole;
    // Absent only while ingesting a legacy payload. GraphState ingress and
    // fragment/project parsers deterministically migrate it before exposure.
    std::optional<transport::SignalDescriptor> signalDescriptor;
    FeedbackBoundary feedbackBoundary = FeedbackBoundary::None;
    std::optional<ModulationMapping> modulation;

    // Smart-wire per-edge audio params (0.4.0 cycle).
    // gain/pan are applied by the WireProcessor inserted between src and tgt.
    // muted/soloed are policy flags: effective_gain = muted ? 0
    //   : (anySoloed && !soloed ? 0 : gain).
    // Effective gain is computed at apply-time (ApgGraphSync) and written to
    // the edge's gain ControlCore; pan is written unchanged.
    float gain    = 1.0f;   // 0.0 .. 2.0 linear
    float pan     = 0.0f;   // -1.0 (L) .. +1.0 (R), equal-power
    bool  muted   = false;
    bool  soloed  = false;

    // M-001 Stage 2 (T-058 fold-in) — audibility result of
    // GraphState::computeEffectiveEdges(). False = muted OR not on a
    // solo-feeding / solo-downstream chain. Serialized in GraphStateSnapshot
    // so JS no longer needs to recompute via SmartWire.jsx silencedBySolo. Default
    // true so unaffected snapshots are visible by default.
    bool  audible = true;

    // GUI-only wire presentation override. Routing and smart-wire params above
    // remain the authority for engine behavior.
    WirePresentation presentation;
};

// Effective per-edge audio params after the smart-wire mute/solo policy is
// folded into gain. Produced by GraphState::computeEffectiveEdges() and
// consumed by BOTH the build path (GraphStateApplier → ApgEdge) and the live
// wire-param path (EventRouter → ControlCore::setKnobPosition). Keeping the
// solo BFS in exactly one place is what lets a wire knob-drag update the live
// cores without a topology rebuild — B-263 (T-375 incremental-path regression).
struct EdgeEffective {
    int   srcIndex = -1;
    int   tgtIndex = -1;
    float gain     = 1.0f;   // effective gain (mute/solo already folded in)
    float pan      = 0.0f;   // pan is policy-independent (passed through)
    bool  audible  = true;   // false = muted or off the active solo chain
    // F-075: the named ports the edge connects (carried through to ApgEdge so
    // the apply path can route a control-output srcPort as a control wire).
    std::string srcPort;
    std::string tgtPort;
    transport::SignalDescriptor signalDescriptor =
        transport::SignalDescriptor::legacyStereo();
    FeedbackBoundary feedbackBoundary = FeedbackBoundary::None;
    std::optional<ModulationMapping> modulation;
};

struct Viewport {
    float x    = 0.0f;
    float y    = 0.0f;
    float zoom = 1.0f;
};

class GraphState {
public:
    GraphState() = default;

    // ── Mutators. Idempotent where sensible. ────────────────────────────
    std::string addModule(ModuleEntry entry);
    void removeModuleByNodeId(const std::string& nodeId);
    bool replaceModuleByNodeId(const std::string& nodeId,
                               ModuleEntry replacement);
    void moveModule(const std::string& nodeId, juce::Point<float> pos);
    // T-292: set a node's user-resized body size (graph px; {0,0} clears the
    // override). Editor-only metadata — no APG/slot apply, like moveModule.
    void resizeNode(const std::string& nodeId, juce::Point<float> size);
    // Renames the script-facing handle and returns the actual applied name.
    // If another module already owns the requested dslName, a numeric suffix is
    // appended to keep clip-local handles unique.
    std::string renameModule(const std::string& nodeId, std::string newDslName);
    void updateModuleCode(const std::string& nodeId, std::string newCode);
    // T-480: per-instance voice policy (physicalVoices 0 = registry default;
    // stealPolicy = vm::Delivery::StealPolicy numeric). Structural — callers
    // follow with the rebuild-and-swap path + a recompile.
    void setModuleVoicePolicy(const std::string& nodeId,
                              int physicalVoices, uint8_t stealPolicy);
    void setModuleProcessingMode(const std::string& nodeId,
                                 ModuleProcessingMode processingMode);
    void setModuleOversamplingFactor(
        const std::string& nodeId,
        ModuleOversamplingFactor oversamplingFactor);
    bool setAudioInputChannelIndex(const std::string& nodeId,
                                   std::uint32_t channelIndex);
    bool setAudioInputBusChannelCount(std::uint32_t channelCount);
    // T-318 step 5b: per-instance schema mutator. Written by the FaustNode
    // schemaChangedCallback after a recompile changes the slider set.
    void updateModuleParams(const std::string& nodeId,
                             std::vector<ParamSchemaEntry> newParams);
    void updateModuleFaceplateElements(const std::string& nodeId,
                                       std::vector<FaceplateElement> elements);
    void updateModuleFaceplateSchema(const std::string& nodeId,
                                     std::vector<FaceplateElement> elements,
                                     std::vector<FaceplateMeter> meters,
                                     FaceplateGroup group);
    // Source replacement re-derives this instance layout from its new
    // parameter schema. This prevents the generic Faust starter controls from
    // surviving beside the new source-authored panel in GRAPH_STATE.
    void updateModuleFaceplateLayout(const std::string& nodeId,
                                     FaceplateLayout layout);
    // F-075: per-instance control-output names ([curlop:cvout]), synced from the
    // live FaustNode after every (re)build. Returns true if the set changed (so
    // the caller can push a fresh GRAPH_STATE only when the sockets moved).
    bool updateModuleAudioSockets(const std::string& nodeId,
                                  std::vector<std::string> inputs,
                                  std::vector<std::string> outputs);
    // Replace the source-derived audio descriptor contract after a live Faust
    // node has supplied its compiled process arity. This is distinct from the
    // socket-name projection: legacy projects can carry correct IN/OUT names
    // alongside an obsolete stereo descriptor.
    bool updateModuleAudioPortDescriptors(
        const std::string& nodeId,
        std::vector<SignalPortDeclaration> inputs,
        std::vector<SignalPortDeclaration> outputs);
    bool updateModuleControlOutputs(const std::string& nodeId,
                                    std::vector<std::string> names,
                                    std::vector<vm::SignalType> types = {});
    // Presentation-only source metadata for existing sockets. The port vectors
    // above remain the authority for identity and routing.
    bool updateModuleSocketPresentation(const std::string& nodeId,
                                        std::vector<SocketPresentation> presentation);
    bool setModuleExposedParamInputs(const std::string& nodeId,
                                     std::vector<std::string> names);
    // Atomically replace source-derived input authority. Stable public-id
    // renames carry the current value and target edges before one final prune.
    bool updateModuleInputContract(
        const std::string& nodeId,
        std::vector<ParamSchemaEntry> params,
        std::vector<std::string> exposedParamInputs,
        std::vector<std::string> audioInputs,
        std::vector<std::string> visualInputs,
        std::vector<std::string> visualOutputs,
        const std::vector<std::pair<std::string, std::string>>& portRenames);
    bool addEdge(EdgeEntry edge);
    void removeEdge(int srcIndex, int tgtIndex,
                    const std::string& srcPort, const std::string& tgtPort);

    // Smart-wire per-edge param mutator (0.4.0 cycle). Returns true if the
    // edge was found and param written; false if no edge matches the
    // (src, tgt, srcPort, tgtPort) key or param name is unknown.
    // A real value change advances edgeRevision so strict UI intents cannot
    // mutate a same-tuple edge recreated after capture.
    // param in {"gain","pan","muted","soloed"}. For bool params value is
    // encoded as 0.0=false, anything-else=true.
    bool setEdgeParam(int srcIndex, int tgtIndex,
                      const std::string& srcPort, const std::string& tgtPort,
                      const std::string& param, float value);

    bool edgeIsInSignalCycle(const EdgeEntry& edge) const;

    void setViewport(Viewport vp);

    // Per-clip param write by dslName. Tier 1 (0.C-1): persistence authority for
    // live knob positions. No-op if dslName not resident. [-r DUMB-VIEW-INV-1]
    bool setParamValue(const std::string& dslName,
                       const std::string& paramName,
                       float value);
    // User-facing writes require an existing persisted key or instance schema
    // declaration and respect schema bounds. Unlike setParamValue, this never
    // creates arbitrary keys supplied by an event payload.
    bool setKnownParamValue(const std::string& dslName,
                            const std::string& paramName,
                            float value,
                            const std::vector<ParamSchemaEntry>* fallbackSchema = nullptr);
    bool setModuleMidiRoute(const std::string& nodeId,
                            std::string inputRouteId,
                            std::string outputRouteId);

    // ── Bulk replace (used by USER_LOAD_CLIP_GRAPH). ────────────────────
    // Returns false without mutation when any incoming edge fails graph
    // contract/cycle admission. Callers must not report partial authority.
    bool replace(std::vector<ModuleEntry> newModules,
                 std::vector<EdgeEntry>   newEdges,
                 Viewport                 newViewport);

    // ── Accessors. ──────────────────────────────────────────────────────
    const std::vector<ModuleEntry>& modules()  const noexcept { return modules_; }
    // T-364: mutable module access for in-place instance edits (version pin /
    // lineageUuid assignment on save-version, source resolve on load). Message
    // thread only — same constraint as the rest of GraphState mutation.
    std::vector<ModuleEntry>&       modulesMutable()  noexcept { return modules_; }
    const std::vector<EdgeEntry>&   edges()    const noexcept { return edges_; }
    uint64_t                        edgeRevision() const noexcept { return edgeRevision_; }
    Viewport                        viewport() const noexcept { return viewport_; }

    // Smart-wire mute/solo policy folded into effective per-edge gain. One
    // EdgeEffective per edge with srcIndex/tgtIndex >= 0, in edges() order.
    // Solo semantics: when any wire is soloed, an edge passes iff it is itself
    // soloed, OR it feeds (backward BFS) a soloed wire's source, OR it carries
    // (forward BFS) a soloed wire's target. Muted edges are always 0. This is
    // the single source of truth for effective gain — B-263. [-r SMARTWIRE-SOLO]
    std::vector<EdgeEffective> computeEffectiveEdges() const;

    // ── Lookups. Return nullptr if not found. ───────────────────────────
    const ModuleEntry* findByNodeId(const std::string&) const noexcept;
    const ModuleEntry* findByDslName(const std::string&) const noexcept;
    const ModuleEntry* findByIndex(int) const noexcept;

    // ── Diagnostics. ────────────────────────────────────────────────────
    size_t moduleCount() const noexcept { return modules_.size(); }
    size_t edgeCount()   const noexcept { return edges_.size(); }

private:
    void reconcileModulePortContract(
        ModuleEntry& module,
        bool forceInputs = false,
        bool forceOutputs = false);

    std::vector<ModuleEntry> modules_;
    std::vector<EdgeEntry>   edges_;
    uint64_t                 edgeRevision_ { 0 };
    Viewport                 viewport_;
    std::uint32_t            audioInputBusChannelCount_ { 0 };
};

// ── Per-clip keyed container ────────────────────────────────────────────
// Keyed by int to match BytecodeMessage::clipId and ClipStore.js numeric ids.
class GraphStateContainer {
public:
    GraphState&       forClip(int clipId);                    // create-on-demand
    const GraphState* tryForClip(int clipId) const noexcept;  // nullptr if absent
    void              eraseClip(int clipId);
    std::vector<int>  clipIds() const;

    // Cut 0.C-3b: deep-copy src clip's GraphState into dst slot. Used by
    // USER_DUPLICATE_CLIP handler. No-op if src absent. [-r DUMB-VIEW-INV-1]
    void              duplicateClip(int srcClipId, int dstClipId);
    void              setAudioInputBusChannelCount(std::uint32_t channelCount);

    // B-182 — bulk wipe. Called on newProject and at the top of setState
    // (project load) to drop orphan GraphStates from the previous project.
    // Without this, a new clip whose id collides with an old clip's key
    // inherits the old graph via `forClip(id)`'s create-on-demand semantics.
    void              clear() noexcept { states_.clear(); }

    size_t size() const noexcept { return states_.size(); }

private:
    std::unordered_map<int, GraphState> states_;
    std::uint32_t audioInputBusChannelCount_ { 0 };
};

// ── core.script_v2 helpers ──────────────────────────────────────────────
// Shared by fresh-project and payload-carry Script V2 creation.
// Both call sites want the same shape: deterministic moduleId, dslName
// disambiguated against existing entries, index = max+1.

// First core.script_v2 entry in `modules`, or nullptr if none.
const ModuleEntry* findFirstCoreScriptV2Module(const std::vector<ModuleEntry>& modules) noexcept;

// Build a fresh core.script_v2 ModuleEntry intended for append to
// `existingModules`. dslName auto-disambiguates ("script", "script2", ...).
// moduleId = "core.script_v2_<idSuffix>_clip<clipId>". Caller pushes it into
// the modules vector / GraphState.
ModuleEntry makeCoreScriptV2ModuleEntry(const std::vector<ModuleEntry>& existingModules,
                                        int clipId,
                                        const std::string& idSuffix,
                                        std::string initialCode);

} // namespace curlop
