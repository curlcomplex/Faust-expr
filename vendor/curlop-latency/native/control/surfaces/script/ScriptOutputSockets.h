#pragma once

#include "control/surfaces/script/ScriptParser.h"
#include "control/vm/machine/Signal.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace curlop {

struct ScriptOutputSocketDecl {
    std::string name;
    float value = 0.0f;
    bool lfo = false;
    curlop::vm::SignalType type = curlop::vm::SignalType::Value;
};

inline bool scriptOutputSocketIsDefault(const std::string& name)
{
    return name == "gate" || name == "pitch" || name == "velocity";
}

inline float scriptOutputValueFromEvent(const script::AstNode& ev)
{
    if (ev.outputSocket.equalsIgnoreCase("gate")) return 1.0f;
    if (ev.outputSocket.equalsIgnoreCase("velocity")) return 0.7f;
    if (! ev.pitch.isEmpty()) return 0.0f;

    if (! ev.processors.empty() && ! ev.processors.front().args.empty()) {
        const auto& p = ev.processors.front();
        if (p.name.equalsIgnoreCase("gate") || p.canonicalName.equalsIgnoreCase("gate"))
            return 1.0f;
        if (p.name.equalsIgnoreCase("velocity") || p.name.equalsIgnoreCase("vel")
            || p.canonicalName.equalsIgnoreCase("velocity")
            || p.canonicalName.equalsIgnoreCase("vel"))
            return 0.7f;
        const auto& v = ev.processors.front().args.front();
        if (v.kind == script::ParseValue::Kind::Number
            || v.kind == script::ParseValue::Kind::UnitNumber)
            return std::isfinite(v.number) ? (float) v.number : 0.0f;
    }
    return 0.0f;
}

inline curlop::vm::SignalType scriptOutputSignalTypeFromEvent(const script::AstNode& ev)
{
    if (! ev.processors.empty()) {
        const auto& p = ev.processors.front();
        const auto& name = p.canonicalName.isNotEmpty() ? p.canonicalName : p.name;
        if (name.equalsIgnoreCase("gate")) return curlop::vm::SignalType::Gate;
        if (name.equalsIgnoreCase("pitch") || name.equalsIgnoreCase("note")
            || name.equalsIgnoreCase("freq"))
            return curlop::vm::SignalType::Pitch;
        if (name.equalsIgnoreCase("velocity") || name.equalsIgnoreCase("vel"))
            return curlop::vm::SignalType::Velocity;
    }
    if (ev.outputSocket.equalsIgnoreCase("gate")) return curlop::vm::SignalType::Gate;
    if (ev.outputSocket.equalsIgnoreCase("pitch")) return curlop::vm::SignalType::Pitch;
    if (ev.outputSocket.equalsIgnoreCase("velocity")) return curlop::vm::SignalType::Velocity;
    return curlop::vm::SignalType::Value;
}

inline bool scriptOutputEventIsLfo(const script::AstNode& ev)
{
    if (ev.processors.empty()) return false;
    const auto& p = ev.processors.front();
    if (p.name.equalsIgnoreCase("lfo") || p.name.equalsIgnoreCase("~lfo")
        || p.canonicalName.equalsIgnoreCase("lfo"))
        return true;
    if (! p.args.empty() && p.args.front().kind == script::ParseValue::Kind::Generator
        && p.args.front().gen && p.args.front().gen->generatorType.equalsIgnoreCase("lfo"))
        return true;
    return false;
}

inline void addScriptOutputSocket(std::vector<ScriptOutputSocketDecl>& out,
                                  std::set<std::string>& seen,
                                  std::string name,
                                  float value = 0.0f,
                                  bool lfo = false,
                                  curlop::vm::SignalType type = curlop::vm::SignalType::Value)
{
    if (name.empty() || seen.count(name) != 0) return;
    seen.insert(name);
    out.push_back({ std::move(name), value, lfo, type });
}

inline curlop::vm::SignalType scriptSocketSignalTypeFromName(const std::string& name)
{
    if (name == "gate") return curlop::vm::SignalType::Gate;
    if (name == "pitch") return curlop::vm::SignalType::Pitch;
    if (name == "velocity" || name == "vel") return curlop::vm::SignalType::Velocity;
    return curlop::vm::SignalType::Value;
}

inline float scriptSocketDefaultValueFromName(const std::string& name)
{
    if (name == "gate") return 1.0f;
    if (name == "velocity" || name == "vel") return 0.7f;
    return 0.0f;
}

inline void collectScriptOutputSocketsFromNode(const script::AstNode* n,
                                               std::vector<ScriptOutputSocketDecl>& out,
                                               std::set<std::string>& seen)
{
    if (n == nullptr) return;
    if (n->type == "Event" && n->eventType == "local_output"
        && n->outputSocket.isNotEmpty()) {
        addScriptOutputSocket(out, seen, n->outputSocket.toStdString(),
                              scriptOutputValueFromEvent(*n),
                              scriptOutputEventIsLfo(*n),
                              scriptOutputSignalTypeFromEvent(*n));
    }
    collectScriptOutputSocketsFromNode(n->value.get(), out, seen);
    collectScriptOutputSocketsFromNode(n->setterEvent.get(), out, seen);
    for (const auto& child : n->steps)
        collectScriptOutputSocketsFromNode(child.get(), out, seen);
    for (const auto& child : n->events)
        collectScriptOutputSocketsFromNode(child.get(), out, seen);
    for (const auto& child : n->options)
        collectScriptOutputSocketsFromNode(child.get(), out, seen);
    if (n->type == "VariableRef")
        collectScriptOutputSocketsFromNode(n->resolved, out, seen);
}

inline std::vector<ScriptOutputSocketDecl>
deriveScriptOutputSocketDecls(const std::string& source,
                              bool includePitchDefaults = true)
{
    std::vector<ScriptOutputSocketDecl> out;
    std::set<std::string> seen;
    addScriptOutputSocket(out, seen, "gate", 1.0f, false, curlop::vm::SignalType::Gate);
    if (includePitchDefaults) {
        addScriptOutputSocket(out, seen, "pitch", 0.0f, false, curlop::vm::SignalType::Pitch);
        addScriptOutputSocket(out, seen, "velocity", 0.7f, false, curlop::vm::SignalType::Velocity);
    }

    const auto ast = script::parse(juce::String(source));
    for (const auto& stmt : ast.statements)
        collectScriptOutputSocketsFromNode(stmt.get(), out, seen);
    return out;
}

inline std::vector<std::string>
deriveScriptOutputSocketNames(const std::string& source,
                              bool includePitchDefaults = true)
{
    auto decls = deriveScriptOutputSocketDecls(source, includePitchDefaults);
    std::vector<std::string> names;
    names.reserve(decls.size());
    for (const auto& d : decls) names.push_back(d.name);
    return names;
}

inline bool lineageHasScriptDerivedOutputs(const std::string& lineageId)
{
    return lineageId == "core.script_v2"
        || lineageId == "core.stepseq" || lineageId == "core.pitch_seq"
        || lineageId == "core.trigger_seq";
}

inline bool lineageUsesPitchOutputDefaults(const std::string& lineageId)
{
    return lineageId != "core.trigger_seq";
}

} // namespace curlop
