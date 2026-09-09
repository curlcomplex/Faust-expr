#include "graph/state/GraphState.h"
#include "control/surfaces/script/ScriptOutputSockets.h"
#include "control/surfaces/script/ScriptOutputSocketsV2.h"
#include "shell/CurlopDebug.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace curlop {

std::optional<ResolvedModulationMapping> resolveModulationMapping(
    const ModuleEntry& source, const ModuleEntry& target,
    const ModulationMapping& mapping)
{
    if (mapping.routeId.empty() || mapping.sourceId.empty() || mapping.targetParamId.empty()
        || ! std::isfinite(mapping.depth) || std::abs(mapping.depth) > 1.0f)
        return std::nullopt;
    if (mapping.polarity != ModulationPolarity::Unipolar
        && mapping.polarity != ModulationPolarity::Bipolar)
        return std::nullopt;
    const auto output = std::find(source.controlOutputs.begin(), source.controlOutputs.end(),
                                   mapping.sourceId);
    const auto parameter = std::find_if(target.params.begin(), target.params.end(),
        [&] (const auto& entry) { return entry.sourceId == mapping.targetParamId; });
    if (output == source.controlOutputs.end() || parameter == target.params.end())
        return std::nullopt;
    if (parameter->acceptance != ParamAcceptance::ControlValue
        || parameter->type == ParamType::Display)
        return std::nullopt;
    if (! std::isfinite(parameter->min) || ! std::isfinite(parameter->max)
        || parameter->max < parameter->min)
        return std::nullopt;
    if (std::count(source.controlOutputs.begin(), source.controlOutputs.end(), mapping.sourceId) != 1
        || std::count_if(target.params.begin(), target.params.end(),
            [&] (const auto& entry) { return entry.sourceId == mapping.targetParamId; }) != 1)
        return std::nullopt;
    return ResolvedModulationMapping {
        static_cast<std::size_t>(std::distance(source.controlOutputs.begin(), output)),
        static_cast<std::size_t>(std::distance(target.params.begin(), parameter))
    };
}

namespace {

using EdgeIdentity = std::tuple<int, int, std::string, std::string>;

bool equalsIgnoreCase(const std::string& a, const char* b)
{
    return juce::String(a).equalsIgnoreCase(b);
}

bool containsName(const std::vector<std::string>& names, const std::string& name)
{
    for (const auto& n : names)
        if (juce::String(n).equalsIgnoreCase(juce::String(name)))
            return true;
    return false;
}

const ParamSchemaEntry* paramByName(const ModuleEntry& module, const std::string& name)
{
    for (const auto& p : module.params)
        if (juce::String(p.name).equalsIgnoreCase(juce::String(name)))
            return &p;
    return nullptr;
}

const ParamSchemaEntry* exactParamByName(const std::vector<ParamSchemaEntry>& params,
                                         const std::string& name)
{
    for (const auto& p : params)
        if (p.name == name)
            return &p;
    return nullptr;
}

bool isScriptV2(const ModuleEntry& module)
{
    return juce::String(module.lineageId).equalsIgnoreCase("core.script_v2")
        || juce::String(module.dslName).equalsIgnoreCase("script_v2");
}

const ModuleEntry* moduleByIndex(const std::vector<ModuleEntry>& modules, int index)
{
    for (const auto& m : modules)
        if (m.index == index)
            return &m;
    return nullptr;
}

const transport::SignalDescriptor* descriptorForPort(
    const std::vector<SignalPortDeclaration>& ports,
    const std::string& name)
{
    const auto found = std::find_if(
        ports.begin(), ports.end(),
        [&name](const SignalPortDeclaration& port) {
            return juce::String(port.name).equalsIgnoreCase(
                juce::String(name));
        });
    return found == ports.end() ? nullptr : &found->descriptor;
}

transport::SignalDescriptor packetOutputDescriptor(
    const std::string& role)
{
    transport::SignalDescriptorDefinition definition;
    definition.semanticRole = role;
    definition.rate = transport::SignalRate::Event;
    definition.capabilities = { true, true, true };
    definition.channels.push_back({ 1u, "voice-0", role });
    auto built =
        transport::SignalDescriptor::create(std::move(definition));
    jassert(built.ok());
    return *built.value;
}

transport::SignalDescriptor controlOutputDescriptor(
    const ModuleEntry& module,
    const std::string& name)
{
    const bool midiVoicePacket =
        equalsIgnoreCase(module.lineageId, "core.midi_in")
        && (equalsIgnoreCase(name, "gate")
            || equalsIgnoreCase(name, "pitch")
            || equalsIgnoreCase(name, "velocity")
            || equalsIgnoreCase(name, "cc"));
    if (midiVoicePacket
        || lineageHasScriptDerivedOutputs(module.lineageId))
        return packetOutputDescriptor(name);
    return transport::SignalDescriptor::legacyScalar();
}

void appendReconciledPort(
    std::vector<SignalPortDeclaration>& result,
    const std::vector<SignalPortDeclaration>& previous,
    const std::string& name,
    const transport::SignalDescriptor& fallback,
    bool retainPrevious)
{
    if (std::any_of(
            result.begin(), result.end(),
            [&name](const SignalPortDeclaration& port) {
                return juce::String(port.name).equalsIgnoreCase(
                    juce::String(name));
            }))
        return;
    const auto* retained =
        retainPrevious ? descriptorForPort(previous, name) : nullptr;
    result.push_back({ name, retained != nullptr ? *retained : fallback });
}

void reconcileCanonicalPortDescriptors(
    ModuleEntry& module,
    bool forceInputs = false,
    bool forceOutputs = false)
{
    const bool hasInputProjection = ! module.audioInputs.empty()
        || ! module.controlInputs.empty()
        || ! module.exposedParamInputs.empty();
    if (hasInputProjection || forceInputs) {
        std::vector<SignalPortDeclaration> reconciled;
        reconciled.reserve(
            module.audioInputs.size()
            + module.controlInputs.size()
            + module.exposedParamInputs.size());
        for (const auto& name : module.audioInputs)
            appendReconciledPort(
                reconciled, module.signalInputs, name,
                transport::SignalDescriptor::legacyStereo(),
                module.signalInputsExplicit);
        for (const auto& name : module.controlInputs)
            appendReconciledPort(
                reconciled, module.signalInputs, name,
                transport::SignalDescriptor::legacyScalar(),
                module.signalInputsExplicit);
        for (const auto& name : module.exposedParamInputs)
            appendReconciledPort(
                reconciled, module.signalInputs, name,
                transport::SignalDescriptor::legacyScalar(),
                module.signalInputsExplicit);
        module.signalInputs = std::move(reconciled);
    }

    const bool hasOutputProjection = ! module.audioOutputs.empty()
        || ! module.controlOutputs.empty();
    if (hasOutputProjection || forceOutputs) {
        std::vector<SignalPortDeclaration> reconciled;
        reconciled.reserve(
            module.audioOutputs.size() + module.controlOutputs.size());
        for (const auto& name : module.audioOutputs)
            appendReconciledPort(
                reconciled, module.signalOutputs, name,
                transport::SignalDescriptor::legacyStereo(),
                module.signalOutputsExplicit);
        for (const auto& name : module.controlOutputs)
            appendReconciledPort(
                reconciled, module.signalOutputs, name,
                controlOutputDescriptor(module, name),
                module.signalOutputsExplicit);
        module.signalOutputs = std::move(reconciled);
    }
}

void admitCanonicalPortDeclarations(ModuleEntry& module)
{
    if (! module.signalInputs.empty())
        module.signalInputsExplicit = true;
    if (! module.signalOutputs.empty())
        module.signalOutputsExplicit = true;
    reconcileCanonicalPortDescriptors(module);
}

const transport::SignalDescriptor* sourcePortDescriptor(
    const std::vector<ModuleEntry>& modules,
    const EdgeEntry& edge)
{
    const auto* source = moduleByIndex(modules, edge.srcIndex);
    if (source != nullptr) {
        if (const auto* descriptor =
                descriptorForPort(source->signalOutputs, edge.srcPort))
            return descriptor;
        if (edge.srcPort.empty() && ! source->signalOutputs.empty())
            return &source->signalOutputs.front().descriptor;
    }
    return nullptr;
}

transport::SignalDescriptor migrateLegacyEdgeDescriptor(
    const std::vector<ModuleEntry>& modules,
    const EdgeEntry& edge)
{
    if (const auto* descriptor = sourcePortDescriptor(modules, edge))
        return *descriptor;
    return edge.srcPort.empty() && edge.tgtPort.empty()
        ? transport::SignalDescriptor::legacyStereo()
        : transport::SignalDescriptor::legacyScalar();
}

std::string effectiveSourcePort(const std::vector<ModuleEntry>& modules,
                                const EdgeEntry& edge)
{
    if (! edge.srcPort.empty()) return edge.srcPort;
    const auto* source = moduleByIndex(modules, edge.srcIndex);
    return source != nullptr && ! source->audioOutputs.empty()
        ? source->audioOutputs.front() : std::string("OUT");
}

std::string effectiveTargetPort(const std::vector<ModuleEntry>& modules,
                                const EdgeEntry& edge)
{
    if (! edge.tgtPort.empty()) return edge.tgtPort;
    const auto* target = moduleByIndex(modules, edge.tgtIndex);
    return target != nullptr && ! target->audioInputs.empty()
        ? target->audioInputs.front() : std::string("IN");
}

bool sourcePortStillExists(const ModuleEntry& module,
                           const std::string& port,
                           bool allowUnknownContract = true)
{
    if (port.empty())
        return true;
    if (descriptorForPort(module.signalOutputs, port) != nullptr)
        return true;
    if (containsName(module.visualOutputs, port))
        return true;
    if (module.signalOutputsExplicit || ! module.signalOutputs.empty())
        return false;
    const bool hasExplicitOutputs = ! module.audioOutputs.empty()
        || ! module.controlOutputs.empty()
        || ! module.visualOutputs.empty();
    if (! hasExplicitOutputs && allowUnknownContract)
        return true;
    if (equalsIgnoreCase(port, "out") && ! isScriptV2(module))
        return module.audioOutputs.empty()
            || containsName(module.audioOutputs, port)
            || containsName(module.controlOutputs, port);
    return containsName(module.audioOutputs, port)
        || containsName(module.controlOutputs, port);
}

bool targetPortStillExists(const ModuleEntry& module,
                           const std::string& port,
                           bool allowUnknownContract = true)
{
    if (port.empty())
        return true;
    if (descriptorForPort(module.signalInputs, port) != nullptr) {
        if (containsName(module.exposedParamInputs, port)
            && ! module.params.empty()
            && paramByName(module, port) == nullptr)
            return false;
        return true;
    }
    if (containsName(module.visualInputs, port))
        return true;
    if (module.signalInputsExplicit || ! module.signalInputs.empty())
        return false;
    const bool hasExplicitInputs = ! module.audioInputs.empty()
        || ! module.controlInputs.empty()
        || ! module.exposedParamInputs.empty()
        || ! module.visualInputs.empty()
        || ! module.params.empty();
    if (! hasExplicitInputs && allowUnknownContract)
        return true;
    if (equalsIgnoreCase(port, "in") && ! isScriptV2(module))
        return module.audioInputs.empty()
            || containsName(module.audioInputs, port)
            || containsName(module.controlInputs, port)
            || containsName(module.exposedParamInputs, port);
    if (containsName(module.exposedParamInputs, port)
        && ! module.params.empty()
        && paramByName(module, port) == nullptr)
        return false;
    return containsName(module.audioInputs, port)
        || containsName(module.controlInputs, port)
        || containsName(module.exposedParamInputs, port);
}

bool isExposedParamTarget(const ModuleEntry& module, const std::string& port)
{
    return ! port.empty() && containsName(module.exposedParamInputs, port);
}

bool sourceIsControlOffset(const ModuleEntry& module, const std::string& port)
{
    return ! port.empty() && containsName(module.controlOutputs, port);
}

bool graphEdgeAcceptedByParamContract(const ModuleEntry& source,
                                      const ModuleEntry& target,
                                      const EdgeEntry& edge)
{
    if (! isExposedParamTarget(target, edge.tgtPort))
        return true;

    const auto* param = paramByName(target, edge.tgtPort);
    if (param == nullptr) {
        if (! target.params.empty()) {
            CDBG(PARAM_ACCEPT,
                 "REJECT src=%s.%s tgt=%s.%s acceptance=unknown reason=missing-target-param-schema",
                 source.dslName.c_str(), edge.srcPort.c_str(),
                 target.dslName.c_str(), edge.tgtPort.c_str());
            return false;
        }
        CDBG(PARAM_ACCEPT,
             "ACCEPT src=%s.%s tgt=%s.%s acceptance=unknown reason=no-target-param-schema",
             source.dslName.c_str(), edge.srcPort.c_str(),
             target.dslName.c_str(), edge.tgtPort.c_str());
        return true;
    }

    const bool controlOffset = sourceIsControlOffset(source, edge.srcPort);
    const bool accepted =
        param->acceptance == ParamAcceptance::ControlValue
        || (! controlOffset
            && (param->acceptance == ParamAcceptance::ControlSet
                || param->acceptance == ParamAcceptance::AudioOnly));

    CDBG(PARAM_ACCEPT,
         "%s src=%s.%s signal=%s tgt=%s.%s acceptance=%s",
         accepted ? "ACCEPT" : "REJECT",
         source.dslName.c_str(), edge.srcPort.empty() ? "out" : edge.srcPort.c_str(),
         controlOffset ? "offset" : "samples",
         target.dslName.c_str(), edge.tgtPort.c_str(),
         paramAcceptanceToStr(param->acceptance));
    return accepted;
}

bool graphEdgeAcceptedByContracts(const std::vector<ModuleEntry>& modules,
                                  const EdgeEntry& edge)
{
    const auto* source = moduleByIndex(modules, edge.srcIndex);
    const auto* target = moduleByIndex(modules, edge.tgtIndex);
    if (source == nullptr || target == nullptr)
        return false;
    if (edge.modulation)
        return resolveModulationMapping(*source, *target, *edge.modulation).has_value()
            && edge.signalRole != "visual-texture"
            && edge.feedbackBoundary == FeedbackBoundary::None;
    if (! sourcePortStillExists(*source, edge.srcPort)
        || ! targetPortStillExists(*target, edge.tgtPort))
        return false;
    const bool sourceVisual = containsName(source->visualOutputs, edge.srcPort);
    const bool targetVisual = containsName(target->visualInputs, edge.tgtPort);
    if (edge.signalRole == "visual-texture")
        return sourceVisual && targetVisual;
    if (sourceVisual || targetVisual)
        return false;
    return graphEdgeAcceptedByParamContract(*source, *target, edge);
}

bool wouldCreateVisualCycle(const std::vector<EdgeEntry>& edges,
                            const EdgeEntry& candidate)
{
    if (candidate.signalRole != "visual-texture")
        return false;
    std::vector<int> pending { candidate.tgtIndex };
    std::set<int> visited;
    while (! pending.empty())
    {
        const int current = pending.back();
        pending.pop_back();
        if (current == candidate.srcIndex)
            return true;
        if (! visited.insert(current).second)
            continue;
        for (const auto& edge : edges)
            if (edge.signalRole == "visual-texture" && edge.srcIndex == current)
                pending.push_back(edge.tgtIndex);
    }
    return false;
}

bool wouldCreateSignalCycle(const std::vector<EdgeEntry>& edges,
                            const EdgeEntry& candidate)
{
    if (candidate.signalRole == "visual-texture")
        return false;
    std::vector<int> pending { candidate.tgtIndex };
    std::set<int> visited;
    while (! pending.empty())
    {
        const int current = pending.back();
        pending.pop_back();
        if (current == candidate.srcIndex)
            return true;
        if (! visited.insert(current).second)
            continue;
        for (const auto& edge : edges)
            if (edge.signalRole != "visual-texture"
                && edge.srcIndex == current)
                pending.push_back(edge.tgtIndex);
    }
    return false;
}

bool supportsOneSampleFeedback(const transport::SignalDescriptor& descriptor)
{
    return descriptor.rate() == transport::SignalRate::Audio
        || descriptor.rate() == transport::SignalRate::FullRateControl;
}

std::string uniqueDslNameForNode(const std::vector<ModuleEntry>& modules,
                                 const std::string& nodeId,
                                 std::string dslName)
{
    if (dslName.empty())
        return dslName;

    auto takenByOther = [&modules, &nodeId](const std::string& name) {
        for (const auto& m : modules)
            if (m.moduleId != nodeId && m.dslName == name)
                return true;
        return false;
    };

    const std::string base = dslName;
    int suffix = 2;
    while (takenByOther(dslName))
        dslName = base + std::to_string(suffix++);
    return dslName;
}

void ensureUniqueDslNames(std::vector<ModuleEntry>& modules)
{
    for (size_t i = 0; i < modules.size(); ++i) {
        auto& dslName = modules[i].dslName;
        if (dslName.empty())
            continue;

        const std::string base = dslName;
        int suffix = 2;
        auto takenEarlier = [&modules, i](const std::string& name) {
            for (size_t j = 0; j < i; ++j)
                if (modules[j].dslName == name)
                    return true;
            return false;
        };
        while (takenEarlier(dslName))
            dslName = base + std::to_string(suffix++);
    }
}

int uniqueIndexForNewModule(const std::vector<ModuleEntry>& modules, int requestedIndex)
{
    std::set<int> used;
    for (const auto& m : modules)
        if (m.index >= 0)
            used.insert(m.index);

    if (requestedIndex >= 0 && used.count(requestedIndex) == 0)
        return requestedIndex;

    int index = 0;
    while (used.count(index) != 0)
        ++index;
    return index;
}

void ensureUniqueModuleIndices(std::vector<ModuleEntry>& modules)
{
    std::set<int> used;
    int next = 0;
    for (auto& m : modules) {
        if (m.index >= 0 && used.count(m.index) == 0) {
            used.insert(m.index);
            continue;
        }
        while (used.count(next) != 0)
            ++next;
        m.index = next;
        used.insert(next);
    }
}

std::unordered_map<std::string, float> reconcileReplacementParamValues(
    const ModuleEntry& previous, const ModuleEntry& replacement)
{
    std::unordered_map<std::string, float> out;
    if (! replacement.params.empty()) {
        out.reserve(replacement.params.size());
        for (const auto& p : replacement.params) {
            auto it = previous.paramValues.find(p.name);
            out[p.name] = (it != previous.paramValues.end())
                ? it->second
                : p.defaultValue;
        }
        return out;
    }

    out = replacement.paramValues;
    for (auto& [name, value] : out) {
        auto it = previous.paramValues.find(name);
        if (it != previous.paramValues.end())
            value = it->second;
    }
    return out;
}

void pruneEdgesForModuleSockets(std::vector<EdgeEntry>& edges,
                                const std::vector<ModuleEntry>& modules,
                                int moduleIndex)
{
    auto it = std::remove_if(edges.begin(), edges.end(),
        [&] (const EdgeEntry& e) {
            if (e.modulation && (e.srcIndex == moduleIndex || e.tgtIndex == moduleIndex)) {
                const auto* source = moduleByIndex(modules, e.srcIndex);
                const auto* target = moduleByIndex(modules, e.tgtIndex);
                return source == nullptr || target == nullptr
                    || ! resolveModulationMapping(*source, *target, *e.modulation);
            }
            if (e.srcIndex == moduleIndex) {
                if (const auto* m = moduleByIndex(modules, moduleIndex))
                    if (! sourcePortStillExists(*m, e.srcPort, false))
                        return true;
            }
            if (e.tgtIndex == moduleIndex) {
                if (const auto* m = moduleByIndex(modules, moduleIndex))
                    if (! targetPortStillExists(*m, e.tgtPort, false))
                        return true;
            }
            return false;
        });
    edges.erase(it, edges.end());
}

} // namespace

// ── GraphState ──────────────────────────────────────────────────────────

std::string GraphState::addModule(ModuleEntry entry) {
    entry.physicalVoices = clampPhysicalVoices(entry.physicalVoices);
    entry.stealPolicy = static_cast<uint8_t>(clampStealPolicy(entry.stealPolicy));
    if (entry.lineageId == "core.audio_input")
        entry.audioInputBusChannelCount = audioInputBusChannelCount_;
    admitCanonicalPortDeclarations(entry);
    if (entry.lineageId == "core.visual_output"
        && std::any_of(modules_.begin(), modules_.end(), [&entry] (const auto& module) {
            return module.lineageId == "core.visual_output"
                && module.moduleId != entry.moduleId;
        }))
        return {};
    entry.dslName = uniqueDslNameForNode(modules_, entry.moduleId, std::move(entry.dslName));
    const auto appliedDslName = entry.dslName;

    // Idempotent: replace if nodeId already present.
    auto it = std::find_if(modules_.begin(), modules_.end(),
        [&](const ModuleEntry& m) { return m.moduleId == entry.moduleId; });
    if (it != modules_.end()) {
        const auto previousIndex = it->index;
        entry.index = previousIndex;
        *it = std::move(entry);
        reconcileModulePortContract(*it);
    } else {
        entry.index = uniqueIndexForNewModule(modules_, entry.index);
        modules_.push_back(std::move(entry));
    }
    return appliedDslName;
}

void GraphState::removeModuleByNodeId(const std::string& nodeId) {
    int removedIndex = -1;
    for (const auto& m : modules_)
        if (m.moduleId == nodeId) {
            removedIndex = m.index;
            break;
        }

    auto it = std::remove_if(modules_.begin(), modules_.end(),
        [&](const ModuleEntry& m) { return m.moduleId == nodeId; });
    modules_.erase(it, modules_.end());

    if (removedIndex >= 0) {
        const auto edgeCountBefore = edges_.size();
        auto edgeIt = std::remove_if(edges_.begin(), edges_.end(),
            [&](const EdgeEntry& e) {
                return e.srcIndex == removedIndex || e.tgtIndex == removedIndex;
            });
        edges_.erase(edgeIt, edges_.end());
        if (edges_.size() != edgeCountBefore) ++edgeRevision_;
    }
}

bool GraphState::replaceModuleByNodeId(const std::string& nodeId,
                                       ModuleEntry replacement) {
    replacement.physicalVoices = clampPhysicalVoices(replacement.physicalVoices);
    replacement.stealPolicy = static_cast<uint8_t>(clampStealPolicy(replacement.stealPolicy));
    admitCanonicalPortDeclarations(replacement);
    for (auto& m : modules_) {
        if (m.moduleId != nodeId)
            continue;

        const auto previous = m;
        replacement.index = previous.index;
        replacement.moduleId = previous.moduleId;
        if (replacement.dslName.empty())
            replacement.dslName = previous.dslName;
        replacement.dslName = uniqueDslNameForNode(modules_, replacement.moduleId, std::move(replacement.dslName));

        replacement.position = previous.position;
        replacement.nodeSize = previous.nodeSize;
        replacement.autoPlaced = previous.autoPlaced;
        replacement.paramValues =
            reconcileReplacementParamValues(previous, replacement);

        m = std::move(replacement);
        reconcileModulePortContract(m);
        return true;
    }
    return false;
}

void GraphState::moveModule(const std::string& nodeId, juce::Point<float> pos) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            m.position   = pos;
            m.autoPlaced = false;  // T-425: an explicit move makes it deliberately placed
            return;
        }
}

void GraphState::resizeNode(const std::string& nodeId, juce::Point<float> size) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) { m.nodeSize = size; return; }
}

std::string GraphState::renameModule(const std::string& nodeId, std::string newDslName) {
    if (newDslName.empty())
        return {};

    auto takenByOther = [this, &nodeId](const std::string& name) {
        for (const auto& m : modules_)
            if (m.moduleId != nodeId && m.dslName == name)
                return true;
        return false;
    };

    const std::string base = newDslName;
    int suffix = 2;
    while (takenByOther(newDslName))
        newDslName = base + std::to_string(suffix++);

    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            m.dslName = newDslName;
            return m.dslName;
        }
    return {};
}

void GraphState::setModuleVoicePolicy(const std::string& nodeId,
                                       int physicalVoices, uint8_t stealPolicy) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            m.physicalVoices = clampPhysicalVoices(physicalVoices);
            m.stealPolicy    = static_cast<uint8_t>(clampStealPolicy(stealPolicy));
            return;
        }
}

void GraphState::setModuleProcessingMode(
    const std::string& nodeId,
    ModuleProcessingMode processingMode) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            m.processingMode = processingMode;
            return;
        }
}

void GraphState::setModuleOversamplingFactor(
    const std::string& nodeId,
    ModuleOversamplingFactor oversamplingFactor) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            m.oversamplingFactor = oversamplingFactor;
            return;
        }
}

bool GraphState::setAudioInputChannelIndex(
    const std::string& nodeId,
    std::uint32_t channelIndex) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            if (m.lineageId != "core.audio_input")
                return false;
            const bool changed = m.audioInputChannelIndex != channelIndex;
            m.audioInputChannelIndex = channelIndex;
            return changed;
        }
    return false;
}

bool GraphState::setAudioInputBusChannelCount(std::uint32_t channelCount)
{
    const bool graphChanged = audioInputBusChannelCount_ != channelCount;
    audioInputBusChannelCount_ = channelCount;
    bool moduleChanged = false;
    for (auto& module : modules_)
        if (module.lineageId == "core.audio_input") {
            moduleChanged =
                moduleChanged
                || module.audioInputBusChannelCount != channelCount;
            module.audioInputBusChannelCount = channelCount;
        }
    return graphChanged || moduleChanged;
}

void GraphState::updateModuleCode(const std::string& nodeId, std::string newCode) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            m.code = std::move(newCode);
            if (lineageHasScriptDerivedOutputs(m.lineageId))
                m.controlOutputs = deriveScriptOutputSocketNamesForLineage(
                    m.code, m.lineageId, lineageUsesPitchOutputDefaults(m.lineageId));
            if (m.lineageId == "core.script_v2") {
                m.controlInputs = deriveScriptInputSocketNamesV2(m.code);
                m.exposedParamInputs.clear();
                m.exposedParamInputsExplicit = true;
            }
            reconcileModulePortContract(
                m, m.lineageId == "core.script_v2",
                lineageHasScriptDerivedOutputs(m.lineageId));
            return;
        }
}

void GraphState::updateModuleParams(const std::string& nodeId,
                                     std::vector<ParamSchemaEntry> newParams) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            // B-330: reconcile paramValues (the per-instance live knob map)
            // to the new schema, by NAME. A param that survives keeps its
            // current knob value; a renamed/added param takes the schema
            // default; a dropped param's stale value is removed. Without this,
            // a Faust source edit (synth→effect) left the old knob keys behind
            // (e.g. {GAIN,FREQ} after the source declares only 'gain') — they
            // were emitted by get_graph AND rode into the bundle rebuild's
            // paramOverride seeding, pinning the new param by index so its knob
            // appeared dead. paramValues stays a dense mirror of the schema.
            std::unordered_map<std::string, float> reconciled;
            reconciled.reserve(newParams.size());
            for (const auto& p : newParams) {
                auto it = m.paramValues.find(p.name);
                reconciled[p.name] = (it != m.paramValues.end())
                                       ? it->second : p.defaultValue;
            }
            m.paramValues = std::move(reconciled);
            m.params = std::move(newParams);
            if (! m.exposedParamInputs.empty()) {
                std::vector<std::string> filtered;
                filtered.reserve(m.exposedParamInputs.size());
                for (const auto& name : m.exposedParamInputs)
                    if (paramByName(m, name) != nullptr)
                        filtered.push_back(name);
                if (filtered != m.exposedParamInputs) {
                    m.exposedParamInputs = std::move(filtered);
                    reconcileModulePortContract(m, true, false);
                }
            }
            return;
        }
}

void GraphState::updateModuleFaceplateElements(
    const std::string& nodeId,
    std::vector<FaceplateElement> elements) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            m.faceplateElements = std::move(elements);
            return;
        }
}

void GraphState::updateModuleFaceplateSchema(
    const std::string& nodeId,
    std::vector<FaceplateElement> elements,
    std::vector<FaceplateMeter> meters,
    FaceplateGroup group) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            m.faceplateElements = std::move(elements);
            m.faceplateMeters = std::move(meters);
            m.faceplateGroup = std::move(group);
            return;
        }
}

void GraphState::updateModuleFaceplateLayout(const std::string& nodeId,
                                             FaceplateLayout layout) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            m.faceplate = std::move(layout);
            return;
        }
}

bool GraphState::updateModuleAudioSockets(const std::string& nodeId,
                                          std::vector<std::string> inputs,
                                          std::vector<std::string> outputs) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            if (m.audioInputs == inputs && m.audioOutputs == outputs) return false;
            m.audioInputs = std::move(inputs);
            m.audioOutputs = std::move(outputs);
            reconcileModulePortContract(m, true, true);
            return true;
        }
    return false;
}

bool GraphState::updateModuleAudioPortDescriptors(
    const std::string& nodeId,
    std::vector<SignalPortDeclaration> inputs,
    std::vector<SignalPortDeclaration> outputs) {
    const auto portsEqual = [] (const std::vector<SignalPortDeclaration>& a,
                                const std::vector<SignalPortDeclaration>& b) {
        return a.size() == b.size()
            && std::equal(a.begin(), a.end(), b.begin(),
                [] (const SignalPortDeclaration& lhs,
                    const SignalPortDeclaration& rhs) {
                    return lhs.name == rhs.name && lhs.descriptor == rhs.descriptor;
                });
    };
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            if (m.signalInputsExplicit && m.signalOutputsExplicit
                && portsEqual(m.signalInputs, inputs)
                && portsEqual(m.signalOutputs, outputs))
                return false;
            m.signalInputs = std::move(inputs);
            m.signalOutputs = std::move(outputs);
            m.signalInputsExplicit = true;
            m.signalOutputsExplicit = true;
            return true;
        }
    return false;
}

bool GraphState::updateModuleControlOutputs(const std::string& nodeId,
                                            std::vector<std::string> names,
                                            std::vector<vm::SignalType> types) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            if (types.size() > names.size()) types.resize(names.size());
            if (m.controlOutputs == names && m.controlOutputTypes == types) return false;   // unchanged — no resnapshot
            m.controlOutputs = std::move(names);
            m.controlOutputTypes = std::move(types);
            reconcileModulePortContract(m, false, true);
            return true;
        }
    return false;
}

bool GraphState::updateModuleSocketPresentation(const std::string& nodeId,
                                                std::vector<SocketPresentation> presentation) {
    for (auto& m : modules_)
        if (m.moduleId == nodeId) {
            const auto same = m.socketPresentation.size() == presentation.size()
                && std::equal(m.socketPresentation.begin(), m.socketPresentation.end(),
                              presentation.begin(), [] (const SocketPresentation& a,
                                                        const SocketPresentation& b) {
                    return a.name == b.name && a.direction == b.direction
                        && a.role == b.role && a.label == b.label && a.shape == b.shape
                        && a.color == b.color && a.wireColor == b.wireColor
                        && a.wireStyle == b.wireStyle && a.polarity == b.polarity
                        && a.minValue == b.minValue && a.maxValue == b.maxValue
                        && a.anchor == b.anchor;
                });
            if (same) return false;
            m.socketPresentation = std::move(presentation);
            return true;
        }
    return false;
}

bool GraphState::setModuleExposedParamInputs(const std::string& nodeId,
                                             std::vector<std::string> names) {
    for (auto& m : modules_)
            if (m.moduleId == nodeId) {
            if (! m.params.empty()) {
                std::vector<std::string> filtered;
                filtered.reserve(names.size());
                for (const auto& name : names)
                    if (paramByName(m, name) != nullptr)
                        filtered.push_back(name);
                names = std::move(filtered);
            }
            if (m.exposedParamInputs == names && m.exposedParamInputsExplicit) return false;
            m.exposedParamInputs = std::move(names);
            m.exposedParamInputsExplicit = true;
            reconcileModulePortContract(m, true, false);
            return true;
        }
    return false;
}

bool GraphState::updateModuleInputContract(
    const std::string& nodeId,
    std::vector<ParamSchemaEntry> params,
    std::vector<std::string> exposedParamInputs,
    std::vector<std::string> audioInputs,
    std::vector<std::string> visualInputs,
    std::vector<std::string> visualOutputs,
    const std::vector<std::pair<std::string, std::string>>& portRenames)
{
    for (auto& module : modules_)
    {
        if (module.moduleId != nodeId) continue;

        std::unordered_map<std::string, float> values;
        values.reserve(params.size());
        for (const auto& replacement : params)
        {
            const ParamSchemaEntry* previous = nullptr;
            if (! replacement.sourceId.empty())
                for (const auto& candidate : module.params)
                    if (candidate.sourceId == replacement.sourceId) {
                        previous = &candidate;
                        break;
                    }
            const auto previousName = previous != nullptr ? previous->name : replacement.name;
            const auto value = module.paramValues.find(previousName);
            values[replacement.name] = juce::jlimit(
                replacement.min, replacement.max,
                value != module.paramValues.end() ? value->second : replacement.defaultValue);
        }

        bool remapped = false;
        for (auto& edge : edges_)
        {
            if (edge.tgtIndex != module.index) continue;
            for (const auto& [before, after] : portRenames)
                if (before != after && edge.tgtPort == before) {
                    edge.tgtPort = after;
                    remapped = true;
                    break;
                }
        }

        module.params = std::move(params);
        module.paramValues = std::move(values);
        module.exposedParamInputs = std::move(exposedParamInputs);
        module.exposedParamInputsExplicit = true;
        module.audioInputs = std::move(audioInputs);
        module.visualInputs = std::move(visualInputs);
        module.visualOutputs = std::move(visualOutputs);
        const auto revisionBeforeReconcile = edgeRevision_;
        reconcileModulePortContract(module, true, false);
        if (remapped && edgeRevision_ == revisionBeforeReconcile)
            ++edgeRevision_;
        return true;
    }
    return false;
}

void GraphState::reconcileModulePortContract(
    ModuleEntry& m,
    bool forceInputs,
    bool forceOutputs)
{
    reconcileCanonicalPortDescriptors(m, forceInputs, forceOutputs);
    const auto edgeCountBefore = edges_.size();
    pruneEdgesForModuleSockets(edges_, modules_, m.index);
    bool descriptorChanged = false;
    for (auto& edge : edges_) {
        if (edge.modulation && (edge.srcIndex == m.index || edge.tgtIndex == m.index)) {
            const auto* source = moduleByIndex(modules_, edge.srcIndex);
            const auto* target = moduleByIndex(modules_, edge.tgtIndex);
            const auto resolved = resolveModulationMapping(*source, *target, *edge.modulation);
            const auto& sourcePort = source->controlOutputs[resolved->sourceOutput];
            const auto& targetPort = target->params[resolved->targetParameter].name;
            descriptorChanged = descriptorChanged || edge.srcPort != sourcePort || edge.tgtPort != targetPort;
            edge.srcPort = sourcePort;
            edge.tgtPort = targetPort;
        }
        if (edge.srcIndex != m.index || edge.signalRole == "visual-texture")
            continue;
        const auto* descriptor = descriptorForPort(
            m.signalOutputs, edge.srcPort);
        if (descriptor == nullptr)
            continue;
        if (! edge.signalDescriptor
            || *edge.signalDescriptor != *descriptor) {
            edge.signalDescriptor = *descriptor;
            descriptorChanged = true;
        }
    }
    if (edges_.size() != edgeCountBefore || descriptorChanged)
        ++edgeRevision_;
}

bool GraphState::addEdge(EdgeEntry edge) {
    if (! graphEdgeAcceptedByContracts(modules_, edge))
        return false;
    if (wouldCreateVisualCycle(edges_, edge))
        return false;
    if (edge.modulation) {
        const auto* source = moduleByIndex(modules_, edge.srcIndex);
        const auto* target = moduleByIndex(modules_, edge.tgtIndex);
        const auto resolved = resolveModulationMapping(*source, *target, *edge.modulation);
        edge.srcPort = source->controlOutputs[resolved->sourceOutput];
        edge.tgtPort = target->params[resolved->targetParameter].name;
        // A mapping has the source's scalar control contract, irrespective of
        // whether its target has ever exposed a patch socket.
        edge.signalDescriptor = controlOutputDescriptor(*source, edge.srcPort);
    }
    if (edge.signalRole == "visual-texture"
        && std::any_of(edges_.begin(), edges_.end(), [&edge] (const auto& existing) {
            return existing.signalRole == "visual-texture"
                && existing.tgtIndex == edge.tgtIndex
                && existing.tgtPort == edge.tgtPort
                && (existing.srcIndex != edge.srcIndex
                    || existing.srcPort != edge.srcPort);
        }))
        return false;

    // Legacy persistence used an empty string for the primary audio socket.
    // Resolve that alias once at ingress so later socket reordering cannot
    // silently retarget an existing edge without advancing edgeRevision_.
    edge.srcPort = effectiveSourcePort(modules_, edge);
    edge.tgtPort = effectiveTargetPort(modules_, edge);
    if (edge.signalRole != "visual-texture") {
        if (const auto* descriptor = sourcePortDescriptor(modules_, edge))
            edge.signalDescriptor = *descriptor;
        else if (! edge.signalDescriptor)
            edge.signalDescriptor = migrateLegacyEdgeDescriptor(modules_, edge);
    }

    const bool closesSignalCycle = wouldCreateSignalCycle(edges_, edge);
    if (edge.modulation && closesSignalCycle)
        return false;
    if (edge.feedbackBoundary == FeedbackBoundary::OneSample) {
        // Restore preserves an explicit z^-1 identity until the complete graph
        // can be validated, rather than deriving it from serialized order.
        if (! edge.signalDescriptor
            || ! supportsOneSampleFeedback(*edge.signalDescriptor)) {
            CDBG(GRAPH_SYNC,
                 "REJECT declared feedback edge src=%d.%s tgt=%d.%s: only audio or full-rate-control cycles are supported",
                 edge.srcIndex, edge.srcPort.c_str(),
                 edge.tgtIndex, edge.tgtPort.c_str());
            return false;
        }
    } else if (closesSignalCycle) {
        if (! edge.signalDescriptor
            || ! supportsOneSampleFeedback(*edge.signalDescriptor)) {
            CDBG(GRAPH_SYNC,
                 "REJECT feedback edge src=%d.%s tgt=%d.%s: only audio or full-rate-control cycles are supported",
                 edge.srcIndex, edge.srcPort.c_str(),
                 edge.tgtIndex, edge.tgtPort.c_str());
            return false;
        }
        edge.feedbackBoundary = FeedbackBoundary::OneSample;
    } else {
        edge.feedbackBoundary = FeedbackBoundary::None;
    }

    // A persistent route identity belongs to exactly one relationship. A new
    // distinct relationship (different endpoints/ports) that reuses an existing
    // mapping's routeId would alias the same modulation route; reject it. Only
    // re-entry of that same relationship may keep the identity, which is
    // handled below by the deduplicating update (routeId retained).
    if (edge.modulation) {
        for (const auto& existing : edges_) {
            if (! existing.modulation
                || existing.modulation->routeId != edge.modulation->routeId)
                continue;
            const bool sameRelationship =
                existing.srcIndex == edge.srcIndex
                && existing.tgtIndex == edge.tgtIndex
                && effectiveSourcePort(modules_, existing) == effectiveSourcePort(modules_, edge)
                && effectiveTargetPort(modules_, existing) == effectiveTargetPort(modules_, edge);
            if (! sameRelationship) {
                CDBG(GRAPH_SYNC,
                     "REJECT modulation edge src=%d.%s tgt=%d.%s: routeId %s already bound to a distinct relationship",
                     edge.srcIndex, edge.srcPort.c_str(),
                     edge.tgtIndex, edge.tgtPort.c_str(),
                     edge.modulation->routeId.c_str());
                return false;
            }
        }
    }

    auto it = std::find_if(edges_.begin(), edges_.end(), [&](const EdgeEntry& e) {
        return e.srcIndex == edge.srcIndex && e.tgtIndex == edge.tgtIndex
            && effectiveSourcePort(modules_, e) == effectiveSourcePort(modules_, edge)
            && effectiveTargetPort(modules_, e) == effectiveTargetPort(modules_, edge);
    });
    if (it == edges_.end()) {
        edges_.push_back(std::move(edge));
        ++edgeRevision_;
    } else if (edge.modulation) {
        // Re-entering mapping mode edits this same relationship. Preserve
        // route identity and its mute/solo state, not a second hidden edge.
        if (it->modulation)
            edge.modulation->routeId = it->modulation->routeId;
        it->modulation = std::move(edge.modulation);
        ++edgeRevision_;
    }
    return true;
}

bool GraphState::edgeIsInSignalCycle(const EdgeEntry& edge) const
{
    std::vector<EdgeEntry> remainder;
    remainder.reserve(edges_.size());
    bool removed = false;
    for (const auto& existing : edges_) {
        const bool same = ! removed
            && existing.srcIndex == edge.srcIndex
            && existing.tgtIndex == edge.tgtIndex
            && existing.srcPort == edge.srcPort
            && existing.tgtPort == edge.tgtPort;
        if (same) {
            removed = true;
            continue;
        }
        remainder.push_back(existing);
    }
    return removed && wouldCreateSignalCycle(remainder, edge);
}

void GraphState::removeEdge(int srcIndex, int tgtIndex,
                            const std::string& srcPort,
                            const std::string& tgtPort) {
    const auto before = edges_.size();
    auto it = std::remove_if(edges_.begin(), edges_.end(), [&](const EdgeEntry& e) {
        return e.srcIndex == srcIndex && e.tgtIndex == tgtIndex
            && e.srcPort  == srcPort  && e.tgtPort  == tgtPort;
    });
    edges_.erase(it, edges_.end());
    if (edges_.size() != before) ++edgeRevision_;
}

bool GraphState::setEdgeParam(int srcIndex, int tgtIndex,
                              const std::string& srcPort,
                              const std::string& tgtPort,
                              const std::string& param,
                              float value)
{
    for (auto& e : edges_) {
        if (e.srcIndex != srcIndex || e.tgtIndex != tgtIndex
            || e.srcPort != srcPort || e.tgtPort != tgtPort) continue;
        if (param == "depth") {
            if (! e.modulation || ! std::isfinite(value) || std::abs(value) > 1.0f)
                return false;
            if (e.modulation->depth == value) return true;
            if (edgeRevision_ == std::numeric_limits<uint64_t>::max()) return false;
            e.modulation->depth = value;
        } else if (param == "gain") {
            if (e.gain == value) return true;
            if (edgeRevision_ == std::numeric_limits<uint64_t>::max()) return false;
            e.gain = value;
        } else if (param == "pan") {
            if (e.pan == value) return true;
            if (edgeRevision_ == std::numeric_limits<uint64_t>::max()) return false;
            e.pan = value;
        } else if (param == "muted") {
            const bool next = value != 0.0f;
            if (e.muted == next) return true;
            if (edgeRevision_ == std::numeric_limits<uint64_t>::max()) return false;
            e.muted = next;
        } else if (param == "soloed") {
            const bool next = value != 0.0f;
            if (e.soloed == next) return true;
            if (edgeRevision_ == std::numeric_limits<uint64_t>::max()) return false;
            e.soloed = next;
        } else return false;
        ++edgeRevision_;
        return true;
    }
    return false;
}

// Smart-wire mute/solo policy → effective per-edge gain. Extracted from
// GraphStateApplier (B-263) so the build path AND the live wire-param path
// share one solo BFS. Semantics documented at the declaration; logic is a
// verbatim lift of the prior in-applier loop.
std::vector<EdgeEffective> GraphState::computeEffectiveEdges() const {
    std::vector<EdgeEffective> out;
    out.reserve(edges_.size());

    bool anySoloed = false;
    std::set<int> upstreamNodes;    // backward reach of soloed srcs (incl. srcs)
    std::set<int> downstreamNodes;  // forward reach of soloed tgts (incl. tgts)
    std::set<EdgeIdentity> soloedEdgeKeys;
    for (const auto& e : edges_) {
        if (e.signalRole == "visual-texture") continue;
        if (!e.muted && e.soloed) {
            anySoloed = true;
            upstreamNodes.insert(e.srcIndex);
            downstreamNodes.insert(e.tgtIndex);
            soloedEdgeKeys.insert({e.srcIndex, e.tgtIndex, e.srcPort, e.tgtPort});
        }
    }
    if (anySoloed) {
        // Backward BFS: if tgt in upstreamNodes, src joins.
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& e : edges_) {
                if (e.signalRole == "visual-texture") continue;
                if (e.muted) continue;
                if (upstreamNodes.count(e.tgtIndex)
                    && !upstreamNodes.count(e.srcIndex)) {
                    upstreamNodes.insert(e.srcIndex);
                    changed = true;
                }
            }
        }
        // Forward BFS: if src in downstreamNodes, tgt joins.
        changed = true;
        while (changed) {
            changed = false;
            for (const auto& e : edges_) {
                if (e.signalRole == "visual-texture") continue;
                if (e.muted) continue;
                if (downstreamNodes.count(e.srcIndex)
                    && !downstreamNodes.count(e.tgtIndex)) {
                    downstreamNodes.insert(e.tgtIndex);
                    changed = true;
                }
            }
        }
    }

    for (const auto& e : edges_) {
        if (e.signalRole == "visual-texture") continue;
        if (e.srcIndex < 0 || e.tgtIndex < 0) continue;
        float effectiveGain = e.gain;
        bool audible = true;
        if (e.muted) {
            effectiveGain = 0.0f;
            audible = false;
        } else if (anySoloed) {
            const bool self = soloedEdgeKeys.count({ e.srcIndex, e.tgtIndex,
                                                     e.srcPort, e.tgtPort }) > 0;
            const bool feedsChain       = upstreamNodes.count(e.tgtIndex) > 0;
            const bool carriesDownstream = downstreamNodes.count(e.srcIndex) > 0;
            if (!self && !feedsChain && !carriesDownstream) {
                effectiveGain = 0.0f;
                audible = false;
            }
        }
        EdgeEffective ee;
        ee.srcIndex = e.srcIndex;
        ee.tgtIndex = e.tgtIndex;
        ee.gain     = effectiveGain;
        ee.pan      = e.pan;
        ee.audible  = audible;
        ee.srcPort  = e.srcPort;   // F-075: carry the named ports through
        ee.tgtPort  = e.tgtPort;
        ee.signalDescriptor = e.signalDescriptor.value_or(
            migrateLegacyEdgeDescriptor(modules_, e));
        ee.feedbackBoundary = e.feedbackBoundary;
        ee.modulation = e.modulation;
        out.push_back(ee);
    }
    return out;
}

void GraphState::setViewport(Viewport vp) { viewport_ = vp; }

bool GraphState::setParamValue(const std::string& dslName,
                               const std::string& paramName,
                               float value) {
    if (! std::isfinite(value))
        return false;

    for (auto& m : modules_)
        if (m.dslName == dslName) {
            m.paramValues[paramName] = value;
            return true;
        }
    return false;
}

bool GraphState::setKnownParamValue(const std::string& dslName,
                                    const std::string& paramName,
                                    float value,
                                    const std::vector<ParamSchemaEntry>* fallbackSchema) {
    if (! std::isfinite(value))
        return false;

    for (auto& m : modules_)
        if (m.dslName == dslName) {
            const auto valueIt = m.paramValues.find(paramName);
            const auto* schema = exactParamByName(m.params, paramName);
            if (schema == nullptr && fallbackSchema != nullptr)
                schema = exactParamByName(*fallbackSchema, paramName);
            const bool hasSchemaAuthority = ! m.params.empty()
                || (fallbackSchema != nullptr && ! fallbackSchema->empty());
            if (schema == nullptr
                && (hasSchemaAuthority || valueIt == m.paramValues.end()))
                return false;
            if (schema != nullptr && (value < schema->min || value > schema->max))
                return false;
            m.paramValues[paramName] = value;
            // An explicit target-unit edit replaces this parameter's authored
            // timing basis. The normal prepared install then drops its binding.
            if (schema != nullptr && ! schema->sourceId.empty())
                m.authoredParamValues.erase(std::remove_if(
                    m.authoredParamValues.begin(), m.authoredParamValues.end(),
                    [&] (const auto& authored) { return authored.paramId == schema->sourceId; }),
                    m.authoredParamValues.end());
            return true;
        }
    return false;
}

bool GraphState::setModuleMidiRoute(const std::string& nodeId,
                                    std::string inputRouteId,
                                    std::string outputRouteId) {
    for (auto& m : modules_) {
        if (m.moduleId != nodeId) continue;
        const bool changed = m.midiInputRouteId != inputRouteId
                          || m.midiOutputRouteId != outputRouteId;
        m.midiInputRouteId = std::move(inputRouteId);
        m.midiOutputRouteId = std::move(outputRouteId);
        return changed;
    }
    return false;
}

bool GraphState::replace(std::vector<ModuleEntry> newModules,
                         std::vector<EdgeEntry>   newEdges,
                         Viewport                 newViewport) {
    for (auto& nm : newModules) {
        nm.physicalVoices = clampPhysicalVoices(nm.physicalVoices);
        nm.stealPolicy = static_cast<uint8_t>(clampStealPolicy(nm.stealPolicy));
        if (nm.lineageId == "core.audio_input")
            nm.audioInputBusChannelCount = audioInputBusChannelCount_;
        admitCanonicalPortDeclarations(nm);
    }

    // Tier 1 (0.C-1): preserve paramValues from surviving modules (matched
    // by moduleId — React Flow uuid, stable across dslName renames) before
    // swapping vectors. paramValues is per-clip persistence authority; it
    // MUST survive USER_LOAD_CLIP_GRAPH re-dispatches fired by in-session
    // view_clip / setModules / setEdges. After Step 6b deleted the JS wire
    // carrying, the payload's paramValues map is always empty on re-dispatch;
    // without this port, the second visit to a clip wipes live + pending-applied
    // values. [-r DUMB-VIEW-INV-1]
    //
    // Similarly, viewport defaults to (0,0,zoom=1) on JS re-dispatches that
    // don't carry a real viewport (GraphStore._dispatchLoadClipGraph hardcodes
    // those). Keep the existing viewport unless the incoming carries a
    // genuine non-default value.
    if (!modules_.empty()) {
        for (auto& nm : newModules) {
            if (nm.moduleId.empty()) continue;
            for (const auto& om : modules_) {
                if (om.moduleId == nm.moduleId) {
                    // Port paramValues without violating schema authority.
                    // When the incoming module carries a concrete schema, keep
                    // the knob map dense to that schema, as updateModuleParams()
                    // and replaceModuleByNodeId() do. For schema-less modules,
                    // keep the historical gap-fill behavior used by JS graph
                    // redispatches that omit paramValues entirely.
                    if (! nm.params.empty()) {
                        std::unordered_map<std::string, float> reconciled;
                        reconciled.reserve(nm.params.size());
                        for (const auto& p : nm.params) {
                            auto incoming = nm.paramValues.find(p.name);
                            if (incoming != nm.paramValues.end()) {
                                reconciled[p.name] = incoming->second;
                                continue;
                            }
                            auto previous = om.paramValues.find(p.name);
                            reconciled[p.name] = previous != om.paramValues.end()
                                ? previous->second
                                : p.defaultValue;
                        }
                        nm.paramValues = std::move(reconciled);
                    } else {
                        for (const auto& [k, v] : om.paramValues) {
                            if (nm.paramValues.find(k) == nm.paramValues.end())
                                nm.paramValues[k] = v;
                        }
                    }
                    if (nm.midiInputRouteId.empty())
                        nm.midiInputRouteId = om.midiInputRouteId;
                    if (nm.midiOutputRouteId.empty())
                        nm.midiOutputRouteId = om.midiOutputRouteId;
                    break;
                }
            }
        }
    }

    ensureUniqueDslNames(newModules);
    ensureUniqueModuleIndices(newModules);
    const bool incomingViewportIsDefault =
        newViewport.x == 0.0f && newViewport.y == 0.0f && newViewport.zoom == 1.0f;

    GraphState candidate = *this;
    candidate.modules_ = std::move(newModules);
    ++candidate.edgeRevision_;
    candidate.edges_.clear();
    candidate.edges_.reserve(newEdges.size());
    for (auto& edge : newEdges)
        if (!candidate.addEdge(std::move(edge)))
            return false;

    modules_ = std::move(candidate.modules_);
    edges_ = std::move(candidate.edges_);
    edgeRevision_ = candidate.edgeRevision_;
    if (!incomingViewportIsDefault) viewport_ = newViewport;
    return true;
}

const ModuleEntry* GraphState::findByNodeId(const std::string& nodeId) const noexcept {
    for (const auto& m : modules_) if (m.moduleId == nodeId) return &m;
    return nullptr;
}

const ModuleEntry* GraphState::findByDslName(const std::string& name) const noexcept {
    for (const auto& m : modules_) if (m.dslName == name) return &m;
    return nullptr;
}

const ModuleEntry* GraphState::findByIndex(int idx) const noexcept {
    for (const auto& m : modules_) if (m.index == idx) return &m;
    return nullptr;
}

// ── GraphStateContainer ─────────────────────────────────────────────────

GraphState& GraphStateContainer::forClip(int clipId) {
    auto [it, inserted] = states_.try_emplace(clipId);
    if (inserted)
        it->second.setAudioInputBusChannelCount(
            audioInputBusChannelCount_);
    return it->second;
}

const GraphState* GraphStateContainer::tryForClip(int clipId) const noexcept {
    auto it = states_.find(clipId);
    return it == states_.end() ? nullptr : &it->second;
}

void GraphStateContainer::eraseClip(int clipId) {
    states_.erase(clipId);
}

std::vector<int> GraphStateContainer::clipIds() const {
    std::vector<int> out;
    out.reserve(states_.size());
    for (const auto& [k, _] : states_) out.push_back(k);
    return out;
}

void GraphStateContainer::setAudioInputBusChannelCount(
    std::uint32_t channelCount)
{
    audioInputBusChannelCount_ = channelCount;
    for (auto& [_, state] : states_)
        state.setAudioInputBusChannelCount(channelCount);
}

// Cut 0.C-3b: deep-copy src clip's GraphState into dst slot. Used by
// USER_DUPLICATE_CLIP handler. No-op if src absent. [-r DUMB-VIEW-INV-1]
void GraphStateContainer::duplicateClip(int srcClipId, int dstClipId) {
    auto it = states_.find(srcClipId);
    if (it == states_.end()) return;
    const GraphState& src = it->second;
    std::vector<ModuleEntry> modulesCopy = src.modules();

    // B-322 (F-069, SF-074): in-graph instances are identity-independent. A
    // duplicate must NOT share a designId (lineageUuid) with its source — the
    // version store is project-level (modules/<uuid>/vN.fdsp), so two instances
    // on one uuid entangle their version histories (saving/stepping one bumps
    // the other; Neo s504 saw #FAUST_JIT2 v8 vs #FAUST_JIT v2). Fork each copied
    // authored module to a fresh, unversioned identity: clear the lineageUuid +
    // version pin, keep the embedded snapshot (code/params/faceplate/paramValues
    // stay current — updateModuleCode tracks the viewed version on time-travel),
    // so the copy carries the current sound but starts its own history. The
    // library stays the only shared artifact (commit publishes, recall copies).
    // Design: .planning/research/f-069-module-presets-and-identity-design.md.
    for (auto& m : modulesCopy) {
        if (! m.lineageUuid.empty()) {
            m.lineageUuid.clear();
            m.moduleVersion = 0;
            m.moduleStoreStatus.clear();
        }
        // B-255: a duplicated clip must be a self-contained unit — re-stamp a
        // fresh, unique moduleId for every copied module. The source ids are
        // copied verbatim otherwise, literally carrying the source clip's id
        // (e.g. 'core.script_default_clip1' lands in clip 2), so two clips
        // share node identity. Downstream maps keyed by moduleId/nodeId then
        // alias across clips (Web editor sessions, schema-fingerprint and
        // version maps — GAP-2/GAP-3 of the s483 per-clip-isolation audit).
        // dslName is the script-facing label and stays as-is (within-clip
        // uniqueness already held in the source). Edges resolve by index, not
        // moduleId, so no edge rewrite is required.
        m.moduleId = m.lineageId + "_c" + std::to_string(dstClipId)
                   + "_" + juce::Uuid().toDashedString().substring(0, 8).toStdString();
    }

    std::vector<EdgeEntry>   edgesCopy   = src.edges();
    Viewport                 viewportCopy = src.viewport();
    forClip(dstClipId).replace(std::move(modulesCopy),
                               std::move(edgesCopy),
                               viewportCopy);
}

// ── core.script_v2 helpers ──────────────────────────────────────────────

const ModuleEntry* findFirstCoreScriptV2Module(const std::vector<ModuleEntry>& modules) noexcept {
    for (const auto& m : modules)
        if (m.lineageId == "core.script_v2") return &m;
    return nullptr;
}

ModuleEntry makeCoreScriptV2ModuleEntry(const std::vector<ModuleEntry>& existingModules,
                                        int clipId,
                                        const std::string& idSuffix,
                                        std::string initialCode) {
    int maxIndex = -1;
    for (const auto& m : existingModules) maxIndex = std::max(maxIndex, m.index);

    ModuleEntry entry;
    entry.index     = maxIndex + 1;
    entry.dslName   = "script";
    int suffix = 2;
    auto dslExists = [&existingModules](const std::string& n) {
        for (const auto& m : existingModules) if (m.dslName == n) return true;
        return false;
    };
    while (dslExists(entry.dslName))
        entry.dslName = "script" + std::to_string(suffix++);
    entry.moduleId  = "core.script_v2_" + idSuffix + "_clip" + std::to_string(clipId);
    entry.lineageId = "core.script_v2";
    entry.position  = juce::Point<float>(0.0f, 0.0f);
    entry.code      = std::move(initialCode);
    entry.controlOutputs = deriveScriptOutputSocketNamesForLineage(
        entry.code, entry.lineageId, true);
    return entry;
}

} // namespace curlop
