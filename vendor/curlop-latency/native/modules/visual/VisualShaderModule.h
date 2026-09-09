#pragma once

#include "modules/backend/FaustUiCapture.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace curlop::visual
{
inline constexpr auto kLineageId = "core.visual_shader";
inline constexpr auto kGraphBackgroundTarget = "graph-background";
inline constexpr auto kAuthoredPackageId = "6b375221-c8e5-4a03-a961-864b26ea1001";
inline constexpr auto kSourceUnitId = "6b375221-c8e5-4a03-a961-864b26ea1002";
inline constexpr auto kSourceKey = "visual.main";
inline constexpr auto kSourceLanguage = "isf-2";

// Matches JavaScript's FNV-1a loop over String.charCodeAt() exactly, including
// UTF-16 surrogate code units, so native source authority and the live WebGL
// build can compare one fingerprint without translating hash domains.
inline juce::String sourceFingerprintForWeb(const std::string& source)
{
    const auto text = juce::String::fromUTF8(source.data(), (int) source.size());
    const auto utf16 = text.toUTF16();
    const auto* units = utf16.getAddress();
    juce::uint32 hash = 0x811c9dc5u;
    while (*units != 0)
    {
        hash ^= static_cast<juce::uint16>(*units++);
        hash *= 0x01000193u;
    }
    return juce::String::toHexString(static_cast<juce::int64>(hash))
        .paddedLeft('0', 8).toLowerCase();
}

struct SourceDiagnostic
{
    std::string path;
    std::string message;
};

struct InputBinding
{
    std::string kind;
    std::string locator;
    std::string surfaceId;
    std::string declaredStableId;
};

struct SourceSchema
{
    std::vector<ParamSchemaEntry> params;
    std::vector<std::string> exposedParamInputs;
    std::vector<std::string> audioInputs;
    std::vector<std::string> visualInputs;
    std::vector<std::string> visualOutputs { "texture" };
    std::vector<InputBinding> bindings;
    std::vector<SourceDiagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

inline std::string stableInputId(const std::string& kind, const std::string& locator)
{
    // Source-derived and deterministic across rebuild/save/reload. The source
    // unit UUID already namespaces the declaration; this suffix is readable
    // and changes only when the authored locator changes.
    const auto digest = sourceFingerprintForWeb(kind + ":" + locator).toStdString();
    return std::string("visual.") + kind + "." + digest;
}

inline bool isFiniteNumber(const juce::var& value, double& result)
{
    if (! (value.isInt() || value.isInt64() || value.isDouble()))
        return false;
    result = static_cast<double>(value);
    return std::isfinite(result);
}

inline std::string applyFaustLabelMetadata(const juce::String& raw,
                                           FaustUiCapture& capture)
{
    auto display = raw;
    int cursor = 0;
    while ((cursor = display.indexOf(cursor, "[")) >= 0)
    {
        const int close = display.indexOf(cursor + 1, "]");
        if (close < 0) break;
        const auto body = display.substring(cursor + 1, close);
        const int colon = body.indexOf(":");
        if (colon > 0)
        {
            const auto key = body.substring(0, colon).trim();
            const auto value = body.substring(colon + 1).trim();
            capture.declareMeta(key.toRawUTF8(), value.toRawUTF8());
            display = display.replaceSection(cursor, close - cursor + 1, {});
            continue;
        }
        cursor = close + 1;
    }
    return display.trim().toStdString();
}

inline SourceSchema parseSourceSchema(const std::string& source)
{
    SourceSchema result;
    const juce::String text(source);
    const int start = text.indexOf("/*");
    const int end = start >= 0 ? text.indexOf(start + 2, "*/") : -1;
    if (start < 0 || end < 0 || text.substring(0, start).trim().isNotEmpty())
    {
        result.diagnostics.push_back({ "/", "ISF source must begin with a JSON metadata comment" });
        return result;
    }

    juce::var header;
    const auto parsed = juce::JSON::parse(text.substring(start + 2, end), header);
    if (parsed.failed() || ! header.isObject())
    {
        result.diagnostics.push_back({ "/", "invalid ISF metadata JSON: " + parsed.getErrorMessage().toStdString() });
        return result;
    }
    if (header.getProperty("ISFVSN", {}).toString() != "2")
        result.diagnostics.push_back({ "/ISFVSN", "V1 requires ISFVSN 2" });
    const auto inputsValue = header.getProperty("INPUTS", {});
    const auto* inputs = inputsValue.getArray();
    if (inputs == nullptr)
    {
        result.diagnostics.push_back({ "/INPUTS", "ISF INPUTS must be an array" });
        return result;
    }

    std::set<std::string> locators;
    for (int index = 0; index < inputs->size(); ++index)
    {
        const auto& input = inputs->getReference(index);
        const auto path = std::string("/INPUTS/") + std::to_string(index);
        if (! input.isObject())
        {
            result.diagnostics.push_back({ path, "ISF input must be an object" });
            continue;
        }
        const auto locatorText = input.getProperty("NAME", {}).toString();
        const auto locator = locatorText.toStdString();
        if (locator.empty() || ! (std::isalpha((unsigned char) locator.front()) || locator.front() == '_')
            || std::any_of(locator.begin() + 1, locator.end(), [] (char ch) {
                return ! (std::isalnum((unsigned char) ch) || ch == '_');
            }))
        {
            result.diagnostics.push_back({ path + "/NAME", "invalid ISF input NAME" });
            continue;
        }
        if (! locators.insert(locator).second)
        {
            result.diagnostics.push_back({ path + "/NAME", "duplicate ISF input NAME" });
            continue;
        }

        const auto type = input.getProperty("TYPE", {}).toString();
        if (type == "float")
        {
            double minimum = 0.0, maximum = 1.0, defaultValue = 0.0;
            const auto minValue = input.getProperty("MIN", {});
            const auto maxValue = input.getProperty("MAX", {});
            const auto defaultVar = input.getProperty("DEFAULT", {});
            if ((! minValue.isVoid() && ! isFiniteNumber(minValue, minimum))
                || (! maxValue.isVoid() && ! isFiniteNumber(maxValue, maximum))
                || minimum >= maximum)
            {
                result.diagnostics.push_back({ path, "float DEFAULT/MIN/MAX must be finite and ordered" });
                continue;
            }
            defaultValue = minimum;
            if ((! defaultVar.isVoid() && ! isFiniteNumber(defaultVar, defaultValue))
                || defaultValue < minimum || defaultValue > maximum)
            {
                result.diagnostics.push_back({ path, "float DEFAULT/MIN/MAX must be finite and ordered" });
                continue;
            }
            if (input.hasProperty("LABEL") && ! input.getProperty("LABEL", {}).isString())
            {
                result.diagnostics.push_back({ path + "/LABEL", "float LABEL must be a string" });
                continue;
            }
            const auto label = input.hasProperty("LABEL")
                ? input.getProperty("LABEL", {}).toString() : locatorText;
            if (label.isEmpty())
            {
                result.diagnostics.push_back({ path + "/LABEL", "float LABEL must be a string" });
                continue;
            }
            FaustUiCapture capture;
            const auto displayLabel = applyFaustLabelMetadata(label, capture);
            capture.addParam(FaustWidgetKind::HSlider, displayLabel.c_str(),
                             defaultValue, minimum, maximum, 0.0);
            auto schema = capture.synthesiseSchema();
            if (schema.size() != 1)
            {
                result.diagnostics.push_back({ path + "/LABEL", "unable to normalize Faust metadata" });
                continue;
            }
            schema.front().sourceId = stableInputId("float", locator);
            result.params.push_back(schema.front());
            if (schema.front().graphInput)
                result.exposedParamInputs.push_back(schema.front().name);
            result.bindings.push_back({ "float", locator, schema.front().name,
                                        schema.front().sourceId });
        }
        else if (type == "audio")
        {
            result.audioInputs.push_back(locator);
            result.bindings.push_back({ "audio", locator, locator,
                                        stableInputId("audio", locator) });
        }
        else if (type == "image")
        {
            result.visualInputs.push_back(locator);
            result.bindings.push_back({ "image", locator, locator,
                                        stableInputId("image", locator) });
        }
        else
        {
            result.diagnostics.push_back({ path + "/TYPE",
                "unsupported ISF input TYPE " + type.toStdString() });
        }
    }
    return result;
}

inline constexpr auto kDefaultIsfSource = R"isf(/*{
  "ISFVSN": "2",
  "DESCRIPTION": "CURLOP waveform proof",
  "INPUTS": [
    {
      "NAME": "controlGain",
      "TYPE": "float",
      "DEFAULT": 0.5,
      "MIN": 0.0,
      "MAX": 1.0,
      "LABEL": "Control gain[unit:ratio][scale:linear][style:knob][curlop:input]"
    },
    { "NAME": "waveformImage", "TYPE": "audio" }
  ]
}*/
void main() {
  vec2 uv = isf_FragNormCoord;
  float waveform = texture(waveformImage, vec2(uv.x, 0.5)).r * 2.0 - 1.0;
  float line = 1.0 - smoothstep(0.0, 0.025, abs((uv.y - 0.5) - waveform * 0.22));
  float beat = 0.5 + 0.5 * sin(CURLOP_BEAT_POSITION * 6.28318530718);
  vec3 field = vec3(0.025, 0.04, 0.075) + vec3(0.02, 0.12, 0.2) * beat;
  vec3 trace = vec3(0.2, 0.75, 1.0) * line * (0.35 + controlGain * 0.65);
  gl_FragColor = vec4(field + trace, 1.0);
})isf";

inline constexpr auto kShapeIsfSource = R"isf(/*{
  "ISFVSN":"2",
  "DESCRIPTION":"CURLOP polygon primitive",
  "INPUTS":[
    {"NAME":"positionX","TYPE":"float","DEFAULT":0.5,"MIN":0,"MAX":1,"LABEL":"X[curlop:input]"},
    {"NAME":"positionY","TYPE":"float","DEFAULT":0.5,"MIN":0,"MAX":1,"LABEL":"Y[curlop:input]"},
    {"NAME":"size","TYPE":"float","DEFAULT":0.25,"MIN":0.01,"MAX":1,"LABEL":"Size[curlop:input]"},
    {"NAME":"rotation","TYPE":"float","DEFAULT":0,"MIN":-3.14159,"MAX":3.14159,"LABEL":"Rotation[curlop:input]"},
    {"NAME":"skew","TYPE":"float","DEFAULT":0,"MIN":-1,"MAX":1,"LABEL":"Skew[curlop:input]"},
    {"NAME":"sides","TYPE":"float","DEFAULT":4,"MIN":3,"MAX":12,"LABEL":"Sides[curlop:input]"},
    {"NAME":"red","TYPE":"float","DEFAULT":0.15,"MIN":0,"MAX":1,"LABEL":"Red[curlop:input]"},
    {"NAME":"green","TYPE":"float","DEFAULT":0.75,"MIN":0,"MAX":1,"LABEL":"Green[curlop:input]"},
    {"NAME":"blue","TYPE":"float","DEFAULT":1,"MIN":0,"MAX":1,"LABEL":"Blue[curlop:input]"},
    {"NAME":"alpha","TYPE":"float","DEFAULT":1,"MIN":0,"MAX":1,"LABEL":"Alpha[curlop:input]"},
    {"NAME":"gate","TYPE":"float","DEFAULT":1,"MIN":0,"MAX":1,"LABEL":"Gate[curlop:input]"}
  ]
}*/
void main() {
  vec2 p = isf_FragNormCoord - vec2(positionX, positionY);
  p.x += p.y * skew;
  float c = cos(rotation), s = sin(rotation);
  p = mat2(c,-s,s,c) * p;
  float n = floor(sides + 0.5);
  float a = atan(p.x,p.y) + 3.14159265;
  float sector = 6.2831853 / n;
  float d = cos(floor(0.5 + a / sector) * sector - a) * length(p);
  float edge = 1.0 - smoothstep(size * 0.49, size * 0.51, d);
  gl_FragColor = vec4(red, green, blue, alpha * edge * step(0.5, gate));
})isf";

inline constexpr auto kBlurIsfSource = R"isf(/*{
  "ISFVSN":"2",
  "DESCRIPTION":"CURLOP texture blur",
  "INPUTS":[
    {"NAME":"sourceImage","TYPE":"image"},
    {"NAME":"amount","TYPE":"float","DEFAULT":0.01,"MIN":0,"MAX":0.08,"LABEL":"Blur[curlop:input]"}
  ]
}*/
void main() {
  vec2 uv = isf_FragNormCoord;
  vec2 px = vec2(amount);
  vec4 colour = texture(sourceImage, uv) * 0.4;
  colour += texture(sourceImage, uv + vec2(px.x, 0.0)) * 0.15;
  colour += texture(sourceImage, uv - vec2(px.x, 0.0)) * 0.15;
  colour += texture(sourceImage, uv + vec2(0.0, px.y)) * 0.15;
  colour += texture(sourceImage, uv - vec2(0.0, px.y)) * 0.15;
  gl_FragColor = colour;
})isf";
}
