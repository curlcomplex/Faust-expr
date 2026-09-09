// F-066 — opcode identities for the rebuilt VM ISA.
//
// Fresh numbering — this is a rebuild, not the old SequencerVM ISA. The
// behavioral truth for each op lives in its single OpsCore.h table entry;
// this enum is only the id.
#pragma once

#include <cstdint>

namespace curlop::vm {

enum class Op : uint8_t {
    Halt = 0x00,
    Loop = 0x01,   // loop length in beats (f32)
    Step = 0x02,   // beat offset (f32), duration in beats (f32)
    Note = 0x03,   // note in semitones from middle C (f32), velocity (f32)
    Rest = 0x04,   // step is a rest — no emissions

    // ── Conditionals (guard the next step; multiple AND together) ──
    // All count loops 1-indexed (musicians count from 1). Cycle-modulo
    // forms take a u16 cycle; 0 = absolute loop count.
    CondMod        = 0x10,  // divisor (u16): fires when loop % divisor == 0
    CondLoopEq     = 0x11,  // loop number (u16)
    CondFirst      = 0x12,  // cycle (u16)
    CondEven       = 0x13,  // cycle (u16)
    CondOdd        = 0x14,  // cycle (u16)
    CondPrime      = 0x15,  // cycle (u16)
    CondNotFirst   = 0x16,  // cycle (u16)
    CondFib        = 0x17,  // cycle (u16)
    CondAfter      = 0x18,  // threshold (u16): fires when loop > threshold
    CondLoopSet    = 0x19,  // membership set (u8 count + u16s)
    CondNotLoopSet = 0x1A,  // membership set (u8 count + u16s)
    CondPrevious   = 0x65,  // previous authored step actually fired
    CondNotPrevious = 0x66, // previous authored step did not fire

    // ── Probability ──
    // Rolls are keyed by (seed operand, loop iteration) — never by block
    // position, so output is exactly block-size independent; and never by
    // instruction index, so per-module projections of the same source step
    // share one verdict. Emitters must stamp unique seeds per source
    // instruction (see ExecCtx::roll).
    Prob      = 0x1B,  // threshold (f32), seed (u32): skip next step on fail
    ProbDyn   = 0x1C,  // thresholdExpr, thresholdImm, seed (u32)
    Bernoulli = 0x1D,  // weight (f32), option count (u8), seed (u32):
                       // exactly one of the next N steps fires per loop
    BernoulliDyn = 0x1E, // weightExpr, weightImm, option count, seed:
                         // dynamic weight expression for option selection
    SelectDyn = 0x1F, // selectorExpr, selectorImm, branch step counts:
                      // deterministic runtime branch selector over following steps

    // ── Articulations (step body, between STEP and NOTE) ──
    Ratchet        = 0x20,  // count (u32): subdivide the step into N triggers
    RatchetPitched = 0x21,  // count (u32), pitches (u16 count + f32s, cycled)
    Flam           = 0x22,  // offset beats (f32), grace velocity scale (f32)
    Buzz           = 0x23,  // pressure 0..1 (f32), duration beats (f32)
    Bounce         = 0x24,  // gravity (f32), interval (f32), intervalDomain (u8: 0 derive 0.4×stepDur BPM-rel / 1 beats BPM-rel / 2 seconds absolute physics-time). F-071 T-516.
    Geiger         = 0x25,  // density 0..1 (f32), seed (u32): Poisson retrigger
    GateLen        = 0x26,  // duration beats (f32): gate-length override

    // ── Polyphony + param control (step body) ──
    NoteChord = 0x27,  // notes (u16 count + f32 semitones each), velocity (f32)
    ParamLock = 0x28,  // lane (u16), value (f32), presence (u8: 1=set 2=offset)

    // ── Decoupled gate/pitch (T-469 — gates and pitches independently
    //    authorable; Delivery's event-held note inputs supply "last pitch") ──
    GateOnly = 0x29,   // velocity (f32): gate at the held pitch, no pitch emission.
                       //   Length = the step's gate (GateLen override applies).
    PitchSet = 0x2A,   // semitones from middle C (f32): legato pitch update, no gate edge.
    Deviate  = 0x2B,   // amount (f32, fraction of step), seed (u32) — B-286 plain
                       //   `deviate:N` STEP op: onset humanization. Every hit of the
                       //   step (train sub-hits and chord voices included, each keyed
                       //   independently) lands late by a seeded random fraction in
                       //   [0, N) of the step. Tilde law: ~deviate targets a param;
                       //   plain deviate shapes timing.
    GenOperand = 0x2C, // B-187 — generator-valued articulation arg, arm-time draw.
                       //   target (u8, GenTarget), kind (u8: 0 random a..b,
                       //   1 bernoulli pick a/b with weight w), a (f32), b (f32),
                       //   w (f32), seed (u32). Precedes the articulation it
                       //   parameterizes; one draw per loop iteration (roll keying),
                       //   overriding that op's immediate operand. The immediate
                       //   stays in the bytecode as the disassembly-readable
                       //   midpoint; it is never consumed while the prefix arms.
    LocalOutput = 0x2D, // lane (u16), signal type (u8), value (f32): named local
                        //   output socket emission. Unlike NOTE/GATE_ONLY/PITCH_SET,
                        //   Delivery routes by the authored socket lane, not by
                        //   voice/type ordinal.
    LocalGateOnly = 0x2E, // lane (u16), velocity (f32): named gate socket that
                          //   uses the same gate length/train articulation path
                          //   as GATE_ONLY, but emits onto an authored lane.
    MarkovEvent = 0x2F, // pitches (f32 array), velocities (f32 array), value lane
                        //   (u16, 0xffff none), values (f32 array), has-value
                        //   (u8), seed (u32): stateful trigger-sampled material.

    // ── Scopes + runtime transforms ──
    // Step offsets inside a scope are scope-relative; the scope's step
    // table is derived at install time. REPEAT is a builder unroll, not an
    // opcode (jump mechanics stay dead); SHUFFLE is runtime because it
    // varies per loop iteration.
    ScopeStart = 0x30,  // base offset beats (f32), duration beats (f32)
    ScopeEnd   = 0x31,
    Transpose  = 0x32,  // semitones (f32) — sums across nested scopes
    Groove     = 0x33,  // template (u8), amount % (f32), vel low (f32), vel high (f32)
    Reverse    = 0x34,  // step order remaps onto the scope's positions
    Rotate     = 0x35,  // amount (i16)
    Shuffle    = 0x36,  // seed (u32) — deterministic per (seed, iteration)
    Invert     = 0x37,  // pivot semitones (f32), use-first-note (u8)
    Timescale  = 0x38,  // factor (f32): offsets + durations divide by it
    StepAbs    = 0x39,  // beat offset (f32), duration in MILLISECONDS (f32)
    Scale      = 0x3A,  // root chroma (f32 0-12), intervals (u16 count + f32 semitone
                        //   offsets within the 12-semitone period). B-289 — runtime
                        //   pitch quantizer: resolvePitch snaps at this frame AFTER
                        //   inner-frame transposes sum (SPEC-016 one-mechanism law).
                        //   Registry resolution (name → intervals) is compile-time
                        //   translation; quantization is fire-time semantics.
    Grid       = 0x3B,  // grid size beats (f32), strength 0-1 (f32). B-291 — runtime
                        //   onset quantizer: every hit firing inside the frame snaps
                        //   its FINAL fire-time onset (onset:/groove shifts included)
                        //   toward the grid, lerped by strength.
    Sort       = 0x3C,  // direction (u8: 0 asc, 1 desc). B-288 — runtime order op:
                        //   the frame's content (steps AND child scopes) plays in
                        //   pitch order of each item's first note; no-note items
                        //   keep authored order at the end.
    ScaleGen   = 0x3D,  // F-071 T-539 — stochastic scale: root chroma (f32),
                        //   genKind (u8: 0 random / 1 bernoulli), weight (f32),
                        //   seed (u32), candidate lengths (u16 count + u16 each),
                        //   flat intervals (u16 count + f32 each). evalScaleGen
                        //   draws ONE candidate scale per scope iteration and
                        //   stashes its intervals on the frame; quantizeAtFrame
                        //   uses them. VM stays registry-free — the compiler bakes
                        //   the candidate interval pool (every control op drives an
                        //   enum arg via the 0-1 option range, SPEC-018 §4).

    // ── Control ops: per-sample stream producers (SF-061 lifecycle) ──
    // All carry a lane (u16) the delivery layer binds. Scope-as-clock:
    // anchored at scope entry, streaming across inner steps, dead past
    // scope exit. AD has a retrigger policy operand; ADSR is scope-only
    // (the retrigger question is an SF-061 open decision).
    // F-071 T-495 — Ctrl ops carry a trailing unit-domain operand so BPM-relative
    // rates/times scale with the scope's cumulative timescale (+ tempo) while
    // absolute (ms/s/hz) args stay wall-clock immune (SPEC-018 §1.2). LFO: a
    // single rateDomain u8 (0 = rate is Hz/absolute, 1 = rate is cycles-per-beat/
    // BPM-relative). AD/ADSR: a u8 timeDomainMask (bit set = that time arg is
    // absolute seconds; clear = BPM-relative beats — bit0 attack, bit1 decay,
    // bit3 release). Default 0 keeps direct-byte callers on the legacy path.
    CtrlLfo       = 0x40,  // lane, waveform (u8: 0 sin 1 tri 2 saw 3 sq 4 S&H), rate (f32), min, max, offsetMode (u8), phase01 (f32), rateDomain (u8)
    CtrlAd        = 0x41,  // lane, attack, decay, min, max, retrigger (u8), offsetMode (u8), timeDomainMask (u8)
    CtrlAdsr      = 0x42,  // lane, attack, decay, sustain level, release, min, max, offsetMode (u8), retrigger (u8), timeDomainMask (u8)
    CtrlAuto      = 0x43,  // lane, start, end, curve (u8: 0 lin 1 exp 2 log 3 eqpow)
    // Stochastics (T-484 + B-286): anchor-clocked by default — one roll per
    // scope iteration (B-279 law: notes sample the op, never retrigger it).
    // The trailing perHit operand (u8) re-rolls per gate-on instead; the
    // compiler sets it when the route is written RIGHT of a gate-multiplying
    // articulation (§5 written position is the selector — no per-op clock).
    CtrlRandom    = 0x44,  // lane, min, max, seed (u32), offsetMode (u8), perHit (u8)
    CtrlDeviate   = 0x45,  // lane, amount, seed (u32), perHit (u8) — OFFSET stream, ±amount
    CtrlBernoulli = 0x46,  // lane, weight, valA, valB, seed (u32), offsetMode (u8), perHit (u8)
    CtrlKeytrack  = 0x47,  // lane, min, max — follows the last note (MIDI 24..96 window)
    Accum         = 0x48,  // lane, start, increment, ceiling — per gate-on, resets per loop
    Glide         = 0x49,  // time beats (f32) — pitch stream ramping between notes
    CtrlMidi      = 0x4A,  // lane, channel (u8), cc (u8), minNorm, maxNorm, offsetMode (u8) — host-fed CC stream (B-275: scoped like every op)
    // F-071 T-571a — nested `timescale:~lfo`: a scope-local timescale swept by
    // an LFO riding the script's own beat clock (SPEC-018 §4.6 local half). The
    // static Timescale (0x38) op is left untouched so fixed-tempo bytecode stays
    // bit-identical (B-324 invariant). Operands mirror TimescaleModulator: the
    // unipolar waveform maps [minMul,maxMul] MULTIPLIERS over rateBeats per cycle.
    // The factor is re-derived each walk as a pure function of the step's authored
    // position (block-size-independent, no per-op state) — see
    // .planning/research/nested-timescale-lfo-warp-design.md.
    TimescaleMod  = 0x4B,  // waveform (u8: 0 sin 1 tri 2 saw 3 sq), rateBeats (f32), minMul (f32), maxMul (f32)
    // F-071 T-571b — nested `bpm:N`: a scope-local tempo OVERRIDE (SPEC-018 §4.6
    // — bpm SETS/replaces the subtree's base rate, last-wins; timescales below
    // it then multiply). Implemented as a scope-frame timescale SET: effective
    // factor = bpm / scriptBpm, applied to step offsets/durations at runtime
    // (evalStepCommon) so inner offsets compress — fixes the collide-at-0 bug
    // where the compile-time slot shrank but offsets didn't. Resets tsInherited
    // (outer timescales above the bpm don't apply — the rate rule's "product
    // from the bpm's scope down"). Root @bpm stays the global tempo setter.
    ScopeBpm      = 0x4C,  // bpm (f32): scope-local tempo; frame.timescale = bpm / machine tempo
    CtrlClip      = 0x4D,  // lane, minNorm, maxNorm — stream transform over previous active lane row
    CtrlInvert    = 0x4E,  // lane, pivotNorm — output = 2*pivot - input
    CtrlScale     = 0x4F,  // lane, minNorm, maxNorm — output = min + input*(max-min)
    CtrlOffset    = 0x50,  // lane, amountNorm — output = input + amount
    CtrlGain      = 0x51,  // lane, amount — output = input * amount
    CtrlAbs       = 0x52,  // lane — output = abs(input)
    CtrlSmooth    = 0x53,  // lane, timeSeconds — one-pole smooth over previous active lane row
    CtrlDyn       = 0x54,  // lane, kind, aux0, aux1, seed, flags, immediates[], argLanes[] — render-time dynamic expression args
    GenOperandDyn = 0x55,  // target, kind, seed, exprA, immA, exprB, immB, exprW, immW — fire-time dynamic expression arg
    TimescaleExpr = 0x56,  // expr, imm, mode — fire-time timescale/BPM sampled per covered step
    GridExpr      = 0x57,  // sizeExpr, sizeImm, strengthExpr, strengthImm — fire-time grid params sampled per step
    ScaleGenDyn   = 0x58,  // root, kind, weightExpr, weightImm, seed, count, pool — stochastic scale with dynamic weight
    TransposeExpr = 0x59,  // expr, imm, factor — fire-time transpose/octave sampled per covered step
    RotateExpr    = 0x5A,  // expr, imm — scope-armed dynamic content rotation amount
    InvertExpr    = 0x5B,  // expr, imm — fire-time semitone pivot sampled per covered step
    GrooveExpr    = 0x5C,  // template, amountExpr/imm, velLoExpr/imm, velHiExpr/imm
    ScaleExpr     = 0x5D,  // rootExpr, rootImm, intervals — fire-time scale root sampled per covered step
    RepeatExpr      = 0x5E,  // expr, imm — fire-time plain-step repeat count sampled per step
    ScopeRepeatExpr = 0x5F,  // expr, imm — fire-time structural repeat count sampled per scope pass
    CtrlQuantizeScale = 0x60, // lane, root chroma, intervals — stream pitch-grid quantizer
    CtrlInput     = 0x61,  // lane, input channel — previous-block script input row source
    NoteExpr      = 0x62,  // expr, imm, midpoint, velocity — fire-time single-voice note pitch
    ScaleGenRootDyn = 0x63, // rootExpr, rootImm, kind, weightExpr, weightImm, seed, count, pool
    CondExpr      = 0x64,  // expr, imm — fire-time truthy guard for the next step
    ChannelApply  = 0x6A,  // plan index (u32) — apply immutable ordered channel gather
    CtrlQuantize  = 0x67,  // lane, stepNorm — generic normalized stream quantizer
    MarkovGate    = 0x68,  // gates (f32 array), velocities (f32 array), seed (u32):
                           // stateful trigger-sampled gate-only material.
    ScopeFitExpr  = 0x69,  // expr, imm — runtime material-scope fit count mask
    ParamLockTyped = 0x6B, // lane, authored value, basis, step beats, presence
};

constexpr uint8_t opcode(Op op) noexcept { return (uint8_t) op; }

// B-187 — GEN_OPERAND override targets: which articulation operand the
// arm-time draw replaces. Mask bit = (1 << target) in ExecCtx.
enum class GenTarget : uint8_t {
    RatchetCount  = 0,
    BuzzPressure  = 1,
    BounceGravity = 2,
    GeigerDensity = 3,
    FlamOffset    = 4,
    FlamGain      = 5,
    // B-186 — generator-driven arp speed (beats per note): RATCHET_PITCHED
    // derives its count from the drawn speed at arm time
    // (count = stepDur / speed, clamped to the 20kHz retrigger ceiling).
    ArpSpeedBeats = 6,
    // F-071 T-531 — vel and len op args as per-loop GEN_OPERAND draws
    // (NoteVel in [0,1]; GateLen already resolved to beats at emit time).
    NoteVel       = 7,
    GateLen       = 8,
    DeviateAmount = 9,
};
constexpr uint8_t kGenTargetCount = 10;

// The GEN_OPERAND masks (ExecCtx::genOperandMask / genOperandRampMask) are
// uint16_t — one bit per target. NOT an arbitrary cap: it is exactly the mask's
// bit width. Adding a target past 16 must widen the mask type, not silently
// overflow — this assert makes that loud (Neo's no-arbitrary-caps law).
static_assert(kGenTargetCount <= 16,
              "GenTarget count exceeds the uint16_t GEN_OPERAND mask width — "
              "widen ExecCtx::genOperandMask/genOperandRampMask to uint32_t");

enum class CtrlDynKind : uint8_t {
    Lfo       = 0,
    Ad        = 1,
    Adsr      = 2,
    Auto      = 3,
    Random    = 4,
    Deviate   = 5,
    Bernoulli = 6,
    Keytrack  = 7,
    Accum     = 8,
    Midi      = 9,
    Clip      = 20,
    Invert    = 21,
    Scale     = 22,
    Offset    = 23,
    Gain      = 24,
    Smooth    = 25,
    Quantize  = 26,
    Preset    = 27,
    MarkovValue = 28,
};

enum class FireExprTag : uint8_t {
    Lit       = 0,
    Lfo       = 1,
    Auto      = 2,
    Random    = 3,
    Bernoulli = 4,
    Clip      = 20,
    Invert    = 21,
    Scale     = 22,
    Offset    = 23,
    Gain      = 24,
    Abs       = 25,
    Divide    = 26,
    Input     = 27,
};

} // namespace curlop::vm
