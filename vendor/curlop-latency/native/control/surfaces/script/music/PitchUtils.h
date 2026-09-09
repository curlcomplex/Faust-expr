#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// PitchUtils.h — C++ port of src/sequencer/pitchUtils.js (subset).
//
// Introduced Cut 0.D-2. Ports the 4 functions called by Parser.js internally:
//   noteToMidi, parseCents, parseFreq, semiToHz.
//
// resolvePitch + CHORD_INTERVALS (+ musicData.js) DEFERRED to 0.D-3 — they
// are only consumed by BytecodeCompiler.js (and tests), not the parser.
//
// [-r DUMB-VIEW-INV-1]
// ═══════════════════════════════════════════════════════════════════════════

#include <juce_core/juce_core.h>
#include <vector>

namespace curlop::script { struct PitchChain; }

namespace curlop::pitch {

// Convert a note string (e.g. "c3", "d#4", "gb2") to a MIDI semitone.
// Convention: standard MIDI — C4=60, C3=48, C0=12.
// Returns -1 on invalid input (JS throws; C++ stays in-band per parser
// never-throw guarantee. Caller emits ScriptDiagnostic on -1.)
int noteToMidi(const juce::String& noteStr);

// Convert a semitone number to Hz using A4=440Hz tuning (standard MIDI A4=69).
double semiToHz(double semitone);

// Parse a cents string ('+10c' → 10, '-5c' → -5, '23c' → 23).
double parseCents(const juce::String& centsStr);

// Parse a frequency string ('440hz' → 440, '1khz' → 1000).
double parseFreq(const juce::String& freqStr);

// Cut 0.D-3a: resolve a PitchChain to scalar form, mirror of JS
// pitchUtils.js resolvePitch.
// - root_semitone: MIDI semitone if note present, -1 otherwise.
// - freq_hz: parsed Hz if freq present, -1 otherwise.
// - cents_offset: cents applied (0 if absent).
// - voicings: root_semitone + chord intervals; empty if no note or no chord
//   expansion (3a simple ops never consume voicings — 3b chord expansion will).
// - final_pitch: freq_hz if present, else root_semitone + cents_offset/100,
//   else -1.0 (sentinel).
struct ResolvedPitch {
    int                 root_semitone = -1;
    double              freq_hz       = -1.0;
    double              cents_offset  = 0.0;
    double              final_pitch   = -1.0;
    std::vector<double> voicings;
    bool                hasFreq  = false;
    bool                hasPitch = false;
};

ResolvedPitch resolvePitch(const curlop::script::PitchChain& chain);

// Convenience — mirror of BytecodeCompiler.js:110 resolvePitchHz. Returns
// 440Hz (A4) fallback if neither freq nor note resolve.
double resolvePitchHz(const curlop::script::PitchChain& chain);

} // namespace curlop::pitch
