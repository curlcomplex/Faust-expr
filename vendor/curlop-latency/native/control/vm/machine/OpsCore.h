// F-066 — THE table (adr-vm "How it's built to grow").
//
// Each op is one entry: opcode, name, operand schema, state size, advance
// policy, lifecycle policy, evaluate function. The kernel, the builder,
// install-time validation, and byte-skipping all read these entries — the
// instruction format has a single source of truth and cannot drift.
//
// Adding an op: write its evaluate function (and state struct if it carries
// state), add ONE entry here, give it an id in OpIds.h. Nothing else.
//
// ── Step-body order convention (the compiler/builder emits in this order) ──
//   STEP
//   gate-length + train articulations   (GateLen, Ratchet[Pitched], Buzz,
//                                        Bounce, Geiger — NOTE arms trains)
//   NOTE / NOTE_CHORD / REST
//   post-note articulations             (Flam — reads the note's pitch/vel)
//   param locks
//
// Hit trains stream from the articulation op's own state-arena slot: the
// step fires once and arms the train; every subsequent walk of the
// articulation instruction emits the sub-hits due in that segment. No
// materialised hit lists, no caps — ratchet sustains at any tempo (pitch
// through repetition is the design). Divergence from the old VM: ornaments
// do not compose sequentially (buzz-of-flam etc.); one train per step, and
// flam composes with plain notes only.
//
// Do not include this header directly — include Machine.h (it provides the
// ExecCtx/Machine surface the evaluate functions are written against and
// pulls this in at its tail).
#pragma once

#include <cmath>

#include "OpIds.h"
#include "OpTable.h"
#include "modules/contract/ParameterValue.h"

namespace curlop::vm {

// Unipolar [0,1] LFO sample for a cycle fraction (mirrors VMRunner's root
// @bpm:~lfo / @timescale:~lfo waveform switch — kept identical so nested
// (T-571a) and root timescale modulation read the same shapes).
inline double lfoUnipolar(uint8_t waveform, double cyc) noexcept
{
    cyc -= std::floor(cyc);                       // wrap into [0,1)
    switch (waveform) {
        case 1:  return 1.0 - std::fabs(2.0 * cyc - 1.0);             // tri
        case 2:  return cyc;                                          // saw
        case 3:  return cyc < 0.5 ? 0.0 : 1.0;                        // square
        default: return 0.5 + 0.5 * std::sin(6.283185307179586 * cyc); // sine
    }
}

// ── Shared step/note helpers ──────────────────────────────────────

// Read the current step body's note voicings through the note instruction.
inline int noteVoiceCount(const ExecCtx& c)
{
    if (c.noteInstrIdx == UINT32_MAX) return 0;
    return c.noteIsChord ? (int) c.arrayLenU16Of(c.noteInstrIdx, 0) : 1;
}

inline float noteSemisOf(const ExecCtx& c, int voice)
{
    if (c.noteIsChord)
        return c.arrayF32AtOf(c.noteInstrIdx, 0, voice);
    return c.noteIsExpr ? c.noteExprSemis
                        : c.f32Of(c.noteInstrIdx, 0);
}

inline float noteVelOf(const ExecCtx& c)
{
    return c.noteIsExpr ? c.f32Of(c.noteInstrIdx, 3)
                        : c.f32Of(c.noteInstrIdx, 1);
}

// One audible hit: pitch + velocity + gate-on now, gate-off scheduled.
// The gate-off lands ONE FRAME EARLY (absFrame + gateFrames - 1): a
// per-sample gate buffer can only show a retrigger edge if the off
// occupies at least one sample before the next on — full-duration legato
// steps (`[#kick.c3 / #kick.d3]`) put the off exactly on the next on's
// frame otherwise, and the envelope never re-arms. The one-sample-early
// release is the same structural edge the old GateBus retriggerEdge
// carved. (F-066 Phase 5 CP3.)
inline float hash01(uint64_t a, uint64_t b);   // defined with the stochastics below
inline float evalFireExprOf(ExecCtx& c, uint32_t instrIdx, int codeOp,
                            int immOp, uint32_t seed);

// B-286 — plain `deviate` step op: each hit lands late by a seeded random
// fraction in [0, amount) of the step. Keyed on the hit's UNJITTERED frame
// (+ a per-voice offset, so chord voices smear independently) — unique per
// hit, deterministic, block-size independent.
inline double deviatedHitFrame(double absFrame, uint32_t voice, float amount,
                               uint32_t seed, double stepFrames)
{
    const uint64_t key = (uint64_t) llround(absFrame)
                       + 7919u * ((uint64_t) voice + 1u);
    return absFrame + (double) hash01(seed, key) * (double) amount * stepFrames;
}

inline void emitHitDeviated(ExecCtx& c, float semis, float vel, double absFrame,
                            double gateFrames, uint32_t chordIdx,
                            float devAmount, uint32_t devSeed, double stepFrames)
{
    // B-290 — every hit is a fresh note INSTANCE (SPEC-014 §6 unbounded
    // logical tag): the allocator downstream rotates physical slots so
    // release tails ring and steal policies engage only at saturation.
    // The chord POSITION rides as metadata; the deviate jitter keys on it
    // (not the instance id) so jitter stays block-size independent.
    const uint32_t vid = c.machine.nextVoiceInstance();
    if (devAmount > 0.0f) {
        const double f = deviatedHitFrame(absFrame, chordIdx, devAmount,
                                          devSeed, stepFrames);
        c.scheduleHit(noteToSignal(semis), vel, f, gateFrames, vid, chordIdx);
        return;
    }
    c.emit(SignalType::Pitch, PacketKind::Set, noteToSignal(semis),
           absFrame, vid, 0, chordIdx, chordIdx,
           static_cast<uint64_t>(chordIdx) + 1u);
    c.emit(SignalType::Velocity, PacketKind::Set, vel,
           absFrame, vid, 0, chordIdx, chordIdx,
           static_cast<uint64_t>(chordIdx) + 1u);
    c.emit(SignalType::Gate, PacketKind::Set, 1.0f,
           absFrame, vid, 0, chordIdx, chordIdx,
           static_cast<uint64_t>(chordIdx) + 1u);
    c.scheduleGateOff(
        std::max(absFrame, absFrame + gateFrames - 1.0),
        vid, chordIdx, 0, chordIdx,
        static_cast<uint64_t>(chordIdx) + 1u);
}

inline void emitHit(ExecCtx& c, float semis, float vel, double absFrame,
                    double gateFrames, uint32_t chordIdx)
{
    emitHitDeviated(c, semis, vel, absFrame, gateFrames, chordIdx,
                    c.deviateAmount, c.deviateSeed,
                    c.stepDurBeats * c.framesPerBeat());
}

// ── Hit-train state (one struct serves all four train kinds) ──────

struct TrainState {
    uint8_t  active = 0;
    uint8_t  kind   = 0;          // mirrors ExecCtx::trainKind
    uint32_t noteInstrIdx = 0;    // pitch/velocity source
    uint8_t  isChord = 0;
    uint32_t articInstrIdx = 0;   // RATCHET_PITCHED pitch-array source
    uint8_t  usePitchArray = 0;
    double   originFrame = 0.0;
    double   durFrames   = 0.0;   // train span
    double   gateBeats   = 0.0;   // per-sub-hit gate in beats (× live fpb)
    float    vel = 0.0f;
    // ratchet / buzz closed-form cursor. F-071 Phase 2 (T-512) — spacing is
    // stored in BEATS and multiplied by the LIVE framesPerBeat each block, so a
    // tempo change re-derives the sub-hit interval mid-train (SPEC-018 §4.1),
    // matching bounce/geiger/the ramp cursor.
    uint64_t count = 0;
    double   subDurFrames = 0.0;  // bounce-reverse span overload (beats); kept for that path
    double   subDurBeats = 0.0;   // indexed train: per-sub-hit interval in beats
    uint64_t nextIdx = 0;
    int64_t  lastSample = -1;     // same-sample retriggers collapse
    // bounce / geiger sequential cursor (beats, ported formulas)
    double   tBeats = 0.0;
    double   intervalBeats = 0.0;
    double   durBeats = 0.0;
    int      idx = 0;
    uint8_t  pendingHit = 0;      // geiger: drawn but not yet emitted
    float    pendingVel = 0.0f;
    double   pendingGateBeats = 0.0;
    uint32_t rng = 1;
    float    lambda = 0.0f;       // geiger hits per beat
    float    gravity = 0.0f;      // bounce decay driver
    uint8_t  finalHit = 0;        // bounce: interval collapsed — stop after this hit
    // F-071 T-516 — bounce interval unit. 0 = BPM-relative (intervalBeats is
    // beats, spaced via framesPerBeat → scales w/ timescale + tempo); 1 =
    // absolute (intervalBeats holds SECONDS, spaced via sampleRate → physics-
    // time, immune to timescale + tempo). The bounce physics is unit-agnostic:
    // it spaces hits via a per-train "frames-per-unit" (fpb or sampleRate).
    uint8_t  intervalAbsolute = 0;
    // B-289 — pitch is captured RESOLVED at arm time (the scope stack is
    // walk-local; the train outlives the walk). The resolved values live in
    // per-instruction arena buffers sized at install: the NOTE/NOTE_CHORD
    // instr's state holds one float per voice; RATCHET_PITCHED's resolved
    // array sits after this TrainState in its own slot. An affine capture
    // (the old pitchM/pitchB) cannot represent SCALE quantization.
    // B-286 deviate captured at arm time too — by the time a later walk
    // advances this train, the ExecCtx step context belongs to a different
    // step. durFrames doubles as the jitter scale (the train's step span).
    float    deviateAmount = 0.0f;
    uint32_t deviateSeed   = 0;
    // F-071 Phase 1 (T-511) — variable-interval ramp cursor (ratchet/arp, kind 1).
    // When the speed arg is a ramp gen (~auto), the inter-hit interval is
    // re-evaluated per hit (interpolated start→end across the step span) and the
    // hit count is a runtime quantity, not a precomputed constant. tBeats is the
    // sequential accumulator (shared with bounce/geiger).
    uint8_t  rampMode = 0;            // 1 = variable-interval ramp
    double   rampStartBeats = 0.0;    // inter-hit interval at progress 0
    double   rampEndBeats   = 0.0;    // inter-hit interval at progress 1
    uint32_t gateLane = 0;             // default gate or namedOutputLane(...)
    uint32_t channelIndex = 0;
    uint8_t  emitVelocity = 1;         // named gate sockets emit gate only
};

inline void emitTrainHit(ExecCtx& c, TrainState& st, double absFrame,
                         float velScale, double gateFrames, float pitchOverride,
                         bool usePitchOverride)
{
    if (st.noteInstrIdx == UINT32_MAX) {
        const uint32_t vid = c.machine.lastVoiceInstance();
        if (st.emitVelocity)
            c.emit(
                SignalType::Velocity, PacketKind::Set, velScale,
                absFrame, vid, 0, 0, st.channelIndex,
                static_cast<uint64_t>(st.channelIndex) + 1u);
        c.emit(SignalType::Gate, PacketKind::Set, 1.0f, absFrame, vid,
               st.gateLane, 0, st.channelIndex,
               static_cast<uint64_t>(st.channelIndex) + 1u);
        c.scheduleGateOff(std::max(absFrame, absFrame + gateFrames - 1.0),
                          vid, 0, st.gateLane, st.channelIndex,
                          static_cast<uint64_t>(st.channelIndex) + 1u);
        return;
    }

    // Pitches were resolved through the live scope stack at arm time
    // (B-289) — the note instr's state buffer holds one float per voice;
    // a pitched-ratchet override arrives already resolved.
    const int voices = st.isChord ? (int) c.arrayLenU16Of(st.noteInstrIdx, 0) : 1;
    const float* resolved = (const float*) c.stateOfInstr(st.noteInstrIdx);
    for (int v = 0; v < voices; ++v) {
        const float semis = usePitchOverride ? pitchOverride
                          : (resolved != nullptr ? resolved[v] : 0.0f);
        // chord index — emitHitDeviated allocates the instance id (B-290)
        emitHitDeviated(c, semis, velScale, absFrame,
                        gateFrames, (uint32_t) v,
                        st.deviateAmount, st.deviateSeed, st.durFrames);
    }
}

// Ratchet / buzz: closed-form index cursor. Emits every sub-hit whose frame
// lands in [segStart, segEnd); same-llround-sample retriggers collapse (a
// retrigger faster than the sample clock is one retrigger).
inline void advanceIndexedTrain(ExecCtx& c, TrainState& st, bool isBuzz)
{
    // F-071 Phase 2 (T-512) — spacing in beats × LIVE framesPerBeat: a tempo
    // change mid-train re-derives the sub-hit interval (was frozen in frames).
    const double fpb = c.framesPerBeat();
    while (st.active && st.nextIdx < st.count) {
        const double f = st.originFrame
                       + (double) st.nextIdx * st.subDurBeats * fpb;
        if (f >= c.segEnd)
            return;
        const int64_t samp = (int64_t) std::llround(f);
        if (f >= c.segStart && samp != st.lastSample) {
            float vel = st.vel;
            if (isBuzz) {
                const float t = (float) st.nextIdx / (float) st.count;
                vel = st.vel * (1.0f - t * t * 0.8f);
                if (vel < 0.02f) vel = 0.02f;
            }
            float pitch = 0.0f;
            bool override_ = false;
            if (st.usePitchArray) {
                const uint16_t n = c.arrayLenU16Of(st.articInstrIdx, 1);
                if (n > 0) {
                    // Resolved at arm time (B-289) — sits after the
                    // TrainState in the articulation instr's state slot.
                    const float* rp = (const float*)
                        ((const uint8_t*) c.stateOfInstr(st.articInstrIdx)
                         + sizeof(TrainState));
                    pitch = rp[st.nextIdx % n];
                    override_ = true;
                }
            }
            emitTrainHit(c, st, f, vel, st.gateBeats * fpb, pitch, override_);
            st.lastSample = samp;
        }
        ++st.nextIdx;
    }
    if (st.nextIdx >= st.count)
        st.active = 0;
}

// Bounce: ported physics — interval starts at 0.4 × dur and decays by
// 1/(1+|gravity|) per bounce; velocity decays 15% per bounce (floor 0.05).
// Old-VM order preserved: a hit lands, THEN the cursor advances; the train
// ends when the next hop would overrun the step, or one hit after the
// interval collapses below a millisecond of beats.
// T-493 — negative gravity: the same hit sequence, time-reversed. Quiet
// taps lead, the main (full-velocity) hit lands last, at the late edge of
// the forward span. Hit k's forward time is closed-form (geometric gaps:
// t_k = H·(1−d^k)/(1−d), H = 0.4·dur), so the reverse cursor needs no
// stored array — count + span were precomputed at arm time.
inline void advanceBounceReverseTrain(ExecCtx& c, TrainState& st)
{
    const double fpb  = c.framesPerBeat();
    // F-071 T-516 — unit-agnostic (see advanceBounceTrain). H = the armed
    // initial interval (0.4×stepDur for the default, or authored); fpu/spanU
    // pick beats×fpb or seconds×sr per the interval's unit domain.
    const double fpu   = st.intervalAbsolute ? c.sampleRate() : fpb;
    const double spanStep = st.intervalAbsolute ? st.durBeats * fpb / c.sampleRate()
                                                : st.durBeats;
    const double d    = 1.0 / (1.0 + std::fabs((double) st.gravity));
    const double H    = st.intervalBeats;
    const double span = st.subDurFrames;   // units — arm-time precompute
    const auto tFwd = [&] (int k) {
        return (d >= 1.0) ? H * k : H * (1.0 - std::pow(d, k)) / (1.0 - d);
    };
    while (st.active && st.idx < (int) st.count) {
        const int j = st.idx;
        const int k = (int) st.count - 1 - j;
        const double tj = span - tFwd(k);
        const double f  = st.originFrame + tj * fpu;
        if (! std::isfinite(f)) {          // B-307 — RT guard
            st.active = 0;
            return;
        }
        if (f >= c.segEnd)
            return;
        if (f >= c.segStart) {
            float vel = st.vel;
            if (k > 0) {
                vel = st.vel * (1.0f - 0.15f * (float) k);
                if (vel < 0.05f) vel = 0.05f;
            }
            double gateUnits;
            if (j + 1 < (int) st.count)
                gateUnits = (span - tFwd(k - 1)) - tj;
            else
                gateUnits = std::max(spanStep - tj, 0.0);
            emitTrainHit(c, st, f, vel, gateUnits * fpu, 0.0f, false);
        }
        ++st.idx;
    }
    if (st.idx >= (int) st.count)
        st.active = 0;
}

inline void advanceBounceTrain(ExecCtx& c, TrainState& st)
{
    if (st.gravity < 0.0f) {
        advanceBounceReverseTrain(c, st);
        return;
    }
    const double fpb   = c.framesPerBeat();
    // F-071 T-516 — unit-agnostic spacing. BPM-relative: unit = beats, frames-
    // per-unit = fpb (tracks tempo; durBeats already timescaled). Absolute:
    // unit = seconds, frames-per-unit = sampleRate (immune to tempo + timescale);
    // the span (step duration) in seconds = durBeats × fpb / sr.
    const double fpu   = st.intervalAbsolute ? c.sampleRate() : fpb;
    const double spanU = st.intervalAbsolute ? st.durBeats * fpb / c.sampleRate()
                                             : st.durBeats;
    const double decay = 1.0 / (1.0 + std::fabs((double) st.gravity));
    while (st.active) {
        const double f = st.originFrame + st.tBeats * fpu;
        if (! std::isfinite(f)) {           // B-307 — RT guard: a poisoned
            st.active = 0;                  // cursor must terminate, never spin
            return;
        }
        if (f >= c.segEnd)
            return;
        if (f >= c.segStart) {
            float vel = st.vel;
            if (st.idx > 0) {
                vel = st.vel * (1.0f - 0.15f * (float) st.idx);
                if (vel < 0.05f) vel = 0.05f;
            }
            emitTrainHit(c, st, f, vel, st.intervalBeats * fpu, 0.0f, false);
        }
        if (st.finalHit) {
            st.active = 0;
            return;
        }
        const double hop = st.intervalBeats;
        if (! (hop > 0.0) || ! std::isfinite(hop)) {   // B-307 — guaranteed progress
            st.active = 0;
            return;
        }
        if (st.tBeats + hop >= spanU) {
            st.active = 0;
            return;
        }
        st.tBeats += hop;
        ++st.idx;
        st.intervalBeats = hop * decay;
        if (st.intervalBeats < 0.001)
            st.finalHit = 1;
    }
}

// Geiger: ported Poisson process — exponential inter-arrival via the old
// 48271 LCG, velocity 0.8–1.2 × base per hit. Draws happen in a fixed
// sequence regardless of segmentation (pending-hit cursor), so the train
// is deterministic and block-size independent.
inline void advanceGeigerTrain(ExecCtx& c, TrainState& st)
{
    const double fpb = c.framesPerBeat();
    while (st.active) {
        if (! st.pendingHit) {
            st.rng = (uint32_t) (((uint64_t) st.rng * 48271u) % 2147483647u);
            float u = (float) st.rng / 2147483647.0f;
            if (u < 1e-10f) u = 1e-10f;
            const double ivl = (double) (-std::log(u) / st.lambda);
            if (! std::isfinite(ivl) || ivl <= 0.0) {   // B-307 — RT guard
                st.active = 0;
                return;
            }
            st.tBeats += ivl;
            if (! (st.tBeats < st.durBeats)) {          // B-307 — NaN-safe exit
                st.active = 0;
                return;
            }
            st.rng = (uint32_t) (((uint64_t) st.rng * 48271u) % 2147483647u);
            const float gv = 0.8f + 0.4f * ((float) st.rng / 2147483647.0f);
            st.pendingVel = std::min(1.0f, st.vel * gv);
            st.pendingGateBeats = std::min(ivl * 0.9, st.durBeats * 0.5);
            st.pendingHit = 1;
        }
        const double f = st.originFrame + st.tBeats * fpb;
        if (f >= c.segEnd)
            return;
        if (f >= c.segStart)
            emitTrainHit(c, st, f, st.pendingVel,
                         st.pendingGateBeats * fpb, 0.0f, false);
        st.pendingHit = 0;
    }
}

// F-071 Phase 1 (T-511) — variable-interval ramp cursor (ratchet/arp). Like
// bounce, it accumulates tBeats sequentially and resumes across blocks from the
// persisted cursor; unlike the closed-form indexed train, the inter-hit interval
// is RE-EVALUATED per hit — interpolated start→end across the step span — so a
// ~auto speed accelerates (or decelerates) the train in real time. The 20kHz
// repetition ceiling (shared with ratchet) clamps the interval per hit.
inline void advanceRampTrain(ExecCtx& c, TrainState& st)
{
    const double fpb  = c.framesPerBeat();
    const double span = st.durBeats;
    // 20kHz ceiling: interval ≥ sampleRate/20000 frames → in beats:
    const double minIntervalBeats =
        (fpb > 0.0) ? (c.sampleRate() / 20000.0) / fpb : 0.0;
    while (st.active) {
        const double f = st.originFrame + st.tBeats * fpb;
        if (! std::isfinite(f)) { st.active = 0; return; }   // B-307 — RT guard
        if (f >= c.segEnd)
            return;
        double progress = (span > 0.0) ? (st.tBeats / span) : 1.0;
        if (progress < 0.0) progress = 0.0;
        if (progress > 1.0) progress = 1.0;
        double interval =
            st.rampStartBeats + (st.rampEndBeats - st.rampStartBeats) * progress;
        if (interval < minIntervalBeats) interval = minIntervalBeats;
        if (! (interval > 0.0) || ! std::isfinite(interval)) {   // guaranteed progress
            st.active = 0;
            return;
        }
        if (f >= c.segStart) {
            float pitch = 0.0f;
            bool  override_ = false;
            if (st.usePitchArray) {
                const uint16_t n = c.arrayLenU16Of(st.articInstrIdx, 1);
                if (n > 0) {
                    const float* rp = (const float*)
                        ((const uint8_t*) c.stateOfInstr(st.articInstrIdx)
                         + sizeof(TrainState));
                    pitch = rp[st.nextIdx % n];
                    override_ = true;
                }
            }
            emitTrainHit(c, st, f, st.vel, interval * fpb, pitch, override_);
        }
        if (st.tBeats + interval >= span) {   // last hit emitted — train done
            st.active = 0;
            return;
        }
        st.tBeats += interval;
        ++st.nextIdx;
    }
}

inline void advanceTrain(ExecCtx& c, TrainState& st)
{
    if (! st.active) return;
    switch (st.kind) {
        case 1:
            if (st.rampMode) advanceRampTrain(c, st);          // ramp ratchet/arp
            else             advanceIndexedTrain(c, st, false); // closed-form ratchet
            break;
        case 2: advanceIndexedTrain(c, st, true);  break;  // buzz
        case 3: advanceBounceTrain(c, st);         break;
        case 4: advanceGeigerTrain(c, st);         break;
        default: st.active = 0; break;
    }
}

// ── Core evaluate functions ───────────────────────────────────────

inline void evalHalt(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Configure)
        c.halted = true;
}

inline void evalLoop(ExecCtx& c)
{
    // Static program fact: recorded once on the Configure walk.
    if (c.phase == ExecCtx::Phase::Configure)
        c.machine.setLoopLength(c.f32(0));
}

// Groove templates — ported verbatim from the old SequencerVM (swing,
// shuffle, push, quint, sept, elastic, samba, fractal).
inline void grooveDisplace(uint8_t templateId, int stepIdx, float amountNorm,
                           float& timeOffset, float& gainWeight)
{
    timeOffset = 0.0f;
    gainWeight = 1.0f;
    switch (templateId) {
        case 0:  // swing — delay offbeats toward triplet position
            if (stepIdx % 2 == 1) { timeOffset = 0.33f * amountNorm; gainWeight = 0.0f; }
            break;
        case 1:  // shuffle — lighter swing, 16th-note feel
            if (stepIdx % 2 == 1) { timeOffset = 0.167f * amountNorm; gainWeight = 0.0f; }
            break;
        case 2:  // push — offbeats arrive early
            if (stepIdx % 2 == 1) { timeOffset = -0.08f * amountNorm; gainWeight = 0.3f; }
            break;
        case 3: {  // quint — 5-against-4
            static const float qo[4] = { 0.0f, 0.05f, -0.1f, 0.05f };
            static const float qg[4] = { 1.0f, 0.7f, 0.5f, 0.8f };
            timeOffset = qo[stepIdx % 4] * amountNorm;
            gainWeight = qg[stepIdx % 4];
            break;
        }
        case 4: {  // sept — 7-against-4
            static const float so[4] = { 0.0f, -0.07f, 0.07f, -0.03f };
            static const float sg[4] = { 1.0f, 0.6f, 0.4f, 0.7f };
            timeOffset = so[stepIdx % 4] * amountNorm;
            gainWeight = sg[stepIdx % 4];
            break;
        }
        case 5: {  // elastic — sinusoidal displacement
            const float phase = (float) stepIdx * 1.5707963f;
            timeOffset = std::sin(phase) * 0.2f * amountNorm;
            gainWeight = 0.5f + 0.5f * std::cos(phase);
            break;
        }
        case 6: {  // samba — 16-step micro-timing pattern
            static const float so[16] = {
                0.0f, 0.0f, 0.05f, -0.03f, 0.0f, 0.08f, 0.0f, -0.05f,
                0.0f, 0.0f, 0.05f, -0.03f, 0.0f, 0.1f, -0.03f, 0.0f };
            static const float sg[16] = {
                1.0f, 0.5f, 0.7f, 0.4f, 0.9f, 0.6f, 0.8f, 0.3f,
                1.0f, 0.5f, 0.7f, 0.4f, 0.9f, 0.7f, 0.5f, 0.6f };
            timeOffset = so[stepIdx % 16] * amountNorm;
            gainWeight = sg[stepIdx % 16];
            break;
        }
        case 7: {  // fractal — displacement from bit-pattern complexity
            int bits = 0;
            for (int v = stepIdx + 1; v > 0; v >>= 1) bits += (v & 1);
            const float complexity = (float) bits / 4.0f;
            timeOffset = complexity * 0.15f * amountNorm;
            gainWeight = 1.0f - complexity * 0.5f;
            break;
        }
        default:
            break;
    }
}

// Perm-row builders (defined with the order-op evals below) — forward
// declared for evalScopeStart's cascade inheritance.
inline void buildShufflePerm(ExecCtx& c, const ScopeMeta* meta, uint32_t seed);
inline void buildSortPerm(ExecCtx& c, const ScopeMeta* meta, bool desc);
inline bool stepCopyInSegment(const ExecCtx& c, double frame);

// B-288/B-292 — content-position permutation, shared by steps and child
// scopes: the frame's perm (reverse/rotate/shuffle/sort) maps the Nth
// content item onto the position of item perm(N).
inline int permutedContentIndex(ExecCtx& c, const ScopeFrame& frame,
                                const ScopeMeta* meta, int index)
{
    // NOTE: callers guarantee `frame` is the CURRENT depth's frame when
    // this runs (evalStepCommon: its own frame; evalScopeStart: the parent,
    // before pushScope) — c.permRow() is that depth's scratch row.
    if (frame.permKind == 0 || meta == nullptr || meta->offsets.empty())
        return index;
    const int n = (int) meta->offsets.size();
    const int i = ((index % n) + n) % n;
    int j = i;
    switch (frame.permKind) {
        case 1: j = n - 1 - i; break;                                  // reverse
        case 2: j = ((i + frame.rotateAmount) % n + n) % n; break;     // rotate
        case 3: j = (int) c.permRow()[i]; break;                       // shuffle
        case 4: {                                                      // sort (B-288)
            // permRow holds content indices in pitch-rank order; item i
            // plays at the position of its RANK — linear inverse scan,
            // allocation-free, n = the frame's content count.
            for (int r = 0; r < n; ++r)
                if ((int) c.permRow()[r] == i) { j = r; break; }
            break;
        }
    }
    return ((j % n) + n) % n;
}

inline void evalStepCommon(ExecCtx& c, bool msDuration)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;

    // Reset the step-body context unconditionally.
    c.stepFiring        = false;
    c.stepInstrIdx      = c.instrIndex;
    c.stepRepeatCount   = 1;
    c.stepRepeatBaseFrame = 0.0;
    c.stepRepeatStrideFrames = 0.0;
    c.noteInstrIdx      = UINT32_MAX;
    c.noteIsChord       = 0;
    c.noteIsExpr        = 0;
    c.noteExprSemis     = 0.0f;
    c.trainKind         = 0;
    c.trainState        = nullptr;
    c.trainInstrIdx     = 0;
    c.ratchetCount      = 0;
    c.ratchetUsePitches = 0;
    c.flamOffsetBeats   = -1.0f;
    c.gateLenBeats      = -1.0f;
    c.deviateAmount     = 0.0f;
    c.deviateSeed       = 0;
    c.genOperandMask    = 0;
    c.genOperandRampMask = 0;

    // Consume guards. The verdict depends only on the iteration, so every
    // walk of the same iteration agrees (anticipatory ops rely on this).
    bool suppressed = false;
    if (c.condSkip) {
        c.condSkip = false;
        suppressed = true;
    }
    if (c.bernoulliRemaining > 0) {
        const int optionIndex = c.bernoulliTotal - c.bernoulliRemaining;
        if (optionIndex != c.bernoulliPick)
            suppressed = true;
        --c.bernoulliRemaining;
    }
    if (c.selectRemainingSteps > 0) {
        if (c.selectCurrentBranch != c.selectPick)
            suppressed = true;
        --c.selectRemainingSteps;
        --c.selectCurrentBranchRemain;
        if (c.selectCurrentBranchRemain <= 0
            && c.selectRemainingSteps > 0
            && c.selectInstrIdx != UINT32_MAX) {
            ++c.selectCurrentBranch;
            if (c.selectCurrentBranch < c.selectBranchCount)
                c.selectCurrentBranchRemain = (int) c.arrayU16AtOf(c.selectInstrIdx, 2,
                                                                    c.selectCurrentBranch);
        }
        if (c.selectRemainingSteps <= 0) {
            c.selectRemainingSteps = 0;
            c.selectCurrentBranch = 0;
            c.selectCurrentBranchRemain = 0;
            c.selectPick = 0;
            c.selectBranchCount = 0;
            c.selectInstrIdx = UINT32_MAX;
        }
    }

    // B-1739 — product Step supplies the sampled clock edge. Guards still
    // consume normally, then the authored-source projection selects every
    // compiled STEP that belongs to this event. The existing body walk remains
    // authoritative; only beat-position prediction is bypassed.
    if (c.externalStepClockActive()) {
        ScopeFrame& frame = c.scope();
        ++frame.stepCounter;
        const bool matches = c.externalStepClockMatches(c.instrIndex);
        c.stepSuppressed = suppressed || ! matches;
        c.stepAbsFrame = c.externalStepClockFrame();
        c.stepWillFireFrame = c.stepAbsFrame;
        c.stepDurBeats = msDuration
            ? (double) c.f32(1) * c.machine.tempoBpm() / 60000.0
            : (double) c.f32(1);
        c.stepRepeatCount = 1;
        c.stepRepeatBaseFrame = c.stepAbsFrame;
        c.stepRepeatStrideFrames = 0.0;
        c.stepVelScale = 1.0f;
        c.stepFiring = ! c.stepSuppressed;
        if (c.stepFiring)
            c.machine.noteStepFired(c.instrIndex);
        return;
    }

    // ── Scope-aware position + duration ──────────────────────────────
    ScopeFrame& frame = c.scope();
    const uint16_t index = frame.stepCounter++;
    if (frame.dynamicFitActive && index >= frame.dynamicFitCount)
        suppressed = true;

    double relBeats = c.f32(0);                      // scope-relative
    double durationBeats = msDuration
        ? (double) c.f32(1) * c.machine.tempoBpm() / 60000.0
        : (double) c.f32(1);

    // Order permutation: this step plays at the position of step perm(i),
    // read from the install-time offset table.
    const ScopeMeta* meta = c.scopeMetaOf(frame);
    int posIdx = (int) index;     // authored position this step PLAYS at
    if (frame.permKind != 0 && meta != nullptr && ! meta->offsets.empty()) {
        posIdx   = permutedContentIndex(c, frame, meta, (int) index);
        relBeats = meta->offsets[(size_t) posIdx];
    }

    // Timescale (innermost frame): positions + durations compress. F-071 T-571a
    // — a modulated frame (timescale:~lfo) derives the factor PER STEP from the
    // LFO sampled at THIS step's authored position (scope-local beat space). The
    // factor is a pure function of (iteration, authored position), so every walk
    // computes the same fire frame — block-size independent, no per-op anchor.
    double tsFactor = frame.timescale;
    c.stepWillFireFrame =
        c.frameOfBeat((double) c.iteration * c.machine.loopLength()
                      + frame.absBaseBeats + relBeats);
    for (int d = 0; d <= c.scopeDepth; ++d) {
        ScopeFrame& sf = c.scopeAt(d);
        if (sf.transposeExprActive && sf.transposeExprInstrIdx >= 0) {
            const float v = evalFireExprOf(c, (uint32_t) sf.transposeExprInstrIdx,
                                           0, 1, 0x7A115EEDu);
            sf.transposeExprSemis =
                std::isfinite(v) ? v * sf.transposeExprFactor : 0.0f;
        }
    }
    if (frame.tsExprActive && frame.tsExprInstrIdx >= 0) {
        const float authored = evalFireExprOf(c, (uint32_t) frame.tsExprInstrIdx,
                                              0, 1, 0x715CA1E5u);
        if (std::isfinite(authored) && authored > 1.0e-6f) {
            if (frame.tsExprMode == 1) {
                const double tempo = c.machine.tempoBpm();
                if (tempo > 0.0)
                    tsFactor = (double) authored / tempo;
            } else {
                double mul = 1.0 / (double) authored;
                if (mul < 1.0e-4) mul = 1.0e-4;
                tsFactor = frame.tsInherited * mul;
            }
        }
    }
    if (frame.tsModActive) {
        const double phase = ((double) c.iteration * c.machine.loopLength()
                              + frame.absBaseBeats + relBeats)
                           / (frame.tsModRateBeats > 1.0e-6 ? frame.tsModRateBeats : 1.0);
        double mul = frame.tsModMinMul
                   + (frame.tsModMaxMul - frame.tsModMinMul)
                       * lfoUnipolar(frame.tsModWaveform, phase);
        if (mul < 1.0e-4) mul = 1.0e-4;   // never zero/negative time
        tsFactor = frame.tsInherited * mul;
    }
    if (tsFactor != 1.0 && tsFactor > 0.0) {
        relBeats      /= tsFactor;
        durationBeats /= tsFactor;
    }

    // The step's SLOT spacing within its own frame — the swing unit (a
    // 16th swings by a fraction of a 16th, regardless of gate length; the
    // old VM displaced by template beats flat, baking in a step=beat
    // assumption that jumped subdivided notes past their neighbours).
    double slotBeats = durationBeats > 0.0 ? durationBeats : 1.0;
    if (meta != nullptr && ! meta->offsets.empty()) {
        const int n  = (int) meta->offsets.size();
        const int pi = posIdx % n;
        const double cur = (double) meta->offsets[(size_t) pi];
        const double nxt = (pi + 1 < n) ? (double) meta->offsets[(size_t) pi + 1]
                                        : frame.durBeats;
        if (nxt > cur) slotBeats = nxt - cur;
    }
    if (tsFactor != 1.0 && tsFactor > 0.0)
        slotBeats /= tsFactor;

    // Groove + grid (B-291): runtime frame ops over EVERY hit — they
    // compose across the whole frame stack like every op (innermost frame
    // first; within one frame in written order, §5). Groove indexes the
    // NOTE STREAM (each STEP that evals inside the frame counts — arp
    // sub-steps included) and displaces by template offset × the step's
    // own duration, so a 16th swings by a fraction of a 16th. Grid snaps
    // the final fire-time onset (onset:/groove shifts included) toward
    // its grid, lerped by strength. Articulation trains inherit the
    // displaced step origin; their internal subdivision stays mechanical
    // (Elektron prior art: microtiming moves trigs, not retrigs).
    c.stepVelScale = 1.0f;
    for (int d = c.scopeDepth; d >= 0; --d) {
        ScopeFrame& gf = c.scopeAt(d);
        if (! gf.grooveActive && ! gf.gridActive) continue;
        const bool grooveFirst = ! gf.gridActive
            || (gf.grooveActive && gf.grooveOrd < gf.gridOrd);
        for (int pass = 0; pass < 2; ++pass) {
            if ((pass == 0) == grooveFirst) {
                if (! gf.grooveActive) continue;
                float timeOff = 0.0f, gainWeight = 1.0f;
                float amount = gf.grooveAmount;
                float velLo = gf.grooveVelLo;
                float velHi = gf.grooveVelHi;
                if (gf.grooveExprActive && gf.grooveExprInstrIdx >= 0) {
                    amount = evalFireExprOf(c, (uint32_t) gf.grooveExprInstrIdx,
                                            1, 2, 0x67000001u);
                    velLo = evalFireExprOf(c, (uint32_t) gf.grooveExprInstrIdx,
                                           3, 4, 0x67000002u);
                    velHi = evalFireExprOf(c, (uint32_t) gf.grooveExprInstrIdx,
                                           5, 6, 0x67000003u);
                    if (! std::isfinite(amount)) amount = 0.0f;
                    if (! std::isfinite(velLo)) velLo = 0.0f;
                    if (! std::isfinite(velHi)) velHi = 1.0f;
                }
                grooveDisplace(gf.grooveTemplate, (int) gf.grooveCounter++,
                               amount / 100.0f, timeOff, gainWeight);
                relBeats += (double) timeOff * slotBeats;
                c.stepVelScale *= gainWeight * (velHi - velLo) + velLo;
            } else {
                if (! gf.gridActive) continue;
                c.stepWillFireFrame =
                    c.frameOfBeat((double) c.iteration * c.machine.loopLength()
                                  + frame.absBaseBeats + relBeats);
                double gridSize = (double) gf.gridSize;
                double gridStrength = (double) gf.gridStrength;
                if (gf.gridExprActive && gf.gridExprInstrIdx >= 0) {
                    gridSize = (double) evalFireExprOf(c, (uint32_t) gf.gridExprInstrIdx,
                                                       0, 1, 0x6D1D0001u);
                    gridStrength = (double) evalFireExprOf(c, (uint32_t) gf.gridExprInstrIdx,
                                                           2, 3, 0x6D1D0002u);
                    if (! std::isfinite(gridSize))
                        gridSize = 0.0;
                    if (! std::isfinite(gridStrength))
                        gridStrength = 0.0;
                    gridStrength = std::max(0.0, std::min(1.0, gridStrength));
                }
                if (gridSize <= 0.0) continue;
                const double absPos  = frame.absBaseBeats + relBeats;
                const double posInG  = absPos - gf.absBaseBeats;
                const double snapped = std::round(posInG / gridSize) * gridSize;
                relBeats += (snapped - posInG) * gridStrength;
            }
        }
    }

    // Wrap the loop-relative position into [0, loopLen) — a grooved or
    // permuted step displaced past the loop end re-enters at the start
    // (old-VM fmod semantics).
    double posBeats = frame.absBaseBeats + relBeats;
    const double loopLen = c.machine.loopLength();
    if (loopLen > 0.0) {
        posBeats = std::fmod(posBeats, loopLen);
        if (posBeats < 0.0) posBeats += loopLen;
    }

    // Frame-domain fire position: map the step's absolute beat through the
    // block's beat↔frame anchor (B-324 — monotonic frame axis, per-block
    // slope; matches the kernel's segment split).
    const double absFrame =
        c.frameOfBeat((double) c.iteration * c.machine.loopLength() + posBeats);

    c.stepWillFireFrame = absFrame;
    for (int d = 0; d <= c.scopeDepth; ++d) {
        ScopeFrame& sf = c.scopeAt(d);
        if (sf.invertExprActive && sf.invertExprInstrIdx >= 0) {
            const float v = evalFireExprOf(c, (uint32_t) sf.invertExprInstrIdx,
                                           0, 1, 0x1A7E8711u);
            sf.invertExprPivot = std::isfinite(v) ? v : 0.0f;
        }
        if (sf.scaleRootExprActive && sf.scaleRootExprInstrIdx >= 0) {
            const float v = evalFireExprOf(c, (uint32_t) sf.scaleRootExprInstrIdx,
                                           0, 1, 0x5CA1E007u);
            sf.scaleRootExprValue = std::isfinite(v) ? v : 0.0f;
        }
    }
    uint32_t scopeRepeatCount = 1;
    double scopeRepeatStrideBeats = 0.0;
    double repeatedPosBeats = posBeats;
    for (int d = c.scopeDepth; d >= 0; --d) {
        const ScopeFrame& rf = c.scopeAt(d);
        if (rf.dynamicRepeatCount <= 1 || rf.durBeats <= 0.0)
            continue;
        const double ts = rf.timescale > 0.0 ? rf.timescale : 1.0;
        const double span = rf.durBeats / ts;
        if (span <= 0.0)
            continue;
        const double local = repeatedPosBeats - rf.absBaseBeats;
        scopeRepeatCount = rf.dynamicRepeatCount;
        scopeRepeatStrideBeats = span / (double) scopeRepeatCount;
        repeatedPosBeats = rf.absBaseBeats
                         + local / (double) scopeRepeatCount;
        durationBeats /= (double) scopeRepeatCount;
        break;
    }

    const double repeatedAbsFrame =
        c.frameOfBeat((double) c.iteration * c.machine.loopLength()
                      + repeatedPosBeats);

    c.stepSuppressed    = suppressed;
    c.stepAbsFrame      = repeatedAbsFrame;
    c.stepDurBeats      = durationBeats;
    c.stepRepeatCount   = std::max<uint32_t>(1, scopeRepeatCount);
    c.stepRepeatBaseFrame = repeatedAbsFrame;
    c.stepRepeatStrideFrames = scopeRepeatStrideBeats * c.framesPerBeat();
    c.stepFiring = false;
    for (uint32_t i = 0; i < c.stepRepeatCount; ++i) {
        const double f = c.stepRepeatBaseFrame
                       + (double) i * c.stepRepeatStrideFrames;
        if (stepCopyInSegment(c, f)) {
            c.stepFiring = ! suppressed;
            break;
        }
    }
    if (c.stepFiring)
        c.machine.noteStepFired(c.instrIndex);   // SF-059 FOLLOW
}

inline void evalStep(ExecCtx& c)    { evalStepCommon(c, false); }
inline void evalStepAbs(ExecCtx& c) { evalStepCommon(c, true); }

inline bool stepCopyInSegment(const ExecCtx& c, double frame)
{
    return ! c.stepSuppressed && frame >= c.segStart && frame < c.segEnd;
}

template <typename Fn>
inline void forEachStepCopy(ExecCtx& c, Fn&& fn)
{
    if (c.stepSuppressed)
        return;
    const uint32_t n = std::max<uint32_t>(1, c.stepRepeatCount);
    const double savedFrame = c.stepAbsFrame;
    for (uint32_t i = 0; i < n; ++i) {
        const double f = c.stepRepeatBaseFrame
                       + (double) i * c.stepRepeatStrideFrames;
        if (! stepCopyInSegment(c, f))
            continue;
        c.stepAbsFrame = f;
        fn(i);
    }
    c.stepAbsFrame = savedFrame;
}

inline uint32_t dynamicRepeatCountOf(float v)
{
    if (std::isfinite(v) && v > 1.0f)
        return (uint32_t) std::min(20000.0, std::floor((double) v));
    return 1;
}

inline uint32_t dynamicFitCountOf(float v)
{
    if (! std::isfinite(v))
        return 1;
    return (uint32_t) std::max(1.0, std::round((double) v));
}

inline void evalRepeatExpr(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    const float v = evalFireExprOf(c, c.instrIndex, 0, 1, 0x7E9EA700u);
    const double originalDur = c.stepDurBeats;
    c.stepRepeatCount = std::max<uint32_t>(1, dynamicRepeatCountOf(v));
    c.stepRepeatBaseFrame = c.stepAbsFrame;
    if (c.stepRepeatCount > 1 && originalDur > 0.0) {
        c.stepDurBeats = originalDur / (double) c.stepRepeatCount;
        c.stepRepeatStrideFrames = c.stepDurBeats * c.framesPerBeat();
    } else {
        c.stepRepeatStrideFrames = 0.0;
    }
    c.stepFiring = false;
    for (uint32_t i = 0; i < c.stepRepeatCount; ++i) {
        const double f = c.stepRepeatBaseFrame
                       + (double) i * c.stepRepeatStrideFrames;
        if (stepCopyInSegment(c, f)) {
            c.stepFiring = true;
            if (c.stepInstrIdx != UINT32_MAX)
                c.machine.noteStepFired(c.stepInstrIdx);
            break;
        }
    }
}

// ── Scope ops ─────────────────────────────────────────────────────

inline void evalScopeStart(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& parent = c.scope();
    const double parentBase = parent.absBaseBeats;
    // B-292 — a child scope is a CONTENT item of its parent: it ticks the
    // parent's content counter and its base offset remaps through the
    // parent's permutation, exactly as a step's offset does.
    const ScopeMeta* parentMeta = c.scopeMetaOf(parent);
    const int contentIdx = (int) parent.stepCounter++;
    double base = (double) c.f32(0);
    if (parent.permKind != 0 && parentMeta != nullptr
        && ! parentMeta->offsets.empty()) {
        const int j = permutedContentIndex(c, parent, parentMeta, contentIdx);
        base = (double) parentMeta->offsets[(size_t) j];
    }
    const uint8_t  inheritKind   = parent.permKind;
    const int16_t  inheritRotate = parent.rotateAmount;
    const uint8_t  inheritDesc   = parent.sortDesc;
    const uint32_t inheritSeed   = parent.shuffleSeed;
    c.pushScope();
    ScopeFrame& f = c.scope();
    f.metaIdx      = c.machine.metaIndexOf(c.instrIndex);
    // B-301 — the parent's cumulative timescale compresses this scope's
    // base offset AND propagates to everything inside (the child's own
    // timescale op, if any, composes on top in evalTimescale). Timing
    // composes through the frame stack like pitch (SPEC-016 §1).
    const double parentTs = (parent.timescale > 0.0) ? parent.timescale : 1.0;
    f.absBaseBeats = parentBase + base / parentTs;
    f.durBeats     = (double) c.f32(1);
    f.timescale    = parentTs;
    f.tsInherited  = parentTs;
    // s498 — order ops CASCADE: an enclosing reverse/rotate/shuffle/sort
    // orders the content at EVERY level inside its frame (one-law: the op
    // covers everything inside it — [bars] sort(pitch,desc) plays the arp
    // hits inside each bar descending too). The child's own order op, if
    // any, evals right after this and overrides the inherited state.
    if (inheritKind != 0) {
        f.permKind     = inheritKind;
        f.rotateAmount = inheritRotate;
        f.sortDesc     = inheritDesc;
        f.shuffleSeed  = inheritSeed;
        const ScopeMeta* childMeta = c.scopeMetaOf(f);
        if (childMeta != nullptr && ! childMeta->offsets.empty()) {
            if (inheritKind == 3)
                buildShufflePerm(c, childMeta, inheritSeed);
            else if (inheritKind == 4)
                buildSortPerm(c, childMeta, inheritDesc != 0);
        } else if (inheritKind == 3 || inheritKind == 4) {
            f.permKind = 0;   // no content table — nothing to permute
        }
    }
}

inline void evalScopeRepeatExpr(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    const float v = evalFireExprOf(c, c.instrIndex, 0, 1, 0x5C0F7E9Eu);
    c.scope().dynamicRepeatCount = std::max<uint32_t>(1, dynamicRepeatCountOf(v));
}

inline void evalScopeFitExpr(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    const float v = evalFireExprOf(c, c.instrIndex, 0, 1, 0xF17E0001u);
    ScopeFrame& f = c.scope();
    f.dynamicFitActive = 1;
    f.dynamicFitCount = dynamicFitCountOf(v);
}

inline void evalScopeEnd(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Configure)
        c.popScope();
}

inline void evalTranspose(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Configure)
        c.scope().transposeSemis += c.f32(0);
}

inline void evalTransposeExpr(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    f.transposeExprActive   = 1;
    f.transposeExprInstrIdx = (int32_t) c.instrIndex;
    f.transposeExprFactor   = c.f32(2);
    f.transposeExprSemis    = 0.0f;
}

inline void evalGroove(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    f.grooveActive   = 1;
    f.grooveTemplate = c.u8(0);
    f.grooveAmount   = c.f32(1);
    f.grooveVelLo    = c.f32(2);
    f.grooveVelHi    = c.f32(3);
    f.grooveOrd      = f.timingOpSerial++;
}

inline void evalGrooveExpr(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    f.grooveActive       = 1;
    f.grooveExprActive   = 1;
    f.grooveExprInstrIdx = (int32_t) c.instrIndex;
    f.grooveTemplate     = c.u8(0);
    f.grooveAmount       = 0.0f;
    f.grooveVelLo        = 0.0f;
    f.grooveVelHi        = 1.0f;
    f.grooveOrd          = f.timingOpSerial++;
}

// B-291 — runtime grid quantizer: every hit firing inside the frame snaps
// its final fire-time onset toward the grid, lerped by strength.
inline void evalGrid(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    f.gridActive   = 1;
    f.gridSize     = c.f32(0);
    f.gridStrength = c.f32(1);
    f.gridOrd      = f.timingOpSerial++;
}

inline void evalGridExpr(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    f.gridActive       = 1;
    f.gridExprActive   = 1;
    f.gridExprInstrIdx = (int32_t) c.instrIndex;
    f.gridSize         = 0.25f;
    f.gridStrength     = 1.0f;
    f.gridOrd          = f.timingOpSerial++;
}

inline void evalReverse(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Configure)
        c.scope().permKind = 1;
}

inline void evalRotate(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    c.scope().permKind     = 2;
    c.scope().rotateAmount = c.i16(0);
}

inline void evalRotateExpr(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    f.permKind           = 2;
    f.rotateExprActive   = 1;
    f.rotateExprInstrIdx = (int32_t) c.instrIndex;
    // Rotation is a single permutation over the scope's content table. Sampling
    // per content item can produce duplicate/missing positions, so dynamic
    // rotate is scope-armed rather than step-sampled.
    const float v = evalFireExprOf(c, c.instrIndex, 0, 1, 0x807A7E11u);
    f.rotateAmount = std::isfinite(v) ? (int16_t) std::lround(v) : 0;
}

// Fisher–Yates seeded by (seed, iteration) — deterministic per loop,
// varies across loops, block-size independent. Shared by evalShuffle and
// cascade inheritance (s498).
inline void buildShufflePerm(ExecCtx& c, const ScopeMeta* meta, uint32_t seed)
{
    const int n = (int) meta->offsets.size();
    uint32_t* perm = c.permRow();
    for (int i = 0; i < n; ++i)
        perm[i] = (uint32_t) i;
    uint64_t x = (uint64_t) seed ^ (c.iteration * 0x9E3779B97F4A7C15ull);
    for (int i = n - 1; i > 0; --i) {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z =  z ^ (z >> 31);
        const int j = (int) (z % (uint64_t) (i + 1));
        const uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
}

inline void evalShuffle(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    const ScopeMeta* meta = c.scopeMetaOf(f);
    if (meta == nullptr || meta->offsets.empty())
        return;
    f.permKind    = 3;
    f.shuffleSeed = c.u32(0);
    buildShufflePerm(c, meta, f.shuffleSeed);
}

// B-288 — runtime sort: the frame's content plays in pitch order of each
// item's first note (asc/desc); no-note items keep authored order at the
// end. permRow holds the position mapping like shuffle's. Insertion sort:
// in-place, stable, ZERO allocation (std::stable_sort buffers — audio
// thread forbids it).
inline void buildSortPerm(ExecCtx& c, const ScopeMeta* meta, bool desc)
{
    const int n = (int) meta->offsets.size();
    uint32_t* perm = c.permRow();
    auto keyOf = [&](uint32_t idx) {
        return meta->hasNote[idx]
            ? (desc ? -meta->noteSemis[idx] : meta->noteSemis[idx])
            : std::numeric_limits<float>::infinity();
    };
    for (int i = 0; i < n; ++i)
        perm[i] = (uint32_t) i;
    for (int i = 1; i < n; ++i) {
        const uint32_t v = perm[i];
        const float    k = keyOf(v);
        int j = i - 1;
        while (j >= 0 && keyOf(perm[j]) > k) {
            perm[j + 1] = perm[j];
            --j;
        }
        perm[j + 1] = v;
    }
    // perm[r] = the content index with pitch-rank r. Item i plays at the
    // position of its rank — permutedContentIndex (kind 4) reads the
    // inverse with a linear scan.
}

inline void evalSort(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    const ScopeMeta* meta = c.scopeMetaOf(f);
    if (meta == nullptr || meta->offsets.empty())
        return;
    f.permKind = 4;
    f.sortDesc = c.u8(0);
    buildSortPerm(c, meta, f.sortDesc != 0);
}

inline void evalInvert(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    f.invertActive = 1;
    f.invertPivot  = c.f32(0);
    f.invertMode   = c.u8(1);   // 0 semitone, 1 first note, 2 content index (B-288)
}

inline void evalInvertExpr(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    f.invertActive       = 1;
    f.invertExprActive   = 1;
    f.invertExprInstrIdx = (int32_t) c.instrIndex;
    f.invertMode         = 0;
    f.invertPivot        = 0.0f;
    f.invertExprPivot    = 0.0f;
}

inline void evalTimescale(ExecCtx& c)
{
    // B-301/B-302 — compose over the INHERITED cumulative product;
    // assignment (not *=) keeps repeated timescale ops on one frame
    // rightmost-wins per the §5 scope rule.
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    const float factor = c.f32(0);
    if (factor > 0.0f)
        c.scope().timescale = c.scope().tsInherited * (double) factor;
}

// F-071 T-571a — nested `timescale:~lfo`. Records the LFO params on the frame;
// the per-step factor is derived in evalStepCommon (sampled in scope-local beat
// space, a pure function of the step's authored position → block-size
// independent). The baseline frame.timescale stays at the inherited product so
// child-scope base offsets + scopeAnchorFrame see a sane static factor.
inline void evalTimescaleMod(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f      = c.scope();
    f.tsModActive       = 1;
    f.tsModWaveform     = c.u8(0);
    f.tsModRateBeats    = (double) c.f32(1);
    f.tsModMinMul       = (double) c.f32(2);
    f.tsModMaxMul       = (double) c.f32(3);
    f.timescale         = f.tsInherited;   // baseline; per-step modulation in evalStepCommon
}

inline void evalTimescaleExpr(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    f.tsExprActive   = 1;
    f.tsExprMode     = c.u8(2);
    f.tsExprInstrIdx = (int32_t) c.instrIndex;
    f.timescale      = f.tsInherited;
}

// F-071 T-571b — nested `bpm:N`: a scope-local tempo OVERRIDE. A timescale SET
// (not multiply): effective factor = bpm / scriptBpm, so step offsets/durations
// in this scope play at tempo N (evalStepCommon divides relBeats by it, exactly
// like timescale → inner offsets compress, fixing the collide-at-0 bug). bpm
// REPLACES the inherited rate (§4.6 "product of timescales from the bpm's scope
// down"): reset tsInherited too, so a timescale op below composes on the bpm
// base while an outer timescale above does not apply.
inline void evalScopeBpm(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    const double bpm   = (double) c.f32(0);
    const double tempo = c.machine.tempoBpm();
    if (bpm > 0.0 && tempo > 0.0) {
        ScopeFrame& f = c.scope();
        f.timescale   = bpm / tempo;   // SET (replaces the inherited product)
        f.tsInherited = f.timescale;   // children compose on the bpm base; outer ts discarded
    }
}

// B-289 — runtime scale quantizer. The frame records WHICH instruction
// carries the root/intervals (program bytes, stable for the install) and
// how much transpose was written LEFT of the scale — resolvePitch quantizes
// inner content + that pre-scale transpose; later transposes escape (§5).
inline void evalScale(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    f.scaleActive        = 1;
    f.scaleInstrIdx      = (int32_t) c.instrIndex;
    f.scaleBaseTranspose = f.transposeSemis;
    f.scaleGenCount      = 0;   // a plain SCALE clears any prior gen draw
    f.scaleRootExprActive = 0;
}

inline void evalScaleExpr(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    ScopeFrame& f = c.scope();
    f.scaleActive           = 1;
    f.scaleInstrIdx         = (int32_t) c.instrIndex;
    f.scaleBaseTranspose    = f.transposeSemis + f.transposeExprSemis;
    f.scaleGenCount         = 0;
    f.scaleRootExprActive   = 1;
    f.scaleRootExprInstrIdx = (int32_t) c.instrIndex;
    f.scaleRootExprValue    = 0.0f;
}

// F-071 T-539 — stochastic scale. Operands: root (f32), genKind (u8: 0 random /
// 1 bernoulli), weight (f32), seed (u32), candidateCount (u8), packed candidate
// pool (f32 array: [len0, iv0..., len1, iv1...]). Draws ONE candidate scale per
// scope iteration (roll keyed by (seed,iteration) — per-loop, like every other
// gen draw) and stashes its intervals on the frame; quantizeAtFrame reads them.
// Enum-arg 0-1 mechanism: random scatters across the option set, bernoulli picks
// between the first two (SPEC-018 §4 ruling). VM stays registry-free.
inline void evalScaleGenCommon(ExecCtx& c, float root, float weight,
                               int kindOperand, int poolOperand)
{
    ScopeFrame& f = c.scope();
    const uint8_t  kind   = c.u8(kindOperand);
    const uint32_t seed   = c.u32(poolOperand - 2);
    const int      count  = (int) c.u8(poolOperand - 1);
    if (count <= 0) { f.scaleGenCount = 0; f.scaleActive = 0; return; }

    const float u = c.roll(seed);
    int pick;
    if (kind == 1)   // bernoulli — pick between the first two candidates
        pick = (u < weight) ? 0 : (count > 1 ? 1 : 0);
    else {           // random — scatter across the whole option set (0-1 → index)
        pick = (int) (u * (float) count);
        if (pick >= count) pick = count - 1;
        if (pick < 0)      pick = 0;
    }

    // Walk the packed pool ([len, ivs...] per candidate) to the picked one.
    const int total = (int) c.arrayLenU16(poolOperand);
    int at = 0, found = -1, foundLen = 0;
    for (int cand = 0; cand < count && at < total; ++cand) {
        const int len = (int) c.arrayF32At(poolOperand, at);
        ++at;
        if (cand == pick) { found = at; foundLen = len; }
        at += len;
    }
    if (found < 0) { f.scaleGenCount = 0; f.scaleActive = 0; return; }

    const int n = foundLen < 12 ? foundLen : 12;   // frame buffer caps at the period
    for (int k = 0; k < n; ++k)
        f.scaleGenIntervals[k] = c.arrayF32At(poolOperand, found + k);
    f.scaleGenCount      = (uint8_t) n;
    f.scaleGenRoot       = root;
    f.scaleActive        = 1;
    f.scaleInstrIdx      = (int32_t) c.instrIndex;
    f.scaleBaseTranspose = f.transposeSemis;
    f.scaleRootExprActive = 0;
}

inline void evalScaleGen(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    evalScaleGenCommon(c, c.f32(0), c.f32(2), 1, 5);
}

inline void evalScaleGenDyn(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    const uint32_t seed = c.u32(4);
    const float weight = evalFireExprOf(c, c.instrIndex, 2, 3, seed ^ 0x5CA1Eu);
    evalScaleGenCommon(c, c.f32(0), weight, 1, 6);
}

inline void evalScaleGenRootDyn(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Configure)
        return;
    const uint32_t seed = c.u32(5);
    const float root = evalFireExprOf(c, c.instrIndex, 0, 1, seed ^ 0x5CA1E007u);
    const float weight = evalFireExprOf(c, c.instrIndex, 3, 4, seed ^ 0x5CA1Eu);
    evalScaleGenCommon(c, std::isfinite(root) ? root : 0.0f, weight, 2, 7);
}

inline double stepGateFrames(const ExecCtx& c)
{
    const double beats = (c.gateLenBeats >= 0.0f) ? (double) c.gateLenBeats
                                                  : c.stepDurBeats;
    return beats * c.framesPerBeat();
}

// Defined below (the articulation section); forward-declared so NOTE / GATE_ONLY
// can read a NoteVel GEN_OPERAND draw (F-071 T-531).
inline float genOperandOr(const ExecCtx& c, GenTarget t, float immediate);

inline void configureArmedTrainCursor(ExecCtx& c, TrainState& st)
{
    switch (c.trainKind) {
        case 1: {   // ratchet / arp
            const uint16_t arpBit =
                (uint16_t) (1u << (uint8_t) GenTarget::ArpSpeedBeats);
            if (c.genOperandRampMask & arpBit) {
                // F-071 Phase 1 — variable-interval ramp: the inter-hit
                // interval is re-evaluated per hit (start→end across the
                // span); the count is a runtime quantity, not precomputed.
                st.rampMode       = 1;
                st.rampStartBeats = c.genOperandVal[(uint8_t) GenTarget::ArpSpeedBeats];
                st.rampEndBeats   = c.genOperandValB[(uint8_t) GenTarget::ArpSpeedBeats];
                st.tBeats         = 0.0;
                st.nextIdx        = 0;
                st.usePitchArray  = c.ratchetUsePitches;
                if (! (st.rampStartBeats > 0.0) || ! (st.rampEndBeats > 0.0)
                    || ! std::isfinite(st.rampStartBeats)
                    || ! std::isfinite(st.rampEndBeats))
                    st.active = 0;
            } else {
                st.count = c.ratchetCount > 0 ? c.ratchetCount : 1;
                st.subDurBeats = st.durBeats / (double) st.count;
                st.gateBeats   = st.subDurBeats;
                st.usePitchArray = c.ratchetUsePitches;
                if (st.subDurBeats <= 0.0) st.active = 0;
            }
            break;
        }
        case 2: {   // buzz — ported interval formula (float math, as the
                    // old VM computed it — the hit count must match)
            const float baseInterval =
                0.015f + (0.005f - 0.015f) * c.buzzPressure;
            const float buzzLen =
                std::min(c.buzzDurBeats, (float) c.stepDurBeats);
            int n = (int) (buzzLen / baseInterval);
            if (n < 1) n = 1;
            st.count = (uint64_t) n;
            st.subDurBeats = (double) baseInterval;          // T-512 — live fpb
            st.gateBeats   = (double) baseInterval * 0.8;
            break;
        }
        case 3: {   // bounce
            // F-071 T-516 — interval unit domain. 0 = legacy default
            // (0.4×stepDur, BPM-relative beats); 1 = authored beats
            // (BPM-relative); 2 = authored seconds (absolute physics-time).
            st.intervalAbsolute = (c.bounceIntervalDomain == 2) ? 1 : 0;
            // domain 0: derive 0.4×stepDur (stepDurBeats is already
            // timescaled). domain 1: authored beats — BPM-relative, so it
            // scales with the scope timescale exactly like step positions
            // (÷ scopeTimescale). domain 2: authored seconds — absolute,
            // stored as-is (physics-time spacing in the advance).
            st.intervalBeats =
                  (c.bounceIntervalDomain == 0) ? c.stepDurBeats * 0.4
                : (c.bounceIntervalDomain == 1) ? (double) c.bounceInterval / c.scopeTimescale()
                                                : (double) c.bounceInterval;
            st.gravity = c.bounceGravity;
            st.tBeats = 0.0;
            st.idx = 0;
            if (! std::isfinite(st.gravity) || ! std::isfinite(st.durBeats)
                || ! std::isfinite(st.intervalBeats) || st.intervalBeats <= 0.0) {
                st.active = 0;          // B-307 — never arm a poisoned cursor
                break;
            }
            if (st.gravity < 0.0f) {
                // T-493 — negative gravity = the SAME hit sequence
                // time-reversed (§5.4): count + span precomputed by
                // replaying the forward loop's bounds; the reverse
                // cursor recovers each forward hit time closed-form
                // (gaps are geometric: H·d^k).
                const double d = 1.0 / (1.0 + std::fabs((double) st.gravity));
                // span in the interval's unit (beats, or seconds for absolute)
                const double spanU = st.intervalAbsolute
                    ? st.durBeats * c.framesPerBeat() / c.sampleRate()
                    : st.durBeats;
                double t = 0.0, interval = st.intervalBeats;
                int n = 0;
                bool fin = false;
                while (true) {
                    ++n;
                    if (fin) break;
                    const double hop = interval;
                    if (t + hop >= spanU) break;
                    t += hop;
                    interval = hop * d;
                    if (interval < 0.001) fin = true;
                }
                st.count = (uint64_t) n;
                st.subDurFrames = t;   // reverse train: SPAN IN UNITS
            }
            break;
        }
        case 4: {   // geiger — seed mixed per (seed, iteration) only,
                    // same uniqueness contract as ExecCtx::roll
            uint32_t rng = c.geigerSeed
                ^ ((uint32_t) c.iteration * 2654435761u);
            if (rng == 0) rng = 1;
            st.rng = rng;
            st.lambda = c.geigerDensity * 10.0f;
            if (! (st.lambda >= 0.01f)) st.lambda = 0.01f;   // B-307 — NaN-safe clamp
            st.tBeats = 0.0;
            break;
        }
        default:
            st.active = 0;
            break;
    }
}

// Shared NOTE / NOTE_CHORD body.
inline void evalNoteCommon(ExecCtx& c, bool isChord)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    c.noteInstrIdx = c.instrIndex;
    c.noteIsChord  = isChord ? 1 : 0;
    c.noteIsExpr   = 0;
    if (! c.stepFiring)
        return;

    // F-071 T-531 — vel may be a per-loop GEN_OPERAND draw (NoteVel).
    const float vel = genOperandOr(c, GenTarget::NoteVel, noteVelOf(c)) * c.stepVelScale;

    if (c.trainKind != 0 && c.trainState != nullptr) {
        // Arm the articulation's train and stream its first segment.
        TrainState& st = *(TrainState*) c.trainState;
        st = TrainState {};
        st.active        = 1;
        st.kind          = c.trainKind;
        st.noteInstrIdx  = c.instrIndex;
        st.isChord       = c.noteIsChord;
        st.articInstrIdx = c.trainInstrIdx;
        st.originFrame   = c.stepAbsFrame;
        st.durBeats      = c.stepDurBeats;
        st.durFrames     = c.stepDurBeats * c.framesPerBeat();
        st.vel           = vel;
        // B-289 — resolve every pitch the train will emit NOW, while the
        // scope stack is live (transposes, invert, AND scale compose in
        // resolvePitch; an affine capture can't represent the quantize).
        {
            float* resolved = (float*) c.state();
            const int nv = isChord ? noteVoiceCount(c) : 1;
            for (int v = 0; v < nv; ++v)
                resolved[v] = c.resolvePitch(noteSemisOf(c, v));
        }
        if (c.ratchetUsePitches) {
            const uint16_t n = c.arrayLenU16Of(c.trainInstrIdx, 1);
            float* rp = (float*) ((uint8_t*) c.stateOfInstr(c.trainInstrIdx)
                                  + sizeof(TrainState));
            for (uint16_t k = 0; k < n; ++k)
                rp[k] = c.resolvePitch(c.arrayF32AtOf(c.trainInstrIdx, 1, (int) k));
        }
        // Capture the step's deviate too (B-286) — sub-hits jitter even on
        // walks where the step context has moved on.
        st.deviateAmount = c.deviateAmount;
        st.deviateSeed   = c.deviateSeed;

        configureArmedTrainCursor(c, st);
        advanceTrain(c, st);
        return;
    }

    // Plain hit (single note or chord voices).
    const int voices = isChord ? noteVoiceCount(c) : 1;
    forEachStepCopy(c, [&] (uint32_t) {
        const double gateFrames = stepGateFrames(c);
        for (int v = 0; v < voices; ++v)
            emitHit(c, c.resolvePitch(noteSemisOf(c, v)), vel, c.stepAbsFrame,
                    gateFrames, (uint32_t) v);
    });
}

inline void evalNote(ExecCtx& c)      { evalNoteCommon(c, false); }
inline void evalNoteChord(ExecCtx& c) { evalNoteCommon(c, true); }

inline void evalNoteExpr(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    c.noteInstrIdx  = c.instrIndex;
    c.noteIsChord   = 0;
    c.noteIsExpr    = 1;
    c.noteExprSemis = evalFireExprOf(c, c.instrIndex, 0, 1,
                                      0x0E71E000u ^ c.instrIndex);
    if (! c.stepFiring)
        return;

    const float vel = genOperandOr(c, GenTarget::NoteVel, c.f32(3)) * c.stepVelScale;

    if (c.trainKind != 0 && c.trainState != nullptr) {
        TrainState& st = *(TrainState*) c.trainState;
        st = TrainState {};
        st.active        = 1;
        st.kind          = c.trainKind;
        st.noteInstrIdx  = c.instrIndex;
        st.isChord       = 0;
        st.articInstrIdx = c.trainInstrIdx;
        st.originFrame   = c.stepAbsFrame;
        st.durBeats      = c.stepDurBeats;
        st.durFrames     = c.stepDurBeats * c.framesPerBeat();
        st.vel           = vel;
        {
            float* resolved = (float*) c.state();
            if (resolved != nullptr)
                resolved[0] = c.resolvePitch(c.noteExprSemis);
        }
        if (c.ratchetUsePitches) {
            const uint16_t n = c.arrayLenU16Of(c.trainInstrIdx, 1);
            float* rp = (float*) ((uint8_t*) c.stateOfInstr(c.trainInstrIdx)
                                  + sizeof(TrainState));
            for (uint16_t k = 0; k < n; ++k)
                rp[k] = c.resolvePitch(c.arrayF32AtOf(c.trainInstrIdx, 1, (int) k));
        }
        st.deviateAmount = c.deviateAmount;
        st.deviateSeed   = c.deviateSeed;

        configureArmedTrainCursor(c, st);
        advanceTrain(c, st);
        return;
    }

    forEachStepCopy(c, [&] (uint32_t) {
        emitHit(c, c.resolvePitch(c.noteExprSemis), vel, c.stepAbsFrame,
                stepGateFrames(c), 0);
    });
}

// T-469 — gate at the held pitch: velocity + gate-on/off, NO pitch emission.
// Delivery's pitch input is event-held, so the module sounds at whatever
// pitch it last played (or its idle default). Gate length follows the step's
// gate (GateLen override applies, same as a note). Train articulations attach
// as gate-only trains; pitch remains held.
inline void evalGateOnlyCommon(ExecCtx& c, float velocity, uint32_t gateLane,
                               bool emitVelocity, uint32_t channelIndex = 0)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    c.noteInstrIdx = UINT32_MAX;
    c.noteIsExpr = 0;
    if (! c.stepFiring)
        return;
    // F-071 T-531 — gate-only vel honours a NoteVel GEN_OPERAND draw too.
    const float vel = genOperandOr(c, GenTarget::NoteVel, velocity) * c.stepVelScale;
    const double gateFrames = stepGateFrames(c);

    if (c.trainKind != 0 && c.trainState != nullptr) {
        TrainState& st = *(TrainState*) c.trainState;
        st = TrainState {};
        st.active        = 1;
        st.kind          = c.trainKind;
        st.noteInstrIdx  = UINT32_MAX;
        st.isChord       = 0;
        st.articInstrIdx = c.trainInstrIdx;
        st.originFrame   = c.stepAbsFrame;
        st.durBeats      = c.stepDurBeats;
        st.durFrames     = c.stepDurBeats * c.framesPerBeat();
        st.vel           = vel;
        st.gateLane      = gateLane;
        st.channelIndex  = channelIndex;
        st.emitVelocity  = emitVelocity ? 1 : 0;
        st.deviateAmount = c.deviateAmount;
        st.deviateSeed   = c.deviateSeed;

        configureArmedTrainCursor(c, st);
        advanceTrain(c, st);
        return;
    }

    // B-290 — the gate fires at the HELD pitch, so it targets the held
    // note's instance id (a fresh id would land on a silent new slot).
    forEachStepCopy(c, [&] (uint32_t) {
        const uint32_t vid = c.machine.lastVoiceInstance();
        if (emitVelocity)
            c.emit(
                SignalType::Velocity, PacketKind::Set, vel,
                c.stepAbsFrame, vid, 0, 0, channelIndex,
                static_cast<uint64_t>(channelIndex) + 1u);
        c.emit(SignalType::Gate, PacketKind::Set, 1.0f, c.stepAbsFrame, vid,
               gateLane, 0, channelIndex,
               static_cast<uint64_t>(channelIndex) + 1u);
        c.scheduleGateOff(std::max(c.stepAbsFrame,
                                   c.stepAbsFrame + gateFrames - 1.0),
                          vid, 0, gateLane, channelIndex,
                          static_cast<uint64_t>(channelIndex) + 1u);
    });
}

inline void evalGateOnly(ExecCtx& c)
{
    evalGateOnlyCommon(c, c.f32(0), 0, true);
}

// T-469 — legato pitch update: a Pitch Set with no gate edge. Under a held
// gate the module changes pitch without retriggering; with the gate low it
// re-aims the held pitch input for the next gate. Scope transposes apply.
inline void evalPitchSet(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    c.noteInstrIdx = UINT32_MAX;
    c.noteIsExpr = 0;
    if (! c.stepFiring)
        return;
    // B-290 — legato re-aims the HELD note instance, never a fresh slot.
    forEachStepCopy(c, [&] (uint32_t) {
        c.emit(SignalType::Pitch, PacketKind::Set,
               noteToSignal(c.resolvePitch(c.f32(0))), c.stepAbsFrame,
               c.machine.lastVoiceInstance(), 0, 0, 0, 1u);
    });
}

inline SignalType localOutputSignalType(uint8_t raw)
{
    switch ((SignalType) raw) {
        case SignalType::Pitch:
        case SignalType::Gate:
        case SignalType::Velocity:
        case SignalType::Value:
            return (SignalType) raw;
        default:
            return SignalType::Value;
    }
}

inline void evalLocalOutput(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire || ! c.stepFiring)
        return;

    const uint32_t lane = namedOutputLane(c.u32(0));
    const uint32_t channel = c.u32(1);
    const auto type = localOutputSignalType(c.u8(2));
    const float value =
        c.externalStepClockOutputValue(lane, type, c.f32(3));
    forEachStepCopy(c, [&] (uint32_t) {
        const uint32_t vid = c.machine.lastVoiceInstance();
        c.emit(type, PacketKind::Set, value, c.stepAbsFrame, vid, lane, 0,
               channel, static_cast<uint64_t>(channel) + 1u);

        if (type == SignalType::Gate && isGateOpen(value)) {
            const double gateFrames = stepGateFrames(c);
            c.scheduleGateOff(std::max(c.stepAbsFrame,
                                       c.stepAbsFrame + gateFrames - 1.0),
                              vid, 0, lane, channel,
                              static_cast<uint64_t>(channel) + 1u);
        } else if (type == SignalType::Value) {
            c.schedule(SignalType::Value, PacketKind::Release, 0.0f,
                       c.stepAbsFrame
                           + c.stepDurBeats * c.framesPerBeat(),
                       vid, lane, 0, channel,
                       static_cast<uint64_t>(channel) + 1u);
        }
    });
}

inline void evalLocalGateOnly(ExecCtx& c)
{
    evalGateOnlyCommon(c, c.f32(2), namedOutputLane(c.u32(0)), false,
                       c.u32(1));
}

struct MarkovEventState {
    uint8_t initialized = 0;
    uint16_t current = 0;
    uint32_t draws = 0;
};

inline bool markovStateEquals(ExecCtx& c, int a, int b, bool hasValue)
{
    if (a == b)
        return true;
    if (c.arrayF32At(0, a) != c.arrayF32At(0, b))
        return false;
    if (hasValue && c.arrayF32At(3, a) != c.arrayF32At(3, b))
        return false;
    return true;
}

inline uint16_t nextMarkovIndex(ExecCtx& c, MarkovEventState& st, uint16_t n,
                                bool hasValue, uint32_t seed)
{
    uint16_t count = 0;
    const int current = (int) (st.current % n);
    for (uint16_t i = 0; i < n; ++i)
        if (markovStateEquals(c, current, (int) i, hasValue))
            ++count;
    if (count <= 1 && n > 1) {
        const uint16_t alternatives = (uint16_t) (n - 1);
        const float r = hash01(seed, ++st.draws);
        uint16_t pick = (uint16_t) std::min<int>((int) alternatives - 1,
                                                 (int) (r * alternatives));
        for (uint16_t i = 0; i < n; ++i) {
            if ((int) i == current)
                continue;
            if (pick-- == 0)
                return i;
        }
    }
    if (count == 0)
        return (uint16_t) ((current + 1) % n);
    const float r = hash01(seed, ++st.draws);
    const uint16_t pick = (uint16_t) std::min<int>((int) count - 1, (int) (r * count));
    uint16_t seen = 0;
    for (uint16_t i = 0; i < n; ++i) {
        if (! markovStateEquals(c, current, (int) i, hasValue))
            continue;
        if (seen++ == pick)
            return (uint16_t) ((i + 1u) % n);
    }
    return (uint16_t) ((current + 1) % n);
}

inline void evalMarkovEvent(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire || ! c.stepFiring)
        return;

    const uint16_t n = c.arrayLenU16(0);
    if (n == 0)
        return;

    MarkovEventState* st = (MarkovEventState*) c.state();
    if (st == nullptr)
        return;
    if (! st->initialized) {
        *st = MarkovEventState {};
        st->initialized = 1;
    }
    if (st->current >= n)
        st->current = 0;

    const bool hasValue = c.u8(4) != 0 && c.u16(2) != 0xffff
                       && c.arrayLenU16(3) == n;
    const uint32_t seed = c.u32(5);

    forEachStepCopy(c, [&] (uint32_t) {
        const uint16_t idx = st->current;
        const float semis = c.arrayF32At(0, idx);
        const float vel = idx < c.arrayLenU16(1) ? c.arrayF32At(1, idx) : 0.7f;
        emitHit(c, c.resolvePitch(semis), vel, c.stepAbsFrame, stepGateFrames(c), 0);
        if (hasValue) {
            const uint16_t lane = c.u16(2);
            c.emit(SignalType::Value, PacketKind::Set, c.arrayF32At(3, idx),
                   c.stepAbsFrame, 0, lane);
            c.schedule(SignalType::Value, PacketKind::Release, 0.0f,
                       c.stepAbsFrame + c.stepDurBeats * c.framesPerBeat(), 0, lane);
        }
        st->current = nextMarkovIndex(c, *st, n, hasValue, seed);
    });
}

inline bool markovGateStateEquals(ExecCtx& c, int a, int b)
{
    if (a == b)
        return true;
    return c.arrayF32At(0, a) == c.arrayF32At(0, b);
}

inline uint16_t nextMarkovGateIndex(ExecCtx& c, MarkovEventState& st,
                                    uint16_t n, uint32_t seed)
{
    uint16_t count = 0;
    const int current = (int) (st.current % n);
    for (uint16_t i = 0; i < n; ++i)
        if (markovGateStateEquals(c, current, (int) i))
            ++count;
    if (count <= 1 && n > 1) {
        const uint16_t alternatives = (uint16_t) (n - 1);
        const float r = hash01(seed, ++st.draws);
        uint16_t pick = (uint16_t) std::min<int>((int) alternatives - 1,
                                                 (int) (r * alternatives));
        for (uint16_t i = 0; i < n; ++i) {
            if ((int) i == current)
                continue;
            if (pick-- == 0)
                return i;
        }
    }
    if (count == 0)
        return (uint16_t) ((current + 1) % n);
    const float r = hash01(seed, ++st.draws);
    const uint16_t pick = (uint16_t) std::min<int>((int) count - 1, (int) (r * count));
    uint16_t seen = 0;
    for (uint16_t i = 0; i < n; ++i) {
        if (! markovGateStateEquals(c, current, (int) i))
            continue;
        if (seen++ == pick)
            return (uint16_t) ((i + 1u) % n);
    }
    return (uint16_t) ((current + 1) % n);
}

inline void evalMarkovGate(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire || ! c.stepFiring)
        return;

    const uint16_t n = c.arrayLenU16(0);
    if (n == 0)
        return;

    MarkovEventState* st = (MarkovEventState*) c.state();
    if (st == nullptr)
        return;
    if (! st->initialized) {
        *st = MarkovEventState {};
        st->initialized = 1;
    }
    if (st->current >= n)
        st->current = 0;

    const uint32_t seed = c.u32(2);
    const uint16_t idx = st->current;
    const float gate = c.arrayF32At(0, idx);
    const float vel = idx < c.arrayLenU16(1) ? c.arrayF32At(1, idx) : 0.7f;
    if (isGateOpen(gate))
        evalGateOnlyCommon(c, vel, 0, true);
    st->current = nextMarkovGateIndex(c, *st, n, seed);
}

inline void evalRest(ExecCtx& c)
{
    // A rest is the explicit absence of emissions for the current step.
    // The note marker stays cleared so post-note articulations stay silent.
    if (c.phase == ExecCtx::Phase::Fire) {
        c.noteInstrIdx = UINT32_MAX;
        c.noteIsExpr = 0;
    }
}

// ── Articulations ─────────────────────────────────────────────────

// B-187 — GEN_OPERAND prefix: a generator-valued articulation arg, drawn at
// arm time (one draw per loop iteration via the roll keying) and stashed for
// the articulation op that follows in the step body. The articulation's own
// immediate operand is the never-consumed disassembly midpoint while the
// prefix arms.
inline float genOperandOr(const ExecCtx& c, GenTarget t, float immediate)
{
    const uint16_t bit = (uint16_t) (1u << (uint8_t) t);
    return (c.genOperandMask & bit) ? c.genOperandVal[(uint8_t) t] : immediate;
}

inline float hash01(uint64_t a, uint64_t b);
inline float evalFireExprOf(ExecCtx& c, uint32_t instrIdx, int codeOp,
                            int immOp, uint32_t seed);

inline void evalGenOperand(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire || ! c.stepFiring)
        return;
    const uint8_t target = c.u8(0);
    const uint8_t kind   = c.u8(1);
    const float a = c.f32(2), b = c.f32(3), w = c.f32(4);
    if (target >= kGenTargetCount)
        return;
    if (kind == 2) {                                  // F-071 — ~auto ramp:
        // stash BOTH endpoints; the consumer (train cursor) re-evaluates the
        // interpolated value per hit. No draw — the ramp IS the value.
        if (! std::isfinite(a) || ! std::isfinite(b))
            return;                                    // B-307 — never arm NaN
        c.genOperandMask     |= (uint16_t) (1u << target);
        c.genOperandRampMask |= (uint16_t) (1u << target);
        c.genOperandVal[target]  = a;
        c.genOperandValB[target] = b;
        return;
    }
    const float r = c.roll(c.u32(5));
    float v = a;
    if (kind == 0)      v = a + r * (b - a);          // ~random(a, b)
    else if (kind == 1) v = (r < w) ? a : b;          // ~bernoulli(w, a, b)
    if (! std::isfinite(v))                           // B-307 — never arm NaN;
        return;                                       // the immediate stands
    c.genOperandMask |= (uint16_t) (1u << target);
    c.genOperandVal[target] = v;
}

inline float fireExprImmediateOf(ExecCtx& c, uint32_t instrIdx, int immOp, int index)
{
    return index < (int) c.arrayLenU16Of(instrIdx, immOp)
        ? c.arrayF32AtOf(instrIdx, immOp, index) : 0.0f;
}

inline float evalFireExprOf(ExecCtx& c, uint32_t instrIdx, int codeOp,
                            int immOp, uint32_t seed)
{
    const int n = (int) c.arrayLenU8Of(instrIdx, codeOp);
    if (n <= 0)
        return fireExprImmediateOf(c, instrIdx, immOp, 0);

    float stack[32] {};
    int sp = 0;
    auto push = [&] (float v) {
        if (sp < (int) (sizeof(stack) / sizeof(stack[0]))) stack[sp++] = v;
    };
    auto pop = [&] (float fb = 0.0f) -> float {
        return sp > 0 ? stack[--sp] : fb;
    };

    for (int i = 0; i < n; ++i) {
        const uint16_t tok = c.arrayU16AtOf(instrIdx, codeOp, i);
        const auto tag = (FireExprTag) (tok >> 8);
        const uint8_t aux = (uint8_t) (tok & 0xff);

        switch (tag) {
            case FireExprTag::Lit:
                push(fireExprImmediateOf(c, instrIdx, immOp, aux));
                break;
            case FireExprTag::Lfo: {
                const float phase01 = pop(0.0f);
                const float mx = pop(1.0f);
                const float mn = pop(0.0f);
                const float rate = std::max(0.0f, pop(1.0f));
                const double f = c.stepWillFireFrame;
                const double t = (f - c.scopeAnchorFrame()) / c.sampleRate();
                double phase = (double) rate * t + (double) phase01;
                phase -= std::floor(phase);
                float norm = 0.5f;
                switch (aux) {
                    case 0:  norm = 0.5f + 0.5f * std::sin((float) (phase * 2.0 * 3.14159265358979)); break;
                    case 1:  norm = (float) (phase < 0.5 ? 2.0 * phase : 2.0 - 2.0 * phase); break;
                    case 2:  norm = (float) phase; break;
                    case 3:  norm = phase < 0.5 ? 1.0f : 0.0f; break;
                    case 4:  norm = hash01((uint64_t) instrIdx * 0xC0FFEEull,
                                           (uint64_t) std::floor(phase + t * rate)); break;
                    default: break;
                }
                push(mn + norm * (mx - mn));
                break;
            }
            case FireExprTag::Auto: {
                const float end = pop(1.0f);
                const float start = pop(0.0f);
                const double span = c.scopeSpanEndFrame() - c.scopeAnchorFrame();
                float p = span > 0.0 ? (float) ((c.stepWillFireFrame - c.scopeAnchorFrame()) / span)
                                      : 0.0f;
                p = std::min(1.0f, std::max(0.0f, p));
                float t = p;
                switch (aux) {
                    case 1:  t = p * p; break;
                    case 2:  t = std::sqrt(p); break;
                    case 3:  t = p < 0.5f ? 2.0f * p * p
                                          : 1.0f - std::pow(-2.0f * p + 2.0f, 2.0f) / 2.0f; break;
                    default: break;
                }
                push(start + t * (end - start));
                break;
            }
            case FireExprTag::Random: {
                (void) pop(0.0f); // slew is render-only; fire-time draw is instantaneous.
                const float mx = pop(1.0f);
                const float mn = pop(0.0f);
                const float r = hash01(seed + (uint32_t) i * 17u,
                                       (uint64_t) c.iteration + 1u);
                push(mn + r * (mx - mn));
                break;
            }
            case FireExprTag::Bernoulli: {
                const float b = pop(1.0f);
                const float a = pop(0.0f);
                const float w = pop(0.5f);
                const float r = hash01(seed + (uint32_t) i * 29u,
                                       (uint64_t) c.iteration + 1u);
                push(r < w ? a : b);
                break;
            }
            case FireExprTag::Clip: {
                float mx = pop(1.0f), mn = pop(-1.0f), v = pop(0.0f);
                if (mn > mx) std::swap(mn, mx);
                push(std::max(mn, std::min(mx, v)));
                break;
            }
            case FireExprTag::Invert: {
                const float pivot = pop(0.5f);
                const float v = pop(0.0f);
                push(2.0f * pivot - v);
                break;
            }
            case FireExprTag::Scale: {
                const float mx = pop(1.0f), mn = pop(0.0f), v = pop(0.0f);
                push(mn + v * (mx - mn));
                break;
            }
            case FireExprTag::Offset: {
                const float amount = pop(0.0f), v = pop(0.0f);
                push(v + amount);
                break;
            }
            case FireExprTag::Gain: {
                const float amount = pop(1.0f), v = pop(0.0f);
                push(v * amount);
                break;
            }
            case FireExprTag::Abs:
                push(std::abs(pop(0.0f)));
                break;
            case FireExprTag::Divide: {
                const float denom = pop(1.0f);
                const float num = pop(0.0f);
                push(std::abs(denom) > 1.0e-9f ? num / denom : 0.0f);
                break;
            }
            case FireExprTag::Input: {
                const int required = std::max(1, (int) std::ceil(c.segEnd - c.segStart));
                const float* row = c.externalInputRow(aux, required);
                if (row == nullptr) {
                    push(0.0f);
                    break;
                }
                const int idx = std::clamp((int) std::floor(c.stepWillFireFrame - c.segStart),
                                           0, required - 1);
                const float v = row[idx];
                push(std::isfinite(v) ? v : 0.0f);
                break;
            }
        }
    }
    return sp > 0 && std::isfinite(stack[sp - 1]) ? stack[sp - 1]
                                                  : fireExprImmediateOf(c, instrIdx, immOp, 0);
}

inline float evalFireExpr(ExecCtx& c, int codeOp, int immOp, uint32_t seed)
{
    return evalFireExprOf(c, c.instrIndex, codeOp, immOp, seed);
}

inline void evalGenOperandDyn(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire || ! c.stepFiring)
        return;
    const uint8_t target = c.u8(0);
    const uint8_t kind = c.u8(1);
    const uint32_t seed = c.u32(2);
    if (target >= kGenTargetCount)
        return;

    const float a = evalFireExpr(c, 3, 4, seed ^ 0xA11u);
    const float b = evalFireExpr(c, 5, 6, seed ^ 0xB22u);
    const float w = evalFireExpr(c, 7, 8, seed ^ 0xC33u);

    if (kind == 2) {
        if (! std::isfinite(a) || ! std::isfinite(b))
            return;
        c.genOperandMask     |= (uint16_t) (1u << target);
        c.genOperandRampMask |= (uint16_t) (1u << target);
        c.genOperandVal[target]  = a;
        c.genOperandValB[target] = b;
        return;
    }

    const float r = c.roll(seed);
    float v = a;
    if (kind == 0)      v = a + r * (b - a);
    else if (kind == 1) v = (r < w) ? a : b;
    if (! std::isfinite(v))
        return;
    c.genOperandMask |= (uint16_t) (1u << target);
    c.genOperandVal[target] = v;
}

// Train articulations (precede NOTE): stream their armed state every walk,
// and stash arm parameters for the note op when the step is firing.
inline void evalRatchetCommon(ExecCtx& c, bool pitched)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    advanceTrain(c, *(TrainState*) c.state());
    if (! c.stepFiring)
        return;
    c.trainKind         = 1;
    c.trainState        = c.state();
    c.trainInstrIdx     = c.instrIndex;
    const float dynCount = genOperandOr(c, GenTarget::RatchetCount, -1.0f);
    const float dynSpeed = genOperandOr(c, GenTarget::ArpSpeedBeats, -1.0f);
    if (dynCount >= 0.0f) {
        c.ratchetCount = (uint64_t) std::max(1.0f, std::round(dynCount));
    } else if (dynSpeed > 0.0f) {
        // B-186 — count derived from the drawn speed; 20000 is the audible
        // retrigger ceiling shared with ratchet (repetition rate ≤ 20kHz).
        const double n = std::floor(c.stepDurBeats / (double) dynSpeed);
        c.ratchetCount = (uint64_t) std::min(20000.0, std::max(1.0, n));
    } else {
        c.ratchetCount = c.u32(0);
    }
    c.ratchetUsePitches = pitched ? 1 : 0;
}

inline void evalRatchet(ExecCtx& c)        { evalRatchetCommon(c, false); }
inline void evalRatchetPitched(ExecCtx& c) { evalRatchetCommon(c, true); }

inline void evalBuzz(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    advanceTrain(c, *(TrainState*) c.state());
    if (! c.stepFiring)
        return;
    c.trainKind     = 2;
    c.trainState    = c.state();
    c.trainInstrIdx = c.instrIndex;
    c.buzzPressure  = genOperandOr(c, GenTarget::BuzzPressure, c.f32(0));
    c.buzzDurBeats  = c.f32(1);
}

inline void evalBounce(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    advanceTrain(c, *(TrainState*) c.state());
    if (! c.stepFiring)
        return;
    c.trainKind     = 3;
    c.trainState    = c.state();
    c.trainInstrIdx = c.instrIndex;
    c.bounceGravity = genOperandOr(c, GenTarget::BounceGravity, c.f32(0));
    // F-071 T-516 — authorable interval + unit domain (0 none / 1 beats / 2 seconds).
    c.bounceInterval       = c.f32(1);
    c.bounceIntervalDomain = c.u8(2);
}

inline void evalGeiger(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    advanceTrain(c, *(TrainState*) c.state());
    if (! c.stepFiring)
        return;
    c.trainKind     = 4;
    c.trainState    = c.state();
    c.trainInstrIdx = c.instrIndex;
    c.geigerDensity = genOperandOr(c, GenTarget::GeigerDensity, c.f32(0));
    c.geigerSeed    = c.u32(1);
}

inline void evalGateLen(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    // F-071 T-531 — len may be a per-loop GEN_OPERAND draw (GateLen, in beats;
    // the compiler resolves the gen endpoints to beats so the draw is direct).
    c.gateLenBeats = genOperandOr(c, GenTarget::GateLen, c.f32(0));
}

// B-286 — plain `deviate` step op (precedes NOTE, like GateLen): arms the
// per-hit onset jitter the emitHit funnel applies. Tilde law: this shapes
// TIMING; ~deviate (CTRL_DEVIATE) targets a param.
inline void evalDeviate(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    c.deviateAmount = genOperandOr(c, GenTarget::DeviateAmount, c.f32(0));
    c.deviateSeed   = c.u32(1);
}

// Flam (follows NOTE): the grace lands BEFORE the beat. The step's fire
// verdict + frame are identical on every walk of an iteration, so the walk
// whose segment contains the grace frame emits it — exact and block-size
// independent. A grace that would wrap into the previous loop iteration
// (step offset < flam offset) clamps to the iteration boundary.
inline void evalFlam(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    if (c.stepSuppressed || c.noteInstrIdx == UINT32_MAX)
        return;
    const float offsetBeats = genOperandOr(c, GenTarget::FlamOffset, c.f32(0));
    const float graceScale  = genOperandOr(c, GenTarget::FlamGain,  c.f32(1));
    const double fpb = c.framesPerBeat();
    const double iterStart =                  // B-324 — anchor-mapped boundary
        c.frameOfBeat((double) c.iteration * c.machine.loopLength());
    const float vel = noteVelOf(c) * c.stepVelScale * graceScale;
    const double gateFrames = (double) offsetBeats * fpb;
    const int voices = c.noteIsChord ? noteVoiceCount(c) : 1;
    const double savedFrame = c.stepAbsFrame;
    const uint32_t n = std::max<uint32_t>(1, c.stepRepeatCount);
    for (uint32_t i = 0; i < n; ++i) {
        const double copyFrame = c.stepRepeatBaseFrame
                               + (double) i * c.stepRepeatStrideFrames;
        double copyGraceFrame = copyFrame - offsetBeats * fpb;
        if (copyGraceFrame < iterStart)
            copyGraceFrame = iterStart;
        if (copyGraceFrame < c.segStart || copyGraceFrame >= c.segEnd)
            continue;
        c.stepAbsFrame = copyFrame;
        for (int v = 0; v < voices; ++v)
            emitHit(c, c.resolvePitch(noteSemisOf(c, v)), vel, copyGraceFrame,
                    gateFrames, (uint32_t) v);
    }
    c.stepAbsFrame = savedFrame;
}

// ── Param lock (follows NOTE; fires for REST steps too) ──────────

inline void evalParamLock(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire || ! c.stepFiring)
        return;
    const uint16_t lane  = c.u16(0);
    const float value    = c.f32(1);
    const uint8_t presence = c.u8(2);
    const PacketKind kind = (presence == 2) ? PacketKind::Offset : PacketKind::Set;
    forEachStepCopy(c, [&] (uint32_t) {
        c.emit(SignalType::Value, kind, value, c.stepAbsFrame, 0, lane);
        // Step-scoped: the lock releases at step end (snappy step-time locks —
        // delivery composes Release as "this source no longer participates").
        c.schedule(SignalType::Value, PacketKind::Release, 0.0f,
                   c.stepAbsFrame + c.stepDurBeats * c.framesPerBeat(), 0, lane);
    });
}

inline void evalParamLockTyped(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire || ! c.stepFiring)
        return;
    const uint16_t lane = c.u16(0);
    const float authored = c.f32(1);
    const uint8_t basis = c.u8(2);
    const float stepBeats = c.f32(3);
    const uint8_t presence = c.u8(4);
    const PacketKind kind = presence == 2 ? PacketKind::Offset : PacketKind::Set;
    forEachStepCopy(c, [&] (uint32_t) {
        // Script-step timing is a live source context: stepDurBeats already
        // includes the controlling scope's timescale. Wall-clock/frequency
        // authored values retain their compact authored context unchanged.
        const float contextStepBeats = basis == (uint8_t) curlop::param::AuthoredBasis::ScriptSteps
            ? (float) c.stepDurBeats : stepBeats;
        c.emitTypedParam(lane, authored, basis, contextStepBeats, kind == PacketKind::Offset,
                         c.stepAbsFrame);
        c.schedule(SignalType::Value, PacketKind::Release, 0.0f,
                   c.stepAbsFrame + c.stepDurBeats * c.framesPerBeat(), 0, lane);
    });
}

// ── Conditionals ──────────────────────────────────────────────────
// All count loops 1-indexed; cycle-modulo forms fold the loop count into
// [1..cycle] first (cycle 0 = absolute). A failed predicate sets the guard
// the next step consumes. Predicates ported from the old SequencerVM.

inline bool vmIsPrime(uint32_t n)
{
    if (n < 2) return false;
    if (n < 4) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (uint32_t i = 5; i * i <= n; i += 6)
        if (n % i == 0 || n % (i + 2) == 0) return false;
    return true;
}

inline bool vmIsFibonacci(uint32_t n)
{
    if (n <= 1) return true;
    uint32_t a = 0, b = 1;
    while (b < n) { const uint32_t t = a + b; a = b; b = t; }
    return b == n;
}

// Shared cycle-modulo conditional body: u16 cycle operand at index 0.
inline void evalCondCycle(ExecCtx& c, bool (*predicate)(uint32_t))
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    const uint16_t cycle = c.u16(0);
    const uint32_t beat  = (uint32_t) c.iteration + 1;   // 1-indexed
    const uint32_t b     = (cycle > 0) ? (((beat - 1) % cycle) + 1) : beat;
    if (! predicate(b))
        c.condSkip = true;
}

inline void evalCondFirst(ExecCtx& c)    { evalCondCycle(c, [] (uint32_t b) { return b == 1; }); }
inline void evalCondNotFirst(ExecCtx& c) { evalCondCycle(c, [] (uint32_t b) { return b != 1; }); }
inline void evalCondEven(ExecCtx& c)     { evalCondCycle(c, [] (uint32_t b) { return b % 2u == 0u; }); }
inline void evalCondOdd(ExecCtx& c)      { evalCondCycle(c, [] (uint32_t b) { return b % 2u == 1u; }); }
inline void evalCondPrime(ExecCtx& c)    { evalCondCycle(c, &vmIsPrime); }
inline void evalCondFib(ExecCtx& c)      { evalCondCycle(c, &vmIsFibonacci); }

inline void evalCondMod(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    const uint16_t divisor = c.u16(0);
    const uint32_t beat    = (uint32_t) c.iteration + 1;
    if (divisor > 0 && (beat % divisor) != 0)
        c.condSkip = true;
}

inline void evalCondLoopEq(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    if ((uint32_t) c.iteration + 1 != c.u16(0))
        c.condSkip = true;
}

inline void evalCondAfter(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    if ((uint32_t) c.iteration + 1 <= c.u16(0))
        c.condSkip = true;
}

inline void evalCondLoopSetImpl(ExecCtx& c, bool wantMember)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    const uint32_t beat = (uint32_t) c.iteration + 1;
    const uint8_t  n    = c.arrayLenU8(0);
    bool matched = false;
    for (int k = 0; k < n; ++k)
        if (beat == c.arrayU16At(0, k)) { matched = true; break; }
    if (matched != wantMember)
        c.condSkip = true;
}

inline void evalCondLoopSet(ExecCtx& c)    { evalCondLoopSetImpl(c, true); }
inline void evalCondNotLoopSet(ExecCtx& c) { evalCondLoopSetImpl(c, false); }

inline void evalCondPreviousImpl(ExecCtx& c, bool wantPrevious)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    if (c.machine.previousStepFiredForGuard(c.instrIndex) != wantPrevious)
        c.condSkip = true;
}

inline void evalCondPrevious(ExecCtx& c)    { evalCondPreviousImpl(c, true); }
inline void evalCondNotPrevious(ExecCtx& c) { evalCondPreviousImpl(c, false); }

inline void evalCondExpr(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    const float v = evalFireExprOf(c, c.instrIndex, 0, 1, 0xC0DE140Du);
    if (! std::isfinite(v) || v <= 0.0f)
        c.condSkip = true;
}

// ── Probability ───────────────────────────────────────────────────

inline void evalProb(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    const float threshold = c.f32(0);
    if (c.roll(c.u32(1)) >= threshold)
        c.condSkip = true;
}

inline void evalProbDyn(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    const uint32_t seed = c.u32(2);
    const float threshold = evalFireExprOf(c, c.instrIndex, 0, 1,
                                           seed ^ 0x9B0B0B0u);
    if (c.roll(seed) >= threshold)
        c.condSkip = true;
}

inline void evalBernoulliCommon(ExecCtx& c, float weight, uint8_t count, uint32_t seed)
{
    // §5.8 weight law: signed, -1..1. 0 = uniform; negative tilts the pick
    // toward the FIRST options (graded, -1 = always first); positive tilts
    // toward the LAST (+1 = always last). Implemented as a power-warp of
    // the uniform roll: r^k with k>1 compresses toward 0 (first options),
    // k<1 stretches toward 1 (last options). k and 1/k mirror.
    if (count == 0)
        return;
    const float r = c.roll(seed);
    int selected;
    const float t = std::min(1.0f, std::fabs(weight));
    if (count <= 1) {
        selected = 0;
    } else if (t < 1e-6f) {
        selected = std::min((int) count - 1, (int) (r * count));
    } else {
        const float k = (weight < 0.0f) ? 1.0f / (1.0f - t + 1e-6f)
                                        : (1.0f - t + 1e-6f);
        selected = std::min((int) count - 1, (int) (std::pow(r, k) * count));
    }
    c.bernoulliRemaining = count;
    c.bernoulliTotal     = count;
    c.bernoulliPick      = selected;
}

inline void evalBernoulli(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    evalBernoulliCommon(c, c.f32(0), c.u8(1), c.u32(2));
}

inline void evalBernoulliDyn(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;
    const uint32_t seed = c.u32(3);
    const float weight = evalFireExprOf(c, c.instrIndex, 0, 1, seed ^ 0xBEA11u);
    evalBernoulliCommon(c, weight, c.u8(2), seed);
}

inline void evalSelectDyn(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Fire)
        return;

    const int branchCount = (int) c.arrayLenU8(2);
    if (branchCount <= 0)
        return;

    int totalSteps = 0;
    for (int i = 0; i < branchCount; ++i)
        totalSteps += (int) c.arrayU16At(2, i);
    if (totalSteps <= 0)
        return;

    const float selector = std::clamp(evalFireExprOf(c, c.instrIndex, 0, 1,
                                                     0x5E1EC7u),
                                      0.0f, 1.0f);
    const int pick = std::min(branchCount - 1, (int) std::floor(selector * branchCount));

    c.selectRemainingSteps = totalSteps;
    c.selectCurrentBranch = 0;
    c.selectCurrentBranchRemain = (int) c.arrayU16At(2, 0);
    c.selectPick = pick;
    c.selectBranchCount = branchCount;
    c.selectInstrIdx = c.instrIndex;
}

// ── Control ops: per-sample stream producers (Render phase) ──────
// SF-061 lifecycle: the scope is the op's clock — anchored at scope entry,
// streaming across inner steps (B-246), dead past scope exit. Notes only
// SAMPLE an op unless its policy says retrigger. Envelope/LFO/keytrack/auto
// math ported from GeneratorEval.h.

inline float hash01(uint64_t a, uint64_t b)
{
    uint64_t x = a ^ (b * 0x9E3779B97F4A7C15ull);
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x =  x ^ (x >> 31);
    return (float) ((x >> 11) * (1.0 / 9007199254740992.0));
}

// Render window for a scope-clocked op: segment ∩ scope span (+ optional
// tail frames past the span end, for release phases).
struct RenderSpan {
    int lo = 0, hi = 0;       // block-relative sample indices
    double anchor = 0.0;      // scope entry (absolute frames)
    double spanEnd = 0.0;
};

inline bool renderSpanOf(ExecCtx& c, double tailFrames, RenderSpan& out)
{
    out.anchor  = c.scopeAnchorFrame();
    out.spanEnd = c.scopeSpanEndFrame();
    const double lo = std::max(c.segStart, out.anchor);
    const double hi = std::min(c.segEnd, out.spanEnd + tailFrames);
    if (hi <= lo)
        return false;
    out.lo = (int) (lo - c.blockStartFrame());
    out.hi = (int) std::ceil(hi - c.blockStartFrame());
    return true;
}

inline void evalCtrlLfo(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;
    float* row = c.streamFor(c.u16(0), SignalType::Value,
                             c.u8(5) ? PacketKind::Offset : PacketKind::Stream);
    const uint8_t wave = c.u8(1);
    const float rate = c.f32(2), mn = c.f32(3), mx = c.f32(4);
    const float phase01 = c.f32(6);   // cycle-fraction start offset
    // F-071 T-495 — unit domain. Absolute (0): `rate` is Hz, phase = rate·t
    // (immune to tempo + timescale). BPM-relative (1): `rate` is cycles-per-
    // beat; advance phase in BEATS via the anchored clock, scaled by the
    // scope's cumulative timescale (tape-stretch). At the clock tempo with
    // timescale=1, cyclesPerBeat·beats == Hz·seconds → bit-identical.
    const bool bpmRel = c.u8(7) != 0;
    const double sr = c.sampleRate();
    const double tsFactor = c.scopeTimescale();
    const double beatAtAnchor = c.beatOfFrame(span.anchor);
    for (int i = span.lo; i < span.hi; ++i) {
        const double f = c.blockStartFrame() + i;
        const double t = (f - span.anchor) / sr;
        const double cyc = bpmRel
            ? (double) rate * (c.beatOfFrame(f) - beatAtAnchor) * tsFactor
            : (double) rate * t;
        double phase = cyc + (double) phase01;
        phase -= std::floor(phase);
        float norm;
        switch (wave) {
            case 0:  norm = 0.5f + 0.5f * std::sin((float) (phase * 2.0 * 3.14159265358979)); break;
            case 1:  norm = (float) (phase < 0.5 ? 2.0 * phase : 2.0 - 2.0 * phase); break;
            case 2:  norm = (float) phase; break;
            case 3:  norm = phase < 0.5 ? 1.0f : 0.0f; break;
            case 4:  norm = hash01((uint64_t) c.instrIndex * 0xC0FFEEull,
                                   (uint64_t) std::floor(cyc)); break;
            default: norm = 0.5f; break;
        }
        row[i] = mn + norm * (mx - mn);
    }
}

inline void evalCtrlClip(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;

    const uint16_t lane = c.u16(0);
    const StreamRow* src = c.takePreviousStreamRow(lane, SignalType::Value);
    if (src == nullptr)
        return;

    float mn = c.f32(1);
    float mx = c.f32(2);
    if (mn > mx)
        std::swap(mn, mx);

    float* out = c.streamFor(lane, SignalType::Value, src->kind);
    if (out == nullptr)
        return;

    const float* in = src->data.data();
    for (int i = span.lo; i < span.hi; ++i)
        out[i] = std::max(mn, std::min(mx, in[i]));
}

inline const StreamRow* takeValueTransformInput(ExecCtx& c, uint16_t lane,
                                                float*& out)
{
    const StreamRow* src = c.takePreviousStreamRow(lane, SignalType::Value);
    if (src == nullptr) {
        out = nullptr;
        return nullptr;
    }
    out = c.streamFor(lane, SignalType::Value, src->kind);
    return out == nullptr ? nullptr : src;
}

inline void evalCtrlInvert(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;

    const uint16_t lane = c.u16(0);
    float* out = nullptr;
    const StreamRow* src = takeValueTransformInput(c, lane, out);
    if (src == nullptr)
        return;

    const float pivot = c.f32(1);
    const float* in = src->data.data();
    for (int i = span.lo; i < span.hi; ++i)
        out[i] = 2.0f * pivot - in[i];
}

inline void evalCtrlScale(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;

    const uint16_t lane = c.u16(0);
    float* out = nullptr;
    const StreamRow* src = takeValueTransformInput(c, lane, out);
    if (src == nullptr)
        return;

    const float mn = c.f32(1);
    const float mx = c.f32(2);
    const float* in = src->data.data();
    for (int i = span.lo; i < span.hi; ++i)
        out[i] = mn + in[i] * (mx - mn);
}

inline float quantizeSignalToStep(float signal, float step)
{
    if (! std::isfinite(signal) || ! std::isfinite(step) || step <= 0.0f)
        return signal;
    return std::round(signal / step) * step;
}

inline void evalCtrlQuantize(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;

    const uint16_t lane = c.u16(0);
    float* out = nullptr;
    const StreamRow* src = takeValueTransformInput(c, lane, out);
    if (src == nullptr)
        return;

    const float step = c.f32(1);
    const float* in = src->data.data();
    for (int i = span.lo; i < span.hi; ++i)
        out[i] = quantizeSignalToStep(in[i], step);
}

inline float quantizeSignalToScale(ExecCtx& c, float signal, float root,
                                   int intervalsOp, int n)
{
    if (n <= 0 || ! std::isfinite(signal))
        return signal;
    const float semis = signalToNote(signal);
    float chroma = std::fmod(semis, 12.0f);
    if (chroma < 0.0f) chroma += 12.0f;
    float bestDiff = std::numeric_limits<float>::infinity();
    float bestTarget = chroma;
    for (int k = 0; k < n; ++k) {
        float target = std::fmod(c.arrayF32At(intervalsOp, k) + root, 12.0f);
        if (target < 0.0f) target += 12.0f;
        float diff = std::fabs(chroma - target);
        if (diff > 6.0f) diff = 12.0f - diff;
        if (diff < bestDiff) { bestDiff = diff; bestTarget = target; }
    }
    float shift = bestTarget - chroma;
    if (shift > 6.0f)  shift -= 12.0f;
    if (shift < -6.0f) shift += 12.0f;
    return noteToSignal(semis + shift);
}

inline void evalCtrlQuantizeScale(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;

    const uint16_t lane = c.u16(0);
    float* out = nullptr;
    const StreamRow* src = takeValueTransformInput(c, lane, out);
    if (src == nullptr)
        return;

    const float root = c.f32(1);
    const int n = (int) c.arrayLenU16(2);
    const float* in = src->data.data();
    for (int i = span.lo; i < span.hi; ++i)
        out[i] = quantizeSignalToScale(c, in[i], root, 2, n);
}

inline void evalCtrlInput(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;

    const uint32_t channel = c.u32(1);
    float* out = c.streamFor(
        c.u32(0), SignalType::Value, PacketKind::Stream, false,
        channel, static_cast<uint64_t>(channel) + 1u);
    if (out == nullptr)
        return;

    const float* in = c.externalInputRow(channel, span.hi);
    if (in == nullptr) {
        for (int i = span.lo; i < span.hi; ++i)
            out[i] = 0.0f;
        return;
    }
    for (int i = span.lo; i < span.hi; ++i)
        out[i] = std::isfinite(in[i]) ? in[i] : 0.0f;
}

inline void evalChannelApply(ExecCtx& c)
{
    if (c.phase == ExecCtx::Phase::Fire)
        c.activateChannelPlan(c.u32(0));
}

inline void evalCtrlOffset(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;

    const uint16_t lane = c.u16(0);
    float* out = nullptr;
    const StreamRow* src = takeValueTransformInput(c, lane, out);
    if (src == nullptr)
        return;

    const float amount = c.f32(1);
    const float* in = src->data.data();
    for (int i = span.lo; i < span.hi; ++i)
        out[i] = in[i] + amount;
}

inline void evalCtrlGain(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;

    const uint16_t lane = c.u16(0);
    float* out = nullptr;
    const StreamRow* src = takeValueTransformInput(c, lane, out);
    if (src == nullptr)
        return;

    const float amount = c.f32(1);
    const float* in = src->data.data();
    for (int i = span.lo; i < span.hi; ++i)
        out[i] = in[i] * amount;
}

inline void evalCtrlAbs(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;

    const uint16_t lane = c.u16(0);
    float* out = nullptr;
    const StreamRow* src = takeValueTransformInput(c, lane, out);
    if (src == nullptr)
        return;

    const float* in = src->data.data();
    for (int i = span.lo; i < span.hi; ++i)
        out[i] = std::abs(in[i]);
}

struct CtrlSmoothState {
    double anchorFrame = -1.0;
    uint8_t initialized = 0;
    float y = 0.0f;
    uint16_t markovCurrent = 0;
    uint32_t markovDraws = 0;
};

inline void evalCtrlSmooth(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;

    const uint16_t lane = c.u16(0);
    float* out = nullptr;
    const StreamRow* src = takeValueTransformInput(c, lane, out);
    if (src == nullptr)
        return;

    CtrlSmoothState& st = *(CtrlSmoothState*) c.state();
    if (st.anchorFrame != span.anchor) {
        st.anchorFrame = span.anchor;
        st.initialized = 0;
    }

    const float seconds = std::max(0.0f, c.f32(1));
    const float alpha = seconds <= 0.0f
        ? 1.0f
        : 1.0f - std::exp(-1.0f / (seconds * (float) c.sampleRate()));

    const float* in = src->data.data();
    for (int i = span.lo; i < span.hi; ++i) {
        if (! st.initialized) {
            st.y = in[i];
            st.initialized = 1;
        } else {
            st.y += alpha * (in[i] - st.y);
        }
        out[i] = st.y;
    }
}

inline float dynArgAt(ExecCtx& c, int immediatesOp, int lanesOp, int index, int frame)
{
    const float immediate = index < (int) c.arrayLenU16(immediatesOp)
        ? c.arrayF32At(immediatesOp, index) : 0.0f;
    if (index >= (int) c.arrayLenU8(lanesOp))
        return immediate;
    const uint16_t lane = c.arrayU16At(lanesOp, index);
    if (lane == 0xffff)
        return immediate;
    const StreamRow* row = c.previousStreamRow(lane, SignalType::Value);
    return (row != nullptr && frame >= 0 && frame < (int) row->data.size())
        ? row->data[(size_t) frame] : immediate;
}

inline bool markovDynValueEquals(ExecCtx& c, int a, int b, int frame)
{
    return dynArgAt(c, 6, 7, a, frame) == dynArgAt(c, 6, 7, b, frame);
}

inline uint16_t nextMarkovDynValueIndex(ExecCtx& c, CtrlSmoothState& st,
                                        uint16_t n, uint32_t seed, int frame)
{
    uint16_t count = 0;
    const int current = (int) (st.markovCurrent % n);
    for (uint16_t i = 0; i < n; ++i)
        if (markovDynValueEquals(c, current, (int) i, frame))
            ++count;
    if (count <= 1 && n > 1) {
        const uint16_t alternatives = (uint16_t) (n - 1);
        const float r = hash01(seed, ++st.markovDraws);
        uint16_t pick = (uint16_t) std::min<int>((int) alternatives - 1,
                                                 (int) (r * alternatives));
        for (uint16_t i = 0; i < n; ++i) {
            if ((int) i == current)
                continue;
            if (pick-- == 0)
                return i;
        }
    }
    if (count == 0)
        return (uint16_t) ((current + 1) % n);
    const float r = hash01(seed, ++st.markovDraws);
    const uint16_t pick = (uint16_t) std::min<int>((int) count - 1, (int) (r * count));
    uint16_t seen = 0;
    for (uint16_t i = 0; i < n; ++i) {
        if (! markovDynValueEquals(c, current, (int) i, frame))
            continue;
        if (seen++ == pick)
            return (uint16_t) ((i + 1u) % n);
    }
    return (uint16_t) ((current + 1) % n);
}

inline void evalCtrlDyn(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;

    const uint16_t lane = c.u16(0);
    const auto kind = (CtrlDynKind) c.u8(1);
    const uint8_t aux0 = c.u8(2);
    const uint8_t aux1 = c.u8(3);
    const uint32_t seed = c.u32(4);
    const uint8_t flags = c.u8(5);
    constexpr int kImmediatesOp = 6;
    constexpr int kLanesOp = 7;

    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;

    const bool offsetMode = (flags & 0x01) != 0;
    const bool perHit = (flags & 0x02) != 0;

    auto arg = [&] (int index, int frame) {
        return dynArgAt(c, kImmediatesOp, kLanesOp, index, frame);
    };

    switch (kind) {
        case CtrlDynKind::Lfo: {
            float* row = c.streamFor(lane, SignalType::Value,
                                     offsetMode ? PacketKind::Offset : PacketKind::Stream);
            if (row == nullptr) return;
            const bool bpmRel = (flags & 0x04) != 0;
            const double sr = c.sampleRate();
            const double tsFactor = c.scopeTimescale();
            const double beatAtAnchor = c.beatOfFrame(span.anchor);
            for (int i = span.lo; i < span.hi; ++i) {
                const double f = c.blockStartFrame() + i;
                const float rate = std::max(0.0f, arg(0, i));
                const float mn = arg(1, i);
                const float mx = arg(2, i);
                const float phase01 = arg(3, i);
                const double t = (f - span.anchor) / sr;
                const double cyc = bpmRel
                    ? (double) rate * (c.beatOfFrame(f) - beatAtAnchor) * tsFactor
                    : (double) rate * t;
                double phase = cyc + (double) phase01;
                phase -= std::floor(phase);
                float norm;
                switch (aux0) {
                    case 0:  norm = 0.5f + 0.5f * std::sin((float) (phase * 2.0 * 3.14159265358979)); break;
                    case 1:  norm = (float) (phase < 0.5 ? 2.0 * phase : 2.0 - 2.0 * phase); break;
                    case 2:  norm = (float) phase; break;
                    case 3:  norm = phase < 0.5 ? 1.0f : 0.0f; break;
                    case 4:  norm = hash01((uint64_t) c.instrIndex * 0xC0FFEEull,
                                           (uint64_t) std::floor(cyc)); break;
                    default: norm = 0.5f; break;
                }
                row[i] = mn + norm * (mx - mn);
            }
            return;
        }
        case CtrlDynKind::Auto: {
            float* row = c.streamFor(lane, SignalType::Value,
                                     offsetMode ? PacketKind::Offset : PacketKind::Stream);
            if (row == nullptr) return;
            const double dur = span.spanEnd - span.anchor;
            for (int i = span.lo; i < span.hi; ++i) {
                const double f = c.blockStartFrame() + i;
                float p = dur > 0.0 ? (float) ((f - span.anchor) / dur) : 0.0f;
                p = std::min(1.0f, std::max(0.0f, p));
                float t;
                switch (aux0) {
                    case 1:  t = p * p; break;
                    case 2:  t = std::sqrt(p); break;
                    case 3:  t = p < 0.5f ? 2.0f * p * p
                                          : 1.0f - std::pow(-2.0f * p + 2.0f, 2.0f) / 2.0f; break;
                    default: t = p; break;
                }
                const float start = arg(0, i);
                const float end = arg(1, i);
                row[i] = start + t * (end - start);
            }
            return;
        }
        case CtrlDynKind::Random: {
            float* row = c.streamFor(lane, SignalType::Value,
                                     offsetMode ? PacketKind::Offset : PacketKind::Stream);
            if (row == nullptr) return;
            float* slot = c.laneSlewSlot(lane);
            const uint64_t anchorKey = (uint64_t) llround(c.scopeAnchorFrame()) + 1u;
            for (int i = span.lo; i < span.hi; ++i) {
                const float mn = arg(0, i);
                const float mx = arg(1, i);
                const float slewBeats = std::max(0.0f, arg(2, i));
                const double tauFrames = (double) slewBeats * c.framesPerBeat();
                const float coeff = (slewBeats > 0.0f && tauFrames > 0.0)
                    ? (float) (1.0 - std::exp(-1.0 / tauFrames)) : 1.0f;
                float target = mn + hash01(seed, anchorKey) * (mx - mn);
                if (perHit) {
                    const uint64_t n = c.gateCountAt(c.blockStartFrame() + i);
                    target = mn + hash01(seed, n + 1u) * (mx - mn);
                }
                if (slot == nullptr) { row[i] = target; continue; }
                float last = *slot;
                if (! (last == last))
                    last = target;
                last += (target - last) * coeff;
                *slot = last;
                row[i] = last;
            }
            return;
        }
        case CtrlDynKind::Deviate: {
            float* row = c.streamFor(lane, SignalType::Value, PacketKind::Offset);
            if (row == nullptr) return;
            if (perHit) {
                for (int i = span.lo; i < span.hi; ++i) {
                    const uint64_t n = c.gateCountAt(c.blockStartFrame() + i);
                    row[i] = (n == 0) ? 0.0f : (hash01(seed, n) - 0.5f) * 2.0f * arg(0, i);
                }
                return;
            }
            const uint64_t anchorKey = (uint64_t) llround(c.scopeAnchorFrame()) + 1u;
            const float r = (hash01(seed, anchorKey) - 0.5f) * 2.0f;
            for (int i = span.lo; i < span.hi; ++i)
                row[i] = r * arg(0, i);
            return;
        }
        case CtrlDynKind::Bernoulli: {
            float* row = c.streamFor(lane, SignalType::Value,
                                     offsetMode ? PacketKind::Offset : PacketKind::Stream);
            if (row == nullptr) return;
            if (perHit) {
                for (int i = span.lo; i < span.hi; ++i) {
                    const uint64_t n = c.gateCountAt(c.blockStartFrame() + i);
                    row[i] = (n == 0 || hash01(seed, n) < arg(0, i)) ? arg(1, i) : arg(2, i);
                }
                return;
            }
            const uint64_t anchorKey = (uint64_t) llround(c.scopeAnchorFrame()) + 1u;
            const float r = hash01(seed, anchorKey);
            for (int i = span.lo; i < span.hi; ++i)
                row[i] = r < arg(0, i) ? arg(1, i) : arg(2, i);
            return;
        }
        case CtrlDynKind::Keytrack: {
            float* row = c.streamFor(lane, SignalType::Value, PacketKind::Stream);
            if (row == nullptr) return;
            for (int i = span.lo; i < span.hi; ++i) {
                const auto p = c.pitchAt(c.blockStartFrame() + i);
                const float midi = p.any ? noteToMidi(signalToNote(p.cur)) : 69.0f;
                float t = (midi - 24.0f) / 72.0f;
                t = std::min(1.0f, std::max(0.0f, t));
                const float mn = arg(0, i);
                const float mx = arg(1, i);
                row[i] = mn + t * (mx - mn);
            }
            return;
        }
        case CtrlDynKind::Accum: {
            float* row = c.streamFor(lane, SignalType::Value, PacketKind::Stream);
            if (row == nullptr) return;
            const uint64_t baseCount = c.gateCountAt(span.anchor - 0.5);
            for (int i = span.lo; i < span.hi; ++i) {
                const uint64_t n = c.gateCountAt(c.blockStartFrame() + i);
                const uint64_t k = n > baseCount ? n - baseCount : 0;
                row[i] = std::min(arg(2, i), arg(0, i) + (float) k * arg(1, i));
            }
            return;
        }
        case CtrlDynKind::Midi: {
            const float cc = c.machine.readMidiCC((int) aux0, (int) aux1);
            const PacketKind kind = offsetMode ? PacketKind::Offset : PacketKind::Stream;
            if (std::isnan(cc)) {
                c.streamStamp(lane, SignalType::Value, kind);
                return;
            }
            float* row = c.streamFor(lane, SignalType::Value, kind);
            if (row == nullptr) return;
            for (int i = span.lo; i < span.hi; ++i) {
                const float mn = arg(0, i);
                const float mx = arg(1, i);
                row[i] = mn + cc * (mx - mn);
            }
            return;
        }
        case CtrlDynKind::Preset: {
            float* row = c.streamFor(lane, SignalType::Value,
                                     offsetMode ? PacketKind::Offset : PacketKind::Stream);
            if (row == nullptr)
                return;
            const int stateCount = (int) aux0;
            if (stateCount <= 0)
                return;
            const bool interp = aux1 != 0;
            for (int i = span.lo; i < span.hi; ++i) {
                const float selector = std::max(0.0f, std::min(1.0f, arg(0, i)));
                if (interp && stateCount > 1) {
                    const float pos = selector * (float) (stateCount - 1);
                    const int lo = (int) std::floor(pos);
                    const int hi = std::min(stateCount - 1, lo + 1);
                    const float frac = pos - (float) lo;
                    const float a = arg(1 + lo, i);
                    const float b = arg(1 + hi, i);
                    row[i] = a + frac * (b - a);
                } else {
                    int idx = (int) std::floor(selector * (float) stateCount);
                    if (idx >= stateCount) idx = stateCount - 1;
                    row[i] = arg(1 + idx, i);
                }
            }
            return;
        }
        case CtrlDynKind::MarkovValue: {
            float* row = c.streamFor(lane, SignalType::Value,
                                     offsetMode ? PacketKind::Offset : PacketKind::Stream);
            if (row == nullptr)
                return;
            const uint16_t n = c.arrayLenU16(kImmediatesOp);
            if (n == 0)
                return;

            CtrlSmoothState& st = *(CtrlSmoothState*) c.state();
            if (! st.initialized) {
                st = CtrlSmoothState {};
                st.initialized = 1;
            }
            if (st.markovCurrent >= n)
                st.markovCurrent = 0;
            if (st.anchorFrame != span.anchor) {
                st.anchorFrame = span.anchor;
                const uint16_t idx = st.markovCurrent;
                st.y = dynArgAt(c, kImmediatesOp, kLanesOp, idx, span.lo);
                st.markovCurrent = nextMarkovDynValueIndex(c, st, n, seed, span.lo);
            }
            for (int i = span.lo; i < span.hi; ++i)
                row[i] = st.y;
            return;
        }
        case CtrlDynKind::Ad: {
            float* row = c.streamFor(lane, SignalType::Value,
                                     offsetMode ? PacketKind::Offset : PacketKind::Stream);
            if (row == nullptr) return;
            const double sr = c.sampleRate();
            for (int i = span.lo; i < span.hi; ++i) {
                const double f = c.blockStartFrame() + i;
                const double g = c.gateAnchorAt(f);
                const float mn = arg(2, i);
                const float mx = arg(3, i);
                if (g < 0.0 || g < span.anchor) { row[i] = mn; continue; }
                const double aSec = std::max(0.0f, arg(0, i));
                const double dSec = std::max(0.0f, arg(1, i));
                const double since = (f - g) / sr;
                float level = 0.0f;
                if (since < 0.0)               level = 0.0f;
                else if (since < aSec && aSec > 0.0)
                    level = 1.0f - std::exp(-5.0f * (float) (since / aSec));
                else if (since < aSec + dSec && dSec > 0.0)
                    level = std::exp(-5.0f * (float) ((since - aSec) / dSec));
                row[i] = mn + level * (mx - mn);
            }
            return;
        }
        case CtrlDynKind::Adsr: {
            const double sr = c.sampleRate();
            const double tailSec = std::max(0.0f, arg(3, span.lo));
            RenderSpan envSpan;
            if (! renderSpanOf(c, tailSec * sr + 1.0, envSpan))
                return;
            float* row = c.streamFor(lane, SignalType::Value,
                                     offsetMode ? PacketKind::Offset : PacketKind::Stream);
            if (row == nullptr) return;
            for (int i = envSpan.lo; i < envSpan.hi; ++i) {
                const double f = c.blockStartFrame() + i;
                const double g = c.gateAnchorAt(f);
                const float mn = arg(4, i);
                const float mx = arg(5, i);
                if (g < 0.0 || g < envSpan.anchor) { row[i] = mn; continue; }
                const double aSec = std::max(0.0f, arg(0, i));
                const double dSec = std::max(0.0f, arg(1, i));
                const float sus = arg(2, i);
                const double rSec = std::max(0.0f, arg(3, i));
                auto levelAt = [&] (double sinceSec) -> float {
                    if (sinceSec < 0.0) return 0.0f;
                    if (sinceSec < aSec && aSec > 0.0)
                        return 1.0f - std::exp(-5.0f * (float) (sinceSec / aSec));
                    if (sinceSec < aSec + dSec && dSec > 0.0)
                        return 1.0f - (1.0f - sus)
                             * (1.0f - std::exp(-5.0f * (float) ((sinceSec - aSec) / dSec)));
                    return sus;
                };
                const double off = c.findGateOffAfter(g);
                float level;
                if (f < off) {
                    level = levelAt((f - g) / sr);
                } else {
                    const double sinceRel = (f - off) / sr;
                    level = (rSec > 0.0 && sinceRel < rSec)
                        ? levelAt((off - g) / sr)
                              * std::exp(-5.0f * (float) (sinceRel / rSec))
                        : 0.0f;
                }
                row[i] = mn + level * (mx - mn);
            }
            return;
        }
        case CtrlDynKind::Clip:
        case CtrlDynKind::Invert:
        case CtrlDynKind::Scale:
        case CtrlDynKind::Offset:
        case CtrlDynKind::Gain:
        case CtrlDynKind::Quantize:
        case CtrlDynKind::Smooth: {
            float* out = nullptr;
            const StreamRow* src = takeValueTransformInput(c, lane, out);
            if (src == nullptr || out == nullptr)
                return;
            const float* in = src->data.data();
            if (kind == CtrlDynKind::Smooth) {
                CtrlSmoothState& st = *(CtrlSmoothState*) c.state();
                if (st.anchorFrame != span.anchor) {
                    st.anchorFrame = span.anchor;
                    st.initialized = 0;
                }
                for (int i = span.lo; i < span.hi; ++i) {
                    const float seconds = std::max(0.0f, arg(0, i));
                    const float alpha = seconds <= 0.0f
                        ? 1.0f
                        : 1.0f - std::exp(-1.0f / (seconds * (float) c.sampleRate()));
                    if (! st.initialized) {
                        st.y = in[i];
                        st.initialized = 1;
                    } else {
                        st.y += alpha * (in[i] - st.y);
                    }
                    out[i] = st.y;
                }
                return;
            }
            for (int i = span.lo; i < span.hi; ++i) {
                if (kind == CtrlDynKind::Clip) {
                    float mn = arg(0, i), mx = arg(1, i);
                    if (mn > mx) std::swap(mn, mx);
                    out[i] = std::max(mn, std::min(mx, in[i]));
                } else if (kind == CtrlDynKind::Invert) {
                    out[i] = 2.0f * arg(0, i) - in[i];
                } else if (kind == CtrlDynKind::Scale) {
                    const float mn = arg(0, i), mx = arg(1, i);
                    out[i] = mn + in[i] * (mx - mn);
                } else if (kind == CtrlDynKind::Offset) {
                    out[i] = in[i] + arg(0, i);
                } else if (kind == CtrlDynKind::Gain) {
                    out[i] = in[i] * arg(0, i);
                } else {
                    out[i] = quantizeSignalToStep(in[i], arg(0, i));
                }
            }
            return;
        }
    }
}

// AD: attack 1-exp(-5p), decay exp(-5p), back to min after a+d. Policy:
// scope-anchored (one envelope per scope entry) or note-retriggered.
inline void evalCtrlAd(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;
    float* row = c.streamFor(c.u16(0), SignalType::Value,
                             c.u8(6) ? PacketKind::Offset : PacketKind::Stream);
    // F-071 T-495 — per-arg unit domain (mask bit0 attack, bit1 decay). Set =
    // absolute seconds (immune to tempo + timescale); clear = BPM-relative
    // beats (× spb tracks tempo, ÷ scopeTimescale stretches with the scope).
    const uint8_t mask = c.u8(7);
    const double spb  = 60.0 / c.machine.tempoBpm();
    const double ts   = c.scopeTimescale();
    const double aSec = (mask & 0x1) ? (double) c.f32(1) : (double) c.f32(1) * spb / ts;
    const double dSec = (mask & 0x2) ? (double) c.f32(2) : (double) c.f32(2) * spb / ts;
    const float  mn   = c.f32(3), mx = c.f32(4);
    const bool retrigger = c.u8(5) != 0;
    const double sr = c.sampleRate();
    for (int i = span.lo; i < span.hi; ++i) {
        const double f = c.blockStartFrame() + i;
        double anchor = span.anchor;
        if (retrigger) {
            // Only gates within the op's scope drive it (T-474 ruling).
            const double g = c.gateAnchorAt(f);
            if (g < 0.0 || g < span.anchor) { row[i] = mn; continue; }
            anchor = g;
        }
        const double since = (f - anchor) / sr;
        float level = 0.0f;
        if (since < 0.0)               level = 0.0f;
        else if (since < aSec && aSec > 0.0)
            level = 1.0f - std::exp(-5.0f * (float) (since / aSec));
        else if (since < aSec + dSec && dSec > 0.0)
            level = std::exp(-5.0f * (float) ((since - aSec) / dSec));
        row[i] = mn + level * (mx - mn);
    }
}

// ADSR. Default policy (T-474 ruling, Neo s493): RETRIGGER — one envelope
// per gate within the op's scope; attack at each gate-on, release starting
// at each gate-off (gates from before the scope don't drive it; the release
// tail rings past the gate, and past the scope via the render-span tail).
// The scope-anchored variant (gate-on = scope entry, gate-off = scope exit)
// stays in the bytecode, unexposed until a surface wants it.
inline void evalCtrlAdsr(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    // F-071 T-495 — per-arg unit domain (mask bit0 attack, bit1 decay, bit3
    // release). Set = absolute seconds (immune); clear = BPM-relative beats
    // (× spb tracks tempo, ÷ scopeTimescale stretches with the scope).
    const uint8_t mask = c.u8(9);
    const double spb  = 60.0 / c.machine.tempoBpm();
    const double ts   = c.scopeTimescale();
    const double rSec = (mask & 0x8) ? (double) c.f32(4) : (double) c.f32(4) * spb / ts;
    const double sr   = c.sampleRate();
    RenderSpan span;
    if (! renderSpanOf(c, rSec * sr + 1.0, span))
        return;
    float* row = c.streamFor(c.u16(0), SignalType::Value,
                             c.u8(7) ? PacketKind::Offset : PacketKind::Stream);
    const double aSec = (mask & 0x1) ? (double) c.f32(1) : (double) c.f32(1) * spb / ts;
    const double dSec = (mask & 0x2) ? (double) c.f32(2) : (double) c.f32(2) * spb / ts;
    const float  sus  = c.f32(3);
    const float  mn   = c.f32(5), mx = c.f32(6);
    const bool retrigger = c.u8(8) != 0;

    auto levelAt = [&] (double sinceSec) -> float {
        if (sinceSec < 0.0) return 0.0f;
        if (sinceSec < aSec && aSec > 0.0)
            return 1.0f - std::exp(-5.0f * (float) (sinceSec / aSec));
        if (sinceSec < aSec + dSec && dSec > 0.0)
            return 1.0f - (1.0f - sus)
                 * (1.0f - std::exp(-5.0f * (float) ((sinceSec - aSec) / dSec)));
        return sus;
    };

    if (retrigger) {
        for (int i = span.lo; i < span.hi; ++i) {
            const double f = c.blockStartFrame() + i;
            const double g = c.gateAnchorAt(f);
            if (g < 0.0 || g < span.anchor) { row[i] = mn; continue; }
            const double off = c.findGateOffAfter(g);
            float level;
            if (f < off) {
                level = levelAt((f - g) / sr);
            } else {
                const double sinceRel = (f - off) / sr;
                level = (rSec > 0.0 && sinceRel < rSec)
                    ? levelAt((off - g) / sr)
                          * std::exp(-5.0f * (float) (sinceRel / rSec))
                    : 0.0f;
            }
            row[i] = mn + level * (mx - mn);
        }
        return;
    }

    const float levelAtRelease = levelAt((span.spanEnd - span.anchor) / sr);

    for (int i = span.lo; i < span.hi; ++i) {
        const double f = c.blockStartFrame() + i;
        float level;
        if (f < span.spanEnd) {
            level = levelAt((f - span.anchor) / sr);
        } else {
            const double sinceRel = (f - span.spanEnd) / sr;
            level = (rSec > 0.0 && sinceRel < rSec)
                ? levelAtRelease * std::exp(-5.0f * (float) (sinceRel / rSec))
                : 0.0f;
        }
        row[i] = mn + level * (mx - mn);
    }
}

inline void evalCtrlAuto(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;
    float* row = c.streamFor(c.u16(0), SignalType::Value,
                             c.u8(4) ? PacketKind::Offset : PacketKind::Stream);
    const float start = c.f32(1), end = c.f32(2);
    const uint8_t curve = c.u8(3);
    const double dur = span.spanEnd - span.anchor;
    for (int i = span.lo; i < span.hi; ++i) {
        const double f = c.blockStartFrame() + i;
        float p = dur > 0.0 ? (float) ((f - span.anchor) / dur) : 0.0f;
        p = std::min(1.0f, std::max(0.0f, p));
        float t;
        switch (curve) {
            case 1:  t = p * p; break;
            case 2:  t = std::sqrt(p); break;
            case 3:  t = p < 0.5f ? 2.0f * p * p
                                  : 1.0f - std::pow(-2.0f * p + 2.0f, 2.0f) / 2.0f; break;
            default: t = p; break;
        }
        row[i] = start + t * (end - start);
    }
}

// Per-gate-on draw helpers: value keyed by (seed, cumulative gate count) —
// deterministic and block-size independent because Render sees the whole
// segment's gates.
inline void evalCtrlRandom(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;
    const uint16_t lane = c.u16(0);
    float* row = c.streamFor(lane, SignalType::Value,
                             c.u8(4) ? PacketKind::Offset : PacketKind::Stream);
    const float mn = c.f32(1), mx = c.f32(2);
    const uint32_t seed = c.u32(3);
    const bool perHit = c.u8(5) != 0;
    const float slewBeats = c.f32(6);   // de-click time (beats; see compiler)
    // B-279 (SYNTAX_V1 §~random "per step" + §5.3 "notes inside a scope
    // sample the op; they never retrigger it"): the draw clocks on the
    // SCOPE ANCHOR — one value per scope iteration (per step for a
    // step-written route, per loop for a sequence-written one), re-drawn
    // when the scope re-enters. The gate-clocked redraw was implementation
    // drift (notes retriggering the op). Per-hit draws are opted into by
    // WRITTEN POSITION (T-484, §5 one-law): right of a compile-time
    // expansion (arp/strum) the route gets its own scope per sub-step;
    // right of a machine train (ratchet/buzz/…) the compiler sets the
    // perHit operand and the roll keys on the gate count instead.
    const uint64_t anchorKey = (uint64_t) llround(c.scopeAnchorFrame()) + 1u;
    const float anchorTarget = mn + hash01(seed, anchorKey) * (mx - mn);

    // Slew de-click: a one-pole toward the drawn target whose running value
    // lives on the LANE (shared across instructions targeting the same
    // param), so per-step routes BLEND from the param's previous value
    // instead of each snapping to its own draw — the snap was the loud
    // cutoff "ping". slot==NaN → first write ever → snap (coeff path below
    // collapses to the target). slewBeats<=0 → coeff 1 → snappy block-fill,
    // and we still park the lane value so a later slewed route blends from
    // it. tauFrames from the LIVE tempo (slewBeats came through the standard
    // time protocol, exactly like ~glide).
    float* slot = c.laneSlewSlot(lane);
    const double tauFrames = (double) slewBeats * c.framesPerBeat();
    const float coeff = (slewBeats > 0.0f && tauFrames > 0.0)
        ? (float) (1.0 - std::exp(-1.0 / tauFrames)) : 1.0f;
    for (int i = span.lo; i < span.hi; ++i) {
        float target = anchorTarget;
        if (perHit) {
            const uint64_t n = c.gateCountAt(c.blockStartFrame() + i);
            target = mn + hash01(seed, n + 1u) * (mx - mn);
        }
        if (slot == nullptr) { row[i] = target; continue; }
        float last = *slot;
        if (! (last == last))               // NaN → first write snaps in
            last = target;
        last += (target - last) * coeff;     // coeff==1 ⇒ last == target (snappy)
        *slot = last;
        row[i] = last;
    }
}

inline void evalCtrlDeviate(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;
    float* row = c.streamFor(c.u16(0), SignalType::Value, PacketKind::Offset);
    const float amount = c.f32(1);
    const uint32_t seed = c.u32(2);
    // B-286(a): anchor-clocked by default like ~random — one offset per
    // scope iteration (SYNTAX_V1 line 648 "per-step variation"); the
    // unconditional per-gate re-roll was spec drift. Per-hit by position
    // (perHit operand), same as evalCtrlRandom above.
    if (c.u8(3)) {
        for (int i = span.lo; i < span.hi; ++i) {
            const uint64_t n = c.gateCountAt(c.blockStartFrame() + i);
            row[i] = (n == 0) ? 0.0f : (hash01(seed, n) - 0.5f) * 2.0f * amount;
        }
        return;
    }
    const uint64_t anchorKey = (uint64_t) llround(c.scopeAnchorFrame()) + 1u;
    const float v = (hash01(seed, anchorKey) - 0.5f) * 2.0f * amount;
    for (int i = span.lo; i < span.hi; ++i)
        row[i] = v;
}

inline void evalCtrlBernoulliStream(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;
    float* row = c.streamFor(c.u16(0), SignalType::Value,
                             c.u8(5) ? PacketKind::Offset : PacketKind::Stream);
    const float weight = c.f32(1), valA = c.f32(2), valB = c.f32(3);
    const uint32_t seed = c.u32(4);
    // B-286(a): one A/B pick per scope iteration by default; per-hit picks
    // by written position (perHit operand), same as evalCtrlRandom above.
    if (c.u8(6)) {
        for (int i = span.lo; i < span.hi; ++i) {
            const uint64_t n = c.gateCountAt(c.blockStartFrame() + i);
            row[i] = (n == 0 || hash01(seed, n) < weight) ? valA : valB;
        }
        return;
    }
    const uint64_t anchorKey = (uint64_t) llround(c.scopeAnchorFrame()) + 1u;
    const float v = (hash01(seed, anchorKey) < weight) ? valA : valB;
    for (int i = span.lo; i < span.hi; ++i)
        row[i] = v;
}

// Keytrack: last note through the MIDI 24..96 window (GeneratorEval port;
// no note yet behaves as A4 = MIDI 69).
inline void evalCtrlKeytrack(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;
    float* row = c.streamFor(c.u16(0), SignalType::Value, PacketKind::Stream);
    const float mn = c.f32(1), mx = c.f32(2);
    for (int i = span.lo; i < span.hi; ++i) {
        const auto p = c.pitchAt(c.blockStartFrame() + i);
        const float midi = p.any ? noteToMidi(signalToNote(p.cur)) : 69.0f;
        float t = (midi - 24.0f) / 72.0f;
        t = std::min(1.0f, std::max(0.0f, t));
        row[i] = mn + t * (mx - mn);
    }
}

// Accum: start + n*increment per gate-on within the loop iteration,
// clamped to the ceiling; resets each iteration.
struct AccumOpState {
    uint64_t iterationPlus1 = 0;   // 0 = never seen
    uint64_t baseCount      = 0;
};

inline void evalAccum(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;
    AccumOpState& st = *(AccumOpState*) c.state();
    if (st.iterationPlus1 != c.iteration + 1) {
        st.iterationPlus1 = c.iteration + 1;
        st.baseCount = c.gateCountAt(span.anchor - 0.5);
    }
    float* row = c.streamFor(c.u16(0), SignalType::Value, PacketKind::Stream);
    const float start = c.f32(1), inc = c.f32(2), ceiling = c.f32(3);
    for (int i = span.lo; i < span.hi; ++i) {
        const uint64_t n = c.gateCountAt(c.blockStartFrame() + i);
        const uint64_t k = n > st.baseCount ? n - st.baseCount : 0;
        row[i] = std::min(ceiling, start + (float) k * inc);
    }
}

// CTRL_MIDI (B-275): a host-fed CC value rendered as a machine stream.
// The CC VALUE arrives from outside (Machine::setMidiCCSource — the host
// owns the matrix, NaN = never received); the ROUTE renders here so the
// scope rule clips it like every other op — step-written = step-bounded,
// sequence-written = loop-wide, released past its span. CC is block-
// constant (one relaxed atomic read per eval).
inline void evalCtrlMidi(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;
    const float cc = c.machine.readMidiCC((int) c.u8(1), (int) c.u8(2));
    const uint16_t lane = c.u16(0);
    const PacketKind kind = c.u8(5) ? PacketKind::Offset : PacketKind::Stream;
    if (std::isnan(cc)) {
        c.streamStamp(lane, SignalType::Value, kind);
        return;   // no CC yet: row stays inactive, route stays released
    }
    float* row = c.streamFor(lane, SignalType::Value, kind, true);
    const float mn = c.f32(3), mx = c.f32(4);
    const float v = mn + cc * (mx - mn);
    for (int i = span.lo; i < span.hi; ++i)
        row[i] = v;
}

// Glide: a pitch-typed stream ramping from the previous note to the
// current one over the glide time (the rebuilt form of the old GLIDE op —
// pitch transition is a stream, not a hit property).
inline void evalGlide(ExecCtx& c)
{
    if (c.phase != ExecCtx::Phase::Render)
        return;
    RenderSpan span;
    if (! renderSpanOf(c, 0.0, span))
        return;
    float* row = c.streamFor(0, SignalType::Pitch, PacketKind::Stream);
    const double glideFrames = (double) c.f32(0) * c.framesPerBeat();
    for (int i = span.lo; i < span.hi; ++i) {
        const double f = c.blockStartFrame() + i;
        const auto p = c.pitchAt(f);
        if (! p.any) { row[i] = 0.0f; continue; }
        if (glideFrames > 0.0 && f < p.curStartFrame + glideFrames) {
            const float t = (float) ((f - p.curStartFrame) / glideFrames);
            row[i] = p.prev + (p.cur - p.prev) * t;
        } else {
            row[i] = p.cur;
        }
    }
}

// ── The table ─────────────────────────────────────────────────────

inline const OpSpec kOpTable[] = {
    { opcode(Op::Halt), "HALT", {},                              0, Advance::None, Lifecycle::Stateless, &evalHalt },
    { opcode(Op::Loop), "LOOP", { 1, { Operand::F32 } },         0, Advance::None, Lifecycle::Stateless, &evalLoop },
    { opcode(Op::Step), "STEP", { 2, { Operand::F32, Operand::F32 } }, 0, Advance::None, Lifecycle::Stateless, &evalStep },
    { opcode(Op::Note), "NOTE", { 2, { Operand::F32, Operand::F32 } }, 0, Advance::None, Lifecycle::Stateless, &evalNote },
    { opcode(Op::NoteExpr), "NOTE_EXPR", { 4, { Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::F32, Operand::F32 } }, 0, Advance::None, Lifecycle::Stateless, &evalNoteExpr },
    { opcode(Op::Rest), "REST", {},                              0, Advance::None, Lifecycle::Stateless, &evalRest },

    { opcode(Op::CondMod),        "COND_MOD",          { 1, { Operand::U16 } },        0, Advance::None, Lifecycle::Stateless, &evalCondMod },
    { opcode(Op::CondLoopEq),     "COND_LOOP_EQ",      { 1, { Operand::U16 } },        0, Advance::None, Lifecycle::Stateless, &evalCondLoopEq },
    { opcode(Op::CondFirst),      "COND_FIRST",        { 1, { Operand::U16 } },        0, Advance::None, Lifecycle::Stateless, &evalCondFirst },
    { opcode(Op::CondEven),       "COND_EVEN",         { 1, { Operand::U16 } },        0, Advance::None, Lifecycle::Stateless, &evalCondEven },
    { opcode(Op::CondOdd),        "COND_ODD",          { 1, { Operand::U16 } },        0, Advance::None, Lifecycle::Stateless, &evalCondOdd },
    { opcode(Op::CondPrime),      "COND_PRIME",        { 1, { Operand::U16 } },        0, Advance::None, Lifecycle::Stateless, &evalCondPrime },
    { opcode(Op::CondNotFirst),   "COND_NOT_FIRST",    { 1, { Operand::U16 } },        0, Advance::None, Lifecycle::Stateless, &evalCondNotFirst },
    { opcode(Op::CondFib),        "COND_FIB",          { 1, { Operand::U16 } },        0, Advance::None, Lifecycle::Stateless, &evalCondFib },
    { opcode(Op::CondAfter),      "COND_AFTER",        { 1, { Operand::U16 } },        0, Advance::None, Lifecycle::Stateless, &evalCondAfter },
    { opcode(Op::CondLoopSet),    "COND_LOOP_SET",     { 1, { Operand::U16ArrayU8 } }, 0, Advance::None, Lifecycle::Stateless, &evalCondLoopSet },
    { opcode(Op::CondNotLoopSet), "COND_NOT_LOOP_SET", { 1, { Operand::U16ArrayU8 } }, 0, Advance::None, Lifecycle::Stateless, &evalCondNotLoopSet },
    { opcode(Op::CondPrevious),   "COND_PREVIOUS",     {},                             0, Advance::None, Lifecycle::Stateless, &evalCondPrevious },
    { opcode(Op::CondNotPrevious), "COND_NOT_PREVIOUS", {},                            0, Advance::None, Lifecycle::Stateless, &evalCondNotPrevious },
    { opcode(Op::CondExpr),       "COND_EXPR",         { 2, { Operand::U16ArrayU8, Operand::F32ArrayU16 } }, 0, Advance::None, Lifecycle::Stateless, &evalCondExpr },

    { opcode(Op::Prob),      "PROB",      { 2, { Operand::F32, Operand::U32 } },              0, Advance::None, Lifecycle::Stateless, &evalProb },
    { opcode(Op::ProbDyn),   "PROB_DYN",  { 3, { Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U32 } }, 0, Advance::None, Lifecycle::Stateless, &evalProbDyn },
    { opcode(Op::Bernoulli),    "BERNOULLI",     { 3, { Operand::F32, Operand::U8, Operand::U32 } }, 0, Advance::None, Lifecycle::Stateless, &evalBernoulli },
    { opcode(Op::BernoulliDyn), "BERNOULLI_DYN", { 4, { Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U8, Operand::U32 } }, 0, Advance::None, Lifecycle::Stateless, &evalBernoulliDyn },
    { opcode(Op::SelectDyn),    "SELECT_DYN",    { 3, { Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U16ArrayU8 } }, 0, Advance::None, Lifecycle::Stateless, &evalSelectDyn },

    { opcode(Op::Ratchet),        "RATCHET",         { 1, { Operand::U32 } },                      (uint16_t) sizeof(TrainState), Advance::OnTrigger, Lifecycle::Scope, &evalRatchet },
    { opcode(Op::RatchetPitched), "RATCHET_PITCHED", { 2, { Operand::U32, Operand::F32ArrayU16 } },(uint16_t) sizeof(TrainState), Advance::OnTrigger, Lifecycle::Scope, &evalRatchetPitched },
    { opcode(Op::Flam),           "FLAM",            { 2, { Operand::F32, Operand::F32 } },        0,                             Advance::None,      Lifecycle::Stateless, &evalFlam },
    { opcode(Op::Buzz),           "BUZZ",            { 2, { Operand::F32, Operand::F32 } },        (uint16_t) sizeof(TrainState), Advance::OnTrigger, Lifecycle::Scope, &evalBuzz },
    { opcode(Op::Bounce),         "BOUNCE",          { 3, { Operand::F32, Operand::F32, Operand::U8 } },                      (uint16_t) sizeof(TrainState), Advance::OnTrigger, Lifecycle::Scope, &evalBounce },
    { opcode(Op::Geiger),         "GEIGER",          { 2, { Operand::F32, Operand::U32 } },        (uint16_t) sizeof(TrainState), Advance::OnTrigger, Lifecycle::Scope, &evalGeiger },
    { opcode(Op::GateLen),        "GATE_LEN",        { 1, { Operand::F32 } },                      0,                             Advance::None,      Lifecycle::Stateless, &evalGateLen },
    { opcode(Op::Deviate),        "DEVIATE",         { 2, { Operand::F32, Operand::U32 } },        0,                             Advance::None,      Lifecycle::Stateless, &evalDeviate },
    { opcode(Op::GenOperand),     "GEN_OPERAND",     { 6, { Operand::U8, Operand::U8, Operand::F32, Operand::F32, Operand::F32, Operand::U32 } }, 0, Advance::None, Lifecycle::Stateless, &evalGenOperand },
    { opcode(Op::GenOperandDyn),  "GEN_OPERAND_DYN", { 9, { Operand::U8, Operand::U8, Operand::U32, Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U16ArrayU8, Operand::F32ArrayU16 } }, 0, Advance::None, Lifecycle::Stateless, &evalGenOperandDyn },

    { opcode(Op::NoteChord), "NOTE_CHORD", { 2, { Operand::F32ArrayU16, Operand::F32 } },       0, Advance::None, Lifecycle::Stateless, &evalNoteChord },
    { opcode(Op::GateOnly),  "GATE_ONLY",  { 1, { Operand::F32 } },                             0, Advance::None, Lifecycle::Stateless, &evalGateOnly },
    { opcode(Op::PitchSet),  "PITCH_SET",  { 1, { Operand::F32 } },                             0, Advance::None, Lifecycle::Stateless, &evalPitchSet },
    { opcode(Op::ParamLock), "PARAM_LOCK", { 3, { Operand::U16, Operand::F32, Operand::U8 } }, 0, Advance::None, Lifecycle::Stateless, &evalParamLock },
    { opcode(Op::ParamLockTyped), "PARAM_LOCK_TYPED", { 5, { Operand::U16, Operand::F32, Operand::U8, Operand::F32, Operand::U8 } }, 0, Advance::None, Lifecycle::Stateless, &evalParamLockTyped },
    { opcode(Op::LocalOutput), "LOCAL_OUTPUT", { 4, { Operand::U32, Operand::U32, Operand::U8, Operand::F32 } }, 0, Advance::None, Lifecycle::Stateless, &evalLocalOutput },
    { opcode(Op::LocalGateOnly), "LOCAL_GATE_ONLY", { 3, { Operand::U32, Operand::U32, Operand::F32 } }, 0, Advance::None, Lifecycle::Stateless, &evalLocalGateOnly },
    { opcode(Op::MarkovEvent), "MARKOV_EVENT", { 6, { Operand::F32ArrayU16, Operand::F32ArrayU16, Operand::U16, Operand::F32ArrayU16, Operand::U8, Operand::U32 } }, (uint16_t) sizeof(MarkovEventState), Advance::None, Lifecycle::Free, &evalMarkovEvent },
    { opcode(Op::MarkovGate),  "MARKOV_GATE",  { 3, { Operand::F32ArrayU16, Operand::F32ArrayU16, Operand::U32 } }, (uint16_t) sizeof(MarkovEventState), Advance::None, Lifecycle::Free, &evalMarkovGate },

    { opcode(Op::ScopeStart), "SCOPE_START", { 2, { Operand::F32, Operand::F32 } },                            0, Advance::None, Lifecycle::Scope,     &evalScopeStart },
    { opcode(Op::ScopeEnd),   "SCOPE_END",   {},                                                               0, Advance::None, Lifecycle::Scope,     &evalScopeEnd },
    { opcode(Op::Transpose),  "TRANSPOSE",   { 1, { Operand::F32 } },                                          0, Advance::None, Lifecycle::Scope,     &evalTranspose },
    { opcode(Op::TransposeExpr), "TRANSPOSE_EXPR", { 3, { Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::F32 } }, 0, Advance::None, Lifecycle::Scope, &evalTransposeExpr },
    { opcode(Op::Groove),     "GROOVE",      { 4, { Operand::U8, Operand::F32, Operand::F32, Operand::F32 } }, 0, Advance::None, Lifecycle::Scope,     &evalGroove },
    { opcode(Op::GrooveExpr), "GROOVE_EXPR", { 7, { Operand::U8, Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U16ArrayU8, Operand::F32ArrayU16 } }, 0, Advance::None, Lifecycle::Scope, &evalGrooveExpr },
    { opcode(Op::Reverse),    "REVERSE",     {},                                                               0, Advance::None, Lifecycle::Scope,     &evalReverse },
    { opcode(Op::Rotate),     "ROTATE",      { 1, { Operand::I16 } },                                          0, Advance::None, Lifecycle::Scope,     &evalRotate },
    { opcode(Op::RotateExpr), "ROTATE_EXPR", { 2, { Operand::U16ArrayU8, Operand::F32ArrayU16 } },             0, Advance::None, Lifecycle::Scope,     &evalRotateExpr },
    { opcode(Op::Shuffle),    "SHUFFLE",     { 1, { Operand::U32 } },                                          0, Advance::None, Lifecycle::Scope,     &evalShuffle },
    { opcode(Op::Invert),     "INVERT",      { 2, { Operand::F32, Operand::U8 } },                             0, Advance::None, Lifecycle::Scope,     &evalInvert },
    { opcode(Op::InvertExpr), "INVERT_EXPR", { 2, { Operand::U16ArrayU8, Operand::F32ArrayU16 } },             0, Advance::None, Lifecycle::Scope,     &evalInvertExpr },
    { opcode(Op::Timescale),  "TIMESCALE",   { 1, { Operand::F32 } },                                          0, Advance::None, Lifecycle::Scope,     &evalTimescale },
    { opcode(Op::TimescaleMod), "TIMESCALE_MOD", { 4, { Operand::U8, Operand::F32, Operand::F32, Operand::F32 } }, 0, Advance::None, Lifecycle::Scope, &evalTimescaleMod },
    { opcode(Op::TimescaleExpr), "TIMESCALE_EXPR", { 3, { Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U8 } }, 0, Advance::None, Lifecycle::Scope, &evalTimescaleExpr },
    { opcode(Op::ScopeBpm),   "SCOPE_BPM",   { 1, { Operand::F32 } },                                          0, Advance::None, Lifecycle::Scope,     &evalScopeBpm },
    { opcode(Op::Scale),      "SCALE",       { 2, { Operand::F32, Operand::F32ArrayU16 } },                    0, Advance::None, Lifecycle::Scope,     &evalScale },
    { opcode(Op::ScaleExpr),  "SCALE_EXPR",  { 3, { Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::F32ArrayU16 } }, 0, Advance::None, Lifecycle::Scope, &evalScaleExpr },
    { opcode(Op::ScaleGen),   "SCALE_GEN",   { 6, { Operand::F32, Operand::U8, Operand::F32, Operand::U32, Operand::U8, Operand::F32ArrayU16 } }, 0, Advance::None, Lifecycle::Scope, &evalScaleGen },
    { opcode(Op::ScaleGenDyn), "SCALE_GEN_DYN", { 7, { Operand::F32, Operand::U8, Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U32, Operand::U8, Operand::F32ArrayU16 } }, 0, Advance::None, Lifecycle::Scope, &evalScaleGenDyn },
    { opcode(Op::ScaleGenRootDyn), "SCALE_GEN_ROOT_DYN", { 8, { Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U8, Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U32, Operand::U8, Operand::F32ArrayU16 } }, 0, Advance::None, Lifecycle::Scope, &evalScaleGenRootDyn },
    { opcode(Op::Grid),       "GRID",        { 2, { Operand::F32, Operand::F32 } },                            0, Advance::None, Lifecycle::Scope,     &evalGrid },
    { opcode(Op::GridExpr),   "GRID_EXPR",   { 4, { Operand::U16ArrayU8, Operand::F32ArrayU16, Operand::U16ArrayU8, Operand::F32ArrayU16 } }, 0, Advance::None, Lifecycle::Scope, &evalGridExpr },
    { opcode(Op::Sort),       "SORT",        { 1, { Operand::U8 } },                                           0, Advance::None, Lifecycle::Scope,     &evalSort },
    { opcode(Op::StepAbs),    "STEP_ABS",    { 2, { Operand::F32, Operand::F32 } },                            0, Advance::None, Lifecycle::Stateless, &evalStepAbs },
    { opcode(Op::RepeatExpr), "REPEAT_EXPR", { 2, { Operand::U16ArrayU8, Operand::F32ArrayU16 } },              0, Advance::None, Lifecycle::Stateless, &evalRepeatExpr },
    { opcode(Op::ScopeRepeatExpr), "SCOPE_REPEAT_EXPR", { 2, { Operand::U16ArrayU8, Operand::F32ArrayU16 } },   0, Advance::None, Lifecycle::Scope,     &evalScopeRepeatExpr },
    { opcode(Op::ScopeFitExpr), "SCOPE_FIT_EXPR", { 2, { Operand::U16ArrayU8, Operand::F32ArrayU16 } },         0, Advance::None, Lifecycle::Scope,     &evalScopeFitExpr },

    // Trailing U8 on the range-taking ctrl routes = offsetMode (B-269 +
    // SPEC-014 lock layering): 0 abs (Stream-kind base), 1 % (Offset-kind
    // delta over the base). Deviate is inherently a delta (always Offset).
    { opcode(Op::CtrlLfo),       "CTRL_LFO",       { 8, { Operand::U16, Operand::U8, Operand::F32, Operand::F32, Operand::F32, Operand::U8, Operand::F32, Operand::U8 } },  0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlLfo },
    { opcode(Op::CtrlAd),        "CTRL_AD",        { 8, { Operand::U16, Operand::F32, Operand::F32, Operand::F32, Operand::F32, Operand::U8, Operand::U8, Operand::U8 } },  0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlAd },
    { opcode(Op::CtrlAdsr),      "CTRL_ADSR",      { 10, { Operand::U16, Operand::F32, Operand::F32, Operand::F32, Operand::F32, Operand::F32, Operand::F32, Operand::U8, Operand::U8, Operand::U8 } },0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlAdsr },
    { opcode(Op::CtrlAuto),      "CTRL_AUTO",      { 5, { Operand::U16, Operand::F32, Operand::F32, Operand::U8, Operand::U8 } },                              0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlAuto },
    { opcode(Op::CtrlRandom),    "CTRL_RANDOM",    { 7, { Operand::U16, Operand::F32, Operand::F32, Operand::U32, Operand::U8, Operand::U8, Operand::F32 } }, 0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlRandom },
    { opcode(Op::CtrlDeviate),   "CTRL_DEVIATE",   { 4, { Operand::U16, Operand::F32, Operand::U32, Operand::U8 } },                                           0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlDeviate },
    { opcode(Op::CtrlBernoulli), "CTRL_BERNOULLI", { 7, { Operand::U16, Operand::F32, Operand::F32, Operand::F32, Operand::U32, Operand::U8, Operand::U8 } }, 0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlBernoulliStream },
    { opcode(Op::CtrlKeytrack),  "CTRL_KEYTRACK",  { 3, { Operand::U16, Operand::F32, Operand::F32 } },                                                        0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlKeytrack },
    { opcode(Op::Accum),         "ACCUM",          { 4, { Operand::U16, Operand::F32, Operand::F32, Operand::F32 } }, (uint16_t) sizeof(AccumOpState),            Advance::PerFrame, Lifecycle::Scope, &evalAccum },
    { opcode(Op::Glide),         "GLIDE",          { 1, { Operand::F32 } },                                                                                    0, Advance::PerFrame, Lifecycle::Scope, &evalGlide },
    { opcode(Op::CtrlMidi),      "CTRL_MIDI",      { 6, { Operand::U16, Operand::U8, Operand::U8, Operand::F32, Operand::F32, Operand::U8 } },                            0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlMidi },
    { opcode(Op::CtrlClip),      "CTRL_CLIP",      { 3, { Operand::U16, Operand::F32, Operand::F32 } },                                                        0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlClip },
    { opcode(Op::CtrlInvert),    "CTRL_INVERT",    { 2, { Operand::U16, Operand::F32 } },                                                                      0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlInvert },
    { opcode(Op::CtrlScale),     "CTRL_SCALE",     { 3, { Operand::U16, Operand::F32, Operand::F32 } },                                                        0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlScale },
    { opcode(Op::CtrlOffset),    "CTRL_OFFSET",    { 2, { Operand::U16, Operand::F32 } },                                                                      0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlOffset },
    { opcode(Op::CtrlGain),      "CTRL_GAIN",      { 2, { Operand::U16, Operand::F32 } },                                                                      0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlGain },
    { opcode(Op::CtrlAbs),       "CTRL_ABS",       { 1, { Operand::U16 } },                                                                                    0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlAbs },
    { opcode(Op::CtrlSmooth),    "CTRL_SMOOTH",    { 2, { Operand::U16, Operand::F32 } },               (uint16_t) sizeof(CtrlSmoothState), Advance::PerFrame, Lifecycle::Scope, &evalCtrlSmooth },
    { opcode(Op::CtrlQuantize),  "CTRL_QUANTIZE",  { 2, { Operand::U16, Operand::F32 } },                                                                      0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlQuantize },
    { opcode(Op::CtrlDyn),       "CTRL_DYN",       { 8, { Operand::U16, Operand::U8, Operand::U8, Operand::U8, Operand::U32, Operand::U8, Operand::F32ArrayU16, Operand::U16ArrayU8 } },
                                                                                                      (uint16_t) sizeof(CtrlSmoothState), Advance::PerFrame, Lifecycle::Scope, &evalCtrlDyn },
    { opcode(Op::CtrlQuantizeScale), "CTRL_QUANTIZE_SCALE", { 3, { Operand::U16, Operand::F32, Operand::F32ArrayU16 } }, 0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlQuantizeScale },
    { opcode(Op::CtrlInput),     "CTRL_INPUT",     { 2, { Operand::U32, Operand::U32 } }, 0, Advance::PerFrame, Lifecycle::Scope, &evalCtrlInput },
    { opcode(Op::ChannelApply),  "CHANNEL_APPLY",  { 1, { Operand::U32 } }, 0, Advance::None, Lifecycle::Stateless, &evalChannelApply },
};

inline const OpSpec* findOp(uint8_t opcode) noexcept
{
    static const OpSpec* index[256] = {};
    static const bool built = [] {
        for (const OpSpec& spec : kOpTable)
            index[spec.opcode] = &spec;
        return true;
    }();
    (void) built;
    return index[opcode];
}

} // namespace curlop::vm
