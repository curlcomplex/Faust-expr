#pragma once
//
// ParamBufferCmd — main-thread → audio-thread command queue payload.
//
// Extracted from CurlopProcessor (s470) so VMRunnerHost can reference the
// FIFO type without pulling in CurlopProcessor.h. The drain happens on the
// audio thread inside VMRunner::drainPblockParamCommands; the push side is
// BridgeMessageHandler / EventRouter / CurlopProcessor itself.
//
// Lock-free SingleReader/SingleWriter discipline (choc::fifo).
//

#include <cstdint>

namespace curlop {

struct ParamBufferCmd {
    enum Type : uint8_t {
        SET_PARAM,          // Set one param slot to a value
        ZERO_GATES,         // Zero all gates (soft stop)
        HARD_RESET,         // Zero gates + clear all param locks/generators
        // B-1910: transport is an engine command, independent of a graph
        // install. A rejected graph must retain the current renderer (or
        // silence) while this command still starts the transport.
        START_TRANSPORT,
        SET_HARD_STOP_RAMP, // Set hard-stop fade ramp length
        CLEAR_PARAM_LOCKS,  // Clear the active slot's modTree (BYTE re-play)
        SET_MIDI_MAP,       // Map a MIDI CC to a module param
        CLEAR_MIDI_MAPS,    // Clear all MIDI CC mappings (clip switch)
        REBUILD_MOD_TREE,   // LIVE-02 stub: atomically rebuild modulation routing
    };
    Type type;
    int moduleIdx;   // SET_PARAM / SET_MIDI_MAP
    int paramIdx;    // SET_PARAM / SET_MIDI_MAP
    float value;     // SET_PARAM / SET_HARD_STOP_RAMP / SET_MIDI_MAP (minVal)
    // SET_MIDI_MAP extra fields packed into unused space:
    float value2;    // SET_MIDI_MAP: maxVal
    int8_t midiChannel;  // SET_MIDI_MAP: channel (0-15)
    uint8_t midiCC;      // SET_MIDI_MAP: CC number (0-127)
    bool midiIsOffset;   // SET_MIDI_MAP: true = offset from knob
    int8_t slotId = -1;  // Target slot (-1 = active slot). Phase 41: slot-addressed from day one.
    uint64_t transportStartSequence = 0; // zero = legacy/internal unconditional start
};

} // namespace curlop
