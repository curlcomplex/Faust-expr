// F-066 — Program container + table-validated builder.
//
// A program is a flat byte sequence. The builder is the only writer and it
// validates every emit against the op table's operand schema — a program
// the builder produces is by construction walkable by the same schema.
//
// Front ends (script compiler, step grid, tests) author programs through
// this builder; the Machine executes them. SPEC-009 R2: presentations are
// emitters, not runtimes.
//
// Zero-JUCE, std-only. Consumers should include Machine.h (which provides
// the op table definitions this builder validates against).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "OpIds.h"
#include "OpTable.h"
#include "Signal.h"

namespace curlop::vm {

enum class ChannelSourceKind : uint8_t {
    LocalEmission = 0,
    DenseStream = 1,
    PacketInput = 2,
};

struct ChannelPlanEntry {
    ChannelSourceKind sourceKind = ChannelSourceKind::LocalEmission;
    uint32_t sourceSignal = 0;
    uint32_t sourceChannel = 0;
    uint64_t outputStableChannelId = 0;
    bool preserveSourceStableChannelId = false;
};

// B-1738 — immutable ordered gather prepared off the audio thread. Output
// channel is the entry's position. Descriptor-backed plans must provide an
// output-local stable identity for every entry; zero remains the explicit
// legacy identity for non-descriptor programs.
struct ChannelPlan {
    uint32_t outputSignal = 0;
    std::vector<ChannelPlanEntry> entries;
    bool descriptorBacked = false;
};

struct Program {
    struct TimelineStep {
        float offsetBeats = 0.0f;
        float durationBeats = 0.0f;
        int sourceOrdinal = -1;
    };

    std::vector<uint8_t> code;
    std::vector<TimelineStep> timeline;
    std::vector<ChannelPlan> channelPlans;
    std::vector<std::pair<uint32_t, uint32_t>> packetInputs;
};

inline std::size_t channelPacketAmplification(
    const Program& program,
    ChannelSourceKind sourceKind =
        ChannelSourceKind::LocalEmission) noexcept
{
    std::size_t amplification = 1u;
    for (const auto& plan : program.channelPlans)
        for (const auto& entry : plan.entries) {
            if (entry.sourceKind != sourceKind)
                continue;
            std::size_t copies = 0u;
            for (const auto& candidatePlan :
                 program.channelPlans)
                for (const auto& candidate :
                     candidatePlan.entries)
                    if (candidate.sourceKind == sourceKind
                        && candidate.sourceSignal
                            == entry.sourceSignal
                        && candidate.sourceChannel
                            == entry.sourceChannel)
                        ++copies;
            amplification = std::max(amplification, copies);
        }
    return amplification;
}

class ProgramBuilder {
public:
    ProgramBuilder() = default;
    explicit ProgramBuilder(Program program) : program_(std::move(program)) {}

    // Generic emit: opcode + raw operand bytes, validated against the
    // table's schema (opcode registered, operand bytes exactly consumed).
    // Returns false (and appends nothing) on any mismatch.
    bool emitRaw(uint8_t opcode, const uint8_t* operands, size_t len)
    {
        const OpSpec* spec = findOp(opcode);
        if (spec == nullptr)
            return false;
        const size_t sz = operandBytes(spec->operands,
                                       len > 0 ? operands : nullptr, len);
        if (sz == SIZE_MAX || sz != len)
            return false;
        program_.code.push_back(opcode);
        if (len > 0)
            program_.code.insert(program_.code.end(), operands, operands + len);
        return true;
    }

    // ── Typed convenience emitters (always schema-correct) ──────────
    ProgramBuilder& loop(double lengthBeats)
    {
        uint8_t b[4];
        putF32(b, (float) lengthBeats);
        emitRaw(opcode(Op::Loop), b, 4);
        return *this;
    }

    ProgramBuilder& step(double beatOffset, double durationBeats)
    {
        uint8_t b[8];
        putF32(b,     (float) beatOffset);
        putF32(b + 4, (float) durationBeats);
        emitRaw(opcode(Op::Step), b, 8);
        return *this;
    }

    ProgramBuilder& note(float semitonesFromMiddleC, float velocity)
    {
        uint8_t b[8];
        putF32(b,     semitonesFromMiddleC);
        putF32(b + 4, velocity);
        emitRaw(opcode(Op::Note), b, 8);
        return *this;
    }

    ProgramBuilder& noteExpr(const std::vector<uint16_t>& pitchExpr,
                             const std::vector<float>& pitchImm,
                             float midpointSemis,
                             float velocity)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, pitchExpr);
        appendF32Array(b, pitchImm);
        size_t at = b.size();
        b.resize(at + 4);
        putF32(b.data() + at, midpointSemis);
        at = b.size();
        b.resize(at + 4);
        putF32(b.data() + at, velocity);
        emitRaw(opcode(Op::NoteExpr), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& rest()
    {
        emitRaw(opcode(Op::Rest), nullptr, 0);
        return *this;
    }

    // ── Articulations (step body: STEP, articulations, NOTE, locks) ──
    ProgramBuilder& ratchet(uint32_t count)
    {
        uint8_t b[4];
        std::memcpy(b, &count, 4);
        emitRaw(opcode(Op::Ratchet), b, 4);
        return *this;
    }

    ProgramBuilder& ratchetPitched(uint32_t count, const std::vector<float>& pitches)
    {
        std::vector<uint8_t> b(4);
        std::memcpy(b.data(), &count, 4);
        appendF32Array(b, pitches);
        emitRaw(opcode(Op::RatchetPitched), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& flam(float offsetBeats, float graceVelocity)
    {
        return emitF32x2(Op::Flam, offsetBeats, graceVelocity);
    }

    ProgramBuilder& buzz(float pressure, float durationBeats)
    {
        return emitF32x2(Op::Buzz, pressure, durationBeats);
    }

    // F-071 T-516 — intervalDomain: 0 = none (derive legacy 0.4×stepDur, BPM-
    // relative); 1 = interval is beats (BPM-relative, scales w/ timescale);
    // 2 = interval is seconds (absolute physics-time, immune to timescale).
    // Default (0,0) keeps direct-byte callers on the legacy derived interval.
    ProgramBuilder& bounce(float gravity, float interval = 0.0f,
                           uint8_t intervalDomain = 0)
    {
        uint8_t b[9];
        putF32(b, gravity);
        putF32(b + 4, interval);
        b[8] = intervalDomain;
        emitRaw(opcode(Op::Bounce), b, 9);
        return *this;
    }

    ProgramBuilder& geiger(float density, uint32_t seed)
    {
        uint8_t b[8];
        putF32(b, density);
        std::memcpy(b + 4, &seed, 4);
        emitRaw(opcode(Op::Geiger), b, 8);
        return *this;
    }

    ProgramBuilder& gateLen(float durationBeats)
    {
        uint8_t b[4];
        putF32(b, durationBeats);
        emitRaw(opcode(Op::GateLen), b, 4);
        return *this;
    }

    ProgramBuilder& noteChord(const std::vector<float>& semitones, float velocity)
    {
        std::vector<uint8_t> b;
        appendF32Array(b, semitones);
        const size_t at = b.size();
        b.resize(at + 4);
        std::memcpy(b.data() + at, &velocity, 4);
        emitRaw(opcode(Op::NoteChord), b.data(), b.size());
        return *this;
    }

    // T-469 — decoupled gate/pitch step body ops.
    ProgramBuilder& gateOnly(float velocity)
    {
        uint8_t b[4];
        putF32(b, velocity);
        emitRaw(opcode(Op::GateOnly), b, 4);
        return *this;
    }

    ProgramBuilder& pitchSet(float semitonesFromMiddleC)
    {
        uint8_t b[4];
        putF32(b, semitonesFromMiddleC);
        emitRaw(opcode(Op::PitchSet), b, 4);
        return *this;
    }

    ProgramBuilder& paramLock(uint16_t lane, float value, bool isOffset)
    {
        uint8_t b[7];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, value);
        b[6] = isOffset ? 2 : 1;
        emitRaw(opcode(Op::ParamLock), b, 7);
        return *this;
    }

    ProgramBuilder& paramLockTyped(uint16_t lane, float value, uint8_t basis,
                                   float stepBeats, bool isOffset)
    {
        uint8_t b[12];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, value);
        b[6] = basis;
        putF32(b + 7, stepBeats);
        b[11] = isOffset ? 2 : 1;
        emitRaw(opcode(Op::ParamLockTyped), b, sizeof(b));
        return *this;
    }

    ProgramBuilder& localOutput(uint32_t lane, SignalType type, float value)
    {
        return localOutput(lane, 0u, type, value);
    }

    ProgramBuilder& localOutput(uint32_t lane, uint32_t channel,
                                SignalType type, float value)
    {
        uint8_t b[13];
        std::memcpy(b, &lane, 4);
        std::memcpy(b + 4, &channel, 4);
        b[8] = (uint8_t) type;
        putF32(b + 9, value);
        emitRaw(opcode(Op::LocalOutput), b, sizeof(b));
        return *this;
    }

    ProgramBuilder& localGateOnly(uint32_t lane, float velocity)
    {
        return localGateOnly(lane, 0u, velocity);
    }

    ProgramBuilder& localGateOnly(uint32_t lane, uint32_t channel,
                                  float velocity)
    {
        uint8_t b[12];
        std::memcpy(b, &lane, 4);
        std::memcpy(b + 4, &channel, 4);
        putF32(b + 8, velocity);
        emitRaw(opcode(Op::LocalGateOnly), b, sizeof(b));
        return *this;
    }

    ProgramBuilder& markovEvent(const std::vector<float>& pitches,
                                const std::vector<float>& velocities,
                                uint16_t valueLane,
                                const std::vector<float>& values,
                                bool hasValue,
                                uint32_t seed)
    {
        std::vector<uint8_t> b;
        appendF32Array(b, pitches);
        appendF32Array(b, velocities);
        const size_t laneAt = b.size();
        b.resize(laneAt + 2);
        std::memcpy(b.data() + laneAt, &valueLane, 2);
        appendF32Array(b, values);
        b.push_back(hasValue ? 1 : 0);
        const size_t seedAt = b.size();
        b.resize(seedAt + 4);
        std::memcpy(b.data() + seedAt, &seed, 4);
        emitRaw(opcode(Op::MarkovEvent), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& markovGate(const std::vector<float>& gates,
                               const std::vector<float>& velocities,
                               uint32_t seed)
    {
        std::vector<uint8_t> b;
        appendF32Array(b, gates);
        appendF32Array(b, velocities);
        const size_t seedAt = b.size();
        b.resize(seedAt + 4);
        std::memcpy(b.data() + seedAt, &seed, 4);
        emitRaw(opcode(Op::MarkovGate), b.data(), b.size());
        return *this;
    }

    // ── Scopes + transforms ──────────────────────────────────────────
    ProgramBuilder& scopeStart(double baseOffsetBeats, double durBeats)
    {
        return emitF32x2(Op::ScopeStart, (float) baseOffsetBeats, (float) durBeats);
    }
    ProgramBuilder& scopeEnd()  { emitRaw(opcode(Op::ScopeEnd), nullptr, 0); return *this; }

    ProgramBuilder& transpose(float semis)
    {
        uint8_t b[4];
        putF32(b, semis);
        emitRaw(opcode(Op::Transpose), b, 4);
        return *this;
    }

    ProgramBuilder& groove(uint8_t templateId, float amountPercent,
                           float velLow, float velHigh)
    {
        uint8_t b[13];
        b[0] = templateId;
        putF32(b + 1, amountPercent);
        putF32(b + 5, velLow);
        putF32(b + 9, velHigh);
        emitRaw(opcode(Op::Groove), b, 13);
        return *this;
    }

    ProgramBuilder& reverse() { emitRaw(opcode(Op::Reverse), nullptr, 0); return *this; }

    ProgramBuilder& rotate(int16_t amount)
    {
        uint8_t b[2];
        std::memcpy(b, &amount, 2);
        emitRaw(opcode(Op::Rotate), b, 2);
        return *this;
    }

    ProgramBuilder& shuffle(uint32_t seed)
    {
        uint8_t b[4];
        std::memcpy(b, &seed, 4);
        emitRaw(opcode(Op::Shuffle), b, 4);
        return *this;
    }

    // mode: 0 = semitone pivot, 1 = pivot on first note, 2 = pivot on the
    // Nth content item's pitch (pivot operand = N, 1-based — B-288).
    // bool callers (true = first-note) convert as before.
    ProgramBuilder& invert(float pivotSemis, uint8_t mode)
    {
        uint8_t b[5];
        putF32(b, pivotSemis);
        b[4] = mode;
        emitRaw(opcode(Op::Invert), b, 5);
        return *this;
    }

    // B-288 — runtime SORT order op: 0 asc, 1 desc.
    ProgramBuilder& sort(uint8_t direction)
    {
        emitRaw(opcode(Op::Sort), &direction, 1);
        return *this;
    }

    ProgramBuilder& timescale(float factor)
    {
        uint8_t b[4];
        putF32(b, factor);
        emitRaw(opcode(Op::Timescale), b, 4);
        return *this;
    }

    // F-071 T-571a — nested `timescale:~lfo`: a scope-local timescale swept by an
    // LFO (SPEC-018 §4.6 local half). waveform 0 sine 1 tri 2 saw 3 square;
    // rateBeats = beats per cycle; min/max are MULTIPLIERS (1.0 = unchanged).
    ProgramBuilder& timescaleMod(uint8_t waveform, float rateBeats,
                                 float minMul, float maxMul)
    {
        uint8_t b[13];
        b[0] = waveform;
        putF32(b + 1, rateBeats);
        putF32(b + 5, minMul);
        putF32(b + 9, maxMul);
        emitRaw(opcode(Op::TimescaleMod), b, 13);
        return *this;
    }

    ProgramBuilder& timescaleExpr(const std::vector<uint16_t>& expr,
                                  const std::vector<float>& immediates,
                                  uint8_t mode = 0)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, expr);
        appendF32Array(b, immediates);
        b.push_back(mode);
        emitRaw(opcode(Op::TimescaleExpr), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& transposeExpr(const std::vector<uint16_t>& expr,
                                  const std::vector<float>& immediates,
                                  float factor = 1.0f)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, expr);
        appendF32Array(b, immediates);
        const size_t at = b.size();
        b.resize(at + 4);
        std::memcpy(b.data() + at, &factor, 4);
        emitRaw(opcode(Op::TransposeExpr), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& rotateExpr(const std::vector<uint16_t>& expr,
                               const std::vector<float>& immediates)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, expr);
        appendF32Array(b, immediates);
        emitRaw(opcode(Op::RotateExpr), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& invertExpr(const std::vector<uint16_t>& expr,
                               const std::vector<float>& immediates)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, expr);
        appendF32Array(b, immediates);
        emitRaw(opcode(Op::InvertExpr), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& grooveExpr(uint8_t templateId,
                               const std::vector<uint16_t>& amountExpr,
                               const std::vector<float>& amountImm,
                               const std::vector<uint16_t>& velLoExpr,
                               const std::vector<float>& velLoImm,
                               const std::vector<uint16_t>& velHiExpr,
                               const std::vector<float>& velHiImm)
    {
        std::vector<uint8_t> b;
        b.push_back(templateId);
        appendU16Array(b, amountExpr);
        appendF32Array(b, amountImm);
        appendU16Array(b, velLoExpr);
        appendF32Array(b, velLoImm);
        appendU16Array(b, velHiExpr);
        appendF32Array(b, velHiImm);
        emitRaw(opcode(Op::GrooveExpr), b.data(), b.size());
        return *this;
    }

    // F-071 T-571b — nested `bpm:N`: a scope-local tempo override (SPEC-018 §4.6).
    // The kernel sets frame.timescale = N / scriptBpm (a timescale SET) so inner
    // offsets play at tempo N.
    ProgramBuilder& scopeBpm(float bpm)
    {
        uint8_t b[4];
        putF32(b, bpm);
        emitRaw(opcode(Op::ScopeBpm), b, 4);
        return *this;
    }

    ProgramBuilder& stepAbs(double beatOffset, float durationMs)
    {
        return emitF32x2(Op::StepAbs, (float) beatOffset, durationMs);
    }

    // B-291 — runtime GRID frame op: size in beats + strength 0-1.
    ProgramBuilder& grid(float sizeBeats, float strength)
    {
        return emitF32x2(Op::Grid, sizeBeats, strength);
    }

    ProgramBuilder& gridExpr(const std::vector<uint16_t>& sizeExpr,
                             const std::vector<float>& sizeImm,
                             const std::vector<uint16_t>& strengthExpr,
                             const std::vector<float>& strengthImm)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, sizeExpr);
        appendF32Array(b, sizeImm);
        appendU16Array(b, strengthExpr);
        appendF32Array(b, strengthImm);
        emitRaw(opcode(Op::GridExpr), b.data(), b.size());
        return *this;
    }

    // B-289 — runtime SCALE frame op: root chroma (0-12) + scale-degree
    // intervals within the 12-semitone period. Registry resolution
    // (name → intervals) happens in the compiler; quantization in
    // resolvePitch at fire time.
    ProgramBuilder& scale(float rootChroma, const std::vector<float>& intervals)
    {
        std::vector<uint8_t> b(4);
        std::memcpy(b.data(), &rootChroma, 4);
        appendF32Array(b, intervals);
        emitRaw(opcode(Op::Scale), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& scaleExpr(const std::vector<uint16_t>& rootExpr,
                              const std::vector<float>& rootImm,
                              const std::vector<float>& intervals)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, rootExpr);
        appendF32Array(b, rootImm);
        appendF32Array(b, intervals);
        emitRaw(opcode(Op::ScaleExpr), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& repeatExpr(const std::vector<uint16_t>& expr,
                               const std::vector<float>& immediates)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, expr);
        appendF32Array(b, immediates);
        emitRaw(opcode(Op::RepeatExpr), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& scopeRepeatExpr(const std::vector<uint16_t>& expr,
                                    const std::vector<float>& immediates)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, expr);
        appendF32Array(b, immediates);
        emitRaw(opcode(Op::ScopeRepeatExpr), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& scopeFitExpr(const std::vector<uint16_t>& expr,
                                 const std::vector<float>& immediates)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, expr);
        appendF32Array(b, immediates);
        emitRaw(opcode(Op::ScopeFitExpr), b.data(), b.size());
        return *this;
    }

    // F-071 T-539 — stochastic scale. The candidate interval sets are baked
    // into a packed f32 pool ([len0, iv0..., len1, iv1...]); evalScaleGen
    // draws one per scope iteration. kind: 0 random / 1 bernoulli.
    ProgramBuilder& scaleGen(float rootChroma, uint8_t kind, float weight,
                             uint32_t seed,
                             const std::vector<std::vector<float>>& candidates)
    {
        std::vector<uint8_t> b;
        size_t at = b.size(); b.resize(at + 4);
        std::memcpy(b.data() + at, &rootChroma, 4);          // F32 root
        b.push_back(kind);                                    // U8 kind
        at = b.size(); b.resize(at + 4);
        std::memcpy(b.data() + at, &weight, 4);               // F32 weight
        at = b.size(); b.resize(at + 4);
        std::memcpy(b.data() + at, &seed, 4);                 // U32 seed
        b.push_back((uint8_t) candidates.size());             // U8 count
        std::vector<float> packed;                            // F32ArrayU16 pool
        for (const auto& cand : candidates) {
            packed.push_back((float) cand.size());
            for (float iv : cand) packed.push_back(iv);
        }
        appendF32Array(b, packed);
        emitRaw(opcode(Op::ScaleGen), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& scaleGenDyn(float rootChroma, uint8_t kind,
                                const std::vector<uint16_t>& weightExpr,
                                const std::vector<float>& weightImm,
                                uint32_t seed,
                                const std::vector<std::vector<float>>& candidates)
    {
        std::vector<uint8_t> b;
        size_t at = b.size(); b.resize(at + 4);
        std::memcpy(b.data() + at, &rootChroma, 4);
        b.push_back(kind);
        appendU16Array(b, weightExpr);
        appendF32Array(b, weightImm);
        at = b.size(); b.resize(at + 4);
        std::memcpy(b.data() + at, &seed, 4);
        b.push_back((uint8_t) candidates.size());
        std::vector<float> packed;
        for (const auto& cand : candidates) {
            packed.push_back((float) cand.size());
            for (float iv : cand) packed.push_back(iv);
        }
        appendF32Array(b, packed);
        emitRaw(opcode(Op::ScaleGenDyn), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& scaleGenRootDyn(const std::vector<uint16_t>& rootExpr,
                                    const std::vector<float>& rootImm,
                                    uint8_t kind,
                                    const std::vector<uint16_t>& weightExpr,
                                    const std::vector<float>& weightImm,
                                    uint32_t seed,
                                    const std::vector<std::vector<float>>& candidates)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, rootExpr);
        appendF32Array(b, rootImm);
        b.push_back(kind);
        appendU16Array(b, weightExpr);
        appendF32Array(b, weightImm);
        size_t at = b.size(); b.resize(at + 4);
        std::memcpy(b.data() + at, &seed, 4);
        b.push_back((uint8_t) candidates.size());
        std::vector<float> packed;
        for (const auto& cand : candidates) {
            packed.push_back((float) cand.size());
            for (float iv : cand) packed.push_back(iv);
        }
        appendF32Array(b, packed);
        emitRaw(opcode(Op::ScaleGenRootDyn), b.data(), b.size());
        return *this;
    }

    // REPEAT is a builder unroll: the body is authored once into a
    // sub-builder, then re-emitted `count` times with each copy's TOP-LEVEL
    // step and scope-start offsets shifted by r*lengthBeats. Offsets INSIDE
    // a nested scope are parent-relative and must NOT shift (B-296: the
    // flat shift double-moved a var's contents out of their bar — they
    // wrapped onto the first pass and the outer copies sounded blank).
    // The rewrite is schema-driven — the same operand description the
    // kernel decodes with.
    template <typename BodyFn>
    ProgramBuilder& repeat(uint32_t count, double lengthBeats, BodyFn body)
    {
        ProgramBuilder sub;
        body(sub);
        const std::vector<uint8_t>& src = sub.program_.code;
        for (uint32_t r = 0; r < count; ++r) {
            const float shift = (float) (r * lengthBeats);
            size_t pc = 0;
            int depth = 0;
            while (pc < src.size()) {
                const OpSpec* spec = findOp(src[pc]);
                if (spec == nullptr)
                    return *this;   // unreachable: sub-builder validated
                const size_t sz = operandBytes(spec->operands,
                                               src.data() + pc + 1,
                                               src.size() - pc - 1);
                std::vector<uint8_t> chunk(src.begin() + (long) pc + 1,
                                           src.begin() + (long) (pc + 1 + sz));
                const bool isScopeStart = spec->opcode == opcode(Op::ScopeStart);
                if (depth == 0
                    && (spec->opcode == opcode(Op::Step)
                        || spec->opcode == opcode(Op::StepAbs)
                        || isScopeStart)) {
                    float off;
                    std::memcpy(&off, chunk.data(), 4);
                    off += shift;
                    std::memcpy(chunk.data(), &off, 4);
                }
                if (isScopeStart) ++depth;
                else if (spec->opcode == opcode(Op::ScopeEnd)) --depth;
                emitRaw(spec->opcode, chunk.empty() ? nullptr : chunk.data(),
                        chunk.size());
                pc += 1 + sz;
            }
        }
        return *this;
    }

    // ── Control ops (per-sample stream producers) ────────────────────
    // Trailing `offsetMode` operand (B-269 + SPEC-014 lock layering):
    //   0 = absolute range (Stream-kind row — the route IS the input's base,
    //       overrides the knob; GUI abs pin)
    //   1 = % range (Offset-kind row — delta summed over whichever base
    //       holds; GUI dotted offset arc)
    // phase01: waveform start offset as a cycle fraction (0..1) from the
    // route's anchor — 0.75 starts a sine at its minimum, 0.25 at its max.
    // F-071 T-495 — rateDomain: 0 = absolute (rate field is Hz, immune to
    // timescale); 1 = BPM-relative (rate field is cycles-per-beat, scales with
    // the scope's cumulative timescale + tracks tempo). Default 0 keeps direct-
    // byte callers on the legacy absolute-Hz path.
    ProgramBuilder& ctrlLfo(uint16_t lane, uint8_t waveform, float rate,
                            float mn, float mx, uint8_t offsetMode = 0,
                            float phase01 = 0.0f, uint8_t rateDomain = 0)
    {
        uint8_t b[21];
        std::memcpy(b, &lane, 2);
        b[2] = waveform;
        putF32(b + 3, rate);
        putF32(b + 7, mn);
        putF32(b + 11, mx);
        b[15] = offsetMode;
        putF32(b + 16, phase01);
        b[20] = rateDomain;
        emitRaw(opcode(Op::CtrlLfo), b, 21);
        return *this;
    }

    ProgramBuilder& ctrlClip(uint16_t lane, float mn, float mx)
    {
        uint8_t b[10];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, mn);
        putF32(b + 6, mx);
        emitRaw(opcode(Op::CtrlClip), b, 10);
        return *this;
    }

    ProgramBuilder& ctrlInvert(uint16_t lane, float pivot = 0.5f)
    {
        uint8_t b[6];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, pivot);
        emitRaw(opcode(Op::CtrlInvert), b, 6);
        return *this;
    }

    ProgramBuilder& ctrlScale(uint16_t lane, float mn, float mx)
    {
        uint8_t b[10];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, mn);
        putF32(b + 6, mx);
        emitRaw(opcode(Op::CtrlScale), b, 10);
        return *this;
    }

    ProgramBuilder& ctrlOffset(uint16_t lane, float amount)
    {
        uint8_t b[6];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, amount);
        emitRaw(opcode(Op::CtrlOffset), b, 6);
        return *this;
    }

    ProgramBuilder& ctrlGain(uint16_t lane, float amount)
    {
        uint8_t b[6];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, amount);
        emitRaw(opcode(Op::CtrlGain), b, 6);
        return *this;
    }

    ProgramBuilder& ctrlAbs(uint16_t lane)
    {
        uint8_t b[2];
        std::memcpy(b, &lane, 2);
        emitRaw(opcode(Op::CtrlAbs), b, 2);
        return *this;
    }

    ProgramBuilder& ctrlSmooth(uint16_t lane, float seconds)
    {
        uint8_t b[6];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, seconds);
        emitRaw(opcode(Op::CtrlSmooth), b, 6);
        return *this;
    }

    ProgramBuilder& ctrlQuantize(uint16_t lane, float step)
    {
        uint8_t b[6];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, step);
        emitRaw(opcode(Op::CtrlQuantize), b, 6);
        return *this;
    }

    ProgramBuilder& ctrlQuantizeScale(uint16_t lane, float root,
                                      const std::vector<float>& intervals)
    {
        std::vector<uint8_t> b;
        size_t at = b.size(); b.resize(at + 2);
        std::memcpy(b.data() + at, &lane, 2);
        at = b.size(); b.resize(at + 4);
        putF32(b.data() + at, root);
        appendF32Array(b, intervals);
        emitRaw(opcode(Op::CtrlQuantizeScale), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& ctrlInput(uint32_t lane, uint32_t inputChannel)
    {
        uint8_t b[8];
        std::memcpy(b, &lane, 4);
        std::memcpy(b + 4, &inputChannel, 4);
        emitRaw(opcode(Op::CtrlInput), b, sizeof(b));
        return *this;
    }

    bool addChannelPlan(const ChannelPlan& plan, uint32_t& index)
    {
        if (plan.entries.empty()
            || program_.channelPlans.size()
                >= (size_t) std::numeric_limits<uint32_t>::max()
            || plan.outputSignal >= kNamedOutputLaneFlag)
            return false;
        for (const auto& entry : plan.entries) {
            if (entry.sourceKind != ChannelSourceKind::LocalEmission
                && entry.sourceKind != ChannelSourceKind::DenseStream
                && entry.sourceKind != ChannelSourceKind::PacketInput)
                return false;
            if (plan.descriptorBacked && entry.outputStableChannelId == 0)
                return false;
        }
        index = (uint32_t) program_.channelPlans.size();
        program_.channelPlans.push_back(plan);
        return true;
    }

    ProgramBuilder& channelApply(uint32_t planIndex)
    {
        uint8_t b[4];
        std::memcpy(b, &planIndex, 4);
        emitRaw(opcode(Op::ChannelApply), b, sizeof(b));
        return *this;
    }

    bool channelApplyBeforeTrailingHalt(uint32_t planIndex)
    {
        if (program_.code.empty()
            || program_.code.back() != opcode(Op::Halt))
            return false;
        program_.code.pop_back();
        channelApply(planIndex);
        halt();
        return true;
    }

    ProgramBuilder& packetInput(uint32_t signal, uint32_t channel)
    {
        program_.packetInputs.push_back({ signal, channel });
        return *this;
    }

    ProgramBuilder& ctrlDyn(uint16_t lane, uint8_t kind, uint8_t aux0, uint8_t aux1,
                            uint32_t seed, uint8_t flags,
                            const std::vector<float>& immediates,
                            const std::vector<uint16_t>& argLanes)
    {
        std::vector<uint8_t> b;
        size_t at = b.size(); b.resize(at + 2);
        std::memcpy(b.data() + at, &lane, 2);
        b.push_back(kind);
        b.push_back(aux0);
        b.push_back(aux1);
        at = b.size(); b.resize(at + 4);
        std::memcpy(b.data() + at, &seed, 4);
        b.push_back(flags);
        appendF32Array(b, immediates);
        appendU16Array(b, argLanes);
        emitRaw(opcode(Op::CtrlDyn), b.data(), b.size());
        return *this;
    }

    // F-071 T-495 — timeDomainMask: bit0 attack, bit1 decay. A set bit = that
    // time is ABSOLUTE (the field is seconds, immune to tempo + timescale); a
    // clear bit = BPM-relative (field is beats, scales w/ tempo + timescale).
    // Default 0 = all-beats (legacy direct-byte path).
    ProgramBuilder& ctrlAd(uint16_t lane, float attack, float decay,
                           float mn, float mx, bool retrigger,
                           uint8_t offsetMode = 0, uint8_t timeDomainMask = 0)
    {
        uint8_t b[21];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, attack);
        putF32(b + 6, decay);
        putF32(b + 10, mn);
        putF32(b + 14, mx);
        b[18] = retrigger ? 1 : 0;
        b[19] = offsetMode;
        b[20] = timeDomainMask;
        emitRaw(opcode(Op::CtrlAd), b, 21);
        return *this;
    }

    // F-071 T-495 — timeDomainMask: bit0 attack, bit1 decay, bit3 release. A
    // set bit = that time is ABSOLUTE (seconds); clear = BPM-relative (beats).
    // sustain (idx 3) is a level, no domain. Default 0 = all-beats.
    ProgramBuilder& ctrlAdsr(uint16_t lane, float a, float d, float s, float r,
                             float mn, float mx, uint8_t offsetMode = 0,
                             bool retrigger = false, uint8_t timeDomainMask = 0)
    {
        uint8_t b[29];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, a);
        putF32(b + 6, d);
        putF32(b + 10, s);
        putF32(b + 14, r);
        putF32(b + 18, mn);
        putF32(b + 22, mx);
        b[26] = offsetMode;
        b[27] = retrigger ? 1 : 0;
        b[28] = timeDomainMask;
        emitRaw(opcode(Op::CtrlAdsr), b, 29);
        return *this;
    }

    ProgramBuilder& ctrlAuto(uint16_t lane, float start, float end, uint8_t curve,
                             uint8_t offsetMode = 0)
    {
        uint8_t b[12];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, start);
        putF32(b + 6, end);
        b[10] = curve;
        b[11] = offsetMode;
        emitRaw(opcode(Op::CtrlAuto), b, 12);
        return *this;
    }

    ProgramBuilder& deviate(float amountFrac, uint32_t seed)
    {
        uint8_t b[8];
        putF32(b, amountFrac);
        std::memcpy(b + 4, &seed, 4);
        emitRaw(opcode(Op::Deviate), b, 8);
        return *this;
    }

    // B-187 — generator-valued articulation arg: arm-time draw overriding
    // the following articulation's immediate operand. kind 0 = random(a, b),
    // kind 1 = bernoulli(w, a, b).
    ProgramBuilder& genOperand(GenTarget target, uint8_t kind,
                               float a, float bVal, float w, uint32_t seed)
    {
        uint8_t b[18];
        b[0] = (uint8_t) target;
        b[1] = kind;
        putF32(b + 2, a);
        putF32(b + 6, bVal);
        putF32(b + 10, w);
        std::memcpy(b + 14, &seed, 4);
        emitRaw(opcode(Op::GenOperand), b, 18);
        return *this;
    }

    ProgramBuilder& genOperandDyn(GenTarget target, uint8_t kind, uint32_t seed,
                                  const std::vector<uint16_t>& exprA,
                                  const std::vector<float>& immA,
                                  const std::vector<uint16_t>& exprB,
                                  const std::vector<float>& immB,
                                  const std::vector<uint16_t>& exprW,
                                  const std::vector<float>& immW)
    {
        std::vector<uint8_t> b;
        b.push_back((uint8_t) target);
        b.push_back(kind);
        const size_t at = b.size();
        b.resize(at + 4);
        std::memcpy(b.data() + at, &seed, 4);
        appendU16Array(b, exprA);
        appendF32Array(b, immA);
        appendU16Array(b, exprB);
        appendF32Array(b, immB);
        appendU16Array(b, exprW);
        appendF32Array(b, immW);
        emitRaw(opcode(Op::GenOperandDyn), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& ctrlRandom(uint16_t lane, float mn, float mx, uint32_t seed,
                               uint8_t offsetMode = 0, uint8_t perHit = 0,
                               float slewBeats = 0.0f)
    {
        // slewBeats = one-pole de-click time in BEATS (converted to frames at
        // runtime with the live tempo, like ~glide). 0 = snappy block-fill,
        // the original behaviour. The 10ms LANGUAGE default lives in the
        // compiler, not here, so existing builder call sites + their golden
        // streams stay byte-identical. The one-pole's running value is
        // lane-keyed in the machine, so per-step routes blend across draws.
        uint8_t b[20];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, mn);
        putF32(b + 6, mx);
        std::memcpy(b + 10, &seed, 4);
        b[14] = offsetMode;
        b[15] = perHit;
        putF32(b + 16, slewBeats);
        emitRaw(opcode(Op::CtrlRandom), b, 20);
        return *this;
    }

    ProgramBuilder& ctrlDeviate(uint16_t lane, float amount, uint32_t seed,
                                uint8_t perHit = 0)
    {
        uint8_t b[11];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, amount);
        std::memcpy(b + 6, &seed, 4);
        b[10] = perHit;
        emitRaw(opcode(Op::CtrlDeviate), b, 11);
        return *this;
    }

    ProgramBuilder& ctrlBernoulli(uint16_t lane, float weight, float valA,
                                  float valB, uint32_t seed,
                                  uint8_t offsetMode = 0, uint8_t perHit = 0)
    {
        uint8_t b[20];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, weight);
        putF32(b + 6, valA);
        putF32(b + 10, valB);
        std::memcpy(b + 14, &seed, 4);
        b[18] = offsetMode;
        b[19] = perHit;
        emitRaw(opcode(Op::CtrlBernoulli), b, 20);
        return *this;
    }

    ProgramBuilder& ctrlKeytrack(uint16_t lane, float mn, float mx)
    {
        uint8_t b[10];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, mn);
        putF32(b + 6, mx);
        emitRaw(opcode(Op::CtrlKeytrack), b, 10);
        return *this;
    }

    ProgramBuilder& accum(uint16_t lane, float start, float increment, float ceiling)
    {
        uint8_t b[14];
        std::memcpy(b, &lane, 2);
        putF32(b + 2, start);
        putF32(b + 6, increment);
        putF32(b + 10, ceiling);
        emitRaw(opcode(Op::Accum), b, 14);
        return *this;
    }

    ProgramBuilder& ctrlMidi(uint16_t lane, uint8_t channel, uint8_t cc,
                             float mnNorm, float mxNorm, uint8_t offsetMode = 0)
    {
        uint8_t b[13];
        std::memcpy(b, &lane, 2);
        b[2] = channel;
        b[3] = cc;
        putF32(b + 4, mnNorm);
        putF32(b + 8, mxNorm);
        b[12] = offsetMode;
        emitRaw(opcode(Op::CtrlMidi), b, 13);
        return *this;
    }

    ProgramBuilder& glide(float timeBeats)
    {
        uint8_t b[4];
        putF32(b, timeBeats);
        emitRaw(opcode(Op::Glide), b, 4);
        return *this;
    }

    // ── Conditionals (guard the next step) ──────────────────────────
    ProgramBuilder& condMod(uint16_t divisor)      { return emitU16(Op::CondMod, divisor); }
    ProgramBuilder& condLoopEq(uint16_t loopNum)   { return emitU16(Op::CondLoopEq, loopNum); }
    ProgramBuilder& condFirst(uint16_t cycle)      { return emitU16(Op::CondFirst, cycle); }
    ProgramBuilder& condEven(uint16_t cycle)       { return emitU16(Op::CondEven, cycle); }
    ProgramBuilder& condOdd(uint16_t cycle)        { return emitU16(Op::CondOdd, cycle); }
    ProgramBuilder& condPrime(uint16_t cycle)      { return emitU16(Op::CondPrime, cycle); }
    ProgramBuilder& condNotFirst(uint16_t cycle)   { return emitU16(Op::CondNotFirst, cycle); }
    ProgramBuilder& condFib(uint16_t cycle)        { return emitU16(Op::CondFib, cycle); }
    ProgramBuilder& condAfter(uint16_t threshold)  { return emitU16(Op::CondAfter, threshold); }
    ProgramBuilder& condPrevious()
    {
        emitRaw(opcode(Op::CondPrevious), nullptr, 0);
        return *this;
    }
    ProgramBuilder& condNotPrevious()
    {
        emitRaw(opcode(Op::CondNotPrevious), nullptr, 0);
        return *this;
    }

    ProgramBuilder& condLoopSet(const std::vector<uint16_t>& loops)
    {
        return emitLoopSet(Op::CondLoopSet, loops);
    }
    ProgramBuilder& condNotLoopSet(const std::vector<uint16_t>& loops)
    {
        return emitLoopSet(Op::CondNotLoopSet, loops);
    }

    ProgramBuilder& condExpr(const std::vector<uint16_t>& expr,
                             const std::vector<float>& immediates)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, expr);
        appendF32Array(b, immediates);
        emitRaw(opcode(Op::CondExpr), b.data(), b.size());
        return *this;
    }

    // ── Probability ─────────────────────────────────────────────────
    ProgramBuilder& prob(float threshold, uint32_t seed)
    {
        uint8_t b[8];
        putF32(b, threshold);
        std::memcpy(b + 4, &seed, 4);
        emitRaw(opcode(Op::Prob), b, 8);
        return *this;
    }

    ProgramBuilder& probDyn(const std::vector<uint16_t>& thresholdExpr,
                            const std::vector<float>& thresholdImm,
                            uint32_t seed)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, thresholdExpr);
        appendF32Array(b, thresholdImm);
        const size_t at = b.size();
        b.resize(at + 4);
        std::memcpy(b.data() + at, &seed, 4);
        emitRaw(opcode(Op::ProbDyn), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& bernoulli(float weight, uint8_t optionCount, uint32_t seed)
    {
        uint8_t b[9];
        putF32(b, weight);
        b[4] = optionCount;
        std::memcpy(b + 5, &seed, 4);
        emitRaw(opcode(Op::Bernoulli), b, 9);
        return *this;
    }

    ProgramBuilder& bernoulliDyn(const std::vector<uint16_t>& weightExpr,
                                 const std::vector<float>& weightImm,
                                 uint8_t optionCount, uint32_t seed)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, weightExpr);
        appendF32Array(b, weightImm);
        b.push_back(optionCount);
        const size_t at = b.size();
        b.resize(at + 4);
        std::memcpy(b.data() + at, &seed, 4);
        emitRaw(opcode(Op::BernoulliDyn), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& selectDyn(const std::vector<uint16_t>& selectorExpr,
                              const std::vector<float>& selectorImm,
                              const std::vector<uint16_t>& branchStepCounts)
    {
        std::vector<uint8_t> b;
        appendU16Array(b, selectorExpr);
        appendF32Array(b, selectorImm);
        appendU16Array(b, branchStepCounts);
        emitRaw(opcode(Op::SelectDyn), b.data(), b.size());
        return *this;
    }

    ProgramBuilder& halt()
    {
        emitRaw(opcode(Op::Halt), nullptr, 0);
        return *this;
    }

    Program build() const { return program_; }
    const Program& program() const noexcept { return program_; }

private:
    static void putF32(uint8_t* dst, float v) { std::memcpy(dst, &v, 4); }

    ProgramBuilder& emitF32x2(Op op, float a, float b2)
    {
        uint8_t b[8];
        putF32(b, a);
        putF32(b + 4, b2);
        emitRaw(opcode(op), b, 8);
        return *this;
    }

    static void appendF32Array(std::vector<uint8_t>& b, const std::vector<float>& vals)
    {
        const uint16_t n = (uint16_t) vals.size();
        size_t at = b.size();
        b.resize(at + 2 + 4 * vals.size());
        std::memcpy(b.data() + at, &n, 2);
        at += 2;
        for (float v : vals) {
            std::memcpy(b.data() + at, &v, 4);
            at += 4;
        }
    }

    static void appendU16Array(std::vector<uint8_t>& b, const std::vector<uint16_t>& vals)
    {
        b.push_back((uint8_t) vals.size());
        for (uint16_t v : vals) {
            const size_t at = b.size();
            b.resize(at + 2);
            std::memcpy(b.data() + at, &v, 2);
        }
    }

    ProgramBuilder& emitU16(Op op, uint16_t v)
    {
        uint8_t b[2];
        std::memcpy(b, &v, 2);
        emitRaw(opcode(op), b, 2);
        return *this;
    }

    ProgramBuilder& emitLoopSet(Op op, const std::vector<uint16_t>& loops)
    {
        std::vector<uint8_t> b;
        b.push_back((uint8_t) loops.size());
        for (uint16_t v : loops) {
            const size_t at = b.size();
            b.resize(at + 2);
            std::memcpy(b.data() + at, &v, 2);
        }
        emitRaw(opcode(op), b.data(), b.size());
        return *this;
    }

    Program program_;
};

} // namespace curlop::vm
