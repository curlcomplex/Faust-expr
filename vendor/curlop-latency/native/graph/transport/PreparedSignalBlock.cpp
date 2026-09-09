#include "graph/transport/PreparedSignalBlock.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace curlop::transport {

PreparedSignalBlock::PreparedSignalBlock(
    SignalDescriptor descriptor,
    SignalWidth preparedWidth,
    std::size_t packetCapacity,
    bool prepareFanIn)
    : descriptor_(std::move(descriptor))
    , preparedWidth_(preparedWidth)
    , packetCapacity_(packetCapacity)
    , fanInPrepared_(prepareFanIn)
    , channelViews_(preparedWidth)
    , packets_(packetCapacity)
    , fanInScratch_(prepareFanIn ? packetCapacity : 0u)
    , sortIndices_(prepareFanIn ? packetCapacity : 0u)
    , requiredWidthHighWater_(preparedWidth)
    , requiredPacketCapacityHighWater_(packetCapacity)
{
    if (preparedWidth == 0u || preparedWidth > descriptor_.width())
        throw std::invalid_argument(
            "prepared width must be within the signal descriptor width");
}

const SignalDescriptor& PreparedSignalBlock::descriptor() const noexcept
{
    return descriptor_;
}

SignalWidth PreparedSignalBlock::preparedWidth() const noexcept
{
    return preparedWidth_;
}

SignalWidth PreparedSignalBlock::activeWidth() const noexcept
{
    return activeWidth_;
}

std::uint32_t PreparedSignalBlock::blockSamples() const noexcept
{
    return blockSamples_;
}

bool PreparedSignalBlock::beginBlock(
    SignalWidth activeWidth,
    std::uint32_t blockSamples) noexcept
{
    activeWidth_ = 0u;
    blockSamples_ = blockSamples;
    packetCount_ = 0u;
    fanInScratchCount_ = 0u;
    packetDemandThisBlock_ = 0u;

    for (auto& view : channelViews_)
        view = {};

    if (activeWidth > preparedWidth_) {
        noteRequiredWidth(activeWidth);
        noteOverflow(OverflowReason::ActiveWidthExceeded);
        return false;
    }

    activeWidth_ = activeWidth;
    return true;
}

bool PreparedSignalBlock::setChannelView(
    std::uint32_t channelIndex,
    const float* read,
    float* write) noexcept
{
    if (channelIndex >= activeWidth_) {
        noteRequiredWidth(
            static_cast<std::uint64_t>(channelIndex) + 1u);
        noteOverflow(OverflowReason::ChannelOutOfRange);
        return false;
    }

    channelViews_[channelIndex] = { read, write, blockSamples_ };
    return true;
}

const PreparedSignalBlock::ChannelView&
PreparedSignalBlock::channelView(std::uint32_t channelIndex) const noexcept
{
    static const ChannelView empty;
    return channelIndex < preparedWidth_ ? channelViews_[channelIndex] : empty;
}

bool PreparedSignalBlock::route(
    std::uint32_t channelIndex,
    const vm::Emission& emission) noexcept
{
    if (channelIndex >= activeWidth_) {
        if (descriptor_.capabilities().packet
            && channelIndex < preparedWidth_) {
            activeWidth_ = channelIndex + 1u;
        } else {
            noteRequiredWidth(
                static_cast<std::uint64_t>(channelIndex) + 1u);
            noteOverflow(OverflowReason::ChannelOutOfRange);
            return false;
        }
    }
    ++packetDemandThisBlock_;
    noteRequiredPacketCapacity(packetDemandThisBlock_);
    if (packetCount_ >= packetCapacity_) {
        noteOverflow(OverflowReason::PacketCapacityExceeded);
        return false;
    }

    packets_[packetCount_++] = { channelIndex, emission };
    return true;
}

std::size_t PreparedSignalBlock::packetCount() const noexcept
{
    return packetCount_;
}

std::size_t PreparedSignalBlock::packetCapacity() const noexcept
{
    return packetCapacity_;
}

const PreparedSignalBlock::RoutedEmission&
PreparedSignalBlock::packet(std::size_t index) const noexcept
{
    return packets_[index];
}

PreparedSignalBlock::OperationResult PreparedSignalBlock::fanOutTo(
    PreparedSignalBlock* const* destinations,
    std::size_t destinationCount) const noexcept
{
    OperationResult result;
    for (std::size_t destinationIndex = 0;
         destinationIndex < destinationCount;
         ++destinationIndex) {
        auto* destination = destinations[destinationIndex];
        if (destination == nullptr) {
            result.rejected += packetCount_;
            continue;
        }
        if (destination == this) {
            result.rejected += packetCount_;
            destination->noteOverflow(OverflowReason::AliasedOperation);
            continue;
        }
        for (std::size_t packetIndex = 0; packetIndex < packetCount_;
             ++packetIndex) {
            const auto& routed = packets_[packetIndex];
            if (destination->route(
                    routed.channelIndex, routed.emission))
                ++result.accepted;
            else
                ++result.rejected;
        }
    }
    return result;
}

PreparedSignalBlock::OperationResult PreparedSignalBlock::mergeFanIn(
    const PreparedSignalBlock* const* inputs,
    std::size_t inputCount) noexcept
{
    OperationResult result;
    if (! fanInPrepared_) {
        for (std::size_t inputIndex = 0; inputIndex < inputCount; ++inputIndex)
            if (inputs[inputIndex] != nullptr)
                result.rejected += inputs[inputIndex]->packetCount_;
        noteOverflow(OverflowReason::FanInNotPrepared);
        return result;
    }
    bool aliasesDestination = false;
    for (std::size_t inputIndex = 0; inputIndex < inputCount; ++inputIndex) {
        const auto* input = inputs[inputIndex];
        if (input != nullptr)
            result.rejected += input->packetCount_;
        aliasesDestination = aliasesDestination || input == this;
    }
    if (aliasesDestination) {
        noteOverflow(OverflowReason::AliasedOperation);
        return result;
    }

    result.rejected = 0u;
    packetCount_ = 0u;
    fanInScratchCount_ = 0u;
    packetDemandThisBlock_ = 0u;

    std::size_t requiredPacketCapacity = 0u;
    for (std::size_t inputIndex = 0; inputIndex < inputCount; ++inputIndex) {
        if (inputs[inputIndex] == nullptr)
            continue;
        const auto inputPackets = inputs[inputIndex]->packetCount_;
        if (inputPackets
            > std::numeric_limits<std::size_t>::max()
                - requiredPacketCapacity) {
            requiredPacketCapacity =
                std::numeric_limits<std::size_t>::max();
            break;
        }
        requiredPacketCapacity += inputPackets;
    }
    packetDemandThisBlock_ = requiredPacketCapacity;
    noteRequiredPacketCapacity(requiredPacketCapacity);

    for (std::size_t inputIndex = 0; inputIndex < inputCount; ++inputIndex) {
        const auto* input = inputs[inputIndex];
        if (input == nullptr)
            continue;

        for (std::size_t packetIndex = 0;
             packetIndex < input->packetCount_;
             ++packetIndex) {
            const auto& routed = input->packets_[packetIndex];
            if (routed.channelIndex >= activeWidth_) {
                noteRequiredWidth(
                    static_cast<std::uint64_t>(routed.channelIndex) + 1u);
                noteOverflow(OverflowReason::ChannelOutOfRange);
                ++result.rejected;
                continue;
            }
            if (fanInScratchCount_ >= packetCapacity_) {
                noteOverflow(OverflowReason::PacketCapacityExceeded);
                ++result.rejected;
                continue;
            }

            fanInScratch_[fanInScratchCount_] = routed;
            sortIndices_[fanInScratchCount_] = fanInScratchCount_;
            ++fanInScratchCount_;
            ++result.accepted;
        }
    }

    std::sort(
        sortIndices_.begin(),
        sortIndices_.begin()
            + static_cast<std::ptrdiff_t>(fanInScratchCount_),
        [this](std::size_t firstIndex, std::size_t secondIndex) {
            const auto& first = fanInScratch_[firstIndex].emission;
            const auto& second = fanInScratch_[secondIndex].emission;
            if (first.frameOffset != second.frameOffset)
                return first.frameOffset < second.frameOffset;
            const auto firstRank = vm::emissionTieRank(first);
            const auto secondRank = vm::emissionTieRank(second);
            if (firstRank != secondRank)
                return firstRank < secondRank;
            if (first.sourceIdentity != second.sourceIdentity)
                return first.sourceIdentity < second.sourceIdentity;
            if (first.sourceOrder != second.sourceOrder)
                return first.sourceOrder < second.sourceOrder;
            return firstIndex < secondIndex;
        });

    for (std::size_t index = 0; index < fanInScratchCount_; ++index)
        packets_[index] = fanInScratch_[sortIndices_[index]];
    packetCount_ = fanInScratchCount_;
    return result;
}

std::uint64_t PreparedSignalBlock::overflowCount() const noexcept
{
    const auto observed =
        overflowGeneration_.load(std::memory_order_acquire);
    const auto acknowledged =
        acknowledgedOverflowGeneration_.load(std::memory_order_acquire);
    return observed > acknowledged ? observed - acknowledged : 0u;
}

PreparedSignalBlock::OverflowReason
PreparedSignalBlock::lastOverflowReason() const noexcept
{
    if (! rebuildRequested())
        return OverflowReason::None;
    return lastOverflowReason_.load(std::memory_order_acquire);
}

bool PreparedSignalBlock::rebuildRequested() const noexcept
{
    return overflowCount() != 0u;
}

std::uint64_t PreparedSignalBlock::capacityRebuildGeneration() const noexcept
{
    return overflowGeneration_.load(std::memory_order_acquire);
}

std::uint64_t PreparedSignalBlock::requiredWidthHighWater() const noexcept
{
    return requiredWidthHighWater_.load(std::memory_order_acquire);
}

std::size_t
PreparedSignalBlock::requiredPacketCapacityHighWater() const noexcept
{
    return requiredPacketCapacityHighWater_.load(std::memory_order_acquire);
}

void PreparedSignalBlock::acknowledgeCapacityRebuild(
    std::uint64_t observedGeneration) noexcept
{
    const auto currentGeneration =
        overflowGeneration_.load(std::memory_order_acquire);
    const auto boundedGeneration =
        std::min(observedGeneration, currentGeneration);
    auto acknowledged =
        acknowledgedOverflowGeneration_.load(std::memory_order_relaxed);
    while (acknowledged < boundedGeneration
           && ! acknowledgedOverflowGeneration_.compare_exchange_weak(
               acknowledged,
               boundedGeneration,
               std::memory_order_release,
               std::memory_order_relaxed)) {
    }
}

PreparedSignalBlock::StorageSnapshot
PreparedSignalBlock::storageSnapshot() const noexcept
{
    return {
        channelViews_.data(),
        packets_.data(),
        fanInScratch_.data(),
        sortIndices_.data(),
        channelViews_.capacity(),
        packets_.capacity(),
        fanInScratch_.capacity(),
        sortIndices_.capacity()
    };
}

bool PreparedSignalBlock::storageMatches(
    const StorageSnapshot& snapshot) const noexcept
{
    return snapshot.channelViews == channelViews_.data()
        && snapshot.packets == packets_.data()
        && snapshot.fanInScratch == fanInScratch_.data()
        && snapshot.sortIndices == sortIndices_.data()
        && snapshot.channelViewCapacity == channelViews_.capacity()
        && snapshot.packetCapacity == packets_.capacity()
        && snapshot.fanInScratchCapacity == fanInScratch_.capacity()
        && snapshot.sortIndexCapacity == sortIndices_.capacity();
}

void PreparedSignalBlock::noteOverflow(OverflowReason reason) noexcept
{
    lastOverflowReason_.store(reason, std::memory_order_relaxed);
    overflowGeneration_.fetch_add(1u, std::memory_order_release);
}

void PreparedSignalBlock::noteRequiredWidth(std::uint64_t width) noexcept
{
    auto observed = requiredWidthHighWater_.load(std::memory_order_relaxed);
    while (observed < width
           && ! requiredWidthHighWater_.compare_exchange_weak(
               observed,
               width,
               std::memory_order_release,
               std::memory_order_relaxed)) {
    }
}

void PreparedSignalBlock::noteRequiredPacketCapacity(
    std::size_t capacity) noexcept
{
    auto observed =
        requiredPacketCapacityHighWater_.load(std::memory_order_relaxed);
    while (observed < capacity
           && ! requiredPacketCapacityHighWater_.compare_exchange_weak(
               observed,
               capacity,
               std::memory_order_release,
               std::memory_order_relaxed)) {
    }
}

} // namespace curlop::transport
