#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "shell/WebViewContract.h"

namespace curlop::wire {

inline juce::String makeParamUpdatedJson(int clipId,
                                         const std::string& dslName,
                                         const std::string& paramName,
                                         float value,
                                         const std::string& nodeId = {})
{
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("contractVersion", webview::contractVersion);
    payload->setProperty("type", "PARAM_UPDATED");
    payload->setProperty("clipId", clipId);
    if (! nodeId.empty())
        payload->setProperty("nodeId", juce::String(nodeId));
    payload->setProperty("dslName", juce::String(dslName));
    payload->setProperty("param", juce::String(paramName));
    payload->setProperty("value", value);
    return juce::JSON::toString(juce::var(payload.get()), false);
}

inline juce::var makeParamStateRowVar(const std::string& dslName,
                                      const std::string& paramName,
                                      float value,
                                      int presence,
                                      float knobValue,
                                      float effectiveValue,
                                      bool midi)
{
    juce::DynamicObject::Ptr row = new juce::DynamicObject();
    row->setProperty("m", juce::String(dslName));
    row->setProperty("p", juce::String(paramName));
    row->setProperty("v", value);
    row->setProperty("pr", presence);
    row->setProperty("kv", std::isnan(knobValue) ? juce::var() : juce::var(knobValue));
    row->setProperty("ev", effectiveValue);
    if (midi)
        row->setProperty("midi", 1);
    return juce::var(row.get());
}

inline juce::String makeParamStateJson(int clipId, juce::Array<juce::var> rows)
{
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("contractVersion", webview::contractVersion);
    payload->setProperty("type", "PARAM_STATE");
    payload->setProperty("clipId", clipId);
    payload->setProperty("params", juce::var(rows));
    return juce::JSON::toString(juce::var(payload.get()), false);
}

inline juce::String makeMrspResultJson(const juce::String& requestId,
                                       const juce::String& resultJson)
{
    juce::DynamicObject::Ptr envelope = new juce::DynamicObject();
    envelope->setProperty("requestId", requestId);
    envelope->setProperty("result", juce::JSON::parse(resultJson));
    return juce::JSON::toString(juce::var(envelope.get()), true);
}

inline juce::String makeMrspErrorJson(const juce::String& requestId,
                                      const juce::String& error)
{
    juce::DynamicObject::Ptr envelope = new juce::DynamicObject();
    envelope->setProperty("requestId", requestId);
    envelope->setProperty("error", error);
    return juce::JSON::toString(juce::var(envelope.get()), true);
}

inline juce::String makeProjectFileInfoJson(const juce::String& name,
                                            const juce::String& path)
{
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("type", "PROJECT_FILE_INFO");
    payload->setProperty("name", name);
    payload->setProperty("path", path);
    return juce::JSON::toString(juce::var(payload.get()), true);
}

inline juce::String makeProjectSavedJson(const juce::String& name,
                                         const juce::String& path)
{
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("type", "PROJECT_SAVED");
    payload->setProperty("name", name);
    payload->setProperty("path", path);
    return juce::JSON::toString(juce::var(payload.get()), true);
}

inline juce::String makeProjectLoadedJson(const juce::String& projectName)
{
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("type", "PROJECT_LOADED");
    payload->setProperty("projectName", projectName);
    return juce::JSON::toString(juce::var(payload.get()), true);
}

inline juce::String makeProjectFileStateJson(const juce::String& event,
                                             bool ok,
                                             bool dirty,
                                             const juce::String& name,
                                             const juce::String& path,
                                             const juce::String& currentPath,
                                             const juce::String& folderPath,
                                             const juce::String& message)
{
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("contractVersion", 1);
    payload->setProperty("type", "PROJECT_FILE_STATE");
    payload->setProperty("event", event);
    payload->setProperty("ok", ok);
    payload->setProperty("dirty", dirty);
    payload->setProperty("name", name);
    payload->setProperty("path", path);
    payload->setProperty("currentPath", currentPath);
    payload->setProperty("folderPath", folderPath);
    if (message.isNotEmpty())
        payload->setProperty("message", message);
    return juce::JSON::toString(juce::var(payload.get()), true);
}

inline juce::String makeRecentProjectsStateJson(const juce::StringArray& paths)
{
    juce::Array<juce::var> entries;
    for (const auto& path : paths)
    {
        const juce::File file(path);
        juce::DynamicObject::Ptr entry = new juce::DynamicObject();
        entry->setProperty("name", file.getFileNameWithoutExtension());
        entry->setProperty("path", path);
        entries.add(juce::var(entry.get()));
    }
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("contractVersion", 1);
    payload->setProperty("type", "RECENT_PROJECTS_STATE");
    payload->setProperty("entries", entries);
    return juce::JSON::toString(juce::var(payload.get()), true);
}

inline juce::String makeProjectActionStateJson(int requestId,
                                               const juce::String& actionId,
                                               const juce::String& phase,
                                               const juce::String& message)
{
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("contractVersion", 1);
    payload->setProperty("type", "PROJECT_ACTION_STATE");
    payload->setProperty("requestId", requestId);
    payload->setProperty("actionId", actionId);
    payload->setProperty("phase", phase);
    payload->setProperty("message", message);
    return juce::JSON::toString(juce::var(payload.get()), true);
}

inline juce::var makeMidiRouteDeviceVar(const juce::String& identifier,
                                        const juce::String& name,
                                        bool present)
{
    juce::DynamicObject::Ptr device = new juce::DynamicObject();
    device->setProperty("identifier", identifier);
    device->setProperty("name", name);
    device->setProperty("present", present);
    return juce::var(device.get());
}

inline juce::String makeMidiRouteStateJson(juce::Array<juce::var> inputs,
                                           juce::Array<juce::var> outputs,
                                           int outputDrops,
                                           int missingInputLookups,
                                           int missingOutputLookups,
                                           int inputDrops = 0,
                                           int inputHighWater = 0)
{
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("type", "MIDI_ROUTE_STATE");
    payload->setProperty("inputs", juce::var(inputs));
    payload->setProperty("outputs", juce::var(outputs));
    payload->setProperty("outputDrops", outputDrops);
    payload->setProperty("inputDrops", inputDrops);
    payload->setProperty("inputHighWater", inputHighWater);
    payload->setProperty("missingInputLookups", missingInputLookups);
    payload->setProperty("missingOutputLookups", missingOutputLookups);
    return juce::JSON::toString(juce::var(payload.get()), true);
}

inline juce::var makeScriptCompileModuleIndexVar(const juce::String& dslName,
                                                 int index)
{
    juce::Array<juce::var> row;
    row.add(dslName);
    row.add(index);
    return juce::var(row);
}

inline juce::String makeScriptCompileResultJson(int clipId,
                                                const juce::String& bytecodeBase64,
                                                juce::Array<juce::var> moduleIndices,
                                                const juce::String& compileError)
{
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("contractVersion", 1);
    payload->setProperty("type", "SCRIPT_COMPILE_RESULT");
    payload->setProperty("clipId", clipId);
    payload->setProperty("bytecode", bytecodeBase64);
    payload->setProperty("moduleIndices", juce::var(moduleIndices));
    payload->setProperty("paramMappings", juce::var(juce::Array<juce::var>()));
    payload->setProperty("bpm", 0);
    payload->setProperty("loopLength", 0);
    if (compileError.isNotEmpty())
        payload->setProperty("compileError", compileError);
    return juce::JSON::toString(juce::var(payload.get()), true);
}

inline juce::var makeParamHistorySampleVar(uint32_t tick,
                                           float vm,
                                           float knobValue,
                                           float presence,
                                           float effectiveValue,
                                           bool hit)
{
    juce::DynamicObject::Ptr sample = new juce::DynamicObject();
    sample->setProperty("tk", static_cast<int>(tick));
    sample->setProperty("vm", vm);
    sample->setProperty("kv", std::isnan(knobValue) ? juce::var() : juce::var(knobValue));
    sample->setProperty("pr", presence);
    sample->setProperty("ev", effectiveValue);
    sample->setProperty("hit", hit);
    return juce::var(sample.get());
}

inline juce::var makeParamHistoryParamEntryVar(const juce::String& name,
                                               juce::Array<juce::var> samples)
{
    juce::DynamicObject::Ptr entry = new juce::DynamicObject();
    entry->setProperty("name", name);
    entry->setProperty("history", juce::var(samples));
    return juce::var(entry.get());
}

inline juce::var makeParamHistoryModuleEntryVar(const juce::String& dslName,
                                                juce::Array<juce::var> params)
{
    juce::DynamicObject::Ptr entry = new juce::DynamicObject();
    entry->setProperty("dslName", dslName);
    entry->setProperty("params", juce::var(params));
    return juce::var(entry.get());
}

template <std::size_t Count>
inline juce::var makeBufferViewFloatArrayVar(const std::array<float, Count>& values)
{
    juce::Array<juce::var> out;
    out.ensureStorageAllocated(static_cast<int>(Count));
    for (const auto value : values)
        out.add(value);
    return juce::var(out);
}

template <std::size_t Count>
inline juce::var makeBufferViewEntryVar(int clipId,
                                        const juce::String& dslName,
                                        int capturedSamples,
                                        int recordingSamples,
                                        int recordingCapacitySamples,
                                        uint32_t generation,
                                        bool recording,
                                        bool playing,
                                        bool recordOverrun,
                                        bool assetBacked,
                                        bool warming,
                                        float playhead,
                                        float residentStart,
                                        float residentEnd,
                                        float requestedResidentStart,
                                        float requestedResidentEnd,
                                        const std::array<float, Count>& mins,
                                        const std::array<float, Count>& maxs,
                                        const std::array<float, Count>& recordingMins,
                                        const std::array<float, Count>& recordingMaxs,
                                        const std::array<float, Count>& resident,
                                        const std::array<float, Count>& requested,
                                        bool includeWaveform = true,
                                        uint32_t sequence = 0,
                                        uint32_t dropped = 0)
{
    juce::DynamicObject::Ptr buffer = new juce::DynamicObject();
    buffer->setProperty("clipId", clipId);
    buffer->setProperty("dslName", dslName);
    buffer->setProperty("capturedSamples", capturedSamples);
    buffer->setProperty("recordingSamples", recordingSamples);
    buffer->setProperty("recordingCapacitySamples", recordingCapacitySamples);
    buffer->setProperty("generation", static_cast<int>(generation));
    buffer->setProperty("recording", recording);
    buffer->setProperty("playing", playing);
    buffer->setProperty("recordOverrun", recordOverrun);
    buffer->setProperty("assetBacked", assetBacked);
    buffer->setProperty("warming", warming);
    buffer->setProperty("playhead", playhead);
    buffer->setProperty("residentStart", residentStart);
    buffer->setProperty("residentEnd", residentEnd);
    buffer->setProperty("requestedResidentStart", requestedResidentStart);
    buffer->setProperty("requestedResidentEnd", requestedResidentEnd);
    buffer->setProperty("incremental", ! includeWaveform);
    buffer->setProperty("sequence", static_cast<int>(sequence));
    buffer->setProperty("dropped", static_cast<int>(dropped));
    if (includeWaveform)
    {
        buffer->setProperty("mins", makeBufferViewFloatArrayVar(mins));
        buffer->setProperty("maxs", makeBufferViewFloatArrayVar(maxs));
        buffer->setProperty("recordingMins", makeBufferViewFloatArrayVar(recordingMins));
        buffer->setProperty("recordingMaxs", makeBufferViewFloatArrayVar(recordingMaxs));
        buffer->setProperty("resident", makeBufferViewFloatArrayVar(resident));
        buffer->setProperty("requested", makeBufferViewFloatArrayVar(requested));
    }
    return juce::var(buffer.get());
}

inline juce::String makeBufferViewJson(juce::Array<juce::var> buffers)
{
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("contractVersion", webview::contractVersion);
    payload->setProperty("type", "BUFFER_VIEW");
    payload->setProperty("buffers", juce::var(buffers));
    return juce::JSON::toString(juce::var(payload.get()), false);
}

} // namespace curlop::wire
