// CURLOP record-profile / tap-layout vocabulary — SPEC-017 §1-3, F-070.
//
// A *recording* captures one or more *taps*. A tap is one named source — the
// master bus, or a single module's output. The MultiTrackWriter lays the taps
// out as channels of one file in tap order: each tap occupies the channel range
// [channelOffset, channelOffset + channelWidth). Master is always the first tap.
//
// A *profile* is the selection policy (SPEC-017 §1): given the live graph, it
// resolves to an ordered tap list. The presets grow as F-070 stages land — only
// Master exists today (SF-069). Stems / Everything arrive with SF-070 / SF-071.
//
// This header is the PURE layout authority: offset assignment + per-profile
// selection, no graph access (the caller gathers the candidate module list).
// Unit-tested via Path 1 (tests/io/RecordProfileTests.cpp). The graph-walk that
// decides which modules are candidates lives at the record-start call site.
//
// Channel width is PER-TAP DATA, never an assumed 2 (SPEC-017 §3 / INV-2):
// modules are stereo today but mono + ambisonic modules are anticipated.
#pragma once

#include <juce_core/juce_core.h>
#include <set>
#include <utility>
#include <vector>

namespace curlop {

// One named source captured into a recording.
struct RecordTap {
    juce::String name;            // track name (iXML TRACK_LIST): "master" or the module dslName
    int channelOffset = 0;        // first channel this tap occupies in the output file
    int channelWidth  = 2;        // channels this tap spans (stereo today; per-tap data)
    int sourceModuleIndex = -1;   // -1 = master bus; otherwise the graph module index to tap
};

// A candidate module the caller offers to a profile for possible inclusion.
struct CandidateModuleTap {
    juce::String dslName;
    int width = 2;
    int moduleIndex = -1;
};

// Which sources a recording captures. Resolved against the live graph at
// record-start (SPEC-017 §1-2). Presets grow as stages land; Master is the only
// value today — adding Stems/Everything is a new enum case + a new switch arm,
// with zero change to the writer (SPEC-017 INV-1).
enum class RecordProfile {
    Master,     // [master] — the stereo bus, as recorded today
    Stems,      // [master] + every output-reachable module as its own iso track (SF-070)
    Everything, // Stems (master + reachable, main file) + every NON-reachable module
                // in a separate companion "everything-else" polywav (SF-071, SPEC-017 §3)
};

// Pure layout: produce the ordered tap list with channel offsets assigned.
// Master is always present and always first. Module taps follow only for
// profiles that include them. `masterWidth` is honoured verbatim (no assumed 2).
inline std::vector<RecordTap> buildTapLayout(RecordProfile profile,
                                             int masterWidth,
                                             const std::vector<CandidateModuleTap>& modules)
{
    std::vector<RecordTap> taps;
    int offset = 0;

    // Master is unconditional and first.
    taps.push_back({ "master", offset, masterWidth, -1 });
    offset += masterWidth;

    switch (profile)
    {
        case RecordProfile::Master:
            // Master-only: candidate modules are not included.
            juce::ignoreUnused(modules);
            break;

        case RecordProfile::Stems:
        case RecordProfile::Everything:
            // Master (tracks 0..masterWidth) + every candidate module as its own
            // iso track, in caller order, each at the next free channel range.
            // The caller decides which modules are candidates (output-reachable);
            // this just assigns their channel offsets. For Everything this is the
            // MAIN file (reachable modules); the NON-reachable companion file is
            // laid out separately by buildModuleOnlyLayout (SPEC-017 §3).
            for (const auto& mod : modules)
            {
                taps.push_back({ mod.dslName, offset, mod.width, mod.moduleIndex });
                offset += mod.width;
            }
            break;
    }

    return taps;
}

// Companion "everything-else" polywav layout (SPEC-017 §3): the NON-output-
// reachable modules, each its own iso track from channel 0 — NO master tap
// (master lives in the main file). Pure layout; the caller supplies the module
// set (the BFS non-reachable partition). Used only by RecordProfile::Everything.
inline std::vector<RecordTap> buildModuleOnlyLayout(const std::vector<CandidateModuleTap>& modules)
{
    std::vector<RecordTap> taps;
    int offset = 0;
    for (const auto& mod : modules)
    {
        taps.push_back({ mod.dslName, offset, mod.width, mod.moduleIndex });
        offset += mod.width;
    }
    return taps;
}

// Backward reachability over the audio graph: which module indices have a
// directed path TO some output node (SPEC-017 §2 — Stems = output-reachable).
// `edges` are (src, tgt) audio wires; `outputIndices` are the output module
// nodes (sinks). Returns every index that reaches an output, INCLUDING the
// output indices themselves (the caller excludes outputs from taps, since an
// output's meter duplicates the master bus). Pure — no graph types, so the
// caller feeds graph-derived (src,tgt) pairs and this stays Path-1 testable.
//
// Algorithm: seed the frontier with the outputs, then walk edges in reverse
// (tgt→src) to a fixpoint. O(E·passes); graphs are tiny (per-clip module set).
inline std::set<int> computeOutputReachable(int numModules,
                                            const std::vector<std::pair<int,int>>& edges,
                                            const std::vector<int>& outputIndices)
{
    std::set<int> reachable;
    for (int o : outputIndices)
        if (o >= 0 && o < numModules)
            reachable.insert(o);

    // Reverse-walk to fixpoint: an edge (s,t) makes s reachable if t is.
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto& e : edges)
        {
            const int s = e.first, t = e.second;
            if (s < 0 || s >= numModules) continue;
            if (reachable.count(t) && !reachable.count(s))
            {
                reachable.insert(s);
                changed = true;
            }
        }
    }
    return reachable;
}

// Total channel count a profile's layout occupies.
inline int totalChannels(const std::vector<RecordTap>& taps)
{
    int n = 0;
    for (const auto& t : taps) n += t.channelWidth;
    return n;
}

} // namespace curlop
