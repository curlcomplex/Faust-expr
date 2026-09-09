#pragma once

#include <juce_core/juce_core.h>
#include "graph/engine/registry/ModuleRegistryGen.h"
#include "modules/backend/FaustNode.h"

namespace curlop {

inline bool embeddedFaustSourceIsBuildable(const std::string& lineageId,
                                           const std::string& source)
{
    // Empty source intentionally resolves to the factory placeholder during
    // graph rehydration. Only an explicitly embedded Faust program needs
    // admission before it can become persisted runtime authority.
    if (lineageId != "core.faust_jit" || source.empty())
        return true;
    std::string compileError;
    FaustNode::compileSchemaOnly(source, compileError);
    return compileError.empty();
}

template <typename Registry>
bool projectRuntimeAdmission(const juce::var& envelope, const Registry& registry)
{
    const auto clipsState = envelope.getProperty("clipsState", juce::var());
    const auto clips = clipsState.getProperty("clips", juce::var());
    if (!clips.isArray()) return false;
    for (const auto& clip : *clips.getArray()) {
        const auto graph = clip.getProperty("graph", juce::var());
        const auto modules = graph.getProperty("modules", juce::var());
        if (graph.isVoid() || !modules.isArray()) continue;
        for (const auto& module : *modules.getArray()) {
            auto lineageId = module.getProperty("lineageId", juce::var())
                .toString().toStdString();
            const auto lineageUuid = module.getProperty("lineageUuid", juce::var())
                .toString().toStdString();
            if (const auto* factory = factoryModuleByUuid(lineageUuid))
                lineageId = factory->lineageId;
            if (lineageId.empty() || registry.find(lineageId) == registry.end())
                return false;
            if (! embeddedFaustSourceIsBuildable(
                    lineageId,
                    module.getProperty("code", juce::var()).toString().toStdString()))
                return false;
        }
    }
    return true;
}

} // namespace curlop
