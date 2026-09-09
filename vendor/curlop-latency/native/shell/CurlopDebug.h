#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// CurlopDebug — Permanent Diagnostic Flag System
//
// Three tiers of diagnostics, all permanent in the codebase:
//
//   Tier 0: Always on — errors, crashes. Just fprintf(stderr) directly.
//   Tier 1: Runtime toggleable — message handlers, state transitions.
//           Toggled via MCP curlop_set_debug tool. Zero rebuild needed.
//   Tier 2: Compile-time — processBlock, per-sample. constexpr bool.
//           Requires rebuild. Zero overhead when off.
//
// Usage:
//   CDBG(APG_EDGES, "added connection src=%d dst=%d", src, dst);
//   CDBG_IF(BYTE_HANDLER, moduleCount > 16, "unusual module count: %d", mc);
//
//   // Tier 2 (hot path):
//   if constexpr (CurlopDebug::PROCESSBLOCK) { ... }
//
// Session 236 — permanent infrastructure, never remove.
// ═══════════════════════════════════════════════════════════════════════════

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "io/CurlopEventLog.h"

namespace CurlopDebug {

// ── Tier 1: Runtime flags (message thread, cold path) ──────────────
// ponytail: two uint64 words keep the old low-word wire path and give the
// next 64 debug flags somewhere to live without changing CDBG call sites.

struct Flag {
    uint8_t word;
    uint8_t bitIndex;
    uint64_t bit;
};

struct Mask {
    uint64_t lo = 0;
    uint64_t hi = 0;
};

inline std::atomic<uint64_t> flagsLo{0};
inline std::atomic<uint64_t> flagsHi{0};

#define CURLOP_DEBUG_FLAG_LIST(X) \
    X(GRAPH_SYNC, 0, 0) \
    X(APG_EDGES, 0, 1) \
    X(BYTE_HANDLER, 0, 2) \
    X(TRANSPORT, 0, 3) \
    X(PSET, 0, 4) \
    X(PARAM_STORE, 0, 5) \
    X(SLOT_POOL, 0, 6) \
    X(PERSISTENCE, 0, 7) \
    X(APG_BUILD, 0, 8) \
    X(PRMI, 0, 9) \
    X(RENDER_DRAIN, 0, 10) \
    X(MCP_CMD, 0, 11) \
    X(BRIDGE, 0, 12) \
    X(WIRE_ORDER, 0, 13) \
    X(SCRIPT_PARSER, 0, 14) \
    X(EMBED_COMPILE, 0, 15) \
    X(SCHEDULER, 0, 16) \
    X(VIEW_PLAY, 0, 17) \
    X(CLIP_LAUNCHER, 0, 18) \
    X(RECORDER, 0, 19) \
    X(MIDI_IN, 0, 20) \
    X(TRANSPORT_UPDATE, 0, 21) \
    X(CPU_TIMING, 0, 22) \
    X(GUI_STATE_MGR, 0, 23) \
    X(NATIVE_PANEL, 0, 24) \
    X(WIDGET_GESTURE, 0, 25) \
    X(NODE_CANVAS, 0, 26) \
    X(VIEWPORT, 0, 27) \
    X(UNDO_MGR, 0, 28) \
    X(COMMAND_MGR, 0, 29) \
    X(SMART_WIRE, 0, 30) \
    X(CONTEXT_MENU, 0, 31) \
    X(B141_PROBE, 0, 32) \
    X(SNAPSHOTS, 0, 33) \
    X(MULTI_PROG, 0, 34) \
    X(AUDIO_TAP, 0, 35) \
    X(SCHEDULER_TRACE, 0, 36) \
    X(PAINT_PERF, 0, 37) \
    X(NODE_PAINT_PERF, 0, 38) \
    X(FAUST_SCHEMA, 0, 44) \
    X(PARAM_COMPOSE, 0, 45) \
    X(PARAM_ACCEPT, 0, 46) \
    X(LIMITER_CLIP, 0, 47) \
    X(LOCK_ANCHOR, 0, 48) \
    X(VOICE_FANOUT, 0, 49) \
    X(MIDI_OUT, 0, 50) \
    X(GATE_EDGE, 0, 51) \
    X(FAUST_RUNTIME, 0, 52) \
    X(MULTITRACK, 0, 53) \
    X(PITCH_EMIT, 0, 54) \
    X(PERF, 0, 55) \
    X(CONTROL_TAP, 0, 56) \
    X(HOST_TRANSPORT, 0, 57) \
    X(APG_BIND, 0, 58) \
    X(PARAM_SOCKET_DISPLAY, 0, 59) \
    X(PARAM_UI, 0, 60) \
    X(FAUST_PARAM_INPUT, 0, 61) \
    X(STEPSEQ_PLAYHEAD, 0, 62) \
    X(VISUALIZER_TAP, 0, 63) \
    X(VISUALIZER_RENDER, 1, 0) \
    X(VISUALIZER_COMPOSITOR, 1, 1) \
    X(FACEPLATE_UI, 1, 2) \
    X(MIDI_ROUTE, 1, 3)

#define CURLOP_DECLARE_DEBUG_FLAG(name, word_, bit_) \
    inline constexpr Flag name{static_cast<uint8_t>(word_), static_cast<uint8_t>(bit_), 1ull << (bit_)};
CURLOP_DEBUG_FLAG_LIST(CURLOP_DECLARE_DEBUG_FLAG)
#undef CURLOP_DECLARE_DEBUG_FLAG

inline bool on(Flag f) {
    const auto word = f.word == 0 ? flagsLo.load(std::memory_order_relaxed)
                                  : flagsHi.load(std::memory_order_relaxed);
    return (word & f.bit) != 0;
}

inline void set(uint64_t mask) {
    flagsLo.store(mask, std::memory_order_relaxed);
}

inline void set(Mask mask) {
    flagsLo.store(mask.lo, std::memory_order_relaxed);
    flagsHi.store(mask.hi, std::memory_order_relaxed);
}

inline void enable(Flag f) {
    auto& word = f.word == 0 ? flagsLo : flagsHi;
    word.fetch_or(f.bit, std::memory_order_relaxed);
}

inline void disable(Flag f) {
    auto& word = f.word == 0 ? flagsLo : flagsHi;
    word.fetch_and(~f.bit, std::memory_order_relaxed);
}

inline uint64_t get() {
    return flagsLo.load(std::memory_order_relaxed);
}

inline Mask getMask() {
    return { flagsLo.load(std::memory_order_relaxed),
             flagsHi.load(std::memory_order_relaxed) };
}

// ── Tier 2: Compile-time flags (audio thread, hot path) ────────────
// constexpr false = compiler strips entire branch. Zero instructions.
// Toggle requires rebuild. Use for processBlock / per-sample diagnostics.

// SF-049 (T-312): PROCESSBLOCK / APG_RENDER promoted out — per-block audio
// levels are now the runtime AUDIO_TAP flag; per-block note/param events are
// the runtime MULTI_PROG "vm-prog" tap. SCHEDULER_TRACE promoted to a runtime
// Flag bit (cold-path FIFO drain, no audio-thread cost). Only per-sample work
// stays compile-time here.
constexpr bool SAMPLE_LEVEL   = false;  // Per-sample: individual sample values (EXTREME overhead — rebuild to toggle)

// ── Flag name table (for MCP get_debug_flags) ──────────────────────

struct FlagInfo {
    const char* name;
    uint8_t     word;
    uint8_t     bitIndex;
    uint64_t    bit;
};

inline constexpr FlagInfo FLAG_TABLE[] = {
#define CURLOP_DEBUG_FLAG_INFO(name, word_, bit_) { #name, static_cast<uint8_t>(word_), static_cast<uint8_t>(bit_), 1ull << (bit_) },
    CURLOP_DEBUG_FLAG_LIST(CURLOP_DEBUG_FLAG_INFO)
#undef CURLOP_DEBUG_FLAG_INFO
};

inline constexpr int FLAG_COUNT = sizeof(FLAG_TABLE) / sizeof(FLAG_TABLE[0]);

// Look up flag bit by name. Returns 0 if not found.
inline uint64_t flagByName(const char* name) {
    for (int i = 0; i < FLAG_COUNT; ++i)
        if (strcmp(FLAG_TABLE[i].name, name) == 0)
            return FLAG_TABLE[i].word == 0 ? FLAG_TABLE[i].bit : 0;
    return 0;
}

} // namespace CurlopDebug

// ── Convenience macros ─────────────────────────────────────────────
// CDBG(FLAG, fmt, ...) — guarded fprintf to stderr with flag tag prefix.
// CDBG_IF(FLAG, cond, fmt, ...) — same but with additional boolean guard.
// CDBG_RT(FLAG, fmt, ...) — audio-thread variant: bounded stack formatting
// plus non-blocking event-log handoff. No stderr, locks, allocation, or file I/O
// on the caller thread.

// 1KB stack buffer per CDBG fire. Cold-path CDBG messages are short (<200B
// typical). Format-then-emit avoids a second printf round through stderr;
// keeps the literal string for both the stderr write and the JSONL emit.
#define CDBG(flag, fmt, ...) \
    do { if (CurlopDebug::on(CurlopDebug::flag)) { \
        char _cdbg_buf[1024]; \
        std::snprintf(_cdbg_buf, sizeof(_cdbg_buf), fmt, ##__VA_ARGS__); \
        fprintf(stderr, "[" #flag "] %s\n", _cdbg_buf); \
        fflush(stderr); \
        CurlopEventLog::emit(#flag, _cdbg_buf); \
    } } while(0)

#define CDBG_IF(flag, cond, fmt, ...) \
    do { if ((cond) && CurlopDebug::on(CurlopDebug::flag)) { \
        char _cdbg_buf[1024]; \
        std::snprintf(_cdbg_buf, sizeof(_cdbg_buf), fmt, ##__VA_ARGS__); \
        fprintf(stderr, "[" #flag "] %s\n", _cdbg_buf); \
        fflush(stderr); \
        CurlopEventLog::emit(#flag, _cdbg_buf); \
    } } while(0)

#define CDBG_RT(flag, fmt, ...) \
    do { if (CurlopDebug::on(CurlopDebug::flag)) { \
        char _cdbg_buf[768]; \
        std::snprintf(_cdbg_buf, sizeof(_cdbg_buf), fmt, ##__VA_ARGS__); \
        (void) CurlopEventLog::emitRealtime(#flag, _cdbg_buf); \
    } } while(0)

// ── Tier 1.5: SNAPSHOT stderr tags ─────────────────────────────────
// Structured snapshot emits consumed by scripts/q/sidecar.py. Compile-
// stripped in Release (no JSON build, no fprintf). Enabled in dev via
// CMake generator expression: $<$<NOT:$<CONFIG:Release>>:CURLOP_DIAGNOSTICS=1>.
//
// CDBG_SNAPSHOT_J — juce::String payload (has .length() + .toRawUTF8())
// CDBG_SNAPSHOT_S — std::string payload (has .size() + .c_str())
// CDBG_DIAGNOSTIC_FPRINTF — escape hatch for inline-format snapshots
//   (e.g. GUI_EVAL_RESULT error cases that sprintf token+error inline).

// s380: snapshot emits now runtime-gated by SNAPSHOTS flag in addition to
// the compile-time CURLOP_DIAGNOSTICS guard. Default off — file stays empty
// at startup until MCP curlop_set_debug SNAPSHOTS true.
#ifdef CURLOP_DIAGNOSTICS
  #define CDBG_SNAPSHOT_J(KIND, juce_string_expr) do { \
      if (! CurlopDebug::on(CurlopDebug::SNAPSHOTS)) break; \
      auto _cdbg_snap_json = (juce_string_expr); \
      const auto _cdbg_snap_bytes = static_cast<std::size_t> (_cdbg_snap_json.getNumBytesAsUTF8()); \
      if (_cdbg_snap_bytes < 8192) \
          fprintf(stderr, "[SNAPSHOT " #KIND "] %s\n", _cdbg_snap_json.toRawUTF8()); \
      else \
          fprintf(stderr, "[SNAPSHOT " #KIND " TRUNCATED size=%zu] %.8192s\n", \
                  _cdbg_snap_bytes, _cdbg_snap_json.toRawUTF8()); \
      fflush(stderr); \
      CurlopEventLog::emitSnapshot(#KIND, _cdbg_snap_json.toRawUTF8(), \
                                   _cdbg_snap_bytes); \
  } while(0)

  #define CDBG_SNAPSHOT_S(KIND, std_string_expr) do { \
      if (! CurlopDebug::on(CurlopDebug::SNAPSHOTS)) break; \
      const auto& _cdbg_snap_str = (std_string_expr); \
      if (_cdbg_snap_str.size() < 8192) \
          fprintf(stderr, "[SNAPSHOT " #KIND "] %s\n", _cdbg_snap_str.c_str()); \
      else \
          fprintf(stderr, "[SNAPSHOT " #KIND " TRUNCATED size=%zu] %.8192s\n", \
                  _cdbg_snap_str.size(), _cdbg_snap_str.c_str()); \
      fflush(stderr); \
      CurlopEventLog::emitSnapshot(#KIND, _cdbg_snap_str.c_str(), _cdbg_snap_str.size()); \
  } while(0)

  #define CDBG_DIAGNOSTIC_FPRINTF(...) do { \
      fprintf(stderr, __VA_ARGS__); \
      fflush(stderr); \
  } while(0)
#else
  #define CDBG_SNAPSHOT_J(KIND, juce_string_expr) do {} while(0)
  #define CDBG_SNAPSHOT_S(KIND, std_string_expr) do {} while(0)
  #define CDBG_DIAGNOSTIC_FPRINTF(...) do {} while(0)
#endif
