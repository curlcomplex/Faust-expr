#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// CurlopEventLog — In-app structured event log (s380)
//
// Mirrors what scripts/q/sidecar.py used to derive by tailing stderr, except
// the app emits the JSONL lines DIRECTLY. No wrapper script required — launch
// CURLOP from Finder, Dock, plugin host, anywhere; the JSONL still flows.
//
// Gating: every emit is fronted by the existing CurlopDebug::on(flag) check
// inside the CDBG macros. Default flag mask = 0 → file stays empty until a
// flag is toggled via MCP curlop_set_debug. No always-on overhead.
//
// Output: append-only JSONL at /tmp/curlop-events.latest.jsonl (override via
// CURLOP_EVENT_LOG_PATH env). One line per emit, e.g.
//   {"ts":1778060000.123,"source":"cpp","flag":"NODE_CANVAS","msg":"..."}
//   {"ts":1778060001.456,"source":"snapshot","kind":"GRAPH_STATE","payload":...}
//
// Reader path: scripts/q/q.py — unchanged. It already reads this file.
// ═══════════════════════════════════════════════════════════════════════════

#include <string>

namespace CurlopEventLog {

// Open the event log file. Idempotent — safe to call multiple times.
// If path is null, uses CURLOP_EVENT_LOG_PATH env or falls back to
// /tmp/curlop-events.latest.jsonl.
void init(const char* path = nullptr);

// Close the file (flushes implicitly). Safe to call without prior init.
void shutdown();

// Append one event line. msg is treated as a UTF-8 string and JSON-escaped.
// flag is the literal name (e.g. "NODE_CANVAS") matching CurlopDebug::Flag.
// No-op if init() never succeeded.
void emit(const char* flag, const char* msg);

// Audio-thread handoff for enabled diagnostics. Bounded, non-blocking, and
// allocation-free; call drainRealtime() from the message thread to write the
// queued events through emit().
bool emitRealtime(const char* flag, const char* msg) noexcept;
void drainRealtime();

// Append one snapshot line. payload is RAW JSON (already valid JSON value);
// it's not re-escaped. Matches the structure scripts/q/sidecar.py emitted
// for [SNAPSHOT KIND] stderr lines.
void emitSnapshot(const char* kind, const char* payload, std::size_t len);

} // namespace CurlopEventLog
