#pragma once

#include "graph/transport/PreparedSignalBlock.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace curlop::transport {

struct PreparedSignalRouteDefinition {
    int sourceGraphIdx = -1;
    int destinationGraphIdx = -1;
    std::string sourcePort;
    std::string destinationPort;
    int sourceChannel = -1;
    int destinationChannel = -1;
    float gain = 1.0f;
    // A declared z^-1 edge is owned by the local feedback executor, never by
    // the block-oriented prepared signal schedule.
    bool feedbackBoundary = false;
    // Zero derives the standalone/test capacity from width × block size.
    // Production graph preparation supplies the source VM's complete prepared
    // emission budget so polyphony is resource-derived rather than capped by
    // the audio block length.
    std::size_t packetCapacity = 0;
    SignalDescriptor descriptor = SignalDescriptor::legacyScalar();
};

// An execution-order edge with no render transport. The compiler/install
// path emits these only when a target VM contains CTRL_INPUT and therefore
// must observe its owning ScriptNode's current-block input rows first.
struct PreparedSignalDependencyDefinition {
    int sourceGraphIdx = -1;
    int destinationGraphIdx = -1;
};

class PreparedSignalSchedule final {
public:
    using InputRowProvider =
        const float* (*)(
            void* context,
            int channel,
            int numSamples) noexcept;

    enum class RouteDisposition : std::uint8_t {
        Prepared,
        ExecutorOwnedFeedback,
        DeferredCycle,
        DeferredCycleDependency,
        Unsupported
    };

    struct RouteRecord {
        int sourceGraphIdx = -1;
        int destinationGraphIdx = -1;
        std::string sourcePort;
        std::string destinationPort;
        int sourceChannel = -1;
        int destinationChannel = -1;
        float gain = 1.0f;
        bool feedbackBoundary = false;
        std::size_t packetCapacity = 0;
        SignalDescriptor descriptor = SignalDescriptor::legacyScalar();
        RouteDisposition disposition = RouteDisposition::Unsupported;
        std::string diagnostic;
    };

    struct PacketTargetBinding {
        std::size_t routeIndex = 0;
        std::uint32_t destinationLane = 0;
    };

    struct PacketAppendResult {
        std::size_t accepted = 0;
        std::size_t rejected = 0;
        std::size_t requiredCapacity = 0;
    };

    struct Dependency {
        int sourceGraphIdx = -1;
        int destinationGraphIdx = -1;
    };

    static std::unique_ptr<PreparedSignalSchedule> build(
        int moduleCount,
        std::vector<PreparedSignalRouteDefinition> routes,
        std::uint32_t preparedBlockSize,
        std::vector<PreparedSignalDependencyDefinition>
            schedulingOnlyDependencies = {});

    PreparedSignalSchedule(const PreparedSignalSchedule&) = delete;
    PreparedSignalSchedule& operator=(const PreparedSignalSchedule&) = delete;
    PreparedSignalSchedule(PreparedSignalSchedule&&) = delete;
    PreparedSignalSchedule& operator=(PreparedSignalSchedule&&) = delete;

    int moduleCount() const noexcept;
    std::uint32_t preparedBlockSize() const noexcept;

    std::size_t routeCount() const noexcept;
    const RouteRecord& route(std::size_t index) const noexcept;
    PreparedSignalBlock* blockForRoute(std::size_t index) noexcept;
    const PreparedSignalBlock* blockForRoute(std::size_t index) const noexcept;
    std::size_t packetStorageBlockCount() const noexcept;
    std::size_t packetStorageCapacity() const noexcept;

    std::size_t dependencyCount() const noexcept;
    const Dependency& dependency(std::size_t index) const noexcept;
    const std::vector<int>& preparedModuleOrder() const noexcept;
    const std::vector<std::size_t>& incomingRoutes(int graphIdx) const noexcept;
    const std::vector<std::size_t>& outgoingRoutes(int graphIdx) const noexcept;
    void publishIncomingChannelViews(
        int destinationGraphIdx,
        void* providerContext,
        InputRowProvider provider,
        int numSamples) noexcept;
    void copyIncomingChannelViews(
        int destinationGraphIdx,
        const float** destinationRows,
        int destinationRowCount,
        int numSamples) const noexcept;
    bool publishPacket(
        std::size_t routeIndex,
        std::uint32_t channelIndex,
        const vm::Emission& emission) noexcept;
    PacketAppendResult appendIncomingPackets(
        int destinationGraphIdx,
        const PacketTargetBinding* bindings,
        std::size_t bindingCount,
        vm::Emission* destination,
        std::size_t destinationSize,
        std::size_t destinationCapacity) const noexcept;

    bool hasDeferredCycles() const noexcept;
    bool moduleInDeferredCycle(int graphIdx) const noexcept;
    bool moduleExecutionDeferredByCycle(int graphIdx) const noexcept;
    std::size_t deferredRouteCount() const noexcept;
    std::size_t unsupportedRouteCount() const noexcept;

private:
    PreparedSignalSchedule(int moduleCount, std::uint32_t preparedBlockSize);

    int moduleCount_ = 0;
    std::uint32_t preparedBlockSize_ = 0;
    std::vector<RouteRecord> routes_;
    std::vector<std::unique_ptr<PreparedSignalBlock>> routeBlocks_;
    // Packet fanout routes from one exact source port share one immutable
    // publication block. Entries point at the owning routeBlocks_ slot.
    std::vector<std::size_t> routeBlockOwnerIndices_;
    std::vector<Dependency> dependencies_;
    std::vector<int> preparedModuleOrder_;
    std::vector<std::vector<std::size_t>> incomingRoutes_;
    std::vector<std::vector<std::size_t>> outgoingRoutes_;
    std::vector<std::uint8_t> deferredCycleModules_;
    std::vector<std::uint8_t> deferredExecutionModules_;
    std::size_t deferredRouteCount_ = 0;
    std::size_t unsupportedRouteCount_ = 0;
};

} // namespace curlop::transport
