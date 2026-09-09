// F-066 — the one-table op registry (adr-vm "How it's built to grow").
//
// An op is a single OpSpec entry: opcode, name, operand schema, evaluate
// function, state size, advance policy, lifecycle policy. The execution
// loop, the program builder, install-time validation, and byte-skipping all
// read that one entry — the instruction format has a single source of truth
// and cannot drift. Adding an op = one evaluate function, one state struct
// (if any), one table entry. Nothing else.
//
// Zero-JUCE, std-only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace curlop::vm {

struct ExecCtx; // defined in Machine.h

// ── Operand schema ────────────────────────────────────────────────
// Self-describing operand layout. Byte-skip, builder emit, and decode all
// derive from this — there is no hand-written "pc += N" anywhere.

enum class Operand : uint8_t {
    U8,
    U16,
    I16,
    U32,
    F32,
    // A u16 element count followed by that many f32s (chord voicings,
    // pitch arrays). Variable-length, still fully schema-described.
    F32ArrayU16,
    // A u8 element count followed by that many u16s (loop-membership sets).
    U16ArrayU8,
};

constexpr int kMaxOperands = 12;

struct OperandSchema {
    uint8_t count = 0;
    Operand kinds[kMaxOperands] = {};
};

// Byte size of one operand kind's fixed part.
constexpr size_t operandFixedBytes(Operand k) noexcept
{
    switch (k) {
        case Operand::U8:          return 1;
        case Operand::U16:         return 2;
        case Operand::I16:         return 2;
        case Operand::U32:         return 4;
        case Operand::F32:         return 4;
        case Operand::F32ArrayU16: return 2; // count prefix; elements follow
        case Operand::U16ArrayU8:  return 1; // count prefix; elements follow
    }
    return 0;
}

// Total operand bytes for an instruction whose operands start at `bytes`
// (length `avail`). Returns SIZE_MAX if the encoding is truncated — callers
// treat that as malformed bytecode.
inline size_t operandBytes(const OperandSchema& schema,
                           const uint8_t* bytes, size_t avail) noexcept
{
    size_t off = 0;
    for (int i = 0; i < schema.count; ++i) {
        const Operand k = schema.kinds[i];
        const size_t fixed = operandFixedBytes(k);
        if (off + fixed > avail)
            return SIZE_MAX;
        if (k == Operand::F32ArrayU16) {
            uint16_t n = 0;
            std::memcpy(&n, bytes + off, 2);
            off += 2;
            if (off + (size_t) n * 4 > avail)
                return SIZE_MAX;
            off += (size_t) n * 4;
        } else if (k == Operand::U16ArrayU8) {
            const uint8_t n = bytes[off];
            off += 1;
            if (off + (size_t) n * 2 > avail)
                return SIZE_MAX;
            off += (size_t) n * 2;
        } else {
            off += fixed;
        }
    }
    return off;
}

// ── Policies ──────────────────────────────────────────────────────
// How an op's state moves forward (adr-vm: "how it advances — every frame,
// on a trigger, or on a clock tick"). Stateless ops use None.
enum class Advance : uint8_t {
    None,
    PerFrame,
    OnTrigger,
    OnTick,
};

// Op lifetime policy (SF-061 s483 sign-off: lifecycle is a per-op PROPERTY,
// not one hardcoded rule). Scope-as-clock is the default for clean-cut ops;
// Free runs independent of scopes. Phase 2 ops parameterize this further.
enum class Lifecycle : uint8_t {
    Stateless,
    Scope,
    Free,
};

// ── The op entry ──────────────────────────────────────────────────

using EvalFn = void (*)(ExecCtx&);

struct OpSpec {
    uint8_t       opcode    = 0;
    const char*   name      = nullptr;
    OperandSchema operands  = {};
    uint16_t      stateSize = 0;          // bytes in the per-instance state arena
    Advance       advance   = Advance::None;
    Lifecycle     lifecycle = Lifecycle::Stateless;
    EvalFn        evaluate  = nullptr;
};

// Registry lookup — populated from the single table in OpsCore.h.
// Returns nullptr for unregistered opcodes.
const OpSpec* findOp(uint8_t opcode) noexcept;

} // namespace curlop::vm
