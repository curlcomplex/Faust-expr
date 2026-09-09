#include "graph/transport/RealtimeWorkerTeam.h"

#if JUCE_MAC
 #include <mach/mach.h>
 #include <mach/semaphore.h>
#elif JUCE_WINDOWS
 #include <windows.h>
#else
 #include <cerrno>
 #include <semaphore.h>
#endif

#include <chrono>

namespace curlop::transport {

namespace {
std::atomic<int> wakeConstructionFailuresForTests { 0 };

bool consumeWakeConstructionFailureForTests() noexcept
{
    int remaining = wakeConstructionFailuresForTests.load(std::memory_order_acquire);
    while (remaining > 0) {
        if (wakeConstructionFailuresForTests.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return true;
    }
    return false;
}
}

class RealtimeWorkerTeam::Worker final : private juce::Thread {
public:
    Worker(RealtimeWorkerTeam& owner, int index)
        : juce::Thread("curlop-render-" + juce::String(index)), owner_(owner), index_(index)
    {
        if (consumeWakeConstructionFailureForTests())
            return;
#if JUCE_MAC
        wakeReady_ = semaphore_create(mach_task_self(), &wakeSemaphore_, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS;
#elif JUCE_WINDOWS
        wakeEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        wakeReady_ = wakeEvent_ != nullptr;
#else
        wakeReady_ = sem_init(&wakeSemaphore_, 0, 0) == 0;
#endif
    }
    ~Worker() override {
        stop();
#if JUCE_MAC
        if (wakeReady_ && wakeSemaphore_ != MACH_PORT_NULL)
            semaphore_destroy(mach_task_self(), wakeSemaphore_);
#elif JUCE_WINDOWS
        if (wakeReady_ && wakeEvent_ != nullptr)
            CloseHandle(wakeEvent_);
#else
        if (wakeReady_) sem_destroy(&wakeSemaphore_);
#endif
    }
    void start(double sampleRate, int blockSize) {
        if (! wakeReady_) return;
        startRealtimeThread(juce::Thread::RealtimeOptions{}
            .withApproximateAudioProcessingTime(blockSize, sampleRate));
    }
    bool ready() const noexcept { return wakeReady_; }
    void stop() { signalThreadShouldExit(); wake(); waitForThreadToExit(2000); }
    void wake() noexcept {
#if JUCE_MAC
        semaphore_signal(wakeSemaphore_);
#elif JUCE_WINDOWS
        SetEvent(wakeEvent_);
#else
        sem_post(&wakeSemaphore_);
#endif
    }
private:
    bool waitForWake() noexcept {
#if JUCE_MAC
        return semaphore_wait(wakeSemaphore_) == KERN_SUCCESS;
#elif JUCE_WINDOWS
        return WaitForSingleObject(wakeEvent_, INFINITE) == WAIT_OBJECT_0;
#else
        int result = 0;
        do { result = sem_wait(&wakeSemaphore_); } while (result != 0 && errno == EINTR);
        return result == 0;
#endif
    }
    void run() override {
        juce::WorkgroupToken token;
        while (!threadShouldExit()) {
            if (! waitForWake()) return;
            if (threadShouldExit()) break;
            owner_.workgroupForWorker().join(token);
            while (owner_.claimAndRun(index_)) {}
        }
    }
    RealtimeWorkerTeam& owner_;
    int index_ = -1;
#if JUCE_MAC
    semaphore_t wakeSemaphore_ = MACH_PORT_NULL;
#elif JUCE_WINDOWS
    HANDLE wakeEvent_ = nullptr;
#else
    sem_t wakeSemaphore_ {};
#endif
    bool wakeReady_ = false;
};

RealtimeWorkerTeam::RealtimeWorkerTeam() = default;
RealtimeWorkerTeam::~RealtimeWorkerTeam() { release(); }

void RealtimeWorkerTeam::setWakeConstructionFailuresForTests(int count) noexcept
{
    wakeConstructionFailuresForTests.store(juce::jmax(0, count), std::memory_order_release);
}

void RealtimeWorkerTeam::prepare(int count, double sampleRate, int blockSize)
{
    release();
    sampleRate_ = sampleRate; blockSize_ = blockSize;
    workers_.reserve(static_cast<size_t>(juce::jmax(0, count)));
    if (count > 0) {
        workerWaveEpochs_ = std::make_unique<std::atomic<std::uint64_t>[]>(
            static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
            workerWaveEpochs_[i].store(0, std::memory_order_relaxed);
    }
    for (int i = 0; i < count; ++i) {
        auto worker = std::make_unique<Worker>(*this, static_cast<int>(workers_.size()));
        if (worker->ready()) workers_.push_back(std::move(worker));
    }
    for (auto& worker : workers_) worker->start(sampleRate_, blockSize_);
}

void RealtimeWorkerTeam::release()
{
    for (auto& worker : workers_) worker->stop();
    workers_.clear(); workerWaveEpochs_.reset(); jobs_.store(nullptr, std::memory_order_release);
    jobCount_.store(0, std::memory_order_release); outstanding_.store(0, std::memory_order_release);
    activeWaveEpoch_.store(0, std::memory_order_release);
    workerExecutionNanos_.store(0, std::memory_order_release);
}

void RealtimeWorkerTeam::setWorkgroup(juce::AudioWorkgroup group)
{
    // This is deliberately a callback-owned mailbox, not a direct write to a
    // worker-visible slot. A rapid sequence coalesces to the newest host
    // context, and dispatch() applies it before its next wake-up.
    pendingWorkgroup_ = std::move(group);
    hasPendingWorkgroup_ = true;
}

void RealtimeWorkerTeam::publishPendingWorkgroup() noexcept
{
    if (! hasPendingWorkgroup_)
        return;

    const int active = activeWorkgroup_.load(std::memory_order_acquire);
    for (std::size_t offset = 1; offset < kWorkgroupSlotCount; ++offset) {
        const auto target = static_cast<int>((active + static_cast<int>(offset))
                                             % static_cast<int>(kWorkgroupSlotCount));
        if (workgroupReaders_[static_cast<std::size_t>(target)].load(std::memory_order_acquire) != 0)
            continue;
        workgroups_[static_cast<std::size_t>(target)] = std::move(pendingWorkgroup_);
        hasPendingWorkgroup_ = false;
        activeWorkgroup_.store(target, std::memory_order_release);
        appliedWorkgroupGeneration_.fetch_add(1, std::memory_order_release);
        return;
    }
}

juce::AudioWorkgroup RealtimeWorkerTeam::workgroupForWorker()
{
    for (;;) {
        const int index = activeWorkgroup_.load(std::memory_order_acquire);
        auto& readers = workgroupReaders_[static_cast<std::size_t>(index)];
        readers.fetch_add(1, std::memory_order_acq_rel);
        if (index == activeWorkgroup_.load(std::memory_order_acquire)) {
            auto copy = workgroups_[static_cast<std::size_t>(index)];
            readers.fetch_sub(1, std::memory_order_release);
            return copy;
        }
        readers.fetch_sub(1, std::memory_order_release);
    }
}

void RealtimeWorkerTeam::dispatch(const Job* jobs, int count) noexcept
{
    publishPendingWorkgroup();
    jobs_.store(jobs, std::memory_order_release);
    jobCount_.store(juce::jmax(0, count), std::memory_order_release);
    nextJob_.store(0, std::memory_order_release);
    outstanding_.store(juce::jmax(0, count), std::memory_order_release);
    workerExecutionNanos_.store(0, std::memory_order_release);
    activeWaveEpoch_.fetch_add(1, std::memory_order_release);
    for (auto& worker : workers_) worker->wake();
}

bool RealtimeWorkerTeam::claimAndRun(int workerIndex) noexcept
{
    const int index = nextJob_.fetch_add(1, std::memory_order_acq_rel);
    if (index >= jobCount_.load(std::memory_order_acquire)) return false;
    if (workerIndex >= 0 && workerIndex < workerCount() && workerWaveEpochs_ != nullptr)
        workerWaveEpochs_[workerIndex].store(
            activeWaveEpoch_.load(std::memory_order_acquire), std::memory_order_release);
    const auto* jobs = jobs_.load(std::memory_order_acquire);
    const auto started = workerIndex >= 0 ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    if (jobs != nullptr && jobs[index].run != nullptr) jobs[index].run(jobs[index].context);
    if (workerIndex >= 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        workerExecutionNanos_.fetch_add(static_cast<std::uint64_t>(std::max<int64_t>(0, elapsed)),
                                        std::memory_order_acq_rel);
    }
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

bool RealtimeWorkerTeam::tryRunOne() noexcept { return claimAndRun(-1); }

int RealtimeWorkerTeam::participatingWorkerCount() const noexcept
{
    const auto epoch = activeWaveEpoch_.load(std::memory_order_acquire);
    if (epoch == 0 || workerWaveEpochs_ == nullptr)
        return 0;
    int count = 0;
    for (int worker = 0; worker < workerCount(); ++worker)
        if (workerWaveEpochs_[worker].load(std::memory_order_acquire) == epoch)
            ++count;
    return count;
}

bool RealtimeWorkerTeam::workerHasClaimedCurrentWave() const noexcept
{
    const auto epoch = activeWaveEpoch_.load(std::memory_order_acquire);
    if (epoch == 0 || workerWaveEpochs_ == nullptr)
        return false;
    for (int worker = 0; worker < workerCount(); ++worker)
        if (workerWaveEpochs_[worker].load(std::memory_order_acquire) == epoch)
            return true;
    return false;
}

} // namespace curlop::transport
