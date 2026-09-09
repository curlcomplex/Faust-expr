#include "graph/transport/FaustGraphRenderer.h"
#include "graph/transport/ControlRangeHandoff.h"

#include "graph/state/GraphState.h"
#include "graph/engine/nodes/ScriptNodeProcessor.h"
#include "graph/engine/nodes/VisualInputProcessor.h"
#include "control/vm/core/MidiOutputRuntime.h"
#include "graph/engine/nodes/HostAutomationInputProcessor.h"
#include "control/vm/core/HostMidiInputRuntime.h"
#include "control/vm/machine/TransportClockRuntime.h"
#include "control/surfaces/script/ScriptOutputSocketsV2.h"
#include "modules/backend/FaustControlInputRewrite.h"
#include "modules/backend/FaustRuntime.h"
#include "modules/backend/FaustSourceMetadata.h"
#include "modules/backend/FaustUiCapture.h"
#include "modules/contract/ModuleTypes.h"

#include <faust/dsp/libfaust-box.h>
#include <faust/dsp/poly-dsp.h>
#include <faust/gui/MapUI.h>
#include <faust/gui/UI.h>
#if JUCE_IOS
#include <faust/dsp/interpreter-dsp.h>
#else
#include <faust/dsp/llvm-dsp.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <list>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// mydsp_poly's architecture owns a process-wide GUI registration list.  The
// renderer is the product owner of that architecture, so define it exactly
// once here rather than leaving the physical voice pool test-only.
std::list<GUI*> GUI::fGuiList;

namespace curlop::transport {
namespace {

#if JUCE_IOS
using NativePolyEffectFactory = interpreter_dsp_factory;
#else
using NativePolyEffectFactory = llvm_dsp_factory;
#endif

class LibContext final {
public:
    LibContext() : lock_(FaustRuntime::compileMutex()) { createLibContext(); }
    ~LibContext() { destroyLibContext(); }

private:
    std::unique_lock<std::mutex> lock_;
};

using RendererBuildClock = std::chrono::steady_clock;

double rendererBuildElapsedUs(RendererBuildClock::time_point start)
{
    return std::chrono::duration<double, std::micro>(
        RendererBuildClock::now() - start).count();
}

class RendererBuildTimingScope final {
public:
    explicit RendererBuildTimingScope(FaustGraphRendererBuild& build)
        : build_(build), start_(RendererBuildClock::now()) {}

    ~RendererBuildTimingScope()
    {
        build_.timing.totalUs = rendererBuildElapsedUs(start_);
    }

private:
    FaustGraphRendererBuild& build_;
    RendererBuildClock::time_point start_;
};

std::vector<const char*> argumentPointers(
    const std::vector<std::string>& storage)
{
    std::vector<const char*> pointers;
    pointers.reserve(storage.size());
    for (const auto& value : storage)
        pointers.push_back(value.c_str());
    return pointers;
}

Box parallelWires(int channels)
{
    Box result = boxWire();
    for (int channel = 1; channel < std::max(1, channels); ++channel)
        result = boxPar(result, boxWire());
    return result;
}

Box parallelBoxes(const std::vector<Box>& boxes)
{
    Box result = boxes.front();
    for (std::size_t index = 1; index < boxes.size(); ++index)
        result = boxPar(result, boxes[index]);
    return result;
}

Box routeBox(int inputs, int outputs,
             const std::vector<std::pair<int, int>>& zeroBasedPairs);
Box parseBox(const std::string& name, const std::string& source,
             std::vector<const char*>& arguments, int expectedInputs,
             int expectedOutputs, std::string& error);

struct ProcessControlLayout {
    std::string source;
    std::vector<RewrittenControl> controls;
    // Faust's UI-schema order is not required to match source declaration
    // order. Keep the source order for the generated process inputs, and map
    // every one back to its canonical GraphState schema slot for VM binding.
    std::vector<std::size_t> parameterIndices;
    std::vector<int> rowStarts;
    std::vector<bool> perVoice;
    int expandedRows = 0;
    int meterOutputCount = 0;
};

bool prepareProcessControlLayout(const ModuleEntry& module,
                                 ProcessControlLayout& layout,
                                 std::string& error)
{
    const auto rewritten = rewriteControlsToProcessInputs(module.code);
    if (! rewritten.error.empty()) {
        error = "module " + module.moduleId
            + " control rewrite failed: " + rewritten.error;
        return false;
    }
    if (rewritten.controls.size() != module.params.size()) {
        error = "module " + module.moduleId + " declares "
            + std::to_string(rewritten.controls.size())
            + " process controls but its schema has "
            + std::to_string(module.params.size());
        return false;
    }
    layout.source = rewritten.source;
    layout.controls = rewritten.controls;
    layout.meterOutputCount = faustMeterOutputMarkerCount(module.code);
    layout.parameterIndices.reserve(layout.controls.size());
    layout.rowStarts.reserve(layout.controls.size());
    layout.perVoice.reserve(layout.controls.size());
    std::vector<bool> matched(module.params.size(), false);
    const int voices = std::max(1, module.physicalVoices);
    for (std::size_t control = 0; control < layout.controls.size(); ++control) {
        const auto& rewrittenControl = layout.controls[control];
        const auto expectedName = faustLabelToParamName(rewrittenControl.label);
        const auto parameter = std::find_if(
            module.params.begin(), module.params.end(),
            [&] (const auto& candidate) {
                return faustLabelToParamName(candidate.name) == expectedName;
            });
        if (parameter == module.params.end()) {
            error = "module " + module.moduleId + " control "
                + rewrittenControl.label + " is absent from its schema";
            return false;
        }
        const auto parameterIndex = static_cast<std::size_t>(
            std::distance(module.params.begin(), parameter));
        if (matched[parameterIndex]) {
            error = "module " + module.moduleId + " rewrites schema parameter "
                + parameter->name + " more than once";
            return false;
        }
        matched[parameterIndex] = true;
        layout.parameterIndices.push_back(parameterIndex);
        const bool perVoice = rewrittenControl.role == vm::SignalType::Gate
            || rewrittenControl.role == vm::SignalType::Pitch
            || rewrittenControl.role == vm::SignalType::Velocity;
        layout.rowStarts.push_back(layout.expandedRows);
        layout.perVoice.push_back(perVoice);
        layout.expandedRows += perVoice ? voices : 1;
    }
    return true;
}

Box processInputVoicePoolBox(const std::string& moduleId, Box moduleBox,
                             int baseControls, int audioInputs, int outputs,
                             int voices, const ProcessControlLayout& layout,
                             std::vector<const char*>& arguments,
                             std::string& error)
{
    if (voices <= 1)
        return boxVGroup(moduleId.c_str(), moduleBox);

    std::ostringstream activeCode;
    activeCode << "active = hslider(\"curlop_voice_active\", 1, 0, 1, 1);\n"
                  "process = ";
    for (int channel = 0; channel < outputs; ++channel) {
        if (channel != 0)
            activeCode << ", ";
        activeCode << "_ * active";
    }
    activeCode << ";\n";
    auto activeBox = parseBox(moduleId + "-voice-active", activeCode.str(),
                              arguments, outputs, outputs, error);
    if (activeBox == nullptr)
        return nullptr;

    std::vector<Box> voiceBoxes;
    voiceBoxes.reserve(static_cast<std::size_t>(voices));
    for (int voice = 0; voice < voices; ++voice) {
        auto voiceBox = boxSeq(moduleBox, activeBox);
        const auto voiceGroup = "voice-" + std::to_string(voice);
        voiceBoxes.push_back(boxVGroup(voiceGroup.c_str(), voiceBox));
    }
    Box pool = parallelBoxes(voiceBoxes);
    const int moduleInputs = baseControls + audioInputs;
    const int poolInputs = layout.expandedRows + audioInputs;
    std::vector<std::pair<int, int>> distribute;
    distribute.reserve(static_cast<std::size_t>(moduleInputs * voices));
    for (int voice = 0; voice < voices; ++voice) {
        for (int control = 0; control < baseControls; ++control) {
            const int source = layout.rowStarts[static_cast<std::size_t>(control)]
                + (layout.perVoice[static_cast<std::size_t>(control)] ? voice : 0);
            distribute.emplace_back(source, voice * moduleInputs + control);
        }
        for (int audio = 0; audio < audioInputs; ++audio)
            distribute.emplace_back(layout.expandedRows + audio,
                                    voice * moduleInputs + baseControls + audio);
    }
    pool = boxSeq(routeBox(poolInputs, moduleInputs * voices, distribute), pool);

    std::vector<std::pair<int, int>> sumOutputs;
    sumOutputs.reserve(static_cast<std::size_t>(outputs * voices));
    for (int voice = 0; voice < voices; ++voice)
        for (int output = 0; output < outputs; ++output)
            sumOutputs.emplace_back(voice * outputs + output, output);
    pool = boxSeq(pool, routeBox(outputs * voices, outputs, sumOutputs));
    return boxVGroup(moduleId.c_str(), pool);
}

// Automatic multi-mono is not voice polyphony: it keeps one independent mono
// DSP state per incoming channel, while every authored control is shared.
Box processInputIndependentMonoBox(const std::string& moduleId, Box moduleBox,
                                   int baseControls, int lanes)
{
    if (lanes <= 1)
        return boxVGroup(moduleId.c_str(), moduleBox);

    const int moduleInputs = baseControls + 1;
    std::vector<Box> laneBoxes;
    laneBoxes.reserve(static_cast<std::size_t>(lanes));
    for (int lane = 0; lane < lanes; ++lane) {
        const auto laneName = "lane-" + std::to_string(lane);
        laneBoxes.push_back(boxVGroup(laneName.c_str(), moduleBox));
    }

    std::vector<std::pair<int, int>> distribute;
    distribute.reserve(static_cast<std::size_t>(lanes * moduleInputs));
    for (int lane = 0; lane < lanes; ++lane) {
        for (int control = 0; control < baseControls; ++control)
            distribute.emplace_back(control, lane * moduleInputs + control);
        distribute.emplace_back(baseControls + lane,
                                lane * moduleInputs + baseControls);
    }
    auto lifted = boxSeq(
        routeBox(baseControls + lanes, moduleInputs * lanes, distribute),
        parallelBoxes(laneBoxes));
    return boxVGroup(moduleId.c_str(), lifted);
}

Box routeBox(int inputs, int outputs,
             const std::vector<std::pair<int, int>>& zeroBasedPairs)
{
    Box routes = nullptr;
    for (const auto& [input, output] : zeroBasedPairs) {
        auto pair = boxPar(boxInt(input + 1), boxInt(output + 1));
        routes = routes == nullptr ? pair : boxPar(routes, pair);
    }
    return boxRoute(boxInt(inputs), boxInt(outputs), routes);
}

Box parseBox(const std::string& name, const std::string& source,
             std::vector<const char*>& arguments, int expectedInputs,
             int expectedOutputs, std::string& error)
{
    int inputs = 0;
    int outputs = 0;
    auto box = DSPToBoxes(name, source, static_cast<int>(arguments.size()),
                          arguments.data(), &inputs, &outputs, error);
    if (box != nullptr
        && ((expectedInputs >= 0 && inputs != expectedInputs)
            || (expectedOutputs >= 0 && outputs != expectedOutputs))) {
        error = name + " has " + std::to_string(inputs) + " inputs and "
            + std::to_string(outputs) + " outputs; expected "
            + std::to_string(expectedInputs) + "/"
            + std::to_string(expectedOutputs);
        return nullptr;
    }
    return box;
}

class ZonePathCaptureUI final : public ::UI {
public:
    struct Zone {
        std::string path;
        std::string label;
        FAUSTFLOAT* value = nullptr;
    };

    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone,
                             FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT,
                             FAUSTFLOAT) override
    { add(label, zone); }
    void addVerticalSlider(const char* label, FAUSTFLOAT* zone,
                           FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT,
                           FAUSTFLOAT) override
    { add(label, zone); }
    void addNumEntry(const char* label, FAUSTFLOAT* zone,
                     FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT,
                     FAUSTFLOAT) override
    { add(label, zone); }
    void addButton(const char* label, FAUSTFLOAT* zone) override
    { add(label, zone); }
    void addCheckButton(const char* label, FAUSTFLOAT* zone) override
    { add(label, zone); }
    void addHorizontalBargraph(const char* label, FAUSTFLOAT* zone,
                               FAUSTFLOAT, FAUSTFLOAT) override
    { addMeter(label, zone); }
    void addVerticalBargraph(const char* label, FAUSTFLOAT* zone,
                             FAUSTFLOAT, FAUSTFLOAT) override
    { addMeter(label, zone); }
    void openTabBox(const char* label) override { open(label); }
    void openHorizontalBox(const char* label) override { open(label); }
    void openVerticalBox(const char* label) override { open(label); }
    void closeBox() override
    {
        if (! groups_.empty())
            groups_.pop_back();
    }
    void declare(FAUSTFLOAT*, const char*, const char*) override {}
    void addSoundfile(const char*, const char*, Soundfile**) override {}

    const std::vector<Zone>& zones() const noexcept { return zones_; }
    const std::vector<Zone>& meters() const noexcept { return meters_; }

private:
    void open(const char* label)
    {
        groups_.emplace_back(label != nullptr ? label : "");
    }

    void add(const char* label, FAUSTFLOAT* zone)
    {
        std::string path;
        for (const auto& group : groups_)
            path += "/" + group;
        const std::string name = label != nullptr ? label : "";
        path += "/" + name;
        zones_.push_back({ std::move(path), name, zone });
    }

    void addMeter(const char* label, FAUSTFLOAT* zone)
    {
        std::string path;
        for (const auto& group : groups_)
            path += "/" + group;
        // libfaust keeps bracket metadata in this generated graph UI path,
        // while FaustUiCapture consumes it before producing the faceplate ID.
        // Compare the visible label on both sides so meterout remains a
        // faceplate meter rather than an unresolvable trailing audio channel.
        std::string name = label != nullptr ? label : "";
        if (const auto metadata = name.find('['); metadata != std::string::npos)
            name.resize(metadata);
        path += "/" + name;
        meters_.push_back({ std::move(path), name, zone });
    }

    std::vector<std::string> groups_;
    std::vector<Zone> zones_;
    std::vector<Zone> meters_;
};

std::string wireCode(int width, float gain, float pan, bool panApplicable)
{
    const auto clampedGain = std::clamp(gain, 0.0f, 2.0f);
    const auto clampedPan = std::clamp(pan, -1.0f, 1.0f);
    std::ostringstream stream;
    stream << std::setprecision(9) << "process = ";
    if (panApplicable) {
        const float angle = (clampedPan + 1.0f) * 0.25f
            * 3.14159265358979323846f;
        stream << "_ * " << std::cos(angle) * clampedGain
               << ", _ * " << std::sin(angle) * clampedGain;
    } else {
        for (int channel = 0; channel < width; ++channel) {
            if (channel != 0)
                stream << ", ";
            stream << "_ * " << clampedGain;
        }
    }
    stream << ";\n";
    return stream.str();
}

struct ControlEdgeTransform {
    float gain = 1.0f;
    bool setValue = false;
    bool pitchSignalToHz = false;
    std::optional<ModulationMapping> mapping;
};

std::string parameterPositionCode(const ParamSchemaEntry& parameter)
{
    const auto target = parameter.valueDeclaration();
    std::ostringstream code;
    code << std::setprecision(17);
    if (target.max <= target.min) {
        code << "encode(x) = 0.5; decodeRaw(x) = " << target.min << ";\n";
    } else if (target.scale == Scale::Logarithmic && target.min > 0) {
        code << "encode(x) = log(max(" << target.min << ", x) / " << target.min
             << ") / log(" << target.max / target.min << ");\n"
             << "decodeRaw(x) = " << target.min << " * pow(" << target.max / target.min
             << ", clamp01(x));\n";
    } else {
        code << "encode(x) = (x - " << target.min << ") / " << target.max - target.min
             << ";\ndecodeRaw(x) = " << target.min << " + clamp01(x) * "
             << target.max - target.min << ";\n";
    }
    if (parameter.type == ParamType::Integer || parameter.type == ParamType::Boolean)
        code << "decode(x) = min(" << target.max << ", max(" << target.min
             << ", floor(decodeRaw(x) + 0.5)));\n";
    else
        code << "decode(x) = decodeRaw(x);\n";
    return code.str();
}

std::string controlMergeCode(const ParamSchemaEntry& parameter,
                             const std::vector<ControlEdgeTransform>& edges)
{
    std::ostringstream stream;
    stream << std::setprecision(9)
           << "clamp01(x) = min(1.0, max(0.0, x));\n";
    const bool mapped = std::any_of(edges.begin(), edges.end(),
        [] (const auto& edge) { return edge.mapping.has_value(); });
    if (mapped)
        stream << parameterPositionCode(parameter);
    if (parameter.scale == Scale::Logarithmic
        && parameter.min > 0.0f && parameter.max > parameter.min) {
        stream << "offset(x) = (abs(x) > 0.0000001) * "
               << parameter.min << " * pow(" << parameter.max / parameter.min
               << ", clamp01(x));\n";
    } else {
        stream << "offset(x) = (abs(x) > 0.0000001) * ("
               << parameter.min << " + clamp01(x) * "
               << (parameter.max - parameter.min) << ");\n";
    }
    stream << "pitchHz(x) = 440.0 * pow(2.0, 10.0 * x - 0.75);\n"
              "process(base";
    for (std::size_t edge = 0; edge < edges.size(); ++edge)
        stream << ", c" << edge;
    stream << ") = ";
    if (mapped)
        stream << "decode(encode(";
    bool hasSet = false;
    for (const auto& edge : edges)
        hasSet = hasSet || edge.setValue;
    if (! hasSet) {
        stream << "base";
    } else {
        bool emittedSet = false;
        for (std::size_t edge = 0; edge < edges.size(); ++edge) {
            if (! edges[edge].setValue)
                continue;
            if (emittedSet)
                stream << " + ";
            const auto value = "(c" + std::to_string(edge) + " * "
                + [&] {
                    std::ostringstream gain;
                    gain << std::setprecision(9) << edges[edge].gain;
                    return gain.str();
                }() + ")";
            stream << (edges[edge].pitchSignalToHz
                ? "pitchHz(" + value + ")" : value);
            emittedSet = true;
        }
    }
    for (std::size_t edge = 0; edge < edges.size(); ++edge) {
        if (edges[edge].setValue || edges[edge].mapping)
            continue;
        stream << " + offset(c" << edge << " * " << edges[edge].gain << ")";
    }
    if (mapped) {
        stream << ")";
        for (std::size_t edge = 0; edge < edges.size(); ++edge) {
            if (! edges[edge].mapping)
                continue;
            const auto& mapping = *edges[edge].mapping;
            const float minimum = mapping.polarity == ModulationPolarity::Bipolar ? -1.0f : 0.0f;
            stream << " + " << mapping.depth << " * min(1.0, max(" << minimum
                   << ", c" << edge << "))";
        }
        stream << ")";
    }
    stream << ";\n";
    return stream.str();
}

struct ScriptRouteModule {
    std::vector<ScriptV2SocketRoute> routes;
    std::unordered_set<std::string> inputs;
    std::unordered_set<std::string> outputs;
};

bool scriptRouteUsesInput(const ScriptRouteModule& module,
                          const std::string& input)
{
    return module.inputs.count(input) != 0;
}

bool scriptRouteProducesOutput(const ScriptRouteModule& module,
                               const std::string& output)
{
    return module.outputs.count(output) != 0;
}

bool scriptRouteProgramIsPure(const std::string& source)
{
    const auto program = script::v2::parseProgramV2(juce::String(source));
    return std::all_of(
        program.statements.begin(), program.statements.end(),
        [](const auto& statement) {
            return statement.kind == script::v2::StatementKind::SocketRoute;
        });
}

std::string routeScalar(float value)
{
    std::ostringstream stream;
    stream << std::setprecision(9) << value;
    return stream.str();
}

std::string staticLfoCode(const ScriptNodeProcessor::V2StaticLfo& lfo)
{
    const auto rate = routeScalar(lfo.rateHz);
    std::string waveform;
    if (lfo.waveform == "tri" || lfo.waveform == "triangle")
        waveform = "os.lf_trianglepos(" + rate + ")";
    else if (lfo.waveform == "saw")
        waveform = "os.lf_sawpos(" + rate + ")";
    else if (lfo.waveform == "square")
        waveform = "os.lf_squarewavepos(" + rate + ")";
    else
        // Script V2's existing evaluator intentionally treats unrecognised
        // waveform names as sine, so preserve that established contract.
        waveform = "((os.m_oscsin(" + rate + ") * 0.5) + 0.5)";
    return "((" + routeScalar(lfo.low) + ") + ((" + waveform + ") * ("
        + routeScalar(lfo.high) + " - " + routeScalar(lfo.low) + ")))";
}

bool applyRouteTransform(std::string value,
                         ScriptNodeProcessor::V2RouteOp::Kind kind,
                         int argCount,
                         const std::string& a,
                         const std::string& b,
                         std::string& expression)
{
    switch (kind) {
        case ScriptNodeProcessor::V2RouteOp::Kind::Invert: {
            const auto pivot = argCount > 0 ? a : "0.5";
            expression = "(2.0 * (" + pivot + ") - (" + value + "))";
            return true;
        }
        case ScriptNodeProcessor::V2RouteOp::Kind::Clip:
            expression = "min(max(" + a + ", " + b + "), max(min("
                + a + ", " + b + "), (" + value + ")))";
            return true;
        case ScriptNodeProcessor::V2RouteOp::Kind::Scale:
            expression = "((" + a + ") + (" + value + ") * (("
                + b + ") - (" + a + ")))";
            return true;
        case ScriptNodeProcessor::V2RouteOp::Kind::Gain:
            expression = "((" + value + ") * (" + a + "))";
            return true;
        case ScriptNodeProcessor::V2RouteOp::Kind::Offset:
            expression = "((" + value + ") + (" + a + "))";
            return true;
        case ScriptNodeProcessor::V2RouteOp::Kind::Abs:
            expression = "abs(" + value + ")";
            return true;
        case ScriptNodeProcessor::V2RouteOp::Kind::Smooth:
            return false;
    }
    return false;
}

std::optional<ScriptNodeProcessor::V2RouteOp::Kind> scriptRouteTransformKind(
    const std::string& name)
{
    using Kind = ScriptNodeProcessor::V2RouteOp::Kind;
    if (name == "invert") return Kind::Invert;
    if (name == "clip") return Kind::Clip;
    if (name == "scale") return Kind::Scale;
    if (name == "gain") return Kind::Gain;
    if (name == "offset") return Kind::Offset;
    if (name == "abs") return Kind::Abs;
    if (name == "smooth") return Kind::Smooth;
    return std::nullopt;
}

std::optional<std::string> scriptRouteArgumentCode(
    const ScriptV2SocketRouteOp::Arg& argument)
{
    if (! argument.dynamic)
        return routeScalar(argument.value);

    ScriptNodeProcessor::V2StaticRouteArgument parsed;
    if (! ScriptNodeProcessor::parseV2StaticRouteArgument(argument.source, parsed))
        return std::nullopt;
    std::string expression = staticLfoCode(parsed.lfo);
    for (const auto& transform : parsed.transforms)
        if (! applyRouteTransform(expression, transform.kind, transform.argCount,
                                  routeScalar(transform.a),
                                  routeScalar(transform.b), expression))
            return std::nullopt;
    return expression;
}

bool scriptRouteHasUnsupportedDynamicArguments(
    const std::vector<ScriptV2SocketRoute>& routes)
{
    for (const auto& route : routes)
        for (const auto& op : route.ops) {
            if (! op.argA.dynamic && ! op.argB.dynamic)
                continue;
            // A time-varying smoothing duration needs a separately specified
            // Faust timing contract; leave it as an actionable refusal here.
            if (op.name == "smooth")
                return true;
            if ((op.argA.dynamic && ! scriptRouteArgumentCode(op.argA))
                || (op.argB.dynamic && ! scriptRouteArgumentCode(op.argB)))
                return true;
        }
    return false;
}

bool applyStaticScriptRouteOp(std::string value,
                              const ScriptV2SocketRouteOp& op,
                              std::string& expression)
{
    const auto a = scriptRouteArgumentCode(op.argA);
    const auto b = scriptRouteArgumentCode(op.argB);
    if (! a || ! b)
        return false;
    const auto kind = scriptRouteTransformKind(op.name);
    if (! kind)
        return false;
    if (*kind == ScriptNodeProcessor::V2RouteOp::Kind::Smooth) {
        // Keep the zero-duration Script V2 contract immediate. A positive
        // fixed duration belongs inside the compiled Faust graph, using its
        // standard exponential one-pole rather than a native route processor.
        expression = op.a <= 0.0f
            ? value
            : "((" + value + ") : si.smooth(ba.tau2pole(" + *a + ")))";
    }
    if (*kind == ScriptNodeProcessor::V2RouteOp::Kind::Smooth)
        return true;
    return applyRouteTransform(value, *kind, op.argCount, *a, *b, expression);
}

bool scriptRouteCode(const ModuleEntry& module,
                     const ScriptRouteModule& routeModule,
                     std::string& code,
                     std::string& error)
{
    std::unordered_map<std::string, int> inputByName;
    for (int input = 0;
         input < static_cast<int>(module.controlInputs.size()); ++input)
        inputByName[module.controlInputs[static_cast<std::size_t>(input)]] = input;

    std::unordered_map<std::string, int> outputByName;
    for (int output = 0;
         output < static_cast<int>(module.controlOutputs.size()); ++output)
        outputByName[module.controlOutputs[static_cast<std::size_t>(output)]] = output;

    std::vector<std::string> expressions;
    expressions.reserve(module.controlOutputs.size());
    for (std::size_t output = 0;
         output < module.controlOutputs.size(); ++output) {
        const bool isGate = output < module.controlOutputTypes.size()
            && module.controlOutputTypes[output] == vm::SignalType::Gate;
        expressions.push_back(routeScalar(isGate ? 0.0f
            : scriptSocketDefaultValueFromName(
                module.controlOutputs[output])));
    }
    for (const auto& route : routeModule.routes) {
        const auto input = inputByName.find(route.input);
        const auto output = outputByName.find(route.output);
        if (input == inputByName.end() || output == outputByName.end()) {
            error = "Script V2 route " + module.moduleId + "." + route.output
                + " does not match its prepared input/output socket contract";
            return false;
        }
        std::string expression = "in" + std::to_string(input->second);
        for (const auto& op : route.ops) {
            if (! applyStaticScriptRouteOp(expression, op, expression)) {
                error = "Script V2 route " + module.moduleId + "." + route.output
                    + " cannot lower transform " + op.name;
                return false;
            }
        }
        expressions[static_cast<std::size_t>(output->second)] =
            "finiteOrZero(" + expression + ")";
    }

    std::ostringstream stream;
    stream << "import(\"stdfaust.lib\");\n";
    stream << "finiteOrZero(x) = ba.if(abs(safe) > ma.MAX, 0.0, safe) "
              "with { safe = max(0.0, x) + min(0.0, x); };\n";
    stream << "process(";
    for (std::size_t input = 0; input < module.controlInputs.size(); ++input) {
        if (input != 0) stream << ", ";
        stream << "in" << input;
    }
    stream << ") = ";
    for (std::size_t output = 0; output < expressions.size(); ++output) {
        if (output != 0) stream << ", ";
        stream << expressions[output];
    }
    stream << ";\n";
    code = stream.str();
    return true;
}

bool isOutput(const ModuleEntry& module)
{
    return module.lineageId == "core.output" || module.dslName == "output";
}

bool isFaust(const ModuleEntry& module)
{
    return module.lineageId == "core.faust_jit";
}

bool isAudioInputBoundary(const ModuleEntry& module)
{
    return module.lineageId == "core.audio_input";
}

bool isRenderDomainBoundary(const ModuleEntry& module)
{
    return module.lineageId == "core.visual_shader"
        || module.lineageId == "core.visual_output";
}

bool isMidiEventBoundary(const ModuleEntry& module)
{
    return module.lineageId == "core.midi_out";
}

bool isHostAutomationSource(const ModuleEntry& module)
{
    return module.lineageId == "core.host_automation";
}

bool isHostMidiSource(const ModuleEntry& module)
{
    return module.lineageId == "core.midi_in";
}

bool isTransportClockSource(const ModuleEntry& module)
{
    return module.lineageId == "core.transport_clock";
}

bool isVmControlSource(const ModuleEntry& module)
{
    return module.lineageId == "core.script_v2"
        || module.lineageId == "core.stepseq"
        || module.lineageId == "core.pitch_seq"
        || module.lineageId == "core.trigger_seq";
}

bool isPreparedStepSource(const ModuleEntry& module)
{
    return module.lineageId == "core.stepseq"
        || module.lineageId == "core.pitch_seq"
        || module.lineageId == "core.trigger_seq";
}

bool isNonAudioBoundary(const ModuleEntry& module)
{
    return isRenderDomainBoundary(module) || isMidiEventBoundary(module)
        || isVmControlSource(module);
}

int namedControlOutputIndex(const ModuleEntry& module,
                            const std::string& port)
{
    const auto found = std::find(module.controlOutputs.begin(),
                                 module.controlOutputs.end(), port);
    if (found != module.controlOutputs.end())
        return static_cast<int>(std::distance(module.controlOutputs.begin(), found));

    // Older projects did not persist the fixed control-output declaration for
    // prepared step sequencers. Their gate/pitch/velocity contract is a
    // lineage invariant, not authored module metadata, so restore it here
    // rather than rejecting otherwise valid legacy control wires.
    if (isPreparedStepSource(module)) {
        static constexpr std::array<std::string_view, 3> kOutputs {
            "gate", "pitch", "velocity"
        };
        const auto canonical = std::find(kOutputs.begin(), kOutputs.end(), port);
        if (canonical != kOutputs.end())
            return static_cast<int>(std::distance(kOutputs.begin(), canonical));
    }
    return -1;
}

int exposedControlInputIndex(const ModuleEntry& module,
                             const std::string& port)
{
    if (std::find(module.exposedParamInputs.begin(),
                  module.exposedParamInputs.end(), port)
        == module.exposedParamInputs.end())
        return -1;
    for (int parameter = 0;
         parameter < static_cast<int>(module.params.size()); ++parameter)
        if (module.params[static_cast<std::size_t>(parameter)].name == port
            || faustLabelToParamName(
                   module.params[static_cast<std::size_t>(parameter)].name)
                == faustLabelToParamName(port))
            return parameter;
    return -1;
}

int vmControlInputIndex(const ModuleEntry& module,
                        const std::string& port)
{
    const auto found = std::find(
        module.controlInputs.begin(), module.controlInputs.end(), port);
    return found == module.controlInputs.end()
        ? -1 : static_cast<int>(std::distance(
            module.controlInputs.begin(), found));
}

int preparedVmInputIndex(const ModuleEntry& module,
                         const std::string& port)
{
    return isPreparedStepSource(module)
        ? exposedControlInputIndex(module, port)
        : vmControlInputIndex(module, port);
}

bool isNamedVmInputEdge(const ModuleEntry& source,
                        const ModuleEntry& destination,
                        const EdgeEffective& edge)
{
    return (destination.lineageId == "core.script_v2"
            || isPreparedStepSource(destination))
        && namedControlOutputIndex(source, edge.srcPort) >= 0
        && preparedVmInputIndex(destination, edge.tgtPort) >= 0;
}

bool isNamedControlEdge(const ModuleEntry& source,
                        const ModuleEntry& destination,
                        const EdgeEffective& edge)
{
    if (edge.modulation)
        return resolveModulationMapping(source, destination, *edge.modulation).has_value();
    return namedControlOutputIndex(source, edge.srcPort) >= 0
        && exposedControlInputIndex(destination, edge.tgtPort) >= 0;
}

struct GraphPlan {
    std::vector<const ModuleEntry*> modules;
    std::unordered_map<int, std::vector<EdgeEffective>> incoming;
    std::unordered_map<int, std::vector<EdgeEffective>> renderIncoming;
    std::unordered_map<int, std::vector<EdgeEffective>> eventIncoming;
    std::unordered_map<int, std::vector<EdgeEffective>> vmInputIncoming;
    std::unordered_map<int, ScriptRouteModule> scriptRoutes;
    std::vector<EdgeEffective> feedbackEdges;
    std::string diagnostic;
    bool rejectPublication = false;
};

GraphPlan prepareGraphPlan(const GraphState& state)
{
    GraphPlan result;
    if (state.modules().empty()) {
        result.diagnostic = "graph has no modules";
        return result;
    }

    std::unordered_map<int, const ModuleEntry*> byIndex;
    std::unordered_map<int, int> indegree;
    std::unordered_map<int, std::vector<int>> outgoing;
    const ModuleEntry* output = nullptr;
    std::size_t audioDomainModuleCount = 0;
    for (const auto& module : state.modules()) {
        if (isTc1ExcludedFromAuthoring(module.lineageId)) {
            result.diagnostic = tc1ExclusionDiagnosticForLineage(
                module.lineageId);
            result.rejectPublication = true;
            return result;
        }
        if (module.lineageId == "core.script_v2"
            && ! module.controlInputs.empty()) {
            std::vector<ScriptV2SocketRoute> routes;
            if (! collectScriptSocketRoutesV2(module.code, routes)) {
                result.diagnostic = "Script V2 input route contains an "
                    "unsupported current-block transform";
                result.rejectPublication = true;
                return result;
            }
            if (! routes.empty()) {
                if (! scriptRouteProgramIsPure(module.code)) {
                    result.diagnostic = "mixed VM and current-block Script V2 "
                        "route programs require explicit unified output ownership";
                    result.rejectPublication = true;
                    return result;
                }
                if (scriptRouteHasUnsupportedDynamicArguments(routes)) {
                    result.diagnostic = "stateful or dynamic Script V2 routes "
                        "require the APG-free publication cutover";
                    result.rejectPublication = true;
                    return result;
                }
                ScriptRouteModule routeModule;
                routeModule.routes = std::move(routes);
                for (const auto& route : routeModule.routes) {
                    routeModule.inputs.insert(route.input);
                    routeModule.outputs.insert(route.output);
                }
                result.scriptRoutes.emplace(module.index,
                                            std::move(routeModule));
            }
        }
        if (isVmControlSource(module) && ! module.controlInputs.empty()) {
            if (module.lineageId != "core.script_v2") {
                if (! isPreparedStepSource(module)) {
                    result.diagnostic = "input-bearing legacy Script requires migration to a prepared VM input route";
                    result.rejectPublication = true;
                    return result;
                }
            }
        }
        if (! byIndex.emplace(module.index, &module).second) {
            result.diagnostic = "graph has duplicate module indices";
            return result;
        }
        if (! isNonAudioBoundary(module)
            || result.scriptRoutes.count(module.index) != 0) {
            indegree[module.index] = 0;
            ++audioDomainModuleCount;
        }
        if (isOutput(module)) {
            if (output != nullptr) {
                result.diagnostic = "graph has more than one output module";
                return result;
            }
            output = &module;
        } else if (! isFaust(module)
                   && ! isAudioInputBoundary(module)
                   && ! isHostAutomationSource(module)
                   && ! isHostMidiSource(module)
                   && ! isTransportClockSource(module)
                   && ! isNonAudioBoundary(module)) {
            result.diagnostic = "module " + module.moduleId
                + " is not yet eligible for the unified Faust renderer";
            return result;
        }
    }
    if (output == nullptr) {
        result.diagnostic = "graph has no core.output module";
        return result;
    }

    for (const auto& edge : state.computeEffectiveEdges()) {
        if (byIndex.count(edge.srcIndex) == 0 || byIndex.count(edge.tgtIndex) == 0) {
            result.diagnostic = "edge refers to an unknown module index";
            return result;
        }
        const auto* source = byIndex.at(edge.srcIndex);
        const auto* destination = byIndex.at(edge.tgtIndex);
        if (edge.modulation
            && (! isFaust(*destination)
                || ! resolveModulationMapping(*source, *destination, *edge.modulation))) {
            result.diagnostic = "parameter mapping does not resolve to an accepted Faust target";
            return result;
        }
        if (isMidiEventBoundary(*source)) {
            result.diagnostic = "MIDI output boundary " + source->moduleId
                + " cannot feed the unified graph";
            return result;
        }
        if (isRenderDomainBoundary(*source)) {
            if (edge.signalDescriptor.semanticRole() == "visual-texture"
                && isRenderDomainBoundary(*destination))
                continue;
            result.diagnostic = "render-domain module " + source->moduleId
                + " cannot feed the unified audio/control graph";
            return result;
        }
        const bool primarySourcePort = edge.srcPort.empty()
            || (! source->audioOutputs.empty()
                && edge.srcPort == source->audioOutputs.front());
        // `core.output` has one fixed audio input named IN. Older persisted
        // projects predate socket persistence for that built-in boundary, so
        // their otherwise canonical OUT -> IN cable must not be reclassified
        // as an unsupported named-control edge merely because audioInputs is
        // absent from the saved Output entry.
        const bool primaryDestinationPort = edge.tgtPort.empty()
            || (isOutput(*destination) && edge.tgtPort == "IN")
            || (! destination->audioInputs.empty()
                && edge.tgtPort == destination->audioInputs.front());
        const auto sourceRoutes = result.scriptRoutes.find(source->index);
        const auto destinationRoutes = result.scriptRoutes.find(
            destination->index);
        const bool routeInputEdge = destinationRoutes != result.scriptRoutes.end()
            && scriptRouteUsesInput(destinationRoutes->second, edge.tgtPort)
            && namedControlOutputIndex(*source, edge.srcPort) >= 0;
        const bool routeOutputEdge = sourceRoutes != result.scriptRoutes.end()
            && scriptRouteProducesOutput(sourceRoutes->second, edge.srcPort);
        const bool vmInputEdge = ! routeInputEdge && isNamedVmInputEdge(
            *source, *destination, edge);
        const bool controlEdge = isNamedControlEdge(
            *source, *destination, edge) || vmInputEdge || routeInputEdge;
        if ((! primarySourcePort || ! primaryDestinationPort) && ! controlEdge) {
            CDBG(GRAPH_SYNC,
                 "unified Faust renderer unresolved named edge: source=%s (%s) port=%s "
                 "destination=%s (%s) port=%s primarySource=%d primaryDestination=%d "
                 "namedControlOutput=%d exposedControlInput=%d vmInput=%d routeInput=%d",
                 source->moduleId.c_str(), source->lineageId.c_str(), edge.srcPort.c_str(),
                 destination->moduleId.c_str(), destination->lineageId.c_str(), edge.tgtPort.c_str(),
                 primarySourcePort ? 1 : 0, primaryDestinationPort ? 1 : 0,
                 namedControlOutputIndex(*source, edge.srcPort) >= 0 ? 1 : 0,
                 exposedControlInputIndex(*destination, edge.tgtPort) >= 0 ? 1 : 0,
                 vmInputEdge ? 1 : 0, routeInputEdge ? 1 : 0);
            result.diagnostic =
                "named edge does not resolve to an audio or exposed-control contract";
            return result;
        }
        if (controlEdge
            && (edge.feedbackBoundary != FeedbackBoundary::None
                || edge.signalDescriptor.width() != 1)) {
            result.diagnostic = "named control edges require a scalar causal contract";
            return result;
        }
        if (routeInputEdge)
            result.vmInputIncoming[edge.tgtIndex].push_back(edge);
        // Packet-capable prepared routes retain their event/lifecycle identity
        // in the schedule and are delivered by the VM. They are not scalar
        // previous-row inputs to the Faust renderer; treating them as such
        // requires a non-existent renderer tap for a non-audio VM source.
        const bool packetVmInputEdge = vmInputEdge
            && edge.signalDescriptor.capabilities().packet;
        if (packetVmInputEdge)
            continue;
        if (vmInputEdge) {
            result.vmInputIncoming[edge.tgtIndex].push_back(edge);
            continue;
        }
        if (isVmControlSource(*source) && controlEdge && ! routeOutputEdge) {
            if (edge.modulation) {
                result.diagnostic = "VM-source parameter mapping requires prepared mapping delivery";
                return result;
            }
            // Script packets and scalar control streams are composed by the
            // VM's prepared Delivery into the destination module's existing
            // declaration rows. Those rows already enter Faust as process
            // inputs, so duplicating this graph edge inside the Faust box
            // would discard logical-voice identity and apply the value twice.
            continue;
        }
        if (isRenderDomainBoundary(*destination)) {
            if (edge.feedbackBoundary != FeedbackBoundary::None) {
                result.diagnostic =
                    "render-domain taps cannot own audio feedback";
                return result;
            }
            result.renderIncoming[edge.tgtIndex].push_back(edge);
            continue;
        }
        if (isMidiEventBoundary(*destination)) {
            if (! controlEdge
                || edge.feedbackBoundary != FeedbackBoundary::None) {
                result.diagnostic =
                    "MIDI output boundaries accept causal named controls only";
                return result;
            }
            result.eventIncoming[edge.tgtIndex].push_back(edge);
            continue;
        }
        result.incoming[edge.tgtIndex].push_back(edge);
        if (edge.feedbackBoundary == FeedbackBoundary::OneSample) {
            result.feedbackEdges.push_back(edge);
            continue;
        }
        outgoing[edge.srcIndex].push_back(edge.tgtIndex);
        ++indegree[edge.tgtIndex];
    }

    std::vector<int> ready;
    for (const auto& module : state.modules()) {
        if ((! isNonAudioBoundary(module)
             || result.scriptRoutes.count(module.index) != 0)
            && indegree[module.index] == 0)
            ready.push_back(module.index);
    }
    for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
        const int index = ready[cursor];
        result.modules.push_back(byIndex.at(index));
        for (const int destination : outgoing[index]) {
            if (--indegree[destination] == 0)
                ready.push_back(destination);
        }
    }
    if (result.modules.size() != audioDomainModuleCount) {
        result.diagnostic = "graph contains an undeclared cycle";
        return result;
    }
    return result;
}

class FaustGraphRenderer final : public PreparedAudioRenderer {
public:
    struct ControlRow {
        enum class Kind { Direct, VoiceCap, NativeVoiceCap, BlockLast };
        Kind kind = Kind::Direct;
        int rowMapIndex = -1;
        float fallbackValue = 0.0f;
        float nonFiniteValue = 0.0f;
        float minimumValue = 0.0f;
        float maximumValue = 0.0f;
        bool reportsOutputLevel = false;
        std::vector<FAUSTFLOAT*> zones;
        const float* samples = nullptr;
        int runEnd = 0;
        FAUSTFLOAT lastValue = 0.0f;
    };

    struct ModuleControls {
        int graphModuleIndex = -1;
        int sampleCount = 0;
        std::vector<ControlRow> rows;
    };

    struct ProcessControlInput {
        std::size_t module = 0;
        std::size_t row = 0;
        std::vector<FAUSTFLOAT> scratch;
        const FAUSTFLOAT* preparedSamples = nullptr;
        FAUSTFLOAT lastValue = 0.0f;

        static float encodeTransport(const ControlRow& row, float value) noexcept
        {
            const double range = static_cast<double>(row.maximumValue) - row.minimumValue;
            return range > 0
                ? static_cast<float>(std::clamp((static_cast<double>(value) - row.minimumValue) / range, 0.0, 1.0))
                : 0.0f;
        }
    };

    struct VisualControlEdge {
        std::size_t sourceTap = 0;
        std::size_t sourceOutput = 0;
        float gain = 1.0f;
        bool setValue = false;
        bool pitchSignalToHz = false;
    };

    struct VisualControlInput {
        ParamSchemaEntry parameter;
        std::vector<VisualControlEdge> edges;
        float heldBase = 0.0f;
    };

    struct VisualAudioEdge {
        std::size_t sourceTap = 0;
        std::array<float, 2> gains { 1.0f, 1.0f };
    };

    struct VisualAudioInput {
        std::vector<VisualAudioEdge> edges;
    };

    struct VisualBoundary {
        int graphModuleIndex = -1;
        std::size_t controlModule = 0;
        std::unique_ptr<VisualInputRuntime> runtime;
        std::vector<VisualControlInput> controls;
        std::vector<VisualAudioInput> audioInputs;
        std::vector<std::vector<float>> channels;
        std::vector<const float*> channelPointers;
    };

    struct MidiBoundary {
        int graphModuleIndex = -1;
        std::size_t controlModule = 0;
        MidiOutputRuntime* runtime = nullptr;
        std::vector<VisualControlInput> controls;
        std::vector<std::vector<float>> channels;
        std::vector<const float*> channelPointers;
    };

    struct AudioInputBoundary {
        int graphModuleIndex = -1;
        std::uint32_t channelIndex = 0;
        AudioInputRuntime* runtime = nullptr;
    };

    struct HostAutomationBoundary {
        int graphModuleIndex = -1;
        std::size_t controlModule = 0;
        const std::atomic<float>* sourceValues = nullptr;
        int sourceLaneCount = 0;
        std::vector<VisualControlInput> controls;
        std::vector<std::vector<float>> controlChannels;
        std::vector<const float*> controlPointers;
        std::vector<std::vector<float>> outputChannels;
        std::vector<float*> outputPointers;
    };

    struct HostMidiBoundary {
        int graphModuleIndex = -1;
        std::size_t controlModule = 0;
        HostMidiInputRuntime* runtime = nullptr;
        std::vector<VisualControlInput> controls;
        std::vector<std::vector<float>> controlChannels;
        juce::AudioBuffer<float> buffer;
    };

    struct VmControlOutputBoundary {
        int graphModuleIndex = -1;
        std::vector<const float*> rows;
        int sampleCount = 0;
    };

    struct VmInputEdge {
        std::size_t sourceTap = 0;
        std::size_t sourceOutput = 0;
        float gain = 1.0f;
    };

    struct VmInputBoundary {
        int graphModuleIndex = -1;
        std::vector<std::vector<VmInputEdge>> edges;
        std::vector<std::vector<float>> previousRows;
    };

    struct LevelHold {
        std::atomic<float> peak { 0.0f };
        std::atomic<float> rms { 0.0f };
    };

    struct InputEdge {
        std::size_t sourceTap = 0;
        std::vector<float> gains;
        bool oneSampleFeedback = false;
        std::vector<float> previousSamples;
    };

    struct InputMix {
        int width = 0;
        std::vector<InputEdge> edges;
    };

    struct FaceplateTelemetry {
        std::vector<std::string> ids;
        std::vector<FAUSTFLOAT*> zones;
        std::vector<bool> useTapSignal;
        std::unique_ptr<std::atomic<float>[]> values;
    };

    ~FaustGraphRenderer() override
    {
        nativePolyPool_.reset();
        nativePolyEffects_.clear();
        instance_.reset();
        if (! nativePolyEffectFactories_.empty()) {
            std::lock_guard<std::mutex> faustLock(
                FaustRuntime::compileMutex());
            for (auto* effectFactory : nativePolyEffectFactories_) {
                if (effectFactory == nullptr)
                    continue;
#if JUCE_IOS
                deleteInterpreterDSPFactory(effectFactory);
#else
                deleteDSPFactory(effectFactory);
#endif
            }
        }
        if (factory_ != nullptr) {
            std::lock_guard<std::mutex> faustLock(
                FaustRuntime::compileMutex());
#if JUCE_IOS
            deleteInterpreterDSPFactory(factory_);
#else
            deleteDSPFactory(factory_);
#endif
        }
    }

    bool processAudio(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer& midi) noexcept override
    {
        if (nativePolyPool_ != nullptr)
            return processNativePolyAudio(buffer, midi);
        if (instance_ == nullptr || buffer.getNumChannels() < 2
            || buffer.getNumSamples() > blockSize_) {
            CDBG_RT(AUDIO_TAP,
                    "unified Faust renderer rejected block instance=%d channels=%d samples=%d capacity=%d",
                    instance_ != nullptr ? 1 : 0, buffer.getNumChannels(),
                    buffer.getNumSamples(), blockSize_);
            clearControlBindings();
            return false;
        }
        if (blockRejected_) {
            CDBG_RT(AUDIO_TAP,
                    "unified Faust renderer rejected a previously invalidated block");
            blockRejected_ = false;
            clearControlBindings();
            return false;
        }
        const int count = buffer.getNumSamples();
        for (const auto& module : controls_)
            if (module.sampleCount != 0 && module.sampleCount != count) {
                CDBG_RT(AUDIO_TAP,
                        "unified Faust renderer rejected stale module controls samples=%d expected=%d",
                        module.sampleCount, count);
                clearControlBindings();
                return false;
            }
        for (const auto& boundary : vmControlOutputBoundaries_)
            if (boundary.sampleCount != count
                || std::any_of(boundary.rows.begin(), boundary.rows.end(),
                               [] (const float* row) {
                                   return row == nullptr;
                               })) {
                CDBG_RT(AUDIO_TAP,
                        "unified Faust renderer rejected incomplete VM boundary samples=%d expected=%d",
                        boundary.sampleCount, count);
                clearControlBindings();
                return false;
            }
        // Host-source runtimes are stateful. Advance them only after every
        // rejecting condition has been checked so APG fallback owns a failed
        // block exactly once and the next candidate block stays aligned.
        prepareHostAutomationSources(count);
        prepareHostMidiSources(count);
        // A Host MIDI plan hook may update the receiver's VOICES row. Recheck
        // after that hook: a reduced/dynamic cap still belongs to APG because
        // fused Faust computes every physical voice continuously. The shared
        // HostMidiInputRuntime retains this already-prepared block so fallback
        // consumes it exactly once without repeating the VM hook or planner.
        if (! voiceCapsUseFullPool(count)) {
            CDBG_RT(AUDIO_TAP,
                    "unified Faust renderer rejected reduced voice-cap block samples=%d",
                    count);
            clearControlBindings();
            return false;
        }
        for (auto& boundary : hostMidiBoundaries_)
            if (boundary.runtime != nullptr)
                boundary.runtime->commitPreparedCandidateBlock();
        for (auto& binding : processControlInputs_) {
            auto& row = controls_[binding.module].rows[binding.row];
            const auto* samples = row.samples;
            if (row.kind == ControlRow::Kind::BlockLast) {
                const float requested = samples != nullptr && count > 0
                    ? samples[count - 1] : row.fallbackValue;
                binding.lastValue = std::isfinite(requested)
                    ? std::clamp(requested, row.minimumValue, row.maximumValue)
                    : row.nonFiniteValue;
                std::fill_n(binding.scratch.data(), count,
                            ProcessControlInput::encodeTransport(row, binding.lastValue));
                binding.preparedSamples = binding.scratch.data();
                if (row.reportsOutputLevel)
                    outputLevel_ = binding.lastValue;
                continue;
            }
            for (int sample = 0; sample < count; ++sample) {
                if (samples != nullptr && std::isfinite(samples[sample]))
                    binding.lastValue = samples[sample];
                binding.scratch[static_cast<std::size_t>(sample)] =
                    ProcessControlInput::encodeTransport(row, binding.lastValue);
            }
            binding.preparedSamples = binding.scratch.data();
        }

        int offset = 0;
        while (offset < count) {
            int next = count;
            for (auto& module : controls_) {
                if (module.sampleCount == 0)
                    continue;
                for (auto& row : module.rows) {
                    if (row.zones.empty() || row.samples == nullptr)
                        continue;
                    float value = row.kind == ControlRow::Kind::BlockLast
                        ? (count > 0 ? row.samples[count - 1]
                                     : row.fallbackValue)
                        : row.samples[offset];
                    if (row.kind == ControlRow::Kind::BlockLast) {
                        value = std::isfinite(value)
                            ? std::clamp(value, row.minimumValue,
                                         row.maximumValue)
                            : row.nonFiniteValue;
                    }
                    if (std::isfinite(value)) {
                        if (row.kind == ControlRow::Kind::VoiceCap) {
                            const int active = std::clamp(
                                static_cast<int>(std::lround(value)), 1,
                                static_cast<int>(row.zones.size()));
                            for (std::size_t voice = 0;
                                 voice < row.zones.size(); ++voice)
                                if (row.zones[voice] != nullptr)
                                    *row.zones[voice] = voice
                                        < static_cast<std::size_t>(active)
                                            ? 1.0f : 0.0f;
                        } else {
                            for (auto* zone : row.zones)
                                if (zone != nullptr)
                                    *zone = value;
                            if (row.kind == ControlRow::Kind::BlockLast
                                && row.reportsOutputLevel)
                                outputLevel_ = value;
                        }
                    }
                    if (row.kind == ControlRow::Kind::BlockLast) {
                        row.runEnd = count;
                        next = std::min(next, row.runEnd);
                        continue;
                    }
                    if (row.runEnd <= offset) {
                        row.runEnd = count;
                        for (int sample = offset + 1; sample < count; ++sample) {
                            if (! preparedControlSamplesShareRun(
                                    row.samples[sample], value)) {
                                row.runEnd = sample;
                                break;
                            }
                        }
                    }
                    next = std::min(next, row.runEnd);
                }
            }
            for (std::size_t channel = 0;
                 channel < outputPointers_.size(); ++channel)
                outputPointers_[channel] = outputs_[channel].data() + offset;
            for (std::size_t input = 0;
                 input < audioInputBoundaries_.size(); ++input) {
                auto& boundary = audioInputBoundaries_[input];
                const auto* prepared = boundary.runtime != nullptr
                    ? boundary.runtime->channelData(offset, next - offset)
                    : nullptr;
                if (prepared != nullptr) {
                    inputPointers_[input] = const_cast<FAUSTFLOAT*>(prepared);
                    continue;
                }
                inputPointers_[input] = boundary.runtime == nullptr
                    && boundary.channelIndex < static_cast<std::uint32_t>(
                        buffer.getNumChannels())
                    ? const_cast<FAUSTFLOAT*>(buffer.getReadPointer(
                          static_cast<int>(boundary.channelIndex), offset))
                    : silentInput_.data() + offset;
            }
            std::size_t processInput = audioInputBoundaries_.size();
            for (auto& boundary : hostAutomationBoundaries_)
                for (auto& channel : boundary.outputChannels)
                    inputPointers_[processInput++] = channel.data() + offset;
            for (auto& boundary : hostMidiBoundaries_)
                for (int channel = HostMidiInputRuntime::kControlCount;
                     channel < HostMidiInputRuntime::kControlCount
                                   + HostMidiInputRuntime::kOutputCount;
                     ++channel)
                    inputPointers_[processInput++] =
                        boundary.buffer.getWritePointer(channel, offset);
            for (auto& boundary : vmControlOutputBoundaries_)
                for (const auto* row : boundary.rows)
                    inputPointers_[processInput++] =
                        const_cast<FAUSTFLOAT*>(row + offset);
            for (auto& binding : processControlInputs_) {
                inputPointers_[processInput++] = const_cast<FAUSTFLOAT*>(
                    binding.preparedSamples + offset);
            }
            instance_->compute(next - offset, inputPointers_.data(),
                               outputPointers_.data());
            offset = next;
        }
        if (count == 0)
            instance_->compute(0, inputPointers_.data(), outputPointers_.data());
        for (std::size_t channel = 0;
             channel < outputPointers_.size(); ++channel)
            outputPointers_[channel] = outputs_[channel].data();
        publishMidiBoundaries(count, midi);
        updateVmInputBoundaries(count);
        publishTelemetry(count);
        publishVisualBoundaries(count);
        clearControlBindings();
        for (auto& boundary : audioInputBoundaries_)
            if (boundary.runtime != nullptr)
                boundary.runtime->clear();
        const auto finalOffset = tapOffsets_[outputTap_];
        buffer.copyFrom(0, 0, outputs_[finalOffset].data(), count);
        buffer.copyFrom(1, 0, outputs_[finalOffset + 1].data(), count);
        for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
            buffer.clear(channel, 0, count);
        return true;
    }

    // The source-pool boundary deliberately uses Faust's physical voice
    // runtime, but CURLOP remains the allocator: VM rows address physical
    // slots directly and we never call mydsp_poly's MIDI keyOn/keyOff path.
    // This physical-pool slice accepts a zero-input polyphonic Faust source,
    // optionally followed by a serial chain of shared Faust effects or driven
    // by the Host MIDI/VM boundary. It replaces the static cloned-Box pool, so a reduced
    // VOICES row is a real render path rather than an APG escape.
    bool processNativePolyAudio(juce::AudioBuffer<float>& buffer,
                                juce::MidiBuffer&) noexcept
    {
        if (nativePolyPool_ == nullptr || buffer.getNumChannels() < 2
            || buffer.getNumSamples() > blockSize_) {
            clearControlBindings();
            return false;
        }
        if (blockRejected_) {
            blockRejected_ = false;
            clearControlBindings();
            return false;
        }
        const int count = buffer.getNumSamples();
        for (const auto& module : controls_)
            if (module.sampleCount != 0 && module.sampleCount != count) {
                clearControlBindings();
                return false;
            }
        if (nativePolySourceControls_ >= controls_.size()
            || nativePolyOutputControls_ >= controls_.size()) {
            clearControlBindings();
            return false;
        }

        // Host MIDI remains a native event boundary. Its prepared plan runs
        // before these source rows are read, so VM Delivery can assign logical
        // events to mydsp_poly's physical rows in this same callback.
        prepareHostMidiSources(count);

        auto applyRow = [count] (ControlRow& row, int offset) noexcept {
            FAUSTFLOAT value = row.lastValue;
            if (row.kind == ControlRow::Kind::BlockLast) {
                const float requested = row.samples != nullptr && count > 0
                    ? row.samples[count - 1] : row.fallbackValue;
                value = std::isfinite(requested)
                    ? std::clamp(requested, row.minimumValue, row.maximumValue)
                    : row.nonFiniteValue;
            } else if (row.samples != nullptr
                       && std::isfinite(row.samples[offset])) {
                value = row.samples[offset];
            }
            row.lastValue = value;
            for (auto* zone : row.zones)
                if (zone != nullptr)
                    *zone = value;
            return value;
        };

        auto& source = controls_[nativePolySourceControls_];
        auto& output = controls_[nativePolyOutputControls_];
        int offset = 0;
        while (offset < count) {
            int next = count;
            int activeVoices = nativePolyVoiceCount_;
            for (std::size_t rowIndex = 0; rowIndex < source.rows.size(); ++rowIndex) {
                auto& row = source.rows[rowIndex];
                const auto value = applyRow(row, offset);
                if (row.kind == ControlRow::Kind::NativeVoiceCap) {
                    activeVoices = std::clamp(
                        static_cast<int>(std::lround(value)), 1,
                        nativePolyVoiceCount_);
                }
                if (row.kind == ControlRow::Kind::BlockLast || row.samples == nullptr)
                    continue;
                if (row.runEnd <= offset) {
                    row.runEnd = count;
                    for (int sample = offset + 1; sample < count; ++sample)
                        if (! preparedControlSamplesShareRun(
                                row.samples[sample], row.samples[offset])) {
                            row.runEnd = sample;
                            break;
                        }
                }
                next = std::min(next, row.runEnd);
            }
            for (const auto effectControl : nativePolyEffectControls_) {
                if (effectControl >= controls_.size()) {
                    clearControlBindings();
                    return false;
                }
                for (auto& row : controls_[effectControl].rows) {
                    applyRow(row, offset);
                    if (row.kind == ControlRow::Kind::BlockLast
                        || row.samples == nullptr)
                        continue;
                    if (row.runEnd <= offset) {
                        row.runEnd = count;
                        for (int sample = offset + 1; sample < count; ++sample)
                            if (! preparedControlSamplesShareRun(
                                    row.samples[sample], row.samples[offset])) {
                                row.runEnd = sample;
                                break;
                            }
                    }
                    next = std::min(next, row.runEnd);
                }
            }
            const auto outputLevel = applyRow(
                output.rows[nativePolyOutputRow_], offset);
            for (int voice = 0; voice < nativePolyVoiceCount_; ++voice) {
                auto* physical = nativePolyPool_->fVoiceTable[
                    static_cast<std::size_t>(voice)];
                if (voice >= activeVoices) {
                    // A VM capacity change explicitly retires this physical
                    // slot. A normal gate release below is left to Faust so
                    // its envelope tail can finish without being reallocated.
                    physical->fCurNote = kFreeVoice;
                    continue;
                }
                const auto gateRow = nativePolyGateRows_[
                    static_cast<std::size_t>(voice)];
                const auto& gate = source.rows[gateRow];
                const float value = gate.zones.empty() || gate.zones.front() == nullptr
                    ? 0.0f : *gate.zones.front();
                if (value > 0.0f) {
                    physical->fCurNote = voice;
                } else if (physical->fCurNote != kFreeVoice) {
                    physical->fCurNote = kReleaseVoice;
                }
            }
            for (int start = offset; start < next;) {
                const int frames = std::min(next - start, MIX_BUFFER_SIZE);
                std::array<FAUSTFLOAT*, 2> sourceOutputs {
                    outputs_[nativePolySourceOffset_].data() + start,
                    outputs_[nativePolySourceOffset_ + 1].data() + start
                };
                nativePolyPool_->compute(frames, nullptr, sourceOutputs.data());
                std::array<FAUSTFLOAT*, 2> effectOutputs = sourceOutputs;
                for (std::size_t effect = 0;
                     effect < nativePolyEffects_.size(); ++effect) {
                    const auto effectOffset = nativePolyEffectOffsets_[effect];
                    std::array<FAUSTFLOAT*, 2> stageOutputs {
                        outputs_[effectOffset].data() + start,
                        outputs_[effectOffset + 1].data() + start
                    };
                    nativePolyEffects_[effect]->compute(
                        frames, effectOutputs.data(), stageOutputs.data());
                    effectOutputs = stageOutputs;
                }
                for (int sample = 0; sample < frames; ++sample)
                    for (int channel = 0; channel < 2; ++channel) {
                        auto& value = effectOutputs[static_cast<std::size_t>(channel)][sample];
                        outputs_[nativePolyOutputOffset_ + static_cast<std::size_t>(channel)]
                            [static_cast<std::size_t>(start + sample)] =
                            std::clamp(value * outputLevel, -1.0f, 1.0f);
                    }
                start += frames;
            }
            offset = next;
        }
        if (count == 0) {
            std::array<FAUSTFLOAT*, 2> sourceOutputs {
                outputs_[nativePolySourceOffset_].data(),
                outputs_[nativePolySourceOffset_ + 1].data()
            };
            nativePolyPool_->compute(0, nullptr, sourceOutputs.data());
        }
        for (auto& boundary : hostMidiBoundaries_)
            if (boundary.runtime != nullptr)
                boundary.runtime->commitPreparedCandidateBlock();
        buffer.copyFrom(0, 0, outputs_[nativePolyOutputOffset_].data(), count);
        buffer.copyFrom(1, 0, outputs_[nativePolyOutputOffset_ + 1].data(), count);
        for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
            buffer.clear(channel, 0, count);
        clearControlBindings();
        return true;
    }

    bool bindModuleControls(
        int graphModuleIndex, const float* rows, int rowStride,
        const int* rowMap, int rowCount, int sampleCount) noexcept override
    {
        if (nativePolyPool_ != nullptr)
            return bindNativePolyControls(graphModuleIndex, rows, rowStride,
                                          rowMap, rowCount, sampleCount);
        if (rows == nullptr || rowStride < sampleCount || rowMap == nullptr
            || rowCount < 0 || sampleCount < 0 || sampleCount > blockSize_)
            return rejectBlock();
        for (auto& module : controls_) {
            if (module.graphModuleIndex != graphModuleIndex)
                continue;
            if (rowCount < static_cast<int>(module.rows.size()))
                return rejectBlock();
            // APG freezes instances above its block-granular active-voice cap.
            // A fused Faust box computes every internal voice continuously, so
            // accepting a reduced cap here would silently change voice state.
            // Keep full-pool polyphony qualified and leave reduced/dynamic cap
            // graphs on the APG fallback until that semantic is re-decided.
            for (int control = 0; control < static_cast<int>(module.rows.size());
                 ++control) {
                const auto& prepared =
                    module.rows[static_cast<std::size_t>(control)];
                if (prepared.kind != ControlRow::Kind::VoiceCap)
                    continue;
                if (prepared.rowMapIndex < 0
                    || prepared.rowMapIndex >= rowCount)
                    return rejectBlock();
                const int row = rowMap[prepared.rowMapIndex];
                if (row < 0)
                    return rejectBlock();
                const auto* samples = rows
                    + static_cast<std::size_t>(row * rowStride);
                const float fullPool = static_cast<float>(prepared.zones.size());
                for (int sample = 0; sample < sampleCount; ++sample)
                    if (! std::isfinite(samples[sample])
                        || std::abs(samples[sample] - fullPool) > 1.0e-3f)
                        return rejectBlock();
            }
            for (int control = 0;
                 control < static_cast<int>(module.rows.size()); ++control) {
                auto& prepared =
                    module.rows[static_cast<std::size_t>(control)];
                if (prepared.rowMapIndex < 0
                    || prepared.rowMapIndex >= rowCount)
                    return rejectBlock();
                const int row = rowMap[prepared.rowMapIndex];
                prepared.samples =
                    row >= 0 ? rows + static_cast<std::size_t>(row * rowStride)
                             : nullptr;
            }
            module.sampleCount = sampleCount;
            return true;
        }
        return rejectBlock();
    }

    bool bindNativePolyControls(
        int graphModuleIndex, const float* rows, int rowStride,
        const int* rowMap, int rowCount, int sampleCount) noexcept
    {
        if (rows == nullptr || rowStride < sampleCount || rowMap == nullptr
            || rowCount < 0 || sampleCount < 0 || sampleCount > blockSize_)
            return rejectBlock();
        for (auto& module : controls_) {
            if (module.graphModuleIndex != graphModuleIndex)
                continue;
            if (rowCount < static_cast<int>(module.rows.size()))
                return rejectBlock();
            for (auto& row : module.rows) {
                if (row.rowMapIndex < 0 || row.rowMapIndex >= rowCount)
                    return rejectBlock();
                const int mapped = rowMap[row.rowMapIndex];
                row.samples = mapped >= 0
                    ? rows + static_cast<std::size_t>(mapped * rowStride)
                    : nullptr;
                row.runEnd = 0;
            }
            module.sampleCount = sampleCount;
            return true;
        }
        return rejectBlock();
    }

    bool acceptsModuleControls(int graphModuleIndex) const noexcept override
    {
        return std::any_of(
            controls_.begin(), controls_.end(),
            [graphModuleIndex] (const auto& module) {
                return module.graphModuleIndex == graphModuleIndex;
            });
    }

    std::size_t audioInputBoundaryCount() const noexcept override
    {
        return audioInputBoundaries_.size();
    }

    int audioInputBoundaryGraphIndex(
        std::size_t boundary) const noexcept override
    {
        return boundary < audioInputBoundaries_.size()
            ? audioInputBoundaries_[boundary].graphModuleIndex : -1;
    }

    std::uint32_t audioInputBoundaryChannelIndex(
        std::size_t boundary) const noexcept override
    {
        return boundary < audioInputBoundaries_.size()
            ? audioInputBoundaries_[boundary].channelIndex : 0;
    }

    bool bindAudioInputRuntime(
        int graphModuleIndex, AudioInputRuntime* runtime) noexcept override
    {
        if (runtime == nullptr)
            return false;
        for (auto& boundary : audioInputBoundaries_) {
            if (boundary.graphModuleIndex != graphModuleIndex)
                continue;
            if (boundary.channelIndex != runtime->channelIndex())
                return false;
            boundary.runtime = runtime;
            return true;
        }
        return false;
    }

    std::size_t visualInputBoundaryCount() const noexcept override
    {
        return visualBoundaries_.size();
    }

    int visualInputBoundaryGraphIndex(
        std::size_t boundary) const noexcept override
    {
        return boundary < visualBoundaries_.size()
            ? visualBoundaries_[boundary].graphModuleIndex : -1;
    }

    VisualInputRuntime* visualInputRuntime(
        int graphModuleIndex) noexcept override
    {
        for (auto& boundary : visualBoundaries_) {
            if (boundary.graphModuleIndex != graphModuleIndex)
                continue;
            return boundary.runtime.get();
        }
        return nullptr;
    }

    std::size_t midiOutputBoundaryCount() const noexcept override
    {
        return midiBoundaries_.size();
    }

    int midiOutputBoundaryGraphIndex(
        std::size_t boundary) const noexcept override
    {
        return boundary < midiBoundaries_.size()
            ? midiBoundaries_[boundary].graphModuleIndex : -1;
    }

    bool bindMidiOutputRuntime(
        int graphModuleIndex, MidiOutputRuntime* runtime) noexcept override
    {
        if (runtime == nullptr)
            return false;
        for (auto& boundary : midiBoundaries_) {
            if (boundary.graphModuleIndex != graphModuleIndex)
                continue;
            if (boundary.controls.size()
                != static_cast<std::size_t>(MidiOutputRuntime::kInputChannels))
                return false;
            boundary.runtime = runtime;
            return true;
        }
        return false;
    }

    bool bindHostAutomationSource(
        const std::atomic<float>* values, int laneCount) noexcept override
    {
        if (values == nullptr || laneCount <= 0)
            return false;
        for (auto& boundary : hostAutomationBoundaries_) {
            boundary.sourceValues = values;
            boundary.sourceLaneCount = laneCount;
        }
        return ! hostAutomationBoundaries_.empty();
    }

    void clearHostAutomationSource() noexcept override
    {
        for (auto& boundary : hostAutomationBoundaries_) {
            boundary.sourceValues = nullptr;
            boundary.sourceLaneCount = 0;
        }
    }

    std::size_t hostMidiInputBoundaryCount() const noexcept override
    {
        return hostMidiBoundaries_.size();
    }

    int hostMidiInputBoundaryGraphIndex(
        std::size_t boundary) const noexcept override
    {
        return boundary < hostMidiBoundaries_.size()
            ? hostMidiBoundaries_[boundary].graphModuleIndex : -1;
    }

    bool bindHostMidiRuntime(
        int graphModuleIndex,
        HostMidiInputRuntime* runtime) noexcept override
    {
        if (runtime == nullptr)
            return false;
        for (auto& boundary : hostMidiBoundaries_)
            if (boundary.graphModuleIndex == graphModuleIndex) {
                boundary.runtime = runtime;
                return true;
            }
        return false;
    }

    bool bindVmControlOutputBoundary(
        int graphModuleIndex, const float* const* rows,
        int rowCount, int sampleCount) noexcept override
    {
        if (rows == nullptr || rowCount < 0 || sampleCount < 0
            || sampleCount > blockSize_)
            return rejectBlock();
        for (auto& boundary : vmControlOutputBoundaries_) {
            if (boundary.graphModuleIndex != graphModuleIndex)
                continue;
            if (rowCount != static_cast<int>(boundary.rows.size()))
                return rejectBlock();
            for (int row = 0; row < rowCount; ++row) {
                if (rows[row] == nullptr)
                    return rejectBlock();
                boundary.rows[static_cast<std::size_t>(row)] = rows[row];
            }
            boundary.sampleCount = sampleCount;
            return true;
        }
        return rejectBlock();
    }

    int vmInputChannelCount(int graphModuleIndex) const noexcept override
    {
        if (graphModuleIndex < 0
            || graphModuleIndex >= static_cast<int>(
                vmInputBoundaryByGraphIndex_.size()))
            return 0;
        const int boundaryIndex = vmInputBoundaryByGraphIndex_[
            static_cast<std::size_t>(graphModuleIndex)];
        if (boundaryIndex < 0
            || boundaryIndex >= static_cast<int>(vmInputBoundaries_.size()))
            return 0;
        return static_cast<int>(vmInputBoundaries_[
            static_cast<std::size_t>(boundaryIndex)].previousRows.size());
    }

    const float* previousVmInputRow(
        int graphModuleIndex, int channel,
        int sampleCount) const noexcept override
    {
        if (graphModuleIndex < 0
            || graphModuleIndex >= static_cast<int>(
                vmInputBoundaryByGraphIndex_.size()))
            return nullptr;
        const int boundaryIndex = vmInputBoundaryByGraphIndex_[
            static_cast<std::size_t>(graphModuleIndex)];
        if (boundaryIndex < 0
            || boundaryIndex >= static_cast<int>(vmInputBoundaries_.size()))
            return nullptr;
        const auto& boundary = vmInputBoundaries_[
            static_cast<std::size_t>(boundaryIndex)];
        if (channel < 0
            || channel >= static_cast<int>(boundary.previousRows.size())
            || sampleCount < 0
            || sampleCount > static_cast<int>(
                boundary.previousRows[static_cast<std::size_t>(channel)].size()))
            return nullptr;
        return boundary.previousRows[static_cast<std::size_t>(channel)].data();
    }

    bool rejectBlock() noexcept
    {
        blockRejected_ = true;
        return false;
    }

    bool voiceCapsUseFullPool(int sampleCount) const noexcept
    {
        for (const auto& module : controls_)
            for (const auto& row : module.rows) {
                if (row.kind != ControlRow::Kind::VoiceCap)
                    continue;
                if (row.samples == nullptr || module.sampleCount != sampleCount)
                    return false;
                const float fullPool = static_cast<float>(row.zones.size());
                for (int sample = 0; sample < sampleCount; ++sample)
                    if (! std::isfinite(row.samples[sample])
                        || std::abs(row.samples[sample] - fullPool) > 1.0e-3f)
                        return false;
            }
        return true;
    }

    void clearControlBindings() noexcept
    {
        for (auto& module : controls_) {
            module.sampleCount = 0;
            for (auto& row : module.rows) {
                row.samples = nullptr;
                row.runEnd = 0;
            }
        }
        for (auto& boundary : vmControlOutputBoundaries_) {
            boundary.sampleCount = 0;
            std::fill(boundary.rows.begin(), boundary.rows.end(), nullptr);
        }
    }

    void updateVmInputBoundaries(int sampleCount) noexcept
    {
        for (auto& boundary : vmInputBoundaries_) {
            for (std::size_t input = 0;
                 input < boundary.previousRows.size(); ++input) {
                auto& row = boundary.previousRows[input];
                std::fill_n(row.data(), sampleCount, 0.0f);
                for (const auto& edge : boundary.edges[input])
                    for (int sample = 0; sample < sampleCount; ++sample)
                        row[static_cast<std::size_t>(sample)] += edge.gain
                            * tapControlOutputSample(
                                edge.sourceTap, edge.sourceOutput, sample);
                // A later host callback may be wider than this completed
                // block. Keep the previous block's terminal control value
                // across that tail instead of exposing stale older samples or
                // making the prepared row disappear on a legal size change.
                const float tail = sampleCount > 0
                    ? row[static_cast<std::size_t>(sampleCount - 1)] : 0.0f;
                std::fill(
                    row.begin() + sampleCount, row.end(), tail);
            }
        }
    }

    static float normalizedControlOffset(
        const ParamSchemaEntry& parameter, float signal) noexcept
    {
        if (! std::isfinite(signal) || std::abs(signal) <= 1.0e-7f)
            return 0.0f;
        const float normalized = std::clamp(signal, 0.0f, 1.0f);
        if (parameter.scale == Scale::Logarithmic
            && parameter.min > 0.0f && parameter.max > parameter.min)
            return parameter.min * std::pow(
                parameter.max / parameter.min, normalized);
        return parameter.min
            + normalized * (parameter.max - parameter.min);
    }

    void fillBoundaryControl(const ModuleControls& module,
                             std::size_t control,
                             VisualControlInput& prepared,
                             std::vector<float>& channel,
                             int count) const noexcept
    {
        const auto& row = module.rows[control];
        const bool hasSet = std::any_of(
            prepared.edges.begin(), prepared.edges.end(),
            [] (const auto& edge) { return edge.setValue; });
        for (int sample = 0; sample < count; ++sample) {
            if (row.samples != nullptr && std::isfinite(row.samples[sample]))
                prepared.heldBase = row.samples[sample];
            float value = hasSet ? 0.0f : prepared.heldBase;
            for (const auto& edge : prepared.edges) {
                float signal = tapControlOutputSample(
                    edge.sourceTap, edge.sourceOutput, sample) * edge.gain;
                if (edge.pitchSignalToHz)
                    signal = 440.0f * std::pow(
                        2.0f, 10.0f * signal - 0.75f);
                value += edge.setValue ? signal
                    : normalizedControlOffset(prepared.parameter, signal);
            }
            channel[static_cast<std::size_t>(sample)] = value;
        }
    }

    void publishMidiBoundaries(int count, juce::MidiBuffer& midi) noexcept
    {
        if (count <= 0)
            return;
        for (auto& boundary : midiBoundaries_) {
            if (boundary.runtime == nullptr)
                continue;
            const auto& module = controls_[boundary.controlModule];
            for (std::size_t control = 0;
                 control < boundary.controls.size(); ++control)
                fillBoundaryControl(
                    module, control, boundary.controls[control],
                    boundary.channels[control], count);
            boundary.runtime->processPreparedControls(
                boundary.channelPointers.data(),
                static_cast<int>(boundary.channelPointers.size()), count,
                midi);
        }
    }

    void prepareHostAutomationSources(int count) noexcept
    {
        if (count <= 0)
            return;
        for (auto& boundary : hostAutomationBoundaries_) {
            const auto& module = controls_[boundary.controlModule];
            for (std::size_t control = 0;
                 control < boundary.controls.size(); ++control)
                fillBoundaryControl(
                    module, control, boundary.controls[control],
                    boundary.controlChannels[control], count);
            if (boundary.sourceValues != nullptr)
                processPreparedHostAutomationControls(
                    boundary.sourceValues, boundary.sourceLaneCount,
                    boundary.controlPointers.data(),
                    static_cast<int>(boundary.controlPointers.size()),
                    boundary.outputPointers.data(),
                    static_cast<int>(boundary.outputPointers.size()), count);
            else
                for (auto& output : boundary.outputChannels)
                    std::fill_n(output.data(), count, 0.0f);
        }
    }

    void prepareHostMidiSources(int count) noexcept
    {
        for (auto& boundary : hostMidiBoundaries_) {
            const auto& module = controls_[boundary.controlModule];
            for (std::size_t control = 0; control < boundary.controls.size(); ++control)
                fillBoundaryControl(module, control, boundary.controls[control],
                    boundary.controlChannels[control], count);
            for (std::size_t control = 0; control < boundary.controls.size(); ++control)
                for (int sample = 0; sample < count; ++sample)
                    boundary.buffer.setSample(
                        static_cast<int>(control), sample,
                        boundary.controlChannels[control][
                            static_cast<std::size_t>(sample)]
                            - boundary.controls[control].parameter.defaultValue);
            for (int channel = static_cast<int>(boundary.controls.size());
                 channel < boundary.buffer.getNumChannels(); ++channel)
                boundary.buffer.clear(channel, 0, count);
            auto readControl = [&boundary] (
                int control, int sample, float fallback) noexcept {
                if (control < 0
                    || control >= static_cast<int>(
                        boundary.controlChannels.size()))
                    return fallback;
                const float value = boundary.controlChannels[
                    static_cast<std::size_t>(control)][
                        static_cast<std::size_t>(sample)];
                return std::isfinite(value) ? value : fallback;
            };
            if (boundary.runtime != nullptr) {
                boundary.runtime->beginPreparedCandidateBlock();
                boundary.runtime->process(
                    boundary.buffer.getWritePointer(2),
                    boundary.buffer.getWritePointer(3),
                    boundary.buffer.getWritePointer(4),
                    boundary.buffer.getWritePointer(5), count, readControl);
            } else
                for (int channel = 2;
                     channel < boundary.buffer.getNumChannels(); ++channel)
                    boundary.buffer.clear(channel, 0, count);
        }
    }

    void publishVisualBoundaries(int count) noexcept
    {
        if (count <= 0)
            return;
        for (auto& boundary : visualBoundaries_) {
            if (boundary.runtime == nullptr)
                continue;
            const auto& module = controls_[boundary.controlModule];
            for (std::size_t control = 0;
                 control < boundary.controls.size(); ++control) {
                fillBoundaryControl(
                    module, control, boundary.controls[control],
                    boundary.channels[control], count);
            }
            const auto audioOffset = boundary.controls.size();
            for (std::size_t audio = 0;
                 audio < boundary.audioInputs.size(); ++audio) {
                auto& left = boundary.channels[audioOffset + audio * 2];
                auto& right = boundary.channels[audioOffset + audio * 2 + 1];
                for (int sample = 0; sample < count; ++sample) {
                    float mixedLeft = 0.0f;
                    float mixedRight = 0.0f;
                    for (const auto& edge : boundary.audioInputs[audio].edges) {
                        mixedLeft += tapSample(
                            edge.sourceTap, 0, sample) * edge.gains[0];
                        mixedRight += tapSample(
                            edge.sourceTap, 1, sample) * edge.gains[1];
                    }
                    left[static_cast<std::size_t>(sample)] = mixedLeft;
                    right[static_cast<std::size_t>(sample)] = mixedRight;
                }
            }
            boundary.runtime->publishPreparedBlock(
                boundary.channelPointers.data(),
                static_cast<int>(boundary.channelPointers.size()), count);
        }
    }

    RendererReplacementPolicy replacementPolicy() const noexcept override
    {
        return {};
    }

    std::string_view rendererId() const noexcept override
    {
        return nativePolyPool_ != nullptr
            ? "faust.graph-state.native-poly.v1"
            : "faust.graph-state.v1";
    }

    std::size_t tapCount() const noexcept override { return tapIds_.size(); }

    std::string_view tapModuleId(std::size_t tap) const noexcept override
    {
        return tap < tapIds_.size() ? std::string_view(tapIds_[tap])
                                    : std::string_view();
    }

    int tapChannelCount(std::size_t tap) const noexcept override
    {
        return tap < tapWidths_.size() ? tapWidths_[tap] : 0;
    }

    bool ownsControlBoundary(std::size_t tap) const noexcept override
    {
        return tap < tapControlBoundaries_.size()
            && tapControlBoundaries_[tap];
    }

    float tapSample(std::size_t tap, int channel, int sample) const noexcept override
    {
        if (tap >= tapIds_.size() || channel < 0
            || channel >= tapWidths_[tap]
            || sample < 0 || sample >= blockSize_)
            return 0.0f;
        return outputs_[tapOffsets_[tap] + static_cast<std::size_t>(channel)]
            [static_cast<std::size_t>(sample)];
    }

    std::size_t tapControlOutputCount(
        std::size_t tap) const noexcept override
    {
        return tap < tapControlOutputIds_.size()
            ? tapControlOutputIds_[tap].size() : 0;
    }

    std::string_view tapControlOutputId(
        std::size_t tap, std::size_t output) const noexcept override
    {
        return tap < tapControlOutputIds_.size()
                && output < tapControlOutputIds_[tap].size()
            ? std::string_view(tapControlOutputIds_[tap][output])
            : std::string_view();
    }

    float tapControlOutputSample(
        std::size_t tap, std::size_t output, int sample) const noexcept override
    {
        if (tap >= tapIds_.size()
            || output >= tapControlOutputIds_[tap].size()
            || sample < 0 || sample >= blockSize_)
            return 0.0f;
        // A native host-automation source can feed another native boundary
        // (such as MIDI Out) without appearing in Faust's final audio box.
        // In that case libfaust eliminates the unused pass-through, so its
        // prepared source buffer is the authoritative control tap.
        if (tap < hostAutomationBoundaryByTap_.size()) {
            const int boundaryIndex = hostAutomationBoundaryByTap_[tap];
            if (boundaryIndex >= 0
                && boundaryIndex < static_cast<int>(hostAutomationBoundaries_.size())) {
                const auto& boundary = hostAutomationBoundaries_[
                    static_cast<std::size_t>(boundaryIndex)];
                if (output < boundary.outputChannels.size())
                    return boundary.outputChannels[output][
                        static_cast<std::size_t>(sample)];
            }
        }
        return outputs_[tapOffsets_[tap]
                        + static_cast<std::size_t>(tapWidths_[tap]) + output]
            [static_cast<std::size_t>(sample)];
    }

    bool exchangeTapControlOutputRange(
        std::size_t tap, std::size_t output,
        float& minimum, float& maximum, float& last) noexcept override
    {
        if (controlOutputHolds_ == nullptr
            || tap >= tapControlOutputIds_.size()
            || output >= tapControlOutputIds_[tap].size()
            || tap >= tapControlTelemetryOffsets_.size())
            return false;
        return controlOutputHolds_[
            tapControlTelemetryOffsets_[tap] + output]
                .exchange(minimum, maximum, last);
    }

    bool publishesProductObservers() const noexcept override { return true; }

    bool exchangeTapOutputMeter(
        std::size_t tap, float& peak, float& rms) noexcept override
    {
        return tap < tapIds_.size()
            && exchangeLevel(outputLevels_.get(), tap, peak, rms);
    }

    bool exchangeTapInputMeter(
        std::size_t tap, float& peak, float& rms) noexcept override
    {
        return tap < inputMixes_.size() && inputMixes_[tap].width > 0
            && exchangeLevel(inputLevels_.get(), tap, peak, rms);
    }

    bool exchangeTapClip(std::size_t tap, float& clip) noexcept override
    {
        if (tap != outputTap_ || clips_ == nullptr)
            return false;
        clip = clips_[tap].exchange(0.0f, std::memory_order_relaxed);
        return true;
    }

    std::size_t tapFaceplateMeterCount(
        std::size_t tap) const noexcept override
    {
        return tap < faceplateTelemetry_.size()
            ? faceplateTelemetry_[tap].ids.size() : 0;
    }

    std::string_view tapFaceplateMeterId(
        std::size_t tap, std::size_t meter) const noexcept override
    {
        return tap < faceplateTelemetry_.size()
                && meter < faceplateTelemetry_[tap].ids.size()
            ? std::string_view(faceplateTelemetry_[tap].ids[meter])
            : std::string_view();
    }

    float tapFaceplateMeterValue(
        std::size_t tap, std::size_t meter) const noexcept override
    {
        if (tap >= faceplateTelemetry_.size()
            || meter >= faceplateTelemetry_[tap].ids.size()
            || faceplateTelemetry_[tap].values == nullptr)
            return 0.0f;
        return faceplateTelemetry_[tap].values[meter].load(
            std::memory_order_relaxed);
    }

    static void holdMaximum(std::atomic<float>& target, float value) noexcept
    {
        float previous = target.load(std::memory_order_relaxed);
        while (value > previous
               && ! target.compare_exchange_weak(
                   previous, value, std::memory_order_relaxed)) {}
    }

    static bool exchangeLevel(
        LevelHold* levels, std::size_t tap, float& peak, float& rms) noexcept
    {
        if (levels == nullptr)
            return false;
        peak = levels[tap].peak.exchange(0.0f, std::memory_order_relaxed);
        rms = levels[tap].rms.exchange(0.0f, std::memory_order_relaxed);
        return true;
    }

    float inputSample(
        std::size_t tap, int channel, int sample) const noexcept
    {
        float value = 0.0f;
        for (const auto& edge : inputMixes_[tap].edges) {
            if (channel >= static_cast<int>(edge.gains.size()))
                continue;
            const float source = edge.oneSampleFeedback
                ? (sample == 0
                    ? edge.previousSamples[static_cast<std::size_t>(channel)]
                    : tapSample(edge.sourceTap, channel, sample - 1))
                : tapSample(edge.sourceTap, channel, sample);
            value += source
                * edge.gains[static_cast<std::size_t>(channel)];
        }
        return value;
    }

    void publishLevel(
        LevelHold& destination, std::size_t tap, int width, int count,
        bool input) noexcept
    {
        float peak = 0.0f;
        double sumSquares = 0.0;
        for (int channel = 0; channel < width; ++channel) {
            for (int sample = 0; sample < count; ++sample) {
                const float value = input
                    ? inputSample(tap, channel, sample)
                    : tapSample(tap, channel, sample);
                const float magnitude = std::abs(value);
                if (magnitude > peak)
                    peak = magnitude;
                sumSquares += static_cast<double>(value)
                    * static_cast<double>(value);
            }
        }
        const auto samples = width * count;
        const float rms = samples > 0
            ? static_cast<float>(std::sqrt(
                sumSquares / static_cast<double>(samples)))
            : 0.0f;
        holdMaximum(destination.peak, peak);
        holdMaximum(destination.rms, rms);
    }

    void publishTelemetry(int count) noexcept
    {
        for (std::size_t tap = 0; tap < tapIds_.size(); ++tap) {
            publishLevel(outputLevels_[tap], tap, tapWidths_[tap], count,
                         false);
            if (inputMixes_[tap].width > 0)
                publishLevel(inputLevels_[tap], tap, inputMixes_[tap].width,
                             count, true);
            if (controlOutputHolds_ != nullptr
                && tap < tapControlTelemetryOffsets_.size())
                for (std::size_t output = 0;
                     output < tapControlOutputIds_[tap].size(); ++output) {
                    float minimum = 0.0f;
                    float maximum = 0.0f;
                    float last = 0.0f;
                    if (count > 0) {
                        minimum = tapControlOutputSample(
                            tap, output, 0);
                        maximum = minimum;
                        for (int sample = 1; sample < count; ++sample) {
                            const float value = tapControlOutputSample(
                                tap, output, sample);
                            minimum = std::min(minimum, value);
                            maximum = std::max(maximum, value);
                        }
                        last = tapControlOutputSample(
                            tap, output, count - 1);
                    }
                    controlOutputHolds_[
                        tapControlTelemetryOffsets_[tap] + output]
                            .publish(minimum, maximum, last);
                }
        }

        if (outputTap_ < inputMixes_.size() && clips_ != nullptr) {
            bool clipped = false;
            const int width = inputMixes_[outputTap_].width;
            for (int channel = 0; channel < width && ! clipped; ++channel)
                for (int sample = 0; sample < count; ++sample)
                    if (std::abs(inputSample(
                            outputTap_, channel, sample) * outputLevel_) > 1.0f) {
                        clipped = true;
                        break;
                    }
            if (clipped)
                clips_[outputTap_].store(1.0f, std::memory_order_relaxed);
        }

        for (auto& module : faceplateTelemetry_)
            for (std::size_t meter = 0; meter < module.ids.size(); ++meter) {
                const auto* zone = module.zones[meter];
                float value = 0.0f;
                if (zone != nullptr && std::isfinite(static_cast<float>(*zone)))
                    value = static_cast<float>(*zone);
                else if (module.useTapSignal[meter]) {
                    const auto tap = static_cast<std::size_t>(&module - faceplateTelemetry_.data());
                    const int width = tap < tapWidths_.size() ? tapWidths_[tap] : 0;
                    for (int channel = 0; channel < width; ++channel)
                        for (int sample = 0; sample < count; ++sample)
                            value = std::max(value, std::abs(tapSample(tap, channel, sample)));
                }
                module.values[meter].store(value, std::memory_order_relaxed);
            }

        if (count > 0)
            for (auto& input : inputMixes_)
                for (auto& edge : input.edges)
                    if (edge.oneSampleFeedback)
                        for (int channel = 0;
                             channel < static_cast<int>(edge.gains.size());
                             ++channel)
                            edge.previousSamples[
                                static_cast<std::size_t>(channel)] = tapSample(
                                    edge.sourceTap, channel, count - 1);
    }

    std::size_t preparedAudioBufferBytes() const noexcept override
    {
        std::size_t bytes = 0;
        for (const auto& channel : outputs_)
            bytes += channel.capacity() * sizeof(FAUSTFLOAT);
        bytes += silentInput_.capacity() * sizeof(FAUSTFLOAT);
        for (const auto& binding : processControlInputs_)
            bytes += binding.scratch.capacity() * sizeof(FAUSTFLOAT);
        for (const auto& boundary : visualBoundaries_)
            for (const auto& channel : boundary.channels)
                bytes += channel.capacity() * sizeof(float);
        for (const auto& boundary : midiBoundaries_)
            for (const auto& channel : boundary.channels)
                bytes += channel.capacity() * sizeof(float);
        for (const auto& boundary : hostAutomationBoundaries_) {
            for (const auto& channel : boundary.controlChannels)
                bytes += channel.capacity() * sizeof(float);
            for (const auto& channel : boundary.outputChannels)
                bytes += channel.capacity() * sizeof(float);
        }
        for (const auto& boundary : hostMidiBoundaries_) {
            for (const auto& channel : boundary.controlChannels)
                bytes += channel.capacity() * sizeof(float);
            bytes += static_cast<std::size_t>(boundary.buffer.getNumChannels())
                * static_cast<std::size_t>(boundary.buffer.getNumSamples())
                * sizeof(float);
        }
        for (const auto& boundary : vmInputBoundaries_)
            for (const auto& row : boundary.previousRows)
                bytes += row.capacity() * sizeof(float);
        bytes += inputPointers_.capacity() * sizeof(FAUSTFLOAT*);
        return bytes;
    }

#if JUCE_IOS
    interpreter_dsp_factory* factory_ = nullptr;
#else
    llvm_dsp_factory* factory_ = nullptr;
#endif
    std::unique_ptr<::dsp> instance_;
    std::unique_ptr<mydsp_poly> nativePolyPool_;
    std::vector<std::unique_ptr<MapUI>> nativePolyVoiceUis_;
    std::vector<NativePolyEffectFactory*> nativePolyEffectFactories_;
    std::vector<std::unique_ptr<::dsp>> nativePolyEffects_;
    std::vector<std::unique_ptr<MapUI>> nativePolyEffectUis_;
    std::size_t nativePolySourceControls_ = 0;
    std::vector<std::size_t> nativePolyEffectControls_;
    std::size_t nativePolyOutputControls_ = 0;
    std::size_t nativePolyOutputRow_ = 0;
    std::size_t nativePolySourceOffset_ = 0;
    std::vector<std::size_t> nativePolyEffectOffsets_;
    std::size_t nativePolyOutputOffset_ = 0;
    int nativePolyVoiceCount_ = 0;
    std::vector<std::size_t> nativePolyGateRows_;
    int blockSize_ = 0;
    std::vector<std::string> tapIds_;
    std::vector<std::size_t> tapOffsets_;
    std::vector<int> tapWidths_;
    std::vector<bool> tapControlBoundaries_;
    std::vector<std::vector<std::string>> tapControlOutputIds_;
    std::vector<std::size_t> tapControlTelemetryOffsets_;
    std::unique_ptr<ControlRangeHandoff[]> controlOutputHolds_;
    std::size_t outputTap_ = 0;
    float outputLevel_ = 1.0f;
    std::vector<InputMix> inputMixes_;
    std::unique_ptr<LevelHold[]> outputLevels_;
    std::unique_ptr<LevelHold[]> inputLevels_;
    std::unique_ptr<std::atomic<float>[]> clips_;
    std::vector<FaceplateTelemetry> faceplateTelemetry_;
    bool blockRejected_ = false;
    std::vector<ModuleControls> controls_;
    std::vector<ProcessControlInput> processControlInputs_;
    std::vector<VisualBoundary> visualBoundaries_;
    std::vector<MidiBoundary> midiBoundaries_;
    std::vector<AudioInputBoundary> audioInputBoundaries_;
    std::vector<HostAutomationBoundary> hostAutomationBoundaries_;
    std::vector<int> hostAutomationBoundaryByTap_;
    std::vector<HostMidiBoundary> hostMidiBoundaries_;
    std::vector<VmControlOutputBoundary> vmControlOutputBoundaries_;
    std::vector<VmInputBoundary> vmInputBoundaries_;
    std::vector<int> vmInputBoundaryByGraphIndex_;
    std::vector<FAUSTFLOAT*> inputPointers_;
    std::vector<FAUSTFLOAT> silentInput_;
    std::vector<std::vector<FAUSTFLOAT>> outputs_;
    std::vector<FAUSTFLOAT*> outputPointers_;
};

} // namespace

FaustGraphRendererBuild buildFaustGraphRenderer(
    const GraphState& state, double sampleRate, int maximumBlockSize)
{
    FaustGraphRendererBuild result;
    RendererBuildTimingScope timingScope(result);
    if (sampleRate <= 0.0 || maximumBlockSize <= 0) {
        result.diagnostic = "sample rate and maximum block size must be positive";
        return result;
    }
    const auto planStart = RendererBuildClock::now();
    const auto plan = prepareGraphPlan(state);
    result.timing.graphPlanUs = rendererBuildElapsedUs(planStart);
    if (! plan.diagnostic.empty()) {
        result.diagnostic = plan.diagnostic;
        result.rejectPublication = plan.rejectPublication;
        return result;
    }

    std::vector<std::string> argumentsStorage { "-I", CURLOP_FAUST_SHARE_DIR };
    auto arguments = argumentPointers(argumentsStorage);
    // Declare renderer ownership before the libfaust context. On any
    // post-factory rejection, reverse destruction must release the context's
    // compile lock before the renderer deletes that factory under the same
    // global lock.
    std::unique_ptr<FaustGraphRenderer> renderer;
    LibContext context;
    std::string error;

    // Production physical-pool cutover: a zero-input polyphonic Faust source
    // can feed Output directly or through an ordered chain of shared Faust
    // effects. The Host MIDI/VM boundary is a separate control-only shape.
    // The general compiler below still owns broader graphs, but these shapes
    // must never be lowered to permanent Box clones: mydsp_poly is the
    // physical voice runtime and the VM supplies its direct per-slot rows.
    const ModuleEntry* nativePolySource = nullptr;
    std::vector<const ModuleEntry*> nativePolyEffects;
    const ModuleEntry* nativePolyHostMidi = nullptr;
    const ModuleEntry* nativePolyOutput = nullptr;
    if (state.modules().size() >= 2u) {
        for (const auto& module : state.modules()) {
            if (isFaust(module) && module.physicalVoices > 1)
                nativePolySource = &module;
            else if (isFaust(module))
                nativePolyEffects.push_back(&module);
            else if (isHostMidiSource(module))
                nativePolyHostMidi = &module;
            else if (isOutput(module))
                nativePolyOutput = &module;
        }
    }
    const auto hasAudioEdge = [&state] (int source, int target) {
        return std::any_of(state.edges().begin(), state.edges().end(),
                           [source, target] (const auto& edge) {
                               return edge.srcIndex == source
                                   && edge.tgtIndex == target
                                   && edge.feedbackBoundary == FeedbackBoundary::None;
                           });
    };
    const auto moduleAt = [&state] (int index) -> const ModuleEntry* {
        const auto found = std::find_if(
            state.modules().begin(), state.modules().end(),
            [index] (const auto& module) { return module.index == index; });
        return found == state.modules().end() ? nullptr : &*found;
    };
    std::vector<const ModuleEntry*> nativePolyEffectChain;
    const bool nativePolyInventoryComplete = nativePolySource != nullptr
        && nativePolyOutput != nullptr
        && (static_cast<std::size_t>(nativePolySource != nullptr)
            + nativePolyEffects.size()
            + (nativePolyHostMidi != nullptr)
            + (nativePolyOutput != nullptr)) == state.modules().size()
        && nativePolySource->controlOutputs.empty()
        && nativePolyOutput->controlOutputs.empty()
        && std::all_of(nativePolyEffects.begin(), nativePolyEffects.end(),
                       [] (const auto* effect) {
                           return effect->physicalVoices <= 1
                               && effect->controlOutputs.empty();
                       });
    bool hasNativePolySerialRoute = false;
    if (nativePolyInventoryComplete
        && std::all_of(state.edges().begin(), state.edges().end(),
                       [] (const auto& edge) {
                           return edge.feedbackBoundary == FeedbackBoundary::None;
                       })
        && (nativePolyHostMidi == nullptr
            || (nativePolyHostMidi->params.size() == 2u
                && nativePolyHostMidi->controlOutputs.size() == 4u))) {
        std::unordered_set<int> visitedEffects;
        int current = nativePolySource->index;
        bool valid = true;
        for (std::size_t position = 0;
             position < nativePolyEffects.size(); ++position) {
            const ModuleEntry* successor = nullptr;
            for (const auto& edge : state.edges()) {
                if (edge.srcIndex != current)
                    continue;
                const auto* candidate = moduleAt(edge.tgtIndex);
                const bool isUnvisitedEffect = candidate != nullptr
                    && std::find(nativePolyEffects.begin(), nativePolyEffects.end(),
                                 candidate) != nativePolyEffects.end()
                    && visitedEffects.count(candidate->index) == 0;
                if (! isUnvisitedEffect || successor != nullptr) {
                    valid = false;
                    break;
                }
                successor = candidate;
            }
            if (! valid || successor == nullptr) {
                valid = false;
                break;
            }
            nativePolyEffectChain.push_back(successor);
            visitedEffects.insert(successor->index);
            current = successor->index;
        }
        hasNativePolySerialRoute = valid
            && visitedEffects.size() == nativePolyEffects.size()
            && hasAudioEdge(current, nativePolyOutput->index)
            && std::all_of(
                state.edges().begin(), state.edges().end(),
                [nativePolySource, nativePolyHostMidi, nativePolyOutput,
                 &nativePolyEffectChain] (const auto& edge) {
                    if (nativePolyHostMidi != nullptr
                        && edge.srcIndex == nativePolyHostMidi->index)
                        return edge.tgtIndex == nativePolySource->index;
                    if (edge.srcIndex == nativePolySource->index)
                        return edge.tgtIndex
                            == (nativePolyEffectChain.empty()
                                ? nativePolyOutput->index
                                : nativePolyEffectChain.front()->index);
                    for (std::size_t effect = 0;
                         effect < nativePolyEffectChain.size(); ++effect)
                        if (edge.srcIndex == nativePolyEffectChain[effect]->index)
                            return edge.tgtIndex
                                == (effect + 1u == nativePolyEffectChain.size()
                                    ? nativePolyOutput->index
                                    : nativePolyEffectChain[effect + 1u]->index);
                    return false;
                });
        if (! hasNativePolySerialRoute)
            nativePolyEffectChain.clear();
    }
    const bool isNativePolyShape = hasNativePolySerialRoute;
    if (isNativePolyShape) {
        auto sourceBox = parseBox(nativePolySource->moduleId,
                                  nativePolySource->code, arguments,
                                  0, 2, error);
        if (sourceBox == nullptr) {
            result.diagnostic = error.empty()
                ? "polyphonic Faust source could not be parsed" : error;
            return result;
        }
        renderer = std::make_unique<FaustGraphRenderer>();
        const auto factoryStart = RendererBuildClock::now();
#if JUCE_IOS
        renderer->factory_ = createInterpreterDSPFactoryFromBoxes(
            nativePolySource->moduleId, sourceBox,
            static_cast<int>(arguments.size()), arguments.data(), error);
#else
        renderer->factory_ = createDSPFactoryFromBoxes(
            nativePolySource->moduleId, sourceBox,
            static_cast<int>(arguments.size()), arguments.data(), "", error,
            -1);
#endif
        result.timing.factoryUs += rendererBuildElapsedUs(factoryStart);
        if (renderer->factory_ == nullptr) {
            result.diagnostic = error.empty()
                ? "polyphonic Faust source factory could not be created" : error;
            return result;
        }
        const auto instanceStart = RendererBuildClock::now();
        auto* baseVoice = renderer->factory_->createDSPInstance();
        if (baseVoice == nullptr) {
            result.diagnostic = "polyphonic Faust source could not create a voice";
            return result;
        }
        const int voices = nativePolySource->physicalVoices;
        renderer->nativePolyPool_ = std::make_unique<mydsp_poly>(
            baseVoice, voices, true, false);
        renderer->nativePolyPool_->init(
            static_cast<int>(std::lround(sampleRate)));
        result.timing.instanceInitUs += rendererBuildElapsedUs(instanceStart);
        if (renderer->nativePolyPool_->getNumInputs() != 0
            || renderer->nativePolyPool_->getNumOutputs() != 2) {
            result.diagnostic = "mydsp_poly source must have zero inputs and stereo output";
            return result;
        }
        renderer->nativePolyEffectFactories_.reserve(
            nativePolyEffectChain.size());
        renderer->nativePolyEffects_.reserve(nativePolyEffectChain.size());
        renderer->nativePolyEffectUis_.reserve(nativePolyEffectChain.size());
        renderer->nativePolyEffectOffsets_.resize(
            nativePolyEffectChain.size(), 0);
        for (const auto* nativePolyEffect : nativePolyEffectChain) {
            auto effectBox = parseBox(nativePolyEffect->moduleId,
                                      nativePolyEffect->code, arguments,
                                      2, 2, error);
            if (effectBox == nullptr) {
                result.diagnostic = error.empty()
                    ? "shared Faust effect could not be parsed" : error;
                return result;
            }
#if JUCE_IOS
            const auto effectFactoryStart = RendererBuildClock::now();
            auto* effectFactory = createInterpreterDSPFactoryFromBoxes(
                nativePolyEffect->moduleId, effectBox,
                static_cast<int>(arguments.size()), arguments.data(), error);
#else
            const auto effectFactoryStart = RendererBuildClock::now();
            auto* effectFactory = createDSPFactoryFromBoxes(
                nativePolyEffect->moduleId, effectBox,
                static_cast<int>(arguments.size()), arguments.data(), "", error,
                -1);
#endif
            result.timing.factoryUs += rendererBuildElapsedUs(effectFactoryStart);
            if (effectFactory == nullptr) {
                result.diagnostic = error.empty()
                    ? "shared Faust effect factory could not be created" : error;
                return result;
            }
            renderer->nativePolyEffectFactories_.push_back(effectFactory);
            const auto effectInstanceStart = RendererBuildClock::now();
            auto effectInstance = std::unique_ptr<::dsp>(
                effectFactory->createDSPInstance());
            if (effectInstance == nullptr) {
                result.diagnostic = "shared Faust effect could not create an instance";
                return result;
            }
            effectInstance->init(static_cast<int>(std::lround(sampleRate)));
            result.timing.instanceInitUs += rendererBuildElapsedUs(effectInstanceStart);
            if (effectInstance->getNumInputs() != 2
                || effectInstance->getNumOutputs() != 2) {
                result.diagnostic = "shared Faust effect must have stereo input and output";
                return result;
            }
            auto effectUi = std::make_unique<MapUI>();
            effectInstance->buildUserInterface(effectUi.get());
            renderer->nativePolyEffects_.push_back(std::move(effectInstance));
            renderer->nativePolyEffectUis_.push_back(std::move(effectUi));
        }
        renderer->blockSize_ = maximumBlockSize;
        renderer->nativePolyVoiceCount_ = voices;
        renderer->nativePolyVoiceUis_.reserve(
            static_cast<std::size_t>(voices));
        for (int voice = 0; voice < voices; ++voice) {
            auto ui = std::make_unique<MapUI>();
            renderer->nativePolyPool_->fVoiceTable[
                static_cast<std::size_t>(voice)]->buildUserInterface(ui.get());
            renderer->nativePolyVoiceUis_.push_back(std::move(ui));
        }

        FaustGraphRenderer::ModuleControls sourceControls;
        sourceControls.graphModuleIndex = nativePolySource->index;
        for (const auto& parameter : nativePolySource->params) {
            const auto control = faustControlInputOf(
                parameter.name, parameter.type == ParamType::Boolean,
                parameter.defaultValue, parameter.min, parameter.max,
                parameter.scale == Scale::Logarithmic ? "log" : "");
            const bool perVoice = control.type == vm::SignalType::Gate
                || control.type == vm::SignalType::Pitch
                || control.type == vm::SignalType::Velocity;
            const int rows = perVoice ? voices : 1;
            for (int voice = 0; voice < rows; ++voice) {
                FaustGraphRenderer::ControlRow row;
                row.rowMapIndex = static_cast<int>(sourceControls.rows.size());
                row.fallbackValue = parameter.defaultValue;
                row.minimumValue = parameter.min;
                row.maximumValue = parameter.max;
                row.lastValue = parameter.defaultValue;
                const int firstVoice = perVoice ? voice : 0;
                const int lastVoice = perVoice ? voice + 1 : voices;
                // MapUI is the Faust architecture's direct UI lookup: it
                // preserves the authored control label, while the canonical
                // CURLOP comparison helper normalises it for schema matching.
                const auto& name = parameter.name;
                for (int physical = firstVoice; physical < lastVoice; ++physical) {
                    auto* zone = renderer->nativePolyVoiceUis_[
                        static_cast<std::size_t>(physical)]->getParamZone(name);
                    if (zone == nullptr) {
                        result.diagnostic = "mydsp_poly control "
                            + nativePolySource->moduleId + "."
                            + parameter.name + " is unavailable on voice "
                            + std::to_string(physical);
                        return result;
                    }
                    row.zones.push_back(zone);
                }
                if (control.type == vm::SignalType::Gate)
                    renderer->nativePolyGateRows_.push_back(
                        sourceControls.rows.size());
                sourceControls.rows.push_back(std::move(row));
            }
        }
        if (renderer->nativePolyGateRows_.size()
            != static_cast<std::size_t>(voices)) {
            result.diagnostic = "mydsp_poly source requires one canonical gate row per physical voice";
            return result;
        }
        FaustGraphRenderer::ControlRow cap;
        cap.kind = FaustGraphRenderer::ControlRow::Kind::NativeVoiceCap;
        cap.rowMapIndex = static_cast<int>(sourceControls.rows.size());
        cap.fallbackValue = static_cast<float>(voices);
        cap.minimumValue = 1.0f;
        cap.maximumValue = static_cast<float>(voices);
        cap.lastValue = cap.fallbackValue;
        sourceControls.rows.push_back(std::move(cap));
        renderer->nativePolySourceControls_ = renderer->controls_.size();
        renderer->controls_.push_back(std::move(sourceControls));

        for (std::size_t effect = 0;
             effect < nativePolyEffectChain.size(); ++effect) {
            const auto* nativePolyEffect = nativePolyEffectChain[effect];
            FaustGraphRenderer::ModuleControls effectControls;
            effectControls.graphModuleIndex = nativePolyEffect->index;
            for (const auto& parameter : nativePolyEffect->params) {
                FaustGraphRenderer::ControlRow row;
                row.rowMapIndex = static_cast<int>(effectControls.rows.size());
                row.fallbackValue = parameter.defaultValue;
                row.minimumValue = parameter.min;
                row.maximumValue = parameter.max;
                row.lastValue = parameter.defaultValue;
                auto* zone = renderer->nativePolyEffectUis_[effect]->getParamZone(
                    parameter.name);
                if (zone == nullptr) {
                    result.diagnostic = "shared Faust effect control "
                        + nativePolyEffect->moduleId + "." + parameter.name
                        + " is unavailable";
                    return result;
                }
                row.zones.push_back(zone);
                effectControls.rows.push_back(std::move(row));
            }
            renderer->nativePolyEffectControls_.push_back(
                renderer->controls_.size());
            renderer->controls_.push_back(std::move(effectControls));
        }

        if (nativePolyHostMidi != nullptr) {
            FaustGraphRenderer::ModuleControls midiControls;
            midiControls.graphModuleIndex = nativePolyHostMidi->index;
            midiControls.rows.reserve(nativePolyHostMidi->params.size());
            for (std::size_t parameter = 0;
                 parameter < nativePolyHostMidi->params.size(); ++parameter) {
                const auto& source = nativePolyHostMidi->params[parameter];
                FaustGraphRenderer::ControlRow row;
                // Native MIDI's declaration places the fixed note bus before
                // its two selector rows, matching the normal renderer path.
                row.rowMapIndex = 3 + static_cast<int>(parameter);
                row.fallbackValue = source.defaultValue;
                row.minimumValue = source.min;
                row.maximumValue = source.max;
                row.lastValue = source.defaultValue;
                midiControls.rows.push_back(std::move(row));
            }
            FaustGraphRenderer::HostMidiBoundary boundary;
            boundary.graphModuleIndex = nativePolyHostMidi->index;
            boundary.controlModule = renderer->controls_.size();
            for (const auto& parameter : nativePolyHostMidi->params) {
                FaustGraphRenderer::VisualControlInput control;
                control.parameter = parameter;
                control.heldBase = parameter.defaultValue;
                boundary.controls.push_back(std::move(control));
            }
            boundary.controlChannels.resize(boundary.controls.size());
            for (auto& channel : boundary.controlChannels)
                channel.resize(static_cast<std::size_t>(maximumBlockSize), 0.0f);
            boundary.buffer.setSize(
                HostMidiInputRuntime::kControlCount
                    + HostMidiInputRuntime::kOutputCount,
                maximumBlockSize, false, true, false);
            renderer->controls_.push_back(std::move(midiControls));
            renderer->hostMidiBoundaries_.push_back(std::move(boundary));
        }

        FaustGraphRenderer::ModuleControls outputControls;
        outputControls.graphModuleIndex = nativePolyOutput->index;
        FaustGraphRenderer::ControlRow level;
        level.kind = FaustGraphRenderer::ControlRow::Kind::BlockLast;
        level.rowMapIndex = 0;
        level.fallbackValue = 1.0f;
        level.minimumValue = 0.0f;
        level.maximumValue = 1.5f;
        if (const auto found = nativePolyOutput->paramValues.find("LEVEL");
            found != nativePolyOutput->paramValues.end())
            level.fallbackValue = std::clamp(found->second,
                                             level.minimumValue,
                                             level.maximumValue);
        level.lastValue = level.fallbackValue;
        outputControls.rows.push_back(std::move(level));
        renderer->nativePolyOutputControls_ = renderer->controls_.size();
        renderer->nativePolyOutputRow_ = 0;
        renderer->controls_.push_back(std::move(outputControls));

        renderer->tapIds_.reserve(2);
        renderer->tapOffsets_.reserve(2);
        renderer->tapWidths_.reserve(2);
        renderer->tapControlBoundaries_.reserve(2);
        renderer->tapControlOutputIds_.reserve(2);
        renderer->tapControlTelemetryOffsets_.reserve(2);
        std::size_t offset = 0;
        for (const auto& module : state.modules()) {
            const int width = isHostMidiSource(module) ? 0 : 2;
            renderer->tapIds_.push_back(module.moduleId);
            renderer->tapOffsets_.push_back(offset);
            renderer->tapWidths_.push_back(width);
            renderer->tapControlBoundaries_.push_back(
                isHostMidiSource(module));
            renderer->tapControlOutputIds_.push_back({});
            renderer->tapControlTelemetryOffsets_.push_back(0);
            if (module.index == nativePolySource->index)
                renderer->nativePolySourceOffset_ = offset;
            for (std::size_t effect = 0;
                 effect < nativePolyEffectChain.size(); ++effect)
                if (module.index == nativePolyEffectChain[effect]->index)
                    renderer->nativePolyEffectOffsets_[effect] = offset;
            if (isOutput(module)) {
                renderer->outputTap_ = renderer->tapIds_.size() - 1;
                renderer->nativePolyOutputOffset_ = offset;
            }
            offset += static_cast<std::size_t>(width);
        }
        renderer->outputs_.resize(offset);
        renderer->outputPointers_.resize(offset);
        for (std::size_t channel = 0; channel < offset; ++channel) {
            renderer->outputs_[channel].resize(
                static_cast<std::size_t>(maximumBlockSize), 0.0f);
            renderer->outputPointers_[channel] = renderer->outputs_[channel].data();
        }
        renderer->inputMixes_.resize(2);
        renderer->faceplateTelemetry_.resize(2);
        renderer->outputLevels_ = std::make_unique<
            FaustGraphRenderer::LevelHold[]>(2);
        renderer->inputLevels_ = std::make_unique<
            FaustGraphRenderer::LevelHold[]>(2);
        renderer->clips_ = std::make_unique<std::atomic<float>[]>(2);
        renderer->clips_[0].store(0.0f, std::memory_order_relaxed);
        renderer->clips_[1].store(0.0f, std::memory_order_relaxed);
        renderer->outputLevel_ = renderer->controls_[
            renderer->nativePolyOutputControls_].rows[0].fallbackValue;
        result.renderer = std::move(renderer);
        return result;
    }

    std::unordered_map<int, ProcessControlLayout> processControls;
    int processControlChannels = 0;
    for (const auto* entry : plan.modules) {
        if (! isFaust(*entry) && ! isOutput(*entry))
            continue;
        ProcessControlLayout layout;
        if (isOutput(*entry)) {
            RewrittenControl level;
            level.ident = "level";
            level.label = "LEVEL";
            level.role = vm::SignalType::Value;
            layout.controls.push_back(std::move(level));
            layout.rowStarts.push_back(0);
            layout.perVoice.push_back(false);
            layout.expandedRows = 1;
        } else {
            if (! prepareProcessControlLayout(*entry, layout, error)) {
                result.diagnostic = error;
                return result;
            }
        }
        if (layout.expandedRows
            > std::numeric_limits<int>::max() - processControlChannels) {
            result.diagnostic = "unified control input width is not representable";
            return result;
        }
        processControlChannels += layout.expandedRows;
        processControls.emplace(entry->index, std::move(layout));
    }

    int feedbackChannels = 0;
    for (const auto& edge : plan.feedbackEdges) {
        const auto isAutomatic = [&plan, &edge] (int index) {
            const auto module = std::find_if(
                plan.modules.begin(), plan.modules.end(),
                [index] (const ModuleEntry* candidate) {
                    return candidate->index == index;
                });
            return module != plan.modules.end()
                && (*module)->processingMode
                    == ModuleProcessingMode::AutomaticMultiMono;
        };
        if (isAutomatic(edge.srcIndex) || isAutomatic(edge.tgtIndex)) {
            result.diagnostic =
                "automatic multi-mono does not support one-sample feedback";
            return result;
        }
        const auto width = edge.signalDescriptor.width();
        if (width == 0 || width > static_cast<std::uint32_t>(
                std::numeric_limits<int>::max() - feedbackChannels)) {
            result.diagnostic = "declared feedback width is invalid";
            return result;
        }
        feedbackChannels += static_cast<int>(width);
    }
    std::vector<const ModuleEntry*> audioInputBoundaries;
    std::vector<const ModuleEntry*> hostAutomationBoundaries;
    std::vector<const ModuleEntry*> hostMidiBoundaries;
    std::vector<const ModuleEntry*> transportClockBoundaries;
    for (const auto* entry : plan.modules)
        if (isAudioInputBoundary(*entry))
            audioInputBoundaries.push_back(entry);
        else if (isHostAutomationSource(*entry))
            hostAutomationBoundaries.push_back(entry);
        else if (isHostMidiSource(*entry))
            hostMidiBoundaries.push_back(entry);
        else if (isTransportClockSource(*entry))
            transportClockBoundaries.push_back(entry);
    const int hostAudioInputChannels =
        static_cast<int>(audioInputBoundaries.size());
    int hostAutomationOutputChannels = 0;
    for (const auto* boundary : hostAutomationBoundaries)
        hostAutomationOutputChannels += static_cast<int>(
            boundary->controlOutputs.size());
    int transportClockOutputChannels = 0;
    for (const auto* boundary : transportClockBoundaries)
        transportClockOutputChannels += static_cast<int>(
            boundary->controlOutputs.size());
    const int externalInputChannels =
        hostAudioInputChannels + hostAutomationOutputChannels
        + static_cast<int>(hostMidiBoundaries.size())
            * HostMidiInputRuntime::kOutputCount
        + transportClockOutputChannels
        + processControlChannels;
    const int initialChannels = feedbackChannels + externalInputChannels;
    // Faust bounds an external process input to [-1, 1] during static range
    // analysis. Feeding real units (e.g. 500 ms) directly can undersize delay
    // tables. Transport controls as linear positions and restore the declared
    // interval before any musical composition or source-authored DSP. This
    // transport scale is not the parameter's modulation curve.
    std::vector<Box> inputDecoders(static_cast<std::size_t>(
        initialChannels - processControlChannels), boxWire());
    for (const auto* entry : plan.modules) {
        const auto layout = processControls.find(entry->index);
        if (layout == processControls.end()) continue;
        for (std::size_t control = 0; control < layout->second.controls.size(); ++control) {
            const auto* parameter = isOutput(*entry) ? nullptr
                : &entry->params[layout->second.parameterIndices[control]];
            const double minimum = parameter == nullptr ? 0.0 : parameter->min;
            const double maximum = parameter == nullptr ? 1.5 : parameter->max;
            std::ostringstream decoder;
            decoder << std::setprecision(17) << "process(x) = " << minimum
                    << " + min(1.0, max(0.0, x)) * " << maximum - minimum << ";\n";
            auto decoded = parseBox("control-transport-" + entry->moduleId,
                                    decoder.str(), arguments, 1, 1, error);
            if (decoded == nullptr) {
                result.diagnostic = error;
                return result;
            }
            const int rows = layout->second.perVoice[control]
                ? std::max(1, entry->physicalVoices) : 1;
            inputDecoders.insert(inputDecoders.end(), static_cast<std::size_t>(rows), decoded);
        }
    }
    Box chain = initialChannels > 0 ? parallelBoxes(inputDecoders) : nullptr;
    int retainedChannels = initialChannels;
    std::unordered_map<int, std::pair<int, int>> moduleOutputs;
    std::unordered_map<int, int> moduleTotalOutputWidths;
    std::unordered_map<int, int> moduleInputWidths;
    std::unordered_map<int, const ModuleEntry*> moduleByIndex;
    for (const auto& module : state.modules())
        moduleByIndex[module.index] = &module;
    std::unordered_map<int, int> externalInputOffsets;
    for (int input = 0; input < hostAudioInputChannels; ++input)
        externalInputOffsets[audioInputBoundaries[static_cast<std::size_t>(input)]->index]
            = feedbackChannels + input;
    int hostAutomationOffset = feedbackChannels + hostAudioInputChannels;
    for (const auto* boundary : hostAutomationBoundaries) {
        externalInputOffsets[boundary->index] = hostAutomationOffset;
        hostAutomationOffset += static_cast<int>(
            boundary->controlOutputs.size());
    }
    for (const auto* boundary : hostMidiBoundaries) {
        externalInputOffsets[boundary->index] = hostAutomationOffset;
        hostAutomationOffset += HostMidiInputRuntime::kOutputCount;
    }
    for (const auto* boundary : transportClockBoundaries) {
        externalInputOffsets[boundary->index] = hostAutomationOffset;
        hostAutomationOffset += static_cast<int>(
            boundary->controlOutputs.size());
    }
    std::unordered_map<int, int> processControlOffsets;
    int processControlOffset = feedbackChannels + hostAudioInputChannels
        + hostAutomationOutputChannels;
    processControlOffset += static_cast<int>(hostMidiBoundaries.size())
        * HostMidiInputRuntime::kOutputCount;
    processControlOffset += transportClockOutputChannels;
    for (const auto* entry : plan.modules) {
        const auto found = processControls.find(entry->index);
        if (found == processControls.end())
            continue;
        processControlOffsets[entry->index] = processControlOffset;
        processControlOffset += found->second.expandedRows;
    }
    float outputLevel = 1.0f;
    for (const auto* entry : plan.modules) {
        if (isAudioInputBoundary(*entry)) {
            if (entry->audioInputChannelIndex
                > static_cast<std::uint32_t>(
                    std::numeric_limits<int>::max())) {
                result.diagnostic = "audio input channel index is not representable";
                return result;
            }
            moduleInputWidths[entry->index] = 0;
            moduleOutputs[entry->index] = {
                externalInputOffsets.at(entry->index), 1 };
            moduleTotalOutputWidths[entry->index] = 1;
            continue;
        }
        if (isHostAutomationSource(*entry)) {
            if (entry->controlOutputs.size()
                != static_cast<std::size_t>(
                    HostAutomationInputProcessor::kOutputCount)) {
                result.diagnostic = "Host Automation requires eight control outputs";
                return result;
            }
            moduleInputWidths[entry->index] = 0;
            moduleOutputs[entry->index] = {
                externalInputOffsets.at(entry->index), 0 };
            moduleTotalOutputWidths[entry->index] = static_cast<int>(
                entry->controlOutputs.size());
            continue;
        }
        if (isHostMidiSource(*entry)) {
            if (entry->controlOutputs.size() != 4u) {
                result.diagnostic = "MIDI In requires four control outputs";
                return result;
            }
            moduleInputWidths[entry->index] = 0;
            moduleOutputs[entry->index] = {
                externalInputOffsets.at(entry->index), 0 };
            moduleTotalOutputWidths[entry->index] = 4;
            continue;
        }
        if (isTransportClockSource(*entry)) {
            if (entry->controlOutputs.size()
                != static_cast<std::size_t>(
                    TransportClockRuntime::OutputCount)) {
                result.diagnostic =
                    "Transport Clock requires eight control outputs";
                return result;
            }
            moduleInputWidths[entry->index] = 0;
            moduleOutputs[entry->index] = {
                externalInputOffsets.at(entry->index), 0 };
            moduleTotalOutputWidths[entry->index] =
                TransportClockRuntime::OutputCount;
            continue;
        }
        Box moduleBox = nullptr;
        int moduleInputs = 0;
        int moduleAudioInputs = 0;
        int moduleOutputCount = 0;
        const auto scriptRoutes = plan.scriptRoutes.find(entry->index);
        const bool isScriptRoute = scriptRoutes != plan.scriptRoutes.end();
        if (isScriptRoute) {
            std::string routeSource;
            if (! scriptRouteCode(*entry, scriptRoutes->second,
                                  routeSource, error)) {
                result.diagnostic = error;
                return result;
            }
            moduleBox = parseBox(entry->moduleId, routeSource, arguments,
                                 static_cast<int>(entry->controlInputs.size()),
                                 static_cast<int>(entry->controlOutputs.size()),
                                 error);
        } else if (isOutput(*entry)) {
            float level = 1.0f;
            if (const auto found = entry->paramValues.find("LEVEL");
                found != entry->paramValues.end())
                level = std::clamp(found->second, 0.0f, 1.5f);
            outputLevel = level;
            std::ostringstream outputCode;
            outputCode << std::setprecision(9)
                       << "limit(x) = min(1.0, max(-1.0, x));\n"
                       << "retainLevel(level, signal) = attach(signal, "
                          "level : hbargraph(\"__curlop_output_level\", "
                          "0, 1.5));\n"
                       << "process(level, left, right) = "
                       << "retainLevel(level, limit(left * level)), "
                          "limit(right * level);\n";
            moduleBox = parseBox(entry->moduleId, outputCode.str(), arguments,
                                 3, 2, error);
            if (moduleBox != nullptr)
                moduleBox = boxVGroup(entry->moduleId.c_str(), moduleBox);
        } else {
            moduleBox = parseBox(entry->moduleId,
                                 processControls.at(entry->index).source,
                                 arguments,
                                 -1, -1, error);
        }
        if (moduleBox == nullptr
            || ! getBoxType(moduleBox, &moduleInputs, &moduleOutputCount)) {
            result.diagnostic = error.empty()
                ? "Faust could not type module " + entry->moduleId : error;
            return result;
        }
        if (isScriptRoute) {
            moduleAudioInputs = 0;
        } else if (isOutput(*entry)) {
            moduleAudioInputs = 2;
        } else if (isFaust(*entry)) {
            const auto& layout = processControls.at(entry->index);
            const int baseControls = static_cast<int>(layout.controls.size());
            moduleAudioInputs = moduleInputs - baseControls;
            if (moduleAudioInputs < 0) {
                result.diagnostic = "module " + entry->moduleId
                    + " process-input rewrite has an invalid audio tail";
                return result;
            }
        } else {
            moduleAudioInputs = moduleInputs;
        }
        const bool automaticMultiMono = isFaust(*entry)
            && entry->processingMode == ModuleProcessingMode::AutomaticMultiMono;
        if (automaticMultiMono) {
            const auto& layout = processControls.at(entry->index);
            if (entry->physicalVoices > 1
                || ! faustProcessingCapabilitiesFromSource(entry->code).independentMono
                || moduleAudioInputs != 1
                || moduleOutputCount != 1
                || ! entry->controlOutputs.empty()) {
                result.diagnostic = "automatic multi-mono module " + entry->moduleId
                    + " requires declared independent-mono, exact mono audio I/O, "
                      "no control outputs, and no physical polyphony";
                return result;
            }
            int lanes = 0;
            if (const auto incoming = plan.incoming.find(entry->index);
                incoming != plan.incoming.end()) {
                for (const auto& edge : incoming->second) {
                    const auto* source = moduleByIndex.at(edge.srcIndex);
                    if (isNamedControlEdge(*source, *entry, edge))
                        continue;
                    if (edge.signalDescriptor.rate() != SignalRate::Audio
                        || edge.signalDescriptor.semanticRole() != "audio")
                        continue;
                    int width = static_cast<int>(edge.signalDescriptor.width());
                    if (source->processingMode
                            == ModuleProcessingMode::AutomaticMultiMono) {
                        const auto preparedSource = moduleOutputs.find(edge.srcIndex);
                        if (preparedSource != moduleOutputs.end())
                            width = preparedSource->second.second;
                    }
                    if (width <= 0 || (lanes != 0 && lanes != width)) {
                        result.diagnostic = "automatic multi-mono module " + entry->moduleId
                            + " has conflicting incoming audio widths";
                        return result;
                    }
                    lanes = width;
                }
            }
            lanes = std::max(1, lanes);
            moduleBox = processInputIndependentMonoBox(
                entry->moduleId, moduleBox,
                static_cast<int>(layout.controls.size()), lanes);
            moduleInputs = static_cast<int>(layout.controls.size()) + lanes;
            moduleAudioInputs = lanes;
            moduleOutputCount = lanes;
        }
        moduleInputWidths[entry->index] = moduleAudioInputs;
        const int controlOutputCount =
            static_cast<int>(entry->controlOutputs.size());
        const int meterOutputCount = isFaust(*entry)
            ? processControls.at(entry->index).meterOutputCount : 0;
        // Faust output order is a boundary contract: real audio first,
        // [curlop:cvout] channels next, and [curlop:meterout] telemetry last.
        // Retaining the prefix therefore preserves cvout channel indices while
        // dropping meterout from the fused audio graph.
        const int retainedOutputCount = moduleOutputCount - meterOutputCount;
        const int audioOutputCount = retainedOutputCount - controlOutputCount;
        if ((! isScriptRoute && audioOutputCount <= 0 && controlOutputCount <= 0)
            || (isScriptRoute && audioOutputCount != 0)
            || controlOutputCount + meterOutputCount > moduleOutputCount) {
            result.diagnostic = "module " + entry->moduleId
                + (isScriptRoute
                    ? " has an invalid scalar route output contract"
                    : " has no retained audio or named control output channels");
            return result;
        }
        if (! isOutput(*entry) && ! isScriptRoute) {
            const int voices = std::max(1, entry->physicalVoices);
            if (voices > 1 && controlOutputCount > 0) {
                result.diagnostic = "polyphonic named control outputs require "
                    "an explicit voice-reduction contract";
                return result;
            }
            if (! automaticMultiMono) {
                const auto& layout = processControls.at(entry->index);
                moduleBox = processInputVoicePoolBox(
                    entry->moduleId, moduleBox,
                    static_cast<int>(layout.controls.size()), moduleAudioInputs,
                    moduleOutputCount, voices, layout, arguments, error);
                if (moduleBox == nullptr) {
                    result.diagnostic = error.empty()
                        ? "Faust could not build fixed voice pool for "
                            + entry->moduleId
                        : error;
                    return result;
                }
                moduleInputs = layout.expandedRows + moduleAudioInputs;
            }
        }
        if (meterOutputCount > 0) {
            std::vector<std::pair<int, int>> retainedChannels;
            retainedChannels.reserve(static_cast<std::size_t>(retainedOutputCount));
            for (int channel = 0; channel < retainedOutputCount; ++channel)
                retainedChannels.emplace_back(channel, channel);
            moduleBox = boxSeq(
                moduleBox,
                routeBox(moduleOutputCount, retainedOutputCount,
                         retainedChannels));
        }

        const auto incoming = plan.incoming.find(entry->index);
        const std::vector<EdgeEffective> noIncoming;
        const auto& incomingEdges = incoming == plan.incoming.end()
            ? noIncoming : incoming->second;
        std::vector<Box> feedBoxes;
        std::vector<int> feedSourceChannels;

        if (isOutput(*entry)) {
            feedSourceChannels.push_back(
                processControlOffsets.at(entry->index));
            feedBoxes.push_back(boxWire());
        } else if (isScriptRoute) {
            for (const auto& inputName : entry->controlInputs) {
                std::vector<const EdgeEffective*> inputEdges;
                for (const auto& edge : incomingEdges)
                    if (edge.tgtPort == inputName
                        && scriptRouteUsesInput(scriptRoutes->second,
                                                edge.tgtPort))
                        inputEdges.push_back(&edge);

                if (inputEdges.empty()) {
                    feedBoxes.push_back(boxReal(0.0));
                    continue;
                }

                std::vector<Box> wires;
                wires.reserve(inputEdges.size());
                for (std::size_t edgeIndex = 0;
                     edgeIndex < inputEdges.size(); ++edgeIndex) {
                    const auto& edge = *inputEdges[edgeIndex];
                    const auto* source = moduleByIndex.at(edge.srcIndex);
                    const auto preparedSource = moduleOutputs.find(edge.srcIndex);
                    const int output = namedControlOutputIndex(
                        *source, edge.srcPort);
                    if (preparedSource == moduleOutputs.end() || output < 0) {
                        result.diagnostic = "Script V2 route input "
                            + entry->moduleId + "." + inputName
                            + " has no prepared scalar source";
                        return result;
                    }
                    feedSourceChannels.push_back(
                        preparedSource->second.first
                        + preparedSource->second.second + output);
                    auto wire = parseBox(
                        "script-route-wire-" + entry->moduleId + "-"
                            + inputName + "-" + std::to_string(edgeIndex),
                        wireCode(1, edge.audible ? edge.gain : 0.0f,
                                 0.0f, false),
                        arguments, 1, 1, error);
                    if (wire == nullptr) {
                        result.diagnostic = error;
                        return result;
                    }
                    wires.push_back(wire);
                }
                Box inputFeed = parallelBoxes(wires);
                if (wires.size() > 1) {
                    std::vector<std::pair<int, int>> sum;
                    sum.reserve(wires.size());
                    for (int edge = 0;
                         edge < static_cast<int>(wires.size()); ++edge)
                        sum.emplace_back(edge, 0);
                    inputFeed = boxSeq(inputFeed, routeBox(
                        static_cast<int>(wires.size()), 1, sum));
                }
                feedBoxes.push_back(inputFeed);
            }
        } else if (isFaust(*entry)) {
            const auto& layout = processControls.at(entry->index);
            const int baseOffset = processControlOffsets.at(entry->index);
            for (int control = 0;
                 control < static_cast<int>(layout.controls.size()); ++control) {
                const int rows = layout.perVoice[static_cast<std::size_t>(control)]
                    ? std::max(1, entry->physicalVoices) : 1;
                std::vector<const EdgeEffective*> controlEdges;
                for (const auto& edge : incomingEdges) {
                    const auto* source = moduleByIndex.at(edge.srcIndex);
                    const auto mapped = edge.modulation
                        ? resolveModulationMapping(*source, *entry, *edge.modulation)
                        : std::nullopt;
                    const int targetIndex = mapped
                        ? static_cast<int>(mapped->targetParameter)
                        : exposedControlInputIndex(*entry, edge.tgtPort);
                    if (isNamedControlEdge(*source, *entry, edge)
                        && targetIndex
                            == static_cast<int>(layout.parameterIndices[
                                static_cast<std::size_t>(control)])
                        && edge.audible && (edge.modulation || edge.gain > 0.0f))
                        controlEdges.push_back(&edge);
                }
                for (int voice = 0; voice < rows; ++voice) {
                    feedSourceChannels.push_back(baseOffset
                        + layout.rowStarts[static_cast<std::size_t>(control)]
                        + voice);
                    if (controlEdges.empty()) {
                        feedBoxes.push_back(boxWire());
                        continue;
                    }
                    std::vector<ControlEdgeTransform> transforms;
                    transforms.reserve(controlEdges.size());
                    const auto& parameter = entry->params[
                        layout.parameterIndices[static_cast<std::size_t>(control)]];
                    const auto targetControl = faustControlInputOf(
                        parameter.name, parameter.type == ParamType::Boolean,
                        parameter.defaultValue, parameter.min, parameter.max,
                        parameter.scale
                            == Scale::Logarithmic ? "log" : "");
                    for (const auto* edge : controlEdges) {
                        const auto* source = moduleByIndex.at(edge->srcIndex);
                        const int output = namedControlOutputIndex(
                            *source, edge->srcPort);
                        const auto preparedSource = moduleOutputs.find(edge->srcIndex);
                        if (output < 0 || preparedSource == moduleOutputs.end()) {
                            result.diagnostic = "named control source is unavailable for "
                                + entry->moduleId + "." + edge->tgtPort;
                            return result;
                        }
                        feedSourceChannels.push_back(
                            preparedSource->second.first
                            + preparedSource->second.second + output);
                        const auto sourceType = output < static_cast<int>(
                                source->controlOutputTypes.size())
                            ? source->controlOutputTypes[static_cast<std::size_t>(output)]
                            : vm::SignalType::Value;
                        const bool setValue = ! edge->modulation && sourceType == targetControl.type
                            && (sourceType == vm::SignalType::Pitch
                                || sourceType == vm::SignalType::Gate
                                || sourceType == vm::SignalType::Velocity);
                        transforms.push_back({ edge->gain, setValue,
                            setValue && sourceType == vm::SignalType::Pitch
                                && targetControl.unit == vm::InputUnit::Hz, edge->modulation });
                    }
                    auto merge = parseBox(
                        "control-" + entry->moduleId + "-"
                            + std::to_string(control) + "-"
                            + std::to_string(voice),
                        controlMergeCode(
                            parameter,
                            transforms),
                        arguments, 1 + static_cast<int>(controlEdges.size()),
                        1, error);
                    if (merge == nullptr) {
                        result.diagnostic = error;
                        return result;
                    }
                    feedBoxes.push_back(merge);
                }
            }
        }

        std::vector<const EdgeEffective*> audioEdges;
        for (const auto& edge : incomingEdges) {
            const auto* source = moduleByIndex.at(edge.srcIndex);
            const bool routeInput = isScriptRoute
                && scriptRouteUsesInput(scriptRoutes->second, edge.tgtPort)
                && namedControlOutputIndex(*source, edge.srcPort) >= 0;
            if (! isNamedControlEdge(*source, *entry, edge) && ! routeInput)
                audioEdges.push_back(&edge);
        }
        if (moduleAudioInputs > 0) {
            if (audioEdges.empty()) {
                // A temporarily disconnected audio module is a normal graph
                // editing state. Feed deterministic silence so one dangling
                // effect cannot prevent transport from publishing the rest of
                // the graph; the Output boundary uses the same rule.
                for (int channel = 0; channel < moduleAudioInputs; ++channel)
                    feedBoxes.push_back(boxReal(0.0));
            } else {
                std::vector<Box> audioWires;
                for (std::size_t edgeIndex = 0;
                     edgeIndex < audioEdges.size(); ++edgeIndex) {
                    const auto& edge = *audioEdges[edgeIndex];
                    // Automatic multi-mono preserves the source module's
                    // declared mono port while lowering it to the incoming
                    // lane width.  Downstream edges therefore carry the
                    // prepared source width, rather than their static
                    // single-channel descriptor.
                    int width = static_cast<int>(edge.signalDescriptor.width());
                    int sourceOffset = -1;
                    int sourceWidth = -1;
                    if (edge.feedbackBoundary == FeedbackBoundary::OneSample) {
                        int feedbackOffset = 0;
                        for (const auto& preparedFeedback : plan.feedbackEdges) {
                            if (preparedFeedback.srcIndex == edge.srcIndex
                                && preparedFeedback.tgtIndex == edge.tgtIndex
                                && preparedFeedback.srcPort == edge.srcPort
                                && preparedFeedback.tgtPort == edge.tgtPort) {
                                sourceOffset = feedbackOffset;
                                sourceWidth = static_cast<int>(
                                    preparedFeedback.signalDescriptor.width());
                                break;
                            }
                            feedbackOffset += static_cast<int>(
                                preparedFeedback.signalDescriptor.width());
                        }
                    } else {
                        const auto source = moduleOutputs.find(edge.srcIndex);
                        if (source != moduleOutputs.end()) {
                            sourceOffset = source->second.first;
                            sourceWidth = source->second.second;
                            if (sourceWidth > 0
                                && moduleByIndex.at(edge.srcIndex)->processingMode
                                    == ModuleProcessingMode::AutomaticMultiMono)
                                width = sourceWidth;
                        }
                    }
                    if (sourceOffset < 0 || width != moduleAudioInputs
                        || sourceWidth != width) {
                        result.diagnostic = "edge into " + entry->moduleId
                            + " has width " + std::to_string(width)
                            + " but its source/target audio contract is "
                            + std::to_string(sourceWidth) + "/"
                            + std::to_string(moduleAudioInputs);
                        return result;
                    }
                    for (int channel = 0; channel < width; ++channel)
                        feedSourceChannels.push_back(sourceOffset + channel);
                    const auto* descriptorLayout = edge.signalDescriptor.layout()
                        ? &*edge.signalDescriptor.layout() : nullptr;
                    const bool panApplicable =
                        edge.feedbackBoundary != FeedbackBoundary::OneSample
                        && width == 2 && descriptorLayout != nullptr
                        && *descriptorLayout == "stereo";
                    auto wire = parseBox(
                        "wire-" + entry->moduleId + "-"
                            + std::to_string(edgeIndex),
                        wireCode(width, edge.gain, edge.pan, panApplicable),
                        arguments, width, width, error);
                    if (wire == nullptr) {
                        result.diagnostic = error;
                        return result;
                    }
                    audioWires.push_back(wire);
                }
                Box audioFeed = parallelBoxes(audioWires);
                if (audioWires.size() > 1) {
                    std::vector<std::pair<int, int>> sums;
                    for (int edge = 0; edge < static_cast<int>(audioWires.size()); ++edge)
                        for (int channel = 0; channel < moduleAudioInputs; ++channel)
                            sums.emplace_back(edge * moduleAudioInputs + channel, channel);
                    audioFeed = boxSeq(audioFeed, routeBox(
                        static_cast<int>(audioWires.size()) * moduleAudioInputs,
                        moduleAudioInputs, sums));
                }
                feedBoxes.push_back(audioFeed);
            }
        }

        if (moduleInputs == 0) {
            chain = chain == nullptr ? moduleBox : boxPar(chain, moduleBox);
        } else {
            if (feedBoxes.empty()) {
                result.diagnostic = "module " + entry->moduleId
                    + " has an invalid prepared input contract";
                return result;
            }
            if (chain == nullptr) {
                if (! feedSourceChannels.empty()) {
                    result.diagnostic = "module " + entry->moduleId
                        + " refers to an unavailable prepared source";
                    return result;
                }
                chain = boxSeq(parallelBoxes(feedBoxes), moduleBox);
                moduleOutputs[entry->index] = {
                    retainedChannels, audioOutputCount };
                moduleTotalOutputWidths[entry->index] = retainedOutputCount;
                retainedChannels += retainedOutputCount;
                continue;
            }
            std::vector<std::pair<int, int>> duplicatePairs;
            duplicatePairs.reserve(static_cast<std::size_t>(retainedChannels)
                + feedSourceChannels.size());
            for (int channel = 0; channel < retainedChannels; ++channel)
                duplicatePairs.emplace_back(channel, channel);
            for (int input = 0;
                 input < static_cast<int>(feedSourceChannels.size()); ++input)
                duplicatePairs.emplace_back(
                    feedSourceChannels[static_cast<std::size_t>(input)],
                    retainedChannels + input);
            chain = boxSeq(chain, routeBox(
                retainedChannels,
                retainedChannels + static_cast<int>(feedSourceChannels.size()),
                duplicatePairs));
            chain = boxSeq(chain, boxPar(
                parallelWires(retainedChannels), parallelBoxes(feedBoxes)));
            chain = boxSeq(chain, boxPar(
                parallelWires(retainedChannels), moduleBox));
        }
        moduleOutputs[entry->index] = { retainedChannels, audioOutputCount };
        moduleTotalOutputWidths[entry->index] = retainedOutputCount;
        retainedChannels += retainedOutputCount;
    }

    std::vector<std::pair<int, int>> tapOrder;
    tapOrder.reserve(static_cast<std::size_t>(retainedChannels));
    int orderedChannel = feedbackChannels;
    int feedbackOutput = 0;
    for (const auto& edge : plan.feedbackEdges) {
        const auto source = moduleOutputs.find(edge.srcIndex);
        const int width = static_cast<int>(edge.signalDescriptor.width());
        if (source == moduleOutputs.end() || source->second.second != width) {
            result.diagnostic = "declared feedback source contract is unavailable";
            return result;
        }
        for (int channel = 0; channel < width; ++channel)
            tapOrder.emplace_back(source->second.first + channel,
                                  feedbackOutput++);
    }
    for (const auto& entry : state.modules()) {
        const bool preparedScriptRoute =
            plan.scriptRoutes.count(entry.index) != 0;
        if (isNonAudioBoundary(entry) && ! preparedScriptRoute)
            continue;
        const auto prepared = moduleOutputs.find(entry.index);
        if (prepared == moduleOutputs.end()) {
            result.diagnostic = "module tap order is incomplete";
            return result;
        }
        const auto total = moduleTotalOutputWidths.find(entry.index);
        if (total == moduleTotalOutputWidths.end()) {
            result.diagnostic = "module total-output contract is unavailable";
            return result;
        }
        for (int channel = 0; channel < total->second; ++channel)
            tapOrder.emplace_back(prepared->second.first + channel,
                                  orderedChannel++);
    }
    const int tapChannels = orderedChannel - feedbackChannels;
    chain = boxSeq(chain, routeBox(
        retainedChannels, feedbackChannels + tapChannels, tapOrder));
    if (feedbackChannels > 0) {
        chain = boxRec(chain, parallelWires(feedbackChannels));
        std::vector<std::pair<int, int>> dropFeedback;
        dropFeedback.reserve(static_cast<std::size_t>(tapChannels));
        for (int channel = 0; channel < tapChannels; ++channel)
            dropFeedback.emplace_back(feedbackChannels + channel, channel);
        chain = boxSeq(chain, routeBox(
            feedbackChannels + tapChannels, tapChannels, dropFeedback));
    }

    renderer = std::make_unique<FaustGraphRenderer>();
    const auto factoryStart = RendererBuildClock::now();
#if JUCE_IOS
    renderer->factory_ = createInterpreterDSPFactoryFromBoxes(
        "curlop-faust-graph", chain, static_cast<int>(arguments.size()),
        arguments.data(), error);
#else
    renderer->factory_ = createDSPFactoryFromBoxes(
        "curlop-faust-graph", chain, static_cast<int>(arguments.size()),
        arguments.data(), "", error, -1);
#endif
    result.timing.factoryUs += rendererBuildElapsedUs(factoryStart);
    if (renderer->factory_ == nullptr) {
        result.diagnostic = error;
        return result;
    }
    const auto instanceStart = RendererBuildClock::now();
    renderer->instance_.reset(renderer->factory_->createDSPInstance());
    if (renderer->instance_ == nullptr) {
        result.diagnostic = "Faust factory did not create a DSP instance";
        return result;
    }
    renderer->instance_->init(static_cast<int>(std::lround(sampleRate)));
    result.timing.instanceInitUs += rendererBuildElapsedUs(instanceStart);
    renderer->blockSize_ = maximumBlockSize;
    renderer->audioInputBoundaries_.reserve(audioInputBoundaries.size());
    for (const auto* input : audioInputBoundaries)
        renderer->audioInputBoundaries_.push_back({
            input->index, input->audioInputChannelIndex, nullptr
        });
    renderer->inputPointers_.resize(
        static_cast<std::size_t>(externalInputChannels), nullptr);
    renderer->silentInput_.resize(
        static_cast<std::size_t>(maximumBlockSize), 0.0f);
    ZonePathCaptureUI controlUi;
    renderer->instance_->buildUserInterface(&controlUi);
    for (const auto& module : state.modules()) {
        FaustGraphRenderer::ModuleControls controls;
        controls.graphModuleIndex = module.index;
        if (isNonAudioBoundary(module)) {
            // The renderer publishes requested streams through this module's
            // native event/render boundary. It is not a Faust audio node, but
            // VMRunner still binds every graph module by identity.
            if (module.lineageId == "core.visual_shader") {
                controls.rows.reserve(module.params.size());
                for (std::size_t parameter = 0;
                     parameter < module.params.size(); ++parameter) {
                    FaustGraphRenderer::ControlRow row;
                    row.rowMapIndex = static_cast<int>(parameter);
                    row.fallbackValue = module.params[parameter].defaultValue;
                    row.minimumValue = module.params[parameter].min;
                    row.maximumValue = module.params[parameter].max;
                    controls.rows.push_back(std::move(row));
                }
            } else if (isMidiEventBoundary(module)) {
                controls.rows.reserve(module.params.size());
                for (std::size_t parameter = 0;
                     parameter < module.params.size(); ++parameter) {
                    FaustGraphRenderer::ControlRow row;
                    // buildSynthesizedDeclaration deduplicates the canonical
                    // GATE/PITCH/VELOCITY schema entries against its note bus,
                    // so MIDI Out's six controls retain schema order.
                    row.rowMapIndex = static_cast<int>(parameter);
                    row.fallbackValue = module.params[parameter].defaultValue;
                    row.minimumValue = module.params[parameter].min;
                    row.maximumValue = module.params[parameter].max;
                    controls.rows.push_back(std::move(row));
                }
            }
            renderer->controls_.push_back(std::move(controls));
            continue;
        }
        if (isHostAutomationSource(module)) {
            controls.rows.reserve(module.params.size());
            for (std::size_t parameter = 0;
                 parameter < module.params.size(); ++parameter) {
                FaustGraphRenderer::ControlRow row;
                // The synthesized native declaration keeps its note bus
                // before the eight automation-lane selectors.
                row.rowMapIndex = 3 + static_cast<int>(parameter);
                row.fallbackValue = module.params[parameter].defaultValue;
                row.minimumValue = module.params[parameter].min;
                row.maximumValue = module.params[parameter].max;
                controls.rows.push_back(std::move(row));
            }
            renderer->controls_.push_back(std::move(controls));
            continue;
        }
        if (isHostMidiSource(module)) {
            controls.rows.reserve(module.params.size());
            for (std::size_t parameter = 0; parameter < module.params.size(); ++parameter) {
                FaustGraphRenderer::ControlRow row;
                row.rowMapIndex = 3 + static_cast<int>(parameter);
                row.fallbackValue = module.params[parameter].defaultValue;
                row.minimumValue = module.params[parameter].min;
                row.maximumValue = module.params[parameter].max;
                controls.rows.push_back(std::move(row));
            }
            renderer->controls_.push_back(std::move(controls));
            continue;
        }
        if (isTransportClockSource(module)) {
            renderer->controls_.push_back(std::move(controls));
            continue;
        }
        if (isOutput(module)) {
            FaustGraphRenderer::ControlRow row;
            row.kind = FaustGraphRenderer::ControlRow::Kind::BlockLast;
            // Native modules without a source declaration use
            // buildSynthesizedDeclaration(includeNoteBus=true): gate, pitch,
            // velocity, then schema values. LEVEL is output's first schema
            // value and therefore declaration row 3.
            row.rowMapIndex = 3;
            row.fallbackValue = outputLevel;
            row.nonFiniteValue = 1.0f;
            row.minimumValue = 0.0f;
            row.maximumValue = 1.5f;
            row.reportsOutputLevel = true;
            controls.rows.push_back(std::move(row));
            renderer->controls_.push_back(std::move(controls));
            continue;
        }
        if (module.params.empty()) {
            // VMRunner binds every ModuleVm, including parameterless Faust
            // sources/effects. Retain a no-op binding target so an eligible
            // module cannot reject the whole immutable bundle.
            renderer->controls_.push_back(std::move(controls));
            continue;
        }
        if (isFaust(module)) {
            const auto& layout = processControls.at(module.index);
            const int voices = std::max(1, module.physicalVoices);
            controls.rows.reserve(static_cast<std::size_t>(layout.expandedRows)
                + (voices > 1 ? 1u : 0u));
            std::vector<int> schemaRowStarts(module.params.size(), 0);
            int schemaRows = 0;
            for (std::size_t parameter = 0; parameter < module.params.size(); ++parameter) {
                schemaRowStarts[parameter] = schemaRows;
                const auto classified = faustControlInputOf(
                    module.params[parameter].name,
                    module.params[parameter].type == ParamType::Boolean,
                    module.params[parameter].defaultValue,
                    module.params[parameter].min,
                    module.params[parameter].max,
                    module.params[parameter].scale == Scale::Logarithmic ? "log" : "");
                schemaRows += (classified.type == vm::SignalType::Gate
                    || classified.type == vm::SignalType::Pitch
                    || classified.type == vm::SignalType::Velocity) ? voices : 1;
            }
            if (schemaRows != layout.expandedRows) {
                result.diagnostic = "module " + module.moduleId
                    + " has incompatible schema/process control widths";
                return result;
            }
            for (std::size_t control = 0; control < layout.controls.size(); ++control) {
                const auto parameterIndex = layout.parameterIndices[control];
                const auto& parameter = module.params[parameterIndex];
                const int rows = layout.perVoice[control] ? voices : 1;
                for (int voice = 0; voice < rows; ++voice) {
                    FaustGraphRenderer::ControlRow row;
                    row.rowMapIndex = schemaRowStarts[parameterIndex] + voice;
                    row.fallbackValue = parameter.defaultValue;
                    row.minimumValue = parameter.min;
                    row.maximumValue = parameter.max;
                    controls.rows.push_back(std::move(row));
                }
            }
            if (voices > 1) {
                FaustGraphRenderer::ControlRow cap;
                cap.kind = FaustGraphRenderer::ControlRow::Kind::VoiceCap;
                cap.rowMapIndex = static_cast<int>(controls.rows.size());
                for (int voice = 0; voice < voices; ++voice) {
                    FAUSTFLOAT* zone = nullptr;
                    const auto voicePath = "/voice-" + std::to_string(voice)
                        + "/";
                    for (const auto& candidate : controlUi.zones()) {
                        const auto modulePath = "/" + module.moduleId + "/";
                        if (candidate.path.find(modulePath) == std::string::npos
                            || candidate.path.find(voicePath) == std::string::npos
                            || candidate.label != "curlop_voice_active")
                            continue;
                        zone = candidate.value;
                        break;
                    }
                    if (zone == nullptr) {
                        result.diagnostic = "Faust active-voice zone "
                            + module.moduleId + " voice "
                            + std::to_string(voice) + " is unavailable";
                        return result;
                    }
                    cap.zones.push_back(zone);
                }
                controls.rows.push_back(std::move(cap));
            }
            renderer->controls_.push_back(std::move(controls));
            continue;
        }
        const int voices = std::max(1, module.physicalVoices);
        controls.rows.reserve(module.params.size()
            * static_cast<std::size_t>(voices) + 1u);
        for (const auto& parameter : module.params) {
            const auto expectedName = faustLabelToParamName(parameter.name);
            const auto classified = faustControlInputOf(
                parameter.name, parameter.type == ParamType::Boolean,
                parameter.defaultValue, parameter.min, parameter.max,
                parameter.scale == Scale::Logarithmic ? "log" : "");
            const bool perVoice = classified.type == vm::SignalType::Gate
                || classified.type == vm::SignalType::Pitch
                || classified.type == vm::SignalType::Velocity;
            const int rowCount = perVoice ? voices : 1;
            for (int rowVoice = 0; rowVoice < rowCount; ++rowVoice) {
                FaustGraphRenderer::ControlRow row;
                row.rowMapIndex = static_cast<int>(controls.rows.size());
                const int firstVoice = perVoice ? rowVoice : 0;
                const int lastVoice = perVoice ? rowVoice + 1 : voices;
                for (int voice = firstVoice; voice < lastVoice; ++voice) {
                    FAUSTFLOAT* zone = nullptr;
                    const auto voicePath = "/voice-" + std::to_string(voice)
                        + "/";
                    for (const auto& candidate : controlUi.zones()) {
                        const auto modulePath = "/" + module.moduleId + "/";
                        const bool sourcePathMatches = parameter.sourceId.empty()
                            || (candidate.path.size() >= parameter.sourceId.size()
                                && candidate.path.compare(
                                    candidate.path.size() - parameter.sourceId.size(),
                                    parameter.sourceId.size(), parameter.sourceId) == 0);
                        if (candidate.path.find(modulePath)
                                == std::string::npos
                            || (voices > 1
                                && candidate.path.find(voicePath)
                                    == std::string::npos)
                            || ! sourcePathMatches
                            || faustLabelToParamName(candidate.label)
                                != expectedName)
                            continue;
                        zone = candidate.value;
                        break;
                    }
                    if (zone == nullptr) {
                        result.diagnostic = "Faust control zone "
                            + module.moduleId + "." + parameter.name
                            + " voice " + std::to_string(voice)
                            + " is unavailable";
                        return result;
                    }
                    row.zones.push_back(zone);
                }
                controls.rows.push_back(std::move(row));
            }
        }
        if (voices > 1) {
            FaustGraphRenderer::ControlRow cap;
            cap.kind = FaustGraphRenderer::ControlRow::Kind::VoiceCap;
            cap.rowMapIndex = static_cast<int>(controls.rows.size());
            for (int voice = 0; voice < voices; ++voice) {
                FAUSTFLOAT* zone = nullptr;
                const auto voicePath = "/voice-" + std::to_string(voice)
                    + "/";
                for (const auto& candidate : controlUi.zones()) {
                    const auto modulePath = "/" + module.moduleId + "/";
                    if (candidate.path.find(modulePath)
                            == std::string::npos
                        || candidate.path.find(voicePath) == std::string::npos
                        || candidate.label != "curlop_voice_active")
                        continue;
                    zone = candidate.value;
                    break;
                }
                if (zone == nullptr) {
                    result.diagnostic = "Faust active-voice zone "
                        + module.moduleId + " voice "
                        + std::to_string(voice) + " is unavailable";
                    return result;
                }
                cap.zones.push_back(zone);
            }
            controls.rows.push_back(std::move(cap));
        }
        renderer->controls_.push_back(std::move(controls));
    }
    renderer->processControlInputs_.reserve(
        static_cast<std::size_t>(processControlChannels));
    std::unordered_map<int, std::size_t> controlModuleByGraphIndex;
    for (std::size_t module = 0; module < state.modules().size(); ++module)
        controlModuleByGraphIndex[state.modules()[module].index] = module;
    for (const auto* entry : plan.modules) {
        const auto layout = processControls.find(entry->index);
        if (layout == processControls.end())
            continue;
        const auto module = controlModuleByGraphIndex.at(entry->index);
        if (renderer->controls_[module].rows.size()
            < static_cast<std::size_t>(layout->second.expandedRows)) {
            result.diagnostic = "prepared process-control rows are incomplete for "
                + entry->moduleId;
            return result;
        }
        for (int row = 0; row < layout->second.expandedRows; ++row) {
            FaustGraphRenderer::ProcessControlInput binding;
            binding.module = module;
            binding.row = static_cast<std::size_t>(row);
            binding.lastValue = renderer->controls_[module]
                .rows[static_cast<std::size_t>(row)].fallbackValue;
            binding.scratch.resize(
                static_cast<std::size_t>(maximumBlockSize), binding.lastValue);
            renderer->processControlInputs_.push_back(std::move(binding));
        }
    }
    renderer->tapIds_.reserve(state.modules().size());
    renderer->tapOffsets_.reserve(state.modules().size());
    renderer->tapWidths_.reserve(state.modules().size());
    renderer->tapControlBoundaries_.reserve(state.modules().size());
    renderer->tapControlOutputIds_.reserve(state.modules().size());
    renderer->tapControlTelemetryOffsets_.reserve(state.modules().size());
    std::size_t tapOffset = 0;
    std::size_t controlTelemetryCount = 0;
    for (const auto& module : state.modules()) {
        const bool preparedScriptRoute =
            plan.scriptRoutes.count(module.index) != 0;
        renderer->tapIds_.push_back(module.moduleId);
        renderer->tapOffsets_.push_back(tapOffset);
        const int width = isNonAudioBoundary(module)
            ? 0 : moduleOutputs.at(module.index).second;
        renderer->tapWidths_.push_back(width);
        renderer->tapControlBoundaries_.push_back(
            isVmControlSource(module) || isHostAutomationSource(module)
                || isHostMidiSource(module) || isTransportClockSource(module)
                || isMidiEventBoundary(module));
        auto controlOutputIds =
            isNonAudioBoundary(module) && ! preparedScriptRoute
                ? std::vector<std::string> {} : module.controlOutputs;
        renderer->tapControlTelemetryOffsets_.push_back(
            controlTelemetryCount);
        controlTelemetryCount += controlOutputIds.size();
        renderer->tapControlOutputIds_.push_back(
            std::move(controlOutputIds));
        if (isOutput(module)) {
            renderer->outputTap_ = renderer->tapIds_.size() - 1;
            if (width != 2) {
                result.diagnostic = "core.output must retain exactly two channels";
                return result;
            }
        }
        if (! isNonAudioBoundary(module) || preparedScriptRoute)
            tapOffset += static_cast<std::size_t>(
                moduleTotalOutputWidths.at(module.index));
    }
    renderer->controlOutputHolds_ =
        std::make_unique<ControlRangeHandoff[]>(
            controlTelemetryCount);
    renderer->outputLevel_ = outputLevel;
    renderer->inputMixes_.resize(renderer->tapIds_.size());
    renderer->faceplateTelemetry_.resize(renderer->tapIds_.size());
    renderer->outputLevels_ = std::make_unique<
        FaustGraphRenderer::LevelHold[]>(renderer->tapIds_.size());
    renderer->inputLevels_ = std::make_unique<
        FaustGraphRenderer::LevelHold[]>(renderer->tapIds_.size());
    renderer->clips_ = std::make_unique<std::atomic<float>[]>(
        renderer->tapIds_.size());

    std::unordered_map<int, std::size_t> tapByModuleIndex;
    int maximumGraphModuleIndex = -1;
    for (std::size_t tap = 0; tap < state.modules().size(); ++tap) {
        tapByModuleIndex[state.modules()[tap].index] = tap;
        maximumGraphModuleIndex = std::max(
            maximumGraphModuleIndex, state.modules()[tap].index);
        renderer->clips_[tap].store(0.0f, std::memory_order_relaxed);
    }
    renderer->hostAutomationBoundaryByTap_.assign(
        state.modules().size(), -1);
    renderer->vmInputBoundaryByGraphIndex_.assign(
        static_cast<std::size_t>(maximumGraphModuleIndex + 1), -1);
    for (const auto& module : state.modules()) {
        if (module.lineageId != "core.script_v2"
            && ! isPreparedStepSource(module))
            continue;
        FaustGraphRenderer::VmInputBoundary boundary;
        boundary.graphModuleIndex = module.index;
        const auto inputCount = isPreparedStepSource(module)
            ? module.params.size() : module.controlInputs.size();
        boundary.edges.resize(inputCount);
        boundary.previousRows.resize(inputCount);
        for (auto& row : boundary.previousRows)
            row.resize(static_cast<std::size_t>(maximumBlockSize), 0.0f);

        const auto incoming = plan.vmInputIncoming.find(module.index);
        if (incoming != plan.vmInputIncoming.end()) {
            for (const auto& edge : incoming->second) {
                const auto* source = moduleByIndex.at(edge.srcIndex);
                const int input = preparedVmInputIndex(module, edge.tgtPort);
                const int output = namedControlOutputIndex(
                    *source, edge.srcPort);
                const auto sourceTap = tapByModuleIndex.find(edge.srcIndex);
                if (input < 0 || output < 0
                    || sourceTap == tapByModuleIndex.end()
                    || output >= static_cast<int>(
                        renderer->tapControlOutputIds_[sourceTap->second].size())) {
                    result.diagnostic = "VM/control input source "
                        + source->moduleId + "." + edge.srcPort
                        + " is unavailable from the unified renderer";
                    return result;
                }
                boundary.edges[static_cast<std::size_t>(input)].push_back({
                    sourceTap->second,
                    static_cast<std::size_t>(output),
                    edge.audible ? edge.gain : 0.0f
                });
            }
        }
        renderer->vmInputBoundaryByGraphIndex_[
            static_cast<std::size_t>(module.index)] = static_cast<int>(
                renderer->vmInputBoundaries_.size());
        renderer->vmInputBoundaries_.push_back(std::move(boundary));
    }

    renderer->hostAutomationBoundaries_.reserve(
        hostAutomationBoundaries.size());
    for (const auto* module : hostAutomationBoundaries) {
        if (module->params.size()
                != static_cast<std::size_t>(
                    HostAutomationInputProcessor::kInputChannels)
            || module->controlOutputs.size()
                != static_cast<std::size_t>(
                    HostAutomationInputProcessor::kOutputCount)) {
            result.diagnostic =
                "Host Automation requires eight selectors and outputs";
            return result;
        }
        FaustGraphRenderer::HostAutomationBoundary boundary;
        boundary.graphModuleIndex = module->index;
        boundary.controlModule = controlModuleByGraphIndex.at(module->index);
        boundary.controls.reserve(module->params.size());
        for (const auto& parameter : module->params) {
            FaustGraphRenderer::VisualControlInput control;
            control.parameter = parameter;
            control.heldBase = parameter.defaultValue;
            boundary.controls.push_back(std::move(control));
        }
        boundary.controlChannels.resize(boundary.controls.size());
        boundary.controlPointers.resize(boundary.controls.size());
        boundary.outputChannels.resize(module->controlOutputs.size());
        boundary.outputPointers.resize(module->controlOutputs.size());
        for (std::size_t channel = 0;
             channel < boundary.controlChannels.size(); ++channel) {
            boundary.controlChannels[channel].resize(
                static_cast<std::size_t>(maximumBlockSize), 0.0f);
            boundary.controlPointers[channel] =
                boundary.controlChannels[channel].data();
        }
        for (std::size_t channel = 0;
             channel < boundary.outputChannels.size(); ++channel) {
            boundary.outputChannels[channel].resize(
                static_cast<std::size_t>(maximumBlockSize), 0.0f);
            boundary.outputPointers[channel] =
                boundary.outputChannels[channel].data();
        }
        const auto tap = tapByModuleIndex.find(module->index);
        if (tap == tapByModuleIndex.end()) {
            result.diagnostic = "Host Automation source tap is unavailable";
            return result;
        }
        renderer->hostAutomationBoundaryByTap_[tap->second] =
            static_cast<int>(renderer->hostAutomationBoundaries_.size());
        renderer->hostAutomationBoundaries_.push_back(
            std::move(boundary));
    }

    renderer->hostMidiBoundaries_.reserve(hostMidiBoundaries.size());
    for (const auto* module : hostMidiBoundaries) {
        if (module->params.size() != 2u || module->controlOutputs.size() != 4u) {
            result.diagnostic = "MIDI In requires two selectors and four outputs";
            return result;
        }
        FaustGraphRenderer::HostMidiBoundary boundary;
        boundary.graphModuleIndex = module->index;
        boundary.controlModule = controlModuleByGraphIndex.at(module->index);
        for (const auto& parameter : module->params) {
            FaustGraphRenderer::VisualControlInput control;
            control.parameter = parameter;
            control.heldBase = parameter.defaultValue;
            boundary.controls.push_back(std::move(control));
        }
        boundary.controlChannels.resize(boundary.controls.size());
        for (auto& channel : boundary.controlChannels)
            channel.resize(static_cast<std::size_t>(maximumBlockSize), 0.0f);
        boundary.buffer.setSize(
            HostMidiInputRuntime::kControlCount
                + HostMidiInputRuntime::kOutputCount,
            maximumBlockSize, false, true, false);
        renderer->hostMidiBoundaries_.push_back(std::move(boundary));
    }

    renderer->vmControlOutputBoundaries_.reserve(
        transportClockBoundaries.size());
    for (const auto* module : transportClockBoundaries) {
        FaustGraphRenderer::VmControlOutputBoundary boundary;
        boundary.graphModuleIndex = module->index;
        boundary.rows.resize(module->controlOutputs.size(), nullptr);
        renderer->vmControlOutputBoundaries_.push_back(
            std::move(boundary));
    }

    for (const auto& module : state.modules()) {
        if (module.lineageId != "core.visual_shader")
            continue;
        FaustGraphRenderer::VisualBoundary boundary;
        boundary.graphModuleIndex = module.index;
        boundary.controlModule = controlModuleByGraphIndex.at(module.index);
        std::vector<VisualInputRuntime::Range> visualRanges;
        visualRanges.reserve(module.params.size());
        boundary.controls.reserve(module.params.size());
        for (const auto& parameter : module.params) {
            FaustGraphRenderer::VisualControlInput control;
            control.parameter = parameter;
            control.heldBase = parameter.defaultValue;
            boundary.controls.push_back(std::move(control));
            visualRanges.push_back({ parameter.min, parameter.max });
        }
        boundary.audioInputs.resize(module.audioInputs.size());
        boundary.runtime = std::make_unique<VisualInputRuntime>(
            std::move(visualRanges),
            static_cast<int>(module.audioInputs.size()));

        const auto incoming = plan.renderIncoming.find(module.index);
        if (incoming != plan.renderIncoming.end()) {
            for (const auto& edge : incoming->second) {
                const auto* source = moduleByIndex.at(edge.srcIndex);
                const auto sourceTap = tapByModuleIndex.find(edge.srcIndex);
                if (sourceTap == tapByModuleIndex.end()) {
                    result.diagnostic = "visual boundary source tap is unavailable";
                    return result;
                }
                if (isNamedControlEdge(*source, module, edge)) {
                    const int target = exposedControlInputIndex(
                        module, edge.tgtPort);
                    const int output = namedControlOutputIndex(
                        *source, edge.srcPort);
                    if (target < 0 || output < 0
                        || target >= static_cast<int>(boundary.controls.size())) {
                        result.diagnostic =
                            "visual control tap contract is unavailable";
                        return result;
                    }
                    const auto sourceType = output < static_cast<int>(
                            source->controlOutputTypes.size())
                        ? source->controlOutputTypes[
                              static_cast<std::size_t>(output)]
                        : vm::SignalType::Value;
                    const auto targetControl = faustControlInputOf(
                        module.params[static_cast<std::size_t>(target)].name,
                        module.params[static_cast<std::size_t>(target)].type
                            == ParamType::Boolean,
                        module.params[static_cast<std::size_t>(target)].defaultValue,
                        module.params[static_cast<std::size_t>(target)].min,
                        module.params[static_cast<std::size_t>(target)].max,
                        module.params[static_cast<std::size_t>(target)].scale
                            == Scale::Logarithmic ? "log" : "");
                    const bool setValue = sourceType == targetControl.type
                        && (sourceType == vm::SignalType::Pitch
                            || sourceType == vm::SignalType::Gate
                            || sourceType == vm::SignalType::Velocity);
                    boundary.controls[static_cast<std::size_t>(target)]
                        .edges.push_back({
                            sourceTap->second,
                            static_cast<std::size_t>(output),
                            edge.audible ? edge.gain : 0.0f,
                            setValue,
                            setValue && sourceType == vm::SignalType::Pitch
                                && targetControl.unit == vm::InputUnit::Hz
                        });
                    continue;
                }

                const auto target = std::find(
                    module.audioInputs.begin(), module.audioInputs.end(),
                    edge.tgtPort);
                const int audio = target == module.audioInputs.end()
                    ? -1 : static_cast<int>(std::distance(
                          module.audioInputs.begin(), target));
                if (audio < 0 || edge.signalDescriptor.width() != 2
                    || renderer->tapWidths_[sourceTap->second] != 2) {
                    result.diagnostic =
                        "visual audio taps require a named stereo source/target contract";
                    return result;
                }
                const float gain = edge.audible
                    ? std::clamp(edge.gain, 0.0f, 2.0f) : 0.0f;
                std::array<float, 2> gains { gain, gain };
                const auto* descriptorLayout = edge.signalDescriptor.layout()
                    ? &*edge.signalDescriptor.layout() : nullptr;
                if (descriptorLayout != nullptr
                    && *descriptorLayout == "stereo") {
                    const float angle = (std::clamp(edge.pan, -1.0f, 1.0f)
                        + 1.0f) * 0.25f * 3.14159265358979323846f;
                    gains = { std::cos(angle) * gain,
                              std::sin(angle) * gain };
                }
                boundary.audioInputs[static_cast<std::size_t>(audio)]
                    .edges.push_back({ sourceTap->second, gains });
            }
        }
        const auto channelCount = boundary.controls.size()
            + boundary.audioInputs.size() * 2;
        boundary.channels.resize(channelCount);
        boundary.channelPointers.resize(channelCount);
        for (std::size_t channel = 0; channel < channelCount; ++channel) {
            boundary.channels[channel].resize(
                static_cast<std::size_t>(maximumBlockSize), 0.0f);
            boundary.channelPointers[channel] =
                boundary.channels[channel].data();
        }
        renderer->visualBoundaries_.push_back(std::move(boundary));
    }

    for (const auto& module : state.modules()) {
        if (! isMidiEventBoundary(module))
            continue;
        if (module.params.size()
            != static_cast<std::size_t>(MidiOutputRuntime::kInputChannels)) {
            result.diagnostic =
                "MIDI output boundary requires its six canonical controls";
            return result;
        }
        FaustGraphRenderer::MidiBoundary boundary;
        boundary.graphModuleIndex = module.index;
        boundary.controlModule = controlModuleByGraphIndex.at(module.index);
        boundary.controls.reserve(module.params.size());
        for (const auto& parameter : module.params) {
            FaustGraphRenderer::VisualControlInput control;
            control.parameter = parameter;
            control.heldBase = parameter.defaultValue;
            boundary.controls.push_back(std::move(control));
        }

        const auto incoming = plan.eventIncoming.find(module.index);
        if (incoming != plan.eventIncoming.end()) {
            for (const auto& edge : incoming->second) {
                const auto* source = moduleByIndex.at(edge.srcIndex);
                const auto sourceTap = tapByModuleIndex.find(edge.srcIndex);
                const int target = exposedControlInputIndex(
                    module, edge.tgtPort);
                const int output = namedControlOutputIndex(
                    *source, edge.srcPort);
                if (sourceTap == tapByModuleIndex.end()
                    || target < 0 || output < 0
                    || target >= static_cast<int>(boundary.controls.size())) {
                    result.diagnostic =
                        "MIDI output control tap contract is unavailable";
                    return result;
                }
                const auto sourceType = output < static_cast<int>(
                        source->controlOutputTypes.size())
                    ? source->controlOutputTypes[static_cast<std::size_t>(output)]
                    : vm::SignalType::Value;
                const auto targetType = target == MidiOutputRuntime::kGate
                    ? vm::SignalType::Gate
                    : target == MidiOutputRuntime::kPitch
                        ? vm::SignalType::Pitch
                        : target == MidiOutputRuntime::kVelocity
                            ? vm::SignalType::Velocity
                            : vm::SignalType::Value;
                const bool setValue = sourceType == targetType
                    && (sourceType == vm::SignalType::Pitch
                        || sourceType == vm::SignalType::Gate
                        || sourceType == vm::SignalType::Velocity
                        // CC_VALUE is normalized. Its -1 minimum is the
                        // disconnected sentinel, not a modulation range, so
                        // a normalized control source owns the value directly.
                        || target == MidiOutputRuntime::kCcValue);
                boundary.controls[static_cast<std::size_t>(target)]
                    .edges.push_back({
                        sourceTap->second,
                        static_cast<std::size_t>(output),
                        edge.audible ? edge.gain : 0.0f,
                        setValue,
                        // MIDI Out consumes CURLOP's normalized pitch signal;
                        // Hz conversion belongs only at Faust DSP controls.
                        false
                    });
            }
        }
        boundary.channels.resize(boundary.controls.size());
        boundary.channelPointers.resize(boundary.controls.size());
        for (std::size_t channel = 0;
             channel < boundary.channels.size(); ++channel) {
            boundary.channels[channel].resize(
                static_cast<std::size_t>(maximumBlockSize), 0.0f);
            boundary.channelPointers[channel] =
                boundary.channels[channel].data();
        }
        renderer->midiBoundaries_.push_back(std::move(boundary));
    }

    for (std::size_t tap = 0; tap < state.modules().size(); ++tap) {
        const auto& module = state.modules()[tap];
        auto& input = renderer->inputMixes_[tap];
        if (isNonAudioBoundary(module)) {
            input.width = 0;
            continue;
        }
        input.width = moduleInputWidths.at(module.index);
        const auto incoming = plan.incoming.find(module.index);
        if (incoming != plan.incoming.end()) {
            input.edges.reserve(incoming->second.size());
            for (const auto& edge : incoming->second) {
                const auto* sourceModule = moduleByIndex.at(edge.srcIndex);
                if (isNamedControlEdge(*sourceModule, module, edge))
                    continue;
                const auto source = tapByModuleIndex.find(edge.srcIndex);
                if (source == tapByModuleIndex.end()) {
                    result.diagnostic = "module input telemetry source is unavailable";
                    return result;
                }
                FaustGraphRenderer::InputEdge telemetryEdge;
                telemetryEdge.sourceTap = source->second;
                telemetryEdge.gains.resize(
                    static_cast<std::size_t>(input.width),
                    std::clamp(edge.gain, 0.0f, 2.0f));
                telemetryEdge.oneSampleFeedback =
                    edge.feedbackBoundary == FeedbackBoundary::OneSample;
                if (telemetryEdge.oneSampleFeedback)
                    telemetryEdge.previousSamples.resize(
                        static_cast<std::size_t>(input.width), 0.0f);
                const auto* layout = edge.signalDescriptor.layout()
                    ? &*edge.signalDescriptor.layout() : nullptr;
                if (input.width == 2 && layout != nullptr
                    && *layout == "stereo"
                    && edge.feedbackBoundary != FeedbackBoundary::OneSample) {
                    const float angle = (std::clamp(edge.pan, -1.0f, 1.0f)
                        + 1.0f) * 0.25f * 3.14159265358979323846f;
                    const float gain = std::clamp(edge.gain, 0.0f, 2.0f);
                    telemetryEdge.gains[0] = std::cos(angle) * gain;
                    telemetryEdge.gains[1] = std::sin(angle) * gain;
                }
                input.edges.push_back(std::move(telemetryEdge));
            }
        }

        auto& faceplate = renderer->faceplateTelemetry_[tap];
        faceplate.ids.reserve(module.faceplateMeters.size());
        faceplate.zones.reserve(module.faceplateMeters.size());
        faceplate.useTapSignal.reserve(module.faceplateMeters.size());
        for (const auto& expected : module.faceplateMeters) {
            const auto expectedId = expected.id;
            auto expectedPath = expectedId;
            if (! expectedPath.empty() && expectedPath.front() == '/') {
                const auto rootEnd = expectedPath.find('/', 1);
                if (rootEnd != std::string::npos)
                    expectedPath = expectedPath.substr(rootEnd);
            }
            // Factory probes retain authored group names (for example
            // /Spring Tank/wet_out), while the aggregate UI may namespace the
            // same meter under a different root. The terminal meter id is the
            // source contract and is unique within a module faceplate.
            const auto terminal = expectedPath.find_last_of('/');
            if (terminal != std::string::npos)
                expectedPath = expectedPath.substr(terminal);
            FAUSTFLOAT* zone = nullptr;
            for (const auto& candidate : controlUi.meters()) {
                const auto voicePath = "/voice-0/";
                const bool idMatches = candidate.path.size()
                        >= expectedPath.size()
                    && candidate.path.compare(
                        candidate.path.size() - expectedPath.size(),
                        expectedPath.size(), expectedPath) == 0;
                // The compiled aggregate UI namespaces a meter by its Faust
                // factory probe path, not necessarily by the mutable graph
                // moduleId. The source-owned meter id is the stable contract.
                if ((module.physicalVoices > 1
                        && candidate.path.find(voicePath) == std::string::npos)
                    || ! idMatches)
                    continue;
                zone = candidate.value;
                break;
            }
            if (zone == nullptr) {
                // Faceplate telemetry is observational. Some aggregate Faust
                // graphs do not retain a source meter zone, but that must
                // never reject an otherwise valid audio route.
                faceplate.ids.push_back(expectedId);
                faceplate.zones.push_back(nullptr);
                faceplate.useTapSignal.push_back(true);
                continue;
            }
            faceplate.ids.push_back(expectedId);
            faceplate.zones.push_back(zone);
            faceplate.useTapSignal.push_back(false);
        }
        if (! faceplate.ids.empty()) {
            faceplate.values = std::make_unique<std::atomic<float>[]>(
                faceplate.ids.size());
            for (std::size_t meter = 0; meter < faceplate.ids.size(); ++meter)
                faceplate.values[meter].store(0.0f,
                                               std::memory_order_relaxed);
        }
    }
    renderer->outputs_.resize(tapOffset);
    renderer->outputPointers_.resize(renderer->outputs_.size());
    for (std::size_t channel = 0; channel < renderer->outputs_.size(); ++channel) {
        renderer->outputs_[channel].resize(static_cast<std::size_t>(maximumBlockSize));
        renderer->outputPointers_[channel] = renderer->outputs_[channel].data();
    }
    if (renderer->instance_->getNumInputs() != externalInputChannels
        || renderer->instance_->getNumOutputs()
            != static_cast<int>(renderer->outputs_.size())) {
        result.diagnostic = "compiled Faust graph has audio contract "
            + std::to_string(renderer->instance_->getNumInputs()) + "/"
            + std::to_string(renderer->instance_->getNumOutputs())
            + "; expected " + std::to_string(externalInputChannels) + "/"
            + std::to_string(renderer->outputs_.size());
        return result;
    }
    result.renderer = std::move(renderer);
    return result;
}

} // namespace curlop::transport
