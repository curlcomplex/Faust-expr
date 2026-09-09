#pragma once

#include "modules/backend/CurlopDspNode.h"
#include "graph/engine/nodes/MeterProcessor.h"
#include "graph/transport/RealtimeWorkerTeam.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <memory>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace curlop::transport {

// Immutable, off-thread-built render ownership for a feed-forward region DAG.
// Every region owns its processor and private block buffer. A destination sees
// completed source spans only through the ordered boundary list, so a later
// worker executor can schedule a dependency wave without ever accumulating
// directly into shared destination storage.
class CompiledRenderPlan final {
public:
    struct WorkerCalibration {
        int selectedJobCount = 0;
        double selectedP99Us = 0.0;
        double deadlineUs = 0.0;
        bool deadlineSafe = false;
    };
    struct Region {
        std::unique_ptr<juce::AudioProcessor> processor;
        int inputChannels = 0;
        int outputChannels = 0;
        juce::AudioBuffer<float> buffer;
        juce::AudioBuffer<float> processView;
        juce::MidiBuffer midi;
    };

    struct Boundary {
        std::size_t sourceRegion = 0;
        std::size_t destinationRegion = 0;
        int sourceChannel = 0;
        int destinationChannel = 0;
        int channels = 0;
        float gain = 1.0f;
        // The current APG owns one WireProcessor buffer per edge. Retaining a
        // preallocated boundary buffer preserves its multiply-then-sum
        // floating-point treatment instead of allowing a fused add/multiply
        // to alter the reference renderer's samples.
        juce::AudioBuffer<float> wireBuffer;
    };

    struct Output {
        std::size_t sourceRegion = 0;
        int sourceChannel = 0;
        int destinationChannel = 0;
        int channels = 0;
    };

    CompiledRenderPlan(std::vector<Region> regions,
                       std::vector<Boundary> boundaries,
                       std::vector<Output> outputs,
                       int outputChannels)
        : regions_(std::move(regions))
        , boundaries_(std::move(boundaries))
        , outputs_(std::move(outputs))
        , outputChannels_(std::max(0, outputChannels))
    {
        validate();
        std::sort(boundaries_.begin(), boundaries_.end(),
                  [] (const Boundary& left, const Boundary& right) {
                      return std::tie(left.destinationRegion, left.sourceRegion,
                                      left.destinationChannel, left.sourceChannel)
                          < std::tie(right.destinationRegion, right.sourceRegion,
                                     right.destinationChannel, right.sourceChannel);
                  });
    }

    void prepareToPlay(double sampleRate, int blockSize)
    {
        sampleRate_ = sampleRate;
        preparedBlockSize_ = std::max(1, blockSize);
        for (auto& region : regions_) {
            region.processor->setPlayConfigDetails(region.inputChannels,
                                                    region.outputChannels,
                                                    sampleRate,
                                                    preparedBlockSize_);
            region.processor->enableAllBuses();
            region.processor->prepareToPlay(sampleRate, preparedBlockSize_);
            region.buffer.setSize(std::max({ 1, region.inputChannels,
                                             region.outputChannels }),
                                  preparedBlockSize_, false, false, true);
            region.processView.setDataToReferTo(
                region.buffer.getArrayOfWritePointers(),
                region.buffer.getNumChannels(), preparedBlockSize_);
            region.midi.clear();
        }
        for (auto& boundary : boundaries_)
            boundary.wireBuffer.setSize(boundary.channels, preparedBlockSize_,
                                        false, false, true);
        for (auto& meter : outputMeters_)
            if (meter != nullptr)
                meter->prepareToPlay(sampleRate_, preparedBlockSize_);
        prepared_ = true;
    }

    void releaseResources()
    {
        for (auto& region : regions_)
            region.processor->releaseResources();
        prepared_ = false;
    }

    void reset()
    {
        for (auto& region : regions_)
            region.processor->reset();
    }

    bool prepared() const noexcept { return prepared_; }
    std::size_t regionCount() const noexcept { return regions_.size(); }
    const std::vector<Boundary>& boundaries() const noexcept { return boundaries_; }
    void setVoiceCohortWave(bool enabled) noexcept { voiceCohortWave_ = enabled; }
    bool isVoiceCohortWave() const noexcept { return voiceCohortWave_; }
    // Set off the audio callback before publication. Negative retains the
    // full eligible wave; zero lets the callback retain every source.
    void setWorkerJobLimit(int limit) noexcept { workerJobLimit_ = limit; }
    int workerJobLimit() const noexcept { return workerJobLimit_; }
    // Preparation-only calibration. The plan and team are not published while
    // this runs; it measures each runnable count through the same bounded wave
    // path used at runtime and selects the lowest p99 that retains the declared
    // 30% deadline margin.
    WorkerCalibration calibrateWorkerJobLimit(RealtimeWorkerTeam& team,
                                              int measuredBlocks = 32)
    {
        WorkerCalibration calibration;
        const auto sourceCount = regions_.size() > 0 ? regions_.size() - 1 : 0;
        const int maximumJobs = sourceCount > 0
            ? std::min(static_cast<int>(sourceCount - 1), team.workerCount()) : 0;
        calibration.deadlineUs = sampleRate_ > 0.0
            ? static_cast<double>(preparedBlockSize_) / sampleRate_ * 1.0e6 : 0.0;
        if (! prepared_ || voiceCohortWave_ || ! eligibleWave_
            || ! team.completed() || maximumJobs < 0) {
            workerJobLimit_ = 0;
            return calibration;
        }

        const int blocks = std::max(1, measuredBlocks);
        const int warmBlocks = std::min(4, blocks);
        juce::AudioBuffer<float> output(std::max(1, outputChannels_), preparedBlockSize_);
        std::vector<double> timings;
        timings.reserve(static_cast<std::size_t>(blocks));
        double bestP99Us = std::numeric_limits<double>::infinity();
        int bestJobs = 0;
        bool bestSafe = false;
        for (int jobs = 0; jobs <= maximumJobs; ++jobs) {
            workerJobLimit_ = jobs;
            reset();
            bool valid = true;
            for (int warm = 0; warm < warmBlocks; ++warm) {
                output.clear();
                valid = processEligibleWave(team, output, preparedBlockSize_);
                if (! valid) break;
            }
            timings.clear();
            for (int block = 0; valid && block < blocks; ++block) {
                output.clear();
                const auto started = std::chrono::steady_clock::now();
                valid = processEligibleWave(team, output, preparedBlockSize_);
                const auto finished = std::chrono::steady_clock::now();
                timings.push_back(std::chrono::duration<double, std::micro>(
                    finished - started).count());
            }
            if (! valid || timings.empty())
                continue;
            std::sort(timings.begin(), timings.end());
            const auto p99Index = static_cast<std::size_t>(
                0.99 * static_cast<double>(timings.size() - 1));
            const double p99Us = timings[p99Index];
            const bool safe = calibration.deadlineUs > 0.0
                && p99Us <= calibration.deadlineUs * 0.70;
            if ((safe && ! bestSafe)
                || (safe == bestSafe
                    && (p99Us < bestP99Us
                        || (p99Us == bestP99Us && jobs < bestJobs)))) {
                bestP99Us = p99Us;
                bestJobs = jobs;
                bestSafe = safe;
            }
        }
        reset();
        workerJobLimit_ = bestJobs;
        // Calibration runs before publication. Its worker waves are not audio
        // callback participation and must not leak into the first snapshot.
        lastCompletedWorkerCount_.store(0, std::memory_order_release);
        lastCompletedWorkerNanos_.store(0, std::memory_order_release);
        calibration.selectedJobCount = bestJobs;
        calibration.selectedP99Us = std::isfinite(bestP99Us) ? bestP99Us : 0.0;
        calibration.deadlineSafe = bestSafe;
        return calibration;
    }
    int lastCompletedWorkerCount() const noexcept {
        return lastCompletedWorkerCount_.load(std::memory_order_acquire);
    }
    std::uint64_t lastCompletedWorkerNanos() const noexcept {
        return lastCompletedWorkerNanos_.load(std::memory_order_acquire);
    }

    // Meter/record taps are observers of a region's completed output. They
    // preserve the existing APG meter contract without reintroducing a shared
    // graph or an extra audio route into the worker wave.
    MeterProcessor* addOutputMeter(std::size_t region,
                                   std::unique_ptr<MeterProcessor> meter)
    {
        if (region >= regions_.size() || meter == nullptr)
            return nullptr;
        const auto width = regions_[region].outputChannels;
        meter->setPlayConfigDetails(width, width, 48000.0, preparedBlockSize_);
        if (prepared_)
            meter->prepareToPlay(sampleRate_, preparedBlockSize_);
        auto* raw = meter.get();
        outputMeters_[region] = std::move(meter);
        return raw;
    }

    // The reference renderer. It deliberately executes regions in compiled
    // causal order; parallel execution is a later ownership-preserving layer.
    void processSerial(juce::AudioBuffer<float>& output, int samples)
    {
        if (! prepared_ || samples < 0 || samples > preparedBlockSize_) {
            output.clear();
            return;
        }
        for (auto& region : regions_)
            region.buffer.clear(0, samples);

        for (std::size_t regionIndex = 0; regionIndex < regions_.size(); ++regionIndex) {
            auto& region = regions_[regionIndex];
            for (auto& boundary : boundaries_)
                if (boundary.destinationRegion == regionIndex)
                    routeBoundary(boundary, regions_[boundary.sourceRegion].buffer,
                                  region.buffer, samples);
            region.midi.clear();
            region.processView.setDataToReferTo(
                region.buffer.getArrayOfWritePointers(),
                region.buffer.getNumChannels(), samples);
            region.processor->processBlock(region.processView, region.midi);
        }

        output.clear();
        for (const auto& endpoint : outputs_)
            addRange(regions_[endpoint.sourceRegion].buffer, endpoint.sourceChannel,
                     output, endpoint.destinationChannel, endpoint.channels,
                     samples, 1.0f);
    }

    // Qualified parallel shape: independent source regions followed by one
    // destination-owned output region. The callback owns the first source;
    // prestarted workers render the remaining sources. This is deliberately
    // fixed at plan construction: callback participation cannot be left to a
    // race with a worker claiming every source job.
    bool processEligibleWave(RealtimeWorkerTeam& team,
                             juce::AudioBuffer<float>& output, int samples)
    {
        if (voiceCohortWave_)
            return processVoiceCohortWave(team, output, samples);
        const auto sourceCount = regions_.size() > 0 ? regions_.size() - 1 : 0;
        const int maximumWorkerJobs = sourceCount > 0
            ? static_cast<int>(sourceCount - 1) : 0;
        const int workerJobs = std::clamp(
            workerJobLimit_ < 0 ? maximumWorkerJobs : workerJobLimit_,
            0, maximumWorkerJobs);
        if (! prepared_ || samples < 0 || samples > preparedBlockSize_
            || regions_.size() < 3 || (workerJobs > 0 && team.workerCount() < 1)
            || !eligibleWave_ || !team.completed()) {
            lastCompletedWorkerCount_.store(0, std::memory_order_release);
            lastCompletedWorkerNanos_.store(0, std::memory_order_release);
            invalidateOutputMeters();
            output.clear();
            return false;
        }
        for (auto& region : regions_) region.buffer.clear(0, samples);
        for (int job = 0; job < workerJobs; ++job) {
            const auto source = static_cast<std::size_t>(job + 1);
            jobContexts_[job] = { this, source, samples };
            jobs_[job] = { runRegion, &jobContexts_[job] };
        }
        if (workerJobs > 0)
            team.dispatch(jobs_.data(), workerJobs);
        // Source zero remains callback-owned, but must overlap the prepared
        // worker wave rather than complete before it is even dispatched.
        processRegion(0, samples, false);
        for (std::size_t source = static_cast<std::size_t>(workerJobs + 1);
             source < sourceCount; ++source)
            processRegion(source, samples, false);
        const auto waitDeadline = std::chrono::steady_clock::now()
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(
                    static_cast<double>(samples) / sampleRate_ * 0.5));
        while (! team.completed()
               && std::chrono::steady_clock::now() < waitDeadline) {
            // Do not steal the first job before a worker has claimed it: that
            // job may be the delayed source whose unfinished private buffer
            // is what makes this wave fail-silent. Once a worker has entered
            // the wave, the callback can safely drain later queued sources
            // rather than burning its remaining budget in an empty spin.
            if (team.workerHasClaimedCurrentWave())
                team.tryRunOne();
        }
        if (! team.completed()) {
            lastCompletedWorkerCount_.store(0, std::memory_order_release);
            lastCompletedWorkerNanos_.store(0, std::memory_order_release);
            invalidateOutputMeters();
            output.clear();
            return false;
        }
        const auto sink = regions_.size() - 1;
        for (std::size_t source = 0; source < sink; ++source)
            observeRegionMeter(source);
        for (auto& boundary : boundaries_)
            routeBoundary(boundary, regions_[boundary.sourceRegion].buffer,
                          regions_[sink].buffer, samples);
        processRegion(sink, samples);
        output.clear();
        for (const auto& endpoint : outputs_)
            addRange(regions_[endpoint.sourceRegion].buffer, endpoint.sourceChannel,
                     output, endpoint.destinationChannel, endpoint.channels, samples, 1.0f);
        lastCompletedWorkerCount_.store(workerJobs > 0
                                            ? team.participatingWorkerCount()
                                            : 0,
                                        std::memory_order_release);
        lastCompletedWorkerNanos_.store(workerJobs > 0 ? team.workerExecutionNanos() : 0,
                                        std::memory_order_release);
        return true;
    }

    int parallelSourceCount() const noexcept
    {
        return eligibleWave_ ? static_cast<int>(regions_.size()) - 1 : 0;
    }

private:
    bool processVoiceCohortWave(RealtimeWorkerTeam& team,
                                juce::AudioBuffer<float>& output, int samples)
    {
        if (! prepared_ || samples < 0 || samples > preparedBlockSize_
            || regions_.size() != 2 || team.workerCount() < 1
            || ! eligibleWave_ || ! team.completed()) {
            lastCompletedWorkerCount_.store(0, std::memory_order_release);
            lastCompletedWorkerNanos_.store(0, std::memory_order_release);
            invalidateOutputMeters();
            output.clear();
            return false;
        }
        for (auto& region : regions_)
            region.buffer.clear(0, samples);
        processRegion(0, samples, false);
        // FaustNode owns the bounded cohort completion wait. An unfinished
        // cohort cannot expose its private source span to the output region.
        if (! team.completed()) {
            lastCompletedWorkerCount_.store(0, std::memory_order_release);
            lastCompletedWorkerNanos_.store(0, std::memory_order_release);
            invalidateOutputMeters();
            output.clear();
            return false;
        }
        observeRegionMeter(0);
        for (auto& boundary : boundaries_)
            routeBoundary(boundary, regions_[boundary.sourceRegion].buffer,
                          regions_[boundary.destinationRegion].buffer, samples);
        processRegion(1, samples);
        output.clear();
        for (const auto& endpoint : outputs_)
            addRange(regions_[endpoint.sourceRegion].buffer, endpoint.sourceChannel,
                     output, endpoint.destinationChannel, endpoint.channels,
                     samples, 1.0f);
        lastCompletedWorkerCount_.store(team.participatingWorkerCount(),
                                        std::memory_order_release);
        lastCompletedWorkerNanos_.store(team.workerExecutionNanos(),
                                        std::memory_order_release);
        return true;
    }
    struct RegionJob { CompiledRenderPlan* owner; std::size_t index; int samples; };
    static void runRegion(void* context) noexcept {
        auto& job = *static_cast<RegionJob*>(context);
        job.owner->processRegion(job.index, job.samples, false);
    }
    void processRegion(std::size_t index, int samples,
                       bool observeMeter = true) noexcept {
        auto& region = regions_[index];
        region.midi.clear();
        region.processView.setDataToReferTo(region.buffer.getArrayOfWritePointers(),
                                            region.buffer.getNumChannels(), samples);
        region.processor->processBlock(region.processView, region.midi);
        if (observeMeter)
            observeRegionMeter(index);
    }
    void observeRegionMeter(std::size_t index) noexcept {
        auto& region = regions_[index];
        if (auto* meter = outputMeters_[index].get())
            meter->processBlock(region.processView, region.midi);
    }
    void invalidateOutputMeters() noexcept
    {
        for (auto& meter : outputMeters_)
            if (meter != nullptr)
                meter->invalidateCapture();
    }
    static void addRange(const juce::AudioBuffer<float>& source, int sourceChannel,
                         juce::AudioBuffer<float>& destination, int destinationChannel,
                         int channels, int samples, float gain) noexcept
    {
        for (int channel = 0; channel < channels; ++channel)
            destination.addFrom(destinationChannel + channel, 0, source,
                                sourceChannel + channel, 0, samples, gain);
    }

    static void routeBoundary(Boundary& boundary,
                              const juce::AudioBuffer<float>& source,
                              juce::AudioBuffer<float>& destination,
                              int samples) noexcept
    {
        for (int channel = 0; channel < boundary.channels; ++channel) {
            const auto* input = source.getReadPointer(boundary.sourceChannel + channel);
            auto* wire = boundary.wireBuffer.getWritePointer(channel);
            auto* output = destination.getWritePointer(
                boundary.destinationChannel + channel);
            for (int sample = 0; sample < samples; ++sample)
                wire[sample] = input[sample] * boundary.gain;
            for (int sample = 0; sample < samples; ++sample)
                output[sample] += wire[sample];
        }
    }

    void validate()
    {
        if (regions_.empty() || outputChannels_ <= 0)
            throw std::invalid_argument("compiled render plan is empty");
        for (const auto& region : regions_)
            if (region.processor == nullptr || region.inputChannels < 0
                || region.outputChannels < 0)
                throw std::invalid_argument("compiled render region is invalid");
        for (const auto& boundary : boundaries_)
            if (boundary.sourceRegion >= regions_.size()
                || boundary.destinationRegion >= regions_.size()
                || boundary.sourceRegion >= boundary.destinationRegion
                || boundary.sourceChannel < 0 || boundary.destinationChannel < 0
                || boundary.channels <= 0
                || boundary.sourceChannel > regions_[boundary.sourceRegion].outputChannels - boundary.channels
                || boundary.destinationChannel > regions_[boundary.destinationRegion].inputChannels - boundary.channels)
                throw std::invalid_argument("compiled render boundary is invalid");
        for (const auto& endpoint : outputs_)
            if (endpoint.sourceRegion >= regions_.size()
                || endpoint.sourceChannel < 0 || endpoint.destinationChannel < 0
                || endpoint.channels <= 0
                || endpoint.sourceChannel > regions_[endpoint.sourceRegion].outputChannels - endpoint.channels
                || endpoint.destinationChannel > outputChannels_ - endpoint.channels)
                throw std::invalid_argument("compiled render output is invalid");
        eligibleWave_ = true;
        const auto sink = regions_.size() - 1;
        for (const auto& boundary : boundaries_)
            eligibleWave_ = eligibleWave_ && boundary.destinationRegion == sink
                && boundary.sourceRegion < sink;
    }

    int outputChannels_ = 0;
    int preparedBlockSize_ = 0;
    double sampleRate_ = 48000.0;
    bool prepared_ = false;
    std::vector<Region> regions_;
    std::vector<Boundary> boundaries_;
    std::vector<Output> outputs_;
    std::vector<std::unique_ptr<MeterProcessor>> outputMeters_ =
        std::vector<std::unique_ptr<MeterProcessor>>(regions_.size());
    bool eligibleWave_ = false;
    bool voiceCohortWave_ = false;
    int workerJobLimit_ = -1;
    std::atomic<int> lastCompletedWorkerCount_ { 0 };
    std::atomic<std::uint64_t> lastCompletedWorkerNanos_ { 0 };
    std::vector<RealtimeWorkerTeam::Job> jobs_ =
        std::vector<RealtimeWorkerTeam::Job>(regions_.size());
    std::vector<RegionJob> jobContexts_ = std::vector<RegionJob>(regions_.size());
};

} // namespace curlop::transport
