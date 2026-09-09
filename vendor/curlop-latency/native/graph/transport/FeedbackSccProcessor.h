#pragma once

#include "graph/transport/FeedbackBoundaryState.h"
#include "modules/backend/CurlopDspNode.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace curlop::transport {

struct FeedbackRouteControl {
    explicit FeedbackRouteControl(float initialGain) : gain(initialGain) {}

    void setGain(float nextGain) noexcept
    {
        gain.store(nextGain, std::memory_order_relaxed);
    }

    float getGain() const noexcept
    {
        return gain.load(std::memory_order_relaxed);
    }

    std::atomic<float> gain { 1.0f };
};

// A prepared, local serial domain for one feedback SCC. Its members run in
// causal order one sample at a time; all surrounding APG nodes remain ordinary
// block processors. Feedback routes are deliberately cut from JUCE topology and
// read their complete prior descriptor frame from prepared state.
class FeedbackSccProcessor final : public DspNode {
public:
    struct Member {
        std::unique_ptr<juce::AudioProcessor> processor;
        int inputChannels = 0;
        int outputChannels = 0;
        juce::AudioBuffer<float> buffer;
        juce::MidiBuffer midi;
    };

    struct Route {
        std::size_t sourceMember = 0;
        std::size_t destinationMember = 0;
        int sourceChannel = 0;
        int destinationChannel = 0;
        SignalDescriptor descriptor = SignalDescriptor::legacyMono();
        float gain = 1.0f;
        bool feedbackBoundary = false;
        std::shared_ptr<FeedbackRouteControl> control;
    };

    struct OutputRoute {
        std::size_t sourceMember = 0;
        int sourceChannel = 0;
        int destinationChannel = 0;
        int channels = 0;
    };

    struct InputRoute {
        int sourceChannel = 0;
        std::size_t destinationMember = 0;
        int destinationChannel = 0;
        int channels = 0;
        float gain = 1.0f;
    };

    struct StaticInput {
        std::size_t destinationMember = 0;
        int destinationChannel = 0;
        float value = 0.0f;
    };

    FeedbackSccProcessor(std::vector<Member> members,
                         std::vector<Route> routes,
                         std::vector<InputRoute> inputs,
                         std::vector<StaticInput> staticInputs,
                         std::vector<OutputRoute> outputs,
                         int inputChannels,
                         int outputChannels)
        : DspNode(makeBuses(inputChannels, outputChannels))
        , inputChannels_(std::max(0, inputChannels))
        , outputChannels_(std::max(0, outputChannels))
        , members_(std::move(members))
        , routes_(std::move(routes))
        , inputs_(std::move(inputs))
        , staticInputs_(std::move(staticInputs))
        , outputs_(std::move(outputs))
    {
        validateTopology();
        routeStates_.reserve(routes_.size());
        routeScratch_.reserve(routes_.size());
        for (const auto& route : routes_) {
            if (route.feedbackBoundary)
                routeStates_.push_back(
                    std::make_unique<FeedbackBoundaryState>(route.descriptor));
            else
                routeStates_.push_back(nullptr);
            routeScratch_.emplace_back(route.descriptor.width(), 0.0f);
        }
        inputScratch_.resize(static_cast<std::size_t>(inputChannels_), 0.0f);
    }

    const juce::String getName() const override { return "FeedbackScc"; }

    void prepareToPlay(double sampleRate, int) override
    {
        for (auto& member : members_) {
            member.processor->setPlayConfigDetails(
                member.inputChannels, member.outputChannels, sampleRate, 1);
            member.processor->enableAllBuses();
            member.processor->prepareToPlay(sampleRate, 1);
            member.buffer.setSize(
                std::max({ 1, member.inputChannels, member.outputChannels }), 1,
                false, false, true);
            member.midi.clear();
        }
        reset();
        prepared_ = true;
    }

    void releaseResources() override
    {
        for (auto& member : members_)
            member.processor->releaseResources();
        prepared_ = false;
    }

    void reset() override
    {
        for (auto& member : members_)
            member.processor->reset();
        for (auto& state : routeStates_)
            if (state != nullptr)
                state->reset();
    }

    void processBlock(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer&) override
    {
        if (! prepared_) {
            buffer.clear();
            return;
        }
        const int samples = buffer.getNumSamples();

        for (int sample = 0; sample < samples; ++sample) {
            for (int channel = 0; channel < inputChannels_; ++channel)
                inputScratch_[static_cast<std::size_t>(channel)] =
                    buffer.getSample(channel, sample);
            for (int channel = 0; channel < outputChannels_; ++channel)
                buffer.setSample(channel, sample, 0.0f);
            for (std::size_t memberIndex = 0;
                 memberIndex < members_.size(); ++memberIndex) {
                auto& member = members_[memberIndex];
                member.buffer.clear();
                for (const auto& input : inputs_) {
                    if (input.destinationMember != memberIndex)
                        continue;
                    for (int channel = 0; channel < input.channels; ++channel)
                        member.buffer.addSample(
                            input.destinationChannel + channel, 0,
                            inputScratch_[static_cast<std::size_t>(
                                input.sourceChannel + channel)] * input.gain);
                }
                for (const auto& input : staticInputs_) {
                    if (input.destinationMember == memberIndex)
                        member.buffer.addSample(
                            input.destinationChannel, 0, input.value);
                }
                for (std::size_t routeIndex = 0;
                     routeIndex < routes_.size(); ++routeIndex) {
                    const auto& route = routes_[routeIndex];
                    if (route.destinationMember != memberIndex)
                        continue;
                    auto& scratch = routeScratch_[routeIndex];
                    if (route.feedbackBoundary)
                        routeStates_[routeIndex]->readFrame(
                            scratch.data(), route.descriptor.width());
                    else
                        copySourceFrame(route, scratch.data());
                    for (std::size_t channel = 0; channel < scratch.size(); ++channel)
                        member.buffer.addSample(
                            route.destinationChannel + static_cast<int>(channel),
                            0, scratch[channel] * (route.control != nullptr
                                ? route.control->getGain() : route.gain));
                }
                member.midi.clear();
                member.processor->processBlock(member.buffer, member.midi);
            }

            for (std::size_t routeIndex = 0;
                 routeIndex < routes_.size(); ++routeIndex) {
                if (! routes_[routeIndex].feedbackBoundary)
                    continue;
                auto& scratch = routeScratch_[routeIndex];
                copySourceFrame(routes_[routeIndex], scratch.data());
                routeStates_[routeIndex]->commitFrame(
                    scratch.data(), routes_[routeIndex].descriptor.width());
            }

            for (const auto& output : outputs_)
                for (int channel = 0; channel < output.channels; ++channel)
                    buffer.addSample(output.destinationChannel + channel, sample,
                        members_[output.sourceMember].buffer.getSample(
                            output.sourceChannel + channel, 0));
        }
    }

private:
    static BusesProperties makeBuses(int inputs, int outputs)
    {
        BusesProperties buses;
        if (inputs > 0)
            buses = buses.withInput(
                "In", juce::AudioChannelSet::discreteChannels(inputs));
        if (outputs > 0)
            buses = buses.withOutput(
                "Out", juce::AudioChannelSet::discreteChannels(outputs));
        return buses;
    }

    void validateTopology() const
    {
        if (members_.empty())
            throw std::invalid_argument("feedback SCC requires members");
        for (const auto& member : members_)
            if (member.processor == nullptr || member.inputChannels < 0
                || member.outputChannels < 0)
                throw std::invalid_argument("feedback SCC member is invalid");
        for (const auto& route : routes_) {
            if (route.sourceMember >= members_.size()
                || route.destinationMember >= members_.size()
                || route.descriptor.width() == 0u
                || route.sourceChannel < 0 || route.destinationChannel < 0
                || (route.descriptor.rate() != SignalRate::Audio
                    && route.descriptor.rate()
                        != SignalRate::FullRateControl)
                || route.descriptor.capabilities().packet
                || ! std::isfinite(route.gain))
                throw std::invalid_argument("feedback SCC route is invalid");
            const int width = static_cast<int>(route.descriptor.width());
            if (route.sourceChannel > members_[route.sourceMember].outputChannels - width
                || route.destinationChannel
                    > members_[route.destinationMember].inputChannels - width)
                throw std::invalid_argument("feedback SCC route exceeds member bus");
            if (! route.feedbackBoundary
                && route.sourceMember >= route.destinationMember)
                throw std::invalid_argument(
                    "ordinary feedback-SCC route violates causal member order");
        }
        for (const auto& output : outputs_)
            if (output.sourceMember >= members_.size() || output.sourceChannel < 0
                || output.destinationChannel < 0 || output.channels <= 0
                || output.sourceChannel
                    > members_[output.sourceMember].outputChannels - output.channels
                || output.destinationChannel > outputChannels_ - output.channels)
                throw std::invalid_argument("feedback SCC output route is invalid");
        for (const auto& input : inputs_)
            if (input.destinationMember >= members_.size()
                || input.sourceChannel < 0 || input.destinationChannel < 0
                || input.channels <= 0
                || input.sourceChannel > inputChannels_ - input.channels
                || input.destinationChannel
                    > members_[input.destinationMember].inputChannels
                        - input.channels)
                throw std::invalid_argument("feedback SCC input route is invalid");
        for (const auto& input : staticInputs_)
            if (input.destinationMember >= members_.size()
                || input.destinationChannel < 0
                || input.destinationChannel
                    >= members_[input.destinationMember].inputChannels
                || ! std::isfinite(input.value))
                throw std::invalid_argument("feedback SCC static input is invalid");
    }

    void copySourceFrame(const Route& route, float* destination) const noexcept
    {
        const auto& source = members_[route.sourceMember].buffer;
        for (std::size_t channel = 0; channel < route.descriptor.width(); ++channel)
            destination[channel] = source.getSample(
                route.sourceChannel + static_cast<int>(channel), 0);
    }

    int inputChannels_ = 0;
    int outputChannels_ = 0;
    std::vector<Member> members_;
    std::vector<Route> routes_;
    std::vector<InputRoute> inputs_;
    std::vector<StaticInput> staticInputs_;
    std::vector<OutputRoute> outputs_;
    std::vector<std::unique_ptr<FeedbackBoundaryState>> routeStates_;
    std::vector<std::vector<float>> routeScratch_;
    std::vector<float> inputScratch_;
    bool prepared_ = false;
};

} // namespace curlop::transport
