#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace curlop::transport {

using SignalChannelId = std::uint64_t;
using SignalWidth = std::uint32_t;
inline constexpr std::uint32_t kSignalDescriptorSchemaVersion = 1u;

enum class SignalRate : std::uint8_t {
    Audio,
    FullRateControl,
    BlockControl,
    Event,
    Midi
};

struct SignalCapabilities {
    bool packet = false;
    bool lifecycle = false;
    bool identity = false;

    bool operator==(const SignalCapabilities& other) const noexcept;
    bool operator!=(const SignalCapabilities& other) const noexcept
    {
        return ! (*this == other);
    }
};

struct SignalChannel {
    SignalChannelId stableId = 0;
    std::string name;
    std::string role;

    bool operator==(const SignalChannel& other) const noexcept;
    bool operator!=(const SignalChannel& other) const noexcept
    {
        return ! (*this == other);
    }
};

struct SignalGroup {
    std::string id;
    std::string name;
    std::vector<SignalChannelId> channelIds;

    bool operator==(const SignalGroup& other) const noexcept;
    bool operator!=(const SignalGroup& other) const noexcept
    {
        return ! (*this == other);
    }
};

enum class HoaDimension : std::uint8_t {
    Planar2D,
    Periphonic3D
};

struct HoaMetadata {
    std::uint32_t order = 0;
    HoaDimension dimension = HoaDimension::Periphonic3D;
    std::string ordering;
    std::string normalization;

    bool operator==(const HoaMetadata& other) const noexcept;
    bool operator!=(const HoaMetadata& other) const noexcept
    {
        return ! (*this == other);
    }
};

struct SignalDescriptorDefinition {
    std::uint32_t schemaVersion = kSignalDescriptorSchemaVersion;
    std::string semanticRole;
    SignalRate rate = SignalRate::FullRateControl;
    SignalCapabilities capabilities;
    std::vector<SignalChannel> channels;
    std::optional<std::string> layout;
    std::vector<SignalGroup> groups;
    std::optional<HoaMetadata> hoa;

    bool operator==(const SignalDescriptorDefinition& other) const noexcept;
    bool operator!=(const SignalDescriptorDefinition& other) const noexcept
    {
        return ! (*this == other);
    }
};

struct SignalDescriptorDiagnostic {
    std::string code;
    std::string path;
    std::string message;
};

struct SignalDescriptorBuildResult;

class SignalDescriptor final {
public:
    SignalDescriptor(const SignalDescriptor&) = default;
    SignalDescriptor(SignalDescriptor&&) noexcept = default;
    SignalDescriptor& operator=(const SignalDescriptor&) = default;
    SignalDescriptor& operator=(SignalDescriptor&&) noexcept = default;

    static SignalDescriptorBuildResult create(
        SignalDescriptorDefinition definition);

    static SignalDescriptor legacyScalar();
    static SignalDescriptor legacyMono();
    static SignalDescriptor legacyStereo();

    std::uint32_t schemaVersion() const noexcept;
    SignalWidth width() const noexcept;
    const std::string& semanticRole() const noexcept;
    SignalRate rate() const noexcept;
    const SignalCapabilities& capabilities() const noexcept;
    const std::vector<SignalChannel>& channels() const noexcept;
    const std::optional<std::string>& layout() const noexcept;
    const std::vector<SignalGroup>& groups() const noexcept;
    const std::optional<HoaMetadata>& hoa() const noexcept;
    const SignalDescriptorDefinition& definition() const noexcept;

    bool operator==(const SignalDescriptor& other) const noexcept;
    bool operator!=(const SignalDescriptor& other) const noexcept
    {
        return ! (*this == other);
    }

private:
    explicit SignalDescriptor(SignalDescriptorDefinition definition);

    SignalDescriptorDefinition definition_;
};

struct SignalDescriptorBuildResult {
    std::optional<SignalDescriptor> value;
    std::vector<SignalDescriptorDiagnostic> diagnostics;

    bool ok() const noexcept;
    bool hasDiagnostic(const std::string& code) const noexcept;
    std::string getDiagnosticSummary() const;
};

struct SignalDescriptorTransformResult {
    std::optional<SignalDescriptor> value;
    std::vector<SignalDescriptorDiagnostic> diagnostics;

    bool ok() const noexcept;
    bool hasDiagnostic(const std::string& code) const noexcept;
    std::string getDiagnosticSummary() const;
};

struct SignalDescriptorSplitResult {
    std::vector<SignalDescriptor> values;
    std::vector<SignalDescriptorDiagnostic> diagnostics;

    bool ok() const noexcept;
    bool hasDiagnostic(const std::string& code) const noexcept;
    std::string getDiagnosticSummary() const;
};

struct ChannelMapEntry {
    std::uint32_t sourceIndex = 0;
    std::optional<std::string> name;
    std::optional<std::string> role;
    std::optional<SignalChannelId> stableId;
};

struct MergeChannelRef {
    std::uint32_t source = 0;
    std::uint32_t channel = 0;
    std::optional<std::string> name;
    std::optional<std::string> role;
    std::optional<SignalChannelId> stableId;
};

struct ExplicitMergePlan {
    std::string semanticRole;
    std::vector<MergeChannelRef> channels;
    std::optional<SignalRate> rate;
    std::optional<SignalCapabilities> capabilities;
    std::optional<std::string> layout;
    std::vector<SignalGroup> groups;
    std::optional<HoaMetadata> hoa;
};

SignalDescriptorSplitResult splitSignalDescriptor(
    const SignalDescriptor& source,
    const std::vector<std::vector<std::uint32_t>>& outputs);

SignalDescriptorTransformResult joinSignalDescriptors(
    const std::vector<SignalDescriptor>& inputs,
    std::string semanticRole);

SignalDescriptorTransformResult mapSignalDescriptor(
    const SignalDescriptor& source,
    const std::vector<ChannelMapEntry>& channels,
    std::string semanticRole);

SignalDescriptorTransformResult reorderSignalDescriptor(
    const SignalDescriptor& source,
    const std::vector<std::uint32_t>& order);

SignalDescriptorTransformResult quantizedSignalDescriptor(
    const SignalDescriptor& source,
    std::string semanticRole);

SignalDescriptorTransformResult mergeSignalDescriptors(
    const std::vector<SignalDescriptor>& inputs,
    const std::optional<ExplicitMergePlan>& plan);

} // namespace curlop::transport
