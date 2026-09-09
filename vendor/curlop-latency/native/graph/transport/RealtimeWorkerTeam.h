#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <array>
#include <vector>

namespace curlop::transport {

// Fixed, prestarted workers for compiled feed-forward waves. Dispatch and
// tryRunOne are callback-safe: they only touch preallocated storage and atomics.
class RealtimeWorkerTeam final {
public:
    struct Job {
        void (*run)(void*) noexcept = nullptr;
        void* context = nullptr;
    };

    RealtimeWorkerTeam();
    ~RealtimeWorkerTeam();
    RealtimeWorkerTeam(const RealtimeWorkerTeam&) = delete;
    RealtimeWorkerTeam& operator=(const RealtimeWorkerTeam&) = delete;

    void prepare(int workerCount, double sampleRate, int blockSize);
    void release();
    // The host callback retains the newest workgroup without waiting for a
    // worker that is copying the current group. dispatch() publishes that
    // pending value before it wakes the next wave.
    void setWorkgroup(juce::AudioWorkgroup);

    // Narrow deterministic seam for the native wake-object failure path.
    // It is used only by the unit suite; production always leaves it false.
    static void setWakeConstructionFailuresForTests(int count) noexcept;

    // Caller retains `jobs` until completed() is true. A new wave may only be
    // dispatched after the preceding wave completes.
    void dispatch(const Job* jobs, int count) noexcept;
    bool tryRunOne() noexcept;
    bool completed() const noexcept { return outstanding_.load(std::memory_order_acquire) == 0; }
    int workerCount() const noexcept { return static_cast<int>(workers_.size()); }
    int participatingWorkerCount() const noexcept;
    bool workerHasClaimedCurrentWave() const noexcept;
    std::uint64_t workerExecutionNanos() const noexcept {
        return workerExecutionNanos_.load(std::memory_order_acquire);
    }
    std::uint64_t appliedWorkgroupGeneration() const noexcept {
        return appliedWorkgroupGeneration_.load(std::memory_order_acquire);
    }

private:
    class Worker;
    bool claimAndRun(int workerIndex) noexcept;
    juce::AudioWorkgroup workgroupForWorker();
    void publishPendingWorkgroup() noexcept;

    std::vector<std::unique_ptr<Worker>> workers_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> workerWaveEpochs_;
    std::atomic<const Job*> jobs_ { nullptr };
    std::atomic<int> jobCount_ { 0 }, nextJob_ { 0 }, outstanding_ { 0 };
    std::atomic<std::uint64_t> activeWaveEpoch_ { 0 };
    std::atomic<std::uint64_t> workerExecutionNanos_ { 0 };
    // A worker only copies the current slot. Three slots give a new wave a
    // fresh target while a just-replaced slot is still being released.
    static constexpr std::size_t kWorkgroupSlotCount = 3;
    std::array<juce::AudioWorkgroup, kWorkgroupSlotCount> workgroups_;
    std::array<std::atomic<int>, kWorkgroupSlotCount> workgroupReaders_ { 0, 0, 0 };
    std::atomic<int> activeWorkgroup_ { 0 };
    // setWorkgroup() and dispatch() are both called by the host callback.
    // Workers never touch this pending handle, so retaining an update is
    // independent of their short-lived slot-copy critical section.
    juce::AudioWorkgroup pendingWorkgroup_;
    bool hasPendingWorkgroup_ = false;
    std::atomic<std::uint64_t> appliedWorkgroupGeneration_ { 0 };
    double sampleRate_ = 0.0;
    int blockSize_ = 0;
};

} // namespace curlop::transport
