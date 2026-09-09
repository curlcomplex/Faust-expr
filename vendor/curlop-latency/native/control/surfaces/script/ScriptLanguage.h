#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// ScriptLanguage.h — C++-side script language schema (authority for the
// CodeMirror editor's schema payload, parser, and compiler).
//
// Introduced Cut 0.D-1. The schema is serialized by the native bridge and
// consumed by the Web editor; parsing and compilation are native.
//
// Simple tables live as constexpr arrays here. Complex tables (generators,
// articulation ops, OP_ARGS, sugar→canonical) live inline in the JSON
// serializer in ScriptLanguage.cpp — avoids constexpr gymnastics for nested
// variable-length specs. When 0.D-2 Parser + 0.D-3 Compiler need structured
// access, those tables graduate back into this header with appropriate
// runtime types.
//
// Consumed by:
//   - ProjectPersistence::dispatchPendingScriptLanguageSchema (this cut)
//   - ScriptParser (0.D-2)
//   - ScriptCompiler (0.D-3)
//
// [-r DUMB-VIEW-INV-1]
// ═══════════════════════════════════════════════════════════════════════════

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace curlop::script {

// ── Symbols ─────────────────────────────────────────────────────────────────
struct Symbols {
    static constexpr char module    = '#';
    static constexpr char variable  = '$';
    static constexpr char override_ = '@';    // `override` is a C++ keyword
    static constexpr char generator = '~';
    static constexpr char hold      = '_';
    static constexpr char rest      = '-';
    static constexpr char advance   = '/';
    static constexpr char assign    = '=';
    static constexpr char dot       = '.';
    static constexpr char colon     = ':';
    static constexpr char comma     = ',';
    static constexpr char bang      = '!';
    static constexpr char plus      = '+';
};

inline constexpr std::array<char, 8> kTriggerCharacters = {
    '#', '$', '@', '~', '.', ':', '(', ' '
};

// ── Standard processors (canonical + sugar aliases per CurlopLanguage.js) ──
// Order preserved from JS Set-insertion order: canonical block, then sugar
// keys not already present.
inline constexpr std::array<std::string_view, 39> kStandardProcessors = {
    "velocity",
    "probability", "step", "len", "poly", "stack",
    "ratchet", "bpm", "pan", "decay", "scale", "seed", "cond", "cent",
    "onset",
    "transpose", "octave", "invert",
    "reverse", "rotate", "shuffle", "sort",
    "timescale", "grid",
    "flam", "strum", "geiger", "bounce", "buzz",
    "arp", "groove", "bernoulli", "deviate",
    "glide",
    "vel", "prob", "repeat", "porta", "portamento"
};

// ── Conditional values ──────────────────────────────────────────────────────
inline constexpr std::array<std::string_view, 11> kCondValues = {
    "first", "!first", "even", "odd", "prime", "fib",
    "previous", "!previous", "silence", "held", "changed"
};

inline constexpr std::array<std::string_view, 4> kCondArgValues = {
    "mod", "every", "once", "after"
};

// ── No-arg ops ──────────────────────────────────────────────────────────────
inline constexpr std::array<std::string_view, 2> kNoArgOps = { "reverse", "shuffle" };

// ── Curve types ─────────────────────────────────────────────────────────────
inline constexpr std::array<std::string_view, 4> kCurveTypes = {"lin", "exp", "log", "eqpow"};

// ── Special identifiers ─────────────────────────────────────────────────────
inline constexpr std::string_view kOutputName = "out";

inline constexpr std::array<std::string_view, 17> kNoteNames = {
    "c", "d", "e", "f", "g", "a", "b",
    "c#", "d#", "f#", "g#", "a#",
    "db", "eb", "gb", "ab", "bb"
};

inline constexpr std::array<int, 11> kOctavePriority = {3, 4, 2, 5, 1, 6, 0, 7, -1, 8, -2};

inline constexpr std::array<std::string_view, 16> kChordNames = {
    "maj", "min", "dim", "aug", "sus", "dom",
    "maj7", "min7", "dim7", "aug7", "sus4", "sus2",
    "dom7", "maj9", "min9", "dom9"
};

// ── Sugar → canonical name mapping (port of SUGAR_TO_CANONICAL, 0.D-2b) ────
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kSugarToCanonical = {{
    {"vel",        "velocity"},
    {"prob",       "probability"},
    {"porta",      "glide"},
    {"portamento", "glide"},
}};

inline std::string_view canonicalize(std::string_view name)
{
    for (auto const& p : kSugarToCanonical)
        if (p.first == name) return p.second;
    return name;
}

// ── Step-level processor set (per Parser.js:1142-1146) ─────────────────────
// Ops listed here are routed to step.processors even in the implicit/outer
// sequence context (isImplicit=true, single event). The compiler reads
// arp/strum via tryEmitStepExpansion and scale via extractCompileTimeOps —
// all from step.processors only with no event fallback — so they must land
// there for bare-form steps like `$out = #kick.c3.min7 arp(up)` to work.
inline constexpr std::array<std::string_view, 16> kStepLevelProcs = {
    "step", "prob", "repeat",
    "transpose", "octave", "invert", "reverse", "rotate", "shuffle",
    "sort", "timescale", "grid", "groove",
    "arp", "strum", "scale"
};

inline bool isStepLevelProc(std::string_view name)
{
    for (auto const& n : kStepLevelProcs)
        if (n == name) return true;
    return false;
}

// ── JSON serializer ─────────────────────────────────────────────────────────
// Produces the body of the SCRIPT_LANGUAGE_SCHEMA payload (fields only —
// caller adds the opening `{"type":"SCRIPT_LANGUAGE_SCHEMA",` envelope prefix
// and the closing `}`).
std::string buildScriptLanguageSchemaJson();

} // namespace curlop::script
