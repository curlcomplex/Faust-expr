#include "graph/transport/SignalDescriptor.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace curlop::transport {
namespace {

using Diagnostics = std::vector<SignalDescriptorDiagnostic>;

void addDiagnostic(
    Diagnostics& diagnostics,
    std::string code,
    std::string path,
    std::string message)
{
    diagnostics.push_back({
        std::move(code), std::move(path), std::move(message)
    });
}

std::string buildDiagnosticSummary(const Diagnostics& diagnostics)
{
    std::ostringstream summary;
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        if (index != 0)
            summary << "; ";
        const auto& diagnostic = diagnostics[index];
        summary << diagnostic.code;
        if (! diagnostic.path.empty())
            summary << " at " << diagnostic.path;
        if (! diagnostic.message.empty())
            summary << ": " << diagnostic.message;
    }
    return summary.str();
}

bool hasDiagnosticCode(
    const Diagnostics& diagnostics,
    const std::string& code) noexcept
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [&code](const SignalDescriptorDiagnostic& diagnostic) {
            return diagnostic.code == code;
        });
}

class OutputIdentityAllocator final {
public:
    explicit OutputIdentityAllocator(SignalChannelId largestSourceId)
        : nextFreshId_(
            largestSourceId
                    == std::numeric_limits<SignalChannelId>::max()
                ? SignalChannelId{1}
                : largestSourceId + 1u)
    {
    }

    SignalChannelId claim(SignalChannelId preferred)
    {
        if (preferred != 0u && used_.insert(preferred).second)
            return preferred;

        while (nextFreshId_ == 0u
               || used_.find(nextFreshId_) != used_.end()) {
            advance();
        }
        const auto allocated = nextFreshId_;
        used_.insert(allocated);
        advance();
        return allocated;
    }

    std::optional<SignalChannelId> claimExact(SignalChannelId requested)
    {
        if (requested == 0u || ! used_.insert(requested).second)
            return std::nullopt;
        return requested;
    }

private:
    void advance()
    {
        nextFreshId_ =
            nextFreshId_ == std::numeric_limits<SignalChannelId>::max()
            ? SignalChannelId{1}
            : nextFreshId_ + 1u;
    }

    SignalChannelId nextFreshId_;
    std::unordered_set<SignalChannelId> used_;
};

SignalChannelId largestChannelId(
    const std::vector<SignalDescriptor>& descriptors)
{
    SignalChannelId largest = 0u;
    for (const auto& descriptor : descriptors)
        for (const auto& channel : descriptor.channels())
            largest = std::max(largest, channel.stableId);
    return largest;
}

std::optional<std::uint64_t> computeExpectedHoaWidth(
    const HoaMetadata& hoa) noexcept
{
    const auto order = static_cast<std::uint64_t>(hoa.order);
    if (hoa.dimension == HoaDimension::Planar2D)
        return order * 2u + 1u;
    if (hoa.dimension != HoaDimension::Periphonic3D)
        return std::nullopt;

    constexpr auto maxFactor = std::uint64_t{65535};
    const auto factor = order + 1u;
    if (factor > maxFactor)
        return std::numeric_limits<std::uint64_t>::max();
    return factor * factor;
}

Diagnostics validateDefinition(const SignalDescriptorDefinition& definition)
{
    Diagnostics diagnostics;

    if (definition.schemaVersion != kSignalDescriptorSchemaVersion) {
        addDiagnostic(
            diagnostics,
            "unsupported_schema_version",
            "schemaVersion",
            "Signal descriptor schema version is not supported");
    }

    if (definition.semanticRole.empty()) {
        addDiagnostic(
            diagnostics,
            "empty_semantic_role",
            "semanticRole",
            "A signal semantic role must be descriptive and non-empty");
    }

    switch (definition.rate) {
        case SignalRate::Audio:
        case SignalRate::FullRateControl:
        case SignalRate::BlockControl:
        case SignalRate::Event:
        case SignalRate::Midi:
            break;
        default:
            addDiagnostic(
                diagnostics,
                "invalid_signal_rate",
                "rate",
                "Signal rate is outside the versioned descriptor vocabulary");
            break;
    }

    if (definition.channels.empty()) {
        addDiagnostic(
            diagnostics,
            "empty_signal",
            "channels",
            "A signal must contain at least one ordered channel");
    }

    if (definition.channels.size()
        > static_cast<std::size_t>(
            std::numeric_limits<SignalWidth>::max())) {
        addDiagnostic(
            diagnostics,
            "signal_width_overflow",
            "channels",
            "Signal width exceeds the public uint32 transport contract");
    }

    std::unordered_set<SignalChannelId> channelIds;
    channelIds.reserve(definition.channels.size());

    for (std::size_t index = 0; index < definition.channels.size(); ++index) {
        const auto& channel = definition.channels[index];
        const auto path = "channels[" + std::to_string(index) + "]";

        if (channel.stableId == 0) {
            addDiagnostic(
                diagnostics,
                "zero_channel_id",
                path + ".stableId",
                "Channel identity zero is reserved as invalid");
        } else if (! channelIds.insert(channel.stableId).second) {
            addDiagnostic(
                diagnostics,
                "duplicate_channel_id",
                path + ".stableId",
                "Channel stable IDs must be unique within a signal");
        }

        if (channel.name.empty()) {
            addDiagnostic(
                diagnostics,
                "empty_channel_name",
                path + ".name",
                "Every ordered channel requires a stable display name");
        }

        if (channel.role.empty()) {
            addDiagnostic(
                diagnostics,
                "empty_channel_role",
                path + ".role",
                "Every channel requires a descriptive role");
        }
    }

    if (definition.layout.has_value() && definition.layout->empty()) {
        addDiagnostic(
            diagnostics,
            "empty_layout",
            "layout",
            "An optional layout must be absent or non-empty");
    }

    std::unordered_set<std::string> groupIds;
    groupIds.reserve(definition.groups.size());
    for (std::size_t index = 0; index < definition.groups.size(); ++index) {
        const auto& group = definition.groups[index];
        const auto path = "groups[" + std::to_string(index) + "]";

        if (group.id.empty()) {
            addDiagnostic(
                diagnostics,
                "empty_group_id",
                path + ".id",
                "Every group requires a stable ID");
        } else if (! groupIds.insert(group.id).second) {
            addDiagnostic(
                diagnostics,
                "duplicate_group_id",
                path + ".id",
                "Group IDs must be unique within a signal");
        }

        if (group.name.empty()) {
            addDiagnostic(
                diagnostics,
                "empty_group_name",
                path + ".name",
                "Every group requires a display name");
        }

        if (group.channelIds.empty()) {
            addDiagnostic(
                diagnostics,
                "empty_group",
                path + ".channelIds",
                "A present group must contain at least one channel");
        }

        std::unordered_set<SignalChannelId> groupMembers;
        groupMembers.reserve(group.channelIds.size());
        for (std::size_t member = 0;
             member < group.channelIds.size();
             ++member) {
            const auto channelId = group.channelIds[member];
            const auto memberPath = path + ".channelIds["
                + std::to_string(member) + "]";
            if (! groupMembers.insert(channelId).second) {
                addDiagnostic(
                    diagnostics,
                    "duplicate_group_channel",
                    memberPath,
                    "A channel may appear only once within one group");
            }
            if (channelIds.find(channelId) == channelIds.end()) {
                addDiagnostic(
                    diagnostics,
                    "unknown_group_channel",
                    memberPath,
                    "Group member does not identify an ordered channel");
            }
        }
    }

    if (definition.capabilities.lifecycle
        && ! definition.capabilities.identity) {
        addDiagnostic(
            diagnostics,
            "lifecycle_requires_identity",
            "capabilities",
            "Lifecycle transitions require packet/voice identity");
    }

    if ((definition.rate == SignalRate::Event
         || definition.rate == SignalRate::Midi)
        && ! definition.capabilities.packet) {
        addDiagnostic(
            diagnostics,
            "packet_rate_requires_packets",
            "capabilities.packet",
            "Event and MIDI rates require packet capability");
    }

    if (definition.hoa.has_value()) {
        const auto& hoa = *definition.hoa;
        if (hoa.ordering.empty()) {
            addDiagnostic(
                diagnostics,
                "empty_hoa_ordering",
                "hoa.ordering",
                "HOA metadata requires an explicit channel ordering");
        }
        if (hoa.normalization.empty()) {
            addDiagnostic(
                diagnostics,
                "empty_hoa_normalization",
                "hoa.normalization",
                "HOA metadata requires an explicit normalization");
        }

        const auto expectedWidth = computeExpectedHoaWidth(hoa);
        if (! expectedWidth.has_value()) {
            addDiagnostic(
                diagnostics,
                "invalid_hoa_dimension",
                "hoa.dimension",
                "HOA dimension is outside the versioned descriptor vocabulary");
        } else if (*expectedWidth
            > static_cast<std::uint64_t>(
                std::numeric_limits<SignalWidth>::max())) {
            addDiagnostic(
                diagnostics,
                "hoa_width_overflow",
                "hoa.order",
                "HOA order exceeds the uint32 transport width");
        } else if (*expectedWidth != definition.channels.size()) {
            addDiagnostic(
                diagnostics,
                "hoa_width_mismatch",
                "hoa",
                "HOA order and dimension do not match channel width");
        }
    }

    return diagnostics;
}

std::vector<SignalGroup> buildGroupsForChannels(
    const std::vector<SignalGroup>& groups,
    const std::vector<SignalChannel>& channels)
{
    std::unordered_set<SignalChannelId> selected;
    selected.reserve(channels.size());
    for (const auto& channel : channels)
        selected.insert(channel.stableId);

    std::vector<SignalGroup> filtered;
    for (const auto& group : groups) {
        SignalGroup result{ group.id, group.name, {} };
        for (const auto channelId : group.channelIds) {
            if (selected.find(channelId) != selected.end())
                result.channelIds.push_back(channelId);
        }
        if (! result.channelIds.empty())
            filtered.push_back(std::move(result));
    }
    return filtered;
}

SignalDescriptorTransformResult buildTransformResult(
    SignalDescriptorBuildResult result)
{
    return {
        std::move(result.value),
        std::move(result.diagnostics)
    };
}

SignalDescriptorTransformResult buildTransformFailure(
    std::string code,
    std::string path,
    std::string message)
{
    SignalDescriptorTransformResult result;
    addDiagnostic(
        result.diagnostics,
        std::move(code),
        std::move(path),
        std::move(message));
    return result;
}

} // namespace

bool SignalCapabilities::operator==(
    const SignalCapabilities& other) const noexcept
{
    return packet == other.packet
        && lifecycle == other.lifecycle
        && identity == other.identity;
}

bool SignalChannel::operator==(const SignalChannel& other) const noexcept
{
    return stableId == other.stableId
        && name == other.name
        && role == other.role;
}

bool SignalGroup::operator==(const SignalGroup& other) const noexcept
{
    return id == other.id
        && name == other.name
        && channelIds == other.channelIds;
}

bool HoaMetadata::operator==(const HoaMetadata& other) const noexcept
{
    return order == other.order
        && dimension == other.dimension
        && ordering == other.ordering
        && normalization == other.normalization;
}

bool SignalDescriptorDefinition::operator==(
    const SignalDescriptorDefinition& other) const noexcept
{
    return schemaVersion == other.schemaVersion
        && semanticRole == other.semanticRole
        && rate == other.rate
        && capabilities == other.capabilities
        && channels == other.channels
        && layout == other.layout
        && groups == other.groups
        && hoa == other.hoa;
}

SignalDescriptor::SignalDescriptor(SignalDescriptorDefinition definition)
    : definition_(std::move(definition))
{
}

SignalDescriptorBuildResult SignalDescriptor::create(
    SignalDescriptorDefinition definition)
{
    SignalDescriptorBuildResult result;
    result.diagnostics = validateDefinition(definition);
    if (result.diagnostics.empty())
        result.value = SignalDescriptor(std::move(definition));
    return result;
}

SignalDescriptor SignalDescriptor::legacyScalar()
{
    SignalDescriptorDefinition definition;
    definition.semanticRole = "control";
    definition.rate = SignalRate::FullRateControl;
    definition.capabilities = { false, false, false };
    definition.channels = { { 1u, "value", "control" } };
    return SignalDescriptor(std::move(definition));
}

SignalDescriptor SignalDescriptor::legacyMono()
{
    SignalDescriptorDefinition definition;
    definition.semanticRole = "audio";
    definition.rate = SignalRate::Audio;
    definition.capabilities = { false, false, false };
    definition.channels = { { 1u, "mono", "audio" } };
    definition.layout = "mono";
    return SignalDescriptor(std::move(definition));
}

SignalDescriptor SignalDescriptor::legacyStereo()
{
    SignalDescriptorDefinition definition;
    definition.semanticRole = "audio";
    definition.rate = SignalRate::Audio;
    definition.capabilities = { false, false, false };
    definition.channels = {
        { 1u, "left", "left" },
        { 2u, "right", "right" }
    };
    definition.layout = "stereo";
    return SignalDescriptor(std::move(definition));
}

std::uint32_t SignalDescriptor::schemaVersion() const noexcept
{
    return definition_.schemaVersion;
}

SignalWidth SignalDescriptor::width() const noexcept
{
    return static_cast<SignalWidth>(definition_.channels.size());
}

const std::string& SignalDescriptor::semanticRole() const noexcept
{
    return definition_.semanticRole;
}

SignalRate SignalDescriptor::rate() const noexcept
{
    return definition_.rate;
}

const SignalCapabilities& SignalDescriptor::capabilities() const noexcept
{
    return definition_.capabilities;
}

const std::vector<SignalChannel>& SignalDescriptor::channels() const noexcept
{
    return definition_.channels;
}

const std::optional<std::string>& SignalDescriptor::layout() const noexcept
{
    return definition_.layout;
}

const std::vector<SignalGroup>& SignalDescriptor::groups() const noexcept
{
    return definition_.groups;
}

const std::optional<HoaMetadata>& SignalDescriptor::hoa() const noexcept
{
    return definition_.hoa;
}

const SignalDescriptorDefinition& SignalDescriptor::definition() const noexcept
{
    return definition_;
}

bool SignalDescriptor::operator==(const SignalDescriptor& other) const noexcept
{
    return definition_ == other.definition_;
}

bool SignalDescriptorBuildResult::ok() const noexcept
{
    return value.has_value() && diagnostics.empty();
}

bool SignalDescriptorBuildResult::hasDiagnostic(
    const std::string& code) const noexcept
{
    return hasDiagnosticCode(diagnostics, code);
}

std::string SignalDescriptorBuildResult::getDiagnosticSummary() const
{
    return buildDiagnosticSummary(diagnostics);
}

bool SignalDescriptorTransformResult::ok() const noexcept
{
    return value.has_value() && diagnostics.empty();
}

bool SignalDescriptorTransformResult::hasDiagnostic(
    const std::string& code) const noexcept
{
    return hasDiagnosticCode(diagnostics, code);
}

std::string SignalDescriptorTransformResult::getDiagnosticSummary() const
{
    return buildDiagnosticSummary(diagnostics);
}

bool SignalDescriptorSplitResult::ok() const noexcept
{
    return ! values.empty() && diagnostics.empty();
}

bool SignalDescriptorSplitResult::hasDiagnostic(
    const std::string& code) const noexcept
{
    return hasDiagnosticCode(diagnostics, code);
}

std::string SignalDescriptorSplitResult::getDiagnosticSummary() const
{
    return buildDiagnosticSummary(diagnostics);
}

SignalDescriptorSplitResult splitSignalDescriptor(
    const SignalDescriptor& source,
    const std::vector<std::vector<std::uint32_t>>& outputs)
{
    SignalDescriptorSplitResult result;
    if (outputs.empty()) {
        addDiagnostic(
            result.diagnostics,
            "split_no_outputs",
            "outputs",
            "A split plan must declare at least one output");
        return result;
    }

    std::vector<bool> selected(source.width(), false);
    for (std::size_t output = 0; output < outputs.size(); ++output) {
        if (outputs[output].empty()) {
            addDiagnostic(
                result.diagnostics,
                "split_empty_output",
                "outputs[" + std::to_string(output) + "]",
                "Every split output must contain at least one channel");
        }
        for (std::size_t member = 0;
             member < outputs[output].size();
             ++member) {
            const auto sourceIndex = outputs[output][member];
            const auto path = "outputs[" + std::to_string(output)
                + "][" + std::to_string(member) + "]";
            if (sourceIndex >= source.width()) {
                addDiagnostic(
                    result.diagnostics,
                    "split_source_out_of_range",
                    path,
                    "Split source index exceeds input width");
                continue;
            }
            if (selected[sourceIndex]) {
                addDiagnostic(
                    result.diagnostics,
                    "split_duplicate_source",
                    path,
                    "A split partition may select each source channel once");
            }
            selected[sourceIndex] = true;
        }
    }

    if (std::find(selected.begin(), selected.end(), false) != selected.end()) {
        addDiagnostic(
            result.diagnostics,
            "split_incomplete",
            "outputs",
            "A split plan must partition every source channel");
    }
    if (! result.diagnostics.empty())
        return result;

    for (const auto& output : outputs) {
        auto definition = source.definition();
        definition.channels.clear();
        definition.channels.reserve(output.size());
        for (const auto sourceIndex : output)
            definition.channels.push_back(source.channels()[sourceIndex]);
        definition.groups = buildGroupsForChannels(
            source.groups(), definition.channels);

        bool isIdentity = outputs.size() == 1u
            && output.size() == source.width();
        for (std::size_t channel = 0;
             isIdentity && channel < output.size();
             ++channel) {
            isIdentity = output[channel] == channel;
        }
        if (! isIdentity) {
            definition.layout.reset();
            definition.hoa.reset();
        }

        auto built = SignalDescriptor::create(std::move(definition));
        if (! built.ok()) {
            result.diagnostics.insert(
                result.diagnostics.end(),
                built.diagnostics.begin(),
                built.diagnostics.end());
            result.values.clear();
            return result;
        }
        result.values.push_back(std::move(*built.value));
    }

    return result;
}

SignalDescriptorTransformResult joinSignalDescriptors(
    const std::vector<SignalDescriptor>& inputs,
    std::string semanticRole)
{
    if (inputs.empty()) {
        return buildTransformFailure(
            "join_no_inputs",
            "inputs",
            "A join requires at least one input descriptor");
    }

    SignalDescriptorDefinition definition;
    definition.semanticRole = std::move(semanticRole);
    definition.rate = inputs.front().rate();
    definition.capabilities = inputs.front().capabilities();

    OutputIdentityAllocator identities(largestChannelId(inputs));
    std::unordered_set<std::string> usedGroupIds;

    for (std::size_t input = 0; input < inputs.size(); ++input) {
        if (inputs[input].rate() != definition.rate) {
            return buildTransformFailure(
                "join_rate_mismatch",
                "inputs[" + std::to_string(input) + "].rate",
                "Joined descriptors require one prepared rate");
        }
        if (inputs[input].capabilities() != definition.capabilities) {
            return buildTransformFailure(
                "join_capability_mismatch",
                "inputs[" + std::to_string(input) + "].capabilities",
                "Joined descriptors require one capability contract");
        }

        std::unordered_map<SignalChannelId, SignalChannelId> channelIdMap;
        channelIdMap.reserve(inputs[input].channels().size());
        for (const auto& sourceChannel : inputs[input].channels()) {
            auto channel = sourceChannel;
            channel.stableId = identities.claim(channel.stableId);
            channelIdMap.emplace(
                sourceChannel.stableId, channel.stableId);
            definition.channels.push_back(std::move(channel));
        }

        for (const auto& sourceGroup : inputs[input].groups()) {
            auto group = sourceGroup;
            if (! usedGroupIds.insert(group.id).second) {
                const auto base = group.id + "@input-"
                    + std::to_string(input + 1u);
                group.id = base;
                std::size_t suffix = 2u;
                while (! usedGroupIds.insert(group.id).second)
                    group.id = base + "-" + std::to_string(suffix++);
            }
            for (auto& channelId : group.channelIds)
                channelId = channelIdMap.at(channelId);
            definition.groups.push_back(std::move(group));
        }
    }

    return buildTransformResult(
        SignalDescriptor::create(std::move(definition)));
}

SignalDescriptorTransformResult mapSignalDescriptor(
    const SignalDescriptor& source,
    const std::vector<ChannelMapEntry>& channels,
    std::string semanticRole)
{
    auto definition = source.definition();
    definition.semanticRole = std::move(semanticRole);
    definition.channels.clear();
    definition.channels.reserve(channels.size());
    definition.groups.clear();

    OutputIdentityAllocator identities(
        source.channels().empty()
            ? SignalChannelId{0}
            : std::max_element(
                  source.channels().begin(),
                  source.channels().end(),
                  [](const auto& first, const auto& second) {
                      return first.stableId < second.stableId;
                  })->stableId);

    bool identityMap = channels.size() == source.channels().size();
    for (std::size_t output = 0; output < channels.size(); ++output) {
        const auto& mapping = channels[output];
        if (mapping.sourceIndex >= source.width()) {
            return buildTransformFailure(
                "map_source_out_of_range",
                "channels[" + std::to_string(output) + "].sourceIndex",
                "Mapped source index exceeds input width");
        }

        auto channel = source.channels()[mapping.sourceIndex];
        if (mapping.name.has_value())
            channel.name = *mapping.name;
        if (mapping.role.has_value())
            channel.role = *mapping.role;
        if (mapping.stableId.has_value()) {
            const auto claimed = identities.claimExact(*mapping.stableId);
            if (! claimed.has_value()) {
                return buildTransformFailure(
                    "map_duplicate_output_identity",
                    "channels[" + std::to_string(output) + "].stableId",
                    "An explicit mapped output identity must be non-zero and unique");
            }
            channel.stableId = *claimed;
        } else {
            channel.stableId = identities.claim(channel.stableId);
        }
        definition.channels.push_back(std::move(channel));

        identityMap = identityMap
            && mapping.sourceIndex == output
            && ! mapping.name.has_value()
            && ! mapping.role.has_value()
            && ! mapping.stableId.has_value();
    }

    for (const auto& sourceGroup : source.groups()) {
        auto group = sourceGroup;
        group.channelIds.clear();
        for (std::size_t output = 0; output < channels.size(); ++output) {
            const auto sourceId =
                source.channels()[channels[output].sourceIndex].stableId;
            if (std::find(sourceGroup.channelIds.begin(),
                          sourceGroup.channelIds.end(),
                          sourceId)
                != sourceGroup.channelIds.end()) {
                group.channelIds.push_back(
                    definition.channels[output].stableId);
            }
        }
        if (! group.channelIds.empty())
            definition.groups.push_back(std::move(group));
    }
    if (! identityMap || definition.semanticRole != source.semanticRole()) {
        definition.layout.reset();
        definition.hoa.reset();
    }

    return buildTransformResult(
        SignalDescriptor::create(std::move(definition)));
}

SignalDescriptorTransformResult reorderSignalDescriptor(
    const SignalDescriptor& source,
    const std::vector<std::uint32_t>& order)
{
    if (order.size() != source.channels().size()) {
        return buildTransformFailure(
            "reorder_not_permutation",
            "order",
            "Reorder must contain every source channel exactly once");
    }

    std::vector<bool> selected(source.width(), false);
    std::vector<ChannelMapEntry> mapping;
    mapping.reserve(order.size());
    for (std::size_t output = 0; output < order.size(); ++output) {
        const auto sourceIndex = order[output];
        if (sourceIndex >= source.width() || selected[sourceIndex]) {
            return buildTransformFailure(
                "reorder_not_permutation",
                "order[" + std::to_string(output) + "]",
                "Reorder must contain every source channel exactly once");
        }
        selected[sourceIndex] = true;
        mapping.push_back({ sourceIndex, std::nullopt, std::nullopt });
    }

    return mapSignalDescriptor(
        source, mapping, source.semanticRole());
}

SignalDescriptorTransformResult quantizedSignalDescriptor(
    const SignalDescriptor& source,
    std::string semanticRole)
{
    auto definition = source.definition();
    definition.semanticRole = std::move(semanticRole);
    for (auto& channel : definition.channels)
        channel.role = definition.semanticRole;
    definition.layout.reset();
    definition.groups.clear();
    definition.hoa.reset();
    return buildTransformResult(
        SignalDescriptor::create(std::move(definition)));
}

SignalDescriptorTransformResult mergeSignalDescriptors(
    const std::vector<SignalDescriptor>& inputs,
    const std::optional<ExplicitMergePlan>& plan)
{
    if (inputs.empty()) {
        return buildTransformFailure(
            "merge_no_inputs",
            "inputs",
            "A merge requires at least one input descriptor");
    }

    if (! plan.has_value()) {
        for (std::size_t input = 1; input < inputs.size(); ++input) {
            if (inputs[input].width() != inputs.front().width()) {
                return buildTransformFailure(
                    "merge_width_mismatch",
                    "inputs[" + std::to_string(input) + "]",
                    "Different widths require an explicit channel merge plan");
            }
        }
        for (std::size_t input = 1; input < inputs.size(); ++input) {
            if (inputs[input] != inputs.front()) {
                return buildTransformFailure(
                    "merge_requires_explicit_plan",
                    "inputs[" + std::to_string(input) + "]",
                    "Implicit merge requires identical ordered descriptors");
            }
        }
        SignalDescriptorTransformResult result;
        result.value = inputs.front();
        return result;
    }

    SignalDescriptorDefinition definition;
    definition.semanticRole = plan->semanticRole;
    definition.rate = plan->rate.value_or(inputs.front().rate());
    definition.capabilities = plan->capabilities.value_or(
        inputs.front().capabilities());
    definition.layout = plan->layout;
    definition.groups = plan->groups;
    definition.hoa = plan->hoa;
    definition.channels.reserve(plan->channels.size());
    OutputIdentityAllocator identities(largestChannelId(inputs));

    for (std::size_t output = 0;
         output < plan->channels.size();
         ++output) {
        const auto& selected = plan->channels[output];
        if (selected.source >= inputs.size()) {
            return buildTransformFailure(
                "merge_source_out_of_range",
                "plan.channels[" + std::to_string(output) + "].source",
                "Merge source descriptor does not exist");
        }
        const auto& input = inputs[selected.source];
        if (selected.channel >= input.width()) {
            return buildTransformFailure(
                "merge_channel_out_of_range",
                "plan.channels[" + std::to_string(output) + "].channel",
                "Merge source channel does not exist");
        }

        auto channel = input.channels()[selected.channel];
        if (selected.name.has_value())
            channel.name = *selected.name;
        if (selected.role.has_value())
            channel.role = *selected.role;
        if (selected.stableId.has_value()) {
            const auto claimed = identities.claimExact(*selected.stableId);
            if (! claimed.has_value()) {
                return buildTransformFailure(
                    "merge_duplicate_output_identity",
                    "plan.channels[" + std::to_string(output) + "].stableId",
                    "An explicit merged output identity must be non-zero and unique");
            }
            channel.stableId = *claimed;
        } else {
            channel.stableId = identities.claim(channel.stableId);
        }
        definition.channels.push_back(std::move(channel));
    }

    return buildTransformResult(
        SignalDescriptor::create(std::move(definition)));
}

} // namespace curlop::transport
