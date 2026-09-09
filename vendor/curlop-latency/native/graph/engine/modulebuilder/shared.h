// T-209 cut 5.3b (s406): shared types + helpers used across the per-method
// build_*.h files. Carries the full original include set so each per-method
// header gets a single #include "shared.h" and works.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "graph/engine/nodes/ScriptNodeProcessor.h"
#include "graph/engine/nodes/ControlCoreProcessor.h"
#include "graph/engine/nodes/ControlMeterProcessor.h"
#include "graph/engine/nodes/MeterProcessor.h"
#include "graph/engine/nodes/StepSequencerProcessor.h"
#include "graph/engine/nodes/TransportClockProcessor.h"
#include "modules/backend/CurlopDspNode.h"
#include "control/vm/machine/Delivery.h"
#include "shell/CurlopDebug.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace curlop {

// Forward declaration so AdapterGraph can hold a FaustNode* without
// pulling FaustNode.h (and the libfaust-stub-collision risk) into
// every per-builder TU. Defined in dsp/FaustNode.h.
class FaustNode;

namespace ModuleBuilder {


    // ConstNode and LevelGainProcessor are exposed at namespace scope so
    // benchmarks / tests can reuse the exact same primitive instances the
    // production builders use.
    class ConstNode : public DspNode
    {
    public:
        ConstNode(float v) : DspNode(BusesProperties()
            .withOutput("Out", juce::AudioChannelSet::mono())), val_(v) {}
        const juce::String getName() const override { return "Const"; }
        void prepareToPlay(double, int) override {}
        void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
            auto* out = buf.getWritePointer(0);
            for (int s = 0; s < buf.getNumSamples(); ++s) out[s] = val_;
        }
    private:
        float val_;
    };

    class ControlSumProcessor : public DspNode
    {
    public:
        ControlSumProcessor()
            : DspNode(BusesProperties()
                .withInput("In", juce::AudioChannelSet::discreteChannels(2))
                .withOutput("Out", juce::AudioChannelSet::mono())) {}
        const juce::String getName() const override { return "ControlSum"; }
        void prepareToPlay(double, int) override {}
        void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            auto* out = buf.getWritePointer(0);
            for (int s = 0; s < buf.getNumSamples(); ++s) {
                const float base = buf.getSample(0, s);
                const float offset = buf.getSample(1, s);
                out[s] = (std::isfinite(base) ? base : 0.0f)
                       + (std::isfinite(offset) ? offset : 0.0f);
            }
        }
    };

    class ControlSetMergeProcessor : public DspNode
    {
    public:
        ControlSetMergeProcessor()
            : DspNode(BusesProperties()
                .withInput("In", juce::AudioChannelSet::discreteChannels(4))
                .withOutput("Out", juce::AudioChannelSet::mono())) {}
        const juce::String getName() const override { return "ControlSetMerge"; }
        void prepareToPlay(double, int) override {}
        void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            auto* out = buf.getWritePointer(0);
            for (int s = 0; s < buf.getNumSamples(); ++s) {
                const float base = buf.getSample(0, s);
                const float offset = buf.getSample(1, s);
                const float setValue = buf.getSample(2, s);
                const float setPresence = buf.getSample(3, s);
                const bool hasSet = std::isfinite(setPresence) && setPresence > 0.5f;
                const float chosen = hasSet ? setValue : base;
                out[s] = (std::isfinite(chosen) ? chosen : 0.0f)
                       + (std::isfinite(offset) ? offset : 0.0f);
            }
        }
    };

    // Two Set authorities with explicit precedence. Used only for the Faust
    // velocity compatibility seam: gate intensity supplies the canonical
    // onset velocity, while an explicitly patched legacy velocity socket may
    // still override it rather than becoming a visible no-op.
    class ControlSetPriorityMergeProcessor : public DspNode
    {
    public:
        ControlSetPriorityMergeProcessor()
            : DspNode(BusesProperties()
                .withInput("In", juce::AudioChannelSet::discreteChannels(6))
                .withOutput("Out", juce::AudioChannelSet::mono())) {}
        const juce::String getName() const override { return "ControlSetPriorityMerge"; }
        void prepareToPlay(double, int) override {}
        void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            auto* out = buf.getWritePointer(0);
            for (int s = 0; s < buf.getNumSamples(); ++s) {
                const float base = buf.getSample(0, s);
                const float offset = buf.getSample(1, s);
                const float lowerSet = buf.getSample(2, s);
                const float lowerPresence = buf.getSample(3, s);
                const float higherSet = buf.getSample(4, s);
                const float higherPresence = buf.getSample(5, s);
                const bool hasLower = std::isfinite(lowerPresence)
                    && lowerPresence > 0.5f;
                const bool hasHigher = std::isfinite(higherPresence)
                    && higherPresence > 0.5f;
                const float chosen = hasHigher ? higherSet
                    : hasLower ? lowerSet : base;
                out[s] = (std::isfinite(chosen) ? chosen : 0.0f)
                       + (std::isfinite(offset) ? offset : 0.0f);
            }
        }
    };

    class PitchSignalToHzProcessor : public DspNode
    {
    public:
        PitchSignalToHzProcessor()
            : DspNode(BusesProperties()
                .withInput("In", juce::AudioChannelSet::mono())
                .withOutput("Out", juce::AudioChannelSet::mono())) {}
        const juce::String getName() const override { return "PitchSignalToHz"; }
        void prepareToPlay(double, int) override {}
        void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            auto* out = buf.getWritePointer(0);
            const auto* in = buf.getReadPointer(0);
            for (int s = 0; s < buf.getNumSamples(); ++s) {
                if (! std::isfinite(in[s])) { out[s] = 0.0f; continue; }
                const float midi = curlop::vm::noteToMidi(curlop::vm::signalToNote(in[s]));
                out[s] = 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
            }
        }
    };

    // A graph gate is one intensity signal. The Faust boundary projects it
    // onto the conventional gate + velocity pair so authored Faust remains
    // ordinary Faust: zero releases, a zero-to-positive transition opens the
    // gate and latches that transition's intensity as velocity, and changes
    // while high do not rewrite the release-tail velocity.
    class GateIntensityToFaustProcessor : public DspNode
    {
    public:
        GateIntensityToFaustProcessor()
            : DspNode(BusesProperties()
                .withInput("Intensity", juce::AudioChannelSet::mono())
                .withOutput("GateVelocity",
                            juce::AudioChannelSet::discreteChannels(2))) {}
        const juce::String getName() const override { return "GateIntensityToFaust"; }
        void prepareToPlay(double, int) override { reset(); }
        void reset() override
        {
            high_ = false;
            latchedVelocity_ = 1.0f;
        }
        void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            auto* gate = buf.getWritePointer(0);
            auto* velocity = buf.getWritePointer(1);
            for (int s = 0; s < buf.getNumSamples(); ++s) {
                const float raw = gate[s];
                const float intensity = std::isfinite(raw)
                    ? juce::jlimit(0.0f, 1.0f, raw) : 0.0f;
                const bool nextHigh = vm::isGateOpen(intensity);
                if (nextHigh && ! high_)
                    latchedVelocity_ = intensity;
                high_ = nextHigh;
                gate[s] = high_ ? 1.0f : 0.0f;
                velocity[s] = latchedVelocity_;
            }
        }

    private:
        bool high_ = false;
        float latchedVelocity_ = 1.0f;
    };

    class NormalizedParamOffsetProcessor : public DspNode
    {
    public:
        NormalizedParamOffsetProcessor(float minValue, float maxValue, bool logScale)
            : DspNode(BusesProperties()
                .withInput("In", juce::AudioChannelSet::mono())
                .withOutput("Out", juce::AudioChannelSet::mono())),
              min_(minValue), max_(maxValue), log_(logScale) {}
        const juce::String getName() const override { return "ParamOffsetMap"; }
        void prepareToPlay(double, int) override {}
        void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            auto* out = buf.getWritePointer(0);
            const auto* in = buf.getReadPointer(0);
            for (int s = 0; s < buf.getNumSamples(); ++s) {
                if (! std::isfinite(in[s]) || std::abs(in[s]) <= 1.0e-7f) {
                    out[s] = 0.0f;
                    continue;
                }
                const float t = juce::jlimit(0.0f, 1.0f, in[s]);
                out[s] = (log_ && min_ > 0.0f && max_ > min_)
                    ? min_ * std::pow(max_ / min_, t)
                    : min_ + t * (max_ - min_);
            }
        }
    private:
        float min_ = 0.0f, max_ = 1.0f;
        bool log_ = false;
    };

    // Brick-wall limiter + gain.
    // LEVEL is pre-limiter: hot upstream audio can be attenuated before it
    // hits the rails, and CLIP reflects post-LEVEL limiter activity.
    // Input ch0=level, ch1=audioL, ch2=audioR. Output ch0=L, ch1=R.
    class LevelGainProcessor : public DspNode
    {
    public:
        LevelGainProcessor()
            : DspNode(BusesProperties()
                .withInput("In", juce::AudioChannelSet::discreteChannels(3))
                .withOutput("Out", juce::AudioChannelSet::stereo()))
        {}

        const juce::String getName() const override { return "LevelGain"; }
        void prepareToPlay(double, int) override {}

        void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
        {
            auto* lvl = buf.getReadPointer(0);
            auto* inL = buf.getReadPointer(1);
            auto* inR = buf.getReadPointer(2);
            auto* outL = buf.getWritePointer(0);
            auto* outR = buf.getWritePointer(1);
            float lvSnapshot = lvl[0];
            float inPeak = 0.0f;
            float outPeak = 0.0f;
            bool clippedAny = false;
            // Cache level before overwriting ch0 — read/write pointers alias.
            for (int s = 0; s < buf.getNumSamples(); ++s) {
                float lv = std::isfinite(lvl[s]) ? lvl[s] : 1.0f;
                float li = std::isfinite(inL[s]) ? inL[s] : 0.0f;
                float ri = std::isfinite(inR[s]) ? inR[s] : 0.0f;
                float a = std::abs(li); if (a > inPeak) inPeak = a;
                float l = li * lv;
                float r = ri * lv;
                if (l >  1.0f) { l =  1.0f; clippedAny = true; }
                if (l < -1.0f) { l = -1.0f; clippedAny = true; }
                if (r >  1.0f) { r =  1.0f; clippedAny = true; }
                if (r < -1.0f) { r = -1.0f; clippedAny = true; }
                outL[s] = l;
                outR[s] = r;
                float b = std::abs(l); if (b > outPeak) outPeak = b;
            }
            if (clippedAny) {
                clipPeak_.store(1.0f, std::memory_order_relaxed);
                CDBG_RT(LIMITER_CLIP,
                        "LevelGain post-level clamp lv=%.4f inPeak=%.4f outPeak=%.4f",
                        lvSnapshot, inPeak, outPeak);
            }
            // Diagnostic: log lv/inPeak/outPeak when APG_BUILD flag on, rate-limited.
            // Exposes whether LevelGain is actually attenuating (lv value + peak ratio).
            if (CurlopDebug::on(CurlopDebug::APG_BUILD)) {
                if ((apgBuildDiagBlocks_++ % 200) == 0 && inPeak > 0.01f) {
                    CDBG_RT(APG_BUILD,
                            "LevelGain lv=%.4f inPeak=%.4f outPeak=%.4f ratio=%.3f",
                            lvSnapshot, inPeak, outPeak,
                            (inPeak > 1e-6f) ? (outPeak / inPeak) : 0.0f);
                }
            }
        }

        // Message-thread accessor. Returns 1.0 if clipping happened since the
        // last call (and resets), 0.0 otherwise. BinaryControl::setMeterValue
        // lights the LED when value > 0.5, so 1.0/0.0 maps cleanly.
        float exchangeClip() noexcept
        {
            return clipPeak_.exchange(0.0f, std::memory_order_relaxed);
        }

    private:
        std::atomic<float> clipPeak_ {0.0f};
        int apgBuildDiagBlocks_ = 0;
    };

// ── Adapter-shared types (ParamDef, AdapterGraph, AdapterEffectGraph).
    struct ParamDef { const char* name; float defaultVal; float minVal; float maxVal; bool logScale = false; };

    struct FaustJitBuildTiming {
        double graphInitUs = 0.0;
        double paramNamesUs = 0.0;
        double declarationUs = 0.0;
        double scriptNodeUs = 0.0;
        double faustNodeUs = 0.0;
        double graphConfigUs = 0.0;
        double controlInputGraphUs = 0.0;
        int controlInputCount = 0;
        int externallyPlumbedControlInputCount = 0;
        double audioTailWireUs = 0.0;
        double controlCoreUs = 0.0;
        double outputWireUs = 0.0;
        double controlOutputMeterUs = 0.0;
        double prepareFaustNodeUs = 0.0;
        double prepareFaustControlStateUs = 0.0;
        double prepareFaustSampleRateInitUs = 0.0;
        double prepareFaustScratchUs = 0.0;
        double constructTotalUs = 0.0;
        double constructFactoryAcquireUs = 0.0;
        double constructCompileLockWaitUs = 0.0;
        double constructInstanceCreateInitUs = 0.0;
        double constructInstanceCreateUs = 0.0;
        double constructInstanceInitUs = 0.0;
        double constructUiZoneResolveUs = 0.0;
        double constructIoControlOutputUs = 0.0;
        double constructInputContractUs = 0.0;
        double constructSchemaUs = 0.0;
        double constructFaceplateUs = 0.0;
        double constructDebugSchemaUs = 0.0;
    };

    struct AdapterGraph {
        std::unique_ptr<juce::AudioProcessorGraph> graph;
        ScriptNodeProcessor* script = nullptr;
        struct Core { std::string name; ControlCoreProcessor* ptr = nullptr; };
        std::vector<Core> cores;
        std::vector<std::unique_ptr<ControlCoreProcessor>> ownedCores;
        int numCores = 0;
        MeterProcessor* inputMeter = nullptr;  // non-null for effects
        // Set by build_faust_jit only. Non-null lets EngineSlot cache the
        // FaustNode* so the editor-window hot-swap callback can reach
        // it by moduleIdx without descending the sub-graph by type at
        // each keystroke.
        ::curlop::FaustNode* faustJit = nullptr;
        std::vector<ControlMeterProcessor*> controlOutputMeters;
        std::vector<std::string> controlInputNames;
        std::vector<vm::ControlInput> controlInputs;
        int controlInputChannelCount = 0;
        FaustJitBuildTiming faustBuildTiming;
    };

    // Build a raw EFFECT processor directly (no ModuleRegistry lookup).

    struct AdapterEffectGraph
    {
        std::unique_ptr<juce::AudioProcessorGraph> graph;
        std::vector<AdapterGraph::Core> cores;
        int numCores = 0;
        MeterProcessor* inputMeter = nullptr;
    };

} // namespace ModuleBuilder
} // namespace curlop
