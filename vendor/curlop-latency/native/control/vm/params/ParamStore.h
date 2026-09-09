#pragma once

// ParamStore.h — Canonical message-thread param store.
// All param writers (knob, MIDI, clip load, @ override) go through this.
// Dynamic sizing (no MAX_MODULES or MAX_PARAMS). Sparse per-voice storage.
// Pushes FIFOEvent to ParamFIFO on every write.

#include "control/vm/params/ParamFIFO.h"

#include <unordered_map>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <cstdio>

namespace curlop {

class ParamStore {
public:
    struct ModuleParams {
        std::string dsl_name;                                      // "kick", "ref" — serialization key
        std::vector<ParamDescriptor> schema;                       // ordered param descriptors
        int voice_count = 1;
        std::vector<float> shared;                                 // shared[param_idx] — base values
        std::unordered_map<uint32_t, std::vector<float>> stacked;  // param_idx -> float[voice_count]
    };

    struct SnapshotParam {
        std::string name;
        float value = 0.0f;
    };

    struct SnapshotModule {
        uint32_t module_id = 0;
        std::string dsl_name;
        std::vector<SnapshotParam> params;
    };

    // ---- Writers (all message thread) ----

    // Register a module. On first registration, initializes to schema defaults.
    // On re-registration (same module_id), preserves existing values for surviving params
    // (matched by name). New params get schema defaults, removed params are dropped.
    // Pushes ModuleAdded + ParamChange events to FIFO (if fifo is set).
    void addModule(uint32_t module_id, const std::string& dsl_name,
                   const std::vector<ParamDescriptor>& schema, int voice_count = 1) {
        auto existing = modules_.find(module_id);
        if (existing != modules_.end() && existing->second.dsl_name == dsl_name) {
            // Re-registration of same module — preserve values for surviving params.
            // This happens on live recompile (updateAST sends BYTE again).
            updateSchema(module_id, schema);
            return;
        }

        ModuleParams mp;
        mp.dsl_name = dsl_name;
        mp.schema = schema;
        mp.voice_count = voice_count;
        mp.shared.resize(schema.size());

        // Initialize shared values to schema defaults
        for (size_t i = 0; i < schema.size(); ++i) {
            mp.shared[i] = schema[i].defaultValue;
        }

        modules_[module_id] = std::move(mp);

        // Push MODULE_ADDED to FIFO
        pushToFifo(FIFOEvent::makeModuleAdded(module_id,
                   static_cast<uint32_t>(schema.size()),
                   static_cast<uint32_t>(voice_count)));

        // Push initial ParamChange events for all defaults
        for (size_t i = 0; i < schema.size(); ++i) {
            pushToFifo(FIFOEvent::makeParamChange(module_id,
                       static_cast<uint32_t>(i), 0, schema[i].defaultValue));
        }
    }

    // Remove a module. Pushes ModuleRemoved to FIFO.
    void removeModule(uint32_t module_id) {
        modules_.erase(module_id);
        pushToFifo(FIFOEvent::makeModuleRemoved(module_id));
    }

    // B-258: drop ALL modules (message thread). The RESET rebuild path calls
    // this before re-registering the new clip's modules so paramStore never
    // accumulates orphan entries from a previous clip/project across
    // clip-switches or project loads — without it, a 13-module clip followed
    // by a 6-module clip leaves the 7 extras in the store (stale base values,
    // misleading snapshots, unbounded growth). Pushes ModuleRemoved per module
    // so the FIFO-synced AudioParamState drops them too. Safe on the RESET
    // path: that runs on a Compiling/fresh slot, not the live Active slot the
    // audio thread reads.
    void clear() {
        for (const auto& kv : modules_)
            pushToFifo(FIFOEvent::makeModuleRemoved(kv.first));
        modules_.clear();
    }

    // Set a single param value. Pushes ParamChange to FIFO.
    // voice=0 writes to shared. voice>0 writes to stacked (creates stacked entry if needed).
    void set(uint32_t module_id, uint32_t param_idx, float value, uint32_t voice = 0) {
        auto it = modules_.find(module_id);
        if (it == modules_.end()) return;

        auto& mp = it->second;
        if (param_idx >= mp.shared.size()) return;

        if (voice == 0) {
            mp.shared[param_idx] = value;
        } else {
            // Create stacked entry if needed
            auto& stack = mp.stacked[param_idx];
            if (stack.empty()) {
                // Initialize stacked array: copy shared value for all voices
                stack.resize(mp.voice_count, mp.shared[param_idx]);
            }
            if (voice < stack.size()) {
                stack[voice] = value;
            }
        }

        pushToFifo(FIFOEvent::makeParamChange(module_id, param_idx, voice, value));
    }

    // Bulk set all params for a module (clip load, PARAM_INIT).
    // Pushes one ParamChange per param.
    void setBulk(uint32_t module_id, const std::vector<float>& values) {
        auto it = modules_.find(module_id);
        if (it == modules_.end()) return;

        auto& mp = it->second;
        size_t count = std::min(values.size(), mp.shared.size());
        for (size_t i = 0; i < count; ++i) {
            mp.shared[i] = values[i];
            pushToFifo(FIFOEvent::makeParamChange(module_id,
                       static_cast<uint32_t>(i), 0, values[i]));
        }
    }

    // Update module schema (live coding). Match params by name:
    // - Surviving params keep their current values
    // - New params get schema default
    // - Removed params are gone
    // Pushes ModuleRemoved + ModuleAdded + ParamChange events.
    void updateSchema(uint32_t module_id, const std::vector<ParamDescriptor>& newSchema) {
        auto it = modules_.find(module_id);
        if (it == modules_.end()) return;

        auto& mp = it->second;

        // Build name->value map from current state
        std::unordered_map<std::string, float> preserved;
        for (size_t i = 0; i < mp.schema.size() && i < mp.shared.size(); ++i) {
            preserved[mp.schema[i].name] = mp.shared[i];
        }

        // Push MODULE_REMOVED (audio thread frees old storage)
        pushToFifo(FIFOEvent::makeModuleRemoved(module_id));

        // Update schema and shared values
        mp.schema = newSchema;
        mp.shared.resize(newSchema.size());
        mp.stacked.clear();  // stacked entries invalidated by schema change

        for (size_t i = 0; i < newSchema.size(); ++i) {
            auto found = preserved.find(newSchema[i].name);
            if (found != preserved.end()) {
                mp.shared[i] = found->second;  // Survivor keeps value
            } else {
                mp.shared[i] = newSchema[i].defaultValue;  // New param gets default
            }
        }

        // Push MODULE_ADDED + ParamChange for new layout
        pushToFifo(FIFOEvent::makeModuleAdded(module_id,
                   static_cast<uint32_t>(newSchema.size()),
                   static_cast<uint32_t>(mp.voice_count)));

        for (size_t i = 0; i < newSchema.size(); ++i) {
            pushToFifo(FIFOEvent::makeParamChange(module_id,
                       static_cast<uint32_t>(i), 0, mp.shared[i]));
        }
    }

    // ---- B-141 Phase 3: SequencerInstall envelope silent install ----
    // Update message-thread state (schema + shared values) for a module
    // WITHOUT pushing REMOVED+ADDED+ParamChange × N to the FIFO. The
    // SequencerInstall envelope carries the values to the audio thread
    // via AudioParamState::installSchemaSnapshot directly — no FIFO churn,
    // no zero window.
    //
    // Caller (BridgeMessageHandler liveRecompile path) builds `values`
    // from prior paramStore state (preserve-by-name) + graphContainer
    // overlay + paramMappings.defaultValue. Schema is the new authoritative
    // ordering. shared values are aligned 1:1 with schema.
    //
    // Cold-load / Compiling-slot landings still use addModule (FIFO path)
    // until Phase 6 collapses them.
    void installSchemaSnapshot(uint32_t module_id, const std::string& dsl_name,
                               const std::vector<ParamDescriptor>& schema,
                               const std::vector<float>& values,
                               int voice_count = 1) {
        auto& mp = modules_[module_id];
        mp.dsl_name = dsl_name;
        mp.schema = schema;
        mp.voice_count = voice_count;
        mp.shared.assign(values.begin(), values.end());
        mp.stacked.clear();
        // No FIFO push — audio thread receives schema+values via envelope.
    }

    // ---- Readers ----

    float get(uint32_t module_id, uint32_t param_idx, uint32_t voice = 0) const {
        auto it = modules_.find(module_id);
        if (it == modules_.end()) return 0.0f;

        const auto& mp = it->second;
        if (param_idx >= mp.shared.size()) return 0.0f;

        if (voice > 0) {
            auto stackIt = mp.stacked.find(param_idx);
            if (stackIt != mp.stacked.end() && voice < stackIt->second.size()) {
                return stackIt->second[voice];
            }
            // Fall back to shared if not stacked
        }

        return mp.shared[param_idx];
    }

    const ModuleParams* getModule(uint32_t module_id) const {
        auto it = modules_.find(module_id);
        return (it != modules_.end()) ? &it->second : nullptr;
    }

    bool hasModule(uint32_t module_id) const {
        return modules_.find(module_id) != modules_.end();
    }

    const std::unordered_map<uint32_t, ModuleParams>& modules() const { return modules_; }

    std::vector<SnapshotModule> snapshotModules() const {
        std::vector<SnapshotModule> out;
        out.reserve(modules_.size());
        for (const auto& [id, mp] : modules_) {
            SnapshotModule module;
            module.module_id = id;
            module.dsl_name = mp.dsl_name;
            const size_t count = std::min(mp.schema.size(), mp.shared.size());
            module.params.reserve(count);
            for (size_t i = 0; i < count; ++i)
                module.params.push_back({ mp.schema[i].name, mp.shared[i] });
            out.push_back(std::move(module));
        }
        return out;
    }

    // ---- Serialization (for getStateInformation) ----
    // Returns JSON string: { "kick": { "DECAY": 500, "TONE": 0.3 }, "ref": { ... } }
    std::string serializeToJson() const {
        std::ostringstream ss;
        ss << "{\n";
        bool firstModule = true;
        for (const auto& [id, mp] : modules_) {
            if (!firstModule) ss << ",\n";
            firstModule = false;
            ss << "  \"" << escapeJson(mp.dsl_name) << "\": {\n";
            bool firstParam = true;
            for (size_t i = 0; i < mp.schema.size() && i < mp.shared.size(); ++i) {
                if (!firstParam) ss << ",\n";
                firstParam = false;
                ss << "    \"" << escapeJson(mp.schema[i].name) << "\": " << mp.shared[i];
            }
            ss << "\n  }";
        }
        ss << "\n}";
        return ss.str();
    }

    std::string serializeSnapshotEntriesToJson() const {
        std::ostringstream ss;
        ss << "[";
        bool firstModule = true;
        for (const auto& module : snapshotModules()) {
            if (!firstModule) ss << ",";
            firstModule = false;
            ss << "{\"moduleId\":" << module.module_id
               << ",\"dslName\":\"" << escapeJson(module.dsl_name) << "\""
               << ",\"params\":[";
            bool firstParam = true;
            for (const auto& param : module.params) {
                if (!firstParam) ss << ",";
                firstParam = false;
                ss << "{\"name\":\"" << escapeJson(param.name) << "\",\"value\":"
                   << param.value << "}";
            }
            ss << "]}";
        }
        ss << "]";
        return ss.str();
    }

    // ---- FIFO connection ----
    // The store doesn't own the FIFO — it's injected. Nullable (for unit testing without FIFO).
    void setFIFO(ParamFIFO* fifo) { fifo_ = fifo; }

private:
    std::unordered_map<uint32_t, ModuleParams> modules_;
    ParamFIFO* fifo_ = nullptr;

    void pushToFifo(const FIFOEvent& event) {
        if (fifo_) fifo_->push(event);
    }

    // JSON string escaping for legacy object-key snapshots.
    static std::string escapeJson(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        return out;
    }

};

} // namespace curlop
