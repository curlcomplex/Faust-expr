#pragma once

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "modules/contract/FaceplateTypes.h"
#include "modules/contract/ModuleTypes.h"

namespace curlop {

enum class FaustModuleRole {
    Unspecified,
    AudioSource,
    AudioProcessor,
    ControlSource,
    ControlProcessor,
};

struct FaustSourceDisplayMetadata
{
    std::string name;
    std::string category;
    std::string description;
};

inline std::string faustDeclareStringValue(const std::string& source,
                                           const std::string& declareName)
{
    const std::string key = "declare " + declareName;
    auto pos = source.find(key);
    if (pos == std::string::npos) return {};

    pos += key.size();
    while (pos < source.size() && std::isspace((unsigned char) source[pos])) ++pos;
    if (pos >= source.size() || source[pos] != '"') return {};

    std::string value;
    bool escaped = false;
    for (size_t i = pos + 1; i < source.size(); ++i) {
        const char ch = source[i];
        if (escaped) {
            value.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') return value;
        value.push_back(ch);
    }
    return {};
}

inline FaustSourceDisplayMetadata faustSourceDisplayMetadataFromSource(const std::string& source)
{
    FaustSourceDisplayMetadata m;
    m.name        = faustDeclareStringValue(source, "name");
    m.category    = faustDeclareStringValue(source, "category");
    m.description = faustDeclareStringValue(source, "description");
    return m;
}

inline float faustSourceMetadataFloat(const std::string& s, float dflt)
{
    const char* start = s.c_str();
    char* end = nullptr;
    const float parsed = std::strtof(start, &end);
    while (end != nullptr && *end != '\0' && std::isspace((unsigned char) *end))
        ++end;
    return end == start || end == nullptr || *end != '\0' || ! std::isfinite(parsed)
        ? dflt
        : parsed;
}

inline std::vector<std::string> faustGuiMetadataTokens(const std::string& spec)
{
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    bool escaped = false;
    for (char ch : spec) {
        if (escaped) {
            cur.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (quote != 0) {
            if (ch == quote) quote = 0;
            else cur.push_back(ch);
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            continue;
        }
        if (std::isspace((unsigned char) ch)) {
            if (! cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur.push_back(ch);
    }
    if (! cur.empty()) out.push_back(cur);
    return out;
}

inline FaceplateElement faceplateElementFromCurlopGuiSpec(const std::string& spec,
                                                          int index)
{
    FaceplateElement e;
    const auto tokens = faustGuiMetadataTokens(spec);
    std::unordered_map<std::string, std::string> kv;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];
        const auto eq = t.find('=');
        if (eq == std::string::npos) {
            if (i == 0 && e.kind.empty()) e.kind = t;
            continue;
        }
        kv[t.substr(0, eq)] = t.substr(eq + 1);
    }
    auto get = [&kv](const char* key) -> std::string {
        auto it = kv.find(key);
        return it == kv.end() ? std::string() : it->second;
    };

    if (e.kind.empty()) e.kind = get("type");
    if (e.kind.empty()) e.kind = get("kind");
    e.id          = get("id");
    e.label       = get("text");
    if (e.label.empty()) e.label = get("label");
    e.variant     = get("variant");
    e.group       = get("group");
    e.source      = get("source");
    e.orientation = normaliseFaceplateOrientationMetadata(get("orientation"));
    e.column      = faustSourceMetadataFloat(get("col"), -1.0f);
    e.row         = faustSourceMetadataFloat(get("row"), -1.0f);
    e.width       = faustSourceMetadataFloat(get("w"), 1.0f);
    e.height      = faustSourceMetadataFloat(get("h"), 1.0f);
    e.accent      = normaliseFaceplateAccentMetadata(get("accent"));
    if (e.accent.empty()) e.accent = normaliseFaceplateAccentMetadata(get("color"));
    e.fontSize    = faustSourceMetadataFloat(get("labelsize").empty() ? get("fontSize") : get("labelsize"), 0.0f);
    e.labelPosition = normaliseFaceplateLabelPositionMetadata(get("labelpos"));
    if (e.labelPosition.empty()) e.labelPosition = normaliseFaceplateLabelPositionMetadata(get("labelPosition"));
    const auto hidden = get("hidden");
    e.hidden      = hidden == "1" || hidden == "true";
    e.minValue    = faustSourceMetadataFloat(get("min"), 0.0f);
    e.maxValue    = faustSourceMetadataFloat(get("max"), 1.0f);
    e.threshold   = faustSourceMetadataFloat(get("threshold"), 0.5f);

    if (e.id.empty())
        e.id = "curlop_gui." + std::to_string(index);
    return e;
}

inline std::string faustSourceWithoutComments(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    bool lineComment = false;
    bool blockComment = false;
    bool quoted = false;
    bool escaped = false;
    for (size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';

        if (lineComment) {
            if (ch == '\n') { lineComment = false; out.push_back(ch); }
            else out.push_back(' ');
            continue;
        }
        if (blockComment) {
            if (ch == '*' && next == '/') {
                blockComment = false;
                out.push_back(' ');
                out.push_back(' ');
                ++i;
            } else {
                out.push_back(ch == '\n' ? '\n' : ' ');
            }
            continue;
        }
        if (quoted) {
            out.push_back(ch);
            if (escaped) { escaped = false; continue; }
            if (ch == '\\') { escaped = true; continue; }
            if (ch == '"') quoted = false;
            continue;
        }
        if (ch == '/' && next == '/') {
            lineComment = true;
            out.push_back(' ');
            out.push_back(' ');
            ++i;
            continue;
        }
        if (ch == '/' && next == '*') {
            blockComment = true;
            out.push_back(' ');
            out.push_back(' ');
            ++i;
            continue;
        }
        if (ch == '"') quoted = true;
        out.push_back(ch);
    }
    return out;
}

inline std::vector<std::string> faustGuiSpecsFromSource(const std::string& source)
{
    std::vector<std::string> out;
    const auto scanSource = faustSourceWithoutComments(source);
    const std::string key = "declare curlop_gui";
    size_t pos = 0;
    while ((pos = scanSource.find(key, pos)) != std::string::npos) {
        pos += key.size();
        while (pos < scanSource.size() && std::isspace((unsigned char) scanSource[pos])) ++pos;
        if (pos >= scanSource.size() || scanSource[pos] != '"') continue;

        std::string value;
        bool escaped = false;
        size_t i = pos + 1;
        for (; i < scanSource.size(); ++i) {
            const char ch = scanSource[i];
            if (escaped) {
                value.push_back(ch);
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') break;
            value.push_back(ch);
        }
        pos = i < scanSource.size() ? i + 1 : scanSource.size();

        out.push_back(std::move(value));
    }
    return out;
}

inline SocketPresentation socketPresentationFromCurlopGuiSpec(const std::string& spec)
{
    SocketPresentation s;
    const auto tokens = faustGuiMetadataTokens(spec);
    std::unordered_map<std::string, std::string> kv;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto eq = tokens[i].find('=');
        if (eq != std::string::npos) kv[tokens[i].substr(0, eq)] = tokens[i].substr(eq + 1);
    }
    const auto get = [&kv](const char* key) -> std::string {
        const auto it = kv.find(key);
        return it == kv.end() ? std::string() : it->second;
    };
    s.name = get("id");
    s.direction = get("direction");
    s.role = get("role");
    s.label = get("label");
    s.shape = get("shape");
    s.color = get("color");
    s.wireColor = get("wireColor");
    s.wireStyle = get("wireStyle");
    s.polarity = get("polarity");
    s.minValue = faustSourceMetadataFloat(get("min"), 0.0f);
    s.maxValue = faustSourceMetadataFloat(get("max"), 1.0f);
    s.anchor = get("anchor");
    return s;
}

inline std::vector<SocketPresentation> faustSocketPresentationsFromSource(const std::string& source)
{
    std::vector<SocketPresentation> out;
    for (const auto& spec : faustGuiSpecsFromSource(source)) {
        const auto tokens = faustGuiMetadataTokens(spec);
        if (tokens.empty() || tokens.front() != "socket") continue;
        auto socket = socketPresentationFromCurlopGuiSpec(spec);
        if (! socket.name.empty() && (socket.direction == "output" || socket.direction == "input"))
            out.push_back(std::move(socket));
    }
    return out;
}

inline std::vector<FaceplateElement> faustGuiElementsFromSource(const std::string& source)
{
    std::vector<FaceplateElement> out;
    for (const auto& spec : faustGuiSpecsFromSource(source)) {
        auto e = faceplateElementFromCurlopGuiSpec(spec, (int) out.size());
        if (! e.kind.empty()
            && e.kind != "socket"
            && (! e.hidden
                || e.kind == "group"
                || e.kind == "panel"
                || e.kind == "faceplate"))
            out.push_back(std::move(e));
    }
    return out;
}

inline ModuleProcessingCapabilities faustProcessingCapabilitiesFromSource(
    const std::string& source)
{
    ModuleProcessingCapabilities capabilities;
    const auto uncommented = faustSourceWithoutComments(source);
    const auto processing =
        faustDeclareStringValue(uncommented, "curlop_processing");
    capabilities.independentMono = processing == "independent-mono";
    capabilities.independentVoiceCohorts =
        processing == "independent-voices";
    return capabilities;
}

inline FaustModuleRole faustModuleRoleFromString(const std::string& s)
{
    if (s == "audio-source")       return FaustModuleRole::AudioSource;
    if (s == "audio-processor")    return FaustModuleRole::AudioProcessor;
    if (s == "control-source")     return FaustModuleRole::ControlSource;
    if (s == "control-processor")  return FaustModuleRole::ControlProcessor;
    return FaustModuleRole::Unspecified;
}

inline bool faustRoleSuppressesAudioOut(FaustModuleRole r)
{
    return r == FaustModuleRole::ControlSource || r == FaustModuleRole::ControlProcessor;
}

inline FaustModuleRole faustModuleRoleFromSource(const std::string& source)
{
    const std::string key = "declare curlop_role";
    const std::string legacyKey = "declare curlop.role";
    auto pos = source.find(key);
    size_t keySize = key.size();
    if (pos == std::string::npos) {
        pos = source.find(legacyKey);
        keySize = legacyKey.size();
    }
    if (pos == std::string::npos) return FaustModuleRole::Unspecified;

    pos += keySize;
    while (pos < source.size() && std::isspace((unsigned char) source[pos])) ++pos;
    if (pos >= source.size()) return FaustModuleRole::Unspecified;

    if (source[pos] == '"') {
        const auto end = source.find('"', pos + 1);
        if (end == std::string::npos) return FaustModuleRole::Unspecified;
        return faustModuleRoleFromString(source.substr(pos + 1, end - pos - 1));
    }

    const auto end = source.find(';', pos);
    if (end == std::string::npos) return FaustModuleRole::Unspecified;
    auto value = source.substr(pos, end - pos);
    while (!value.empty() && std::isspace((unsigned char) value.back())) value.pop_back();
    return faustModuleRoleFromString(value);
}

inline std::vector<std::string> exposedParamInputsFromSchemaMetadata(
    const std::vector<ParamSchemaEntry>& schema)
{
    std::vector<std::string> out;
    for (const auto& p : schema)
        if (p.graphInput && !p.name.empty() && p.type != ParamType::Display)
            out.push_back(p.name);
    return out;
}

inline std::string faustLabelBase(const std::string& label)
{
    const auto bracket = label.find('[');
    return bracket == std::string::npos ? label : label.substr(0, bracket);
}

inline std::string setCurlopLabelFlag(std::string label,
                                      const std::string& flag,
                                      bool enabled)
{
    const std::string tag = "[curlop:" + flag + "]";
    for (;;) {
        const auto p = label.find(tag);
        if (p == std::string::npos) break;
        label.erase(p, tag.size());
    }
    if (enabled)
        label += tag;
    return label;
}

inline std::string rewriteFaustInputTags(const std::string& source,
                                         const std::vector<std::string>& paramNames,
                                         const std::vector<std::string>& exposedNames)
{
    std::set<std::string> params(paramNames.begin(), paramNames.end());
    std::set<std::string> exposed(exposedNames.begin(), exposedNames.end());
    std::string out;
    out.reserve(source.size() + exposed.size() * 16);

    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] != '"') {
            out.push_back(source[i]);
            continue;
        }

        size_t j = i + 1;
        bool escaped = false;
        std::string label;
        for (; j < source.size(); ++j) {
            const char ch = source[j];
            if (escaped) {
                label.push_back(ch);
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                label.push_back(ch);
                escaped = true;
                continue;
            }
            if (ch == '"') break;
            label.push_back(ch);
        }
        if (j >= source.size()) {
            out.append(source.substr(i));
            break;
        }

        const auto base = faustLabelBase(label);
        if (params.count(base) > 0)
            label = setCurlopLabelFlag(label, "input", exposed.count(base) > 0);

        out.push_back('"');
        out.append(label);
        out.push_back('"');
        i = j;
    }
    return out;
}

} // namespace curlop
