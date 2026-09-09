// F-066 — the rebuilt VM's value model (adr-vm "How signals move").
//
// Every signal on the graph is normalized bipolar ±1. Pitch is 0.1 per
// octave with 0 = middle C (MIDI 60); a semitone is 1/120, so ±1 spans
// ±10 octaves around middle C. Music logic (scales, chords, membership)
// happens in exact note space inside ops; the wire carries the normalized
// value. MIDI 2.0 conversion at the edge is a multiply.
//
// Zero-JUCE, std-only: this library must run offline (program in, exact
// output out, no app, no audio device).
#pragma once

#include <cstdint>

namespace curlop::vm {

// Thin port-type taxonomy (adr-vm): every wire is the same ±1 float stream;
// the type tells the port how to interpret and the display how to render.
enum class SignalType : uint8_t {
    Value    = 0,   // generic control
    Pitch    = 1,   // 0.1/octave, 0 = middle C
    Gate     = 2,   // unipolar intensity; see isGateOpen()
    Phase    = 3,   // 0..1
    Velocity = 4,
    Audio    = 5,
};

// How a value composes at its destination (adr-vm composition rule):
// Set replaces (last wins), Offset adds (several sum), Stream is the dense
// per-frame carrier. Phase 1 emits Set packets; streams arrive with the
// control ops (Phase 2) and delivery (Phase 3).
enum class PacketKind : uint8_t {
    Set     = 0,
    Offset  = 1,
    Stream  = 2,
    // End of the emitting op's contribution on this lane (a step lock
    // expiring at step end). Delivery (Phase 3) composes it as "this
    // source no longer participates" — CURLOP locks are step-scoped.
    Release = 3,
};

constexpr uint32_t kNamedOutputLaneFlag = 0x80000000u;
inline uint32_t namedOutputLane(uint32_t lane) noexcept
{
    return lane | kNamedOutputLaneFlag;
}
inline bool isNamedOutputLane(uint32_t lane) noexcept
{
    return (lane & kNamedOutputLaneFlag) != 0;
}
inline uint32_t namedOutputLaneIndex(uint32_t lane) noexcept
{
    return lane & ~kNamedOutputLaneFlag;
}

// ── Pitch conversions ─────────────────────────────────────────────
// Note space: floating semitones relative to middle C (exact for integer
// notes, fractional values carry microtonal scales/chords).

constexpr float kSemitone = 1.0f / 120.0f;   // one semitone on the wire
constexpr float kOctave   = 0.1f;            // one octave on the wire

// The one gate boundary used by the VM, delivery adapters, and graph sinks.
// Values at or below epsilon are closed; any larger positive value is an
// open gate whose magnitude remains available as continuous intensity.
constexpr float kGateEpsilon = 1.0e-6f;

constexpr bool isGateOpen(float value) noexcept
{
    return value > kGateEpsilon;
}

constexpr float noteToSignal(float semitonesFromMiddleC) noexcept
{
    return semitonesFromMiddleC * kSemitone;
}

constexpr float signalToNote(float signal) noexcept
{
    return signal * 120.0f;
}

// MIDI interop helpers (middle C = MIDI 60).
constexpr float midiToNote(float midi) noexcept   { return midi - 60.0f; }
constexpr float noteToMidi(float note) noexcept   { return note + 60.0f; }

} // namespace curlop::vm
