#pragma once

// ParamTypes.h — Shared type definitions for the param pipeline.
// Used by ParamStore, ParamFIFO, and AudioParamState.
// No JUCE or Elementary dependencies — STL only.

#include <cstdint>
#include <string>

namespace curlop {

// -- ParamDescriptor: schema for one param --
struct ParamDescriptor {
    std::string name;              // "CUTOFF", "DECAY", etc.
    float min = 0.0f;
    float max = 1.0f;
    float defaultValue = 0.5f;
    // unit and scale omitted — UI concerns, not pipeline concerns
};

// -- ParamChange: single param change event for the FIFO --
struct ParamChange {
    uint32_t module_id;
    uint32_t param_idx;
    uint32_t voice_idx;            // 0 for shared, >0 for per-voice stacked
    float value;
};

// -- ModuleAdded: structural change — audio thread must allocate storage --
struct ModuleAdded {
    uint32_t module_id;
    uint32_t param_count;
    uint32_t voice_count;
};

// -- ModuleRemoved: structural change — audio thread must free storage --
struct ModuleRemoved {
    uint32_t module_id;
};

// -- FIFOEvent: tagged union of all events the FIFO can carry --
struct FIFOEvent {
    enum Type : uint8_t {
        PARAM_CHANGE,              // base value update
        MODULE_ADDED,              // audio thread must allocate storage
        MODULE_REMOVED,            // audio thread must free storage
    };

    Type type;

    union {
        ParamChange paramChange;
        ModuleAdded moduleAdded;
        ModuleRemoved moduleRemoved;
    };

    // Convenience factory methods
    static FIFOEvent makeParamChange(uint32_t mod, uint32_t param, uint32_t voice, float val) {
        FIFOEvent e;
        e.type = PARAM_CHANGE;
        e.paramChange.module_id = mod;
        e.paramChange.param_idx = param;
        e.paramChange.voice_idx = voice;
        e.paramChange.value = val;
        return e;
    }

    static FIFOEvent makeModuleAdded(uint32_t mod, uint32_t paramCount, uint32_t voiceCount) {
        FIFOEvent e;
        e.type = MODULE_ADDED;
        e.moduleAdded.module_id = mod;
        e.moduleAdded.param_count = paramCount;
        e.moduleAdded.voice_count = voiceCount;
        return e;
    }

    static FIFOEvent makeModuleRemoved(uint32_t mod) {
        FIFOEvent e;
        e.type = MODULE_REMOVED;
        e.moduleRemoved.module_id = mod;
        return e;
    }
};

} // namespace curlop
