// F-066 — declared-control delivery (adr-vm §Modules, adr-modules §"How
// modules receive control").
//
// Nothing is hardwired: the module DECLARES its control inputs (the F-065
// faceplate-from-source capture: label + init/min/max + signal type); the
// delivery layer binds whatever the VM emits onto that list. Note-event
// parts route by TYPE — voice n lands on the n-th declared input of that
// type. Value lanes route through explicit bindings (the patch cable).
// There are no reserved channel positions and no magic zone names: a module
// with three triggers and no pitch is just as legal as a synth.
//
// Composition (adr-vm "How signals move") happens in NORMALIZED space:
//   final = base (set replaces, last wins; else the knob)
//         + summed offset locks
//         + summed modulation streams
// Release withdraws the emitting source. The declared range and curve map
// to real units only at the edge — the declaration is the converter.
//
// Per-sample throughout: delivery reads the machine's dense stream rows;
// there is no block-rate constant fill (B-241's decimation is structurally
// impossible here).
//
// Zero-JUCE, std-only.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <vector>

#include "graph/state/GraphStateLimits.h"
#include "Signal.h"
#include "Machine.h"
#include "modules/contract/ParameterValue.h"

namespace curlop::vm {

// How a declared input converts the normalized wire to its real unit.
enum class InputUnit : uint8_t {
    Linear,   // min + (v+1)/2 * (max-min)          (Value: bipolar wire)
    Exp,      // min * (max/min)^((v+1)/2)
    Hz,       // pitch signal (0.1/octave, 0 = middle C) -> Hertz
    Gate,     // public gate intensity in [0, 1]
    Signal,   // raw VM signal; no unit conversion at this boundary
    BinaryGate, // receiver-only gate; onset intensity feeds paired velocity
    TimeMilliseconds,
    TimeSeconds,
};

struct ControlInput {
    std::string name;
    SignalType  type = SignalType::Value;
    float min = 0.0f, max = 1.0f, def = 0.0f;
    InputUnit unit = InputUnit::Linear;
    // SF-083 (adr-modules §"Per-voice control"): a param is shared by default
    // and becomes per-voice when the author / control layer asks (a {} voice
    // stack or a per-voice modulator). A per-voice VALUE input is expanded ×
    // voices exactly like gate/pitch — its own channel per voice into the
    // per-voice DSP instance — instead of one shared channel. Set per-clip on
    // the declaration build (default false = shared, the existing behaviour).
    bool perVoice = false;
};

struct ModuleDeclaration {
    std::vector<ControlInput> inputs;
};

// T-477 (SF-065 polyphony) — voice-expand a module's source declaration.
// Gate, Pitch, and Velocity inputs become `voices` consecutive copies in place, so the
// type-ordinal IS the voice index (the routing contract above: voice n lands
// on the n-th declared input of that type). Value inputs are shared across
// voices unless explicitly marked perVoice.
//
// T-476 (ADR-0019 voice-cap model): expansion appends a VOICES value input —
// the script-writable/modulatable active_voices cap, clamped at render to
// the structural ceiling (`voices` = the schema's physical_voices). It rides
// the normal param machinery: faceplate knob, #mod.VOICES:n lock, ~lfo
// routes. The adapter consumes it (instances >= cap skip render); voice
// index is voicing ordinal (voice 0 = lowest), so the cap keeps the low N.
inline ModuleDeclaration expandDeclarationVoices(const ModuleDeclaration& base,
                                                 int voices)
{
    if (voices <= 1) return base;
    std::size_t perVoiceInputCount = 0;
    for (const auto& input : base.inputs)
        if (input.type == SignalType::Gate
            || input.type == SignalType::Pitch
            || input.type == SignalType::Velocity
            || input.perVoice)
            ++perVoiceInputCount;

    PhysicalVoicePreparationRequest preparation;
    preparation.voices = voices;
    preparation.baseInputCount = base.inputs.size();
    preparation.perVoiceInputCount = perVoiceInputCount;
    preparation.blockFrames = 1u;
    preparation.fixedBytesPerExpandedInput = sizeof(ControlInput);
    const auto checked = validatePhysicalVoicePreparation(preparation);
    if (! checked.ok())
        throw std::length_error(checked.diagnostic());

    ModuleDeclaration out;
    if (checked.expandedInputCount > out.inputs.max_size())
        throw std::length_error(
            "physical voice declaration exceeds container capacity");
    out.inputs.reserve(checked.expandedInputCount);
    for (const auto& in : base.inputs) {
        // Note-instance signals are per-voice by type; a Value input is per-voice only
        // when flagged (SF-083 — a {} stack / per-voice modulator). Either way
        // it expands ×voices with a .v<v> channel per voice.
        const bool expand = in.type == SignalType::Gate
                         || in.type == SignalType::Pitch
                         || in.type == SignalType::Velocity
                         || in.perVoice;
        if (expand) {
            for (int v = 0; v < voices; ++v) {
                ControlInput c = in;
                c.name = in.name + ".v" + std::to_string(v);
                out.inputs.push_back(std::move(c));
            }
        } else {
            out.inputs.push_back(in);
        }
    }
    ControlInput cap;
    cap.name = "VOICES";
    cap.type = SignalType::Value;
    cap.unit = InputUnit::Linear;
    cap.min  = 1.0f;
    cap.max  = (float) voices;
    cap.def  = (float) voices;
    out.inputs.push_back(std::move(cap));
    return out;
}

// T-597 (s514 ruling) — the standard module-contract params every module
// carries by default: PAN, one IN_GAIN per audio input, one OUT_GAIN per audio
// output. Ordinary declared Value inputs (controllable + sequenceable like any
// other param — "all modules equal"), NOT reserved slots. Appended to EVERY
// module declaration so layout rows follow declaration order with an identity
// rowMap. Audio application of pan/gain stays with the existing edge-pan /
// output-gain mechanisms; this gives the param its standard home + row.
inline void appendStandardModuleParams(ModuleDeclaration& decl,
                                       int inputs, int outputs)
{
    auto val = [] (std::string nm, float def, float mn, float mx) {
        ControlInput in;
        in.name = std::move(nm);
        in.type = SignalType::Value;
        in.unit = InputUnit::Linear;
        in.min = mn; in.max = mx; in.def = def;
        return in;
    };
    decl.inputs.push_back(val("PAN", 0.0f, -1.0f, 1.0f));
    for (int i = 0; i < inputs; ++i)
        decl.inputs.push_back(val("IN_GAIN_" + std::to_string(i), 1.0f, 0.0f, 2.0f));
    for (int o = 0; o < outputs; ++o)
        decl.inputs.push_back(val("OUT_GAIN_" + std::to_string(o), 1.0f, 0.0f, 2.0f));
}

// T-597 — the single declaration-finalizer every build site funnels through:
// voice-expand the base control declaration (gate/pitch ×voices + the appended
// VOICES cap for voices>1), then append the standard contract params. The
// result is THE addressing contract: rows = these inputs, in this order.
inline ModuleDeclaration finalizeModuleDeclaration(
    const ModuleDeclaration& base, int voices, int inputs, int outputs)
{
    ModuleDeclaration decl = expandDeclarationVoices(base, voices);
    appendStandardModuleParams(decl, inputs, outputs);
    return decl;
}

// One patch cable: VM value lane -> declared input index.
struct LaneBinding {
    uint32_t lane = 0;
    uint32_t inputIndex = 0;
    // Source-local named outputs project onto their own declared row and keep
    // the historical direct binding. A prepared graph route sets this flag so
    // the binding qualifies the destination socket but the receiving module's
    // note allocator still chooses the physical voice row.
    bool receiverAllocatesVoice = false;
};

class Delivery {
public:
    struct SourceTimingContext { SourceIdentity sourceIdentity = 0; float bpm = 120.0f; };
    void setSourceTimingContexts(const SourceTimingContext* contexts, size_t count) noexcept
    { sourceTimingContexts_ = contexts; sourceTimingContextCount_ = count; }
    // T-479 (SPEC-014 §6): when voice_idx arrivals exceed the physical
    // slots, the steal policy decides who renders — never truncation.
    // Default LastStolen per the ADR-0019 ruling; T-480 wires the
    // per-instance override.
    enum class StealPolicy : uint8_t { LastStolen, RoundRobin, OldestReleased };

    void setStealPolicy(StealPolicy p) { stealPolicy_ = p; }

    void prepare(ModuleDeclaration decl, std::vector<LaneBinding> bindings,
                 int maxBlockFrames, size_t eventReserve = 0)
    {
        if (maxBlockFrames <= 0)
            throw std::invalid_argument(
                "delivery preparation requires a positive block size");

        const size_t n = decl.inputs.size();
        if (n > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error(
                "delivery preparation exceeds addressable input count");

        PhysicalVoicePreparationRequest preparation;
        preparation.voices = 1;
        preparation.baseInputCount = n;
        preparation.blockFrames =
            static_cast<std::size_t>(maxBlockFrames);
        preparation.fixedBytesPerExpandedInput =
            sizeof(ControlInput)
            + sizeof(InputState)
            + sizeof(std::vector<float>)
            + sizeof(std::vector<std::uint8_t>);
        preparation.bytesPerExpandedInputFrame =
            sizeof(float) + sizeof(std::uint8_t);
        const auto checked = validatePhysicalVoicePreparation(preparation);
        if (! checked.ok())
            throw std::length_error(checked.diagnostic());

        // Prepare every owning container locally. If the allocator rejects
        // the request, the currently published Delivery remains untouched.
        std::vector<std::vector<float>> outputs(
            n,
            std::vector<float>(
                static_cast<std::size_t>(maxBlockFrames),
                0.0f));
        std::vector<std::vector<std::uint8_t>> baseMask(
            n,
            std::vector<std::uint8_t>(
                static_cast<std::size_t>(maxBlockFrames),
                0));
        std::vector<std::uint8_t> noteStreamFilled(n, 0);
        std::vector<InputState> states(n, InputState {});

        // B-1742: bindings are immutable after prepare. Keep them lane-sorted
        // so callback lookups visit only the intentional fanout range instead
        // of scanning every graph binding for every packet/stream row.
        std::stable_sort(
            bindings.begin(), bindings.end(),
            [] (const LaneBinding& first, const LaneBinding& second) {
                return first.lane < second.lane;
            });
        std::vector<std::vector<uint32_t>> bindingChannelInputs(
            bindings.size());
        for (size_t bindingIndex = 0;
             bindingIndex < bindings.size();
             ++bindingIndex) {
            const auto& binding = bindings[bindingIndex];
            if (binding.inputIndex >= n)
                continue;
            auto& targets = bindingChannelInputs[bindingIndex];
            const auto& input = decl.inputs[binding.inputIndex];
            const auto suffix = input.name.rfind(".v0");
            if (binding.receiverAllocatesVoice
                && suffix != std::string::npos
                && suffix + 3u == input.name.size()) {
                const auto prefix =
                    input.name.substr(0, suffix) + ".v";
                for (size_t candidate = 0;
                     candidate < decl.inputs.size();
                     ++candidate)
                    if (decl.inputs[candidate].type == input.type
                        && decl.inputs[candidate].name.compare(
                               0, prefix.size(), prefix) == 0)
                        targets.push_back(
                            static_cast<uint32_t>(candidate));
            }
            if (targets.empty())
                targets.push_back(binding.inputIndex);
        }

        // B-1734: Value Set/Offset ownership is one contribution per
        // (source identity, lane, target input). One event may fan out through
        // every valid matching binding, so prepare for the largest lane fanout
        // across the event budget without growing on the audio thread.
        size_t maxBindingFanOut = 0;
        for (size_t first = 0; first < bindings.size();) {
            size_t last = first + 1u;
            while (last < bindings.size()
                   && bindings[last].lane == bindings[first].lane)
                ++last;
            size_t fanOut = 0;
            for (size_t binding = first; binding < last; ++binding)
                if (bindings[binding].inputIndex < n)
                    ++fanOut;
            maxBindingFanOut = std::max(maxBindingFanOut, fanOut);
            first = last;
        }
        if (maxBindingFanOut > 0
            && eventReserve
                > std::numeric_limits<size_t>::max() / maxBindingFanOut)
            throw std::length_error(
                "delivery contribution reserve exceeds addressable size");
        const size_t eventContributionReserve =
            eventReserve * maxBindingFanOut;
        const size_t contributionReserve =
            std::max(bindings.size(), eventContributionReserve);
        std::vector<ValueContribution> valueContributions;
        valueContributions.reserve(contributionReserve);
        std::vector<PendingPitch> pendingPitch;
        std::vector<PendingVelocity> pendingVelocity;
        if (eventReserve > 0)
        {
            pendingPitch.reserve(eventReserve);
            pendingVelocity.reserve(eventReserve);
        }

        // Voice routing tables: the n-th declared input of a note-signal
        // type receives voice n. Declaration order is the voice order.
        std::vector<uint32_t> gateInputs;
        std::vector<uint32_t> pitchInputs;
        std::vector<uint32_t> velInputs;
        std::vector<std::uint8_t> fixedNamedNoteInputs(n, 0u);
        std::vector<std::uint8_t> allocatedNoteInputs(n, 0u);
        for (const auto& binding : bindings) {
            if (binding.inputIndex >= n)
                continue;
            auto& marked = binding.receiverAllocatesVoice
                ? allocatedNoteInputs[binding.inputIndex]
                : fixedNamedNoteInputs[binding.inputIndex];
            marked = 1u;
        }
        for (size_t i = 0; i < n; ++i) {
            states[i].knob  = toNormalizedKnob(decl.inputs[i]);
            states[i].value = states[i].knob;
            // A directly bound named note output is a fixed output row, not
            // another physical synth voice. Keep it out of the allocator
            // unless a prepared receiving route also explicitly qualifies
            // that same input for receiver-owned voice allocation.
            const bool fixedNamedNote =
                fixedNamedNoteInputs[i] != 0u
                && allocatedNoteInputs[i] == 0u;
            if (fixedNamedNote)
                continue;
            const auto inputIndex = static_cast<std::uint32_t>(i);
            switch (decl.inputs[i].type) {
                case SignalType::Gate:
                    gateInputs.push_back(inputIndex);
                    break;
                case SignalType::Pitch:
                    pitchInputs.push_back(inputIndex);
                    break;
                case SignalType::Velocity:
                    velInputs.push_back(inputIndex);
                    break;
                default: break;
            }
        }

        // T-479: one allocator slot per physical voice (gate-input count).
        std::vector<VoiceSlot> slots(gateInputs.size(), VoiceSlot {});

        decl_ = std::move(decl);
        bindings_ = std::move(bindings);
        bindingChannelInputs_ =
            std::move(bindingChannelInputs);
        outputs_ = std::move(outputs);
        baseMask_ = std::move(baseMask);
        noteStreamFilled_ = std::move(noteStreamFilled);
        states_ = std::move(states);
        gateInputs_ = std::move(gateInputs);
        pitchInputs_ = std::move(pitchInputs);
        velInputs_ = std::move(velInputs);
        slots_ = std::move(slots);
        valueContributions_ = std::move(valueContributions);
        pendingPitch_ = std::move(pendingPitch);
        pendingVelocity_ = std::move(pendingVelocity);
        maxFrames_ = maxBlockFrames;
        rtBounded_ = eventReserve > 0;
        rtOverflowDropsThisBlock_ = 0;
        contributionSequence_ = 0;
        allocSeq_ = 0;
        nextRoundRobin_ = 0;
    }

    // F-066 Phase 5: knob baseline update (the ParamStore→AudioParamState
    // pipeline feeds this per block, normalized). The knob is the base layer
    // when no set lock holds; an active set lock keeps winning until its
    // Release. RT-safe: no allocation, applies from the next process().
    // B-304 — inverse of toUnit for value-like units: a REAL-unit value →
    // its knob-position on the normalized wire, through the declared curve.
    // The knob feed (VMRunner) and the declared default both go through this
    // so an Exp param's base composes positionally, not as a linear fraction.
    static float valueToNormalized(const ControlInput& in, float v)
    {
        switch (in.unit) {
            case InputUnit::Linear:
                if (in.type == SignalType::Velocity)
                    return in.max > in.min
                        ? (v - in.min) / (in.max - in.min) : 0.0f;
                return in.max > in.min
                    ? ((v - in.min) / (in.max - in.min)) * 2.0f - 1.0f : 0.0f;
            case InputUnit::Exp:
                return (v > 0.0f && in.min > 0.0f && in.max > in.min)
                    ? (std::log(v / in.min) / std::log(in.max / in.min)) * 2.0f - 1.0f
                    : -1.0f;
            case InputUnit::Signal:
                return v;
            default:
                return 0.0f;
        }
    }

    void setKnob(uint32_t inputIndex, float normalized)
    {
        if (inputIndex >= states_.size())
            return;
        InputState& st = states_[inputIndex];
        st.knob = normalized;
        st.value = composeValue(st);
    }

    // Compose one block. `emissions` are the machine's (frame-sorted)
    // outputs for this block; `streams` its stream rows.
    void process(const std::vector<Emission>& emissions,
                 const std::vector<StreamRow>& streams, int numFrames)
    {
        const std::vector<StreamRow>* one[1] = { &streams };
        const uint32_t zero[1] = { 0 };
        processMulti(emissions, one, zero, 1, numFrames, 120.0);
    }

    // F-066 Phase 5: reset the composed state to the knob baseline — gates
    // off, set/offset contributions withdrawn. RT-safe (no allocation;
    // clear() keeps prepared capacity). The transport-stop / hard-reset
    // analog of the old ModulationTree::clearAll.
    void resetComposition()
    {
        valueContributions_.clear();
        contributionSequence_ = 0;
        for (auto& st : states_) {
            st.hasSet = false;
            st.setVal = 0.0f;
            st.offsetSum = 0.0f;
            st.offsetCount = 0;
            st.baseFromStream = false;
            st.value = st.knob;
        }
        // T-479: gates reset — the allocator state resets with them.
        for (auto& s : slots_)
            s = VoiceSlot {};
        allocSeq_ = 0;
        nextRoundRobin_ = 0;
    }

    // F-066 Phase 5: display introspection (GUI presence bits).
    bool inputHasSet(size_t i) const
    {
        return i < states_.size() && states_[i].hasSet;
    }
    bool inputHasOffsets(size_t i) const
    {
        return i < states_.size() && states_[i].offsetCount > 0;
    }
    // B-269: a routed generator stream is this input's base this block —
    // the GUI shows it as modulation presence.
    bool inputHasStreamBase(size_t i) const
    {
        return i < states_.size() && states_[i].baseFromStream;
    }

    // ── SPEC-014 lock-layering display taps (param-pipeline-manager.md
    // §12 #6/#7). The GUI paints two independent layers per knob:
    //   abs layer    → pin marker (pr=1 row, ev = the BASE value)
    //   offset layer → dotted arc (pr=2 row, ev = the COMPOSED value)
    // Presence derives from the same per-block composition state the audio
    // resolves from — absent ⇒ inactive, structurally self-clearing.
    bool inputAbsActive(size_t i) const
    {
        return i < states_.size()
            && (states_[i].hasSet || states_[i].baseFromStream);
    }
    bool inputOffsetActive(size_t i) const
    {
        return i < states_.size()
            && (states_[i].offsetCount > 0 || states_[i].offsetFromStream);
    }
    // Abs-layer (base) value in REAL units: set lock > routed stream > knob.
    float inputAbsUnit(size_t i) const
    {
        if (i >= states_.size() || i >= decl_.inputs.size())
            return 0.0f;
        const InputState& st = states_[i];
        const float baseNorm = st.hasSet ? st.setVal
                             : (st.baseFromStream ? st.streamBaseLast : st.knob);
        return toUnit(decl_.inputs[i], baseNorm);
    }
    float inputComposedNorm(size_t i) const
    {
        return i < states_.size() ? states_[i].value : 0.0f;
    }
    const ModuleDeclaration& declaration() const { return decl_; }
    size_t bindingCount() const { return bindings_.size(); }
    int firstBoundInput() const
    {
        return bindings_.empty() ? -1 : (int) bindings_[0].inputIndex;
    }

    // F-066 Phase 5: live-edit state carry. applyInstall copies the outgoing
    // delivery's NOTE-input state (gate/pitch/velocity) into THIS (freshly-
    // prepared) delivery so a sounding note survives a live edit — while the
    // new declaration + bindings stay in force. Carrying the old OBJECT
    // instead would resurrect its stale lane map: a delivery built before the
    // source artifacts existed (state-restore order) has zero bindings, and
    // since input counts rarely change it would shadow every later install
    // (the s489 binds=0 / dead-declared-params regression).
    //
    // Value-input lock state does NOT carry: locks are step-scoped with a
    // Release owned by the OLD program — after a swap that Release never
    // fires, so a carried Set pins the param forever (live-verified: level:0
    // leaked across a program change and muted the module). The new program
    // re-asserts its own locks at its next step boundary; until then the
    // knob baseline stands — same semantics as the old VM's install-time
    // REBUILD_MOD_TREE composition reset. RT-safe: bounded element copies.
    void migrateStateFrom(const Delivery& old)
    {
        const size_t n = std::min(states_.size(), old.states_.size());
        for (size_t i = 0; i < n; ++i) {
            if (i >= decl_.inputs.size()) continue;
            const SignalType t = decl_.inputs[i].type;
            if (t == SignalType::Value)
                continue;   // locks stay with their owning program
            // B-276 (s494): gates don't carry HERE — a carried gate=1 whose
            // off-schedule died with the old machine is an eternal drone.
            // B-283 unifies: applyInstall carries a voice's gate via
            // carryGateFrom ONLY when it also adopted that voice's pending
            // off-schedule into the new machine (Machine::adoptGateOff), so
            // a carried note both survives the edit and releases on time.
            if (t == SignalType::Gate)
                continue;
            states_[i].value = old.states_[i].value;
        }
    }

    // B-283: composed gate level of LOGICAL voice v (0 when unmapped).
    float gateLevel(SourceIdentity sourceIdentity,
                    uint32_t voice,
                    uint64_t stableChannelId) const
    {
        const int s =
            slotOfLogical(sourceIdentity, voice, stableChannelId);
        if (s < 0 || (size_t) s >= gateInputs_.size()) return 0.0f;
        const size_t gi = gateInputs_[(size_t) s];
        return gi < states_.size() ? states_[gi].value : 0.0f;
    }
    float gateLevel(SourceIdentity sourceIdentity, uint32_t voice) const
    {
        float composedGate = 0.0f;
        for (size_t s = 0; s < slots_.size(); ++s) {
            if (slots_[s].sourceIdentity != sourceIdentity
                || slots_[s].logical != voice
                || s >= gateInputs_.size())
                continue;
            const size_t input = gateInputs_[s];
            if (input < states_.size())
                composedGate =
                    std::max(composedGate, states_[input].value);
        }
        return composedGate;
    }
    float gateLevel(uint32_t voice) const
    {
        return gateLevel(0u, voice);
    }

    // B-283: carry ONE logical voice's gate state across a live edit.
    // Caller (applyInstall) pairs this with Machine::adoptGateOff for the
    // same voice — never carry a gate whose off-schedule did not carry.
    // T-479: the old allocator's slot binding replicates at the SAME slot
    // index (the ordinal pitch carry in migrateStateFrom lines up with it),
    // so the adopted off finds its voice.
    void carryGateFrom(const Delivery& old,
                       SourceIdentity sourceIdentity,
                       uint32_t voice,
                       uint64_t stableChannelId)
    {
        const int sOld =
            old.slotOfLogical(sourceIdentity, voice, stableChannelId);
        if (sOld < 0 || (size_t) sOld >= slots_.size()) return;
        const size_t di = gateInputs_[(size_t) sOld];
        const size_t si = old.gateInputs_[(size_t) sOld];
        if (di >= states_.size() || si >= old.states_.size()) return;
        states_[di].value = old.states_[si].value;
        if ((size_t) sOld < pitchInputs_.size()
            && (size_t) sOld < old.pitchInputs_.size()) {
            const size_t pitchDst = pitchInputs_[(size_t) sOld];
            const size_t pitchSrc = old.pitchInputs_[(size_t) sOld];
            if (pitchDst < states_.size() && pitchSrc < old.states_.size())
                states_[pitchDst].value = old.states_[pitchSrc].value;
        }
        if ((size_t) sOld < velInputs_.size()
            && (size_t) sOld < old.velInputs_.size()) {
            const size_t velocityDst = velInputs_[(size_t) sOld];
            const size_t velocitySrc = old.velInputs_[(size_t) sOld];
            if (velocityDst < states_.size() && velocitySrc < old.states_.size())
                states_[velocityDst].value = old.states_[velocitySrc].value;
        }
        VoiceSlot& slot = slots_[(size_t) sOld];
        slot.sourceIdentity = sourceIdentity;
        slot.stableChannelId = stableChannelId;
        slot.logical   = voice;
        slot.held      = old.slots_[(size_t) sOld].held;
        slot.lastOnSeq = ++allocSeq_;
    }
    void carryGateFrom(const Delivery& old,
                       SourceIdentity sourceIdentity,
                       uint32_t voice)
    {
        int selected = -1;
        for (size_t s = 0; s < old.slots_.size(); ++s) {
            const auto& candidate = old.slots_[s];
            if (! candidate.held
                || candidate.sourceIdentity != sourceIdentity
                || candidate.logical != voice)
                continue;
            if (candidate.stableChannelId == 0u) {
                selected = static_cast<int>(s);
                break;
            }
            if (selected >= 0)
                return;
            selected = static_cast<int>(s);
        }
        if (selected >= 0)
            carryGateFrom(
                old, sourceIdentity, voice,
                old.slots_[static_cast<size_t>(selected)].stableChannelId);
    }
    void carryGateFrom(const Delivery& old, uint32_t voice)
    {
        carryGateFrom(old, 0u, voice);
    }

    // F-066 Phase 5: multi-machine composition. A module can be driven by
    // several script programs (one Machine per (script-node, module)
    // projection — T-309: core.script nodes are peer sequencers). The host
    // merges their emissions frame-sorted into one list and passes every
    // machine's stream rows here. RT-safe: callers pass preallocated arrays.
    // setLaneOffsets[k] namespaces set k's compiler-local lanes into the
    // module's concatenated lane space (lanes are baked into bytecode
    // operands, so the offset applies at composition time). The caller is
    // responsible for offsetting Value-lane EMISSIONS when merging.
    void processMulti(const std::vector<Emission>& emissions,
                      const std::vector<StreamRow>* const* streamSets,
                      const uint32_t* setLaneOffsets,
                      int numStreamSets, int numFrames, double bpm = 120.0)
    {
        processBpm_ = bpm;
        for (auto& contribution : valueContributions_)
            if (contribution.typed) {
                float sourceTempo = contribution.sourceTempo;
                for (size_t c = 0; c < sourceTimingContextCount_; ++c)
                    if (sourceTimingContexts_[c].sourceIdentity == contribution.sourceIdentity)
                        sourceTempo = sourceTimingContexts_[c].bpm;
                const float resolved = resolveTyped(contribution.inputIndex,
                    contribution.authoredValue, contribution.authoredBasis,
                    contribution.authoredStepBeats, sourceTempo);
                if (contribution.hasSet) contribution.setValue = resolved;
                if (contribution.hasOffset) contribution.offsetValue = resolved;
            }
        for (size_t i = 0; i < states_.size(); ++i)
            if (decl_.inputs[i].type == SignalType::Value)
                refreshValueState(static_cast<uint32_t>(i), states_[i]);
        rtOverflowDropsThisBlock_ = 0;
        if (numFrames > maxFrames_)
            return;

        // Pass 0 — B-269 (Neo ruling s490): a Stream-kind row routed at an
        // input IS that input's base — ~lfo(min,max) sweeps the actual
        // declared range and overrides the knob for the route's scope.
        // Flag those inputs so Pass 1 composes a zero base placeholder
        // (offsets still sum on top); the stream lands in Pass 2. A
        // discrete set lock outranks the stream for its step scope
        // (tracked per sample via baseMask_). Offset-kind rows (% routes)
        // keep summing as before.
        for (auto& st : states_) {
            st.baseFromStream   = false;
            st.offsetFromStream = false;
            st.streamBaseLast   = 0.0f;
        }
        for (int setIdx = 0; setIdx < numStreamSets; ++setIdx) {
            if (streamSets[setIdx] == nullptr)
                continue;
            const uint32_t laneOff = setLaneOffsets ? setLaneOffsets[setIdx] : 0u;
            for (const auto& row : *streamSets[setIdx]) {
                if (! row.active || row.type != SignalType::Value)
                    continue;
                const auto range = bindingsForLane(row.lane + laneOff);
                for (auto binding = range.first;
                     binding != range.second;
                     ++binding) {
                    const auto input =
                        addressedBindingInput(
                            &*binding, row.channelIndex);
                    if (input < decl_.inputs.size()) {
                        if (row.kind == PacketKind::Stream)
                            states_[input].baseFromStream = true;
                        else if (row.kind == PacketKind::Offset)
                            states_[input].offsetFromStream = true;
                    }
                }
            }
        }
        // Re-derive Value-input running values under the (possibly changed)
        // base rule. Note-typed inputs are event-held (gate stays up across
        // blocks) — never formula-recomposed.
        for (size_t i = 0; i < states_.size() && i < decl_.inputs.size(); ++i)
            if (decl_.inputs[i].type == SignalType::Value)
                states_[i].value = composeValue(states_[i]);

        // Pass 1 — event-composed base, per input, in normalized space.
        // Walk emissions in frame order, filling each input's buffer with
        // the running composed value between events.
        for (auto& st : states_)
            st.fillFrom = 0;
        pendingPitch_.clear();   // T-479: deferred pitches are block-local
        pendingVelocity_.clear();

        for (const auto& e : emissions) {
            const int at = std::min(numFrames, (int) e.frameOffset);
            deferredPitchInput_ = -1;
            deferredVelocityInput_ = -1;
            if (e.type == SignalType::Value) {
                // Discrete Value packets fan out exactly like stream rows.
                // Named outputs decode only for binding lookup: the raw lane
                // remains in contribution identity so named and ordinary
                // source sockets cannot withdraw each other's ownership.
                // Invalid stale bindings are ignored before any state access.
                const uint32_t bindingLane = isNamedOutputLane(e.lane)
                    ? namedOutputLaneIndex(e.lane)
                    : e.lane;
                const auto range = bindingsForLane(bindingLane);
                for (auto binding = range.first;
                     binding != range.second;
                     ++binding) {
                    const auto input =
                        addressedBindingInput(
                            &*binding, e.channelIndex);
                    if (input >= states_.size())
                        continue;
                    if (binding->receiverAllocatesVoice
                        && input < decl_.inputs.size()
                        && decl_.inputs[input].type
                            != SignalType::Value)
                        continue;
                    InputState& st = states_[input];
                    fillTo(static_cast<int>(input), at, numFrames);
                    applyEvent(input, st, e);
                }
                continue;
            }
            const int target = targetInputOf(e);
            // T-479: a gate-on that stole a slot carries its voice's
            // deferred pitch — land it on the slot's pitch input at the
            // same frame, before the gate applies.
            if (deferredPitchInput_ >= 0) {
                InputState& ps = states_[(size_t) deferredPitchInput_];
                fillTo(deferredPitchInput_, at, numFrames);
                ps.value = deferredPitchValue_;
            }
            if (deferredVelocityInput_ >= 0) {
                InputState& vs = states_[(size_t) deferredVelocityInput_];
                fillTo(deferredVelocityInput_, at, numFrames);
                vs.value = deferredVelocityValue_;
            }
            if (target < 0)
                continue;
            InputState& st = states_[(size_t) target];
            fillTo(target, at, numFrames);
            applyEvent(static_cast<uint32_t>(target), st, e);
        }
        for (int i = 0; i < (int) decl_.inputs.size(); ++i)
            fillTo(i, numFrames, numFrames);

        // Pass 2 — streams (per sample, bound lanes). Stream-kind rows fill
        // the base placeholder (skipping samples where a discrete set lock
        // held — baseMask_); Offset-kind rows sum unconditionally.
        std::fill(noteStreamFilled_.begin(), noteStreamFilled_.end(), 0);
        for (int setIdx = 0; setIdx < numStreamSets; ++setIdx) {
            if (streamSets[setIdx] == nullptr)
                continue;
            const uint32_t laneOff = setLaneOffsets ? setLaneOffsets[setIdx] : 0u;
            for (const auto& row : *streamSets[setIdx]) {
                if (! row.active)
                    continue;
                const auto range = bindingsForLane(row.lane + laneOff);
                const bool hasExplicitBinding =
                    range.first != range.second;
                for (auto binding = range.first;
                     binding != range.second;
                     ++binding) {
                    const auto& b = *binding;
                    const auto input =
                        addressedBindingInput(
                            &b, row.channelIndex);
                    if (input >= decl_.inputs.size())
                        continue;
                    auto& out = outputs_[input];
                    if (row.kind == PacketKind::Stream) {
                        const bool noteTyped = decl_.inputs[input].type == SignalType::Gate
                                            || decl_.inputs[input].type == SignalType::Pitch
                                            || decl_.inputs[input].type == SignalType::Velocity;
                        const bool firstNoteStream = noteTyped
                            && noteStreamFilled_[input] == 0;
                        const auto& mask = baseMask_[input];
                        for (int i = 0; i < numFrames; ++i)
                            if (mask[(size_t) i] == 0) {
                                if (firstNoteStream)
                                    out[(size_t) i] = row.data[(size_t) i];
                                else
                                    out[(size_t) i] += row.data[(size_t) i];
                            }
                        if (noteTyped)
                            noteStreamFilled_[input] = 1;
                        // Abs-layer display value: the stream base's last
                        // sample (summed if several streams share the input).
                        states_[input].streamBaseLast +=
                            row.data[(size_t) numFrames - 1];
                    } else {
                        for (int i = 0; i < numFrames; ++i)
                            out[(size_t) i] += row.data[(size_t) i];
                    }
                }
                // Pitch-typed streams (glide) replace the composed pitch on
                // the legacy voice-0 pitch input only when no graph binding
                // addresses the row explicitly.
                if (row.type == SignalType::Pitch
                    && ! hasExplicitBinding
                    && ! pitchInputs_.empty()) {
                    auto& out = outputs_[pitchInputs_[0]];
                    for (int i = 0; i < numFrames; ++i)
                        out[(size_t) i] = row.data[(size_t) i];
                }
            }
        }

        // Pass 3 — the declaration converts to real units at the edge.
        for (size_t i = 0; i < decl_.inputs.size(); ++i) {
            const ControlInput& in = decl_.inputs[i];
            auto& out = outputs_[i];
            for (int s = 0; s < numFrames; ++s)
                out[(size_t) s] = toUnit(in, out[(size_t) s]);
        }
    }

    const std::vector<std::vector<float>>& outputs() const { return outputs_; }

    uint32_t drainRtOverflowDrops()
    {
        const uint32_t n = rtOverflowDropsThisBlock_;
        rtOverflowDropsThisBlock_ = 0;
        return n;
    }

private:
    struct InputState {
        float value   = 0.0f;     // running composed normalized value
        float knob    = 0.0f;     // base when no set lock / routed stream holds
        bool  hasSet  = false;
        float setVal  = 0.0f;
        // B-269: a Stream-kind row is routed at this input THIS block — the
        // knob is overridden; Pass 1 composes a zero placeholder the stream
        // fills in Pass 2. Re-derived every block (rows reset per block).
        bool  baseFromStream = false;
        // SPEC-014 display: an Offset-kind row (% route) is summing at this
        // input this block (dotted-arc layer); streamBaseLast = the stream
        // base's last sample for the abs-pin display value.
        bool  offsetFromStream = false;
        float streamBaseLast   = 0.0f;
        float offsetSum = 0.0f;
        size_t offsetCount = 0;
        int   fillFrom = 0;
    };

    // The one composition formula (adr-vm + B-269): base = set lock (its
    // step scope) > routed stream (zero placeholder here; the actual stream
    // lands in Pass 2) > knob; offsets always sum over the base.
    float composeValue(const InputState& st) const
    {
        float v = st.hasSet ? st.setVal
                            : (st.baseFromStream ? 0.0f : st.knob);
        return v + st.offsetSum;
    }

    // B-1734: a routed Value contribution is owned by the exact
    // (source identity, lane, input) tuple. Set and Offset may coexist for
    // that owner; Release withdraws only this record. Set sequence supplies
    // deterministic latest-active precedence and released-winner fallback.
    struct ValueContribution {
        uint32_t inputIndex = 0;
        SourceIdentity sourceIdentity = 0;
        uint32_t lane = 0;
        bool hasSet = false;
        bool hasOffset = false;
        float setValue = 0.0f;
        float offsetValue = 0.0f;
        uint64_t setSequence = 0;
        bool typed = false;
        float authoredValue = 0.0f;
        uint8_t authoredBasis = 0;
        float authoredStepBeats = 1.0f;
        float sourceTempo = 0.0f;
    };

    float resolveTyped(uint32_t inputIndex, float value, uint8_t basis,
                       float stepBeats, float sourceTempo = 0.0f) const
    {
        if (inputIndex >= decl_.inputs.size()) return 0.0f;
        const auto& in = decl_.inputs[inputIndex];
        const bool conventionalTime = in.name == "time" || in.name == "TIME";
        param::ParameterDeclaration target {
            in.min, in.max,
            (in.unit == InputUnit::TimeMilliseconds || conventionalTime) ? "ms" :
                (in.unit == InputUnit::TimeSeconds || in.unit == InputUnit::Exp ? "s" :
                 (in.unit == InputUnit::Hz ? "hz" : "")),
            in.unit == InputUnit::Exp ? param::Scale::Logarithmic : param::Scale::Linear
        };
        const auto basisValue = static_cast<param::AuthoredBasis>(basis);
        const double timingBpm = basisValue == param::AuthoredBasis::TempoBeats
            ? processBpm_ : (sourceTempo > 0.0f ? sourceTempo : processBpm_);
        const auto converted = param::convertSet(
            target, { value, static_cast<param::AuthoredBasis>(basis) },
            { timingBpm, stepBeats });
        return converted.ok ? static_cast<float>(converted.normalized) : 0.0f;
    }

    ValueContribution* findValueContribution(uint32_t inputIndex,
                                             SourceIdentity sourceIdentity,
                                             uint32_t lane)
    {
        for (auto& contribution : valueContributions_)
            if (contribution.inputIndex == inputIndex
                && contribution.sourceIdentity == sourceIdentity
                && contribution.lane == lane)
                return &contribution;
        return nullptr;
    }

    ValueContribution* getOrAppendValueContribution(
        uint32_t inputIndex,
        SourceIdentity sourceIdentity,
        uint32_t lane)
    {
        if (auto* existing =
                findValueContribution(inputIndex, sourceIdentity, lane))
            return existing;
        if (! rtBounded_
            || valueContributions_.size() < valueContributions_.capacity()) {
            valueContributions_.push_back(
                { inputIndex, sourceIdentity, lane });
            return &valueContributions_.back();
        }
        ++rtOverflowDropsThisBlock_;
        return nullptr;
    }

    void refreshValueState(uint32_t inputIndex, InputState& st)
    {
        const ValueContribution* winner = nullptr;
        float offsetSum = 0.0f;
        size_t offsetCount = 0;
        for (const auto& contribution : valueContributions_) {
            if (contribution.inputIndex != inputIndex)
                continue;
            if (contribution.hasSet
                && (winner == nullptr
                    || contribution.setSequence > winner->setSequence))
                winner = &contribution;
            if (contribution.hasOffset) {
                offsetSum += contribution.offsetValue;
                ++offsetCount;
            }
        }
        st.hasSet = winner != nullptr;
        st.setVal = winner != nullptr ? winner->setValue : 0.0f;
        st.offsetSum = offsetSum;
        st.offsetCount = offsetCount;
        st.value = composeValue(st);
    }

    bool appendPendingPitch(SourceIdentity sourceIdentity,
                            uint32_t voice,
                            uint64_t stableChannelId,
                            float value)
    {
        for (auto& pp : pendingPitch_)
            if (pp.sourceIdentity == sourceIdentity
                && pp.voice == voice
                && pp.stableChannelId == stableChannelId) {
                pp.value = value;
                return true;
            }
        if (! rtBounded_ || pendingPitch_.size() < pendingPitch_.capacity()) {
            pendingPitch_.push_back(
                { sourceIdentity, voice, stableChannelId, value });
            return true;
        }
        ++rtOverflowDropsThisBlock_;
        return false;
    }

    bool appendPendingVelocity(SourceIdentity sourceIdentity,
                               uint32_t voice,
                               uint64_t stableChannelId,
                               float value)
    {
        for (auto& pending : pendingVelocity_)
            if (pending.sourceIdentity == sourceIdentity
                && pending.voice == voice
                && pending.stableChannelId == stableChannelId) {
                pending.value = value;
                return true;
            }
        if (! rtBounded_ || pendingVelocity_.size() < pendingVelocity_.capacity()) {
            pendingVelocity_.push_back(
                { sourceIdentity, voice, stableChannelId, value });
            return true;
        }
        ++rtOverflowDropsThisBlock_;
        return false;
    }

    static float toNormalizedKnob(const ControlInput& in)
    {
        switch (in.unit) {
            case InputUnit::Linear:
            case InputUnit::Exp:
            case InputUnit::TimeMilliseconds:
            case InputUnit::TimeSeconds:
                return valueToNormalized(in, in.def);
            case InputUnit::Hz: {
                // T-466 — a declared freq input's default is in HERTZ (CP2
                // capture: sine.dsp freq def=440); the wire idles at the
                // equivalent pitch signal so toUnit round-trips it. def<=0
                // (the synthesized legacy shape) idles at signal 0 = middle C.
                if (in.def > 0.0f) {
                    const float midi = 69.0f + 12.0f * std::log2(in.def / 440.0f);
                    return noteToSignal(midiToNote(midi));
                }
                return 0.0f;
            }
            case InputUnit::Gate:
            case InputUnit::BinaryGate: return 0.0f;
            case InputUnit::Signal: return in.def;
        }
        return 0.0f;
    }

    static float toUnit(const ControlInput& in, float v)
    {
        switch (in.unit) {
            case InputUnit::Linear: {
                if (in.type == SignalType::Velocity) {
                    const float t = std::min(1.0f, std::max(0.0f, v));
                    return in.min + t * (in.max - in.min);
                }
                const float t = std::min(1.0f, std::max(-1.0f, v)) * 0.5f + 0.5f;
                return in.min + t * (in.max - in.min);
            }
            case InputUnit::TimeMilliseconds:
            case InputUnit::TimeSeconds: {
                const float t = std::min(1.0f, std::max(-1.0f, v)) * 0.5f + 0.5f;
                return in.min + t * (in.max - in.min);
            }
            case InputUnit::Exp: {
                const float t = std::min(1.0f, std::max(-1.0f, v)) * 0.5f + 0.5f;
                return (in.min > 0.0f && in.max > in.min)
                    ? in.min * std::pow(in.max / in.min, t) : in.min;
            }
            case InputUnit::Hz: {
                const float midi = noteToMidi(signalToNote(v));
                return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
            }
            case InputUnit::Gate:
                return isGateOpen(v)
                    ? std::min(1.0f, std::max(0.0f, v))
                    : 0.0f;
            case InputUnit::BinaryGate:
                return isGateOpen(v) ? 1.0f : 0.0f;
            case InputUnit::Signal:
                return v;
        }
        return v;
    }

    // ── T-479 voice allocator (SPEC-014 §6) ──────────────────────────
    // The substrate's voice_idx is an unbounded logical tag; the receiver
    // owns its polyphony. Logical voices are source- and stable-channel-
    // qualified, then map onto the physical slots
    // (gate-input ordinals) by time-ordered allocation; when every slot is
    // held, the steal policy picks the victim. Gate-offs route to the slot
    // their logical voice holds — a stolen voice's off finds nothing and
    // drops (its slot now belongs to the thief).

    int slotOfLogical(SourceIdentity sourceIdentity,
                      uint32_t logical,
                      uint64_t stableChannelId) const
    {
        for (size_t s = 0; s < slots_.size(); ++s)
            if (slots_[s].logical == logical
                && slots_[s].sourceIdentity == sourceIdentity
                && slots_[s].stableChannelId == stableChannelId)
                return (int) s;
        return -1;
    }

    int allocateSlot(SourceIdentity sourceIdentity,
                     uint32_t logical,
                     uint64_t stableChannelId)
    {
        const int existing =
            slotOfLogical(sourceIdentity, logical, stableChannelId);
        if (existing >= 0) return existing;
        if (slots_.empty()) return -1;
        // Free slot: never-mapped first (by index), else least recently
        // touched. A pitch-first allocation maps without holding — the
        // recency stamp keeps the slot from being re-allocated before its
        // same-frame gate-on lands.
        int best = -1;
        uint64_t bestTouch = 0;
        for (size_t s = 0; s < slots_.size(); ++s) {
            if (slots_[s].held) continue;
            if (slots_[s].logical == UINT32_MAX) { best = (int) s; break; }
            const uint64_t touch =
                std::max(slots_[s].lastOnSeq, slots_[s].lastOffSeq);
            if (best < 0 || touch < bestTouch) { best = (int) s; bestTouch = touch; }
        }
        if (best < 0) {
            // All held — steal.
            switch (stealPolicy_) {
                case StealPolicy::LastStolen:
                    for (size_t s = 0; s < slots_.size(); ++s)
                        if (best < 0 || slots_[s].lastOnSeq > slots_[(size_t) best].lastOnSeq)
                            best = (int) s;
                    break;
                case StealPolicy::RoundRobin:
                    best = (int) (nextRoundRobin_ % slots_.size());
                    nextRoundRobin_ = (nextRoundRobin_ + 1) % slots_.size();
                    break;
                case StealPolicy::OldestReleased:
                    for (size_t s = 0; s < slots_.size(); ++s)
                        if (best < 0 || slots_[s].lastOnSeq < slots_[(size_t) best].lastOnSeq)
                            best = (int) s;
                    break;
            }
        }
        slots_[(size_t) best].sourceIdentity = sourceIdentity;
        slots_[(size_t) best].stableChannelId = stableChannelId;
        slots_[(size_t) best].logical   = logical;
        slots_[(size_t) best].lastOnSeq = ++allocSeq_;   // recency at mapping
        return best;
    }

    int targetInputOf(const Emission& e)
    {
        if (isNamedOutputLane(e.lane)) {
            const uint32_t lane = namedOutputLaneIndex(e.lane);
            const auto range = bindingsForLane(lane);
            auto routeBinding = range.first;
            while (routeBinding != range.second
                   && routeBinding->inputIndex >= decl_.inputs.size())
                ++routeBinding;
            if (routeBinding == range.second)
                return -1;
            if (! routeBinding->receiverAllocatesVoice)
                return (int) routeBinding->inputIndex;
            if (decl_.inputs[routeBinding->inputIndex].type != e.type)
                return -1;
        }

        switch (e.type) {
            case SignalType::Gate: {
                if (e.kind == PacketKind::Set && isGateOpen(e.value)) {
                    const int previousSlot = slotOfLogical(
                        e.sourceIdentity, e.voice, e.stableChannelId);
                    const bool freshOnset = previousSlot < 0
                        || ! slots_[(size_t) previousSlot].held;
                    const int s = allocateSlot(
                        e.sourceIdentity, e.voice, e.stableChannelId);
                    if (s < 0) return -1;
                    slots_[(size_t) s].held      = true;
                    slots_[(size_t) s].lastOnSeq = ++allocSeq_;
                    // EVERY gate-on consumes its voice's pending pitch (the
                    // Pitch case records one for each arrival): wherever the
                    // allocation landed — reuse, free claim, or steal — the
                    // voice's own pitch lands WITH its gate, so pairing
                    // survives chord overflow and stale loop-over mappings.
                    for (size_t i = 0; i < pendingPitch_.size(); ++i)
                        if (pendingPitch_[i].sourceIdentity == e.sourceIdentity
                            && pendingPitch_[i].voice == e.voice
                            && pendingPitch_[i].stableChannelId
                                == e.stableChannelId) {
                            if ((size_t) s < pitchInputs_.size()) {
                                deferredPitchInput_ = (int) pitchInputs_[(size_t) s];
                                deferredPitchValue_ = pendingPitch_[i].value;
                            }
                            pendingPitch_[i] = pendingPitch_.back();
                            pendingPitch_.pop_back();
                            break;
                        }
                    bool consumedExplicitVelocity = false;
                    for (size_t i = 0; i < pendingVelocity_.size(); ++i)
                        if (pendingVelocity_[i].sourceIdentity == e.sourceIdentity
                            && pendingVelocity_[i].voice == e.voice
                            && pendingVelocity_[i].stableChannelId
                                == e.stableChannelId) {
                            if ((size_t) s < velInputs_.size()) {
                                deferredVelocityInput_ = (int) velInputs_[(size_t) s];
                                deferredVelocityValue_ = pendingVelocity_[i].value;
                            }
                            consumedExplicitVelocity = true;
                            pendingVelocity_[i] = pendingVelocity_.back();
                            pendingVelocity_.pop_back();
                            break;
                        }
                    // Gate-only control producers use the public intensity
                    // contract. Existing Script/note producers may still stage
                    // a separate Velocity emission; that explicit value wins.
                    // This happens after logical-slot allocation, so a stolen
                    // or reassigned physical voice is always a fresh onset.
                    if (! consumedExplicitVelocity && freshOnset
                        && decl_.inputs[gateInputs_[(size_t) s]].unit
                            == InputUnit::BinaryGate
                        && (size_t) s < velInputs_.size()) {
                        deferredVelocityInput_ = (int) velInputs_[(size_t) s];
                        deferredVelocityValue_ = std::clamp(e.value, 0.0f, 1.0f);
                    }
                    return (int) gateInputs_[(size_t) s];
                }
                const int s = slotOfLogical(
                    e.sourceIdentity, e.voice, e.stableChannelId);
                if (s < 0) return -1;   // stolen or never allocated
                slots_[(size_t) s].held       = false;
                slots_[(size_t) s].lastOffSeq = ++allocSeq_;
                return (int) gateInputs_[(size_t) s];
            }
            case SignalType::Pitch: {
                // Pitch precedes its gate-on at the same frame (emission tie
                // rank). The value ALWAYS records into the pending buffer —
                // the voice's gate-on re-applies it on whatever slot it
                // lands (above). An immediate write additionally goes to the
                // voice's mapped slot, or claims a NEVER-mapped one (keeps
                // gate-less pitch flows working — T-469 PITCH_SET). It never
                // claims a mapped slot: that thrashes pairings before the
                // gates arrive. A gate-less declaration has no slots:
                // ordinal mapping stands.
                // B-290: gate-less declarations map by chord POSITION
                // (e.voiceIdx) — e.voice is a per-note-instance id now.
                if (slots_.empty())
                    return e.voiceIdx < pitchInputs_.size()
                        ? (int) pitchInputs_[e.voiceIdx] : -1;
                appendPendingPitch(
                    e.sourceIdentity, e.voice, e.stableChannelId, e.value);
                int s = slotOfLogical(
                    e.sourceIdentity, e.voice, e.stableChannelId);
                if (s < 0) {
                    for (size_t i = 0; i < slots_.size(); ++i)
                        if (slots_[i].logical == UINT32_MAX) { s = (int) i; break; }
                    if (s < 0)
                        return -1;   // deferred — the gate-on applies it
                    slots_[(size_t) s].sourceIdentity = e.sourceIdentity;
                    slots_[(size_t) s].stableChannelId = e.stableChannelId;
                    slots_[(size_t) s].logical   = e.voice;
                    slots_[(size_t) s].lastOnSeq = ++allocSeq_;
                }
                return (size_t) s < pitchInputs_.size()
                    ? (int) pitchInputs_[(size_t) s] : -1;
            }
            case SignalType::Velocity: {
                if (slots_.empty())
                    return e.voiceIdx < velInputs_.size()
                        ? (int) velInputs_[e.voiceIdx] : -1;
                appendPendingVelocity(
                    e.sourceIdentity, e.voice, e.stableChannelId, e.value);
                const int s = slotOfLogical(
                    e.sourceIdentity, e.voice, e.stableChannelId);
                return s >= 0 && (size_t) s < velInputs_.size()
                    ? (int) velInputs_[(size_t) s] : -1;
            }
            case SignalType::Value:
                // Value fan-out is handled directly in processMulti().
                return -1;
            default:
                return -1;
        }
    }

    void applyEvent(uint32_t inputIndex, InputState& st, const Emission& e)
    {
        if (e.type == SignalType::Value) {
            float value = e.value;
            if (e.typedParam && inputIndex < decl_.inputs.size())
                value = resolveTyped(inputIndex, e.authoredValue,
                                     e.authoredBasis, e.authoredStepBeats, e.sourceTempo);
            switch (e.kind) {
                case PacketKind::Set: {
                    auto* contribution = getOrAppendValueContribution(
                        inputIndex, e.sourceIdentity, e.lane);
                    if (contribution == nullptr)
                        return;
                    contribution->hasSet = true;
                    contribution->setValue = value;
                    contribution->typed = e.typedParam != 0;
                    contribution->authoredValue = e.authoredValue;
                    contribution->authoredBasis = e.authoredBasis;
                    contribution->authoredStepBeats = e.authoredStepBeats;
                    contribution->sourceTempo = e.sourceTempo;
                    contribution->setSequence = ++contributionSequence_;
                    st.hasSet = true;
                    st.setVal = contribution->setValue;
                    st.value = composeValue(st);
                    break;
                }
                case PacketKind::Offset: {
                    auto* contribution = getOrAppendValueContribution(
                        inputIndex, e.sourceIdentity, e.lane);
                    if (contribution == nullptr)
                        return;
                    if (! contribution->hasOffset)
                        ++st.offsetCount;
                    else
                        st.offsetSum -= contribution->offsetValue;
                    contribution->hasOffset = true;
                    contribution->offsetValue = value;
                    contribution->typed = e.typedParam != 0;
                    contribution->authoredValue = e.authoredValue;
                    contribution->authoredBasis = e.authoredBasis;
                    contribution->authoredStepBeats = e.authoredStepBeats;
                    contribution->sourceTempo = e.sourceTempo;
                    st.offsetSum += contribution->offsetValue;
                    st.value = composeValue(st);
                    break;
                }
                case PacketKind::Release: {
                    valueContributions_.erase(
                        std::remove_if(
                            valueContributions_.begin(),
                            valueContributions_.end(),
                            [&] (const ValueContribution& contribution) {
                                return contribution.inputIndex == inputIndex
                                    && contribution.sourceIdentity
                                        == e.sourceIdentity
                                    && contribution.lane == e.lane;
                            }),
                        valueContributions_.end());
                    refreshValueState(inputIndex, st);
                    break;
                }
                default:
                    break;
            }
        } else {
            // Preserve public note data. Only a receiver declaring BinaryGate
            // projects intensity to on/off in the final unit-conversion pass;
            // source outputs must retain their intensity across blocks.
            st.value = e.value;
        }
    }

    void fillTo(int input, int upTo, int numFrames)
    {
        InputState& st = states_[(size_t) input];
        auto& out  = outputs_[(size_t) input];
        auto& mask = baseMask_[(size_t) input];
        const int end = std::min(upTo, numFrames);
        const uint8_t setHeld = st.hasSet ? 1 : 0;
        for (int i = st.fillFrom; i < end; ++i) {
            out[(size_t) i]  = st.value;
            mask[(size_t) i] = setHeld;
        }
        st.fillFrom = std::max(st.fillFrom, end);
    }

    using BindingIterator = std::vector<LaneBinding>::const_iterator;

    std::pair<BindingIterator, BindingIterator> bindingsForLane(
        uint32_t lane) const noexcept
    {
        const auto first = std::lower_bound(
            bindings_.begin(), bindings_.end(), lane,
            [] (const LaneBinding& binding, uint32_t candidateLane) {
                return binding.lane < candidateLane;
            });
        const auto last = std::upper_bound(
            first, bindings_.end(), lane,
            [] (uint32_t candidateLane, const LaneBinding& binding) {
                return candidateLane < binding.lane;
            });
        return { first, last };
    }

    uint32_t addressedBindingInput(
        const LaneBinding* binding,
        uint32_t channelIndex) const noexcept
    {
        if (binding == nullptr || bindings_.empty())
            return UINT32_MAX;
        const auto bindingIndex =
            static_cast<size_t>(binding - bindings_.data());
        if (bindingIndex >= bindingChannelInputs_.size()
            || bindingChannelInputs_[bindingIndex].empty())
            return binding->inputIndex;
        const auto& targets =
            bindingChannelInputs_[bindingIndex];
        return targets[
            static_cast<size_t>(channelIndex) % targets.size()];
    }

    ModuleDeclaration decl_;
    std::vector<LaneBinding> bindings_;
    std::vector<std::vector<uint32_t>>
        bindingChannelInputs_;
    int maxFrames_ = 0;
    std::vector<std::vector<float>> outputs_;
    // Per-input per-sample: 1 = a discrete set lock held at this sample
    // (Stream-kind base rows skip it — set outranks route, B-269).
    std::vector<std::vector<uint8_t>> baseMask_;
    // Per-input per-block: note-typed stream rows replace the event-held
    // default once, then additional stream rows sum. Prepared to keep
    // Delivery::processMulti allocation-free.
    std::vector<uint8_t> noteStreamFilled_;
    std::vector<InputState> states_;
    std::vector<ValueContribution> valueContributions_;
    uint64_t contributionSequence_ = 0;
    std::vector<uint32_t> gateInputs_, pitchInputs_, velInputs_;

    // T-479 allocator state — one slot per physical voice (gate input).
    struct VoiceSlot {
        SourceIdentity sourceIdentity = 0;
        uint64_t stableChannelId = 0;
        uint32_t logical    = UINT32_MAX;   // mapped logical voice
        uint64_t lastOnSeq  = 0;
        uint64_t lastOffSeq = 0;
        bool     held       = false;
    };
    std::vector<VoiceSlot> slots_;
    uint64_t allocSeq_ = 0;
    size_t   nextRoundRobin_ = 0;
    StealPolicy stealPolicy_ = StealPolicy::LastStolen;
    double processBpm_ = 120.0;
    const SourceTimingContext* sourceTimingContexts_ = nullptr;
    size_t sourceTimingContextCount_ = 0;
    // Deferred pitches: (source, stable channel, logical voice, value) for
    // pitches that arrived before their voice had a slot (chord overflow —
    // resolved by the gate-on steal). Cleared per process() call; emissions
    // are per-block.
    struct PendingPitch {
        SourceIdentity sourceIdentity = 0;
        uint32_t voice = 0;
        uint64_t stableChannelId = 0;
        float value = 0.0f;
    };
    std::vector<PendingPitch> pendingPitch_;
    struct PendingVelocity {
        SourceIdentity sourceIdentity = 0;
        uint32_t voice = 0;
        uint64_t stableChannelId = 0;
        float value = 0.0f;
    };
    std::vector<PendingVelocity> pendingVelocity_;
    int   deferredPitchInput_ = -1;
    float deferredPitchValue_ = 0.0f;
    int   deferredVelocityInput_ = -1;
    float deferredVelocityValue_ = 0.0f;
    bool  rtBounded_ = false;
    uint32_t rtOverflowDropsThisBlock_ = 0;
};

} // namespace curlop::vm
