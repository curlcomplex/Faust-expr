// CURLOP VM Event Bridge
//
// Lock-free bridge between the audio thread (VM) and the main thread
// Uses std::atomic for all shared state.
//
// Audio thread writes module snapshots (gate, pitch, params) after
// each VM evaluation. Main thread reads them for graph-control state
// reporting and presentation snapshots.
//
// No locks, no allocation — safe for real-time audio thread writes.
#pragma once

#include "control/vm/core/VMConstNode.h"   // kDefaultInitialModules
#include <atomic>
#include <cstdint>
#include <vector>
#include <string>
#include <utility>

namespace curlop {

// Per-module state snapshot.
// Written by audio thread (processBlock), read by main thread (timer).
struct ModuleSnapshot {
    static constexpr int MAX_PARAMS = 64;

    // Param values and presence flags, indexed by param_idx.
    // params    = composed REAL-unit value (post-offset — the offset-arc
    //             endpoint when the offset layer is active).
    // absValues = abs-layer (base) REAL-unit value (set lock / routed
    //             stream / knob — the abs-pin position).
    // presence  = SPEC-014 layer bitmask: bit0 (=1) absolute layer active,
    //             bit1 (=2) offset layer active. 0 = knob only.
    // midiPresence mirrors presence for GUI MIDI-learn lanes so widgets can
    // tint only the layer currently driven by MIDI.
    std::atomic<float> params[MAX_PARAMS] = {};
    std::atomic<float> absValues[MAX_PARAMS] = {};
    std::atomic<float> presence[MAX_PARAMS] = {};
    std::atomic<int> midiPresence[MAX_PARAMS] = {};
    // PARAM_STATE presentation dirtiness is per cell. A module-wide flag made
    // one changing sequencer/parameter serialize every declared parameter at
    // the 120 Hz processor timer rate.
    std::atomic<bool> paramDirty[MAX_PARAMS] = {};
    std::atomic<int> sequencerStep{-1};

    // Dirty flag — set by audio thread, cleared by main thread after reading
    std::atomic<bool> dirty{false};
};

class VMEventBridge {
public:
    // T-328 slice 4c-6: VMEventBridge::MAX_MODULES alias retired.
    // Callers use EngineSlot::moduleCapacity() (per-slot runtime) for
    // bounds-check and snapshotCapacity() below for the bridge's own
    // capacity. Default ctor cap is kDefaultInitialModules — explicitly
    // a sizing hint, not a cap. ModuleSnapshot's std::atomic members
    // are non-movable so the vector is never resized after ctor — the
    // install envelope (slice 4c-4) rebuilds the bridge fresh.
    explicit VMEventBridge(size_t cap = static_cast<size_t>(::curlop::kDefaultInitialModules))
        : modules_(cap), moduleNodeIds_(cap), moduleSourceFingerprints_(cap) {}

    int snapshotCapacity() const { return static_cast<int>(modules_.size()); }

    // F-066 Phase 5: direct per-cell write from the machine pipeline.
    // value = composed REAL-unit param value; absValue = abs-layer (base)
    // REAL-unit value; presence = SPEC-014 layer bitmask (bit0 abs pin,
    // bit1 offset arc). midiPresence uses the same bits for MIDI-driven
    // layers.
    void setParam(int moduleIdx, int paramIdx, float value, float absValue,
                  float presence, int midiPresence = 0)
    {
        if (moduleIdx < 0 || moduleIdx >= (int) modules_.size())
            return;
        if (paramIdx < 0 || paramIdx >= ModuleSnapshot::MAX_PARAMS)
            return;
        auto& mod = modules_[(size_t) moduleIdx];
        const bool changed =
            mod.params[paramIdx].load(std::memory_order_relaxed) != value
            || mod.absValues[paramIdx].load(std::memory_order_relaxed) != absValue
            || mod.presence[paramIdx].load(std::memory_order_relaxed) != presence
            || mod.midiPresence[paramIdx].load(std::memory_order_relaxed) != midiPresence;
        mod.params[paramIdx].store(value, std::memory_order_relaxed);
        mod.absValues[paramIdx].store(absValue, std::memory_order_relaxed);
        mod.presence[paramIdx].store(presence, std::memory_order_relaxed);
        mod.midiPresence[paramIdx].store(midiPresence, std::memory_order_relaxed);
        if (changed)
            markParamDirty(moduleIdx, paramIdx);
    }

    void setSequencerStep(int moduleIdx, int step)
    {
        if (moduleIdx < 0 || moduleIdx >= (int) modules_.size())
            return;
        auto& mod = modules_[(size_t) moduleIdx];
        mod.sequencerStep.store(step, std::memory_order_release);
    }

    void markParamDirty(int moduleIdx, int paramIdx)
    {
        if (moduleIdx < 0 || moduleIdx >= (int) modules_.size())
            return;
        if (paramIdx < 0 || paramIdx >= ModuleSnapshot::MAX_PARAMS)
            return;
        auto& mod = modules_[(size_t) moduleIdx];
        mod.paramDirty[paramIdx].store(true, std::memory_order_release);
        mod.dirty.store(true, std::memory_order_release);
    }

    bool consumeParamDirty(int moduleIdx, int paramIdx)
    {
        if (moduleIdx < 0 || moduleIdx >= (int) modules_.size())
            return false;
        if (paramIdx < 0 || paramIdx >= ModuleSnapshot::MAX_PARAMS)
            return false;
        return modules_[(size_t) moduleIdx].paramDirty[paramIdx]
            .exchange(false, std::memory_order_acq_rel);
    }

    bool isParamDirty(int moduleIdx, int paramIdx) const
    {
        if (moduleIdx < 0 || moduleIdx >= (int) modules_.size())
            return false;
        if (paramIdx < 0 || paramIdx >= ModuleSnapshot::MAX_PARAMS)
            return false;
        return modules_[(size_t) moduleIdx].paramDirty[paramIdx]
            .load(std::memory_order_acquire);
    }

    bool consumeDirty(int moduleIdx)
    {
        if (moduleIdx < 0 || moduleIdx >= (int) modules_.size())
            return false;
        return modules_[(size_t) moduleIdx].dirty.exchange(false, std::memory_order_acq_rel);
    }

    int sequencerStep(int moduleIdx) const
    {
        if (moduleIdx < 0 || moduleIdx >= (int) modules_.size())
            return -1;
        return modules_[(size_t) moduleIdx].sequencerStep.load(std::memory_order_acquire);
    }

    // Called from main thread to read latest state for a given module.
    const ModuleSnapshot& getModule(int idx) const
    {
        return modules_[idx];
    }

    // Called from main thread after reading a module's state.
    void clearDirty(int idx)
    {
        auto& mod = modules_[idx];
        mod.dirty.store(false, std::memory_order_relaxed);
        for (auto& cell : mod.paramDirty)
            cell.store(false, std::memory_order_relaxed);
    }

    // Called on transport stop to reset all modules to silence.
    void resetAll()
    {
        const size_t n = modules_.size();
        for (size_t i = 0; i < n; ++i)
        {
            modules_[i].sequencerStep.store(-1, std::memory_order_relaxed);
            modules_[i].dirty.store(true, std::memory_order_release);
            for (auto& cell : modules_[i].paramDirty)
                cell.store(true, std::memory_order_release);
        }
    }

    int getModuleCount() const { return moduleCount_.load(std::memory_order_relaxed); }
    void setModuleCount(int count) { moduleCount_.store(count, std::memory_order_relaxed); }

    void setModuleNodeId(int moduleIdx, std::string nodeId)
    {
        if (moduleIdx < 0 || moduleIdx >= static_cast<int>(moduleNodeIds_.size())) return;
        moduleNodeIds_[(size_t) moduleIdx] = std::move(nodeId);
    }

    const std::string& moduleNodeId(int moduleIdx) const
    {
        static const std::string empty;
        if (moduleIdx < 0 || moduleIdx >= static_cast<int>(moduleNodeIds_.size())) return empty;
        return moduleNodeIds_[(size_t) moduleIdx];
    }

    void setModuleSourceFingerprint(int moduleIdx, std::string fingerprint)
    {
        if (moduleIdx < 0
            || moduleIdx >= static_cast<int>(moduleSourceFingerprints_.size())) return;
        moduleSourceFingerprints_[(size_t) moduleIdx] = std::move(fingerprint);
    }

    const std::string& moduleSourceFingerprint(int moduleIdx) const
    {
        static const std::string empty;
        if (moduleIdx < 0
            || moduleIdx >= static_cast<int>(moduleSourceFingerprints_.size())) return empty;
        return moduleSourceFingerprints_[(size_t) moduleIdx];
    }

private:
    std::vector<ModuleSnapshot> modules_;
    std::vector<std::string> moduleNodeIds_;
    std::vector<std::string> moduleSourceFingerprints_;
    std::atomic<int> moduleCount_{0};
};

} // namespace curlop
