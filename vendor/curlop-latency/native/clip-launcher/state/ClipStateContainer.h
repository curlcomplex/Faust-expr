#pragma once

#include <juce_core/juce_core.h>
#include "graph/state/GraphState.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace curlop {

// T-833 shared descriptor projection used by project/fragment persistence and
// the GRAPH_STATE readback surface.
juce::var signalDescriptorToVar(const transport::SignalDescriptor& descriptor);
juce::var signalPortDeclarationsToVar(
    const std::vector<SignalPortDeclaration>& ports);
bool readPersistedSignalWidth(
    const juce::var& value,
    transport::SignalWidth& out);
bool signalDescriptorFromVar(
    const juce::var& value,
    transport::SignalDescriptor& out);
bool signalPortDeclarationsFromVar(
    const juce::var& value,
    std::vector<SignalPortDeclaration>& out);

// T-309 / SF-048 Phase D S3c: legacy `clips[].script` payloads are retained
// only as quarantined, non-executable project data. Current per-clip sequencer
// programs live on `core.script_v2` graph nodes (ModuleEntry.code). The
// transient compile-output fields below remain —
// they cache compileClipAndEmit's result for the clip's *program 0*,
// which is HALT-only post-T-309 but still travels through the install
// envelope to carry schemaSnapshots / loopLength / paramMappings.

// B-1903: one TC1-excluded module blocking publication, projected to the
// WebView graph so the offending node can render a visible blocked state.
struct BlockedModuleInfo {
    juce::String nodeId;
    juce::String lineageId;
    juce::String reason;
};

struct ClipData {
    int          id = 0;
    juce::String name;

    // F-076 / ADR adr-clips ¶21: per-clip presentation state. A user-set colour
    // (hex string, e.g. "#FF8800"; empty = no colour) persisted with the clip so
    // a live set reads by colour. Travels with the clip on cross-project import.
    // The launcher colour-picker GUI is the separate "clip list polish" item; this
    // is the data layer it builds on (and import carries).
    juce::String colour;

    // Issue #185: preserve pre-T-309 clip-level Script V1 payloads without
    // executing them; current executable sequencers are core.script_v2 graph
    // nodes. Presence is separate because JSON null is meaningful.
    bool legacyScriptPresent = false;
    juce::var legacyScriptPayload;

    // F-076 / T-578 (ADR adr-clips ¶43): the scratch/preview clip used to audition
    // a foreign clip before import. It lives in clips_ so the play path
    // (compileClipAndEmit + USER_PLAY_CLIP guard, both keyed by clipId) can reach
    // it, but it is filtered out of the launcher wire payload (toClipsStateVar)
    // AND project persistence (toJson) — no residue in the stack, never saved.
    // Always kept as the last element so the real-clip prefix keeps stable
    // indices for viewedClipIndex_ / getClipIdAtIndex.
    bool ephemeral = false;

    std::vector<uint8_t> bytecode;            // transient: populated by compileClipAndEmit, empty on fresh/error
    juce::String         diagnosticsJson;     // transient: raw JSON array from embed bundle ("[]" default)
    juce::String         compileError;        // transient: non-empty iff last compile failed
    std::vector<BlockedModuleInfo> blockedModules; // transient: TC1-excluded modules blocking publication (B-1903)
    juce::String         compileMetadataJson; // transient: raw JSON object with moduleCount/moduleNames/
                                              //            layouts/paramMappings/bpm/loopLength.
                                              //            USER_PLAY_CLIP handler parses to synthesize a
                                              //            BytecodeMessage for VM install (0.D-4 step 5a).
                                              //            "null" default until first compile.
};

struct VisualSettings {
    bool backgroundEnabled = true;
    bool windowEnabled = false;
    int displayIndex = 0;
    bool fullscreen = false;
    int width = 1280;
    int height = 720;
    float resolutionScale = 1.0f;
    float targetFps = 60.0f;
    bool captureEnabled = false;
    juce::String mode = "auto"; // "auto" | "milkdrop" | "synthesizer"
};

// ═══════════════════════════════════════════════════════════════════════════
// ClipStateContainer — C++-authoritative clip list + project metadata.
//
// Cut 0.C-3b Session A (scaffold): declares the target authority surface for
// ClipStore (JS Zustand) + PersistenceStore (JS coord). Methods are reachable
// from USER_*_CLIP wire handlers but are NOT driven by JS production paths
// until Session B atomic cutover. `toJson` / `fromJson` serialize the
// full project view (per-clip graph pulled from GraphStateContainer at
// snapshot time — graphContainer_ remains the single graph authority,
// clips[].graph is a view, not a duplicate store).
//
// Lock order: acquire ClipStateContainer.lock_ BEFORE GraphStateContainer
// access when serializing snapshots. GraphStateContainer itself is not
// internally locked — callers own ordering.
//
// [-r DUMB-VIEW-INV-1]
// ═══════════════════════════════════════════════════════════════════════════

class ClipStateContainer {
public:
    ClipStateContainer() = default;

    // ── Bulk replace (used by fromJson / envelope restore). ─────────────────
    void replace(std::vector<ClipData>&& clips,
                 int viewedIdx,
                 juce::String projectName,
                 int nextClipId);

    // ── Authority mutators (driven by USER_*_CLIP handlers). ────────────────
    int  addClip(juce::String name);
    void removeClip(int id);
    bool renameClip(int id, juce::String name);
    void setClipColour(int id, juce::String colour);     // F-076: per-clip colour (hex, "" clears)
    int  duplicateClip(int sourceId);                    // returns newId, -1 if source absent
    void reorderClips(const std::vector<int>& clipIdOrder);

    // F-076 / T-578: the audition scratch clip (ADR ¶43). ensurePreviewClip
    // returns a stable, reusable ephemeral clipId (allocated once via the normal
    // id sequence, flagged ephemeral, kept last) so the play path can target it;
    // clearPreviewClip removes it cleanly (audio teardown + graph erase are the
    // caller's job). getPreviewClipId returns the live id or -1 if none.
    int  ensurePreviewClip();
    void clearPreviewClip();
    int  getPreviewClipId() const;
    void setViewedClipIndex(int idx);
    void setProjectName(juce::String name);
    void setVisualSettings(VisualSettings settings);

    // ── Accessors. Return copies — thread-safe. ─────────────────────────────
    int          getViewedClipIndex() const;
    int          getViewedClipId() const;                // -1 when no non-ephemeral clip is viewed
    int          getClipIdAtIndex(int idx) const;         // -1 if out-of-range
    int          getClipIndexById(int id) const;          // -1 if not found
    int          getRealClipIndexById(int id) const;      // -1 for absent or ephemeral
    juce::String getProjectName() const;
    VisualSettings getVisualSettings() const;
    std::vector<ClipData> getClipsCopy() const;
    size_t       size() const;

    // Cut 0.D-4: write embed-compile output onto a clip's transient fields.
    // Called by CurlopProcessor::compileClipAndEmit after each compile. Silent
    // no-op if id not found. `bytecode` may be empty (compile failed);
    // `diagnosticsJson` should be a valid JSON array ("[]" on empty);
    // `compileError` non-empty iff compile failed; `metadataJson` is the raw
    // envelope metadata object string ("null" on compile failure).
    void setClipCompileResult(int id,
                              std::vector<uint8_t> bytecode,
                              juce::String diagnosticsJson,
                              juce::String compileError,
                              juce::String metadataJson);

    // Update only the aggregate clip-level error projected by CLIPS_STATE.
    // Node-program compilation runs after the clip envelope has installed and
    // must not discard that envelope's cached metadata while reporting a
    // persisted source failure. Returns true when the visible value changed.
    bool setClipCompileError(int id, juce::String compileError);

    // B-1903: update only the structured TC1-excluded module list projected by
    // CLIPS_STATE, alongside setClipCompileError. Empty list clears. Returns
    // true when the visible value changed.
    bool setClipBlockedModules(int id,
                               std::vector<BlockedModuleInfo> blockedModules);

    // Cut 0.D-4: read embed-compile output for a clip. Returns false if id
    // not found. Out-params receive copies (bytecode vector copy is cheap
    // since typical scripts produce <2KB).
    bool getClipCompileResult(int id,
                              std::vector<uint8_t>& outBytecode,
                              juce::String& outDiagnosticsJson,
                              juce::String& outCompileError,
                              juce::String& outMetadataJson) const;

    // ── Serialization. ──────────────────────────────────────────────────────
    // toJson: walks this container's clips_ + reads each clip's graph from
    // gc.tryForClip(id). Output: {clips:[{id,name,graph:{modules,edges,
    // viewport}}], viewedClipIndex, projectName, nextClipId}.
    // Used by ProjectPersistence (full-state save). Wire-envelope path uses
    // the slim toClipsStateVar() below.
    juce::String toJson(const GraphStateContainer& gc) const;

    // T-266 (Debt-2/3/12, Phase E): slim CLIPS_STATE wire payload. No graph
    // walk, no double-parse, no index round-trip. Output:
    //   {clips:[{id, name}], viewedClipId, projectName}
    // Serialized into CLIPS_STATE for the Web presentation.
    juce::var toClipsStateVar() const;

    // fromJson: wipes clips_ + repopulates from JSON. For each clip's graph
    // payload, calls gc.forClip(id).replace(modules, edges, viewport). Caller
    // responsible for clearing gc first if needed (this method only replaces
    // clips touched by the payload).
    void fromJson(const juce::String& json,
                  GraphStateContainer& gc,
                  bool canonicalizeGraphIndices = false);

    // Pre-commit project validation. Uses the same module/edge/viewport row
    // parsers as fromJson so malformed authority is rejected before teardown
    // instead of being silently skipped into a partial graph.
    static bool validatePersistedGraphVar(const juce::var& graph);

    // F-078 (T-580) — graph-fragment clipboard format. A fragment is a
    // self-contained, shareable-as-text subset of a graph: the selected
    // modules in full (params, code, voice policy, faceplate — reusing the
    // same entry serializers as save/load, so a pasted module is byte-identical
    // to a saved one) plus the edges *internal* to that subset. Edges carry
    // LOCAL indices (positions into `modules`), so the fragment is portable
    // across clips, projects, and instances. Wrapped + versioned with a
    // `curlop_fragment` marker so paste can distinguish it from arbitrary
    // clipboard text. Static — no instance state; callable from EventRouter +
    // unit tests. See 2026-06-15-F-078-module-copy-paste-architecture-decision.md.
    //
    // serializeFragment: `edgesLocal` must already use local indices (the
    // caller maps global→local while collecting internal edges).
    static juce::String serializeFragment(const std::vector<ModuleEntry>& modules,
                                          const std::vector<EdgeEntry>&   edgesLocal);

    // parseFragment: returns false (and leaves outputs untouched) if `text` is
    // not a valid curlop fragment (bad/absent marker, unsupported version, or
    // malformed JSON). On success `outModules`/`outEdges` are replaced; edge
    // indices remain LOCAL (the paste handler remaps them to new globals).
    static bool parseFragment(const juce::String& text,
                              std::vector<ModuleEntry>& outModules,
                              std::vector<EdgeEntry>&   outEdges);

    // F-078 — whole-clip clipboard. Same module/edge payload as a fragment plus
    // the clip name, under a distinct `curlop_clip` marker (so clip-paste and
    // module-paste don't cross-trigger). Used by USER_COPY_CLIP / USER_PASTE_CLIP.
    static juce::String serializeClipFragment(const juce::String& name,
                                              const std::vector<ModuleEntry>& modules,
                                              const std::vector<EdgeEntry>&   edgesLocal);
    static bool parseClipFragment(const juce::String& text,
                                  juce::String& outName,
                                  std::vector<ModuleEntry>& outModules,
                                  std::vector<EdgeEntry>&   outEdges);

    // Current fragment schema version emitted by serializeFragment.
    static constexpr int kFragmentVersion = 1;

private:
    mutable juce::CriticalSection lock_;
    std::vector<ClipData> clips_;
    int          viewedClipIndex_   = 0;
    juce::String projectName_       = "Untitled Project";
    int          nextClipId_        = 1;
    VisualSettings visualSettings_;
    int          previewClipId_     = -1;   // F-076/T-578 audition scratch clip; -1 = none
};

// ═══════════════════════════════════════════════════════════════════════════
// Cross-project clip import (F-076). importClipFromPayload generalises
// GraphStateContainer::duplicateClip to a FOREIGN JSON source: it brings one
// clip from another project's clips[] into `dstClips` as a NEW appended clip.
// See the .cpp for the per-module identity transforms; design + collision table:
// .planning/research/clip-portability-cross-project-import-design.md.
// ═══════════════════════════════════════════════════════════════════════════

// Per-import dependency policy for the clip's authored (Faust JIT) modules
// (ADR adr-clips ¶31). The embedded code snapshot travels in BOTH modes, so the
// sound is preserved regardless — these only decide library identity.
enum class ImportDepMode {
    ClipPrivate,      // fork each authored module to a fresh unversioned identity
                      // (lineageUuid cleared, version 0) — hermetic; no file I/O.
    MergeToLibrary    // keep the (designId, version) identity so the module joins
                      // the destination project's store, and materialise the
                      // carried version from srcProjectFolder into dstProjectFolder.
};

struct ClipImportResult {
    int                       newClipId = -1;   // allocated dest clipId (-1 on failure)
    std::vector<juce::String> warnings;         // non-fatal issues (unknown factory type, …)
    bool                      ok        = false;
};

// Import `clipJson` (one entry from a foreign project's clips[] —
// { name, colour?, graph:{ modules, edges, viewport } }) into `dstClips` /
// `dstGraph` as a new appended clip. srcProjectFolder / dstProjectFolder are used
// only by ImportDepMode::MergeToLibrary (to copy the authored module's version
// files); pass juce::File() for ClipPrivate or unsaved projects. Returns ok=false
// (and allocates nothing) when the payload has no graph/modules or no parseable
// module rows.
ClipImportResult importClipFromPayload(const juce::var& clipJson,
                                       ClipStateContainer& dstClips,
                                       GraphStateContainer& dstGraph,
                                       ImportDepMode depMode,
                                       const juce::File& srcProjectFolder,
                                       const juce::File& dstProjectFolder);

// F-076 / T-578 — the resolution core shared by import and audition (ADR ¶43:
// "the same resolution path minus the append"). Parses `clipJson`'s graph, runs
// the per-module identity transforms + dependency resolution against `depMode`,
// and writes the resolved graph into `dstGraph.forClip(targetClipId)` — it does
// NOT touch any ClipStateContainer (no addClip, no colour). importClipFromPayload
// is this plus the addClip/colour append; audition targets a preview clipId.
// result.newClipId echoes targetClipId; result.warnings carries the same
// factory-unknown / version-missing notes import surfaces. ok=false (writes
// nothing) when the payload has no graph/modules or no parseable module rows.
ClipImportResult buildClipGraphFromPayload(const juce::var& clipJson,
                                           int targetClipId,
                                           GraphStateContainer& dstGraph,
                                           ImportDepMode depMode,
                                           const juce::File& srcProjectFolder,
                                           const juce::File& dstProjectFolder);

} // namespace curlop
