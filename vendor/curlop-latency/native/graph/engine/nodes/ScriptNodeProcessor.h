#pragma once
#include "control/surfaces/script/music/PitchUtils.h"
#include "control/vm/machine/Signal.h"
#include "modules/backend/CurlopDspNode.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace curlop {

// Script node — graph participant that outputs gate/pitch/params
// as audio-rate signals through APG edges.
//
// Two modes:
//   1. Manual trigger (Phase 1): setGate/setPitch/setParam — per-block constants.
//   2. ParamBuffer bridge: reads directly from VM paramBuffer — per-sample accurate.
//      Activated by calling bindParamBuffer(). The VM fills paramBuffer in
//      processBlock Phase 1+2, then this node copies the relevant slices
//      to its output channels during its own processBlock.
//
// Output channels (SF-065 / ADR-0019 — voice-aware; voices==1 is the legacy
// 3+numParams layout):
//   ch[2v]   = GATE_V<v>   (0.0 or 1.0)   for v in 0..voices-1
//   ch[2v+1] = PITCH_V<v>  (Hz)
//   ch[2*voices]       = velocity (0.0-1.0, shared across voices)
//   ch[2*voices+1 .. ]  = declared params (one per param)
//
// V2 script nodes may also expose control/audio-rate input buses; those rows
// are copied for VM input reads and direct APG socket routes.

class ScriptNodeProcessor : public DspNode
{
public:
    using PreparedModuleHook = void (*)(
        void*, ScriptNodeProcessor&, int, int) noexcept;

    // numParams = number of module params (DECAY, TONE, etc.)
    // voices    = polyphony voice count (SF-065). Output channels =
    //             2*voices + 1 + numParams (N gate/pitch pairs, shared vel,
    //             params). voices==1 → 3 + numParams (the pre-polyphony bus).
    explicit ScriptNodeProcessor(int numParams = 0, int voices = 1)
        : DspNode(BusesProperties()
            .withOutput("Script Out",
                juce::AudioChannelSet::discreteChannels(
                    2 * (voices < 1 ? 1 : voices) + 1 + numParams))),
          numParams_(numParams),
          voices_(voices < 1 ? 1 : voices)
    {
        if (numParams > 0) {
            paramValues_ = std::make_unique<std::atomic<float>[]>(numParams);
            for (int i = 0; i < numParams; ++i)
                paramValues_[i].store(0.0f, std::memory_order_relaxed);
        }
    }

    // T-466 — declaration-ordered mode: the node emits one channel per
    // DECLARED module input (.dsp source order, matching the adapter's input
    // bus + the VM delivery's declaration). Channel i copies paramBuffer row
    // rows[i] (bindParamBufferRows below); unrouted inputs (row -1) and
    // unbound blocks emit the input's DECLARED DEFAULT — never raw zeros,
    // which would feed e.g. cutoff=0/attack=0 into a Faust filter and NaN
    // its recursive state permanently. The trio/voice-interleave layout
    // above remains for the legacy consumers (core.faust_jit until its
    // T-318 unification).
    struct DeclarationOrdered {
        int numInputs = 0;
        std::vector<float> defaults;   // real-unit declared init per input
    };
    explicit ScriptNodeProcessor(DeclarationOrdered d)
        : DspNode(BusesProperties()
            .withOutput("Script Out",
                juce::AudioChannelSet::discreteChannels(
                    d.numInputs < 1 ? 1 : d.numInputs))),
          numParams_(0),
          voices_(1),
          declChannels_(d.numInputs < 1 ? 1 : d.numInputs),
          declDefaults_(std::move(d.defaults))
    {
        declDefaults_.resize((size_t) declChannels_, 0.0f);
        // T-481: per-block row binding copies into this — sized once here
        // (message thread), exact to the declared input count. No cap, no
        // audio-thread allocation in bindParamBufferRows.
        declRowsCopy_.assign((size_t) declChannels_, -1);
    }

    struct V2RouteOp {
        enum class Kind { Invert, Clip, Scale, Gain, Offset, Abs, Smooth };
        struct Arg {
            bool dynamic = false;
            float value = 0.0f;
            std::string source;
            int exprIndex = -1;
        };
        Kind kind = Kind::Gain;
        float a = 0.0f;
        float b = 0.0f;
        int argCount = 0;
        Arg argA;
        Arg argB;
    };

    struct V2Route {
        int outputChannel = -1;
        int inputChannel = -1;
        bool valid = true;
        std::vector<V2RouteOp> ops;
    };

    struct V2GraphProcessor {
        int numInputs = 0;
        int numOutputs = 0;
        std::vector<std::string> inputNames;
        std::vector<float> defaults;
        std::vector<V2Route> routes;
    };

    // A read-only decoder for the subset of Script V2 route expressions that
    // can live entirely in a compiled Faust graph. It deliberately reuses the
    // APG-era parser so the two runtimes cannot drift on units or waveform
    // spelling; it neither constructs nor executes an APG node.
    struct V2StaticLfo {
        std::string waveform;
        float rateHz = 0.0f;
        float low = 0.0f;
        float high = 1.0f;
    };
    struct V2StaticRouteArgument {
        struct Transform {
            V2RouteOp::Kind kind = V2RouteOp::Kind::Gain;
            float a = 0.0f;
            float b = 0.0f;
            int argCount = 0;
        };
        V2StaticLfo lfo;
        std::vector<Transform> transforms;
    };
    static bool parseV2StaticRouteArgument(
        const std::string& source, V2StaticRouteArgument& out);

    explicit ScriptNodeProcessor(V2GraphProcessor cfg)
        : DspNode(makeV2Buses(cfg.numInputs, cfg.numOutputs)),
          numParams_(0),
          voices_(1),
          declChannels_(cfg.numOutputs < 1 ? 1 : cfg.numOutputs),
          declDefaults_(std::move(cfg.defaults)),
          v2InputChannels_(cfg.numInputs < 0 ? 0 : cfg.numInputs),
          v2InputNames_(std::move(cfg.inputNames)),
          v2Routes_(std::move(cfg.routes))
    {
        declDefaults_.resize((size_t) declChannels_, 0.0f);
        declRowsCopy_.assign((size_t) declChannels_, -1);
        for (auto& route : v2Routes_) {
            route.outputChannel = juce::jlimit(0, declChannels_ - 1, route.outputChannel);
            route.inputChannel = juce::jlimit(0, juce::jmax(0, v2InputChannels_ - 1), route.inputChannel);
        }
        compileV2RouteArgs();
        v2SmoothStates_.resize(v2RouteSmoothStateCount());
    }

    bool isDeclarationOrdered() const { return declChannels_ > 0; }

    const juce::String getName() const override { return "ScriptNode"; }

    void prepareToPlay(double sr, int maxBlock) override
    {
        sampleRate_ = sr;
        samplePos_ = 0;
        v2InputStride_ = juce::jmax(1, maxBlock);
        if (v2InputChannels_ > 0)
            v2InputScratch_.assign((size_t) v2InputChannels_ * (size_t) v2InputStride_,
                                   0.0f);
        for (auto& state : v2SmoothStates_)
            state = {};
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int numSamples = buffer.getNumSamples();

        // ── Declaration-ordered mode (T-466): channel i ← row rows[i];
        //    unrouted/unbound → the input's declared default ──
        if (declChannels_ > 0)
        {
            copyV2Inputs(buffer, numSamples);
            if (preparedModuleHook_ != nullptr)
                preparedModuleHook_(
                    preparedModuleHookContext_,
                    *this,
                    preparedModuleGraphIdx_,
                    numSamples);

            const int chans = std::min(declChannels_, buffer.getNumChannels());
            for (int i = 0; i < chans; ++i)
            {
                const int rowIdx = (paramBufferData_ != nullptr && i < declRowCount_)
                                       ? declRows_[(size_t) i] : -1;
                auto* out = buffer.getWritePointer(i);
                if (rowIdx >= 0)
                    std::memcpy(out, row(rowIdx),
                                sizeof(float) * (size_t) numSamples);
                else
                    for (int s = 0; s < numSamples; ++s)
                        out[s] = declDefaults_[(size_t) i];
            }
            applyV2Routes(buffer, numSamples);
            samplePos_ += numSamples;
            return;
        }

        // ── Manual trigger mode: per-block constants ──
        if (preparedModuleHook_ != nullptr)
            preparedModuleHook_(
                preparedModuleHookContext_,
                *this,
                preparedModuleGraphIdx_,
                numSamples);

        // Legacy convenience path (Phase 1, mono only) — lights V0 (ch0/ch1);
        // vel + params sit at the voice-aware channel offsets so the channel
        // map matches the bus width. voices_==1 → ch2 vel / ch3+p params.
        auto* gateOut = buffer.getWritePointer(0);
        auto* pitchOut = buffer.getWritePointer(1);
        auto* velOut = buffer.getWritePointer(2 * voices_);

        float gate = gate_.load(std::memory_order_relaxed);
        float pitch = pitch_.load(std::memory_order_relaxed);
        float vel = velocity_.load(std::memory_order_relaxed);

        for (int s = 0; s < numSamples; ++s)
        {
            gateOut[s] = gate;
            pitchOut[s] = pitch;
            velOut[s] = vel;
        }

        // Write param channels
        if (paramValues_) {
            for (int p = 0; p < numParams_; ++p)
            {
                auto* paramOut = buffer.getWritePointer(2 * voices_ + 1 + p);
                float val = paramValues_[p].load(std::memory_order_relaxed);
                for (int s = 0; s < numSamples; ++s)
                    paramOut[s] = val;
            }
        }

        samplePos_ += numSamples;
    }

    // ── ParamBuffer bridge (per-sample VM output) ─────────────────
    // Called from VMRunner::processBlock AFTER the VM fills paramBuffer,
    // BEFORE the APG processBlock. The pointer is valid for the duration
    // of one processBlock call — the buffer is owned by EngineSlot.
    //
    // T-327 (T-295 slice 3b): paramBuffer is now stored flat as
    // `std::vector<float>` of length `numParams * numSamples` inside
    // VMProcessData::ParamBufferAccess. We cache (data, stride) here so
    // `row(p)` returns the float* for param p without an additional
    // pointer dereference per sample (compared to storing the whole
    // ParamBufferAccess pointer and doing `(*pb)[p]`).
    // T-466 — declaration-ordered bind: rows[i] is the paramBuffer row for
    // declared input i (-1 = unrouted → silence). Rows are copied (into a
    // vector sized to the declared input count at construction — T-481: no
    // 64-row cap, no audio-thread allocation) so the binding never dangles
    // into a swapped moduleVms vector. Called per block from VMRunner
    // before APG processing.
    void bindParamBufferRows(const float* paramBufferData,
                             int paramBufferStride,
                             const int* rows, int numRows)
    {
        paramBufferData_   = paramBufferData;
        paramBufferStride_ = paramBufferStride;
        declRowCount_ = std::min(numRows, (int) declRowsCopy_.size());
        for (int i = 0; i < declRowCount_; ++i)
            declRowsCopy_[(size_t) i] = rows[i];
        declRows_ = declRowsCopy_.data();
    }

    void unbindParamBuffer()
    {
        paramBufferData_ = nullptr;
        paramBufferStride_ = 0;
        declRowCount_ = 0;
    }

    // ── Manual trigger interface (Phase 1) ────────────────────────
    void setGate(float g) { gate_.store(g, std::memory_order_relaxed); }
    void setPitch(float hz) { pitch_.store(hz, std::memory_order_relaxed); }
    void setVelocity(float v) { velocity_.store(v, std::memory_order_relaxed); }

    void setParam(int idx, float val) {
        if (paramValues_ && idx >= 0 && idx < numParams_)
            paramValues_[idx].store(val, std::memory_order_relaxed);
    }

    // Trigger a note (convenience: sets gate=1, pitch, velocity in one call)
    void noteOn(float pitchHz, float vel = 0.7f) {
        pitch_.store(pitchHz, std::memory_order_relaxed);
        velocity_.store(vel, std::memory_order_relaxed);
        gate_.store(1.0f, std::memory_order_release);
    }

    void noteOff() {
        gate_.store(0.0f, std::memory_order_release);
    }

    int getNumParams() const { return numParams_; }
    int getNumVoices() const { return voices_; }

    int getV2InputChannelCount() const noexcept { return v2InputChannels_; }

    const float* lastV2InputRow(int ch, int numSamples) const
    {
        return v2InputRow(ch, numSamples);
    }

    void bindPreparedModuleHook(
        void* context,
        PreparedModuleHook hook,
        int graphIdx) noexcept
    {
        preparedModuleHookContext_ = context;
        preparedModuleHook_ = hook;
        preparedModuleGraphIdx_ = graphIdx;
    }

    void clearPreparedModuleHook() noexcept
    {
        preparedModuleHookContext_ = nullptr;
        preparedModuleHook_ = nullptr;
        preparedModuleGraphIdx_ = -1;
    }

private:
    static BusesProperties makeV2Buses(int numInputs, int numOutputs)
    {
        BusesProperties buses;
        if (numInputs > 0)
            buses = buses.withInput("Script V2 In",
                                    juce::AudioChannelSet::discreteChannels(numInputs));
        return buses.withOutput("Script V2 Out",
                                juce::AudioChannelSet::discreteChannels(
                                    numOutputs < 1 ? 1 : numOutputs));
    }

    struct V2SmoothState {
        bool initialized = false;
        float y = 0.0f;
    };

    struct V2RouteExpr {
        enum class Kind { Constant, Input, Lfo, Chain, Invalid };
        struct Transform {
            V2RouteOp::Kind kind = V2RouteOp::Kind::Gain;
            V2RouteOp::Arg argA;
            V2RouteOp::Arg argB;
            int argCount = 0;
        };

        Kind kind = Kind::Invalid;
        float value = 0.0f;
        std::string name;
        std::vector<V2RouteExpr> args;
        std::vector<Transform> chain;
    };

    int numParams_;
    int voices_ = 1;  // SF-065 / ADR-0019 — output bus carries 2*voices_ gate/pitch + vel + params
    double sampleRate_ = 48000.0;
    int64_t samplePos_ = 0;

    // Manual trigger mode state
    std::atomic<float> gate_ { 0.0f };
    std::atomic<float> pitch_ { 440.0f };
    std::atomic<float> velocity_ { 0.7f };
    std::unique_ptr<std::atomic<float>[]> paramValues_;

    // ParamBuffer bridge mode state (set per processBlock cycle).
    // T-327 (T-295 slice 3b): paramBuffer is now flat
    // `std::vector<float>` of length `numParams * numSamples` (see
    // VMProcessData::ParamBufferAccess). We cache (data, stride) so
    // `row(p)` is one mul + one add per access — same as the
    // historical `paramBuffer_[p]` pointer-to-row indexing, just with
    // an explicit stride instead of a compile-time array dim.
    const float* paramBufferData_   = nullptr;
    int          paramBufferStride_ = 0;

    // T-466 declaration-ordered mode state. declChannels_ > 0 selects the
    // mode (set at construction); the row table is a per-block copy.
    int declChannels_ = 0;
    std::vector<float> declDefaults_;       // real-unit declared init per input
    std::vector<int> declRowsCopy_;         // sized to declChannels_ at construction (T-481)
    const int* declRows_ = nullptr;
    int declRowCount_ = 0;

    int v2InputChannels_ = 0;
    std::vector<std::string> v2InputNames_;
    std::vector<V2Route> v2Routes_;
    std::vector<V2RouteExpr> v2RouteArgExprs_;
    std::vector<V2SmoothState> v2SmoothStates_;
    std::vector<float> v2InputScratch_;
    int v2InputStride_ = 0;
    void* preparedModuleHookContext_ = nullptr;
    PreparedModuleHook preparedModuleHook_ = nullptr;
    int preparedModuleGraphIdx_ = -1;

    const float* row(int p) const {
        return paramBufferData_ + (size_t) p * (size_t) paramBufferStride_;
    }

    size_t v2RouteSmoothStateCount() const
    {
        size_t count = 0;
        for (const auto& route : v2Routes_)
            for (const auto& op : route.ops)
                if (op.kind == V2RouteOp::Kind::Smooth)
                    ++count;
        return count;
    }

    void copyV2Inputs(const juce::AudioBuffer<float>& buffer, int numSamples)
    {
        if (v2InputChannels_ <= 0)
            return;

        if (v2InputStride_ < numSamples
            || v2InputScratch_.size() < (size_t) v2InputChannels_ * (size_t) v2InputStride_)
            return;

        for (int ch = 0; ch < v2InputChannels_; ++ch) {
            auto* dst = v2InputScratch_.data() + (size_t) ch * (size_t) v2InputStride_;
            if (ch < buffer.getNumChannels())
                std::memcpy(dst, buffer.getReadPointer(ch),
                            sizeof(float) * (size_t) numSamples);
            else
                std::fill(dst, dst + numSamples, 0.0f);
        }
    }

    const float* v2InputRow(int ch, int numSamples) const
    {
        if (ch < 0 || ch >= v2InputChannels_ || v2InputScratch_.empty())
            return nullptr;
        if (v2InputStride_ < numSamples)
            return nullptr;
        return v2InputScratch_.data() + (size_t) ch * (size_t) v2InputStride_;
    }

    enum class V2RouteArgDomain { Scalar, RateHz, Seconds };

    static bool splitV2TopLevel(const juce::String& body, juce::juce_wchar delimiter,
                                std::vector<juce::String>& parts)
    {
        juce::String current;
        int parens = 0, brackets = 0, braces = 0;
        for (int i = 0; i < body.length(); ++i) {
            const auto c = body[i];
            if (c == '(') ++parens;
            else if (c == ')') --parens;
            else if (c == '[') ++brackets;
            else if (c == ']') --brackets;
            else if (c == '{') ++braces;
            else if (c == '}') --braces;
            if (parens < 0 || brackets < 0 || braces < 0)
                return false;
            if (c == delimiter && parens == 0 && brackets == 0 && braces == 0) {
                const auto part = current.trim();
                if (part.isEmpty())
                    return false;
                parts.push_back(part);
                current = {};
            } else {
                current << juce::String::charToString(c);
            }
        }
        if (parens != 0 || brackets != 0 || braces != 0)
            return false;
        const auto tail = current.trim();
        if (tail.isNotEmpty())
            parts.push_back(tail);
        return true;
    }

    static bool parseV2RouteNumber(const juce::String& raw, float& out)
    {
        const auto t = raw.trim();
        if (t.isEmpty())
            return false;
        const int slash = t.indexOfChar('/');
        if (slash > 0 && slash < t.length() - 1 && t.indexOfChar(slash + 1, '/') < 0) {
            float n = 0.0f, d = 0.0f;
            if (! parseV2RouteNumber(t.substring(0, slash), n)
                || ! parseV2RouteNumber(t.substring(slash + 1), d)
                || d == 0.0f)
                return false;
            const float value = n / d;
            if (! std::isfinite(value))
                return false;
            out = value;
            return true;
        }

        const auto text = t.toStdString();
        char* end = nullptr;
        errno = 0;
        const float value = std::strtof(text.c_str(), &end);
        if (end == text.c_str()
            || end == nullptr
            || *end != '\0'
            || errno == ERANGE
            || ! std::isfinite(value))
            return false;

        out = value;
        return true;
    }

    static bool parseV2RouteScalar(const juce::String& raw, V2RouteArgDomain domain,
                                   float& out)
    {
        const auto t = raw.trim();
        const auto lower = t.toLowerCase();
        if (parseV2RouteNumber(t, out))
            return true;
        if (lower.endsWith("khz") && t.length() > 3) {
            float v = 0.0f;
            if (! parseV2RouteNumber(t.dropLastCharacters(3), v))
                return false;
            if (domain == V2RouteArgDomain::RateHz) {
                out = v * 1000.0f;
                return true;
            }
            if (v <= 0.0f)
                return false;
            const double midi = 69.0 + 12.0 * std::log2(std::max(1.0e-9, (double) v * 1000.0) / 440.0);
            out = curlop::vm::noteToSignal((float) (midi - 60.0));
            return true;
        }
        if (lower.endsWith("hz") && t.length() > 2) {
            float v = 0.0f;
            if (! parseV2RouteNumber(t.dropLastCharacters(2), v))
                return false;
            if (domain == V2RouteArgDomain::RateHz) {
                out = v;
                return true;
            }
            if (v <= 0.0f)
                return false;
            const double midi = 69.0 + 12.0 * std::log2(std::max(1.0e-9, (double) v) / 440.0);
            out = curlop::vm::noteToSignal((float) (midi - 60.0));
            return true;
        }
        if (lower.endsWith("%") && t.length() > 1) {
            float v = 0.0f;
            if (! parseV2RouteNumber(t.dropLastCharacters(1), v))
                return false;
            out = v / 100.0f;
            return true;
        }
        if (lower.endsWith("ms") && t.length() > 2) {
            float v = 0.0f;
            if (! parseV2RouteNumber(t.dropLastCharacters(2), v))
                return false;
            out = v / 1000.0f;
            return true;
        }
        if (lower.endsWith("steps") && t.length() > 5)
            return parseV2RouteNumber(t.dropLastCharacters(5), out);
        if (lower.endsWith("step") && t.length() > 4)
            return parseV2RouteNumber(t.dropLastCharacters(4), out);
        if (lower.endsWith("s") && t.length() > 1)
            return parseV2RouteNumber(t.dropLastCharacters(1), out);
        const int midi = curlop::pitch::noteToMidi(t);
        if (midi >= 0) {
            out = domain == V2RouteArgDomain::RateHz
                ? (float) (440.0 * std::pow(2.0, ((double) midi - 69.0) / 12.0))
                : curlop::vm::noteToSignal((float) (midi - 60));
            return true;
        }
        return false;
    }

    static bool parseV2RouteCall(const juce::String& source, juce::String& name,
                                 std::vector<juce::String>& args)
    {
        const auto s = source.trim();
        const int lp = s.indexOfChar('(');
        if (lp <= 0 || ! s.endsWithChar(')'))
            return false;
        name = s.substring(0, lp).trim().toLowerCase();
        return splitV2TopLevel(s.substring(lp + 1, s.length() - 1), ',', args);
    }

    static bool parseV2RouteExpr(const juce::String& raw, V2RouteArgDomain domain,
                                 V2RouteExpr& out)
    {
        const auto src = raw.trim();
        float scalar = 0.0f;
        if (parseV2RouteScalar(src, domain, scalar)) {
            out.kind = V2RouteExpr::Kind::Constant;
            out.value = scalar;
            return true;
        }

        std::vector<juce::String> chainParts;
        if (! splitV2TopLevel(src, ':', chainParts) || chainParts.empty())
            return false;
        if (chainParts.size() > 1) {
            V2RouteExpr sourceExpr;
            if (! parseV2RouteExpr(chainParts.front(), V2RouteArgDomain::Scalar, sourceExpr))
                return false;
            out.kind = V2RouteExpr::Kind::Chain;
            out.args.push_back(std::move(sourceExpr));
            for (size_t i = 1; i < chainParts.size(); ++i) {
                juce::String name;
                std::vector<juce::String> args;
                if (! parseV2RouteCall(chainParts[i], name, args))
                    return false;
                V2RouteExpr::Transform t;
                if (name == "slew") name = "smooth";
                if (name == "invert" && args.size() <= 1) {
                    t.kind = V2RouteOp::Kind::Invert;
                    t.argCount = (int) args.size();
                } else if (name == "clip" && args.size() == 2) {
                    t.kind = V2RouteOp::Kind::Clip;
                    t.argCount = 2;
                } else if (name == "scale" && args.size() == 2) {
                    t.kind = V2RouteOp::Kind::Scale;
                    t.argCount = 2;
                } else if (name == "gain" && args.size() == 1) {
                    t.kind = V2RouteOp::Kind::Gain;
                    t.argCount = 1;
                } else if (name == "offset" && args.size() == 1) {
                    t.kind = V2RouteOp::Kind::Offset;
                    t.argCount = 1;
                } else if (name == "abs" && args.empty()) {
                    t.kind = V2RouteOp::Kind::Abs;
                    t.argCount = 0;
                } else {
                    return false;
                }
                if (! args.empty()) {
                    t.argA.dynamic = true;
                    t.argA.source = args[0].trim().toStdString();
                }
                if (args.size() > 1) {
                    t.argB.dynamic = true;
                    t.argB.source = args[1].trim().toStdString();
                }
                out.chain.push_back(std::move(t));
            }
            return true;
        }

        if (src.startsWithChar('<')) {
            out.kind = V2RouteExpr::Kind::Input;
            out.name = src.substring(1).trim().toStdString();
            return ! out.name.empty();
        }

        juce::String name;
        std::vector<juce::String> args;
        if (! parseV2RouteCall(src, name, args))
            return false;
        if (name == "lfo") {
            if (args.size() != 4)
                return false;
            out.kind = V2RouteExpr::Kind::Lfo;
            out.name = args[0].trim().toLowerCase().toStdString();
            V2RouteExpr rate, low, high;
            if (! parseV2RouteExpr(args[1], V2RouteArgDomain::RateHz, rate)
                || ! parseV2RouteExpr(args[2], V2RouteArgDomain::Scalar, low)
                || ! parseV2RouteExpr(args[3], V2RouteArgDomain::Scalar, high))
                return false;
            out.args.push_back(std::move(rate));
            out.args.push_back(std::move(low));
            out.args.push_back(std::move(high));
            return true;
        }
        return false;
    }

    bool compileV2NestedRouteArg(V2RouteOp::Arg& arg, V2RouteArgDomain domain)
    {
        if (! arg.dynamic)
            return true;
        V2RouteExpr expr;
        if (! parseV2RouteExpr(juce::String(arg.source), domain, expr))
            return false;
        if (! compileV2RouteExprNestedArgs(expr))
            return false;
        arg.exprIndex = (int) v2RouteArgExprs_.size();
        v2RouteArgExprs_.push_back(std::move(expr));
        return true;
    }

    bool compileV2RouteExprNestedArgs(V2RouteExpr& expr)
    {
        for (auto& arg : expr.args)
            if (! compileV2RouteExprNestedArgs(arg))
                return false;
        for (auto& t : expr.chain) {
            const auto domain = t.kind == V2RouteOp::Kind::Smooth
                ? V2RouteArgDomain::Seconds : V2RouteArgDomain::Scalar;
            if (! compileV2NestedRouteArg(t.argA, domain)
                || ! compileV2NestedRouteArg(t.argB, V2RouteArgDomain::Scalar))
                return false;
        }
        return true;
    }

    void compileV2RouteArgs()
    {
        auto compileArg = [&] (V2RouteOp::Arg& arg, V2RouteArgDomain domain) {
            if (! arg.dynamic)
                return true;
            V2RouteExpr expr;
            if (! parseV2RouteExpr(juce::String(arg.source), domain, expr))
                return false;
            if (! compileV2RouteExprNestedArgs(expr))
                return false;
            arg.exprIndex = (int) v2RouteArgExprs_.size();
            v2RouteArgExprs_.push_back(std::move(expr));
            return true;
        };

        for (auto& route : v2Routes_) {
            for (auto& op : route.ops) {
                const auto domain = op.kind == V2RouteOp::Kind::Smooth
                    ? V2RouteArgDomain::Seconds : V2RouteArgDomain::Scalar;
                if (! compileArg(op.argA, domain)
                    || ! compileArg(op.argB, V2RouteArgDomain::Scalar))
                    route.valid = false;
            }
        }
    }

    int v2InputIndexForName(const std::string& name) const
    {
        for (int i = 0; i < (int) v2InputNames_.size(); ++i)
            if (v2InputNames_[(size_t) i] == name)
                return i;
        return -1;
    }

    float evalV2RouteExpr(const V2RouteExpr& expr, int sample, int numSamples) const
    {
        switch (expr.kind) {
            case V2RouteExpr::Kind::Constant:
                return expr.value;
            case V2RouteExpr::Kind::Input: {
                const int ch = v2InputIndexForName(expr.name);
                const auto* row = v2InputRow(ch, numSamples);
                return row != nullptr ? row[sample] : 0.0f;
            }
            case V2RouteExpr::Kind::Lfo: {
                const float rate = expr.args.size() > 0
                    ? evalV2RouteExpr(expr.args[0], sample, numSamples) : 1.0f;
                const float lo = expr.args.size() > 1
                    ? evalV2RouteExpr(expr.args[1], sample, numSamples) : 0.0f;
                const float hi = expr.args.size() > 2
                    ? evalV2RouteExpr(expr.args[2], sample, numSamples) : 1.0f;
                const double phase = ((double) samplePos_ + (double) sample)
                    * std::max(0.0f, rate) / std::max(1.0, sampleRate_);
                const double frac = phase - std::floor(phase);
                float shaped = 0.0f;
                if (expr.name == "tri" || expr.name == "triangle")
                    shaped = (float) (frac < 0.5 ? frac * 2.0 : (1.0 - frac) * 2.0);
                else if (expr.name == "saw")
                    shaped = (float) frac;
                else if (expr.name == "square")
                    shaped = frac < 0.5 ? 1.0f : 0.0f;
                else
                    shaped = 0.5f + 0.5f * std::sin((float) (frac * juce::MathConstants<double>::twoPi));
                return lo + shaped * (hi - lo);
            }
            case V2RouteExpr::Kind::Chain: {
                if (expr.args.empty())
                    return 0.0f;
                float v = evalV2RouteExpr(expr.args.front(), sample, numSamples);
                for (const auto& op : expr.chain)
                    v = applyV2RouteTransform(v, op.kind, op.argA, op.argB,
                                              op.argCount, sample, numSamples);
                return v;
            }
            case V2RouteExpr::Kind::Invalid:
                break;
        }
        return 0.0f;
    }

    float evalV2RouteArg(const V2RouteOp::Arg& arg, int sample, int numSamples,
                         float fallback) const
    {
        if (! arg.dynamic)
            return arg.value;
        if (arg.exprIndex < 0 || arg.exprIndex >= (int) v2RouteArgExprs_.size())
            return fallback;
        const float v = evalV2RouteExpr(v2RouteArgExprs_[(size_t) arg.exprIndex],
                                        sample, numSamples);
        return std::isfinite(v) ? v : fallback;
    }

    float applyV2RouteTransform(float v, V2RouteOp::Kind kind,
                                const V2RouteOp::Arg& argA,
                                const V2RouteOp::Arg& argB,
                                int argCount,
                                int sample,
                                int numSamples) const
    {
        switch (kind) {
            case V2RouteOp::Kind::Invert: {
                const float pivot = argCount > 0
                    ? evalV2RouteArg(argA, sample, numSamples, 0.5f) : 0.5f;
                return 2.0f * pivot - v;
            }
            case V2RouteOp::Kind::Clip: {
                const float a = evalV2RouteArg(argA, sample, numSamples, 0.0f);
                const float b = evalV2RouteArg(argB, sample, numSamples, 1.0f);
                return juce::jlimit(std::min(a, b), std::max(a, b), v);
            }
            case V2RouteOp::Kind::Scale: {
                const float a = evalV2RouteArg(argA, sample, numSamples, 0.0f);
                const float b = evalV2RouteArg(argB, sample, numSamples, 1.0f);
                return a + v * (b - a);
            }
            case V2RouteOp::Kind::Gain:
                return v * evalV2RouteArg(argA, sample, numSamples, 1.0f);
            case V2RouteOp::Kind::Offset:
                return v + evalV2RouteArg(argA, sample, numSamples, 0.0f);
            case V2RouteOp::Kind::Abs:
                return std::abs(v);
            case V2RouteOp::Kind::Smooth:
                return v;
        }
        return v;
    }

    void applyV2Routes(juce::AudioBuffer<float>& buffer, int numSamples)
    {
        if (v2Routes_.empty() || v2InputChannels_ <= 0)
            return;

        size_t smoothBase = 0;
        for (const auto& route : v2Routes_) {
            const auto* in = v2InputRow(route.inputChannel, numSamples);
            if (! route.valid
                || in == nullptr || route.outputChannel < 0
                || route.outputChannel >= buffer.getNumChannels()) {
                for (const auto& op : route.ops)
                    if (op.kind == V2RouteOp::Kind::Smooth)
                        ++smoothBase;
                continue;
            }

            auto* out = buffer.getWritePointer(route.outputChannel);
            for (int s = 0; s < numSamples; ++s) {
                float v = in[s];
                size_t smoothIndex = smoothBase;
                for (const auto& op : route.ops) {
                    switch (op.kind) {
                        case V2RouteOp::Kind::Invert: {
                            const float pivot = op.argCount > 0
                                ? evalV2RouteArg(op.argA, s, numSamples, op.a)
                                : 0.5f;
                            v = 2.0f * pivot - v;
                            break;
                        }
                        case V2RouteOp::Kind::Clip: {
                            const float a = evalV2RouteArg(op.argA, s, numSamples, op.a);
                            const float b = evalV2RouteArg(op.argB, s, numSamples, op.b);
                            v = juce::jlimit(std::min(a, b), std::max(a, b), v);
                            break;
                        }
                        case V2RouteOp::Kind::Scale: {
                            const float a = evalV2RouteArg(op.argA, s, numSamples, op.a);
                            const float b = evalV2RouteArg(op.argB, s, numSamples, op.b);
                            v = a + v * (b - a);
                            break;
                        }
                        case V2RouteOp::Kind::Gain:
                            v *= evalV2RouteArg(op.argA, s, numSamples, op.a);
                            break;
                        case V2RouteOp::Kind::Offset:
                            v += evalV2RouteArg(op.argA, s, numSamples, op.a);
                            break;
                        case V2RouteOp::Kind::Abs:
                            v = std::abs(v);
                            break;
                        case V2RouteOp::Kind::Smooth: {
                            if (smoothIndex >= v2SmoothStates_.size())
                                break;
                            auto& st = v2SmoothStates_[smoothIndex];
                            ++smoothIndex;
                            const float seconds = std::max(0.0f,
                                evalV2RouteArg(op.argA, s, numSamples, op.a));
                            const float alpha = seconds <= 0.0f
                                ? 1.0f
                                : 1.0f - std::exp(-1.0f / (seconds * (float) sampleRate_));
                            if (! st.initialized) {
                                st.y = v;
                                st.initialized = true;
                            } else {
                                st.y += alpha * (v - st.y);
                            }
                            v = st.y;
                            break;
                        }
                    }
                }
                out[s] = std::isfinite(v) ? v : 0.0f;
            }
            for (const auto& op : route.ops)
                if (op.kind == V2RouteOp::Kind::Smooth)
                    ++smoothBase;
        }
    }
};

inline bool ScriptNodeProcessor::parseV2StaticRouteArgument(
    const std::string& source, V2StaticRouteArgument& out)
{
    V2RouteExpr expr;
    if (! parseV2RouteExpr(juce::String(source), V2RouteArgDomain::Scalar, expr))
        return false;

    const V2RouteExpr* lfo = &expr;
    if (expr.kind == V2RouteExpr::Kind::Chain) {
        if (expr.args.size() != 1)
            return false;
        lfo = &expr.args.front();
    }
    if (lfo->kind != V2RouteExpr::Kind::Lfo || lfo->args.size() != 3
        || lfo->args[0].kind != V2RouteExpr::Kind::Constant
        || lfo->args[1].kind != V2RouteExpr::Kind::Constant
        || lfo->args[2].kind != V2RouteExpr::Kind::Constant)
        return false;

    out.lfo.waveform = lfo->name;
    out.lfo.rateHz = lfo->args[0].value;
    out.lfo.low = lfo->args[1].value;
    out.lfo.high = lfo->args[2].value;
    out.transforms.clear();
    if (expr.kind != V2RouteExpr::Kind::Chain)
        return true;

    auto parseConstant = [] (const V2RouteOp::Arg& argument,
                             V2RouteArgDomain domain, float& value) {
        if (! argument.dynamic) {
            value = argument.value;
            return true;
        }
        V2RouteExpr nested;
        return parseV2RouteExpr(juce::String(argument.source), domain, nested)
            && nested.kind == V2RouteExpr::Kind::Constant
            ? (value = nested.value, true) : false;
    };
    for (const auto& transform : expr.chain) {
        if (transform.kind == V2RouteOp::Kind::Smooth)
            return false;
        V2StaticRouteArgument::Transform staticTransform;
        staticTransform.kind = transform.kind;
        staticTransform.argCount = transform.argCount;
        if ((transform.argCount > 0
             && ! parseConstant(transform.argA, V2RouteArgDomain::Scalar,
                                staticTransform.a))
            || (transform.argCount > 1
                && ! parseConstant(transform.argB, V2RouteArgDomain::Scalar,
                                   staticTransform.b)))
            return false;
        out.transforms.push_back(staticTransform);
    }
    return true;
}

} // namespace curlop
