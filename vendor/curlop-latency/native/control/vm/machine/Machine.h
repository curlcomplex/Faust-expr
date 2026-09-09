// F-066 — the Machine: the rebuilt VM's execution kernel (adr-vm "How it
// runs"). Block-rate, opcode-blind: the loop walks decoded instructions and
// calls each one's table entry. The kernel knows no opcodes — new capability
// is a new table entry, never a kernel edit.
//
// Ownership (adr-vm "The rules"): the Machine owns the clock, the per-op
// state arena, and pending gate-offs. Nothing keeps a second authoritative
// copy. Emissions carry voice identity on every value.
//
// Zero-JUCE, std-only: program in, exact emissions out, no app, no device.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Signal.h"
#include "StepClockPlan.h"
#include "OpIds.h"
#include "OpTable.h"
#include "Program.h"

namespace curlop::vm {

// B-1731 — off-thread-prepared identity of one persistent control-source
// instance. Zero is the direct/legacy namespace.
using SourceIdentity = uint64_t;

// One value leaving the machine. frameOffset is sample-accurate within the
// current process() block; voice identity always travels with the value.
struct Emission {
    SignalType type        = SignalType::Value;
    PacketKind kind        = PacketKind::Set;
    float      value       = 0.0f;
    float      frameOffset = 0.0f;
    // B-290 — per-note-INSTANCE id (SPEC-014 §6: an unbounded logical tag;
    // the receiver owns its polyphony). Every hit gets a fresh id, so
    // Delivery's allocator rotates physical slots and release tails ring.
    uint32_t   voice       = 0;
    // Target lane for Value packets (which param a lock addresses).
    // Pitch/gate/velocity address by voice; lane stays 0.
    uint32_t   lane        = 0;
    // B-290 — chord-POSITION metadata (0 = root). Identity for nothing;
    // used by per-voice diagnostics, gate-less ordinal mapping, and the
    // MIDI2 adapter's per-voice lanes (SF-052 per-voice params later).
    uint32_t   voiceIdx    = 0;
    // B-1731 — stable identity of the source that owns `voice`. Machine voice
    // counters are intentionally source-local, so Delivery keys note
    // lifecycle by {sourceIdentity, voice}. Zero preserves the historical
    // single-source/direct-delivery contract.
    SourceIdentity sourceIdentity = 0;
    // B-1747: stable order within one prepared source's current block.
    // Cross-source merge compares {sourceIdentity, sourceOrder} after the
    // frame/tie rank, so routing and route-table order cannot change
    // same-frame composition relative to direct delivery.
    uint32_t sourceOrder = 0;
    // B-1738 — ordered signal channel identity. This is orthogonal to lane
    // (socket/parameter namespace) and voice (logical note lifecycle).
    uint32_t channelIndex = 0;
    // Descriptor-backed channels carry a stable identity across reorder and
    // split. Zero is the explicit legacy/unaddressed identity.
    uint64_t stableChannelId = 0;
    float sourceTempo = 0.0f;
    // Typed parameter lock metadata. Conversion is deliberately deferred to
    // Delivery so tempo-relative values follow live BPM per block.
    uint8_t typedParam = 0;
    uint8_t authoredBasis = 0;
    float authoredValue = 0.0f;
    float authoredStepBeats = 1.0f;
};

// Same-frame ordering rank: withdrawals precede asserts. Gate-offs (0),
// then Value Releases (1), then pitch/vel/Value Sets/Offsets (2), then
// gate-ons (3). Shared by the machine's per-block sort and the host's
// cross-machine merge — both MUST agree or a loop-wrap lock's Release
// (old step) lands after the next step's Set and withdraws it.
inline int emissionTieRank(const Emission& e)
{
    if (e.type == SignalType::Gate && e.kind == PacketKind::Set)
        return isGateOpen(e.value) ? 3 : 0;
    if (e.type == SignalType::Value && e.kind == PacketKind::Release)
        return 1;
    return 2;
}

// A decoded instruction: opcode + spec pointer + absolute operand offsets
// (precomputed at install from the schema — eval never re-parses bytes)
// + this instance's slot in the state arena.
struct Instr {
    const OpSpec* spec = nullptr;
    uint32_t pc = 0;
    uint32_t operandOff[kMaxOperands] = {};
    uint32_t stateOff = UINT32_MAX;
};

// One nesting level of scope state during a walk. The stack itself is
// machine-owned and preallocated at install (depth is a static program
// property) — never grown on the audio thread, never capped.
struct ScopeFrame {
    int32_t  metaIdx      = -1;     // index into the install-time scope table
    double   absBaseBeats = 0.0;    // cumulative base (parent + own offset)
    double   durBeats     = 0.0;
    float    transposeSemis = 0.0f; // sums across the stack at pitch resolve
    // B-301 — timescale composes through the frame stack like every op
    // (SPEC-016 §1): `timescale` holds the CUMULATIVE product (inherited at
    // SCOPE_START x this frame's own factor); `tsInherited` keeps the
    // parent's product so a repeated timescale op on one frame re-composes
    // rightmost-wins (B-302 ruling) instead of compounding.
    double   timescale    = 1.0;
    double   tsInherited  = 1.0;
    // F-071 T-571a — nested `timescale:~lfo`. When tsModActive, evalStepCommon
    // computes a PER-STEP timescale factor from this LFO (sampled in scope-local
    // beat space) instead of the static `timescale` product; composes on top of
    // tsInherited. Re-populated each walk by evalTimescaleMod — no cross-walk
    // memory, so the transient frame is the correct home (the factor is a pure
    // function of the step's authored position → block-size-independent).
    uint8_t  tsModActive   = 0;
    uint8_t  tsModWaveform = 0;     // 0 sine 1 tri 2 saw 3 square
    double   tsModRateBeats = 1.0;  // beats per LFO cycle (scope-local clock)
    double   tsModMinMul   = 1.0;   // multipliers (1.0 = unchanged)
    double   tsModMaxMul   = 1.0;
    // Dynamic timing expressions are fire-time bytecode operands held on the
    // frame op instruction, then sampled by evalStepCommon for each covered step.
    uint8_t  tsExprActive = 0;
    uint8_t  tsExprMode = 0; // 0 authored timescale, 1 authored BPM
    int32_t  tsExprInstrIdx = -1;
    uint8_t  transposeExprActive = 0;
    int32_t  transposeExprInstrIdx = -1;
    float    transposeExprFactor = 1.0f;
    float    transposeExprSemis = 0.0f;
    uint8_t  grooveActive = 0;
    uint8_t  grooveTemplate = 0;
    float    grooveAmount = 0.0f, grooveVelLo = 0.0f, grooveVelHi = 1.0f;
    uint8_t  grooveExprActive = 0;
    int32_t  grooveExprInstrIdx = -1;
    // B-291 — groove indexes the NOTE STREAM: every STEP that evals while
    // this frame is on the stack counts (arp-expanded sub-steps included),
    // not just the frame's direct children.
    uint16_t grooveCounter = 0;
    // B-291 — runtime grid quantizer at this frame.
    uint8_t  gridActive = 0;
    float    gridSize = 0.25f, gridStrength = 1.0f;
    uint8_t  gridExprActive = 0;
    int32_t  gridExprInstrIdx = -1;
    // Written order of timing ops within this frame (§5 one-law: rightmost
    // covers leftward output — apply in written order).
    uint8_t  grooveOrd = 0, gridOrd = 0, timingOpSerial = 0;
    uint8_t  permKind     = 0;      // 0 none, 1 reverse, 2 rotate, 3 shuffle, 4 sort
    int16_t  rotateAmount = 0;
    uint8_t  rotateExprActive = 0;
    int32_t  rotateExprInstrIdx = -1;
    // Rebuild data so order ops CASCADE into nested frames (s498 — the
    // one-law: an op covers everything inside its frame, so [bars] sort
    // orders the arp hits inside each bar too; an explicit inner order op
    // overrides the inherited one).
    uint8_t  sortDesc     = 0;
    uint32_t shuffleSeed  = 0;
    uint8_t  invertActive = 0;
    uint8_t  invertMode   = 0;   // 0 semitone pivot, 1 first note, 2 content index (B-288)
    float    invertPivot  = 0.0f;
    uint8_t  invertExprActive = 0;
    int32_t  invertExprInstrIdx = -1;
    float    invertExprPivot = 0.0f;
    uint16_t stepCounter  = 0;      // program-order index within the scope
    // B-289 — runtime SCALE quantizer at this frame. Root + intervals read
    // from the SCALE instruction's operands (program bytes, stable for the
    // install's lifetime). scaleBaseTranspose = the frame's transposeSemis
    // when SCALE evaluated, i.e. the transposes written LEFT of scale —
    // covered by the quantize; transposes written right escape (§5 one-law).
    uint8_t  scaleActive = 0;
    int32_t  scaleInstrIdx = -1;
    float    scaleBaseTranspose = 0.0f;
    uint8_t  scaleRootExprActive = 0;
    int32_t  scaleRootExprInstrIdx = -1;
    float    scaleRootExprValue = 0.0f;
    // F-071 T-539 — stochastic scale (SCALE_GEN): evalScaleGen draws ONE
    // candidate scale per scope iteration and stashes its root + intervals
    // here. scaleGenCount > 0 → quantizeAtFrame uses these instead of the
    // SCALE op's baked operands. Intervals cap at the 12-semitone period.
    uint8_t  scaleGenCount = 0;
    float    scaleGenRoot  = 0.0f;
    float    scaleGenIntervals[12] = {};
    uint32_t dynamicRepeatCount = 1;
    uint8_t  dynamicFitActive = 0;
    uint32_t dynamicFitCount = 0;
};

// Install-time facts about one scope: a CONTENT table — one entry per
// direct content item (steps AND child scopes) in program order, with its
// authored scope-relative offset and the first note inside it (B-288/B-292:
// order ops permute content positions, sort orders content by pitch,
// invert:N pivots on the Nth item's pitch). firstNoteSemis is the first
// note anywhere inside the scope, nested frames included.
struct ScopeMeta {
    std::vector<float>   offsets;
    std::vector<float>   noteSemis;   // first note per content entry
    std::vector<uint8_t> hasNote;
    float firstNoteSemis = 0.0f;
    bool  hasFirstNote   = false;
};

// One control op's per-block output stream: dense samples, machine-owned,
// allocated by prepare()/install() off the audio thread. Delivery (Phase 3)
// binds lanes onto module declarations.
struct StreamRow {
    uint32_t   instrIdx = 0;
    uint32_t   lane     = 0;
    uint32_t   channelIndex = 0;
    uint64_t   stableChannelId = 0;
    SignalType type     = SignalType::Value;
    PacketKind kind     = PacketKind::Stream;
    bool       midi     = false;
    bool       active   = false;
    std::vector<float> data;
};

class Machine;

// The evaluate-side view of the kernel: operand access, segment/step
// context, emission + scheduling services. This is the entire surface an
// op implementation sees.
struct ExecCtx {
    enum class Phase : uint8_t {
        Configure,   // install-time walk — ops record static facts (loop length)
        Fire,        // per-segment runtime walk: steps fire, gates land
        Render,      // second per-segment walk: stream ops render — they see
                     // ALL of the segment's gate-ons, so trigger-coupled
                     // streams stay block-size independent
    };

    Machine&     machine;
    const Instr* instr = nullptr;
    Phase        phase = Phase::Fire;

    // Current loop segment (Fire phase): [segStart, segEnd) in absolute
    // FRAMES, all inside loop iteration `iteration`. The machine's
    // authoritative clock is the integer frame counter; beat values convert
    // to frames with a single rounding — no accumulated drift, so loop
    // boundaries are exact when the maths is exact.
    double   segStart  = 0.0;
    double   segEnd    = 0.0;
    uint64_t iteration = 0;

    // Current step (set by the step-defining op, read by step-body ops).
    bool   stepFiring   = false;
    double stepAbsFrame = 0.0;
    double stepDurBeats = 0.0;
    uint32_t stepInstrIdx = UINT32_MAX;
    uint32_t stepRepeatCount = 1;
    double stepRepeatBaseFrame = 0.0;
    double stepRepeatStrideFrames = 0.0;

    // Per-iteration step facts, computed on EVERY walk (not only the firing
    // one). Because conditional/probability verdicts are keyed by iteration
    // (never block position), every walk of the same iteration computes the
    // same values — anticipatory ops (flam's grace) rely on this to place
    // hits before the beat, block-size independently.
    double stepWillFireFrame = 0.0;   // this iteration's fire frame
    bool   stepSuppressed    = true;  // cond/prob/bernoulli verdict

    // The note instruction of the current step body (set by NOTE/NOTE_CHORD
    // on every walk; UINT32_MAX = none/REST). Articulations that follow the
    // note (flam) read pitch/velocity through it.
    uint32_t noteInstrIdx = UINT32_MAX;
    uint8_t  noteIsChord  = 0;
    uint8_t  noteIsExpr   = 0;
    float    noteExprSemis = 0.0f;

    // Groove velocity shaping for the current step (set by the step op).
    float stepVelScale = 1.0f;

    // Scope-stack depth for this walk (frame 0 = the implicit root).
    int scopeDepth = 0;

    // ── Step-body articulation context (reset by the step op) ───────
    // Program order within a step: STEP, articulation ops, NOTE/REST,
    // param locks. Articulation evals stash parameters here; the note op
    // consumes them at emission time.
    //
    // Train-shaped articulations (ratchet/buzz/bounce/geiger) arm at most
    // one train per step: the articulation eval points trainState at its
    // own state-arena slot; the note op fills it and streams the first
    // segment. Subsequent walks of the articulation instruction stream the
    // rest. (Divergence from the old VM noted in the table header: the old
    // pipeline composed sequential ornament transforms; here one train +
    // flam compose, additional trains replace.)
    uint8_t  trainKind     = 0;          // 0=none 1=ratchet 2=buzz 3=bounce 4=geiger
    void*    trainState    = nullptr;
    uint32_t trainInstrIdx = 0;
    uint64_t ratchetCount  = 0;
    uint8_t  ratchetUsePitches = 0;
    float    buzzPressure  = 0.0f, buzzDurBeats = 0.0f;
    float    bounceGravity = 0.0f;
    // F-071 T-516 — authorable bounce interval + unit domain. domain 0 = none
    // (derive the legacy 0.4×stepDur, BPM-relative); 1 = BPM-relative (interval
    // is beats, scales w/ timescale + tempo); 2 = absolute (interval is seconds,
    // physics-time — immune to timescale + tempo, the real-ball-bounce bypass).
    float    bounceInterval = 0.0f;
    uint8_t  bounceIntervalDomain = 0;
    float    geigerDensity = 0.0f;
    uint32_t geigerSeed    = 0;
    float    flamOffsetBeats = -1.0f, flamGraceVel = 0.0f;
    float    gateLenBeats    = -1.0f;   // < 0 = no override
    float    deviateAmount   = 0.0f;    // B-286 plain deviate: onset jitter, fraction of step
    uint32_t deviateSeed     = 0;

    // B-187 — GEN_OPERAND prefix overrides: arm-time generator draws that
    // replace the following articulation's immediate operand. Mask bit
    // (1 << GenTarget); cleared at every STEP header.
    // uint16_t masks — one bit per GenTarget (kGenTargetCount). F-071 T-531
    // pushed the target count past 8 (NoteVel/GateLen), so the mask widened
    // from uint8_t; OpIds.h static_asserts the count fits 16 bits.
    uint16_t genOperandMask = 0;
    float    genOperandVal[kGenTargetCount] = {};
    // F-071 Phase 1 (T-511) — a ramp gen (~auto) in an op-argument slot carries
    // BOTH endpoints; the train cursor re-evaluates the interpolated value PER
    // HIT across the step span (the squishy VM — SPEC-018 §1.1). A single-draw
    // gen (~random/~bernoulli) leaves the ramp mask clear and uses genOperandVal.
    float    genOperandValB[kGenTargetCount] = {};  // ramp end-point (start is genOperandVal)
    uint16_t genOperandRampMask = 0;  // targets whose operand is a ramp, not a draw

    // Conditional guard: a conditional that fails sets this; the next step
    // consumes it and does not fire. Multiple conditionals AND together
    // (any failure suppresses the step). Walk-local by construction.
    bool condSkip = false;

    // Bernoulli option selection: the next `bernoulliRemaining` steps are
    // alternatives; only the `bernoulliPick`-th of them (0-indexed among
    // the option group of `bernoulliTotal`) fires.
    int bernoulliRemaining = 0;
    int bernoulliTotal     = 0;
    int bernoulliPick      = 0;

    // Dynamic select branch selection: the next grouped branch steps are
    // alternatives; all steps in the selected branch fire, all others suppress.
    int selectRemainingSteps      = 0;
    int selectCurrentBranch       = 0;
    int selectCurrentBranchRemain = 0;
    int selectPick                = 0;
    int selectBranchCount         = 0;
    uint32_t selectInstrIdx       = UINT32_MAX;

    // Index of the current instruction in the decoded program — part of the
    // probability hash key (block-size-independent determinism).
    uint32_t instrIndex = 0;

    bool halted = false;

    // ── Operand access (offsets precomputed from the schema) ────────
    float f32(int i) const
    {
        float v;
        std::memcpy(&v, codeAt(instr->operandOff[i]), 4);
        return v;
    }
    uint8_t u8(int i) const   { return *codeAt(instr->operandOff[i]); }
    uint16_t u16(int i) const
    {
        uint16_t v;
        std::memcpy(&v, codeAt(instr->operandOff[i]), 2);
        return v;
    }
    int16_t i16(int i) const
    {
        int16_t v;
        std::memcpy(&v, codeAt(instr->operandOff[i]), 2);
        return v;
    }
    uint32_t u32(int i) const
    {
        uint32_t v;
        std::memcpy(&v, codeAt(instr->operandOff[i]), 4);
        return v;
    }

    // Array operands (count-prefixed; offset points at the count).
    uint8_t arrayLenU8(int i) const { return *codeAt(instr->operandOff[i]); }
    uint16_t arrayU16At(int i, int k) const
    {
        uint16_t v;
        std::memcpy(&v, codeAt(instr->operandOff[i]) + 1 + 2 * k, 2);
        return v;
    }
    uint16_t arrayLenU16(int i) const
    {
        uint16_t v;
        std::memcpy(&v, codeAt(instr->operandOff[i]), 2);
        return v;
    }
    float arrayF32At(int i, int k) const
    {
        float v;
        std::memcpy(&v, codeAt(instr->operandOff[i]) + 2 + 4 * k, 4);
        return v;
    }

    // Deterministic roll in [0,1) keyed by (seed operand, loop iteration) —
    // nothing else. Block position never enters the key, and neither does
    // the instruction index: the same source step projected into several
    // per-module programs must reach the same verdict (one prob() guarding
    // a kick+snare step rolls ONCE, musically). The flip side is the
    // emitter's contract: independence between two random ops rests
    // ENTIRELY on stamping each source instruction a unique seed — the
    // compiler does this mechanically; hand-authored programs (tests!)
    // must use distinct seeds or their rolls correlate. (Neo, s489.)
    float roll(uint32_t seed) const
    {
        uint64_t x = (uint64_t) seed
                   ^ (iteration * 0x9E3779B97F4A7C15ull);
        // splitmix64 finalizer
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        x =  x ^ (x >> 31);
        return (float) ((x >> 11) * (1.0 / 9007199254740992.0)); // 2^53
    }

    // ── Kernel services (defined after Machine) ─────────────────────
    // emit() is immediate (absFrame must be inside the current block —
    // clamped to block start if an anticipatory hit lands in the past).
    // schedule() queues an emission for a future block; the machine drains
    // due entries at the end of every process().
    void emit(SignalType type, PacketKind kind, float value,
              double absFrame, uint32_t voice, uint32_t lane = 0,
              uint32_t voiceIdx = 0, uint32_t channelIndex = 0,
              uint64_t stableChannelId = 0);
    void schedule(SignalType type, PacketKind kind, float value,
                  double absFrame, uint32_t voice, uint32_t lane = 0,
                  uint32_t voiceIdx = 0, uint32_t channelIndex = 0,
                  uint64_t stableChannelId = 0);
    void emitTypedParam(uint16_t lane, float authoredValue, uint8_t basis,
                        float stepBeats, bool isOffset, double absFrame);
    void scheduleGateOff(double absFrame, uint32_t voice, uint32_t voiceIdx = 0,
                         uint32_t lane = 0, uint32_t channelIndex = 0,
                         uint64_t stableChannelId = 0);
    void scheduleHit(float pitchSignal, float vel, double absFrame,
                     double gateFrames, uint32_t voice, uint32_t voiceIdx = 0);
    double framesPerBeat() const;
    double loopFrames() const;
    double blockEndFrame() const;
    void* state() const;

    // Cross-instruction operand access — train advance helpers read chord
    // voicings / pitch arrays from the note instruction they were armed by.
    float f32Of(uint32_t instrIdx, int operandIdx) const;
    uint8_t arrayLenU8Of(uint32_t instrIdx, int operandIdx) const;
    uint16_t arrayU16AtOf(uint32_t instrIdx, int operandIdx, int k) const;
    uint16_t arrayLenU16Of(uint32_t instrIdx, int operandIdx) const;
    float arrayF32AtOf(uint32_t instrIdx, int operandIdx, int k) const;

    // ── Scope services ───────────────────────────────────────────────
    ScopeFrame& scope();                       // innermost frame
    ScopeFrame& scopeAt(int depth);
    void pushScope();                          // copies nothing; fresh frame
    void popScope();
    const ScopeMeta* scopeMetaOf(const ScopeFrame& f) const;
    uint32_t* permRow();                       // scratch row for this depth

    // Pitch resolution through the scope stack: transpose sums across all
    // frames; the innermost active invert reflects first (invert defines
    // the melodic shape, transpose then moves it). A SCALE frame (B-289)
    // quantizes everything inside it plus its own left-written transposes;
    // transposes outside/right of it escape the key (§5 one-law).
    float resolvePitch(float semis) const;
    float quantizeAtFrame(const ScopeFrame& f, float semis) const;
    void* stateOfInstr(uint32_t instrIdx) const;

    // ── Stream services (Render phase) ───────────────────────────────
    // The row for THIS instruction: marks it active, stamps lane/type/kind,
    // returns block-relative sample storage (index = frame - blockStart).
    float* streamFor(uint32_t lane, SignalType type, PacketKind kind,
                     bool midi = false, uint32_t channelIndex = 0,
                     uint64_t stableChannelId = 0);
    void streamStamp(uint32_t lane, SignalType type, PacketKind kind);
    const StreamRow* previousStreamRow(uint32_t lane, SignalType type) const;
    StreamRow* takePreviousStreamRow(uint32_t lane, SignalType type);
    const float* externalInputRow(uint32_t channel, int requiredSamples) const;
    double sampleRate() const;
    double blockStartFrame() const;

    // Per-LANE slew state (~random de-click). The one-pole's running value
    // lives on the lane, shared across every instruction that targets the
    // same param — so per-step routes (separate instructions, one lane)
    // blend continuously instead of each snapping to its own draw. NaN =
    // never written → first write snaps. Returns nullptr for an out-of-range
    // lane (fall back to snappy).
    float* laneSlewSlot(uint32_t lane);
    void activateChannelPlan(uint32_t planIndex);

    // Scope clock (SF-061): the innermost scope's span for this iteration.
    double scopeAnchorFrame() const;
    double scopeSpanEndFrame() const;

    // B-324 — absolute frame (monotonic axis) for a beat, via the clock anchor
    // (frameBase_, beatBase_). Ops that derive a fire frame from a beat
    // position MUST route through this, not iteration·loopFrames-from-frame-0,
    // or a live tempo change shifts them against already-walked frames.
    double frameOfBeat(double beat) const;

    // B-324 — inverse of frameOfBeat: the beat position at an absolute frame,
    // through the same anchor (block-size independent). Used by scope-time
    // generators (T-495) to advance BPM-relative phase in beats.
    double beatOfFrame(double frame) const;

    // F-071 T-495 — the current scope's cumulative timescale (B-301 machine
    // factor; 1.0 = identity). Valid in Render: scopeStack_[scopeDepth] is the
    // op's own frame during its Render walk. BPM-relative generator rates scale
    // by this (a compressed scope speeds them up like tape); absolute (ms/s/hz)
    // args ignore it (SPEC-018 §1.2). For a modulated timescale:~lfo scope this
    // is the baseline tsInherited — the per-step sweep is not seen here (same
    // follow-up family as modulated bpm:~lfo).
    double scopeTimescale() const;

    // Gate/pitch history at sample resolution (block list + carry-in).
    uint64_t gateCountAt(double frame) const;
    double gateAnchorAt(double frame) const;       // latest gate-on <= frame, -1 none
    double findGateOffAfter(double onFrame) const;     // earliest scheduled off >= onFrame, +inf none
    struct PitchAt { float cur; float prev; double curStartFrame; bool any; };
    PitchAt pitchAt(double frame) const;

    // B-1739 — prepared product-Step clock. The host adapter supplies the
    // sampled clock edge; the existing decoded walk remains the sole
    // instruction and packet authority.
    bool externalStepClockActive() const;
    bool externalStepClockMatches(uint32_t instrIdx) const;
    bool externalStepClockReaim() const;
    bool externalStepClockSuppressesGateOff() const;
    double externalStepClockFrame() const;
    float externalStepClockOutputValue(uint32_t lane, SignalType type,
                                       float compiledValue) const;

private:
    const uint8_t* codeAt(uint32_t off) const;
    friend class Machine;
};

class Machine {
public:
    explicit Machine(double sampleRate) : sampleRate_(sampleRate) {}

    void setTempo(double bpm) { bpm_ = bpm; }
    double tempoBpm() const   { return bpm_; }

    // Host-fed MIDI CC matrix for CTRL_MIDI streams (B-275: a ~midi route is
    // an op like any other — it renders inside the machine so the scope rule
    // clips it; only the CC VALUES arrive from outside). The host owns the
    // storage (row-major [channels][ccs], atomic floats, NaN = never
    // received); the machine keeps a borrowed pointer. Null = no CC source:
    // CTRL_MIDI renders nothing and its route stays released.
    void setMidiCCSource(const std::atomic<float>* rowMajor, int channels, int ccs)
    {
        midiCC_ = rowMajor;
        midiCCChannels_ = channels;
        midiCCCount_ = ccs;
    }
    float readMidiCC(int channel, int cc) const
    {
        if (midiCC_ == nullptr || channel < 0 || channel >= midiCCChannels_
            || cc < 0 || cc >= midiCCCount_)
            return std::numeric_limits<float>::quiet_NaN();
        return midiCC_[(size_t) channel * (size_t) midiCCCount_ + (size_t) cc]
                   .load(std::memory_order_relaxed);
    }

    void setExternalInputRows(const float* const* rows, int count, int stride)
    {
        externalInputRows_ = rows;
        externalInputCount_ = count;
        externalInputStride_ = stride;
    }

    // Maximum frames a single process() call may pass. Stream rows are
    // sized here, off the audio thread. Re-prepare with a larger size any
    // time; process() with more frames than prepared is a no-op error.
    void prepare(int maxBlockFrames, size_t eventReserve = 0)
    {
        preparedFrames_ = std::max(preparedFrames_, maxBlockFrames);
        for (auto& row : streamRows_)
            row.data.assign((size_t) preparedFrames_, 0.0f);
        if (eventReserve > 0) {
            rtBounded_ = true;
            pending_.reserve(eventReserve);
            blockGateOns_.reserve(eventReserve);
            blockGateOffs_.reserve(eventReserve);
            blockPitchSets_.reserve(eventReserve);
            sortKeys_.reserve(eventReserve);
            sortTmp_.reserve(eventReserve);
            channelMapTmp_.reserve(eventReserve);
        }
    }

    const std::vector<StreamRow>& streams() const { return streamRows_; }

    // B-1738 — bind a program-local packet input signal to the prepared
    // runtime lane assigned to its graph edge. Channel plans remain stable
    // across graph rebuilds; only this off-thread binding follows route
    // allocation. Returning false means the program never declared the
    // logical signal as a packet input.
    bool bindPacketInputSignal(uint32_t logicalSignal, uint32_t runtimeLane)
    {
        const bool declared = std::any_of(
            program_.packetInputs.begin(), program_.packetInputs.end(),
            [logicalSignal] (const auto& input) {
                return input.first == logicalSignal;
            });
        if (! installed_ || ! declared
            || runtimeLane >= kNamedOutputLaneFlag)
            return false;
        for (const auto& binding : packetInputSignalBindings_)
            if (binding.first == runtimeLane)
                return binding.second == logicalSignal;
        packetInputSignalBindings_.push_back({
            runtimeLane, logicalSignal
        });
        return true;
    }

    // Decode + validate against the op table, lay out the state arena, run
    // the Configure walk. Returns false on unknown opcode or truncated
    // operands — a failed install leaves no runnable program.
    bool install(const Program& program)
    {
        program_ = program;
        instrs_.clear();
        stateArena_.clear();
        pending_.clear();
        stepClockOutputs_.clear();
        heldStepGates_.clear();
        packetInputSignalBindings_.clear();
        activeStepClockEvent_ = nullptr;
        sampledStepClockRelease_ = false;
        voiceSeq_ = 0;
        lastVoiceInstance_ = 0;
        framePos_ = 0;
        loopLen_ = 0.0;
        installed_ = false;

        const uint8_t* code = program_.code.data();
        const size_t   size = program_.code.size();
        if (size > (size_t) std::numeric_limits<uint32_t>::max())
            return false;
        size_t pc = 0;
        uint32_t arena = 0;
        std::unordered_map<uint32_t, uint32_t> markovStateBySeed;

        while (pc < size) {
            const OpSpec* spec = findOp(code[pc]);
            if (spec == nullptr)
                return false;

            Instr in;
            in.spec = spec;
            in.pc   = (uint32_t) pc;

            // Operand offsets from the schema — the same walk byte-skip uses.
            size_t off = pc + 1;
            uint16_t lastArrayLen = 0;
            for (int i = 0; i < spec->operands.count; ++i) {
                in.operandOff[i] = (uint32_t) off;
                const Operand k = spec->operands.kinds[i];
                const size_t fixed = operandFixedBytes(k);
                if (off + fixed > size)
                    return false;
                if (k == Operand::F32ArrayU16) {
                    uint16_t n = 0;
                    std::memcpy(&n, code + off, 2);
                    lastArrayLen = n;
                    off += 2 + (size_t) n * 4;
                    if (off > size)
                        return false;
                } else if (k == Operand::U16ArrayU8) {
                    const uint8_t n = code[off];
                    off += 1 + (size_t) n * 2;
                    if (off > size)
                        return false;
                } else {
                    off += fixed;
                }
            }

            // B-289 — resolved-pitch capture buffers. Articulation trains
            // outlive the walk-local scope stack, and SCALE makes the pitch
            // transform non-affine, so trains arm with RESOLVED pitches:
            // NOTE/NOTE_CHORD get a float per voice; RATCHET_PITCHED gets a
            // float per array entry (after its TrainState). Sized from the
            // static program here, off the audio thread — never grown,
            // never capped.
            uint32_t stateNeed = spec->stateSize;
            uint32_t markovSeed = 0;
            if (code[pc] == opcode(Op::Note)
                || code[pc] == opcode(Op::NoteExpr))
                stateNeed += 4;
            else if (code[pc] == opcode(Op::NoteChord))
                stateNeed += (uint32_t) lastArrayLen * 4;
            else if (code[pc] == opcode(Op::RatchetPitched))
                stateNeed += (uint32_t) lastArrayLen * 4;
            else if (code[pc] == opcode(Op::MarkovEvent)) {
                std::memcpy(&markovSeed, code + in.operandOff[5], 4);
                const auto existing = markovStateBySeed.find(markovSeed);
                if (existing != markovStateBySeed.end()) {
                    in.stateOff = existing->second;
                    stateNeed = 0;
                }
            } else if (code[pc] == opcode(Op::MarkovGate)) {
                std::memcpy(&markovSeed, code + in.operandOff[2], 4);
                markovSeed ^= 0x647A7E00u;
                const auto existing = markovStateBySeed.find(markovSeed);
                if (existing != markovStateBySeed.end()) {
                    in.stateOff = existing->second;
                    stateNeed = 0;
                }
            } else if (code[pc] == opcode(Op::CtrlDyn)
                       && code[in.operandOff[1]] == (uint8_t) CtrlDynKind::MarkovValue) {
                std::memcpy(&markovSeed, code + in.operandOff[4], 4);
                markovSeed ^= 0xC7D34A11u;
                const auto existing = markovStateBySeed.find(markovSeed);
                if (existing != markovStateBySeed.end()) {
                    in.stateOff = existing->second;
                    stateNeed = 0;
                }
            }

            if (stateNeed > 0) {
                arena = (arena + 7u) & ~7u;   // 8-byte align each slot
                in.stateOff = arena;
                arena += stateNeed;
                if (code[pc] == opcode(Op::MarkovEvent)
                    || code[pc] == opcode(Op::MarkovGate)
                    || (code[pc] == opcode(Op::CtrlDyn)
                        && code[in.operandOff[1]] == (uint8_t) CtrlDynKind::MarkovValue))
                    markovStateBySeed.emplace(markovSeed, in.stateOff);
            }

            instrs_.push_back(in);
            pc = off;
        }

        // B-1738 — resolve every plan against the decoded program before the
        // machine becomes runnable. Side-table data is never trusted on the
        // callback and U32 local lanes must not collide with the named-output
        // namespace tag.
        const auto channelKey = [] (uint32_t signal, uint32_t channel) {
            return (static_cast<uint64_t>(signal) << 32u)
                | static_cast<uint64_t>(channel);
        };
        std::unordered_set<uint64_t> localChannels;
        std::unordered_set<uint64_t> denseChannels;
        std::unordered_set<uint64_t> packetInputs;
        for (const auto& input : program_.packetInputs)
            packetInputs.insert(channelKey(
                input.first, input.second));
        for (const auto& in : instrs_) {
            if (in.spec->opcode == opcode(Op::LocalOutput)
                || in.spec->opcode == opcode(Op::LocalGateOnly)) {
                uint32_t lane = 0;
                uint32_t channel = 0;
                std::memcpy(&lane, code + in.operandOff[0], 4);
                std::memcpy(&channel, code + in.operandOff[1], 4);
                if (lane >= kNamedOutputLaneFlag)
                    return false;
                localChannels.insert(channelKey(lane, channel));
            } else if (in.spec->opcode == opcode(Op::CtrlInput)) {
                uint32_t lane = 0;
                uint32_t channel = 0;
                std::memcpy(&lane, code + in.operandOff[0], 4);
                std::memcpy(&channel, code + in.operandOff[1], 4);
                denseChannels.insert(channelKey(lane, channel));
            } else if (in.spec->opcode == opcode(Op::ChannelApply)) {
                uint32_t planIndex = 0;
                std::memcpy(&planIndex, code + in.operandOff[0], 4);
                if (planIndex >= program_.channelPlans.size())
                    return false;
            }
        }
        std::unordered_set<uint32_t> outputSignals;
        for (const auto& plan : program_.channelPlans) {
            std::unordered_set<uint64_t> outputStableIds;
            if (plan.entries.empty()
                || plan.entries.size()
                    > (size_t) std::numeric_limits<uint32_t>::max()
                || plan.outputSignal >= kNamedOutputLaneFlag)
                return false;
            if (! outputSignals.insert(plan.outputSignal).second)
                return false;
            for (const auto& entry : plan.entries) {
                if (plan.descriptorBacked
                    && entry.outputStableChannelId == 0)
                    return false;
                if (plan.descriptorBacked
                    && ! outputStableIds.insert(
                            entry.outputStableChannelId).second)
                    return false;
                if (entry.sourceKind == ChannelSourceKind::LocalEmission) {
                    if (localChannels.find(channelKey(
                            entry.sourceSignal, entry.sourceChannel))
                        == localChannels.end())
                        return false;
                } else if (entry.sourceKind == ChannelSourceKind::DenseStream) {
                    if (denseChannels.find(channelKey(
                            entry.sourceSignal, entry.sourceChannel))
                        == denseChannels.end())
                        return false;
                } else if (entry.sourceKind
                           == ChannelSourceKind::PacketInput) {
                    if (packetInputs.find(channelKey(
                            entry.sourceSignal, entry.sourceChannel))
                        == packetInputs.end())
                        return false;
                } else {
                    return false;
                }
            }
        }
        activeChannelPlans_.assign(program_.channelPlans.size(), 0);

        stateArena_.assign(arena, 0);

        // Per-lane ~random slew state, sized from the max lane operand. Every
        // Ctrl* stream op carries its lane as operand 0 (U16); reading those
        // off-thread here gives the lane count without a runtime grow. NaN =
        // uninited (first write snaps). Over-sizing (a non-lane U16 operand)
        // is harmless — only ~random with slew>0 ever reads a slot.
        {
            uint16_t maxLane = 0;
            for (const auto& in : instrs_) {
                if (in.spec->operands.count > 0
                    && in.spec->operands.kinds[0] == Operand::U16) {
                    uint16_t ln = 0;
                    std::memcpy(&ln, code + in.operandOff[0], 2);
                    maxLane = std::max(maxLane, ln);
                }
            }
            laneSlewLast_.assign((size_t) maxLane + 1u,
                                 std::numeric_limits<float>::quiet_NaN());
        }

        // ── Scope scan: step-offset tables, first notes, max depth ──────
        // All static program facts, derived off the audio thread. The
        // walk-time scope stack and permutation scratch are preallocated
        // here — never grown later, never capped.
        instrMetaIdx_.assign(instrs_.size(), -1);
        instrStepOrdinal_.assign(instrs_.size(), -1);
        instrSourceOrdinal_.assign(instrs_.size(), -1);
        nextStepOrdinalAfterInstr_.assign(instrs_.size(), -1);
        stepOrdinalCount_ = 0;
        lastFiredStep_ = -1;
        currentSourceStep_ = -1;
        {
            int ord = 0;
            for (uint32_t i = 0; i < (uint32_t) instrs_.size(); ++i) {
                const uint8_t oc = instrs_[i].spec->opcode;
                if (oc == (uint8_t) Op::Step || oc == (uint8_t) Op::StepAbs)
                    instrStepOrdinal_[i] = ord++;
            }
            stepOrdinalCount_ = ord;
            int nextStepOrd = -1;
            for (int i = (int) instrs_.size() - 1; i >= 0; --i) {
                nextStepOrdinalAfterInstr_[(size_t) i] = nextStepOrd;
                if (instrStepOrdinal_[(size_t) i] >= 0)
                    nextStepOrd = instrStepOrdinal_[(size_t) i];
            }
        }
        scopeMetas_.clear();
        {
            std::vector<int32_t> open;     // metaIdx stack
            size_t maxDepth = 0, maxSteps = 0;
            auto pushContent = [&](int32_t metaIdx, float off) {
                auto& meta = scopeMetas_[(size_t) metaIdx];
                meta.offsets.push_back(off);
                meta.noteSemis.push_back(0.0f);
                meta.hasNote.push_back(0);
                maxSteps = std::max(maxSteps, meta.offsets.size());
            };
            for (uint32_t i = 0; i < (uint32_t) instrs_.size(); ++i) {
                const uint8_t oc = instrs_[i].spec->opcode;
                if (oc == (uint8_t) Op::ScopeStart) {
                    // A child scope is a CONTENT item of its parent (B-292):
                    // order ops permute bars exactly like steps.
                    if (! open.empty()) {
                        float off;
                        std::memcpy(&off, code + instrs_[i].operandOff[0], 4);
                        pushContent(open.back(), off);
                    }
                    instrMetaIdx_[i] = (int32_t) scopeMetas_.size();
                    open.push_back((int32_t) scopeMetas_.size());
                    scopeMetas_.push_back({});
                    maxDepth = std::max(maxDepth, open.size());
                } else if (oc == (uint8_t) Op::ScopeEnd) {
                    if (! open.empty()) open.pop_back();
                } else if (oc == (uint8_t) Op::Step || oc == (uint8_t) Op::StepAbs) {
                    if (! open.empty()) {
                        float off;
                        std::memcpy(&off, code + instrs_[i].operandOff[0], 4);
                        pushContent(open.back(), off);
                    }
                } else if (oc == (uint8_t) Op::Note
                           || oc == (uint8_t) Op::NoteExpr
                           || oc == (uint8_t) Op::NoteChord) {
                    float semis;
                    if (oc == (uint8_t) Op::NoteExpr) {
                        const uint32_t o = instrs_[i].operandOff[2];
                        std::memcpy(&semis, code + o, 4);
                    } else if (oc == (uint8_t) Op::NoteChord) {
                        const uint32_t o = instrs_[i].operandOff[0];
                        std::memcpy(&semis, code + o + 2, 4);  // first voicing
                    } else {
                        const uint32_t o = instrs_[i].operandOff[0];
                        std::memcpy(&semis, code + o, 4);
                    }
                    // The note belongs to the latest content entry of EVERY
                    // open scope (nested frames included) — first one wins.
                    // (The old scan filled only the innermost scope, so
                    // invert:1 over varref bars reflected around a default
                    // pivot — B-292's seventh instance.)
                    for (int32_t mi : open) {
                        auto& meta = scopeMetas_[(size_t) mi];
                        if (! meta.hasFirstNote) {
                            meta.firstNoteSemis = semis;
                            meta.hasFirstNote   = true;
                        }
                        if (! meta.offsets.empty() && ! meta.hasNote.back()) {
                            meta.noteSemis.back() = semis;
                            meta.hasNote.back()   = 1;
                        }
                    }
                }
            }
            scopeStack_.assign(maxDepth + 1, ScopeFrame {});
            permRows_.assign(maxDepth + 1, std::vector<uint32_t>(maxSteps, 0));
        }

        // Stream rows: one per per-frame-advancing (control) op instance.
        streamRows_.clear();
        instrStreamIdx_.assign(instrs_.size(), -1);
        for (uint32_t i = 0; i < (uint32_t) instrs_.size(); ++i) {
            if (instrs_[i].spec->advance == Advance::PerFrame) {
                instrStreamIdx_[i] = (int32_t) streamRows_.size();
                StreamRow row;
                row.instrIdx = i;
                row.data.assign((size_t) preparedFrames_, 0.0f);
                streamRows_.push_back(std::move(row));
            }
        }
        baseStreamRowCount_ = streamRows_.size();
        densePlanRows_.clear();
        for (uint32_t planIndex = 0;
             planIndex < (uint32_t) program_.channelPlans.size();
             ++planIndex) {
            const auto& plan = program_.channelPlans[planIndex];
            for (uint32_t entryIndex = 0;
                 entryIndex < (uint32_t) plan.entries.size();
                 ++entryIndex) {
                if (plan.entries[entryIndex].sourceKind
                    != ChannelSourceKind::DenseStream)
                    continue;
                StreamRow row;
                row.instrIdx = UINT32_MAX;
                row.channelIndex = entryIndex;
                row.stableChannelId =
                    plan.entries[entryIndex].outputStableChannelId;
                row.data.assign((size_t) preparedFrames_, 0.0f);
                densePlanRows_.push_back({
                    planIndex, entryIndex, streamRows_.size()
                });
                streamRows_.push_back(std::move(row));
            }
        }

        gateOnCount_   = 0;
        lastGateOn_    = -1.0;
        lastGateOff_   = -1.0;
        lastPitch_     = 0.0f;
        prevPitch_     = 0.0f;
        lastPitchFrame_ = -1.0;
        anyPitch_      = false;
        currentSourceStep_ = -1;

        installed_ = true;

        // Configure walk: ops record static program facts (e.g. LOOP length).
        ExecCtx ctx { *this };
        ctx.phase = ExecCtx::Phase::Configure;
        for (const Instr& in : instrs_) {
            ctx.instr = &in;
            in.spec->evaluate(ctx);
        }
        return true;
    }

    // B-1739 — install the authored-source projection and exact compiled
    // local-output contract beside the ordinary decoded program. The legacy
    // one-argument install remains the unprojected timeline path.
    bool install(const Program& program,
                 const std::vector<int>& stepSourceOrdinals,
                 const std::vector<StepClockOutputBinding>& outputs)
    {
        if (! install(program))
            return false;
        if (stepSourceOrdinals.size() != (size_t) stepOrdinalCount_) {
            installed_ = false;
            return false;
        }

        for (size_t i = 0; i < instrStepOrdinal_.size(); ++i) {
            const int stepOrdinal = instrStepOrdinal_[i];
            if (stepOrdinal >= 0)
                instrSourceOrdinal_[i] =
                    stepSourceOrdinals[(size_t) stepOrdinal];
        }

        stepClockOutputs_ = outputs;
        heldStepGates_.clear();
        heldStepGates_.reserve(outputs.size());
        for (const auto& output : outputs) {
            if (output.type != SignalType::Gate)
                continue;
            HeldStepGate held;
            held.lane = output.lane;
            heldStepGates_.push_back(held);
        }
        return true;
    }

    bool isInstalled() const { return installed_; }

    // F-066 Phase 5: rewind the authoritative clock to frame 0 and clear all
    // runtime state — the transport-start seam. RT-safe by construction: no
    // allocation (clear() keeps capacity, fill touches existing storage);
    // the program, decoded instrs, scope tables and stream rows are install-
    // time facts and stay untouched.
    void resetClock()
    {
        framePos_ = 0;
        beatBase_  = 0.0;           // B-324 — re-anchor the beat clock at bar 0
        frameBase_ = 0.0;
        clockFpb_  = 0.0;           // forces a fresh anchor on the next block
        clockSeeked_ = false;
        reassertGateOnReplay_ = false;
        pending_.clear();
        std::fill(stateArena_.begin(), stateArena_.end(), (uint8_t) 0);
        std::fill(laneSlewLast_.begin(), laneSlewLast_.end(),
                  std::numeric_limits<float>::quiet_NaN());
        gateOnCount_   = 0;
        lastGateOn_    = -1.0;
        lastGateOff_   = -1.0;
        lastPitch_     = 0.0f;
        prevPitch_     = 0.0f;
        lastPitchFrame_ = -1.0;
        anyPitch_      = false;
        lastFiredStep_ = -1;
        currentSourceStep_ = -1;
        activeStepClockEvent_ = nullptr;
        sampledStepClockRelease_ = false;
        for (auto& held : heldStepGates_)
            held.held = false;
    }

    uint64_t framePosition() const { return framePos_; }

    void reassertGateOnNextReplay() noexcept { reassertGateOnReplay_ = true; }

    // F-066 Phase 5 (SF-059 T-421 FOLLOW): ordinal of the most recently
    // FIRED Step instruction (nth Step/StepAbs in program order), -1 before
    // any fire. The host maps it to a source-script step via the compile
    // artifact's stepSourceOrdinals.
    int lastFiredStepOrdinal() const { return lastFiredStep_; }
    int currentSourceStepOrdinal() const { return currentSourceStep_; }
    bool previousStepFiredForGuard(uint32_t guardInstrIdx) const
    {
        if (guardInstrIdx >= (uint32_t) nextStepOrdinalAfterInstr_.size()
            || stepOrdinalCount_ <= 0
            || lastFiredStep_ < 0)
            return false;
        const int nextOrd = nextStepOrdinalAfterInstr_[(size_t) guardInstrIdx];
        if (nextOrd < 0)
            return false;
        const int prevOrd = nextOrd == 0 ? stepOrdinalCount_ - 1 : nextOrd - 1;
        return lastFiredStep_ == prevOrd;
    }
    void noteStepFired(uint32_t instrIdx)
    {
        if (instrIdx < instrStepOrdinal_.size())
            lastFiredStep_ = instrStepOrdinal_[instrIdx];
    }

    // F-066 Phase 5: clock continuity for live recompiles — a freshly
    // installed Machine picks up the transport position of the one it
    // replaces. Same RT-safety shape as resetClock (no allocation).
    // Deterministic ops re-derive everything from the clock; the OLD
    // machine's pending gate-offs carry separately via adoptGateOff
    // (B-283) — seekClock itself clears pending_, so adoption must run
    // AFTER the seek.
    void seekClock(uint64_t frame)
    {
        resetClock();
        framePos_ = frame;
        // B-324 — re-anchor the beat clock at this frame on the next block
        // (process() sets beatBase_ = frame / framesPerBeat_ when the flag is
        // set, then clears it). frame 0 keeps the reset anchor at bar 0.
        clockSeeked_ = (frame > 0);
        // B-305 — a non-zero seek means this machine replaces one that was
        // mid-flight: the next process() runs the replay pre-pass so
        // in-flight trains (ratchet/buzz/bounce/geiger — stateful cursors
        // armed at step-fire) re-derive instead of dying with the old
        // machine. Frame 0 needs no replay (nothing can be in flight).
        replayPending_ = (frame > 0) ? (uint8_t) 1 : (uint8_t) 0;
    }

    // T-687 — host PPQ lock needs a frame correction that keeps the machine's
    // in-flight state intact. Unlike seekClock(), this only moves the clock
    // anchor; pending gate-offs, voice state, streams, and replayPending_ stay
    // live.
    void alignClockToFrame(uint64_t frame)
    {
        const double fpb = bpm_ > 0.0 ? (60.0 * sampleRate_ / bpm_) : 0.0;
        framePos_ = frame;
        frameBase_ = (double) frame;
        beatBase_ = fpb > 0.0 ? (double) frame / fpb : 0.0;
        clockFpb_ = fpb;
    }

    // B-283: live-edit gate carry. The outgoing machine's scheduled
    // gate-offs would die with it, stranding any carried gate=1 as an
    // eternal drone (B-276) — so applyInstall harvests them here and
    // adopts them into the replacement machine, preserving the original
    // release frames. Withdrawals only: pending asserts (gate-ons, sets,
    // step-end releases) belong to the old program and never carry.
    template <typename Fn>
    void forEachPendingGateOff(Fn&& fn) const
    {
        for (const auto& p : pending_)
            if (p.e.type == SignalType::Gate && p.e.kind == PacketKind::Set
                && ! isGateOpen(p.e.value)) {
                if constexpr (std::is_invocable_v<
                                  Fn, double, uint32_t,
                                  uint32_t, uint64_t>)
                    fn(p.absFrame, p.e.voice,
                       p.e.channelIndex,
                       p.e.stableChannelId);
                else
                    fn(p.absFrame, p.e.voice);
            }
    }

    // B-290 — per-note-instance id allocator. Monotone u32; install resets
    // it, adoptGateOff bumps past carried ids so a live-recompile's fresh
    // hits never collide with a ringing carried tail's slot mapping.
    uint32_t nextVoiceInstance() { lastVoiceInstance_ = voiceSeq_++; return lastVoiceInstance_; }
    uint32_t lastVoiceInstance() const { return lastVoiceInstance_; }

    void adoptGateOff(double absFrame, uint32_t voice,
                      uint32_t channelIndex = 0,
                      uint64_t stableChannelId = 0)
    {
        lastGateOff_ = std::max(lastGateOff_, absFrame);
        if (voice != UINT32_MAX && voice + 1 > voiceSeq_) {
            voiceSeq_ = voice + 1;
            lastVoiceInstance_ = voice;
        }
        Emission e;
        e.type  = SignalType::Gate;
        e.kind  = PacketKind::Set;
        e.value = 0.0f;
        e.voice = voice;
        e.channelIndex = channelIndex;
        e.stableChannelId = stableChannelId;
        if (! rtBounded_ || pending_.size() < pending_.capacity())
            pending_.push_back({ absFrame, e });
        else
            ++rtOverflowDropsThisBlock_;
    }

    // F-066 Phase 5 (T-422 PREVIEW, B-252): authoring preview window. When
    // active, the clock wraps back to startFrame whenever it sits outside
    // [startFrame, endFrame) at block start — block-granular wrap, full
    // determinism within the window (iteration derives from the wrapped
    // frame position). Pending gate-offs beyond the window clear on wrap.
    void setPreviewWindow(uint64_t startFrame, uint64_t endFrame, bool active)
    {
        previewStart_  = startFrame;
        previewEnd_    = endFrame;
        previewActive_ = active && endFrame > startFrame;
    }

    void setLoopLength(double beats) { loopLen_ = beats; }
    double loopLength() const        { return loopLen_; }

    void updateCurrentSourceStep(double beat)
    {
        currentSourceStep_ = -1;
        if (program_.timeline.empty())
            return;

        const double len = loopLen_ > 0.0 ? loopLen_ : 0.0;
        double pos = beat;
        if (len > 0.0)
        {
            pos = std::fmod(pos, len);
            if (pos < 0.0) pos += len;
        }

        for (const auto& step : program_.timeline)
        {
            if (step.durationBeats <= 0.0f || step.sourceOrdinal < 0)
                continue;

            const double start = (double) step.offsetBeats;
            const double end   = start + (double) step.durationBeats;
            if (len > 0.0 && end > len)
            {
                if (pos >= start || pos < end - len)
                {
                    currentSourceStep_ = step.sourceOrdinal;
                    return;
                }
            }
            else if (pos >= start && pos < end)
            {
                currentSourceStep_ = step.sourceOrdinal;
                return;
            }
        }
    }

    int32_t metaIndexOf(uint32_t instrIdx) const
    {
        return instrIdx < instrMetaIdx_.size() ? instrMetaIdx_[instrIdx] : -1;
    }

    // Run one block. Appends emissions (sorted by frameOffset) to `out`.
    // The frame axis (framePos_) is the monotonic firing clock; the beat
    // position is an anchored piecewise-linear function of frame (B-324):
    // beat(f) = beatBase_ + (f - frameBase_)/framesPerBeat_, re-anchored only
    // on a tempo change or seek. At fixed tempo beat(f) == f/fpb exactly
    // (block-size-independent); under a continuous @bpm:~lfo sweep the slope
    // changes per tempo span, so the schedule glides smoothly instead of
    // jumping (the old code divided an ever-growing frame counter by the live
    // framesPerBeat — B-324 cluster/gap).
    void process(int numFrames, std::vector<Emission>& out)
    {
        rtOverflowDropsThisBlock_ = 0;
        if (! installed_ || numFrames <= 0)
            return;

        if (numFrames > preparedFrames_)
            return;   // contract: prepare() first

        const size_t firstNew = out.size();
        framesPerBeat_ = 60.0 * sampleRate_ / bpm_;

        // B-324 — anchored piecewise-linear beat clock. The frame axis
        // (framePos_) is the monotonic firing clock; the beat position is a
        // LINEAR function of frame within each constant-tempo span, anchored at
        // (frameBase_, beatBase_): beat(f) = beatBase_ + (f - frameBase_)/fpb.
        // The anchor moves only when the tempo (fpb) changes or the clock is
        // seeked — so at FIXED tempo beat(f) == f/fpb exactly (no accumulator
        // drift, fully block-size-independent, bit-identical to the old frame-
        // domain clock). When @bpm:~lfo changes fpb at a block boundary we
        // re-anchor at the beat already reached, so the schedule stays
        // CONTINUOUS in beats across the change instead of jumping (B-324).
        if (clockSeeked_ || clockFpb_ <= 0.0) {
            // Fresh start or post-seek: anchor at the current frame + tempo
            // (preserves seekClock's "resume at frame N" semantics).
            clockSeeked_ = false;
            frameBase_ = (double) framePos_;
            beatBase_  = framesPerBeat_ > 0.0 ? (double) framePos_ / framesPerBeat_ : 0.0;
            clockFpb_  = framesPerBeat_;
        } else if (framesPerBeat_ != clockFpb_) {
            // Tempo changed at this block boundary → re-anchor at the beat we
            // have reached, then accumulate beats at the new rate going forward.
            beatBase_ += ((double) framePos_ - frameBase_) / clockFpb_;
            frameBase_ = (double) framePos_;
            clockFpb_  = framesPerBeat_;
        }

        // Preview wrap (block-granular): outside the window → jump to its
        // start. Pending entries beyond the window die with the jump.
        if (previewActive_
            && (framePos_ < previewStart_ || framePos_ >= previewEnd_)) {
            framePos_  = previewStart_;
            frameBase_ = (double) framePos_;   // re-anchor the beat clock on the wrap
            beatBase_  = framesPerBeat_ > 0.0 ? (double) framePos_ / framesPerBeat_ : 0.0;
            clockFpb_  = framesPerBeat_;
            pending_.clear();
        }

        blockStartFrame_ = (double) framePos_;
        blockStartBeat_  = beatAtFrame(blockStartFrame_);
        blockEndFrame_   = (double) (framePos_ + (uint64_t) numFrames);
        updateCurrentSourceStep(blockStartBeat_);

        // B-305 — replay pre-pass. First block after an install-seek: walk
        // the current loop iteration's prefix [iterStart, seekFrame) in
        // Fire phase with all asserts suppressed. Deterministic ops
        // re-derive byte-identical state from the clock, so every train
        // armed before the seek point ends up with its cursor exactly
        // where the displaced machine left it — the remaining sub-hits
        // fire on schedule instead of dying with the edit (Neo s500:
        // typing muted the ratcheted arps). Suppression drops only the
        // pre-seek PACKETS; gate/pitch history records normally, so
        // trigger-coupled streams re-anchor as if the machine had been
        // running all along. The ringing note's gate itself carries via
        // B-283 (applyInstall), not here. Trains that span a loop wrap
        // are out of replay reach (prefix-only walk) — accepted edge.
        bool reassertReplayGate = false;
        bool reassertReplayPitch = false;
        float replayPitch = 0.0f;
        if (replayPending_) {
            replayPending_ = 0;
            const double lf = loopLen_ * framesPerBeat_;
            if (lf > 0.0 && blockStartFrame_ > 0.0) {
                // Beat-anchored (B-324): the current iteration is keyed off the
                // beat position, and its start frame maps back through the
                // block's beat↔frame anchor.
                const uint64_t rIter =
                    (uint64_t) std::floor(beatAtFrame(blockStartFrame_) / loopLen_);
                const double iterStart =
                    frameOfBeat((double) rIter * loopLen_);
                if (blockStartFrame_ > iterStart) {
                    emitOut_        = &out;
                    suppressBefore_ = blockStartFrame_;
                    walk(iterStart, blockStartFrame_, rIter, ExecCtx::Phase::Fire);
                    suppressBefore_ = -1.0;
                    emitOut_        = nullptr;
                }
            }
            reassertReplayGate = reassertGateOnReplay_
                && lastGateOn_ <= blockStartFrame_
                && (lastGateOff_ < lastGateOn_ || lastGateOff_ >= blockStartFrame_);
            reassertReplayPitch = anyPitch_;
            replayPitch = lastPitch_;
            reassertGateOnReplay_ = false;
        }

        beginProcessBlockState(numFrames);

        // A hard seek into a held note has no displaced Delivery to carry note
        // state when the machine is brand new (hosted clip install while the
        // DAW is already playing). Reassert the current note at block start;
        // the replay pre-pass above already scheduled its future gate-off.
        if (reassertReplayGate) {
            if (reassertReplayPitch) {
                appendEmission(out, { SignalType::Pitch, PacketKind::Set, replayPitch,
                                      0.0f, 0u, 0u, 0u });
                appendPitchSet({ blockStartFrame_, replayPitch, replayPitch });
            }
            appendEmission(out, { SignalType::Gate, PacketKind::Set, 1.0f,
                                  0.0f, 0u, 0u, 0u });
            appendBlockFrame(blockGateOns_, blockStartFrame_);
        }

        // Split the block window into loop-iteration-aligned segments so a
        // step can fire at most once per walk. Each segment is walked twice:
        // Fire (steps land, gates counted), then Render (streams) — so
        // trigger-coupled streams see the segment's full gate history.
        emitOut_ = &out;
        const double loopFrames = loopLen_ * framesPerBeat_;
        double segStart = blockStartFrame_;
        // T-465 — under a preview window the walk never crosses previewEnd_:
        // the block-granular wrap above fires next block, and a step sitting
        // exactly at the window end used to fire in this block's tail —
        // inaudible (its gate died on the wrap's pending-clear) but it
        // stamped lastFiredStepOrdinal, so FOLLOW showed a step outside the
        // window. Frames in [previewEnd_, blockEnd) are dead; the clock
        // (framePos_) still advances the full block.
        const double walkEnd = previewActive_
            ? std::min(blockEndFrame_, (double) previewEnd_)
            : blockEndFrame_;
        while (segStart < walkEnd) {
            uint64_t iter = 0;
            double   segEnd = walkEnd;
            if (loopFrames > 0.0) {
                // B-324 — iteration keys off the BEAT at segStart (monotonic
                // beat axis), and the loop boundary maps back to a frame through
                // the block's beat↔frame anchor — so a tempo change can't move
                // the boundary relative to already-walked frames.
                const double segStartBeat = beatAtFrame(segStart);
                iter   = (uint64_t) std::floor(segStartBeat / loopLen_);
                segEnd = std::min(walkEnd, frameOfBeat((double) (iter + 1) * loopLen_));
                // B-278 — fractional framesPerBeat (e.g. bpm 186 @48k):
                // when segStart sits ON an iteration boundary but the
                // floor rounds DOWN to the previous iter, segEnd ==
                // segStart and the old degenerate guard broke out of the
                // walk — silently dropping every step left in the block
                // (Neo: "the first tone note skips on the third loop, every
                // time"). The boundary frame belongs to the NEXT iteration:
                // step into it and recompute the segment end.
                if (segEnd <= segStart) {
                    iter  += 1;
                    segEnd = std::min(walkEnd, frameOfBeat((double) (iter + 1) * loopLen_));
                }
            }
            walk(segStart, segEnd, iter, ExecCtx::Phase::Fire);
            std::sort(blockGateOns_.begin(), blockGateOns_.end());
            std::sort(blockGateOffs_.begin(), blockGateOffs_.end());
            std::sort(blockPitchSets_.begin(), blockPitchSets_.end(),
                      [] (const PitchSet& a, const PitchSet& b) {
                          return a.frame < b.frame;
                      });
            walk(segStart, segEnd, iter, ExecCtx::Phase::Render);
            if (segEnd <= segStart)
                break;   // degenerate guard (loop shorter than fp epsilon)
            segStart = segEnd;
        }

        finishProcessBlock(numFrames, out, firstNew);
    }

    // B-1739 — execute a sampled product-Step plan through the same decoded
    // instruction walk, pending lifecycle and final packet ordering as the
    // ordinary timeline. The adapter owns clock decisions; Machine remains the
    // sole packet owner.
    void processStepClockPlan(const StepClockPlanView& plan,
                              std::vector<Emission>& out)
    {
        rtOverflowDropsThisBlock_ = 0;
        if (! installed_ || plan.numSamples <= 0
            || plan.numSamples > preparedFrames_
            || (plan.count > 0 && plan.events == nullptr))
            return;

        const int numFrames = plan.numSamples;
        const size_t firstNew = out.size();
        framesPerBeat_ = 60.0 * sampleRate_ / bpm_;
        if (clockSeeked_ || clockFpb_ <= 0.0) {
            clockSeeked_ = false;
            frameBase_ = (double) framePos_;
            beatBase_ = framesPerBeat_ > 0.0
                ? (double) framePos_ / framesPerBeat_ : 0.0;
            clockFpb_ = framesPerBeat_;
        } else if (framesPerBeat_ != clockFpb_) {
            beatBase_ += ((double) framePos_ - frameBase_) / clockFpb_;
            frameBase_ = (double) framePos_;
            clockFpb_ = framesPerBeat_;
        }

        blockStartFrame_ = (double) framePos_;
        blockStartBeat_ = beatAtFrame(blockStartFrame_);
        blockEndFrame_ = (double) (framePos_ + (uint64_t) numFrames);
        beginProcessBlockState(numFrames);
        emitOut_ = &out;

        for (size_t i = 0; i < plan.count; ++i) {
            const StepClockEvent& event = plan.events[i];
            if (event.frameOffset >= (uint32_t) numFrames)
                continue;

            activeStepClockEvent_ = &event;
            currentSourceStep_ = (int) event.sourceOrdinal;

            // A combined edge is a sampled withdrawal followed by the new
            // assertion. The shared final sort keeps that lifecycle order even
            // when the walk inserts pitch/velocity packets between them.
            if ((event.flags & (uint8_t) GateOff) != 0)
                releaseHeldStepClockGates(event);

            if ((event.flags & (uint8_t) GateOn) != 0) {
                nextVoiceInstance();
                walk(blockStartFrame_, blockEndFrame_, event.iteration,
                     ExecCtx::Phase::Fire);
            } else if ((event.flags & (uint8_t) Reaim) != 0) {
                for (const auto& held : heldStepGates_) {
                    if (held.held) {
                        lastVoiceInstance_ = held.voice;
                        break;
                    }
                }
                walk(blockStartFrame_, blockEndFrame_, event.iteration,
                     ExecCtx::Phase::Fire);
            }
        }

        activeStepClockEvent_ = nullptr;
        sampledStepClockRelease_ = false;
        finishProcessBlock(numFrames, out, firstNew);
    }

    uint32_t drainRtOverflowDrops()
    {
        const uint32_t n = rtOverflowDropsThisBlock_;
        rtOverflowDropsThisBlock_ = 0;
        return n;
    }

    void applyActivatedPacketPlans(
        std::vector<Emission>& emissions,
        std::size_t firstInput,
        std::uint32_t outputSignalOffset)
    {
        applyActiveChannelPlans(
            emissions, firstInput,
            ChannelSourceKind::PacketInput,
            outputSignalOffset);
    }

private:
    struct HeldStepGate {
        uint32_t lane = 0;
        uint32_t voice = 0;
        uint32_t channelIndex = 0;
        uint64_t stableChannelId = 0;
        bool held = false;
    };

    void beginProcessBlockState(int numFrames)
    {
        std::fill(activeChannelPlans_.begin(), activeChannelPlans_.end(), 0);
        for (auto& row : streamRows_) {
            row.active = false;
            row.midi = false;
            std::fill(row.data.begin(), row.data.begin() + numFrames, 0.0f);
        }
        gateCountAtBlockStart_ = gateOnCount_;
        lastGateOnAtStart_ = lastGateOn_;
        lastGateOffAtStart_ = lastGateOff_;
        blockGateOns_.clear();
        blockGateOffs_.clear();
        blockPitchSets_.clear();
        pitchAtStart_ = { lastPitch_, prevPitch_, lastPitchFrame_, anyPitch_ };
    }

    void finishProcessBlock(int numFrames, std::vector<Emission>& out,
                            size_t firstNew)
    {
        // Drain scheduled emissions due in this block after the decoded walk.
        for (size_t i = 0; i < pending_.size();) {
            if (pending_[i].absFrame < blockEndFrame_) {
                Emission e = pending_[i].e;
                e.frameOffset = (float) std::max(
                    0.0, pending_[i].absFrame - blockStartFrame_);
                appendEmission(out, e);
                pending_[i] = pending_.back();
                pending_.pop_back();
            } else {
                ++i;
            }
        }
        emitOut_ = nullptr;
        applyActiveDensePlans(numFrames);
        applyActiveChannelPlans(
            out, firstNew,
            ChannelSourceKind::LocalEmission,
            0u);

        // Shared allocation-free ordering: withdrawals, releases, values,
        // assertions. Original index is the stable final tiebreak.
        const size_t nSort = out.size() - firstNew;
        if (nSort > 1) {
            if (rtBounded_
                && (nSort > sortKeys_.capacity()
                    || nSort > sortTmp_.capacity())) {
                rtOverflowDropsThisBlock_ += (uint32_t) nSort;
                framePos_ += (uint64_t) numFrames;
                return;
            }
            Emission* base = out.data() + firstNew;
            sortKeys_.resize(nSort);
            for (uint32_t i = 0; i < (uint32_t) nSort; ++i)
                sortKeys_[i] = {
                    base[i].frameOffset, emissionTieRank(base[i]), i
                };
            std::sort(sortKeys_.begin(), sortKeys_.end(),
                      [] (const SortKey& a, const SortKey& b) {
                          if (a.frame != b.frame) return a.frame < b.frame;
                          if (a.rank != b.rank) return a.rank < b.rank;
                          return a.idx < b.idx;
                      });
            sortTmp_.resize(nSort);
            for (size_t i = 0; i < nSort; ++i)
                sortTmp_[i] = base[sortKeys_[i].idx];
            for (size_t i = 0; i < nSort; ++i)
                base[i] = sortTmp_[i];
        }
        framePos_ += (uint64_t) numFrames;
    }

    void noteHeldStepClockGate(uint32_t lane, uint32_t voice,
                               uint32_t channelIndex,
                               uint64_t stableChannelId)
    {
        for (auto& held : heldStepGates_) {
            if (held.lane != lane)
                continue;
            held.voice = voice;
            held.channelIndex = channelIndex;
            held.stableChannelId = stableChannelId;
            held.held = true;
            return;
        }
    }

    void releaseHeldStepClockGates(const StepClockEvent& event)
    {
        const double absFrame =
            blockStartFrame_ + (double) event.frameOffset;
        ExecCtx ctx { *this };
        ctx.phase = ExecCtx::Phase::Fire;
        uint32_t releaseVoice = lastVoiceInstance_;
        sampledStepClockRelease_ = true;
        for (auto& held : heldStepGates_) {
            if (! held.held)
                continue;
            releaseVoice = held.voice;
            ctx.scheduleGateOff(
                absFrame, held.voice, 0, held.lane,
                held.channelIndex, held.stableChannelId);
            held.held = false;
        }
        for (const auto& output : stepClockOutputs_) {
            if (output.type != SignalType::Velocity || ! output.usePlanValue)
                continue;
            ctx.schedule(SignalType::Velocity, PacketKind::Set, 0.0f,
                         absFrame, releaseVoice, output.lane);
        }
        sampledStepClockRelease_ = false;
    }

    // B-324 — beat↔frame through the STABLE clock anchor (frameBase_,beatBase_),
    // not the per-block (blockStartFrame_, blockStartBeat_) origin: the anchor
    // moves only on a tempo change, so these are pure functions of position and
    // fpb — identical regardless of how the timeline is chunked into blocks
    // (block-size independence). The slope changes only between tempo spans, so
    // a live tempo change never moves frames already walked.
    double frameOfBeat(double beat) const
    {
        return frameBase_ + (beat - beatBase_) * framesPerBeat_;
    }
    double beatAtFrame(double frame) const
    {
        return beatBase_ + (frame - frameBase_) / framesPerBeat_;
    }

    struct PendingEmission {
        double   absFrame = 0.0;
        Emission e;
    };
    struct PitchSet { double frame; float value; float prev; };

    bool appendEmission(std::vector<Emission>& out, const Emission& e)
    {
        if (! rtBounded_ || out.size() < out.capacity()) {
            out.push_back(e);
            return true;
        }
        ++rtOverflowDropsThisBlock_;
        return false;
    }

    bool appendPending(PendingEmission p)
    {
        if (! rtBounded_ || pending_.size() < pending_.capacity()) {
            pending_.push_back(p);
            return true;
        }
        ++rtOverflowDropsThisBlock_;
        return false;
    }

    bool appendBlockFrame(std::vector<double>& frames, double frame)
    {
        if (! rtBounded_ || frames.size() < frames.capacity()) {
            frames.push_back(frame);
            return true;
        }
        ++rtOverflowDropsThisBlock_;
        return false;
    }

    bool appendPitchSet(PitchSet pitch)
    {
        if (! rtBounded_ || blockPitchSets_.size() < blockPitchSets_.capacity()) {
            blockPitchSets_.push_back(pitch);
            return true;
        }
        ++rtOverflowDropsThisBlock_;
        return false;
    }

    void applyActiveChannelPlans(
        std::vector<Emission>& out,
        size_t firstNew,
        ChannelSourceKind sourceKind,
        uint32_t outputSignalOffset)
    {
        bool anyActive = false;
        for (const uint8_t active : activeChannelPlans_)
            anyActive = anyActive || active != 0;
        if (! anyActive || firstNew >= out.size())
            return;

        channelMapTmp_.clear();
        const size_t sourceCount = out.size() - firstNew;
        if (rtBounded_ && sourceCount > channelMapTmp_.capacity()) {
            rtOverflowDropsThisBlock_ += (uint32_t) sourceCount;
            out.resize(firstNew);
            return;
        }
        channelMapTmp_.insert(channelMapTmp_.end(),
                              out.begin() + (ptrdiff_t) firstNew, out.end());
        out.resize(firstNew);

        // Preserve material no active plan references.
        for (const auto& emission : channelMapTmp_) {
            bool signalGoverned = false;
            for (uint32_t p = 0;
                 p < activeChannelPlans_.size() && ! signalGoverned;
                 ++p) {
                if (! activeChannelPlans_[p])
                    continue;
                for (const auto& entry : program_.channelPlans[p].entries) {
                    if (entry.sourceKind != sourceKind)
                        continue;
                    uint32_t signal = isNamedOutputLane(emission.lane)
                        ? namedOutputLaneIndex(emission.lane) : emission.lane;
                    if (sourceKind == ChannelSourceKind::PacketInput)
                        for (const auto& binding :
                             packetInputSignalBindings_)
                            if (binding.first == signal) {
                                signal = binding.second;
                                break;
                            }
                    if (entry.sourceSignal == signal) {
                        signalGoverned = true;
                        break;
                    }
                }
            }
            if (! signalGoverned)
                appendEmission(out, emission);
        }

        // Ordered gather: plan order, then entry order, then original packet
        // order. Every field is copied; only explicit channel identity changes.
        for (uint32_t p = 0; p < activeChannelPlans_.size(); ++p) {
            if (! activeChannelPlans_[p])
                continue;
            const auto& plan = program_.channelPlans[p];
            for (uint32_t outputChannel = 0;
                 outputChannel < (uint32_t) plan.entries.size();
                 ++outputChannel) {
                const auto& entry = plan.entries[outputChannel];
                if (entry.sourceKind != sourceKind)
                    continue;
                for (const auto& source : channelMapTmp_) {
                    uint32_t signal = isNamedOutputLane(source.lane)
                        ? namedOutputLaneIndex(source.lane) : source.lane;
                    if (sourceKind == ChannelSourceKind::PacketInput)
                        for (const auto& binding :
                             packetInputSignalBindings_)
                            if (binding.first == signal) {
                                signal = binding.second;
                                break;
                            }
                    if (entry.sourceSignal != signal
                        || entry.sourceChannel != source.channelIndex)
                        continue;
                    Emission addressed = source;
                    if (plan.outputSignal
                        > ~kNamedOutputLaneFlag
                            - outputSignalOffset) {
                        ++rtOverflowDropsThisBlock_;
                        continue;
                    }
                    addressed.lane = namedOutputLane(
                        plan.outputSignal
                        + outputSignalOffset);
                    addressed.channelIndex = outputChannel;
                    if (! entry.preserveSourceStableChannelId
                        && entry.outputStableChannelId != 0)
                        addressed.stableChannelId =
                            entry.outputStableChannelId;
                    appendEmission(out, addressed);
                }
            }
        }
    }

    void applyActiveDensePlans(int numFrames)
    {
        for (const auto& binding : densePlanRows_) {
            if (! activeChannelPlans_[binding.planIndex])
                continue;
            const auto& entry =
                program_.channelPlans[binding.planIndex].entries[
                    binding.entryIndex];
            const StreamRow* source = nullptr;
            for (size_t i = 0; i < baseStreamRowCount_; ++i) {
                const auto& candidate = streamRows_[i];
                if (! candidate.active
                    || candidate.lane != entry.sourceSignal
                    || candidate.channelIndex != entry.sourceChannel)
                    continue;
                if (source == nullptr
                    || candidate.instrIdx > source->instrIdx)
                    source = &candidate;
            }
            if (source == nullptr)
                continue;
            auto& output = streamRows_[binding.rowIndex];
            output.active = true;
            output.midi = source->midi;
            output.lane =
                program_.channelPlans[binding.planIndex].outputSignal;
            output.channelIndex = binding.entryIndex;
            output.stableChannelId =
                ! entry.preserveSourceStableChannelId
                    && entry.outputStableChannelId != 0
                    ? entry.outputStableChannelId
                    : source->stableChannelId;
            output.type = source->type;
            output.kind = source->kind;
            std::copy_n(source->data.begin(), numFrames,
                        output.data.begin());
        }

        // Every channel of a governed dense source is replaced by the
        // gathered outputs. Channels omitted from the plan are an explicit
        // structural drop.
        for (size_t i = 0; i < baseStreamRowCount_; ++i) {
            auto& source = streamRows_[i];
            if (! source.active)
                continue;
            for (uint32_t p = 0; p < activeChannelPlans_.size(); ++p) {
                if (! activeChannelPlans_[p])
                    continue;
                for (const auto& entry : program_.channelPlans[p].entries) {
                    if (entry.sourceKind == ChannelSourceKind::DenseStream
                        && entry.sourceSignal == source.lane) {
                        source.active = false;
                        break;
                    }
                }
                if (! source.active)
                    break;
            }
        }
    }

    void walk(double segStart, double segEnd, uint64_t iteration,
              ExecCtx::Phase phase)
    {
        ExecCtx ctx { *this };
        ctx.phase     = phase;
        ctx.segStart  = segStart;
        ctx.segEnd    = segEnd;
        ctx.iteration = iteration;
        scopeStack_[0] = ScopeFrame {};   // reset the root frame
        for (uint32_t i = 0; i < (uint32_t) instrs_.size(); ++i) {
            ctx.instr      = &instrs_[i];
            ctx.instrIndex = i;
            instrs_[i].spec->evaluate(ctx);
            if (ctx.halted)
                break;
        }
    }

    double   sampleRate_;
    double   bpm_       = 120.0;
    uint64_t framePos_  = 0;       // monotonic frame axis (the firing clock)
    // B-324 — anchored piecewise-linear beat clock: beat(f) = beatBase_ +
    // (f - frameBase_) / clockFpb_. The anchor moves only on a tempo change or
    // seek, so at fixed tempo beat(f) == f/fpb exactly (no drift, chunk-free).
    double   beatBase_    = 0.0;
    double   frameBase_   = 0.0;
    double   clockFpb_    = 0.0;   // framesPerBeat in force since the anchor (0 = uninit)
    bool     clockSeeked_ = false; // B-324 — a seek set framePos_ directly this block
    uint8_t  replayPending_  = 0;   // B-305 — set by a non-zero seekClock
    bool     reassertGateOnReplay_ = false;
    double   suppressBefore_ = -1.0; // B-305 — replay pre-pass assert gate
    uint64_t previewStart_ = 0, previewEnd_ = 0;
    bool     previewActive_ = false;
    double   loopLen_   = 0.0;     // beats
    bool     installed_ = false;

    double blockStartFrame_ = 0.0;
    double blockStartBeat_  = 0.0;   // B-324 — beat at block start (map anchor)
    double blockEndFrame_   = 0.0;
    double framesPerBeat_   = 0.0;

    Program program_;
    std::vector<Instr>           instrs_;
    std::vector<uint8_t>         stateArena_;
    std::vector<float>           laneSlewLast_;   // per-lane ~random slew (NaN = uninited)
    std::vector<PendingEmission> pending_;
    std::vector<Emission>*       emitOut_ = nullptr;
    bool                         rtBounded_ = false;
    uint32_t                     rtOverflowDropsThisBlock_ = 0;

    std::vector<int32_t>               instrMetaIdx_;
    std::vector<int32_t>               instrStepOrdinal_;
    std::vector<int32_t>               instrSourceOrdinal_;
    std::vector<int32_t>               nextStepOrdinalAfterInstr_;
    int                                stepOrdinalCount_ = 0;
    int                                lastFiredStep_ = -1;
    int                                currentSourceStep_ = -1;
    std::vector<ScopeMeta>             scopeMetas_;
    std::vector<ScopeFrame>            scopeStack_;   // preallocated at install
    std::vector<std::vector<uint32_t>> permRows_;     // per-depth scratch
    std::vector<StepClockOutputBinding> stepClockOutputs_;
    std::vector<HeldStepGate>           heldStepGates_;
    const StepClockEvent*               activeStepClockEvent_ = nullptr;
    bool                                sampledStepClockRelease_ = false;

    // Streams + per-block gate/pitch history.
    int preparedFrames_ = 4096;
    std::vector<StreamRow> streamRows_;
    struct DensePlanRow {
        uint32_t planIndex = 0;
        uint32_t entryIndex = 0;
        size_t rowIndex = 0;
    };
    size_t baseStreamRowCount_ = 0;
    std::vector<DensePlanRow> densePlanRows_;
    const float* const* externalInputRows_ = nullptr;
    int externalInputCount_ = 0;
    int externalInputStride_ = 0;
    std::vector<int32_t>   instrStreamIdx_;

    // Borrowed host CC matrix (see setMidiCCSource).
    const std::atomic<float>* midiCC_ = nullptr;
    int midiCCChannels_ = 0, midiCCCount_ = 0;

    uint64_t gateOnCount_ = 0;
    uint32_t voiceSeq_ = 0;            // B-290 — next per-note-instance id
    uint32_t lastVoiceInstance_ = 0;   // held-voice target for GATE_ONLY/PITCH_SET
    double   lastGateOn_  = -1.0;
    double   lastGateOff_ = -1.0;   // latest SCHEDULED off (abs frame, may be future)
    float    lastPitch_ = 0.0f, prevPitch_ = 0.0f;
    double   lastPitchFrame_ = -1.0;
    bool     anyPitch_ = false;
    uint64_t gateCountAtBlockStart_ = 0;
    double   lastGateOnAtStart_ = -1.0;
    double   lastGateOffAtStart_ = -1.0;
    ExecCtx::PitchAt        pitchAtStart_ { 0.0f, 0.0f, -1.0, false };
    std::vector<double>     blockGateOns_;
    std::vector<double>     blockGateOffs_;   // scheduled-off frames, recorded at schedule time
    std::vector<PitchSet>   blockPitchSets_;
    // Allocation-free per-block emission sort scratch. RT hosts call prepare()
    // with an event reserve so these never grow from process().
    struct SortKey { float frame; int32_t rank; uint32_t idx; };
    std::vector<SortKey>    sortKeys_;
    std::vector<Emission>   sortTmp_;
    std::vector<Emission>   channelMapTmp_;
    std::vector<uint8_t>    activeChannelPlans_;
    // Prepared runtime lane -> immutable program-local packet input signal.
    std::vector<std::pair<uint32_t, uint32_t>>
        packetInputSignalBindings_;

    friend struct ExecCtx;
};

// ── ExecCtx service definitions ───────────────────────────────────

inline const uint8_t* ExecCtx::codeAt(uint32_t off) const
{
    return machine.program_.code.data() + off;
}

inline bool ExecCtx::externalStepClockActive() const
{
    return machine.activeStepClockEvent_ != nullptr;
}

inline bool ExecCtx::externalStepClockMatches(uint32_t instrIdx) const
{
    if (machine.activeStepClockEvent_ == nullptr
        || instrIdx >= machine.instrSourceOrdinal_.size())
        return false;
    const int source = machine.instrSourceOrdinal_[instrIdx];
    return source >= 0
        && (uint32_t) source == machine.activeStepClockEvent_->sourceOrdinal;
}

inline bool ExecCtx::externalStepClockReaim() const
{
    return machine.activeStepClockEvent_ != nullptr
        && (machine.activeStepClockEvent_->flags & (uint8_t) Reaim) != 0;
}

inline bool ExecCtx::externalStepClockSuppressesGateOff() const
{
    return machine.activeStepClockEvent_ != nullptr
        && ! machine.sampledStepClockRelease_
        && (machine.activeStepClockEvent_->flags
            & (uint8_t) (GateOn | Reaim)) != 0;
}

inline double ExecCtx::externalStepClockFrame() const
{
    return machine.blockStartFrame_
        + (double) machine.activeStepClockEvent_->frameOffset;
}

inline float ExecCtx::externalStepClockOutputValue(
    uint32_t lane, SignalType type, float compiledValue) const
{
    if (machine.activeStepClockEvent_ == nullptr)
        return compiledValue;
    for (const auto& output : machine.stepClockOutputs_) {
        if (output.lane != lane || output.type != type
            || ! output.usePlanValue)
            continue;
        if (type == SignalType::Pitch)
            return machine.activeStepClockEvent_->pitch;
        if (type == SignalType::Velocity)
            return machine.activeStepClockEvent_->velocity;
        break;
    }
    return compiledValue;
}

inline void ExecCtx::emit(SignalType type, PacketKind kind, float value,
                          double absFrame, uint32_t voice, uint32_t lane,
                          uint32_t voiceIdx, uint32_t channelIndex,
                          uint64_t stableChannelId)
{
    if (externalStepClockReaim()
        && type == SignalType::Gate && kind == PacketKind::Set
        && isGateOpen(value))
        return;

    Emission e;
    e.type        = type;
    e.kind        = kind;
    e.value       = value;
    e.frameOffset = (float) std::max(0.0, absFrame - machine.blockStartFrame_);
    e.voice       = voice;
    e.lane        = lane;
    e.channelIndex = channelIndex;
    e.stableChannelId = stableChannelId;
    e.voiceIdx    = voiceIdx;
    // B-305 replay pre-pass: pre-seek asserts are the already-sounded past —
    // drop the packet, keep the history bookkeeping below (streams anchor).
    if (absFrame >= machine.suppressBefore_)
        machine.appendEmission(*machine.emitOut_, e);

    // Gate/pitch history for trigger-coupled streams (envelopes, random,
    // accum, keytrack, glide).
    if (type == SignalType::Gate && kind == PacketKind::Set && isGateOpen(value)) {
        if (externalStepClockActive())
            machine.noteHeldStepClockGate(
                lane, voice, channelIndex, stableChannelId);
        ++machine.gateOnCount_;
        machine.lastGateOn_ = std::max(machine.lastGateOn_, absFrame);
        machine.appendBlockFrame(machine.blockGateOns_, absFrame);
    } else if (type == SignalType::Pitch && voiceIdx == 0) {
        machine.appendPitchSet({ absFrame, value,
                                 machine.anyPitch_ ? machine.lastPitch_ : value });
        machine.prevPitch_ = machine.anyPitch_ ? machine.lastPitch_ : value;
        machine.lastPitch_ = value;
        machine.lastPitchFrame_ = absFrame;
        machine.anyPitch_ = true;
    }
}

inline void ExecCtx::emitTypedParam(uint16_t lane, float authoredValue,
                                    uint8_t basis, float stepBeats,
                                    bool isOffset, double absFrame)
{
    Emission e;
    e.type = SignalType::Value;
    e.kind = isOffset ? PacketKind::Offset : PacketKind::Set;
    e.value = 0.0f;
    e.frameOffset = (float) std::max(0.0, absFrame - machine.blockStartFrame_);
    e.lane = lane;
    e.typedParam = 1;
    e.authoredBasis = basis;
    e.authoredValue = authoredValue;
    e.authoredStepBeats = stepBeats;
    if (absFrame >= machine.suppressBefore_)
        machine.appendEmission(*machine.emitOut_, e);
}

inline void ExecCtx::schedule(SignalType type, PacketKind kind, float value,
                              double absFrame, uint32_t voice, uint32_t lane,
                              uint32_t voiceIdx, uint32_t channelIndex,
                              uint64_t stableChannelId)
{
    if (externalStepClockReaim()
        && type == SignalType::Gate && kind == PacketKind::Set
        && isGateOpen(value))
        return;

    // B-305 replay pre-pass: never queue pre-seek asserts — they would
    // drain at this block's end as a same-frame burst of the whole replayed
    // prefix. (Callers' history bookkeeping has already run; that is the
    // part the replay wants.)
    if (absFrame < machine.suppressBefore_)
        return;
    Emission e;
    e.type     = type;
    e.kind     = kind;
    e.value    = value;
    e.voice    = voice;
    e.voiceIdx = voiceIdx;
    e.lane  = lane;
    e.channelIndex = channelIndex;
    e.stableChannelId = stableChannelId;
    machine.appendPending({ absFrame, e });
    if (externalStepClockActive()
        && type == SignalType::Gate && kind == PacketKind::Set
        && isGateOpen(value))
        machine.noteHeldStepClockGate(
            lane, voice, channelIndex, stableChannelId);
}

inline void ExecCtx::scheduleGateOff(double absFrame, uint32_t voice,
                                     uint32_t voiceIdx, uint32_t lane,
                                     uint32_t channelIndex,
                                     uint64_t stableChannelId)
{
    if (externalStepClockSuppressesGateOff())
        return;

    // Off history is recorded at SCHEDULE time (the off frame is known the
    // moment the gate fires, even when it lands in a future block) — it
    // pairs with the gate-on history retriggered envelopes anchor to.
    machine.lastGateOff_ = std::max(machine.lastGateOff_, absFrame);
    machine.appendBlockFrame(machine.blockGateOffs_, absFrame);
    schedule(SignalType::Gate, PacketKind::Set, 0.0f, absFrame, voice, lane,
             voiceIdx, channelIndex, stableChannelId);
}

inline void ExecCtx::scheduleHit(float pitchSignal, float vel, double absFrame,
                                 double gateFrames, uint32_t voice,
                                 uint32_t voiceIdx)
{
    // A full hit at a future (possibly cross-block) frame — the deviate
    // step op's jittered onsets. Same convention as scheduleGateOff:
    // gate/pitch history records at SCHEDULE time so trigger-coupled
    // streams (envelopes, per-hit stochastics, keytrack) anchor to the
    // jittered frame.
    schedule(SignalType::Pitch, PacketKind::Set, pitchSignal,
             absFrame, voice, 0, voiceIdx, voiceIdx,
             static_cast<uint64_t>(voiceIdx) + 1u);
    schedule(SignalType::Velocity, PacketKind::Set, vel,
             absFrame, voice, 0, voiceIdx, voiceIdx,
             static_cast<uint64_t>(voiceIdx) + 1u);
    if (externalStepClockReaim())
        return;
    schedule(SignalType::Gate, PacketKind::Set, 1.0f,
             absFrame, voice, 0, voiceIdx, voiceIdx,
             static_cast<uint64_t>(voiceIdx) + 1u);
    ++machine.gateOnCount_;
    machine.lastGateOn_ = std::max(machine.lastGateOn_, absFrame);
    machine.appendBlockFrame(machine.blockGateOns_, absFrame);
    if (voiceIdx == 0) {
        machine.appendPitchSet({ absFrame, pitchSignal,
                                 machine.anyPitch_ ? machine.lastPitch_ : pitchSignal });
        machine.prevPitch_ = machine.anyPitch_ ? machine.lastPitch_ : pitchSignal;
        machine.lastPitch_ = pitchSignal;
        machine.lastPitchFrame_ = absFrame;
        machine.anyPitch_ = true;
    }
    scheduleGateOff(
        std::max(absFrame, absFrame + gateFrames - 1.0),
        voice, voiceIdx, 0, voiceIdx,
        static_cast<uint64_t>(voiceIdx) + 1u);
}

inline double ExecCtx::framesPerBeat() const  { return machine.framesPerBeat_; }
inline double ExecCtx::blockEndFrame() const  { return machine.blockEndFrame_; }
inline double ExecCtx::loopFrames() const
{
    return machine.loopLen_ * machine.framesPerBeat_;
}

inline float ExecCtx::f32Of(uint32_t instrIdx, int operandIdx) const
{
    float v;
    std::memcpy(&v, codeAt(machine.instrs_[instrIdx].operandOff[operandIdx]), 4);
    return v;
}

inline uint8_t ExecCtx::arrayLenU8Of(uint32_t instrIdx, int operandIdx) const
{
    return *codeAt(machine.instrs_[instrIdx].operandOff[operandIdx]);
}

inline uint16_t ExecCtx::arrayU16AtOf(uint32_t instrIdx, int operandIdx, int k) const
{
    uint16_t v;
    std::memcpy(&v, codeAt(machine.instrs_[instrIdx].operandOff[operandIdx]) + 1 + 2 * k, 2);
    return v;
}

inline uint16_t ExecCtx::arrayLenU16Of(uint32_t instrIdx, int operandIdx) const
{
    uint16_t v;
    std::memcpy(&v, codeAt(machine.instrs_[instrIdx].operandOff[operandIdx]), 2);
    return v;
}

inline float ExecCtx::arrayF32AtOf(uint32_t instrIdx, int operandIdx, int k) const
{
    float v;
    std::memcpy(&v, codeAt(machine.instrs_[instrIdx].operandOff[operandIdx]) + 2 + 4 * k, 4);
    return v;
}

inline ScopeFrame& ExecCtx::scope()           { return machine.scopeStack_[(size_t) scopeDepth]; }
inline ScopeFrame& ExecCtx::scopeAt(int d)    { return machine.scopeStack_[(size_t) d]; }

inline void ExecCtx::pushScope()
{
    ++scopeDepth;
    machine.scopeStack_[(size_t) scopeDepth] = ScopeFrame {};
}

inline void ExecCtx::popScope()
{
    if (scopeDepth > 0)
        --scopeDepth;
}

inline const ScopeMeta* ExecCtx::scopeMetaOf(const ScopeFrame& f) const
{
    return f.metaIdx >= 0 ? &machine.scopeMetas_[(size_t) f.metaIdx] : nullptr;
}

inline uint32_t* ExecCtx::permRow()
{
    return machine.permRows_[(size_t) scopeDepth].data();
}

inline float* ExecCtx::streamFor(uint32_t lane, SignalType type, PacketKind kind,
                                 bool midi, uint32_t channelIndex,
                                 uint64_t stableChannelId)
{
    const int32_t rowIdx = machine.instrStreamIdx_[instrIndex];
    if (rowIdx < 0)
        return nullptr;
    StreamRow& row = machine.streamRows_[(size_t) rowIdx];
    row.active = true;
    row.midi   = midi;
    row.lane   = lane;
    row.channelIndex = channelIndex;
    row.stableChannelId = stableChannelId;
    row.type   = type;
    row.kind   = kind;
    return row.data.data();
}

inline void ExecCtx::streamStamp(uint32_t lane, SignalType type, PacketKind kind)
{
    const int32_t rowIdx = machine.instrStreamIdx_[instrIndex];
    if (rowIdx < 0)
        return;
    StreamRow& row = machine.streamRows_[(size_t) rowIdx];
    row.lane = lane;
    row.type = type;
    row.kind = kind;
}

inline const StreamRow* ExecCtx::previousStreamRow(uint32_t lane, SignalType type) const
{
    const StreamRow* previous = nullptr;
    for (const auto& row : machine.streamRows_) {
        if (row.instrIdx >= instrIndex)
            continue;
        if (row.lane != lane || row.type != type)
            continue;
        if (previous == nullptr || row.instrIdx > previous->instrIdx)
            previous = &row;
    }
    if (previous == nullptr || ! previous->active)
        return nullptr;
    return previous;
}

inline StreamRow* ExecCtx::takePreviousStreamRow(uint32_t lane, SignalType type)
{
    StreamRow* previous = nullptr;
    for (auto& row : machine.streamRows_) {
        if (row.instrIdx >= instrIndex)
            continue;
        if (row.lane != lane || row.type != type)
            continue;
        if (previous == nullptr || row.instrIdx > previous->instrIdx)
            previous = &row;
    }
    if (previous == nullptr || ! previous->active)
        return nullptr;

    previous->active = false;
    return previous;
}

inline const float* ExecCtx::externalInputRow(uint32_t channel, int requiredSamples) const
{
    if (channel >= (uint32_t) std::max(0, machine.externalInputCount_)
        || machine.externalInputRows_ == nullptr
        || machine.externalInputStride_ < requiredSamples)
        return nullptr;
    return machine.externalInputRows_[channel];
}

inline double ExecCtx::sampleRate() const      { return machine.sampleRate_; }

inline float* ExecCtx::laneSlewSlot(uint32_t lane)
{
    return lane < machine.laneSlewLast_.size()
        ? &machine.laneSlewLast_[(size_t) lane] : nullptr;
}

inline void ExecCtx::activateChannelPlan(uint32_t planIndex)
{
    if (planIndex < machine.activeChannelPlans_.size())
        machine.activeChannelPlans_[planIndex] = 1;
}

inline double ExecCtx::blockStartFrame() const { return machine.blockStartFrame_; }

inline double ExecCtx::frameOfBeat(double beat) const
{
    // B-324 — through the stable clock anchor (frameBase_, beatBase_), so the
    // result is block-size independent (see Machine::frameOfBeat).
    return machine.frameBase_
         + (beat - machine.beatBase_) * machine.framesPerBeat_;
}

inline double ExecCtx::beatOfFrame(double frame) const
{
    // B-324 — inverse of frameOfBeat through the stable clock anchor.
    return machine.framesPerBeat_ > 0.0
         ? machine.beatBase_ + (frame - machine.frameBase_) / machine.framesPerBeat_
         : 0.0;
}

inline double ExecCtx::scopeTimescale() const
{
    const double ts = machine.scopeStack_[(size_t) scopeDepth].timescale;
    return ts > 0.0 ? ts : 1.0;
}

inline double ExecCtx::scopeAnchorFrame() const
{
    const ScopeFrame& f = machine.scopeStack_[(size_t) scopeDepth];
    // B-324 — map the scope's base BEAT to a frame through the clock anchor,
    // not iteration·loopFrames-from-frame-0. The frame axis is monotonic; only
    // the per-tempo-span slope (framesPerBeat_) varies, so a live tempo change
    // can't shift the anchor against already-walked frames.
    return frameOfBeat((double) iteration * machine.loopLen_ + f.absBaseBeats);
}

inline double ExecCtx::scopeSpanEndFrame() const
{
    const ScopeFrame& f = machine.scopeStack_[(size_t) scopeDepth];
    if (f.durBeats > 0.0)
        return scopeAnchorFrame() + f.durBeats * machine.framesPerBeat_;
    if (machine.loopLen_ > 0.0)   // B-324 — next loop boundary via the anchor
        return frameOfBeat((double) (iteration + 1) * machine.loopLen_);
    return 1e300;   // no loop, no scope: unbounded
}

inline uint64_t ExecCtx::gateCountAt(double frame) const
{
    const auto& ons = machine.blockGateOns_;
    const size_t n = (size_t) (std::upper_bound(ons.begin(), ons.end(), frame)
                               - ons.begin());
    return machine.gateCountAtBlockStart_ + n;
}

inline double ExecCtx::gateAnchorAt(double frame) const
{
    const auto& ons = machine.blockGateOns_;
    const auto it = std::upper_bound(ons.begin(), ons.end(), frame);
    if (it != ons.begin())
        return *(it - 1);
    return machine.lastGateOnAtStart_ <= frame ? machine.lastGateOnAtStart_ : -1.0;
}

inline double ExecCtx::findGateOffAfter(double onFrame) const
{
    // Earliest off scheduled at/after the gate-on — its gate-off. Offs are
    // monotone with their ons (each fires alongside its on in the same Fire
    // walk), so this-block list first, carried latest-off as the cross-block
    // fallback. No candidate => the gate is still open.
    const auto& offs = machine.blockGateOffs_;
    const auto it = std::lower_bound(offs.begin(), offs.end(), onFrame);
    double best = 1e300;
    if (it != offs.end())
        best = *it;
    if (machine.lastGateOffAtStart_ >= onFrame)
        best = std::min(best, machine.lastGateOffAtStart_);
    return best;
}

inline ExecCtx::PitchAt ExecCtx::pitchAt(double frame) const
{
    const auto& sets = machine.blockPitchSets_;
    const Machine::PitchSet* latest = nullptr;
    for (const auto& ps : sets) {
        if (ps.frame > frame) break;
        latest = &ps;
    }
    if (latest != nullptr)
        return { latest->value, latest->prev, latest->frame, true };
    return machine.pitchAtStart_;
}

inline float ExecCtx::quantizeAtFrame(const ScopeFrame& f, float semis) const
{
    // Nearest-degree snap, circular over the 12-semitone period — same
    // algorithm (incl. the first-found tie rule) as music::quantizeToScale;
    // intervals read from the SCALE instruction's operands.
    // F-071 T-539 — a stochastic scale (SCALE_GEN) draws its intervals onto the
    // frame per iteration; prefer those. Otherwise read the SCALE op's operands.
    const bool gen = f.scaleGenCount > 0;
    int n = 0;
    float root = 0.0f;
    if (gen) {
        n = (int) f.scaleGenCount;
        root = f.scaleGenRoot;
    } else {
        if (f.scaleInstrIdx < 0) return semis;
        const auto idx = (uint32_t) f.scaleInstrIdx;
        const bool rootExpr = f.scaleRootExprActive != 0;
        const int intervalsOp = rootExpr ? 2 : 1;
        n = (int) arrayLenU16Of(idx, intervalsOp);
        if (n == 0) return semis;
        root = rootExpr ? f.scaleRootExprValue : f32Of(idx, 0);
    }
    float chroma = std::fmod(semis, 12.0f);
    if (chroma < 0.0f) chroma += 12.0f;
    float bestDiff = std::numeric_limits<float>::infinity();
    float bestTarget = chroma;
    for (int k = 0; k < n; ++k) {
        const float interval = gen ? f.scaleGenIntervals[k]
                             : arrayF32AtOf((uint32_t) f.scaleInstrIdx,
                                            f.scaleRootExprActive ? 2 : 1, k);
        float target = std::fmod(interval + root, 12.0f);
        if (target < 0.0f) target += 12.0f;
        float diff = std::fabs(chroma - target);
        if (diff > 6.0f) diff = 12.0f - diff;
        if (diff < bestDiff) { bestDiff = diff; bestTarget = target; }
    }
    float shift = bestTarget - chroma;
    if (shift > 6.0f)  shift -= 12.0f;
    if (shift < -6.0f) shift += 12.0f;
    return semis + shift;
}

inline float ExecCtx::resolvePitch(float semis) const
{
    // Innermost invert reflects first, on the raw pitch (B-284 model).
    for (int d = scopeDepth; d >= 0; --d) {
        const ScopeFrame& f = machine.scopeStack_[(size_t) d];
        if (f.invertActive) {
            float pivot = f.invertExprActive ? f.invertExprPivot : f.invertPivot;
            if (f.invertMode == 1) {
                const ScopeMeta* meta = scopeMetaOf(f);
                if (meta != nullptr && meta->hasFirstNote)
                    pivot = meta->firstNoteSemis;
            } else if (f.invertMode == 2) {
                // B-288 — invert:N pivots on the Nth content item's pitch
                // (1-based, wrapping modulo the content count; no-note
                // items fall back to the scope's first note).
                const ScopeMeta* meta = scopeMetaOf(f);
                if (meta != nullptr && ! meta->offsets.empty()) {
                    const int n  = (int) meta->offsets.size();
                    const int ni = (((int) f.invertPivot - 1) % n + n) % n;
                    if (meta->hasNote[(size_t) ni])
                        pivot = meta->noteSemis[(size_t) ni];
                    else if (meta->hasFirstNote)
                        pivot = meta->firstNoteSemis;
                }
            }
            semis = 2.0f * pivot - semis;
            break;
        }
    }
    // Innermost -> outermost: transposes sum; a SCALE frame quantizes the
    // accumulated pitch (inner transposes + its own left-written ones) and
    // re-bases; transposes right of / outside the scale apply after.
    float t = 0.0f;
    for (int d = scopeDepth; d >= 0; --d) {
        const ScopeFrame& f = machine.scopeStack_[(size_t) d];
        const float frameTranspose = f.transposeSemis + f.transposeExprSemis;
        if (f.scaleActive) {
            semis = quantizeAtFrame(f, semis + t + f.scaleBaseTranspose);
            t = frameTranspose - f.scaleBaseTranspose;
        } else {
            t += frameTranspose;
        }
    }
    return semis + t;
}

inline void* ExecCtx::stateOfInstr(uint32_t instrIdx) const
{
    const uint32_t off = machine.instrs_[instrIdx].stateOff;
    return off == UINT32_MAX ? nullptr : machine.stateArena_.data() + off;
}

inline void* ExecCtx::state() const
{
    return instr->stateOff == UINT32_MAX
               ? nullptr
               : machine.stateArena_.data() + instr->stateOff;
}

} // namespace curlop::vm

// The op table itself — eval functions + the single registry. Included at
// the tail so table entries can use the full ExecCtx/Machine surface.
#include "OpsCore.h"
