#pragma once

// AudioParamState.h — Audio-thread local copy of param values.
// Populated by draining ParamFIFO events at the top of processBlock.
// AUDIO-01: All reads come from local storage, never from ParamStore.
// AUDIO-02: No NaN values — all initialized to 0.0f, safe fallback for missing modules.
// No locks. Allocation only on MODULE_ADDED events (rare — clip switch, not per-block).

#include "control/vm/params/ParamTypes.h"
#include "control/vm/core/VMConstNode.h"  // VMProcessData (for writeDisplaySnapshot)

#include <vector>
#include <atomic>
#include <algorithm>
#include <utility>

namespace curlop {

class AudioParamState {
public:
    struct Module {
        std::vector<float> shared;                                // shared[param_idx] — base values
        std::vector<std::pair<uint32_t, std::vector<float>>> stacked;  // (param_idx, values[voice_idx])
        int param_count = 0;
        int voice_count = 1;
        bool active = false;                                      // false = removed/unused slot
    };

    // An install builds this complete backing store on the message thread.
    // The callback adopts it with one vector swap; the displaced storage stays
    // owned by the consumed install for message-thread retirement.
    class PreparedState {
    public:
        void installSchemaSnapshot(uint32_t module_id, int voice_count,
                                   const std::vector<float>& values) {
            if (module_id >= modules_.size())
                modules_.resize(module_id + 1);
            auto& mod = modules_[module_id];
            mod.param_count = static_cast<int>(values.size());
            mod.voice_count = voice_count;
            mod.active = true;
            mod.shared.assign(values.begin(), values.end());
            mod.stacked.clear();
        }

        void removeModule(uint32_t module_id) {
            if (module_id >= modules_.size()) return;
            auto& mod = modules_[module_id];
            mod.active = false;
            mod.shared.clear();
            mod.stacked.clear();
            mod.param_count = 0;
            mod.voice_count = 1;
        }

    private:
        friend class AudioParamState;
        std::vector<Module> modules_;
    };

    void adoptPrepared(PreparedState& prepared) noexcept {
        modules_.swap(prepared.modules_);
    }

    // -- Called by processBlock at TOP of block (drains FIFO events) --

    // Handle a single FIFO event. Called from the drain loop.
    void handleEvent(const FIFOEvent& event) {
        switch (event.type) {
            case FIFOEvent::MODULE_ADDED:
                handleModuleAdded(event.moduleAdded);
                break;
            case FIFOEvent::MODULE_REMOVED:
                handleModuleRemoved(event.moduleRemoved);
                break;
            case FIFOEvent::PARAM_CHANGE:
                handleParamChange(event.paramChange);
                break;
        }
    }

    // -- B-141 Phase 3: SequencerInstall envelope direct-apply --
    // Install schema + values in one step, no zero window. Called by
    // VMRunner pendingInstall drain when an envelope's schemaSnapshots
    // land. Replaces the REMOVED → ADDED (zero) → ParamChange × N FIFO
    // sequence whose race surface caused R2 (modTree.evaluate reading
    // aps.shared = [0,…,0] for one block on every BYTE landing).
    //
    // shared.assign(begin, end) reuses existing capacity when size matches —
    // no allocation on value-only edits. Schema-changing edits reallocate
    // at the same cost as the existing handleModuleAdded path.
    void installSchemaSnapshot(uint32_t module_id, int voice_count,
                               const std::vector<float>& values) {
        ensureCapacity(module_id);
        auto& mod = modules_[module_id];
        mod.param_count = static_cast<int>(values.size());
        mod.voice_count = voice_count;
        mod.active = true;
        mod.shared.assign(values.begin(), values.end());
        mod.stacked.clear();
    }

    // Direct-remove for envelope's removedModuleIds. Same effect as
    // handleModuleRemoved(MODULE_REMOVED FIFO event).
    void removeModuleDirect(uint32_t module_id) {
        if (module_id >= modules_.size()) return;
        auto& mod = modules_[module_id];
        mod.active = false;
        mod.shared.clear();
        mod.stacked.clear();
        mod.param_count = 0;
        mod.voice_count = 1;
    }

    // -- Readers (audio thread only, during Phase 1 flat-fill) --

    // Get shared base value for a param. Returns 0.0f if module/param doesn't exist (AUDIO-02).
    float getShared(uint32_t module_id, uint32_t param_idx) const {
        if (module_id >= modules_.size()) return 0.0f;
        const auto& mod = modules_[module_id];
        if (!mod.active) return 0.0f;
        if (param_idx >= mod.shared.size()) return 0.0f;
        return mod.shared[param_idx];
    }

    // Get per-voice value. Falls back to shared if param is not stacked.
    float getVoice(uint32_t module_id, uint32_t param_idx, uint32_t voice_idx) const {
        if (module_id >= modules_.size()) return 0.0f;
        const auto& mod = modules_[module_id];
        if (!mod.active) return 0.0f;
        if (param_idx >= mod.shared.size()) return 0.0f;

        // Check stacked entries for this param
        for (const auto& entry : mod.stacked) {
            if (entry.first == param_idx) {
                if (voice_idx < entry.second.size()) {
                    return entry.second[voice_idx];
                }
                break;
            }
        }

        // Fall back to shared
        return mod.shared[param_idx];
    }

    // Check if a module exists and is active
    bool hasModule(uint32_t module_id) const {
        if (module_id >= modules_.size()) return false;
        return modules_[module_id].active;
    }

    // Get module info (read-only). Returns nullptr if module doesn't exist or is inactive.
    const Module* getModule(uint32_t module_id) const {
        if (module_id >= modules_.size()) return nullptr;
        const auto& mod = modules_[module_id];
        return mod.active ? &mod : nullptr;
    }

    // -- Display snapshot (written by processBlock, read by 120Hz timer) --
    // After Phase 1-3 completes, processBlock writes the first sample of each param buffer
    // into displayValues. The timer reads these for GUI feedback (knob arcs, lock indicators).
    // This replaces the 3rd copy of generator eval in timerCallback.

    // NOTE: displayValues is a FIXED array (not dynamic) because std::atomic arrays
    // cannot be resized safely on the audio thread. 64 modules is 2x the old MAX_MODULES=32
    // and generous for any realistic session. If exceeded, module 65+ simply won't show
    // display feedback — no crash, no audio impact. This is a GUI-only limitation.
    // The actual audio-thread storage (modules_ vector below) IS dynamic.
    static constexpr int DISPLAY_MAX_MODULES = 64;
    static constexpr int DISPLAY_MAX_PARAMS = 64;

    // Written by audio thread at end of processBlock phases
    std::atomic<float> displayValues[DISPLAY_MAX_MODULES][DISPLAY_MAX_PARAMS] = {};
    std::atomic<int> displayModuleCount { 0 };

    // Write display snapshot from paramBuffer after Phase 1-3.
    // Phase 26: method exists but is NOT called from processBlock.
    // Phase 27: processBlock calls this after Phase 1-3 completes.
    void writeDisplaySnapshot(const VMProcessData& data) {
        int count = std::min(data.moduleCount, DISPLAY_MAX_MODULES);
        for (int m = 0; m < count; ++m) {
            int paramLimit = std::min(static_cast<int>(VMProcessData::MAX_PARAMS),
                                      DISPLAY_MAX_PARAMS);
            for (int p = 0; p < paramLimit; ++p) {
                displayValues[m][p].store(data.modules[m].paramBuffer[p][0],
                                          std::memory_order_relaxed);
            }
        }
        displayModuleCount.store(count, std::memory_order_release);
    }

private:
    // Sparse storage — modules indexed by module_id.
    // Use vector with grow-on-demand. Most sessions use <16 modules.
    std::vector<Module> modules_;

    void ensureCapacity(uint32_t module_id) {
        if (module_id >= modules_.size()) {
            modules_.resize(module_id + 1);
        }
    }

    void handleModuleAdded(const ModuleAdded& event) {
        ensureCapacity(event.module_id);
        auto& mod = modules_[event.module_id];
        mod.param_count = event.param_count;
        mod.voice_count = event.voice_count;
        mod.active = true;
        // Zero-initialize all shared values (AUDIO-02: no NaN)
        mod.shared.assign(event.param_count, 0.0f);
        mod.stacked.clear();
    }

    void handleModuleRemoved(const ModuleRemoved& event) {
        if (event.module_id < modules_.size()) {
            auto& mod = modules_[event.module_id];
            mod.active = false;
            mod.shared.clear();
            mod.stacked.clear();
            mod.param_count = 0;
            mod.voice_count = 1;
        }
    }

    void handleParamChange(const ParamChange& event) {
        if (event.module_id >= modules_.size()) return;
        auto& mod = modules_[event.module_id];
        if (!mod.active) return;

        if (event.voice_idx == 0) {
            // Write to shared
            if (event.param_idx < mod.shared.size()) {
                mod.shared[event.param_idx] = event.value;
            }
        } else {
            // Write to stacked (per-voice)
            // Find existing stacked entry for this param
            for (auto& entry : mod.stacked) {
                if (entry.first == event.param_idx) {
                    if (event.voice_idx < entry.second.size()) {
                        entry.second[event.voice_idx] = event.value;
                    }
                    return;
                }
            }
            // Create new stacked entry — initialize all voices from shared base
            float baseVal = (event.param_idx < mod.shared.size())
                            ? mod.shared[event.param_idx] : 0.0f;
            std::vector<float> voices(mod.voice_count, baseVal);
            if (event.voice_idx < voices.size()) {
                voices[event.voice_idx] = event.value;
            }
            mod.stacked.emplace_back(event.param_idx, std::move(voices));
        }
    }
};

} // namespace curlop
