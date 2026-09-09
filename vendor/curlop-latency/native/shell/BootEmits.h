#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// BootEmits — Path-A liveness primitives (s427 / SPEC-011 PATH-A-LIVENESS)
//
// Always-on snapshot events emitted to /tmp/curlop-events.latest.jsonl:
//
//   BUILD_INFO          — commit / commit_time / dirty / process_start_time.
//                         Emitted once per process at CurlopProcessor ctor.
//                         Lets `q diagnose` detect a stale running binary
//                         (HEAD ≠ process commit ⇒ rebuild was never picked
//                         up by the running app).
//
//   DEBUG_FLAG_MANIFEST — per-flag CDBG site count baked at build time.
//                         Emitted once per process at ctor. A flag with
//                         live_emit_sites=0 is unambiguously a stale binary
//                         for that flag (or the flag has no emit sites in
//                         the codebase — a SPEC-010 coverage gap).
//
//   DEBUG_FLAGS_STATE   — current uint64 mask + enabled-name list. Emitted
//                         at ctor (initial mask=0) and on every DCMD 0x30
//                         change (NativeBridgeServer). Lets `q diagnose`
//                         answer "is this flag currently enabled?" without
//                         a readback MCP tool (0.E-3 architecture).
//
//   HEARTBEAT           — 1Hz liveness ping: transport_playing + master
//                         peak-since-last-heartbeat + recorder_running +
//                         active_clip_id. Ungated, always on. Lets `q wait
//                         --kind HEARTBEAT` confirm audio is flowing before
//                         a flag-firing smoke even starts.
//
// All emits go through CurlopEventLog::emitSnapshot — message-thread safe.
// The HEARTBEAT peak source is an atomic written by the audio thread (see
// CurlopProcessor::heartbeatMasterPeak_) and exchanged on each emit.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>

namespace CurlopDebug { struct Mask; }
class CurlopProcessor;

namespace BootEmits {

// One-shot emits — call once at process start.
void emitBuildInfo();
void emitDebugFlagManifest();

// Current debug flag state — call at ctor + on every DCMD 0x30 change.
void emitDebugFlagsState(CurlopDebug::Mask mask);

// s429 / SPEC-012 — current GUI panel-visibility snapshot. Emitted at
// ctor (initially closed) and on every authoritative panel toggle. Lets
// `q state --kind PANEL_STATE --latest` answer "is the browser open?"
// without a readback MCP tool. HEARTBEAT also carries this state at 1Hz
// for periodic-poll style pre-flight; PANEL_STATE-on-toggle is the
// instantaneous-readback channel.
void emitPanelState(CurlopProcessor& proc);

// 1Hz liveness ping. Reads master peak via processor reference,
// resetting the atomic accumulator. Safe on the message thread only.
// Payload includes panel-visibility booleans (s429/B-1414) so a single fetch
// gives both audio-liveness and GUI-state in one snapshot.
void emitHeartbeat(CurlopProcessor& proc);

} // namespace BootEmits
