#pragma once

#include "control/vm/machine/Machine.h"
#include "graph/transport/SignalDescriptor.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace curlop::transport {

// Off-thread-prepared, allocation-free block transport for one described
// signal. The descriptor is a stable value; the runtime capacity is only an
// observable rebuild diagnostic and is deliberately absent from persistence.
class PreparedSignalBlock final {
public:
    enum class OverflowReason : std::uint8_t {
        None,
        ActiveWidthExceeded,
        ChannelOutOfRange,
        PacketCapacityExceeded,
        AliasedOperation,
        FanInNotPrepared
    };

    struct ChannelView {
        const float* read = nullptr;
        float* write = nullptr;
        std::uint32_t samples = 0;
    };

    struct RoutedEmission {
        std::uint32_t channelIndex = 0;
        vm::Emission emission;
    };

    struct OperationResult {
        std::size_t accepted = 0;
        std::size_t rejected = 0;
    };

    struct StorageSnapshot {
        const void* channelViews = nullptr;
        const void* packets = nullptr;
        const void* fanInScratch = nullptr;
        const void* sortIndices = nullptr;
        std::size_t channelViewCapacity = 0;
        std::size_t packetCapacity = 0;
        std::size_t fanInScratchCapacity = 0;
        std::size_t sortIndexCapacity = 0;
    };

    PreparedSignalBlock(
        SignalDescriptor descriptor,
        SignalWidth preparedWidth,
        std::size_t packetCapacity,
        bool prepareFanIn = true);

    PreparedSignalBlock(const PreparedSignalBlock&) = delete;
    PreparedSignalBlock& operator=(const PreparedSignalBlock&) = delete;
    PreparedSignalBlock(PreparedSignalBlock&&) = delete;
    PreparedSignalBlock& operator=(PreparedSignalBlock&&) = delete;

    const SignalDescriptor& descriptor() const noexcept;
    SignalWidth preparedWidth() const noexcept;
    SignalWidth activeWidth() const noexcept;
    std::uint32_t blockSamples() const noexcept;

    bool beginBlock(
        SignalWidth activeWidth,
        std::uint32_t blockSamples) noexcept;
    bool setChannelView(
        std::uint32_t channelIndex,
        const float* read,
        float* write) noexcept;
    const ChannelView& channelView(std::uint32_t channelIndex) const noexcept;

    bool route(
        std::uint32_t channelIndex,
        const vm::Emission& emission) noexcept;
    std::size_t packetCount() const noexcept;
    std::size_t packetCapacity() const noexcept;
    const RoutedEmission& packet(std::size_t index) const noexcept;

    // A destination may not alias this source. An aliased destination is
    // rejected without changing the source; other destinations still receive
    // the immutable source snapshot.
    OperationResult fanOutTo(
        PreparedSignalBlock* const* destinations,
        std::size_t destinationCount) const noexcept;
    // Inputs may not alias this destination. If any does, the complete merge
    // is rejected and the destination remains unchanged.
    OperationResult mergeFanIn(
        const PreparedSignalBlock* const* inputs,
        std::size_t inputCount) noexcept;

    std::uint64_t overflowCount() const noexcept;
    OverflowReason lastOverflowReason() const noexcept;
    bool rebuildRequested() const noexcept;
    std::uint64_t capacityRebuildGeneration() const noexcept;
    std::uint64_t requiredWidthHighWater() const noexcept;
    std::size_t requiredPacketCapacityHighWater() const noexcept;
    void acknowledgeCapacityRebuild(
        std::uint64_t observedGeneration) noexcept;

    StorageSnapshot storageSnapshot() const noexcept;
    bool storageMatches(const StorageSnapshot& snapshot) const noexcept;

private:
    void noteOverflow(OverflowReason reason) noexcept;
    void noteRequiredWidth(std::uint64_t width) noexcept;
    void noteRequiredPacketCapacity(std::size_t capacity) noexcept;

    const SignalDescriptor descriptor_;
    const SignalWidth preparedWidth_;
    const std::size_t packetCapacity_;
    const bool fanInPrepared_;
    SignalWidth activeWidth_ = 0;
    std::uint32_t blockSamples_ = 0;

    // Every vector is fully sized during construction. Runtime code indexes
    // prepared storage directly and never calls a growth-capable operation.
    std::vector<ChannelView> channelViews_;
    std::vector<RoutedEmission> packets_;
    std::vector<RoutedEmission> fanInScratch_;
    // Packet capacity is a host-size preparation concern, not a channel
    // count. Keep sort indices lossless even when capacity exceeds uint32.
    std::vector<std::size_t> sortIndices_;
    std::size_t packetCount_ = 0;
    std::size_t fanInScratchCount_ = 0;
    std::size_t packetDemandThisBlock_ = 0;

    // Overflow demand is a cross-thread hand-off. The callback only advances
    // the observed count; an off-thread rebuild owner explicitly acknowledges
    // the count it has seen. A later callback overflow therefore cannot be
    // erased by either beginBlock() or a concurrent acknowledgement.
    std::atomic<std::uint64_t> overflowGeneration_{ 0 };
    std::atomic<std::uint64_t> acknowledgedOverflowGeneration_{ 0 };
    std::atomic<OverflowReason> lastOverflowReason_{ OverflowReason::None };
    // One wider than SignalWidth so an attempted channel UINT32_MAX can
    // report the exact required width 2^32 instead of wrapping to zero.
    std::atomic<std::uint64_t> requiredWidthHighWater_;
    std::atomic<std::size_t> requiredPacketCapacityHighWater_;
};

} // namespace curlop::transport
