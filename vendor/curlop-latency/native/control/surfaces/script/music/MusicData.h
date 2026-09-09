#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// MusicData.h — C++ port of src/sequencer/musicData.js tables.
// Cut 0.D-3a: NOTE_NAMES + CHORDS + chordToIntervals. SCALES +
// quantizeToScale deferred to 0.D-3b (only needed for the `scale:` compile-
// time op which also lives in 3b scope).
// Float intervals (neutral, harm7, just_maj, etc.) are preserved — microtonal
// chords must not round to integer semitones.
// [-r DUMB-VIEW-INV-1]
// ═══════════════════════════════════════════════════════════════════════════

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace curlop::music {

// Chromatic note names (C = 0, B = 11).
inline constexpr std::array<std::string_view, 12> kNoteNames = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// Chord quality → interval array (semitones above root). Float values
// preserved for microtonal chords.
struct ChordEntry {
    std::string_view    name;
    std::vector<double> intervals;
};

// Full CHORDS table — 44 entries. Sourced byte-for-byte from
// src/sequencer/musicData.js CHORDS (Object.freeze(...)).
const std::vector<ChordEntry>& chordTable();

// Look up intervals by chord name. Returns empty vector on miss.
std::vector<double> chordToIntervals(std::string_view chordName);

// Scale table (Cut 0.D-3b). Interval values are doubles to preserve microtonal
// scales (slendro, maqam_rast, maqam_bayati, maqam_sikah, quarter_tone) that
// use half-flat / quarter-tone steps. Do NOT round to int.
struct ScaleEntry {
    std::string_view    name;
    std::vector<double> intervals;
};

// Full SCALES table — ports src/sequencer/musicData.js SCALES byte-for-byte.
const std::vector<ScaleEntry>& scaleTable();

// Look up scale by name (case-insensitive). Returns chromatic on miss, matching
// JS quantizeToScale's fallback to SCALES.chromatic.
const std::vector<double>& scaleIntervals(std::string_view scaleName);

// Quantize a pitch (Hz) to nearest degree of a named scale. Mirrors
// musicData.js:quantizeToScale — nearest-note snap with wrap-around. Never
// filters; always returns a valid Hz.
double quantizeToScale(double pitchHz, std::string_view scaleName,
                       std::string_view rootNote = "c");

// Root note name → chromatic index 0-11 (enharmonic flats normalized,
// case-insensitive). Unknown names → 0 (C), matching quantizeToScale.
// B-289 — the compiler resolves scale args with this; quantization itself
// happens in the machine (SCALE frame op).
int noteChroma(std::string_view rootNote);

} // namespace curlop::music
