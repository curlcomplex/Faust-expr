#pragma once

#include "control/surfaces/script/ScriptOutputSockets.h"
#include "control/surfaces/script/ScriptParserV2.h"
#include "control/surfaces/script/music/PitchUtils.h"
#include "control/vm/machine/Signal.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <unordered_map>

namespace curlop {

struct ScriptV2SimpleSocketRoute {
    std::string output;
    std::string input;
};

struct ScriptV2SocketRouteOp {
    struct Arg {
        bool dynamic = false;
        float value = 0.0f;
        std::string source;
    };

    std::string name;
    float a = 0.0f;
    float b = 0.0f;
    int argCount = 0;
    Arg argA;
    Arg argB;
};

struct ScriptV2SocketRoute {
    std::string output;
    std::string input;
    std::vector<ScriptV2SocketRouteOp> ops;
};

inline bool collectScriptSocketRoutesV2(const std::string& source,
                                        std::vector<ScriptV2SocketRoute>& routes);

inline bool scriptV2ParseStrictFloatLiteral(const juce::String& raw, float& out)
{
    const auto t = raw.trim();
    if (t.isEmpty())
        return false;

    const auto text = t.toStdString();
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str()
        || end == nullptr
        || *end != '\0'
        || errno == ERANGE
        || ! std::isfinite(value))
        return false;

    out = value;
    return true;
}

inline void addScriptV2BundleSelector(std::vector<std::string>& out,
                                      std::set<std::string>& seen,
                                      const juce::String& name)
{
    const auto s = name.toStdString();
    if (! s.empty() && seen.insert(s).second)
        out.push_back(s);
}

inline void collectScriptV2BundleSelectorsFromMaterial(
    const script::v2::Material& material,
    std::vector<std::string>& out,
    std::set<std::string>& seen)
{
    if (material.kind != script::v2::MaterialKind::Sequence)
        return;

    for (const auto& step : material.steps) {
        for (const auto& item : step.items) {
            if (item.kind == script::v2::StepItemKind::Receiver) {
                addScriptV2BundleSelector(out, seen, item.receiver.target);
            } else if (item.kind == script::v2::StepItemKind::GroupedReceiver) {
                for (const auto& target : item.grouped.targets)
                    addScriptV2BundleSelector(out, seen, target);
            } else if (item.kind == script::v2::StepItemKind::Tuplet && item.nested) {
                collectScriptV2BundleSelectorsFromMaterial(*item.nested, out, seen);
            }
        }
    }

    for (const auto& op : material.postfix)
        if (op.kind == script::v2::PostfixKind::GroupReceiver)
            addScriptV2BundleSelector(out, seen, op.name);
}

inline std::vector<std::string> scriptV2BundleSelectorsForStatement(
    const script::v2::Statement& stmt,
    const std::unordered_map<juce::String, script::v2::Material>& definitions)
{
    std::vector<std::string> selectors;
    std::set<std::string> seen;
    addScriptV2BundleSelector(selectors, seen, "gate");
    addScriptV2BundleSelector(selectors, seen, "pitch");
    addScriptV2BundleSelector(selectors, seen, "velocity");

    script::v2::Material resolvedRefWithPostfix;
    const script::v2::Material* material = &stmt.material;
    if (stmt.material.kind == script::v2::MaterialKind::Ref) {
        const auto it = definitions.find(stmt.material.refName);
        if (it != definitions.end()) {
            resolvedRefWithPostfix = it->second;
            resolvedRefWithPostfix.postfix.insert(resolvedRefWithPostfix.postfix.end(),
                                                  stmt.material.postfix.begin(),
                                                  stmt.material.postfix.end());
            material = &resolvedRefWithPostfix;
        }
    }
    collectScriptV2BundleSelectorsFromMaterial(*material, selectors, seen);
    return selectors;
}

inline std::vector<ScriptOutputSocketDecl>
deriveScriptOutputSocketDeclsV2(const std::string& source,
                                bool includePitchDefaults = true)
{
    std::vector<ScriptOutputSocketDecl> out;
    std::set<std::string> seen;
    std::set<std::string> supportedRouteOutputs;
    std::vector<ScriptV2SocketRoute> supportedRoutes;
    if (collectScriptSocketRoutesV2(source, supportedRoutes))
        for (const auto& route : supportedRoutes)
            supportedRouteOutputs.insert(route.output);

    addScriptOutputSocket(out, seen, "gate", 1.0f, false, curlop::vm::SignalType::Gate);
    if (includePitchDefaults) {
        addScriptOutputSocket(out, seen, "pitch", 0.0f, false, curlop::vm::SignalType::Pitch);
        addScriptOutputSocket(out, seen, "velocity", 0.7f, false, curlop::vm::SignalType::Velocity);
    }

    const auto program = script::v2::parseProgramV2(juce::String(source));
    std::unordered_map<juce::String, script::v2::Material> definitions;
    for (const auto& stmt : program.statements)
        if (stmt.kind == script::v2::StatementKind::Definition)
            definitions[stmt.name] = stmt.material;

    for (const auto& stmt : program.statements) {
        if (stmt.kind == script::v2::StatementKind::SocketRoute) {
            const auto name = stmt.name.toStdString();
            if (supportedRouteOutputs.count(name) == 0
                && ! script::v2::isChannelRouteSyntax(stmt))
                continue;
            addScriptOutputSocket(out, seen, name,
                                  scriptSocketDefaultValueFromName(name), false,
                                  scriptSocketSignalTypeFromName(name));
            continue;
        }
        if (stmt.kind != script::v2::StatementKind::SocketExport)
            continue;
        if (! stmt.socketSelected) {
            for (const auto& selector : scriptV2BundleSelectorsForStatement(stmt, definitions)) {
                const auto leaf = juce::String(selector).fromLastOccurrenceOf(".", false, false)
                    .toStdString();
                const auto socketName = stmt.name.toStdString() + "." + selector;
                addScriptOutputSocket(out, seen, socketName,
                                      scriptSocketDefaultValueFromName(leaf), false,
                                      scriptSocketSignalTypeFromName(leaf));
            }
            continue;
        }
        for (const auto& selector : stmt.selectors) {
            const auto leaf = selector.name.fromLastOccurrenceOf(".", false, false)
                .toStdString();
            const auto socketName = stmt.name.toStdString() + "." + selector.name.toStdString();
            if (selector.kind == script::v2::SocketSelectorKind::DerivedStream
                && supportedRouteOutputs.count(socketName) == 0)
                continue;
            addScriptOutputSocket(out, seen, socketName,
                                  scriptSocketDefaultValueFromName(leaf), false,
                                  scriptSocketSignalTypeFromName(leaf));
        }
    }
    return out;
}

inline std::vector<std::string>
deriveScriptOutputSocketNamesForLineage(const std::string& source,
                                        const std::string& lineageId,
                                        bool includePitchDefaults = true)
{
    auto decls = lineageId == "core.script_v2"
        ? deriveScriptOutputSocketDeclsV2(source, includePitchDefaults)
        : deriveScriptOutputSocketDecls(source, includePitchDefaults);
    std::vector<std::string> names;
    names.reserve(decls.size());
    for (const auto& d : decls) names.push_back(d.name);
    return names;
}

inline bool scriptV2ParseSocketRouteOp(const juce::String& source,
                                       ScriptV2SocketRouteOp& op)
{
    auto s = source.trim();
    const int lp = s.indexOfChar('(');
    if (lp <= 0 || ! s.endsWithChar(')'))
        return false;

    op.name = s.substring(0, lp).trim().toLowerCase().toStdString();
    const auto args = s.substring(lp + 1, s.length() - 1).trim();
    const auto numeric = [](const juce::String& raw, float& out) {
        const auto t = raw.trim();
        const int slash = t.indexOfChar('/');
        if (slash > 0 && slash < t.length() - 1
            && t.indexOfChar(slash + 1, '/') < 0) {
            float num = 0.0f;
            float den = 0.0f;
            if (! scriptV2ParseStrictFloatLiteral(t.substring(0, slash), num)
                || ! scriptV2ParseStrictFloatLiteral(t.substring(slash + 1), den)
                || den == 0.0f)
                return false;
            const float value = num / den;
            if (! std::isfinite(value))
                return false;
            out = value;
            return true;
        }
        return scriptV2ParseStrictFloatLiteral(t, out);
    };
    const auto pitchSignalFromHz = [](double hz, float& out) {
        if (! std::isfinite(hz) || hz <= 0.0)
            return false;
        const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
        out = curlop::vm::noteToSignal((float) (midi - 60.0));
        return true;
    };
    const auto routeScalar = [&](const juce::String& raw, float& out) {
        const auto t = raw.trim();
        const auto lower = t.toLowerCase();
        if (numeric(t, out))
            return true;
        if (lower.endsWith("khz") && t.length() > 3) {
            float v = 0.0f;
            if (! numeric(t.dropLastCharacters(3), v))
                return false;
            return pitchSignalFromHz((double) v * 1000.0, out);
        }
        if (lower.endsWith("hz") && t.length() > 2) {
            float v = 0.0f;
            if (! numeric(t.dropLastCharacters(2), v))
                return false;
            return pitchSignalFromHz((double) v, out);
        }
        if (lower.endsWith("%") && t.length() > 1) {
            float v = 0.0f;
            if (! numeric(t.dropLastCharacters(1), v))
                return false;
            out = v / 100.0f;
            return true;
        }
        if (lower.endsWith("ms") && t.length() > 2) {
            float v = 0.0f;
            if (! numeric(t.dropLastCharacters(2), v))
                return false;
            out = v / 1000.0f;
            return true;
        }
        if (lower.endsWith("steps") && t.length() > 5) {
            return numeric(t.dropLastCharacters(5), out);
        }
        if (lower.endsWith("step") && t.length() > 4) {
            return numeric(t.dropLastCharacters(4), out);
        }
        if (lower.endsWith("s") && t.length() > 1) {
            return numeric(t.dropLastCharacters(1), out);
        }
        const int midi = curlop::pitch::noteToMidi(t);
        if (midi >= 0) {
            out = curlop::vm::noteToSignal((float) (midi - 60));
            return true;
        }
        return false;
    };
    const auto timeSeconds = [&](const juce::String& raw, float& out) {
        const auto t = raw.trim().toLowerCase();
        if (t.endsWith("ms")) {
            float v = 0.0f;
            if (! numeric(t.dropLastCharacters(2), v))
                return false;
            out = v / 1000.0f;
            return true;
        }
        if (t.endsWith("s")) {
            return numeric(t.dropLastCharacters(1), out);
        }
        return numeric(t, out);
    };
    const auto splitArgs = [](const juce::String& body,
                              std::vector<juce::String>& parts) {
        juce::String current;
        int parens = 0, brackets = 0, braces = 0;
        for (int i = 0; i < body.length(); ++i) {
            const auto c = body[i];
            if (c == '(') ++parens;
            else if (c == ')') --parens;
            else if (c == '[') ++brackets;
            else if (c == ']') --brackets;
            else if (c == '{') ++braces;
            else if (c == '}') --braces;
            if (parens < 0 || brackets < 0 || braces < 0)
                return false;
            if (c == ',' && parens == 0 && brackets == 0 && braces == 0) {
                const auto part = current.trim();
                if (part.isEmpty())
                    return false;
                parts.push_back(part);
                current = {};
            } else {
                current << juce::String::charToString(c);
            }
        }
        if (parens != 0 || brackets != 0 || braces != 0)
            return false;
        const auto tail = current.trim();
        if (tail.isNotEmpty())
            parts.push_back(tail);
        return true;
    };
    const auto splitChain = [](const juce::String& body,
                               std::vector<juce::String>& parts) {
        juce::String current;
        int parens = 0, brackets = 0, braces = 0;
        for (int i = 0; i < body.length(); ++i) {
            const auto c = body[i];
            if (c == '(') ++parens;
            else if (c == ')') --parens;
            else if (c == '[') ++brackets;
            else if (c == ']') --brackets;
            else if (c == '{') ++braces;
            else if (c == '}') --braces;
            if (parens < 0 || brackets < 0 || braces < 0)
                return false;
            if (c == ':' && parens == 0 && brackets == 0 && braces == 0) {
                const auto part = current.trim();
                if (part.isEmpty())
                    return false;
                parts.push_back(part);
                current = {};
            } else {
                current << juce::String::charToString(c);
            }
        }
        if (parens != 0 || brackets != 0 || braces != 0)
            return false;
        const auto tail = current.trim();
        if (tail.isNotEmpty())
            parts.push_back(tail);
        return true;
    };
    const std::function<bool(const juce::String&)> dynamicArgSupported =
        [&](const juce::String& raw) -> bool {
            const auto src = raw.trim();
            if (src.isEmpty())
                return false;
            float ignored = 0.0f;
            if (routeScalar(src, ignored))
                return true;

            std::vector<juce::String> chain;
            if (! splitChain(src, chain) || chain.empty())
                return false;
            if (chain.size() > 1) {
                if (! dynamicArgSupported(chain.front()))
                    return false;
                for (size_t i = 1; i < chain.size(); ++i) {
                    ScriptV2SocketRouteOp nested;
                    if (! scriptV2ParseSocketRouteOp(chain[i], nested))
                        return false;
                    if (nested.name == "smooth")
                        return false;
                }
                return true;
            }

            const auto single = chain.front().trim();
            if (single.startsWithChar('<'))
                return single.length() > 1
                    && ! single.substring(1).trim().containsAnyOf(" \t\r\n");

            const int lp = single.indexOfChar('(');
            if (lp <= 0 || ! single.endsWithChar(')'))
                return false;
            const auto name = single.substring(0, lp).trim().toLowerCase();
            std::vector<juce::String> callArgs;
            if (! splitArgs(single.substring(lp + 1, single.length() - 1), callArgs))
                return false;
            if (name == "lfo") {
                if (callArgs.size() != 4 || callArgs.front().trim().isEmpty())
                    return false;
                return dynamicArgSupported(callArgs[1])
                    && dynamicArgSupported(callArgs[2])
                    && dynamicArgSupported(callArgs[3]);
            }
            return false;
        };
    const auto routeArg = [&](const juce::String& raw,
                              ScriptV2SocketRouteOp::Arg& arg,
                              float& scalar) {
        if (routeScalar(raw, scalar)) {
            arg.dynamic = false;
            arg.value = scalar;
            return true;
        }
        if (! dynamicArgSupported(raw))
            return false;
        arg.dynamic = true;
        arg.source = raw.trim().toStdString();
        return true;
    };

    if (op.name == "slew")
        op.name = "smooth";
    if (op.name == "invert") {
        if (! args.isEmpty() && ! routeArg(args, op.argA, op.a))
            return false;
        if (args.isEmpty())
            op.a = 0.5f;
        op.argA.value = op.a;
        op.argCount = args.isEmpty() ? 0 : 1;
        return true;
    }
    if (op.name == "abs") {
        op.argCount = 0;
        return args.isEmpty();
    }
    if (op.name == "gain" || op.name == "offset") {
        if (! routeArg(args, op.argA, op.a))
            return false;
        op.argA.value = op.a;
        op.argCount = 1;
        return true;
    }
    if (op.name == "transpose" || op.name == "octave") {
        float amount = 0.0f;
        if (! numeric(args, amount))
            return false;
        const bool octave = op.name == "octave";
        op.name = "offset";
        op.a = amount * (octave ? curlop::vm::kOctave : curlop::vm::kSemitone);
        op.argA.value = op.a;
        op.argCount = 1;
        return true;
    }
    if (op.name == "smooth") {
        if (! timeSeconds(args, op.a)) {
            if (! dynamicArgSupported(args))
                return false;
            op.argA.dynamic = true;
            op.argA.source = args.trim().toStdString();
        }
        op.argA.value = op.a;
        op.argCount = 1;
        return true;
    }
    if (op.name == "clip" || op.name == "scale") {
        std::vector<juce::String> parts;
        if (! splitArgs(args, parts) || parts.size() != 2)
            return false;
        if (! routeArg(parts[0], op.argA, op.a)
            || ! routeArg(parts[1], op.argB, op.b))
            return false;
        op.argA.value = op.a;
        op.argB.value = op.b;
        op.argCount = 2;
        return true;
    }
    return false;
}

inline bool scriptV2RouteExprIsSimpleInputRead(const script::v2::SignalExpr& expr,
                                               juce::String& inputName)
{
    if (expr.inputReads.size() != 1 || expr.chain.size() != 1)
        return false;

    const auto compact = expr.source.removeCharacters(" \t\r\n");
    const auto expected = "<" + expr.inputReads.front();
    if (compact != expected)
        return false;

    inputName = expr.inputReads.front();
    return true;
}

inline bool scriptV2RouteExprStartsWithInputRead(const script::v2::SignalExpr& expr,
                                                 juce::String& inputName)
{
    if (expr.inputReads.size() != 1 || expr.chain.empty())
        return false;

    const auto compact = expr.chain.front().removeCharacters(" \t\r\n");
    const auto expected = "<" + expr.inputReads.front();
    if (compact != expected)
        return false;

    inputName = expr.inputReads.front();
    return true;
}

inline bool scriptV2AppendSocketRouteFromExpr(std::vector<ScriptV2SocketRoute>& routes,
                                              const std::string& outputName,
                                              const script::v2::SignalExpr& expr)
{
    juce::String inputName;
    if (! scriptV2RouteExprStartsWithInputRead(expr, inputName))
        return false;

    ScriptV2SocketRoute route;
    route.output = outputName;
    route.input = inputName.toStdString();
    for (size_t i = 1; i < expr.chain.size(); ++i) {
        ScriptV2SocketRouteOp op;
        if (! scriptV2ParseSocketRouteOp(expr.chain[i], op))
            return false;
        route.ops.push_back(std::move(op));
    }
    routes.push_back(std::move(route));
    return true;
}

inline bool collectScriptSocketRoutesV2(const std::string& source,
                                       std::vector<ScriptV2SocketRoute>& routes)
{
    routes.clear();
    const auto program = script::v2::parseProgramV2(juce::String(source));
    bool allRoutesSupported = true;
    for (const auto& stmt : program.statements) {
        if (stmt.kind == script::v2::StatementKind::SocketRoute) {
            if (script::v2::isChannelRouteSyntax(stmt)) {
                script::v2::ChannelRoute channelRoute;
                juce::String error;
                if (! script::v2::parseChannelRoute(stmt, channelRoute, error))
                    allRoutesSupported = false;
                continue;
            }
            if (! scriptV2AppendSocketRouteFromExpr(routes,
                                                   stmt.name.toStdString(),
                                                   stmt.body))
                allRoutesSupported = false;
            continue;
        }

        if (stmt.kind != script::v2::StatementKind::SocketExport || ! stmt.socketSelected)
            continue;

        for (const auto& selector : stmt.selectors) {
            if (selector.kind != script::v2::SocketSelectorKind::DerivedStream)
                continue;
            const auto outputName = stmt.name.toStdString() + "."
                + selector.name.toStdString();
            if (! scriptV2AppendSocketRouteFromExpr(routes, outputName, selector.expr))
                allRoutesSupported = false;
        }
    }
    return allRoutesSupported;
}

inline std::vector<ScriptV2SocketRoute>
deriveScriptSocketRoutesV2(const std::string& source)
{
    std::vector<ScriptV2SocketRoute> routes;
    collectScriptSocketRoutesV2(source, routes);
    return routes;
}

inline std::vector<ScriptV2SimpleSocketRoute>
deriveScriptSimpleSocketRoutesV2(const std::string& source)
{
    std::vector<ScriptV2SimpleSocketRoute> routes;
    for (const auto& route : deriveScriptSocketRoutesV2(source))
        if (route.ops.empty())
            routes.push_back({ route.output, route.input });
    return routes;
}

inline void collectScriptInputReadsFromExpr(const script::v2::SignalExpr& expr,
                                            std::vector<std::string>& out,
                                            std::set<std::string>& seen)
{
    for (const auto& input : expr.inputReads) {
        const auto s = input.toStdString();
        if (! s.empty() && seen.insert(s).second)
            out.push_back(s);
    }
}

inline void collectScriptInputReadsFromMaterial(const script::v2::Material& material,
                                                std::vector<std::string>& out,
                                                std::set<std::string>& seen)
{
    if (material.kind != script::v2::MaterialKind::Sequence)
        return;
    for (const auto& step : material.steps) {
        for (const auto& item : step.items) {
            if (item.kind == script::v2::StepItemKind::Receiver)
                collectScriptInputReadsFromExpr(item.receiver.expr, out, seen);
            else if (item.kind == script::v2::StepItemKind::GroupedReceiver)
                collectScriptInputReadsFromExpr(item.grouped.expr, out, seen);
            else if (item.kind == script::v2::StepItemKind::Markov
                     || item.kind == script::v2::StepItemKind::Stack
                     || item.kind == script::v2::StepItemKind::Preset
                     || item.kind == script::v2::StepItemKind::Select)
                collectScriptInputReadsFromExpr(item.receiver.expr, out, seen);

            if ((item.kind == script::v2::StepItemKind::Tuplet
                 || item.kind == script::v2::StepItemKind::Stack)
                && item.nested)
                collectScriptInputReadsFromMaterial(*item.nested, out, seen);
            for (const auto& option : item.blockOptions)
                if (option)
                    collectScriptInputReadsFromMaterial(*option, out, seen);
        }
    }
    for (const auto& op : material.postfix)
        collectScriptInputReadsFromExpr(op.expr, out, seen);
}

inline std::vector<std::string>
deriveScriptInputSocketNamesV2(const std::string& source)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::vector<ScriptV2SocketRoute> supportedRoutes;
    const bool routesSupported = collectScriptSocketRoutesV2(source, supportedRoutes);
    const auto addSupportedRouteInputsForOutput = [&](const std::string& outputName) {
        if (! routesSupported)
            return;
        for (const auto& route : supportedRoutes) {
            if (route.output != outputName)
                continue;
            if (! route.input.empty() && seen.insert(route.input).second)
                out.push_back(route.input);
        }
    };

    const auto program = script::v2::parseProgramV2(juce::String(source));
    for (const auto& stmt : program.statements) {
        if (stmt.kind == script::v2::StatementKind::SocketRoute) {
            if (script::v2::isChannelRouteSyntax(stmt)) {
                script::v2::ChannelRoute channelRoute;
                juce::String error;
                if (script::v2::parseChannelRoute(stmt, channelRoute, error)) {
                    for (const auto& reference : channelRoute.references) {
                        if (reference.sourceKind
                                != vm::ChannelSourceKind::PacketInput)
                            continue;
                        const auto name = reference.signal.toStdString();
                        if (seen.insert(name).second)
                            out.push_back(name);
                    }
                }
                continue;
            }
            addSupportedRouteInputsForOutput(stmt.name.toStdString());
            continue;
        }
        collectScriptInputReadsFromExpr(stmt.body, out, seen);
        collectScriptInputReadsFromMaterial(stmt.material, out, seen);
        for (const auto& selector : stmt.selectors) {
            if (stmt.kind == script::v2::StatementKind::SocketExport
                && selector.kind == script::v2::SocketSelectorKind::DerivedStream) {
                const auto outputName = stmt.name.toStdString() + "."
                    + selector.name.toStdString();
                addSupportedRouteInputsForOutput(outputName);
                continue;
            }
            collectScriptInputReadsFromExpr(selector.expr, out, seen);
        }
    }
    return out;
}

} // namespace curlop
