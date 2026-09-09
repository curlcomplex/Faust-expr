#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>
#include "shell/CurlopDebug.h"
#include "control/vm/machine/Signal.h"   // T-597: row SignalType (declaration-derived layout)
// F-066 Phase 5: the constants this header used to pull from
// SequencerVM_state.h live here now (the old VM is deleted).

namespace curlop {

// Default initial sizing hint for per-clip vectors (EngineSlot ctor /
// reset() pre-allocation before the first install) — NOT a cap.
static constexpr int kDefaultInitialModules = 32;
// Per-block RT event reserve budget (articulation trains can expand to many
// sub-hits). EngineSlot reserves this off-thread and VM hot paths fail closed
// with diagnostics at the budget instead of reallocating in processBlock.
// Debt: true de-cap needs a scheduler/emission storage design that can grow
// off-thread and atomically hand over a larger reserve, not a larger constant.
static constexpr int kEventVectorReserve = 20000;


// ── Per-module parameter layout descriptor (T-597 de-reserve) ──
// The layout is DECLARATION-DERIVED: paramBuffer row r == the module's r-th
// expanded-declaration input (post expandDeclarationVoices + the standard
// module-contract params PAN/IN_GAIN/OUT_GAIN appended). There are NO reserved
// channel slots and no fixed offsets — gate/pitch/velocity/pan/gains/declared
// params all live at their declaration position, addressed by NAME or by
// declared TYPE. `rowMap` (built in buildModuleVms) is consequently the
// identity: declaration idx i → row i.
//
// Build it with `computeLayoutFromDeclaration(layout, decl)` (DeclarationRowMap.h,
// which owns the vm::ModuleDeclaration dependency). This struct is the passive
// descriptor the audio path + schema seeders read.
struct ModuleLayout {
    uint32_t voices = 1;
    // T-480: receiver steal policy (vm::Delivery::StealPolicy numeric).
    // Rides the compile metadata layouts envelope; consumed by
    // EngineSlot::buildModuleVms when preparing the Delivery.
    uint8_t stealPolicy = 0;
    uint32_t inputs = 0;
    uint32_t outputs = 1;
    uint32_t totalParams = 0;   // == declaration input count

    // Declared module params in schema order. Populated by callers that know
    // the schema; feeds the declaration build (the synthesized fallback +
    // the byte-deck path). Carries declared-VALUE names only.
    std::vector<std::string> declaredParamNames;

    // SF-083: the ALL_CAPS names of value params that are PER-VOICE for this
    // clip (a {} voice stack / a per-voice modulator drives them). The
    // declaration build marks the matching ControlInput.perVoice so that input
    // expands ×voices (per-voice rows <NAME>_V<k>). Empty = all params shared
    // (the default). Populated by the compiler (script-side {} detection) and
    // carried so EngineSlot / ApgGraphSync / the byte-deck all expand
    // identically — the same convergence the T-597 declaration build needs.
    std::vector<std::string> perVoiceParams;

    bool isPerVoiceParam(const std::string& allCapsName) const noexcept {
        for (const auto& n : perVoiceParams) if (n == allCapsName) return true;
        return false;
    }

    // Row r → declared input SignalType (the de-reserve contract: a row is a
    // gate/pitch/velocity bus row IFF its declaration input is note-typed).
    // Filled by computeLayoutFromDeclaration.
    std::vector<vm::SignalType> rowType;

    // Name → buffer row. Populated by computeLayoutFromDeclaration. Includes
    // canonical note names (GATE_V0, PITCH_V0, VELOCITY), mono aliases
    // (GATE, PITCH, OUT_GAIN, IN_GAIN), the standard contract names
    // (PAN, IN_GAIN_<i>, OUT_GAIN_<o>, VOICES), and declared-param names in
    // both raw (Faust label) and ALL_CAPS-normalised forms.
    std::unordered_map<std::string, uint32_t> paramIndexByName;

    // Look up buffer row by name. Pure lookup — returns -1 on miss.
    int indexOf(const std::string& name) const noexcept {
        auto it = paramIndexByName.find(name);
        return it != paramIndexByName.end() ? static_cast<int>(it->second) : -1;
    }

    // Row-type queries replacing the reserved-offset formulas. A "note row"
    // is a gate/pitch/velocity bus row (direct realtime fill); a value row
    // routes through the Delivery knob baseline.
    vm::SignalType rowTypeAt(int r) const noexcept {
        return (r >= 0 && r < (int) rowType.size())
                   ? rowType[(size_t) r] : vm::SignalType::Value;
    }
    bool isGateRow(int r) const noexcept {
        return r >= 0 && r < (int) rowType.size()
               && rowType[(size_t) r] == vm::SignalType::Gate;
    }
    bool isNoteRow(int r) const noexcept {
        if (r < 0 || r >= (int) rowType.size()) return false;
        const auto t = rowType[(size_t) r];
        return t == vm::SignalType::Gate || t == vm::SignalType::Pitch
            || t == vm::SignalType::Velocity;
    }
    // Count of declared VALUE rows (declared params + the standard contract
    // params + VOICES) — the de-reserve replacement for totalParams-declaredStart.
    int valueRowCount() const noexcept {
        int n = 0;
        for (auto t : rowType) if (t == vm::SignalType::Value) ++n;
        return n;
    }
};

// ── Per-block data shared between VM and audio processors ──
// Filled by the VM each audio block. ScriptNodeProcessor reads paramBuffer
// to drive per-sample gate/pitch/velocity/params.
struct VMProcessData {
    // T-328 slice 4c-6: VMProcessData::MAX_MODULES alias retired.
    // Per-clip module count is per-slot dynamic; callers use
    // EngineSlot::moduleCapacity() (runtime). T-481: MAX_PARAMS is the
    // paramBuffer row FLOOR (the pre-layout default), never a ceiling —
    // installs size each module's declaration-derived note/value rows and
    // every row-bound check reads the live
    // paramBuffer.numParams(). MAX_BLOCK stays a real fixed cap
    // (sample-block ceiling = 2048).
    static constexpr int MAX_PARAMS = 64;
    static constexpr int MAX_BLOCK = 2048;

    // T-327 (T-295 slice 3b, SPEC-013 §The standard Family C) — per-module
    // param scratch storage, flat std::vector<float> sized
    // numParams * numSamples. Drop-in for the historical
    // `float paramBuffer[MAX_PARAMS][MAX_BLOCK]` shape:
    //   - `pb[p][s]` continues to work because `operator[](p)` returns
    //     `float*` (row pointer) and `float*[s]` indexes the sample
    //     identically.
    //   - Row pointer is what every existing consumer needs
    //     (ScriptNodeProcessor::bindParamBuffer for memcpy,
    //     ModulationTree::evaluate for std::fill_n etc.).
    //
    // Sized at SlotPool::prepareToPlay(samplesPerBlock) — message-thread
    // (JUCE prepareToPlay). The slot OWNS its paramBuffer storage in
    // place; no install-envelope swap (paramBuffer is per-block VM
    // scratch, recomputed each block, never carries survivor state).
    //
    // ~96 MB fixed-runtime reclaim once prepareToPlay shrinks the
    // typical 256-sample block (MAX_PARAMS * 256 = 16 KB) from the
    // default MAX_BLOCK construction (MAX_PARAMS * 2048 = 512 KB per
    // ModuleState × MAX_MODULES × 2 procdata × 3 slots ≈ 96 MB).
    struct ParamBufferAccess {
        std::vector<float> data_;
        int numSamples_ = 0;
        int numParams_  = 0;

        // Construct at MAX_PARAMS × MAX_BLOCK by default to match the
        // historical inline-array shape — code paths between EngineSlot
        // construction and SlotPool::prepareToPlay (e.g. test rigs that
        // bypass prepareToPlay) keep working without OOB. prepareToPlay
        // shrinks via fresh-vector-swap so the freed memory actually
        // returns to the allocator (std::vector::resize down keeps
        // capacity).
        ParamBufferAccess() { reallocate(MAX_PARAMS, MAX_BLOCK); }

        // Fresh-vector-swap shrink/grow: forces a new allocation at the
        // exact size requested + releases the old buffer. Message-
        // thread only (called from SlotPool::prepareToPlay).
        void reallocate(int numParams, int numSamples) {
            numParams_  = numParams;
            numSamples_ = numSamples;
            std::vector<float>((size_t) numParams * (size_t) numSamples, 0.0f)
                .swap(data_);
        }

        int numSamples() const { return numSamples_; }
        int numParams () const { return numParams_; }
        float*       data()       { return data_.data(); }
        const float* data() const { return data_.data(); }

        float* operator[](int p) {
            return data_.data() + (size_t) p * (size_t) numSamples_;
        }
        const float* operator[](int p) const {
            return data_.data() + (size_t) p * (size_t) numSamples_;
        }
    };

    struct ModuleState {
        ParamBufferAccess paramBuffer;
        float pitch = 440.0f;
        float velocity = 0.7f;

        // SF-065 / ADR-0019 polyphony survivor state. voicePitch[v] is the
        // per-voice mirror of `pitch` — the last frequency note-on lit voice v
        // with, held across audio blocks so a sustained chord's carry-forward
        // keeps each voice on its own note instead of collapsing to the root.
        // activeVoices is how many voices the current note lit; carry-forward
        // holds the gate high for [0, activeVoices) and low for the rest.
        //
        // Default-sized to MAX_PARAMS/2 on EVERY ModuleState construction (the
        // paramBuffer-style "always usable" default) so it can never desync from
        // the voice count regardless of which path creates / grows / resets the
        // ModuleState. A separately-sized vector silently fell back to mod.pitch
        // (the root) for v>=size, collapsing every chord voice to the root.
        // MAX_PARAMS=64 caps the paramBuffer at 64 rows ⇒ at most ~31 voices, so
        // 32 slots always cover the voice block — a consequence of the existing
        // cap, not a new one. ApgGraphSync re-asserts it per layout (+ resets
        // activeVoices on graph rebuild).
        std::vector<float> voicePitch = std::vector<float>(MAX_PARAMS / 2, 440.0f);
        uint32_t activeVoices = 0;
    };

    // T-327 (T-295 slice 3a, SPEC-013 §The standard Family C) — modules
    // moved from fixed-array to std::vector. EngineSlot construction creates a
    // default usable floor; clip installs swap in vectors sized to the active
    // module count. reset() clears live counters but does not rebuild this
    // scratch storage, because paramBuffer is recomputed each block and the
    // next install owns authoritative sizing.
    std::vector<ModuleState> modules;
    int moduleCount = 0;
    int blockSize = 0;

    float frequency = 440.0f;
    int diagPostSwapBlock = -1;
};

} // namespace curlop
