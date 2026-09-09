#include "graph/transport/PreparedSignalSchedule.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace curlop::transport {
namespace {

std::size_t preparedPacketCapacity(
    SignalWidth width,
    std::uint32_t blockSize) noexcept
{
    const auto samples = std::max<std::uint32_t>(1u, blockSize);
    if (width > std::numeric_limits<std::size_t>::max() / samples)
        return std::numeric_limits<std::size_t>::max();
    return static_cast<std::size_t>(width) * samples;
}

bool routeLess(
    const PreparedSignalRouteDefinition& first,
    const PreparedSignalRouteDefinition& second)
{
    return std::tie(
               first.sourceGraphIdx,
               first.destinationGraphIdx,
               first.sourcePort,
               first.destinationPort,
               first.sourceChannel,
               first.destinationChannel,
               first.gain,
               first.feedbackBoundary,
               first.packetCapacity)
        < std::tie(
               second.sourceGraphIdx,
               second.destinationGraphIdx,
               second.sourcePort,
               second.destinationPort,
               second.sourceChannel,
               second.destinationChannel,
               second.gain,
               second.feedbackBoundary,
               second.packetCapacity);
}

} // namespace

PreparedSignalSchedule::PreparedSignalSchedule(
    int moduleCount,
    std::uint32_t preparedBlockSize)
    : moduleCount_(std::max(0, moduleCount))
    , preparedBlockSize_(preparedBlockSize)
    , incomingRoutes_(static_cast<std::size_t>(moduleCount_))
    , outgoingRoutes_(static_cast<std::size_t>(moduleCount_))
    , deferredCycleModules_(static_cast<std::size_t>(moduleCount_), 0u)
    , deferredExecutionModules_(
          static_cast<std::size_t>(moduleCount_), 0u)
{
}

std::unique_ptr<PreparedSignalSchedule> PreparedSignalSchedule::build(
    int moduleCount,
    std::vector<PreparedSignalRouteDefinition> definitions,
    std::uint32_t preparedBlockSize,
    std::vector<PreparedSignalDependencyDefinition>
        schedulingOnlyDependencies)
{
    auto schedule = std::unique_ptr<PreparedSignalSchedule>(
        new PreparedSignalSchedule(moduleCount, preparedBlockSize));
    std::stable_sort(definitions.begin(), definitions.end(), routeLess);

    std::vector<std::vector<int>> adjacency(
        static_cast<std::size_t>(schedule->moduleCount_));
    for (const auto& definition : definitions) {
        if (definition.feedbackBoundary)
            continue;
        if (definition.sourceGraphIdx < 0
            || definition.sourceGraphIdx >= schedule->moduleCount_
            || definition.destinationGraphIdx < 0
            || definition.destinationGraphIdx >= schedule->moduleCount_)
            continue;
        adjacency[static_cast<std::size_t>(definition.sourceGraphIdx)]
            .push_back(definition.destinationGraphIdx);
    }
    for (const auto& dependency : schedulingOnlyDependencies) {
        if (dependency.sourceGraphIdx < 0
            || dependency.sourceGraphIdx >= schedule->moduleCount_
            || dependency.destinationGraphIdx < 0
            || dependency.destinationGraphIdx >= schedule->moduleCount_)
            continue;
        adjacency[static_cast<std::size_t>(dependency.sourceGraphIdx)]
            .push_back(dependency.destinationGraphIdx);
    }
    for (auto& destinations : adjacency) {
        std::sort(destinations.begin(), destinations.end());
        destinations.erase(
            std::unique(destinations.begin(), destinations.end()),
            destinations.end());
    }

    std::vector<int> visitIndex(
        static_cast<std::size_t>(schedule->moduleCount_), -1);
    std::vector<int> lowLink(
        static_cast<std::size_t>(schedule->moduleCount_), -1);
    std::vector<int> stack;
    std::vector<std::uint8_t> onStack(
        static_cast<std::size_t>(schedule->moduleCount_), 0u);
    std::vector<int> componentOf(
        static_cast<std::size_t>(schedule->moduleCount_), -1);
    std::vector<int> componentSizes;
    int nextVisitIndex = 0;

    std::function<void(int)> visit = [&](int node) {
        const auto nodeIndex = static_cast<std::size_t>(node);
        visitIndex[nodeIndex] = nextVisitIndex;
        lowLink[nodeIndex] = nextVisitIndex;
        ++nextVisitIndex;
        stack.push_back(node);
        onStack[nodeIndex] = 1u;

        for (const auto destination : adjacency[nodeIndex]) {
            const auto destinationIndex =
                static_cast<std::size_t>(destination);
            if (visitIndex[destinationIndex] < 0) {
                visit(destination);
                lowLink[nodeIndex] =
                    std::min(lowLink[nodeIndex], lowLink[destinationIndex]);
            } else if (onStack[destinationIndex] != 0u) {
                lowLink[nodeIndex] =
                    std::min(lowLink[nodeIndex], visitIndex[destinationIndex]);
            }
        }

        if (lowLink[nodeIndex] != visitIndex[nodeIndex])
            return;

        const int component = static_cast<int>(componentSizes.size());
        int componentSize = 0;
        while (! stack.empty()) {
            const int member = stack.back();
            stack.pop_back();
            onStack[static_cast<std::size_t>(member)] = 0u;
            componentOf[static_cast<std::size_t>(member)] = component;
            ++componentSize;
            if (member == node)
                break;
        }
        componentSizes.push_back(componentSize);
    };

    for (int module = 0; module < schedule->moduleCount_; ++module)
        if (visitIndex[static_cast<std::size_t>(module)] < 0)
            visit(module);

    std::vector<std::uint8_t> cyclicComponents(
        componentSizes.size(), 0u);
    for (std::size_t component = 0; component < componentSizes.size();
         ++component)
        if (componentSizes[component] > 1)
            cyclicComponents[component] = 1u;
    for (int source = 0; source < schedule->moduleCount_; ++source)
        for (const auto destination :
             adjacency[static_cast<std::size_t>(source)])
            if (source == destination)
                cyclicComponents[static_cast<std::size_t>(
                    componentOf[static_cast<std::size_t>(source)])] = 1u;
    for (int module = 0; module < schedule->moduleCount_; ++module) {
        const auto component =
            componentOf[static_cast<std::size_t>(module)];
        if (component >= 0
            && cyclicComponents[static_cast<std::size_t>(component)] != 0u)
            schedule->deferredCycleModules_[
                static_cast<std::size_t>(module)] = 1u;
    }

    // A module downstream from an unresolved cycle cannot be placed in the
    // prepared execution order without observing an incomplete input. Carry
    // the deferral forward through the directed graph; upstream modules remain
    // independently executable, while their edge into the cycle is deferred.
    std::vector<int> deferredQueue;
    for (int module = 0; module < schedule->moduleCount_; ++module) {
        if (schedule->deferredCycleModules_[
                static_cast<std::size_t>(module)] == 0u)
            continue;
        schedule->deferredExecutionModules_[
            static_cast<std::size_t>(module)] = 1u;
        deferredQueue.push_back(module);
    }
    for (std::size_t cursor = 0; cursor < deferredQueue.size(); ++cursor) {
        const int module = deferredQueue[cursor];
        for (const auto destination :
             adjacency[static_cast<std::size_t>(module)]) {
            auto& deferred = schedule->deferredExecutionModules_[
                static_cast<std::size_t>(destination)];
            if (deferred != 0u)
                continue;
            deferred = 1u;
            deferredQueue.push_back(destination);
        }
    }

    schedule->routes_.reserve(definitions.size());
    schedule->routeBlocks_.reserve(definitions.size());
    schedule->routeBlockOwnerIndices_.reserve(definitions.size());
    for (auto& definition : definitions) {
        RouteRecord record;
        record.sourceGraphIdx = definition.sourceGraphIdx;
        record.destinationGraphIdx = definition.destinationGraphIdx;
        record.sourcePort = std::move(definition.sourcePort);
        record.destinationPort = std::move(definition.destinationPort);
        record.sourceChannel = definition.sourceChannel;
        record.destinationChannel = definition.destinationChannel;
        record.gain = definition.gain;
        record.feedbackBoundary = definition.feedbackBoundary;
        record.packetCapacity = definition.packetCapacity != 0u
            ? definition.packetCapacity
            : preparedPacketCapacity(
                definition.descriptor.width(), preparedBlockSize);
        record.descriptor = std::move(definition.descriptor);

        const bool validSource =
            record.sourceGraphIdx >= 0
            && record.sourceGraphIdx < schedule->moduleCount_;
        const bool validDestination =
            record.destinationGraphIdx >= 0
            && record.destinationGraphIdx < schedule->moduleCount_;
        if (! validSource || ! validDestination) {
            record.disposition = RouteDisposition::Unsupported;
            record.diagnostic = "signal route module index is outside prepared graph";
            ++schedule->unsupportedRouteCount_;
            schedule->routes_.push_back(std::move(record));
            schedule->routeBlocks_.push_back(nullptr);
            schedule->routeBlockOwnerIndices_.push_back(
                schedule->routeBlocks_.size() - 1u);
            continue;
        }

        if (record.feedbackBoundary) {
            record.disposition = RouteDisposition::ExecutorOwnedFeedback;
            record.diagnostic =
                "declared feedback boundary is owned by the local SCC executor";
        } else {
            const bool sourceCyclic =
                schedule->deferredCycleModules_[
                    static_cast<std::size_t>(record.sourceGraphIdx)] != 0u;
            const bool destinationCyclic =
                schedule->deferredCycleModules_[
                    static_cast<std::size_t>(record.destinationGraphIdx)] != 0u;
            const bool sameComponent =
                componentOf[static_cast<std::size_t>(record.sourceGraphIdx)]
                == componentOf[
                    static_cast<std::size_t>(record.destinationGraphIdx)];
            if (sourceCyclic && destinationCyclic && sameComponent) {
                record.disposition = RouteDisposition::DeferredCycle;
                record.diagnostic =
                    "cycle-forming route is deferred to exact feedback scheduling";
                ++schedule->deferredRouteCount_;
            } else if (
                schedule->deferredExecutionModules_[
                    static_cast<std::size_t>(record.sourceGraphIdx)] != 0u
                || schedule->deferredExecutionModules_[
                    static_cast<std::size_t>(record.destinationGraphIdx)] != 0u) {
                record.disposition = RouteDisposition::DeferredCycleDependency;
                record.diagnostic =
                    "route depends on a cycle deferred to exact feedback scheduling";
                ++schedule->deferredRouteCount_;
            } else {
                record.disposition = RouteDisposition::Prepared;
            }
        }

        const auto routeIndex = schedule->routes_.size();
        schedule->outgoingRoutes_[
            static_cast<std::size_t>(record.sourceGraphIdx)]
            .push_back(routeIndex);
        schedule->incomingRoutes_[
            static_cast<std::size_t>(record.destinationGraphIdx)]
            .push_back(routeIndex);
        std::size_t storageOwner = routeIndex;
        if (record.disposition == RouteDisposition::Prepared
            && record.descriptor.capabilities().packet) {
            for (std::size_t candidate = 0;
                 candidate < schedule->routes_.size();
                 ++candidate) {
                const auto& prior = schedule->routes_[candidate];
                if (prior.disposition == RouteDisposition::Prepared
                    && prior.descriptor.capabilities().packet
                    && prior.sourceGraphIdx == record.sourceGraphIdx
                    && prior.sourcePort == record.sourcePort
                    && prior.sourceChannel == record.sourceChannel
                    && prior.packetCapacity == record.packetCapacity
                    && prior.descriptor == record.descriptor) {
                    storageOwner =
                        schedule->routeBlockOwnerIndices_[candidate];
                    break;
                }
            }
        }
        if (record.disposition != RouteDisposition::Prepared)
            schedule->routeBlocks_.push_back(nullptr);
        else if (storageOwner == routeIndex)
            schedule->routeBlocks_.push_back(
                std::make_unique<PreparedSignalBlock>(
                    record.descriptor,
                    record.descriptor.width(),
                    record.packetCapacity,
                    false));
        else
            schedule->routeBlocks_.push_back(nullptr);
        schedule->routeBlockOwnerIndices_.push_back(storageOwner);
        schedule->routes_.push_back(std::move(record));
    }

    std::set<std::pair<int, int>> dependencyPairs;
    for (const auto& route : schedule->routes_)
        if (route.disposition == RouteDisposition::Prepared)
            dependencyPairs.insert({
                route.sourceGraphIdx, route.destinationGraphIdx
            });
    for (const auto& dependency : schedulingOnlyDependencies) {
        if (dependency.sourceGraphIdx < 0
            || dependency.sourceGraphIdx >= schedule->moduleCount_
            || dependency.destinationGraphIdx < 0
            || dependency.destinationGraphIdx >= schedule->moduleCount_)
            continue;
        if (schedule->deferredExecutionModules_[
                static_cast<std::size_t>(
                    dependency.sourceGraphIdx)] != 0u
            || schedule->deferredExecutionModules_[
                static_cast<std::size_t>(
                    dependency.destinationGraphIdx)] != 0u)
            continue;
        dependencyPairs.insert({
            dependency.sourceGraphIdx,
            dependency.destinationGraphIdx
        });
    }
    schedule->dependencies_.reserve(dependencyPairs.size());
    for (const auto& pair : dependencyPairs)
        schedule->dependencies_.push_back({ pair.first, pair.second });

    std::vector<int> indegree(
        static_cast<std::size_t>(schedule->moduleCount_), 0);
    std::vector<std::vector<int>> preparedAdjacency(
        static_cast<std::size_t>(schedule->moduleCount_));
    for (const auto& dependency : schedule->dependencies_) {
        preparedAdjacency[
            static_cast<std::size_t>(dependency.sourceGraphIdx)]
            .push_back(dependency.destinationGraphIdx);
        ++indegree[static_cast<std::size_t>(dependency.destinationGraphIdx)];
    }

    std::set<int> ready;
    for (int module = 0; module < schedule->moduleCount_; ++module)
        if (schedule->deferredExecutionModules_[
                static_cast<std::size_t>(module)] == 0u
            && indegree[static_cast<std::size_t>(module)] == 0)
            ready.insert(module);
    while (! ready.empty()) {
        const int module = *ready.begin();
        ready.erase(ready.begin());
        schedule->preparedModuleOrder_.push_back(module);
        for (const auto destination :
             preparedAdjacency[static_cast<std::size_t>(module)]) {
            auto& degree = indegree[static_cast<std::size_t>(destination)];
            --degree;
            if (degree == 0)
                ready.insert(destination);
        }
    }

    return schedule;
}

int PreparedSignalSchedule::moduleCount() const noexcept
{
    return moduleCount_;
}

std::uint32_t PreparedSignalSchedule::preparedBlockSize() const noexcept
{
    return preparedBlockSize_;
}

std::size_t PreparedSignalSchedule::routeCount() const noexcept
{
    return routes_.size();
}

const PreparedSignalSchedule::RouteRecord&
PreparedSignalSchedule::route(std::size_t index) const noexcept
{
    return routes_[index];
}

PreparedSignalBlock*
PreparedSignalSchedule::blockForRoute(std::size_t index) noexcept
{
    if (index >= routeBlockOwnerIndices_.size())
        return nullptr;
    const auto owner = routeBlockOwnerIndices_[index];
    return owner < routeBlocks_.size()
        ? routeBlocks_[owner].get() : nullptr;
}

const PreparedSignalBlock*
PreparedSignalSchedule::blockForRoute(std::size_t index) const noexcept
{
    if (index >= routeBlockOwnerIndices_.size())
        return nullptr;
    const auto owner = routeBlockOwnerIndices_[index];
    return owner < routeBlocks_.size()
        ? routeBlocks_[owner].get() : nullptr;
}

std::size_t
PreparedSignalSchedule::packetStorageBlockCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        routeBlocks_.begin(), routeBlocks_.end(),
        [] (const auto& block) { return block != nullptr; }));
}

std::size_t
PreparedSignalSchedule::packetStorageCapacity() const noexcept
{
    std::size_t capacity = 0u;
    for (const auto& block : routeBlocks_)
        if (block != nullptr) {
            if (block->packetCapacity()
                > std::numeric_limits<std::size_t>::max() - capacity)
                return std::numeric_limits<std::size_t>::max();
            capacity += block->packetCapacity();
        }
    return capacity;
}

std::size_t PreparedSignalSchedule::dependencyCount() const noexcept
{
    return dependencies_.size();
}

const PreparedSignalSchedule::Dependency&
PreparedSignalSchedule::dependency(std::size_t index) const noexcept
{
    return dependencies_[index];
}

const std::vector<int>&
PreparedSignalSchedule::preparedModuleOrder() const noexcept
{
    return preparedModuleOrder_;
}

const std::vector<std::size_t>&
PreparedSignalSchedule::incomingRoutes(int graphIdx) const noexcept
{
    static const std::vector<std::size_t> empty;
    return graphIdx >= 0 && graphIdx < moduleCount_
        ? incomingRoutes_[static_cast<std::size_t>(graphIdx)] : empty;
}

const std::vector<std::size_t>&
PreparedSignalSchedule::outgoingRoutes(int graphIdx) const noexcept
{
    static const std::vector<std::size_t> empty;
    return graphIdx >= 0 && graphIdx < moduleCount_
        ? outgoingRoutes_[static_cast<std::size_t>(graphIdx)] : empty;
}

void PreparedSignalSchedule::publishIncomingChannelViews(
    int destinationGraphIdx,
    void* providerContext,
    InputRowProvider provider,
    int numSamples) noexcept
{
    if (provider == nullptr || numSamples < 0)
        return;

    for (const auto routeIndex : incomingRoutes(destinationGraphIdx)) {
        if (routeIndex >= routes_.size()
            || routes_[routeIndex].disposition
                != RouteDisposition::Prepared)
            continue;
        auto* block = blockForRoute(routeIndex);
        if (block == nullptr
            || block->blockSamples()
                != static_cast<std::uint32_t>(numSamples))
            continue;
        const auto& routeRecord = routes_[routeIndex];
        for (SignalWidth channel = 0;
             channel < block->activeWidth();
             ++channel) {
            const int destinationChannel =
                routeRecord.destinationChannel
                + static_cast<int>(channel);
            const auto* row = destinationChannel >= 0
                ? provider(
                    providerContext,
                    destinationChannel,
                    numSamples)
                : nullptr;
            block->setChannelView(channel, row, nullptr);
        }
    }
}

void PreparedSignalSchedule::copyIncomingChannelViews(
    int destinationGraphIdx,
    const float** destinationRows,
    int destinationRowCount,
    int numSamples) const noexcept
{
    if (destinationRows == nullptr || destinationRowCount <= 0)
        return;
    for (int row = 0; row < destinationRowCount; ++row)
        destinationRows[row] = nullptr;
    if (numSamples < 0)
        return;

    for (const auto routeIndex : incomingRoutes(destinationGraphIdx)) {
        if (routeIndex >= routes_.size()
            || routes_[routeIndex].disposition
                != RouteDisposition::Prepared)
            continue;
        const auto* block = blockForRoute(routeIndex);
        if (block == nullptr
            || block->blockSamples()
                != static_cast<std::uint32_t>(numSamples))
            continue;
        const auto& routeRecord = routes_[routeIndex];
        for (SignalWidth channel = 0;
             channel < block->activeWidth();
             ++channel) {
            const int destinationChannel =
                routeRecord.destinationChannel
                + static_cast<int>(channel);
            if (destinationChannel < 0
                || destinationChannel >= destinationRowCount
                || destinationRows[destinationChannel] != nullptr)
                continue;
            const auto& view = block->channelView(channel);
            if (view.samples
                    == static_cast<std::uint32_t>(numSamples)
                && view.read != nullptr)
                destinationRows[destinationChannel] = view.read;
        }
    }
}

bool PreparedSignalSchedule::publishPacket(
    std::size_t routeIndex,
    std::uint32_t channelIndex,
    const vm::Emission& emission) noexcept
{
    if (routeIndex >= routes_.size()
        || routes_[routeIndex].disposition != RouteDisposition::Prepared
        || ! routes_[routeIndex].descriptor.capabilities().packet)
        return false;
    if (routeIndex >= routeBlockOwnerIndices_.size())
        return false;
    // One exact source port owns publication storage; later fanout routes
    // acknowledge the same publication without duplicating the packet.
    if (routeBlockOwnerIndices_[routeIndex] != routeIndex)
        return true;
    auto* block = blockForRoute(routeIndex);
    if (block == nullptr)
        return false;
    auto addressed = emission;
    addressed.channelIndex = channelIndex;
    const auto& channels =
        routes_[routeIndex].descriptor.channels();
    if (channelIndex >= channels.size())
        return false;
    const auto descriptorStableId =
        channels[channelIndex].stableId;
    if (addressed.stableChannelId == 0u) {
        addressed.stableChannelId =
            descriptorStableId;
    } else if (addressed.stableChannelId
               != descriptorStableId)
        return false;
    return block->route(channelIndex, addressed);
}

PreparedSignalSchedule::PacketAppendResult
PreparedSignalSchedule::appendIncomingPackets(
    int destinationGraphIdx,
    const PacketTargetBinding* bindings,
    std::size_t bindingCount,
    vm::Emission* destination,
    std::size_t destinationSize,
    std::size_t destinationCapacity) const noexcept
{
    PacketAppendResult result;
    if (bindings == nullptr || destination == nullptr
        || destinationSize > destinationCapacity)
        return result;

    for (std::size_t bindingIndex = 0;
         bindingIndex < bindingCount;
         ++bindingIndex) {
        const auto& binding = bindings[bindingIndex];
        if (binding.routeIndex >= routes_.size())
            continue;
        const auto& routeRecord = routes_[binding.routeIndex];
        if (routeRecord.disposition != RouteDisposition::Prepared
            || ! routeRecord.descriptor.capabilities().packet
            || routeRecord.destinationGraphIdx != destinationGraphIdx)
            continue;
        const auto* block = blockForRoute(binding.routeIndex);
        if (block == nullptr)
            continue;
        for (std::size_t packetIndex = 0;
             packetIndex < block->packetCount();
             ++packetIndex) {
            ++result.requiredCapacity;
            if (destinationSize + result.accepted
                >= destinationCapacity) {
                ++result.rejected;
                continue;
            }
            const auto& routed = block->packet(packetIndex);
            auto emission = routed.emission;
            emission.channelIndex = routed.channelIndex;
            if (emission.stableChannelId == 0u) {
                const auto& channels =
                    routeRecord.descriptor.channels();
                if (routed.channelIndex >= channels.size()) {
                    ++result.rejected;
                    continue;
                }
                emission.stableChannelId =
                    channels[routed.channelIndex].stableId;
            }
            emission.value *= routeRecord.gain;
            emission.lane =
                vm::namedOutputLane(binding.destinationLane);
            destination[destinationSize + result.accepted] = emission;
            ++result.accepted;
        }
    }
    return result;
}

bool PreparedSignalSchedule::hasDeferredCycles() const noexcept
{
    return deferredRouteCount_ != 0u;
}

bool PreparedSignalSchedule::moduleInDeferredCycle(int graphIdx) const noexcept
{
    return graphIdx >= 0 && graphIdx < moduleCount_
        && deferredCycleModules_[static_cast<std::size_t>(graphIdx)] != 0u;
}

bool PreparedSignalSchedule::moduleExecutionDeferredByCycle(
    int graphIdx) const noexcept
{
    return graphIdx >= 0 && graphIdx < moduleCount_
        && deferredExecutionModules_[static_cast<std::size_t>(graphIdx)] != 0u;
}

std::size_t PreparedSignalSchedule::deferredRouteCount() const noexcept
{
    return deferredRouteCount_;
}

std::size_t PreparedSignalSchedule::unsupportedRouteCount() const noexcept
{
    return unsupportedRouteCount_;
}

} // namespace curlop::transport
