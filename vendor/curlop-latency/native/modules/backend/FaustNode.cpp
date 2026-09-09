// FaustNode — libfaust LLVM-JIT pimpl implementation (s438).
//
// libfaust headers (<faust/dsp/llvm-dsp.h>, <faust/gui/UI.h>) declare
// `class UI` and `class dsp` in the global namespace, which collides with
// the local stubs in audio/faust/Faust{Djembe,Bell}Adapter.h. Keeping
// those headers confined to this TU lets both coexist.
//
// Hot-swap model (mirror of audio/SequencerVM.h:19 AtomicProgramSwap):
//   - active_ is std::atomic<ActiveProgram*>; processBlock loads acquire.
//   - setSource() signals the worker; worker compiles, then exchange()s
//     the new pointer in and parks the old in a single-slot graveyard.
//   - The graveyard is freed on the *next* successful swap, guaranteeing
//     the audio thread has moved past its last load() of it.
//
#include "modules/backend/FaustNode.h"
#include "modules/backend/FaustRuntime.h"          // compileMutex() — the process-wide libfaust lock (B-205, T-404)
#include "modules/backend/FaustUiCapture.h"        // FaustUiCapture — UI-tree model + schema synth (T-357)
#include "modules/contract/ModuleTypes.h"  // ParamSchemaEntry — synthesised faceplate schema (T-318)
#include "modules/backend/FaustSourceMetadata.h"
#include "graph/transport/RealtimeWorkerTeam.h"
#include "runtime/DeclarationRowMap.h"  // markPerVoiceParams — SF-052 per-voice value inputs
#include "shell/CurlopDebug.h"        // CDBG taps for the schema-change loop (T-318)

#if CURLOP_ENABLE_FAUST_JIT
#include <faust/dsp/llvm-dsp.h>
#else
#include <faust/dsp/interpreter-dsp.h>
#endif
#include <faust/gui/UI.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>
#include <unordered_map>
#include <vector>

namespace curlop {

namespace {

std::string normalisedFaustZoneLabel(std::string label)
{
    const auto slash = label.find_last_of('/');
    if (slash != std::string::npos)
        label = label.substr(slash + 1);
    if (label.size() > 2 && label[1] == ':')
        label = label.substr(2);
    if (! label.empty() && label.front() == '[') {
        const auto end = label.find(']');
        if (end != std::string::npos)
            label = label.substr(end + 1);
    }
    const auto meta = label.find('[');
    if (meta != std::string::npos)
        label = label.substr(0, meta);
    while (! label.empty() && std::isspace((unsigned char) label.front())) label.erase(label.begin());
    while (! label.empty() && std::isspace((unsigned char) label.back())) label.pop_back();
    return label;
}

void storeFaustZoneAlias(std::unordered_map<std::string, FAUSTFLOAT*>& zones,
                         const char* label,
                         FAUSTFLOAT* zone)
{
    const std::string raw = label ? label : "";
    zones[raw] = zone;
    const auto normalised = normalisedFaustZoneLabel(raw);
    if (! normalised.empty() && normalised != raw)
        zones.emplace(normalised, zone);
}

// B-205 (s440): libfaust's createDSPFactoryFromString drives LLVM internals
// (DataLayout, EarlyCSE, instruction simplification) that are NOT safe for
// concurrent invocation. Two compiles racing produced an EXC_BAD_ACCESS in
// llvm::DataLayout::getTypeAllocSize on M1 at session restore — the BYTE
// clip-switch backfill's compileSchemaOnly on the message thread collided
// with the FaustNode worker's compileSync still finishing a prior
// setSource. The fix is a process-wide mutex around every libfaust entry
// point. Cost: compiles serialize — a single faust_jit module never sees
// contention; multiple instances or back-to-back clip switches with stale
// pending compiles can see one extra ~50ms wait each. Acceptable.
// T-404: the canonical mutex now lives on FaustRuntime (the shared Faust
// service); this alias keeps the call sites here readable. One mutex,
// process-wide — runtime compiles, node compiles, factory teardowns.
std::mutex& faustGlobalCompileMutex()
{
    return FaustRuntime::compileMutex();
}

bool ensureJitFactoryClassInitLocked(const FaustRuntime::FactoryPtr& factory,
                                     int sampleRate)
{
#if !CURLOP_ENABLE_FAUST_JIT
    (void) factory;
    (void) sampleRate;
    return false;
#else
    if (factory == nullptr || factory->backend() != FaustRuntime::Backend::Jit)
        return false;

    struct InitEntry {
        std::weak_ptr<FaustRuntime::Factory> factory;
        int sampleRate = 0;
    };
    static std::mutex initMu;
    static std::vector<InitEntry> initialized;

    {
        std::lock_guard<std::mutex> lock(initMu);
        initialized.erase(
            std::remove_if(initialized.begin(), initialized.end(),
                           [] (const InitEntry& e) { return e.factory.expired(); }),
            initialized.end());

        for (const auto& entry : initialized) {
            auto existing = entry.factory.lock();
            if (entry.sampleRate == sampleRate
                && existing
                && !factory.owner_before(existing)
                && !existing.owner_before(factory)) {
                return true;
            }
        }

        initialized.push_back({ factory, sampleRate });
    }

    // Caller holds FaustRuntime::compileMutex(). LLVM JIT factories expose
    // classInit(sampleRate); interpreter factories do not, so they use the
    // normal dsp::init fallback at the call site.
    static_cast<llvm_dsp_factory*>(factory->raw())->classInit(sampleRate);
    return true;
#endif
}

struct FaustGroupSourceMeta
{
    std::string accent;
    float fontSize = 0.0f;
    bool hasFontSize = false;
    std::string labelPosition;
};

struct FaustControlSourceMeta
{
    std::string sourcePath;
    std::string leaf;
    std::string orientation;
    std::string skin;
    std::string accent;
    float fontSize = 0.0f;
    bool hasFontSize = false;
    std::string labelPosition;
    float size = 1.0f;
    bool hasSize = false;
    float column = -1.0f;
    bool hasColumn = false;
    float row = -1.0f;
    bool hasRow = false;
    float width = 1.0f;
    bool hasWidth = false;
    float height = 1.0f;
    bool hasHeight = false;
};

std::string trimSourceMetaCopy (std::string s)
{
    while (! s.empty() && std::isspace ((unsigned char) s.front())) s.erase (s.begin());
    while (! s.empty() && std::isspace ((unsigned char) s.back())) s.pop_back();
    return s;
}

std::string baseFaustGroupLabel (std::string label)
{
    const auto bracket = label.find ('[');
    if (bracket != std::string::npos)
        label = label.substr (0, bracket);
    if (label.size() > 2 && label[1] == ':')
    {
        const char prefix = (char) std::tolower ((unsigned char) label[0]);
        if (prefix == 'h' || prefix == 'v' || prefix == 't')
            label = label.substr (2);
    }
    return trimSourceMetaCopy (std::move (label));
}

std::string normalisedFaustGroupName (std::string s)
{
    s = baseFaustGroupLabel (std::move (s));
    std::string out;
    out.reserve (s.size());
    for (char ch : s)
    {
        if (ch == ' ' || ch == '_' || ch == '-') continue;
        out.push_back ((char) std::tolower ((unsigned char) ch));
    }
    return out;
}

float parseSourceMetaFloat (const std::string& value, float fallback)
{
    char* end = nullptr;
    const auto parsed = std::strtof (value.c_str(), &end);
    while (end != nullptr && *end != '\0' && std::isspace ((unsigned char) *end))
        ++end;
    return end == value.c_str() || end == nullptr || *end != '\0' || ! std::isfinite (parsed)
        ? fallback
        : parsed;
}

std::string faustPathSegmentBase (std::string segment)
{
    segment = trimSourceMetaCopy (std::move (segment));
    if (segment.size() > 2 && segment[1] == ':')
    {
        const char prefix = (char) std::tolower ((unsigned char) segment[0]);
        if (prefix == 'h' || prefix == 'v' || prefix == 't')
            segment = segment.substr (2);
    }
    const auto bracket = segment.find ('[');
    if (bracket != std::string::npos)
        segment = segment.substr (0, bracket);
    return trimSourceMetaCopy (std::move (segment));
}

std::vector<std::string> faustLabelPathParts (const std::string& label)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= label.size())
    {
        const auto slash = label.find ('/', start);
        auto part = faustPathSegmentBase (label.substr (start,
                                                        slash == std::string::npos
                                                            ? std::string::npos
                                                            : slash - start));
        if (! part.empty())
            out.push_back (std::move (part));
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return out;
}

std::string faustControlSourcePath (const std::string& label)
{
    const auto parts = faustLabelPathParts (label);
    if (parts.empty())
        return {};

    std::string out;
    for (const auto& part : parts)
        out += "/" + part;
    return out;
}

std::string faustControlLeafSegment (const std::string& label)
{
    const auto slash = label.find_last_of ('/');
    return slash == std::string::npos ? label : label.substr (slash + 1);
}

std::string faustControlLeafBase (const std::string& label)
{
    return faustPathSegmentBase (faustControlLeafSegment (label));
}

std::string sourceMetaStyleToken (const std::string& value)
{
    const auto brace = value.find ('{');
    return brace == std::string::npos ? value : value.substr (0, brace);
}

bool sourcePathSuffixMatches (const std::string& schemaPath,
                              const std::string& sourcePath)
{
    if (schemaPath.empty() || sourcePath.empty())
        return false;
    if (schemaPath == sourcePath)
        return true;
    if (schemaPath.size() <= sourcePath.size())
        return false;
    return schemaPath.compare (schemaPath.size() - sourcePath.size(),
                               sourcePath.size(),
                               sourcePath) == 0;
}

bool quotedStringStartsFaustControlLabel (const std::string& text, size_t quotePos)
{
    size_t p = quotePos;
    while (p > 0 && std::isspace ((unsigned char) text[p - 1])) --p;
    if (p == 0 || text[p - 1] != '(') return false;

    --p;
    while (p > 0 && std::isspace ((unsigned char) text[p - 1])) --p;
    const auto nameEnd = p;
    while (p > 0)
    {
        const char ch = text[p - 1];
        if (! (std::isalnum ((unsigned char) ch) || ch == '_')) break;
        --p;
    }
    if (p == nameEnd) return false;
    const auto name = text.substr (p, nameEnd - p);
    return name == "button" || name == "checkbox" || name == "hslider"
        || name == "vslider" || name == "nentry" || name == "hbargraph"
        || name == "vbargraph";
}

bool parseFaustControlSourceMeta (std::string label, FaustControlSourceMeta& meta)
{
    meta.sourcePath = faustControlSourcePath (label);
    meta.leaf = faustControlLeafBase (label);
    if (meta.sourcePath.empty() || meta.leaf.empty())
        return false;

    auto leaf = faustControlLeafSegment (label);
    bool hasMeta = false;
    size_t pos = 0;
    while ((pos = leaf.find ('[', pos)) != std::string::npos)
    {
        const auto close = leaf.find (']', pos + 1);
        if (close == std::string::npos)
            break;
        const auto body = leaf.substr (pos + 1, close - pos - 1);
        const auto colon = body.find (':');
        if (colon == std::string::npos)
        {
            pos = close + 1;
            continue;
        }

        const auto key = trimSourceMetaCopy (body.substr (0, colon));
        const auto value = trimSourceMetaCopy (body.substr (colon + 1));
        if (key == "orientation")
        {
            meta.orientation = normaliseFaceplateOrientationMetadata (value);
            hasMeta = hasMeta || ! meta.orientation.empty();
        }
        else if (key == "skin")
        {
            meta.skin = normaliseFaceplateSkinMetadata (value);
            hasMeta = hasMeta || ! meta.skin.empty();
        }
        else if (key == "accent")
        {
            meta.accent = normaliseFaceplateAccentMetadata (value);
            hasMeta = hasMeta || ! meta.accent.empty();
        }
        else if (key == "labelsize" || key == "fontSize")
        {
            meta.fontSize = parseSourceMetaFloat (value, 0.0f);
            meta.hasFontSize = meta.fontSize > 0.0f;
            hasMeta = hasMeta || meta.hasFontSize;
        }
        else if (key == "labelpos" || key == "labelPosition")
        {
            meta.labelPosition = normaliseFaceplateLabelPositionMetadata (value);
            hasMeta = hasMeta || ! meta.labelPosition.empty();
        }
        else if (key == "size")
        {
            meta.size = parseSourceMetaFloat (value, 1.0f);
            meta.hasSize = true;
            hasMeta = true;
        }
        else if (key == "col")
        {
            meta.column = parseSourceMetaFloat (value, -1.0f);
            meta.hasColumn = true;
            hasMeta = true;
        }
        else if (key == "row")
        {
            meta.row = parseSourceMetaFloat (value, -1.0f);
            meta.hasRow = true;
            hasMeta = true;
        }
        else if (key == "w")
        {
            meta.width = parseSourceMetaFloat (value, 1.0f);
            meta.hasWidth = true;
            hasMeta = true;
        }
        else if (key == "h")
        {
            meta.height = parseSourceMetaFloat (value, 1.0f);
            meta.hasHeight = true;
            hasMeta = true;
        }

        pos = close + 1;
    }

    return hasMeta;
}

std::vector<FaustControlSourceMeta> faustControlSourceMetadata (const std::string& source)
{
    std::vector<FaustControlSourceMeta> out;
    const auto text = faustSourceWithoutComments (source);
    size_t pos = 0;
    while ((pos = text.find ('"', pos)) != std::string::npos)
    {
        auto q1 = pos + 1;
        bool escaped = false;
        for (; q1 < text.size(); ++q1)
        {
            const char ch = text[q1];
            if (escaped) { escaped = false; continue; }
            if (ch == '\\') { escaped = true; continue; }
            if (ch == '"') break;
        }
        if (q1 >= text.size()) break;

        if (quotedStringStartsFaustControlLabel (text, pos))
        {
            FaustControlSourceMeta meta;
            if (parseFaustControlSourceMeta (text.substr (pos + 1, q1 - pos - 1), meta))
                out.push_back (std::move (meta));
        }
        pos = q1 + 1;
    }
    return out;
}

void applyFaustControlSourceMetadata (std::vector<ParamSchemaEntry>& schema,
                                      const std::string& source)
{
    const auto metas = faustControlSourceMetadata (source);
    if (metas.empty())
        return;

    for (auto& p : schema)
    {
        const auto schemaPath = p.sourceId.empty() ? std::string ("/") + p.name : p.sourceId;
        for (const auto& meta : metas)
        {
            if (! sourcePathSuffixMatches (schemaPath, meta.sourcePath)
                && ! (p.name == meta.leaf && meta.sourcePath.find ('/', 1) == std::string::npos))
                continue;

            if (! meta.orientation.empty()) p.orientation = meta.orientation;
            if (! meta.skin.empty())        p.skin = meta.skin;
            if (! meta.accent.empty())      p.accent = meta.accent;
            if (meta.hasFontSize)           p.fontSize = meta.fontSize;
            if (! meta.labelPosition.empty()) p.labelPosition = meta.labelPosition;
            if (meta.hasSize)               p.size = meta.size;
            if (meta.hasColumn)             p.column = meta.column;
            if (meta.hasRow)                p.row = meta.row;
            if (meta.hasWidth)              p.width = meta.width;
            if (meta.hasHeight)             p.height = meta.height;
            break;
        }
    }
}

void parseFaustGroupSourceMeta (std::string segment,
                                std::unordered_map<std::string, FaustGroupSourceMeta>& out)
{
    const auto groupKey = normalisedFaustGroupName (segment);
    if (groupKey.empty())
        return;

    FaustGroupSourceMeta meta;
    bool hasMeta = false;
    size_t pos = 0;
    while ((pos = segment.find ('[', pos)) != std::string::npos)
    {
        const auto close = segment.find (']', pos + 1);
        if (close == std::string::npos)
            break;
        const auto body = segment.substr (pos + 1, close - pos - 1);
        const auto colon = body.find (':');
        if (colon == std::string::npos)
        {
            pos = close + 1;
            continue;
        }
        const auto key = trimSourceMetaCopy (body.substr (0, colon));
        const auto value = trimSourceMetaCopy (body.substr (colon + 1));
        if (key == "accent")
        {
            meta.accent = normaliseFaceplateAccentMetadata (value);
            hasMeta = hasMeta || ! meta.accent.empty();
        }
        else if (key == "labelsize" || key == "fontSize")
        {
            meta.fontSize = parseSourceMetaFloat (value, 0.0f);
            meta.hasFontSize = meta.fontSize > 0.0f;
            hasMeta = hasMeta || meta.hasFontSize;
        }
        else if (key == "labelpos" || key == "labelPosition")
        {
            meta.labelPosition = normaliseFaceplateLabelPositionMetadata (value);
            hasMeta = hasMeta || ! meta.labelPosition.empty();
        }
        pos = close + 1;
    }

    if (! hasMeta)
        return;

    auto& dst = out[groupKey];
    if (! meta.accent.empty()) dst.accent = meta.accent;
    if (meta.hasFontSize)
    {
        dst.fontSize = meta.fontSize;
        dst.hasFontSize = true;
    }
    if (! meta.labelPosition.empty()) dst.labelPosition = meta.labelPosition;
}

bool quotedStringStartsFaustGroupLabel (const std::string& text, size_t quotePos)
{
    size_t p = quotePos;
    while (p > 0 && std::isspace ((unsigned char) text[p - 1])) --p;
    if (p == 0 || text[p - 1] != '(') return false;

    --p;
    while (p > 0 && std::isspace ((unsigned char) text[p - 1])) --p;
    const auto nameEnd = p;
    while (p > 0)
    {
        const char ch = text[p - 1];
        if (! (std::isalnum ((unsigned char) ch) || ch == '_')) break;
        --p;
    }
    if (p == nameEnd) return false;
    const auto name = text.substr (p, nameEnd - p);
    return name == "hgroup" || name == "vgroup" || name == "tgroup";
}

std::unordered_map<std::string, FaustGroupSourceMeta>
faustGroupSourceMetadata (const std::string& source)
{
    std::unordered_map<std::string, FaustGroupSourceMeta> out;
    const auto text = faustSourceWithoutComments (source);
    size_t pos = 0;
    while ((pos = text.find ('"', pos)) != std::string::npos)
    {
        auto q1 = pos + 1;
        bool escaped = false;
        for (; q1 < text.size(); ++q1)
        {
            const char ch = text[q1];
            if (escaped) { escaped = false; continue; }
            if (ch == '\\') { escaped = true; continue; }
            if (ch == '"') break;
        }
        if (q1 >= text.size()) break;

        const auto label = text.substr (pos + 1, q1 - pos - 1);
        if (quotedStringStartsFaustGroupLabel (text, pos))
            parseFaustGroupSourceMeta (label, out);

        size_t partStart = 0;
        while (partStart <= label.size())
        {
            const auto slash = label.find ('/', partStart);
            if (slash == std::string::npos)
                break;
            parseFaustGroupSourceMeta (label.substr (partStart, slash - partStart), out);
            partStart = slash + 1;
        }
        pos = q1 + 1;
    }
    return out;
}

void applyFaustGroupSourceMetadata (FaceplateGroup& group,
                                    const std::unordered_map<std::string, FaustGroupSourceMeta>& meta)
{
    if (const auto it = meta.find (normalisedFaustGroupName (group.name)); it != meta.end())
    {
        if (! it->second.accent.empty()) group.accent = it->second.accent;
        if (it->second.hasFontSize) group.fontSize = it->second.fontSize;
        if (! it->second.labelPosition.empty()) group.labelPosition = it->second.labelPosition;
    }

    for (auto& child : group.children)
        applyFaustGroupSourceMetadata (child, meta);
}

void mergeFaustGroupSourceMetadata (FaceplateSchema& schema, const std::string& source)
{
    const auto meta = faustGroupSourceMetadata (source);
    if (! meta.empty())
        applyFaustGroupSourceMetadata (schema.rootGroup, meta);
}

// libfaust `::UI` adapter — walks Faust's UI tree once per compile and
// forwards it into a backend-agnostic FaustUiCapture. One walk, two captures:
//   - zone pointers (label → FAUSTFLOAT*) for runtime param writes — kept
//     here because FAUSTFLOAT* is a libfaust type and must not escape this TU;
//   - the structural UI tree (params / meters / groups / declare metadata) —
//     forwarded to capture_, which is libfaust-free and owns the schema
//     synthesis (FaustUiCapture.h — T-357 / ADR-009).
// Declaration order is preserved so the synthesised schema reads top-to-bottom
// from the .dsp source.
class ParamCaptureUI : public ::UI
{
public:
    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone,
                             FAUSTFLOAT init, FAUSTFLOAT lo, FAUSTFLOAT hi,
                             FAUSTFLOAT step) override
    {
        storeFaustZoneAlias(zones_, label, zone);
        capture_.addParam(FaustWidgetKind::HSlider, label, init, lo, hi, step);
    }
    void addVerticalSlider(const char* label, FAUSTFLOAT* zone,
                           FAUSTFLOAT init, FAUSTFLOAT lo, FAUSTFLOAT hi,
                           FAUSTFLOAT step) override
    {
        storeFaustZoneAlias(zones_, label, zone);
        capture_.addParam(FaustWidgetKind::VSlider, label, init, lo, hi, step);
    }
    void addNumEntry(const char* label, FAUSTFLOAT* zone,
                     FAUSTFLOAT init, FAUSTFLOAT lo, FAUSTFLOAT hi,
                     FAUSTFLOAT step) override
    {
        storeFaustZoneAlias(zones_, label, zone);
        capture_.addParam(FaustWidgetKind::NumEntry, label, init, lo, hi, step);
    }
    void addButton(const char* label, FAUSTFLOAT* zone) override
    {
        storeFaustZoneAlias(zones_, label, zone);
        capture_.addParam(FaustWidgetKind::Button, label, 0, 0, 1, 1);
    }
    void addCheckButton(const char* label, FAUSTFLOAT* zone) override
    {
        storeFaustZoneAlias(zones_, label, zone);
        capture_.addParam(FaustWidgetKind::CheckBox, label, 0, 0, 1, 1);
    }
    void addHorizontalBargraph(const char* label, FAUSTFLOAT* zone,
                               FAUSTFLOAT lo, FAUSTFLOAT hi) override
    {
        const bool cvout = capture_.nextWidgetIsControlOutput();
        capture_.addMeter(/*horizontal*/ true, label, lo, hi);
        if (! cvout) meterZones_.push_back(zone);
    }
    void addVerticalBargraph(const char* label, FAUSTFLOAT* zone,
                             FAUSTFLOAT lo, FAUSTFLOAT hi) override
    {
        const bool cvout = capture_.nextWidgetIsControlOutput();
        capture_.addMeter(/*horizontal*/ false, label, lo, hi);
        if (! cvout) meterZones_.push_back(zone);
    }
    void openTabBox(const char* label) override
    { capture_.openBox(GroupKind::Tab, label); }
    void openHorizontalBox(const char* label) override
    { capture_.openBox(GroupKind::Horizontal, label); }
    void openVerticalBox(const char* label) override
    { capture_.openBox(GroupKind::Vertical, label); }
    void closeBox() override { capture_.closeBox(); }
    void declare(FAUSTFLOAT*, const char* key, const char* value) override
    { capture_.declareMeta(key, value); }
    void addSoundfile(const char*, const char*, Soundfile**) override {}

    FAUSTFLOAT* find(const std::string& label) const
    {
        auto it = zones_.find(label);
        return it == zones_.end() ? nullptr : it->second;
    }
    void clear() { zones_.clear(); meterZones_.clear(); capture_.clear(); }
    void reserveZones(size_t expectedControls)
    {
        zones_.reserve(expectedControls);
        meterZones_.reserve(expectedControls);
    }

    // Legacy per-instance ParamSchemaEntry list — the T-318 hot-swap schema
    // diff + GraphState::ModuleEntry::params consume this. Kept verbatim
    // until T357-5 migrates those consumers onto FaceplateSchema.
    std::vector<ParamSchemaEntry> synthesiseSchema() const
    { return capture_.synthesiseSchema(); }

    // T-357: the normalised faceplate schema — params, meters, group tree.
    FaceplateSchema synthesiseFaceplateSchema() const
    { return capture_.synthesiseFaceplateSchema(); }

    std::vector<FAUSTFLOAT*> faceplateMeterZones() const
    { return meterZones_; }

    // F-075: the declared control outputs (cvout-tagged bargraphs), in order.
    std::vector<ControlOutputDecl> synthesiseControlOutputs() const
    { return capture_.synthesiseControlOutputs(); }

    int meterOutputCount() const noexcept
    { return capture_.countMeterOutputs(); }

private:
    std::unordered_map<std::string, FAUSTFLOAT*> zones_;
    std::vector<FAUSTFLOAT*>                     meterZones_;
    FaustUiCapture                               capture_;
};

// Per-voice Faust instances after voice 0 need only label -> zone pointers.
// Schema, faceplate, control-output declarations, and meter zones are captured
// from voice 0 above; repeating that structural capture for every voice adds
// prepared-clip build cost without changing runtime state.
class ZoneCaptureUI : public ::UI
{
public:
    explicit ZoneCaptureUI(size_t expectedControls = 0)
    {
        zones_.reserve(expectedControls);
    }

    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone,
                             FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT,
                             FAUSTFLOAT) override
    { storeFaustZoneAlias(zones_, label, zone); }
    void addVerticalSlider(const char* label, FAUSTFLOAT* zone,
                           FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT,
                           FAUSTFLOAT) override
    { storeFaustZoneAlias(zones_, label, zone); }
    void addNumEntry(const char* label, FAUSTFLOAT* zone,
                     FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT,
                     FAUSTFLOAT) override
    { storeFaustZoneAlias(zones_, label, zone); }
    void addButton(const char* label, FAUSTFLOAT* zone) override
    { storeFaustZoneAlias(zones_, label, zone); }
    void addCheckButton(const char* label, FAUSTFLOAT* zone) override
    { storeFaustZoneAlias(zones_, label, zone); }
    void addHorizontalBargraph(const char*, FAUSTFLOAT*,
                               FAUSTFLOAT, FAUSTFLOAT) override {}
    void addVerticalBargraph(const char*, FAUSTFLOAT*,
                             FAUSTFLOAT, FAUSTFLOAT) override {}
    void openTabBox(const char*) override {}
    void openHorizontalBox(const char*) override {}
    void openVerticalBox(const char*) override {}
    void closeBox() override {}
    void declare(FAUSTFLOAT*, const char*, const char*) override {}
    void addSoundfile(const char*, const char*, Soundfile**) override {}

    FAUSTFLOAT* find(const std::string& label) const
    {
        auto it = zones_.find(label);
        return it == zones_.end() ? nullptr : it->second;
    }

private:
    std::unordered_map<std::string, FAUSTFLOAT*> zones_;
};

struct SourceUiMetadata
{
    std::vector<ParamSchemaEntry> schema;
    std::vector<ControlOutputDecl> controlOutputs;
    int meterOutputs = 0;
    FaceplateSchema faceplate;
};

SourceUiMetadata sourceUiMetadataForFaustSource(const std::string& source,
                                                const ParamCaptureUI& ui)
{
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, SourceUiMetadata> cache;

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto it = cache.find(source);
        if (it != cache.end())
            return it->second;
    }

    SourceUiMetadata metadata;
    metadata.schema = ui.synthesiseSchema();
    metadata.controlOutputs = ui.synthesiseControlOutputs();
    metadata.meterOutputs = ui.meterOutputCount();
    metadata.faceplate = ui.synthesiseFaceplateSchema();
    mergeFaustGroupSourceMetadata(metadata.faceplate, source);

    std::lock_guard<std::mutex> lock(cacheMutex);
    const auto [it, inserted] = cache.emplace(source, std::move(metadata));
    (void) inserted;
    return it->second;
}

// T-357: count every descendant FaceplateGroup under `g` — the implicit root
// itself excluded. Reported by the Path-A schema-extract emit.
inline int countDescendantGroups(const FaceplateGroup& g)
{
    int n = (int) g.children.size();
    for (const auto& c : g.children)
        n += countDescendantGroups(c);
    return n;
}

// T-617: the voice-EXPANDED control-channel count for `paramNames` at
// `voices` — classify each param by name (faustControlInputOf), then
// expandDeclarationVoices. Used at construction to size the input bus the
// same way compileSync sizes prog->numControlInputs and the same way the
// feeding ScriptNode (build_faust_jit) sizes its outputs.
// SF-052: perVoiceParams flags value inputs as per-voice BEFORE expansion, so
// a {} voice-stack's value input expands ×voices into its own channels (like
// gate/pitch). The bus width MUST use the identical marked base as compileSync.
inline int expandedControlInputCount(const std::vector<std::string>& paramNames,
                                     int voices,
                                     const std::vector<std::string>& perVoiceParams = {})
{
    vm::ModuleDeclaration base;
    base.inputs.reserve(paramNames.size());
    for (const auto& nm : paramNames)
        base.inputs.push_back(faustControlInputOf(nm, /*buttonLike*/ false,
                                                  0.0f, 0.0f, 1.0f, ""));
    markPerVoiceParams(base, perVoiceParams);   // SF-052 — mark before expand
    return (int) vm::expandDeclarationVoices(base, voices < 1 ? 1 : voices)
                     .inputs.size();
}

// One installed program: a shared runtime factory + N per-voice dsp
// instances + resolved per-voice param-zone pointers (parallel to paramNames).
// T-405: the factory is FaustRuntime-cached and reference-counted — N nodes
// on the same source share one factory; this struct owns only the instance.
struct ActiveProgram
{
    FaustRuntime::FactoryPtr factory;           // shared, runtime-cached
    // T-617: N independent instances created from the ONE shared factory
    // (the CON-016 R-clone model, mirror of FaustSynthAdapter::dsps_). Each
    // instance has its own zone memory, so zones is per-voice.
    std::vector<::dsp*>      instances;          // one per voice
    std::vector<ParamCaptureUI> uis;             // voice 0 structural UI capture
    // zones[voice][param] — parallel to Impl::paramNames within each voice.
    std::vector<std::vector<FAUSTFLOAT*>> zones;

    // T-572: audio I/O arity of the compiled `process`. numAudioIn > 0 ⇒ this
    // is an effect (consumes upstream audio); 0 ⇒ synth. Set in compileSync
    // from instance->getNumInputs()/getNumOutputs().
    int numAudioIn  = 0;
    // F-075: `process` outputs split into REAL audio channels (first
    // numAudioOut) + control-output channels (trailing numControlOut). numDspOut
    // = numAudioOut + numControlOut = the raw getNumOutputs() — used to size the
    // compute() scratch (compute writes every DSP channel). numAudioOut is what
    // reaches the module's exact audio bus; the control channels are routed to
    // the node's control-output sockets.
    int numDspOut   = 0;   // raw getNumOutputs() — compute() scratch sizing
    int numAudioOut = 0;   // real audio channels (channels [0 .. numAudioOut-1])
    int numControlOut = 0; // control channels (channels [numAudioOut .. numDspOut-1])
    std::vector<ControlOutputDecl> controlOutputs; // one per control channel, in order
    ModuleProcessingCapabilities processingCapabilities;

    // T-318: synthesised faceplate schema in Faust declaration order. Set by
    // compileSync after `instance->buildUserInterface(&ui)`. Drives the
    // change-detect in the hot-swap callback (Step 3) and the per-instance
    // params written to GraphState::ModuleEntry.
    std::vector<ParamSchemaEntry> schema;
    int numMeterOut = 0; // [curlop:meterout] channels discarded after compute
    std::vector<FaceplateMeter> faceplateMeters;
    std::vector<FAUSTFLOAT*> faceplateMeterZones;
    std::unique_ptr<std::atomic<float>[]> faceplateMeterValues;
    size_t faceplateMeterValueCount = 0;

    // T-617: voice-EXPANDED input channel contract — identical to
    // FaustSynthAdapter (chanStart_/perVoice_/voicesCapCh_). The expanded
    // input layout is expandDeclarationVoices(base, voices); the base
    // declaration is the paramNames classified by faustControlInputOf. Each
    // BASE param `b` maps to a first expanded channel chanStart[b]; per-voice
    // inputs (gate/pitch/velocity) occupy `voices` consecutive channels (perVoice=1),
    // value inputs one shared channel (perVoice=0). The VOICES cap rides the
    // last expanded channel (voicesCapCh, -1 when voices==1).
    int voices = 1;                     // per-instance voice count
    int automaticMultiMonoLanes = 1;    // prepared independent processor width
    int numControlInputs = 0;           // expanded control-channel count
    std::vector<int> chanStart;         // base param b → first expanded channel
    std::vector<int> perVoice;          // base param b → 1 if per-voice (gate/pitch)
    int voicesCapCh = -1;               // expanded channel carrying the VOICES cap
    int initializedSampleRate = 0;       // sample rate used for the last dsp::init

    ~ActiveProgram()
    {
        {
            // B-205: instance teardown touches libfaust state shared with
            // concurrent compiles — same global lock as the compile paths.
            std::lock_guard<std::mutex> faustLock(faustGlobalCompileMutex());
            for (auto*& inst : instances)
                if (inst) { delete inst; inst = nullptr; }
            instances.clear();
        }
        // OUTSIDE the lock: dropping the last factory reference runs
        // Factory::~Factory, which takes compileMutex itself — releasing
        // under our lock would self-deadlock (std::mutex is non-recursive).
        factory.reset();
    }
};

// T-318: two schemas are equivalent iff the param count, names, types,
// widgets, and ranges all match. Renaming a slider, changing its bounds, or
// reordering counts as a schema change (each materially affects the faceplate
// + ControlCore arrangement). Defaults are excluded from the comparison —
// the user changing only the `init` value should not trigger a sub-graph
// rebuild; it's a continuous knob position, not a structural change.
inline bool schemasEqual(const std::vector<ParamSchemaEntry>& a,
                         const std::vector<ParamSchemaEntry>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].name   != b[i].name)   return false;
        if (a[i].type   != b[i].type)   return false;
        if (a[i].widget != b[i].widget) return false;
        if (a[i].min    != b[i].min)    return false;
        if (a[i].max    != b[i].max)    return false;
    }
    return true;
}

// T-405 (ADR-0008): the node's ModuleContract. Sibling of
// NativeModuleContract — same projection idea, but the declaration source is
// the LIVE installed schema, which a recompile can change, so ports are
// re-projected at install time and topologyVersion bumps for consumers to
// re-read. Ports update on the compile-worker thread under `mu`; consumers
// follow the header's documented model (observe a higher topologyVersion on
// the message thread, then re-read) — the same window the installedSchema
// mirror has always had.
class FaustModuleContract final : public ModuleContract
{
public:
    LineageId    lineageId()   const override { return lineage_; }
    juce::String displayName() const override { return name_; }
    // Returns a reference per the interface; safe under the header's thread
    // model (declarations are message-thread-consumed — observe a higher
    // topologyVersion, then re-read). A lock here would release before the
    // caller dereferences, so it buys nothing; project() swaps the vector
    // wholesale under mu to keep writers serialized with each other.
    const std::vector<InputPortDecl>& inputPorts() const override { return ports_; }
    // F-075: the audio "OUT" bus + one control-output port per [curlop:cvout]
    // channel, in declaration order. Re-projected by project() on each install;
    // consumers observe a higher topologyVersion() and re-read.
    const std::vector<OutputPortDecl>& outputPorts() const override { return outPorts_; }
    // SPEC-014 §6: the substrate is uncapped; physical_voices is the per-
    // instance voice count this node renders (T-617). Set at construction
    // (build_faust_jit threads the schema/clip voice count); the engine
    // expands the declaration ×voices and FaustNode renders one DSP instance
    // per voice, raw-summed. Defaults to 1 (mono) when unset.
    VoicePolicy voicePolicy() const override { return { voices_, StealPolicy::LastStolen }; }
    void setVoices(std::uint32_t v) { voices_ = v < 1u ? 1u : v; }
    Flavour     flavour()     const override { return Flavour::Audio; }
    ModuleProcessingCapabilities processingCapabilities() const override
    { return processingCapabilities_; }
    std::uint64_t topologyVersion() const override
    { return topology_.load(std::memory_order_acquire); }

    void seed(const char* nodeName)
    {
        name_ = nodeName;
        // Identity of an unsaved, instance-local program. F-069 library
        // save/recall assigns persistent lineage when a module is saved;
        // until then each live node IS its own lineage.
        lineage_ = juce::Uuid();
    }

    // Re-project ports from `schema`; bump topologyVersion only when the
    // surface actually changed (install-in-place of an equal schema is not
    // a topology event).
    void project(const std::vector<std::string>& paramNames,
                 const std::vector<ParamSchemaEntry>& schema,
                 bool bumpIfChanged,
                 const std::vector<ControlOutputDecl>& controlOutputs = {},
                 ModuleProcessingCapabilities processingCapabilities = {})
    {
        // F-075: output ports = the audio "OUT" bus + one control-output port
        // per [curlop:cvout] channel, in declaration order.
        std::vector<OutputPortDecl> nextOut;
        nextOut.reserve(controlOutputs.size() + 1);
        nextOut.push_back({ "OUT", /*isControl*/ false, 0.0f, 1.0f });
        for (const auto& c : controlOutputs)
            nextOut.push_back({ c.name, /*isControl*/ true, c.min, c.max });

        std::vector<InputPortDecl> next;
        next.reserve(schema.size());
        // T-564 (adr-modules §"How modules receive control"): the declaration
        // IS the complete list of control inputs — no reserved gate/pitch/vel
        // trio. gate/freq/velocity are ordinary declared params; the control layer
        // types them by name at the delivery seam (faustControlInputOf in
        // EngineSlot::buildModuleVms), not here. One channel per declared port,
        // Faust slider order.
        for (const auto& p : schema) {
            if (p.type == ParamType::Display) continue; // visualisation, not a port
            next.push_back({ p.name, ParamAcceptance::ControlValue });
        }
        (void) paramNames; // channel order is schema order; names kept for symmetry

        std::lock_guard<std::mutex> lk(mu);
        const bool changed = portsDiffer(ports_, next)
            || outPortsDiffer(outPorts_, nextOut)
            || processingCapabilities_.version != processingCapabilities.version
            || processingCapabilities_.independentMono
                != processingCapabilities.independentMono
            || processingCapabilities_.independentVoiceCohorts
                != processingCapabilities.independentVoiceCohorts;
        ports_    = std::move(next);
        outPorts_ = std::move(nextOut);
        processingCapabilities_ = processingCapabilities;
        if (changed && bumpIfChanged)
            topology_.fetch_add(1, std::memory_order_acq_rel);
    }

private:
    static bool portsDiffer(const std::vector<InputPortDecl>& a,
                            const std::vector<InputPortDecl>& b)
    {
        return ! std::equal(a.begin(), a.end(), b.begin(), b.end(),
            [](const InputPortDecl& x, const InputPortDecl& y)
            { return x.name == y.name && x.acceptance == y.acceptance; });
    }

    static bool outPortsDiffer(const std::vector<OutputPortDecl>& a,
                               const std::vector<OutputPortDecl>& b)
    {
        return ! std::equal(a.begin(), a.end(), b.begin(), b.end(),
            [](const OutputPortDecl& x, const OutputPortDecl& y)
            { return x.name == y.name && x.isControl == y.isControl; });
    }

    mutable std::mutex          mu;
    juce::String                name_;
    LineageId                   lineage_;
    std::vector<InputPortDecl>  ports_;
    std::vector<OutputPortDecl> outPorts_{ { "OUT", false, 0.0f, 1.0f } };
    ModuleProcessingCapabilities processingCapabilities_;
    std::atomic<std::uint64_t>  topology_ { 1 };
    std::uint32_t               voices_ { 1 };   // T-617 per-instance voice count
};

} // anonymous namespace

struct FaustNode::Impl : private juce::Thread
{
    const char*                 name = "FaustJit";
    std::vector<std::string>    paramNames;
    int                         voices = 1;   // T-617 per-instance voice count
    int                         automaticMultiMonoLanes = 1;
    ModuleOversamplingFactor    oversamplingFactor = ModuleOversamplingFactor::X1;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    int                         oversamplingChannels = 0;
    double                      preparedHostSampleRate = 0.0;
    std::atomic<float>          oversamplingBoundaryLoad { 0.0f };
    std::atomic<bool>           oversamplingBoundaryLoadMeasured { false };
    int                         ctrlTapBlk = 0;  // CONTROL_TAP throttle (T-665)
    int                         paramInputTapBlk = 0; // FAUST_PARAM_INPUT throttle for graph-driven param inputs
    int                         psetTapBlk = 0;
    int                         b346CondTapBlk = 0;
    int                         b346PreBlk = 0;
    int                         b346ProbeBlk = 0;
    int                         b346TapBlk = 0;
    int                         b346ConsecZeroBlocks = 0;
    std::unique_ptr<std::atomic<float>[]> lastControlInputs;
    int                         lastControlInputCount = 0;
    std::vector<std::string>    perVoiceParams;   // SF-052 per-voice value inputs

    // T-405: the shared compilation service this node acquires factories
    // from. Non-owning; defaults to FaustRuntime::instance(), tests inject.
    FaustRuntime*               runtime = nullptr;

    // T-405 (ADR-0008): this node's declaration surface.
    FaustModuleContract         contract;

    // Hot-swap bundle. processBlock loads acquire; worker thread exchanges.
    std::atomic<ActiveProgram*> active { nullptr };

    // T-318: snapshot of the schema currently driving the faceplate and the
    // sub-graph's ControlCore arrangement. Written by:
    //   - construction (initial compile) — seeds with the first program's schema
    //   - install-in-place path (no schema change) — re-affirmed
    //   - never written by the schema-change path: when the schema changes the
    //     new program is NOT installed in this node, so this mirror stays as
    //     the *installed* schema. ApgGraphSync::rebuildSingleModule constructs
    //     a fresh FaustNode for the new schema.
    // Worker- and message-thread-readable under installedSchemaMu.
    mutable std::mutex            installedSchemaMu;
    std::vector<ParamSchemaEntry> installedSchema;

    // T-318: hot-swap callback. Invoked from the worker thread when a
    // successful recompile yields a schema differing from installedSchema.
    // Not invoked when schemas match (install-in-place path). Set once at
    // construction-time via setSchemaChangedCallback; read-only on the worker
    // thread after.
    FaustNode::SchemaChangedFn schemaChangedFn;

    // Single-slot graveyard. Replaced on each successful swap; the previous
    // occupant is deleted before the new one is parked here, guaranteeing
    // ≥1 full block boundary between the audio thread's last load() of the
    // old pointer and its release.
    std::unique_ptr<ActiveProgram> graveyard;

    // Worker-thread inbox. Mutex covers pendingSource + hasPending; the
    // worker drains under lock, compiles, then notifies.
    std::mutex                  threadStartMu;
    bool                        threadStarted = false;
    std::mutex                  inboxMu;
    std::string                 pendingSource;   // protected by inboxMu
    std::vector<ParamSchemaEntry> pendingValidatedSchema; // protected by inboxMu
    bool                        hasPendingValidatedSchema = false;
    bool                        hasPending = false;
    std::atomic<bool>           compileInFlight { false };

    // Message-thread-visible diagnostic. Written by the worker after each
    // compile attempt; read by lastCompileError().
    mutable std::mutex          errorMu;
    std::string                 lastError;

    // Sample rate is set in prepareToPlay; the worker reads it when calling
    // instance->init(). 48000 is a safe default that prepareToPlay corrects
    // before processBlock runs.
    std::atomic<int>            sampleRate { 48000 };

    // T-572: RT-safe scratch for the effect (audio-input) path — preallocated
    // in prepareToPlay, never resized on the audio thread. scratchIn holds a
    // private copy of the upstream audio channels (the input channels alias
    // the output channels in the flattened buffer, so compute() must not read
    // them after it has written output); scratchOut catches every DSP output.
    // Sized to the active program's arity × blockSize.
    juce::AudioBuffer<float>    scratchIn;
    juce::AudioBuffer<float>    scratchOut;
    // Cached compute() pointer arrays (point into scratchIn/scratchOut; stable
    // until the next setSize). Sized to the active program's arity in
    // prepareToPlay so processBlock allocates nothing.
    std::vector<FAUSTFLOAT*>    inPtrs;
    std::vector<FAUSTFLOAT*>    outPtrs;
    // Gate-retrigger fix: per-segment audio-input pointers (inPtrs offset to the
    // segment start) for the general path's segmented compute. Sized = inPtrs.
    std::vector<FAUSTFLOAT*>    inPtrsSeg;
    int                         preparedBlockSize = 0;

    // T-617: per-voice stereo-synth fast-path accumulation scratch.
    // Wider and mono programs use the arity-generic scratchOut path below.
    juce::AudioBuffer<float>    voiceScratch;
    FAUSTFLOAT*                 voiceOut[2] = { nullptr, nullptr };
    // A1R.5: each qualified physical voice receives a private stereo span.
    // The serial path still reduces these spans in ascending voice order; the
    // compiled scheduler can therefore later partition compute without ever
    // sharing a voice-output buffer between workers.
    juce::AudioBuffer<float>    voiceCohortScratch;
    struct VoiceCohortJob {
        ActiveProgram* program = nullptr;
        int voice = 0;
        FAUSTFLOAT* output[2] { nullptr, nullptr };
        int samples = 0;
    };
    std::vector<VoiceCohortJob> voiceCohortJobs;
    std::vector<transport::RealtimeWorkerTeam::Job> voiceCohortWork;
    std::atomic<transport::RealtimeWorkerTeam*> voiceCohortTeam { nullptr };

    static void runVoiceCohortJob(void* opaque) noexcept
    {
        auto& job = *static_cast<VoiceCohortJob*>(opaque);
        job.program->instances[static_cast<std::size_t>(job.voice)]->compute(
            job.samples, nullptr, job.output);
    }

    // Gate-retrigger fix (mirrors FaustSynthAdapter's segmented compute): a
    // snapshot of the control-input channels [0..ncc), taken BEFORE any compute()
    // writes output (output ch0/1 alias control ch0/1 in the in-place graph
    // buffer). The block is computed in SEGMENTS split wherever any control
    // channel changes, so a `button("gate")` zone sees the gate drop to 0 between
    // two touching notes and the envelope re-arms — `#kick / #kick` re-strikes in
    // mono AND poly, instead of block-granular [last] stepping over the edge.
    juce::AudioBuffer<float>    ctrlSnapshot;
    FaustNode::PrepareTiming    lastPrepareTiming;
    FaustNode::ConstructionTiming lastConstructionTiming;

    Impl() : juce::Thread("FaustJitCompile",
                          kFaustCompilerThreadStackBytes) {}

    ~Impl() override
    {
        // Stop the worker before tearing the active program down — the worker
        // touches `active`. A libfaust compile cannot be cancelled safely: the
        // worker may hold the process-wide compile mutex, so a finite
        // stopThread timeout can force-kill it and strand that mutex forever.
        // Signal and wake first, then join without JUCE's cancellation path.
        signalThreadShouldExit();
        if (threadStarted) {
            notify();   // private-inheritance OK from inside Impl
            stopThread(-1);
        }

        if (auto* a = active.exchange(nullptr, std::memory_order_acq_rel)) {
            delete a;
        }
        graveyard.reset();
    }

    void start()
    {
        std::lock_guard<std::mutex> lock(threadStartMu);
        if (!threadStarted) {
            // Live source compilation is intentionally below presentation and
            // audio scheduling priority. libfaust/LLVM inherits this worker's
            // QoS on macOS, so an editor debounce cannot starve WebView frames.
            startThread(juce::Thread::Priority::background);
            threadStarted = true;
        }
    }
    void wake()  { notify(); }

    // Synchronous program build. Returns a fresh ActiveProgram on success,
    // or nullptr (with errOut populated) on failure. Caller decides whether
    // to swap it in. Used for the construction-time build and the worker's
    // recompile path.
    //
    // T-405: the factory comes from the runtime's cache (acquire compiles
    // only on a miss, under the same B-205 mutex); this method then creates
    // THIS node's instance and walks its UI tree — instances and zone
    // pointers are per-node, never shared.
    static ActiveProgram* compileSync(FaustRuntime& runtime,
                                       const std::string& moduleName,
                                       const std::string& source,
                                       const std::vector<std::string>& paramNames,
                                       int sampleRate,
                                       int voices,
                                       int automaticMultiMonoLanes,
                                       const std::vector<std::string>& perVoiceParams,
                                       bool allowCompile,
                                       std::string& errOut,
                                       FaustNode::ConstructionTiming* timingOut = nullptr)
    {
        const auto totalStart = std::chrono::steady_clock::now();
        auto phaseStart = totalStart;
        FaustNode::ConstructionTiming timing;
        auto markPhase = [&] (double& target) {
            const auto now = std::chrono::steady_clock::now();
            target += std::chrono::duration<double, std::micro>(now - phaseStart).count();
            phaseStart = now;
        };
        auto finishTiming = [&] {
            timing.totalUs = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - totalStart).count();
            if (timingOut != nullptr)
                *timingOut = timing;
        };

        errOut.clear();
        const int nVoices = voices < 1 ? 1 : voices;
        const int nLanes = automaticMultiMonoLanes < 1
            ? 1 : automaticMultiMonoLanes;
        if (nLanes > 1 && nVoices > 1) {
            errOut =
                "automatic multi-mono rejects polyphonic module instances";
            finishTiming();
            return nullptr;
        }
        auto factory = allowCompile
            ? runtime.acquire(moduleName, source, errOut)
            : runtime.loadFromStore(source, runtime.defaultBackend(), errOut);
        markPhase(timing.factoryAcquireUs);
        if (!factory) {
            finishTiming();
            return nullptr;
        }

        // B-205: instance creation + UI walk touch libfaust state shared
        // with concurrent compiles — same global lock as the compile paths.
        std::unique_lock<std::mutex> faustLock(faustGlobalCompileMutex());
        markPhase(timing.compileLockWaitUs);

        auto* prog = new ActiveProgram();
        prog->factory = factory;
        prog->voices  = nVoices;
        prog->automaticMultiMonoLanes = nLanes;
        prog->initializedSampleRate = sampleRate;
        const bool jitClassInitHandled =
            ensureJitFactoryClassInitLocked(factory, sampleRate);

        // T-617: create N independent instances from the one shared factory.
        // Each gets its own zone memory; a UI walk per instance resolves that
        // instance's distinct zones (mirror of FaustSynthAdapter::dsps_).
        const int instanceCount = nLanes > 1 ? nLanes : nVoices;
        prog->instances.assign((size_t) instanceCount, nullptr);
        prog->uis.resize(1);
        prog->zones.assign((size_t) instanceCount,
                           std::vector<FAUSTFLOAT*>(paramNames.size(), nullptr));
        for (int v = 0; v < instanceCount; ++v) {
            auto* instance = factory->raw()->createDSPInstance();
            markPhase(timing.instanceCreateUs);
            if (!instance) {
                errOut = "createDSPInstance: null";
                faustLock.unlock();
                delete prog;   // ~ActiveProgram deletes any instances already made
                finishTiming();
                return nullptr;
            }
            if (v == 0 && !jitClassInitHandled)
                instance->init(sampleRate);
            else
                instance->instanceInit(sampleRate);
            markPhase(timing.instanceInitUs);
            prog->instances[(size_t) v] = instance;
            timing.instanceCreateInitUs =
                timing.instanceCreateUs + timing.instanceInitUs;

            if (v == 0)
                prog->uis[0].reserveZones(paramNames.size());
            ZoneCaptureUI zoneUi(paramNames.size());
            if (v == 0)
                instance->buildUserInterface(&prog->uis[0]);
            else
                instance->buildUserInterface(&zoneUi);
            // Resolve zones in the *original* paramNames order. Sliders renamed
            // in the new source resolve to nullptr; processBlock no-ops on null.
            for (size_t i = 0; i < paramNames.size(); ++i) {
                prog->zones[(size_t) v][i] = v == 0
                    ? prog->uis[0].find(paramNames[i])
                    : zoneUi.find(paramNames[i]);
                // B-346 probe: null zone here = processBlock can never write that
                // control → a null gate zone silences the whole module (ADSR never
                // opens). One shot per ctor: log param name + resolved/null.
                if (v == 0)
                    CDBG(FAUST_RUNTIME,
                         "zone-resolve name='%s' -> %s",
                         paramNames[i].c_str(),
                         prog->zones[0][i] ? "ok" : "NULL");
            }
            markPhase(timing.uiZoneResolveUs);
        }

        auto* instance0 = prog->instances[0];
        // T-572: capture audio I/O arity — declares synth (0 in) vs effect (>0 in).
        prog->numAudioIn  = instance0->getNumInputs();
        const auto sourceUiMetadata =
            sourceUiMetadataForFaustSource(source, prog->uis[0]);
        // F-075: split `process` outputs into audio + control channels. The
        // control channels are the trailing channels marked by `[curlop:cvout]`
        // bargraphs (synthesiseControlOutputs, in declaration order). Audio is
        // whatever remains at the front. meterout channels are trailing
        // telemetry only: compute() still writes them, but they never reach
        // the module audio bus. numDspOut is the raw arity compute() writes.
        prog->numDspOut       = instance0->getNumOutputs();
        prog->controlOutputs  = sourceUiMetadata.controlOutputs;
        prog->numControlOut   = (int) prog->controlOutputs.size();
        prog->numMeterOut     = sourceUiMetadata.meterOutputs;
        prog->numAudioOut     = juce::jmax(0, prog->numDspOut
            - prog->numControlOut - prog->numMeterOut);
        prog->processingCapabilities =
            faustProcessingCapabilitiesFromSource(source);
        if (nLanes > 1
            && (! prog->processingCapabilities.independentMono
                || prog->numAudioIn != 1
                || prog->numAudioOut != 1
                || prog->numControlOut != 0)) {
            errOut =
                "automatic multi-mono requires declare curlop_processing "
                "\"independent-mono\" and exact mono audio input/output "
                "without control outputs";
            faustLock.unlock();
            delete prog;
            finishTiming();
            return nullptr;
        }
        markPhase(timing.ioControlOutputUs);

        // T-617: build the voice-EXPANDED input channel contract — identical
        // to FaustSynthAdapter's chanStart_/perVoice_/voicesCapCh_ walk so the
        // ScriptNode that feeds this node (build_faust_jit, expanded via the
        // same expandDeclarationVoices) and this node agree channel-for-channel.
        // The base declaration is paramNames classified by faustControlInputOf.
        vm::ModuleDeclaration base;
        base.inputs.reserve(paramNames.size());
        for (const auto& nm : paramNames)
            base.inputs.push_back(faustControlInputOf(nm, /*buttonLike*/ false,
                                                      0.0f, 0.0f, 1.0f, ""));
        // SF-052: mark per-voice value inputs BEFORE expanding + before the
        // chanStart/perVoice walk, so a {} voice-stack's value expands ×voices
        // into its own channels (each voice's instance gets its own value). The
        // identical marked base feeds expandedControlInputCount (the bus width)
        // and the build_faust_jit ScriptNode — all three agree channel-for-channel.
        markPerVoiceParams(base, perVoiceParams);
        const vm::ModuleDeclaration expanded =
            vm::expandDeclarationVoices(base, nVoices);
        prog->numControlInputs = (int) expanded.inputs.size();

        prog->chanStart.reserve(base.inputs.size());
        prog->perVoice.reserve(base.inputs.size());
        int chan = 0;
        for (const auto& in : base.inputs) {
            const bool pv = in.type == vm::SignalType::Gate
                         || in.type == vm::SignalType::Pitch
                         || in.type == vm::SignalType::Velocity
                         || in.perVoice;
            prog->chanStart.push_back(chan);
            prog->perVoice.push_back(pv ? 1 : 0);
            chan += pv ? nVoices : 1;
        }
        if (nVoices > 1 && ! expanded.inputs.empty()
            && expanded.inputs.back().name == "VOICES")
            prog->voicesCapCh = prog->numControlInputs - 1;
        markPhase(timing.inputContractUs);

        // T-318: snapshot the synthesised faceplate schema in Faust declaration
        // order. Compared against the previous program's schema by the
        // hot-swap completion path (Step 3) — a diff triggers a per-instance
        // sub-graph rebuild so faceplate widget set + ControlCore arrangement
        // both follow the source's slider declarations.
        prog->schema = sourceUiMetadata.schema;
        markPhase(timing.schemaUs);
        {
            prog->faceplateMeters = sourceUiMetadata.faceplate.meters;
            prog->faceplateMeterZones = prog->uis[0].faceplateMeterZones();
            prog->faceplateMeterValueCount = std::min(prog->faceplateMeters.size(),
                                                       prog->faceplateMeterZones.size());
            if (prog->faceplateMeterValueCount > 0)
            {
                prog->faceplateMeterValues.reset(new std::atomic<float>[prog->faceplateMeterValueCount]);
                for (size_t i = 0; i < prog->faceplateMeterValueCount; ++i)
                    prog->faceplateMeterValues[i].store(0.0f, std::memory_order_relaxed);
            }
        }
        markPhase(timing.faceplateUs);

        // T-357: synthesise the normalised faceplate schema (ADR-009) from the
        // same UI walk. Not yet stored on the program — T357-5 routes it into
        // the module registry; here it is extracted + logged so a Path-A
        // `core.faust_jit` add proves the extractor populates params / meters
        // / groups from the live libfaust UI tree.
        if (CurlopDebug::on(CurlopDebug::FAUST_SCHEMA)) {
            auto fp = prog->uis[0].synthesiseFaceplateSchema();
            mergeFaustGroupSourceMetadata (fp, source);
            CDBG(FAUST_SCHEMA,
                 "faust-extract module=%s params=%d meters=%d groups=%d",
                 moduleName.c_str(),
                 (int) fp.params.size(), (int) fp.meters.size(),
                 countDescendantGroups(fp.rootGroup));
        }
        markPhase(timing.debugSchemaUs);

        finishTiming();
        return prog;
    }

    // Atomic swap. Caller hands ownership of `next` to Impl; we exchange it
    // into `active`, sweep the previous graveyard, and park the old active
    // in graveyard's slot.
    void prepareScratchForProgram(ActiveProgram* prog, int blockSize)
    {
        if (prog == nullptr || blockSize <= 0) return;
        const int bs = juce::jmax(1, blockSize);
        preparedBlockSize = bs;
        scratchIn.setSize(juce::jmax(1, prog->numAudioIn), bs, false, false, true);
        scratchOut.setSize(juce::jmax(1, prog->numDspOut), bs, false, false, true);
        inPtrs.resize((size_t) juce::jmax(0, prog->numAudioIn));
        for (size_t c = 0; c < inPtrs.size(); ++c)
            inPtrs[c] = scratchIn.getWritePointer((int) c);
        inPtrsSeg.resize(inPtrs.size());
        outPtrs.resize((size_t) juce::jmax(1, prog->numDspOut));
        for (size_t c = 0; c < outPtrs.size(); ++c)
            outPtrs[c] = scratchOut.getWritePointer((int) c);
        voiceScratch.setSize(2, bs, false, false, true);
        voiceOut[0] = voiceScratch.getWritePointer(0);
        voiceOut[1] = voiceScratch.getWritePointer(1);
        ctrlSnapshot.setSize(juce::jmax(1, prog->numControlInputs), bs, false, false, true);
    }

    void installNewProgram(ActiveProgram* next)
    {
        if (active.load(std::memory_order_acquire) == nullptr)
            prepareScratchForProgram(next, preparedBlockSize);
        auto* old = active.exchange(next, std::memory_order_acq_rel);
        // Free the *previous* graveyard occupant first — it's been off-active
        // for at least one swap cycle, so the audio thread can no longer hold
        // a pointer to it.
        graveyard.reset();
        if (old) graveyard.reset(old);
    }

    void setError(const std::string& e)
    {
        std::lock_guard<std::mutex> lk(errorMu);
        lastError = e;
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            wait(-1);
            if (threadShouldExit()) break;

            // Drain pending source. Loop because new requests may arrive
            // while we compile — we only build the *latest* one.
            for (;;) {
                std::string source;
                std::vector<ParamSchemaEntry> validatedSchema;
                bool hasValidatedSchema = false;
                {
                    std::lock_guard<std::mutex> lk(inboxMu);
                    if (!hasPending) break;
                    source = std::move(pendingSource);
                    pendingSource.clear();
                    validatedSchema = std::move(pendingValidatedSchema);
                    pendingValidatedSchema.clear();
                    hasValidatedSchema = hasPendingValidatedSchema;
                    hasPendingValidatedSchema = false;
                    // Keep isCompilePending() truthful across the inbox-to-worker
                    // handoff. Lazy-started workers otherwise expose a brief false-idle
                    // window after clearing hasPending but before compilation begins.
                    compileInFlight.store(true, std::memory_order_release);
                    hasPending = false;
                }

                std::string err;
                auto* prog = compileSync(*runtime, name, source, paramNames,
                                          sampleRate.load(std::memory_order_acquire),
                                          voices,
                                          automaticMultiMonoLanes,
                                          perVoiceParams,
                                          /*allowCompile=*/true,
                                          err);
                if (prog) {
                    // T-318: schema diff against the currently-installed program.
                    // Equal → install-in-place (cheap zone re-resolve only, today's
                    // path). Different → don't install; hand off to the
                    // schemaChangedFn so the host can rebuild this module's
                    // sub-graph with the new ControlCore arrangement. The new
                    // FaustNode that replaces this one will recompile the same
                    // source — a minor (~ms) double-work cost we accept for the
                    // simpler flow (no need to expose ActiveProgram as a public
                    // handoff type). Note: the rebuild is the only audio glitch;
                    // skipping the in-place install means we don't double-glitch.
                    std::vector<ParamSchemaEntry> previous;
                    {
                        std::lock_guard<std::mutex> lk(installedSchemaMu);
                        previous = installedSchema;
                    }
                    // T-572: a change in audio I/O arity (synth ↔ effect, or a
                    // change in audio-input count) is a structural change even
                    // when the slider schema is identical — it flips the outer
                    // audio-input bus (hasInput) and the sub-graph wiring. Force
                    // the rebuild path so applyFromGraphState reconstructs the
                    // module with the correct bus, exactly like a slider change.
                    auto* cur = active.load(std::memory_order_acquire);
                    const int curIn  = cur ? cur->numAudioIn  : 0;
                    const int curOut = cur ? cur->numDspOut : 0;
                    // F-075: compare RAW output arity (numDspOut), so adding or
                    // removing a control-output channel — which leaves the audio
                    // count and slider schema unchanged — still forces the rebuild
                    // path that re-projects the node's output sockets.
                    const bool arityChanged =
                        (prog->numAudioIn != curIn) || (prog->numDspOut != curOut);
                    const bool capabilityChanged = cur != nullptr
                        && (prog->processingCapabilities.version
                                != cur->processingCapabilities.version
                            || prog->processingCapabilities.independentMono
                                != cur->processingCapabilities.independentMono
                            || prog->processingCapabilities.independentVoiceCohorts
                                != cur->processingCapabilities.independentVoiceCohorts);
                    // A compiled cohort holds the program's per-voice DSP
                    // instances on worker threads. A name-stable source edit
                    // must therefore rebuild and publish a fresh bundle,
                    // never atomically replace this program underneath an
                    // in-flight cohort wave.
                    const bool cohortRebuildRequired = cur != nullptr
                        && (cur->processingCapabilities.independentVoiceCohorts
                            || prog->processingCapabilities.independentVoiceCohorts);
                    // B-1679: EventRouter has already compiled and persisted
                    // the exact live-authoring source before queueing this
                    // worker. Name-stable edits may legitimately change range
                    // or widget metadata without changing ControlCore wiring.
                    // Compare the acquired program with that validated result,
                    // not the older installed presentation schema. Raw I/O
                    // arity remains an independent structural safety gate.
                    const auto& expectedSchema = hasValidatedSchema
                        ? validatedSchema : previous;
                    const bool eq = schemasEqual(expectedSchema, prog->schema)
                                 && !arityChanged
                                 && !capabilityChanged
                                 && !cohortRebuildRequired;
                    bool accepted = true;
                    if (eq) {
                        const auto installed = prog->schema;
                        installNewProgram(prog);
                        {
                            std::lock_guard<std::mutex> lk(installedSchemaMu);
                            installedSchema = installed;
                        }
                        // T-405: equal schema = same declaration surface;
                        // re-project (ranges may differ) without a version bump.
                        // F-075: pass control outputs (equal arity ⇒ equal set).
                        contract.project(paramNames, installed, /*bumpIfChanged*/ false,
                                         prog->controlOutputs,
                                         prog->processingCapabilities);
                    } else if (schemaChangedFn) {
                        // Discard the just-compiled program; the rebuild path
                        // will compile fresh inside the new FaustNode's
                        // construction. installedSchema is *not* updated here —
                        // it tracks what's installed in THIS node, which is
                        // about to be replaced.
                        auto newSchema = prog->schema;
                        delete prog;
                        schemaChangedFn(source, std::move(newSchema));
                    } else if (arityChanged) {
                        // Exact bus arity is immutable for this prepared node.
                        // Production installs a rebuild callback; isolated
                        // nodes without one must reject rather than install a
                        // program that can overrun or truncate the fixed bus.
                        delete prog;
                        setError(
                            "Faust audio I/O arity changed; module rebuild required");
                        accepted = false;
                    } else {
                        // No callback wired (placeholder construction, tests).
                        // Fall back to installing in place — the audio path
                        // tolerates this because zone resolution by name handles
                        // renames as nullptr writes (no-op) and new sliders
                        // simply aren't driven from outside the program.
                        const auto installed = prog->schema;
                        installNewProgram(prog);
                        {
                            std::lock_guard<std::mutex> lk(installedSchemaMu);
                            installedSchema = installed;
                        }
                        // T-405: this install CHANGED the declaration surface
                        // — re-project ports and bump topologyVersion so
                        // contract consumers re-read (ADR-0008 lifecycle).
                        contract.project(paramNames, installed, /*bumpIfChanged*/ true,
                                         prog->controlOutputs,
                                         prog->processingCapabilities);
                    }
                    if (accepted)
                        setError({});
                } else {
                    // Compile failed — leave previous active in place.
                    setError(err);
                }

                compileInFlight.store(false, std::memory_order_release);
            }
        }
    }
};

namespace {
struct FaustBusLayout
{
    int audioInputs = 2;
    int audioOutputs = 2;
    int controlOutputs = 0;
    int meterOutputs = 0;
};

FaustBusLayout faustBusLayoutFor(const std::string& source,
                                 FaustRuntime* runtime,
                                 bool allowSynchronousCompile)
{
    std::string errOut;
    auto& rt = runtime ? *runtime : FaustRuntime::instance();
    auto factory = allowSynchronousCompile
        ? rt.acquire("bus-layout-probe", source, errOut)
        : rt.loadFromStore(source, rt.defaultBackend(), errOut);
    if (! factory)
        return {};

    std::lock_guard<std::mutex> faustLock(faustGlobalCompileMutex());
    auto* instance = factory->raw()->createDSPInstance();
    if (instance == nullptr)
        return {};
    instance->init(48000);
    ParamCaptureUI ui;
    instance->buildUserInterface(&ui);
    const int controlOutputs =
        static_cast<int>(ui.synthesiseControlOutputs().size());
    const int meterOutputs = ui.meterOutputCount();
    FaustBusLayout layout {
        juce::jmax(0, instance->getNumInputs()),
        juce::jmax(0, instance->getNumOutputs() - controlOutputs - meterOutputs),
        controlOutputs,
        meterOutputs
    };
    delete instance;
    return layout;
}

DspNode::BusesProperties faustBusesFor(
    const std::string& source,
    FaustRuntime* runtime,
    bool allowSynchronousCompile,
    int controlInputs,
    int automaticMultiMonoLanes)
{
    const auto layout =
        faustBusLayoutFor(source, runtime, allowSynchronousCompile);
    DspNode::BusesProperties buses;
    const int lanes = juce::jmax(1, automaticMultiMonoLanes);
    const int audioInputs =
        lanes > 1 && layout.audioInputs == 1 ? lanes : layout.audioInputs;
    const int audioOutputs =
        lanes > 1 && layout.audioOutputs == 1 && layout.controlOutputs == 0
            && layout.meterOutputs == 0
            ? lanes : layout.audioOutputs;
    const int inputs = juce::jmax(0, controlInputs) + audioInputs;
    const int outputs = audioOutputs + layout.controlOutputs;
    if (inputs > 0)
        buses = buses.withInput(
            "In", juce::AudioChannelSet::discreteChannels(inputs));
    if (outputs > 0)
        buses = buses.withOutput(
            "Out", juce::AudioChannelSet::discreteChannels(outputs));
    return buses;
}
} // anonymous namespace

const char* FaustNode::placeholderSource()
{
    return
        "import(\"stdfaust.lib\");\n"
        "FREQ = hslider(\"FREQ\", 440, 20, 8000, 0.01);\n"
        "GAIN = hslider(\"GAIN\", 0.3, 0, 1, 0.001);\n"
        "process = os.osc(FREQ) * GAIN <: _, _;\n";
}

FaustNode::FaustNode(const char* name,
                           const std::string& source,
                           const std::vector<std::string>& paramNames,
                           FaustRuntime* runtime,
                           int voices,
                           const std::vector<std::string>& perVoiceParams,
                           bool deferColdCompile,
                           int automaticMultiMonoLanes,
                           ModuleOversamplingFactor oversamplingFactor)
  : DspNode(faustBusesFor(
        source, runtime, !deferColdCompile,
        expandedControlInputCount(paramNames, voices, perVoiceParams),
        automaticMultiMonoLanes)),
    impl_(std::make_unique<Impl>())
{
    impl_->name           = name;
    impl_->paramNames     = paramNames;
    impl_->voices         = voices < 1 ? 1 : voices;   // T-617 per-instance voice count
    impl_->automaticMultiMonoLanes =
        automaticMultiMonoLanes < 1 ? 1 : automaticMultiMonoLanes;
    impl_->oversamplingFactor = oversamplingFactor;
    impl_->perVoiceParams = perVoiceParams;            // SF-052 per-voice value inputs
    // T-405: factories come from the shared runtime — the app-wide instance
    // unless a test injects its own for isolation.
    impl_->runtime    = runtime ? runtime : &FaustRuntime::instance();
    impl_->contract.seed(name);
    impl_->contract.setVoices((std::uint32_t) impl_->voices);   // T-617

    std::string err;
    FaustNode::ConstructionTiming constructionTiming;
    auto* prog = Impl::compileSync(*impl_->runtime, impl_->name, source,
                                    impl_->paramNames,
                                    impl_->sampleRate.load(std::memory_order_acquire),
                                    impl_->voices,
                                    impl_->automaticMultiMonoLanes,
                                    impl_->perVoiceParams,
                                    /*allowCompile=*/!deferColdCompile,
                                    err,
                                    &constructionTiming);
    impl_->lastConstructionTiming = constructionTiming;
    if (prog) {
        // T-318: seed installedSchema with the construction-time program so
        // the first setSource() call has a baseline to diff against. Without
        // this every first edit would be reported as a schema change.
        {
            std::lock_guard<std::mutex> lk(impl_->installedSchemaMu);
            impl_->installedSchema = prog->schema;
        }
        // T-405 (ADR-0008): first declaration projection — version stays 1.
        // F-075: include the program's control outputs in the projection.
        impl_->contract.project(impl_->paramNames, prog->schema,
                                /*bumpIfChanged*/ false,
                                prog->controlOutputs,
                                prog->processingCapabilities);
        impl_->active.store(prog, std::memory_order_release);
    } else {
        impl_->setError(err);
    }

    if (deferColdCompile && prog == nullptr)
        setSource(source);
}

const ModuleContract& FaustNode::contract() const
{
    return impl_->contract;
}

FaustNode::~FaustNode() = default;

const juce::String FaustNode::getName() const { return impl_->name; }

int FaustNode::numAudioInputs() const
{
    auto* a = impl_->active.load(std::memory_order_acquire);
    return a ? (a->automaticMultiMonoLanes > 1
                    ? a->automaticMultiMonoLanes
                    : a->numAudioIn)
             : 0;
}

int FaustNode::numAudioOutputs() const
{
    auto* a = impl_->active.load(std::memory_order_acquire);
    return a ? (a->automaticMultiMonoLanes > 1
                    ? a->automaticMultiMonoLanes
                    : a->numAudioOut)
             : 0;
}

int FaustNode::numControlOutputs() const
{
    auto* a = impl_->active.load(std::memory_order_acquire);
    return a ? a->numControlOut : 0;
}

std::vector<ControlOutputDecl> FaustNode::controlOutputs() const
{
    auto* a = impl_->active.load(std::memory_order_acquire);
    return a ? a->controlOutputs : std::vector<ControlOutputDecl>{};
}

void FaustNode::prepareToPlay(double sr, int blockSize)
{
    const auto prepareStart = std::chrono::steady_clock::now();
    // Until we have inspected the compiled port layout, keep the background
    // compiler at host rate. The active program below receives the selected
    // rate only when it is eligible for this first, audio-only boundary.
    impl_->sampleRate.store((int) sr, std::memory_order_release);
    impl_->preparedHostSampleRate = sr;
    impl_->oversamplingBoundaryLoad.store(0.0f, std::memory_order_relaxed);
    impl_->oversamplingBoundaryLoadMeasured.store(false, std::memory_order_relaxed);
    impl_->preparedBlockSize = juce::jmax(1, blockSize);
    impl_->lastControlInputCount = (int) impl_->paramNames.size();
    impl_->lastControlInputs.reset(new std::atomic<float>[(size_t) impl_->lastControlInputCount]);
    for (int i = 0; i < impl_->lastControlInputCount; ++i)
        impl_->lastControlInputs[(size_t) i].store(std::numeric_limits<float>::quiet_NaN(),
                                                   std::memory_order_relaxed);
    const auto controlStateEnd = std::chrono::steady_clock::now();

    double sampleRateInitUs = 0.0;
    if (auto* a = impl_->active.load(std::memory_order_acquire)) {
        const int requestedFactor = moduleOversamplingMultiplier(impl_->oversamplingFactor);
        const bool oversamplingEligible = requestedFactor > 1 && a->numControlOut == 0
            && impl_->automaticMultiMonoLanes == 1;
        const int factor = oversamplingEligible ? requestedFactor : 1;
        impl_->sampleRate.store((int) sr * factor, std::memory_order_release);
        const int srInt = (int) sr * factor;
        if (a->initializedSampleRate != srInt) {
            const auto sampleRateInitStart = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> faustLock(faustGlobalCompileMutex());
            // T-617: re-init EVERY voice instance + re-resolve its zones when
            // the host sample rate changes. init() resets each instance's
            // zones to declared defaults, so the fresh UI walk per instance
            // restores the distinct zone pointers.
            const bool jitClassInitHandled =
                ensureJitFactoryClassInitLocked(a->factory, srInt);
            for (int v = 0; v < (int) a->instances.size(); ++v) {
                if (v == 0 && !jitClassInitHandled)
                    a->instances[(size_t) v]->init(srInt);
                else
                    a->instances[(size_t) v]->instanceInit(srInt);
                ZoneCaptureUI zoneUi(impl_->paramNames.size());
                if (v == 0) {
                    a->uis[0].clear();
                    a->uis[0].reserveZones(impl_->paramNames.size());
                    a->instances[(size_t) v]->buildUserInterface(&a->uis[0]);
                } else {
                    a->instances[(size_t) v]->buildUserInterface(&zoneUi);
                }
                for (size_t i = 0; i < impl_->paramNames.size(); ++i)
                    a->zones[(size_t) v][i] = v == 0
                        ? a->uis[0].find(impl_->paramNames[i])
                        : zoneUi.find(impl_->paramNames[i]);
                if (v == 0)
                    a->faceplateMeterZones = a->uis[0].faceplateMeterZones();
            }
            a->initializedSampleRate = srInt;
            sampleRateInitUs = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - sampleRateInitStart).count();
        }

        const auto scratchStart = std::chrono::steady_clock::now();
        // T-572: preallocate the effect-path scratch (RT-safe — never resized
        // in processBlock). Arity is fixed for this node's lifetime (a hot-swap
        // that changes it reconstructs the node and re-prepares). >=1 channel
        // keeps getWritePointer valid even for the synth (0-input) path.
        const int bs = juce::jmax(1, blockSize);
        const int rateBlockSize = bs * factor;
        const int oversamplingChannels = oversamplingEligible
            ? juce::jmax(1, juce::jmax(a->numAudioIn, a->numAudioOut))
            : 0;
        impl_->preparedBlockSize = bs;
        impl_->scratchIn.setSize(
            juce::jmax(juce::jmax(juce::jmax(1, a->numAudioIn),
                                  a->automaticMultiMonoLanes),
                       oversamplingChannels),
            rateBlockSize, false, false, true);
        // T-617: the scratch holds ONE voice's compute output; outL/outR
        // accumulate across voices. F-075: sized to the RAW DSP output arity
        // (numDspOut = audio + control channels), because compute() writes every
        // declared output channel — undersizing to the audio-only count would
        // overrun on a module that also emits control outputs. Per-voice render
        // reuses it serially.
        impl_->scratchOut.setSize(juce::jmax(1, a->numDspOut), rateBlockSize, false, false, true);
        // Cache compute() pointer arrays (exactly arity-length so compute reads
        // only valid pointers, even if the DSP declares >2 audio I/O).
        impl_->inPtrs.resize((size_t) juce::jmax(0, a->numAudioIn));
        for (size_t c = 0; c < impl_->inPtrs.size(); ++c)
            impl_->inPtrs[c] = impl_->scratchIn.getWritePointer((int) c);
        impl_->inPtrsSeg.resize(impl_->inPtrs.size());   // gate-retrigger fix: per-segment offsets
        impl_->outPtrs.resize((size_t) juce::jmax(1, a->numDspOut));
        for (size_t c = 0; c < impl_->outPtrs.size(); ++c)
            impl_->outPtrs[c] = impl_->scratchOut.getWritePointer((int) c);
        // T-617: per-voice synth-path accumulation scratch (nIn==0 fast path).
        // The synth fast-path computes each voice into voiceScratch then sums
        // into outL/outR — RT-safe, sized here. Two channels (stereo synth).
        impl_->voiceScratch.setSize(2, rateBlockSize, false, false, true);
        impl_->voiceOut[0] = impl_->voiceScratch.getWritePointer(0);
        impl_->voiceOut[1] = impl_->voiceScratch.getWritePointer(1);
        const bool cohortShape = a->processingCapabilities.independentVoiceCohorts
            && a->voices > 1 && a->numAudioIn == 0 && a->numAudioOut == 2
            && a->numControlOut == 0 && a->automaticMultiMonoLanes == 1
            && impl_->oversamplingFactor == ModuleOversamplingFactor::X1;
        impl_->voiceCohortScratch.setSize(
            cohortShape ? 2 * a->voices : 0, bs, false, false, true);
        impl_->voiceCohortJobs.resize(
            cohortShape ? static_cast<std::size_t>(a->voices - 1) : 0u);
        impl_->voiceCohortWork.resize(impl_->voiceCohortJobs.size());
        // Gate-retrigger fix: control-channel snapshot scratch, so the segmented
        // compute reads pristine control values after output (aliasing control
        // ch0/1) is written. >=1 channel keeps getWritePointer valid.
        impl_->ctrlSnapshot.setSize(juce::jmax(1, a->numControlInputs), bs, false, false, true);
        impl_->oversampling.reset();
        impl_->oversamplingChannels = 0;
        if (oversamplingEligible) {
            const int channels = oversamplingChannels;
            impl_->oversampling = std::make_unique<juce::dsp::Oversampling<float>>(
                static_cast<std::size_t>(channels), factor == 2 ? 1u : 2u,
                juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                true, true);
            impl_->oversampling->initProcessing(static_cast<std::size_t>(bs));
            impl_->oversamplingChannels = channels;
            setLatencySamples((int) impl_->oversampling->getLatencyInSamples());
        } else {
            setLatencySamples(0);
        }
        const auto prepareEnd = std::chrono::steady_clock::now();
        impl_->lastPrepareTiming.controlStateUs = std::chrono::duration<double, std::micro>(
            controlStateEnd - prepareStart).count();
        impl_->lastPrepareTiming.sampleRateInitUs = sampleRateInitUs;
        impl_->lastPrepareTiming.scratchUs = std::chrono::duration<double, std::micro>(
            prepareEnd - scratchStart).count();
        impl_->lastPrepareTiming.totalUs = std::chrono::duration<double, std::micro>(
            prepareEnd - prepareStart).count();
        return;
    }

    const auto prepareEnd = std::chrono::steady_clock::now();
    impl_->lastPrepareTiming.controlStateUs = std::chrono::duration<double, std::micro>(
        controlStateEnd - prepareStart).count();
    impl_->lastPrepareTiming.sampleRateInitUs = 0.0;
    impl_->lastPrepareTiming.scratchUs = 0.0;
    impl_->lastPrepareTiming.totalUs = std::chrono::duration<double, std::micro>(
        prepareEnd - prepareStart).count();
}

void FaustNode::reset()
{
    // Compiled-plan calibration renders disposable blocks before publication.
    // JUCE's default AudioProcessor::reset is a no-op, whereas a Faust DSP
    // instance owns oscillator/envelope/delay state. Clear every independent
    // instance so the first published block is independent of calibration.
    if (auto* program = impl_->active.load(std::memory_order_acquire))
        for (auto* instance : program->instances)
            if (instance != nullptr)
                instance->instanceClear();
}

FaustNode::PrepareTiming FaustNode::lastPrepareTiming() const
{
    return impl_->lastPrepareTiming;
}

FaustNode::ConstructionTiming FaustNode::lastConstructionTiming() const
{
    return impl_->lastConstructionTiming;
}

void FaustNode::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const int n = buffer.getNumSamples();
    if (n <= 0)
        return;

    auto* a = impl_->active.load(std::memory_order_acquire);
    if (!a) {
        // No active program (initial compile failed). Emit silence.
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.clear(ch, 0, n);
        return;
    }

    // T-617: the input channels are the VOICE-EXPANDED control declaration
    // (gate/pitch ×voices, value inputs shared, VOICES cap last) followed by
    // the source-declared audio tail. Each base param `p` feeds voice v's zone from the
    // mapped channel: chanStart[p] + v for per-voice inputs, chanStart[p]
    // (shared) otherwise. Block-granular last-sample zone delivery (unchanged
    // from T-564 — per-sample segmentation is out of scope here). Zone reads
    // happen BEFORE compute() below, so it is safe that output channels alias
    // input channels in the flattened graph buffer.
    const int np  = (int) impl_->paramNames.size();
    const int nch = buffer.getNumChannels();
    const int nVoices = a->voices;
    const int last = n - 1;

    // T-476/T-617: the VOICES cap channel (last expanded control channel) is
    // the active-voice ceiling. The channel's declared default is `voices`
    // (full polyphony) — the engine's ScriptNode feeds that default; only a
    // script/knob/mod write of >=1 narrows it. An UNDRIVEN channel reads 0
    // (raw graph-warmup / a unit-test buffer that never writes it): treat
    // anything below the declared min (1) as "undriven" = full voices, so a
    // not-yet-connected cap never collapses polyphony to a single voice.
    int activeVoices = nVoices;
    for (int p = 0; p < np; ++p) {
        const int ch = (p < (int) a->chanStart.size()) ? a->chanStart[(size_t) p] : -1;
        if (ch >= 0 && ch < nch && p < impl_->lastControlInputCount)
            impl_->lastControlInputs[(size_t) p].store(buffer.getReadPointer(ch)[last],
                                                       std::memory_order_relaxed);
    }
    if (a->voicesCapCh >= 0 && a->voicesCapCh < nch) {
        const float capRaw = buffer.getReadPointer(a->voicesCapCh)[last];
        // SF-052 robustness: a real VOICES cap is ALWAYS an integer voice count
        // (the channel default is `voices`; only a script/knob/mod write of an
        // integer narrows it). A FRACTIONAL value on this channel means the
        // control contract is STALE — a live `param{...}` per-voice-value edit
        // widened the fed buffer's per-voice rows, but this DSP node was not
        // rebuilt, so its computed voicesCapCh now lands on a per-voice VALUE
        // channel carrying e.g. a gain of 0.72. Reading that as the cap rounds
        // to 1 and collapses polyphony to a single voice (the "edit a gain in a
        // {} stack → goes mono" bug). Treat a non-integer cap as undriven →
        // full voices (the safe default), so a stale contract degrades to
        // polyphony, never to mono.
        const float capRounded = std::round(capRaw);
        const bool  capIsInteger = std::abs(capRaw - capRounded) < 1.0e-3f;
        if (capIsInteger && capRounded >= 1.0f)
            activeVoices = juce::jlimit(1, nVoices, (int) capRounded);
    }

    // PSET probe (B-339 per-voice mono): the FaustNode read-side channel
    // contract. If numControlInputs / voicesCapCh disagree with the feed (a
    // perVoiceParams mismatch), the cap is read off the wrong channel and
    // collapses polyphony to 1 voice. Throttled.
    if (CurlopDebug::on(CurlopDebug::PSET)) {
        if (impl_->psetTapBlk++ % 40 == 0)
            CDBG_RT(PSET, "faustNodeRead ncc=%d voicesCapCh=%d nch=%d capVal=%.2f nVoices=%d activeVoices=%d",
                    a->numControlInputs, a->voicesCapCh, nch,
                    (a->voicesCapCh >= 0 && a->voicesCapCh < nch)
                        ? buffer.getReadPointer(a->voicesCapCh)[last] : -1.0f,
                    nVoices, activeVoices);
    }

    const int nIn  = a->numAudioIn;
    const int nOut = a->numAudioOut;

    // Gate-retrigger fix: snapshot the control-input channels [0..ncc) BEFORE any
    // compute() writes output (output ch0/1 alias control ch0/1). The block is then
    // computed in SEGMENTS split wherever any control channel changes, so a
    // `button("gate")` zone sees the gate drop to 0 between two touching notes and
    // the envelope re-arms (FaustSynthAdapter's mechanism; a block of static
    // controls finds the first boundary at e==n and costs exactly one compute).
    const int ncc = a->numControlInputs;   // first audio-tail channel
    for (int c = 0; c < ncc; ++c) {
        float* dst = impl_->ctrlSnapshot.getWritePointer(c);
        if (c < nch) juce::FloatVectorOperations::copy(dst, buffer.getReadPointer(c), n);
        else         juce::FloatVectorOperations::clear(dst, n);
    }
    const auto ctrlAt = [&] (int ch, int s) -> float {
        return (ch >= 0 && ch < ncc) ? impl_->ctrlSnapshot.getReadPointer(ch)[s] : 0.0f;
    };
    // Smallest e > s at which any control channel changes value, else n.
    const auto nextBoundary = [&] (int s) -> int {
        int e = s + 1;
        for (; e < n; ++e) {
            bool changed = false;
            for (int c = 0; c < ncc; ++c)
                if (impl_->ctrlSnapshot.getReadPointer(c)[e]
                        != impl_->ctrlSnapshot.getReadPointer(c)[s]) { changed = true; break; }
            if (changed) break;
        }
        return e;
    };
    // Write voice v's zones from the control snapshot at sample s (segment start).
    const auto writeVoiceZonesAt = [&] (int v, int s) {
        for (int p = 0; p < np && p < (int) a->zones[(size_t) v].size(); ++p) {
            if (a->zones[(size_t) v][p] == nullptr) continue;
            const int ch = a->chanStart[(size_t) p]
                         + (a->perVoice[(size_t) p] ? v : 0);
            float value = ctrlAt(ch, s);
            if (! std::isfinite(value)) continue;
            const auto& name = impl_->paramNames[(size_t) p];
            for (const auto& schemaParam : a->schema) {
                if (schemaParam.name == name) {
                    value = juce::jlimit(schemaParam.min, schemaParam.max, value);
                    break;
                }
            }
            *a->zones[(size_t) v][p] = (FAUSTFLOAT) value;
        }
    };
    const auto snapshotFaceplateMeters = [&] {
        if (! a->faceplateMeterValues) return;
        for (size_t i = 0; i < a->faceplateMeterValueCount; ++i)
        {
            auto* zone = i < a->faceplateMeterZones.size() ? a->faceplateMeterZones[i] : nullptr;
            const float value = zone != nullptr && std::isfinite((float) *zone)
                ? (float) *zone
                : 0.0f;
            a->faceplateMeterValues[i].store(value, std::memory_order_relaxed);
        }
    };

    // The oversampled domain is deliberately local to the Faust DSP compute.
    // Graph control rows and packet delivery remain host-rate: their host
    // segment boundaries are expanded by the integer factor, never resampled.
    // Sources with cv outputs and independent-mono wrappers are not eligible
    // yet because they require a separately specified full-rate crossing.
    if (impl_->oversampling != nullptr && a->numControlOut == 0
        && impl_->automaticMultiMonoLanes == 1) {
        const auto boundaryStart = std::chrono::steady_clock::now();
        const int factor = moduleOversamplingMultiplier(impl_->oversamplingFactor);
        const int channels = impl_->oversamplingChannels;
        const int rateSamples = n * factor;
        for (int channel = 0; channel < channels; ++channel) {
            auto* destination = impl_->scratchIn.getWritePointer(channel);
            const int source = ncc + channel;
            if (channel < nIn && source < nch)
                juce::FloatVectorOperations::copy(destination,
                                                    buffer.getReadPointer(source), n);
            else
                juce::FloatVectorOperations::clear(destination, n);
        }
        auto hostInput = juce::dsp::AudioBlock<const float>(impl_->scratchIn)
            .getSubBlock(0, static_cast<std::size_t>(n));
        auto upsampled = impl_->oversampling->processSamplesUp(hostInput);
        for (int channel = 0; channel < channels; ++channel)
            juce::FloatVectorOperations::copy(impl_->scratchIn.getWritePointer(channel),
                                                upsampled.getChannelPointer(channel),
                                                rateSamples);
        for (int channel = 0; channel < channels; ++channel)
            juce::FloatVectorOperations::clear(upsampled.getChannelPointer(channel), rateSamples);

        for (int s = 0; s < n; ) {
            const int e = nextBoundary(s);
            const int segmentSamples = (e - s) * factor;
            const int segmentOffset = s * factor;
            for (int voice = 0; voice < activeVoices; ++voice) {
                writeVoiceZonesAt(voice, s);
                for (int channel = 0; channel < nIn; ++channel)
                    impl_->inPtrsSeg[(size_t) channel] =
                        impl_->scratchIn.getWritePointer(channel) + segmentOffset;
                for (int channel = 0; channel < nOut; ++channel)
                    juce::FloatVectorOperations::clear(
                        impl_->outPtrs[(size_t) channel], segmentSamples);
                a->instances[(size_t) voice]->compute(
                    segmentSamples,
                    nIn > 0 ? impl_->inPtrsSeg.data() : nullptr,
                    impl_->outPtrs.data());
                for (int channel = 0; channel < nOut; ++channel)
                    juce::FloatVectorOperations::add(
                        upsampled.getChannelPointer(channel) + segmentOffset,
                        impl_->outPtrs[(size_t) channel], segmentSamples);
            }
            s = e;
        }
        auto hostOutput = juce::dsp::AudioBlock<float>(impl_->scratchIn)
            .getSubBlock(0, static_cast<std::size_t>(n));
        impl_->oversampling->processSamplesDown(hostOutput);
        for (int channel = 0; channel < nOut && channel < nch; ++channel)
            juce::FloatVectorOperations::copy(buffer.getWritePointer(channel),
                                                impl_->scratchIn.getReadPointer(channel), n);
        snapshotFaceplateMeters();
        const auto elapsedSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - boundaryStart).count();
        const auto blockSeconds = impl_->preparedHostSampleRate > 0.0
            ? static_cast<double>(n) / impl_->preparedHostSampleRate : 0.0;
        const auto measuredLoad = blockSeconds > 0.0
            ? static_cast<float>(elapsedSeconds / blockSeconds) : 0.0f;
        const auto priorLoad = impl_->oversamplingBoundaryLoad.load(std::memory_order_relaxed);
        impl_->oversamplingBoundaryLoad.store(
            priorLoad == 0.0f ? measuredLoad : priorLoad + 0.2f * (measuredLoad - priorLoad),
            std::memory_order_relaxed);
        impl_->oversamplingBoundaryLoadMeasured.store(true, std::memory_order_release);
        return;
    }

    // T-835: an explicitly prepared independent-mono wrapper owns one Faust
    // instance per incoming lane. Controls remain shared, but every instance
    // carries separate delay/filter/history state. Copy all lane inputs before
    // clearing aliased outputs; then lane k reads and writes only channel k.
    const int automaticLanes = a->automaticMultiMonoLanes;
    if (automaticLanes > 1) {
        for (int lane = 0; lane < automaticLanes; ++lane) {
            auto* dst = impl_->scratchIn.getWritePointer(lane);
            const int src = ncc + lane;
            if (src < nch)
                juce::FloatVectorOperations::copy(
                    dst, buffer.getReadPointer(src), n);
            else
                juce::FloatVectorOperations::clear(dst, n);
        }
        for (int lane = 0; lane < automaticLanes && lane < nch; ++lane)
            buffer.clear(lane, 0, n);

        for (int s = 0; s < n; ) {
            const int e = nextBoundary(s);
            const int len = e - s;
            for (int lane = 0; lane < automaticLanes; ++lane) {
                for (int p = 0;
                     p < np
                         && p < (int) a->zones[(size_t) lane].size();
                     ++p) {
                    auto* zone = a->zones[(size_t) lane][(size_t) p];
                    if (zone == nullptr)
                        continue;
                    const int ch = a->chanStart[(size_t) p];
                    float value = ctrlAt(ch, s);
                    if (! std::isfinite(value))
                        continue;
                    const auto& name = impl_->paramNames[(size_t) p];
                    for (const auto& schemaParam : a->schema) {
                        if (schemaParam.name == name) {
                            value = juce::jlimit(
                                schemaParam.min, schemaParam.max, value);
                            break;
                        }
                    }
                    *zone = (FAUSTFLOAT) value;
                }

                FAUSTFLOAT* laneInput[1] = {
                    impl_->scratchIn.getWritePointer(lane) + s
                };
                FAUSTFLOAT* laneOutput[1] = {
                    impl_->scratchOut.getWritePointer(0)
                };
                a->instances[(size_t) lane]->compute(
                    len, laneInput, laneOutput);
                if (lane < nch)
                    juce::FloatVectorOperations::copy(
                        buffer.getWritePointer(lane) + s,
                        laneOutput[0], len);
            }
            s = e;
        }
        snapshotFaceplateMeters();
        return;
    }

    // Path-A control-input proof for every Faust JIT path, including the synth
    // fast-path below (which returns before the old tail diagnostic). Shows the
    // exact value the Faust UI zone will read for each declared control input.
    if (CurlopDebug::on(CurlopDebug::FAUST_PARAM_INPUT) && ++impl_->paramInputTapBlk >= 32) {
        impl_->paramInputTapBlk = 0;
        CDBG_RT(FAUST_PARAM_INPUT,
                "FaustParamFrame module=%s ncc=%d nch=%d nIn=%d nOut=%d dspOut=%d ctrlOut=%d voices=%d active=%d",
                getName().toRawUTF8(), ncc, nch, nIn, nOut, a->numDspOut,
                a->numControlOut, nVoices, activeVoices);
        for (int p = 0; p < np && p < (int) a->chanStart.size(); ++p) {
            const int ch = a->chanStart[(size_t) p];
            if (ch < 0 || ch >= ncc) continue;
            const float first = ctrlAt(ch, 0);
            const float lastV = ctrlAt(ch, last);
            const float peak = impl_->ctrlSnapshot.getMagnitude(ch, 0, n);
            CDBG_RT(FAUST_PARAM_INPUT,
                    "FaustParamInput module=%s p=%s ch=%d first=%.5f last=%.5f peak=%.5f zone=%d perVoice=%d",
                    getName().toRawUTF8(), impl_->paramNames[(size_t) p].c_str(),
                    ch, first, lastV, peak,
                    (a->zones.empty() || a->zones[0].size() <= (size_t) p || a->zones[0][(size_t) p] == nullptr) ? 0 : 1,
                    (p < (int) a->perVoice.size()) ? a->perVoice[(size_t) p] : 0);
        }
    }

    // T-617 synth fast-path: no audio inputs, stereo output. Compute each
    // active voice into per-voice scratch and RAW-ACCUMULATE into the buffer's
    // stereo channels (T-434: no 1/sqrt(N) normalisation). nullptr inputs are
    // safe when the program declares none.
    // F-075 / B-342: the fast path is valid ONLY for a pure stereo synth with NO
    // control outputs (its voiceScratch is only stereo, and it does no control
    // split). A synth that emits control MUST take the general path below, whose
    // outPtrs scratch is sized to the full DSP arity and which copies the cvout
    // channels after the exact audio width. Guard on numControlOut == 0 — NOT
    // numDspOut:
    // a MONO-audio synth + one cvout has numDspOut == 2 yet still emits control,
    // so the old numDspOut-only guard swallowed it (the control channel was read
    // as audio-R and never reached its trailing bus channel).
    // B-346 probe: log the fast-path condition operands (one-shot/~80 blocks).
    // Cymbal source (`process = sig <: _, _`, 7 buttons/sliders, no bargraphs)
    // ought to hit the stereo-synth fast path below — but it does NOT. Logging
    // nIn/numDspOut/numControlOut shows which operand diverges from the expected
    // (nIn=0, numDspOut=2, numControlOut=0). nOut is the audio-out after the
    // control-out split; the general path below still delivers the gate zone via
    // writeVoiceZonesAt, so silence here is only explained if compute emits 0 or
    // numAudioOut itself is 0 (outPtrs uninitialised / read as garbage).
    if (CurlopDebug::on(CurlopDebug::FAUST_RUNTIME)) {
        if (impl_->b346CondTapBlk++ % 80 == 0)
            CDBG_RT(FAUST_RUNTIME,
                    "fast-cond nIn=%d numDspOut=%d numControlOut=%d numAudioIn=%d numAudioOut=%d ncc=%d nVoices=%d activeVoices=%d",
                    nIn, a->numDspOut, a->numControlOut, a->numAudioIn, a->numAudioOut,
                    ncc, nVoices, activeVoices);
    }

    if (nIn == 0 && a->numDspOut == 2 && a->numControlOut == 0) {
        auto* outL = buffer.getWritePointer(0);
        auto* outR = buffer.getWritePointer(1);
        // T-617: write EVERY active voice's zones from the (pristine) input
        // channels BEFORE clearing the output — the output channels 0/1 alias
        // input control channels 0/1 in the in-place graph buffer, so a memset
        // first would erase the gate/pitch the first voices read. Each voice's
        // zones live in distinct DSP memory, so all voices can be written up
        // front; the per-voice compute then renders into separate scratch.
        // Control inputs are snapshotted, so clearing the output (which aliases
        // control ch0/1) is safe. Compute in segments split at control-value
        // changes: zones written from the snapshot at each segment start, so gate
        // edges land sample-accurately and a percussive envelope re-arms per step.
        std::memset(outL, 0, sizeof(float) * (size_t) n);
        std::memset(outR, 0, sizeof(float) * (size_t) n);
        const bool privateVoiceSpans = isVoiceCohortEligible()
            && impl_->voiceCohortScratch.getNumChannels() >= 2 * activeVoices;
        auto* voiceTeam = impl_->voiceCohortTeam.load(std::memory_order_acquire);
        const bool useVoiceWorkers = privateVoiceSpans && activeVoices > 1
            && voiceTeam != nullptr && voiceTeam->workerCount() > 0;
        if (useVoiceWorkers && ! voiceTeam->completed()) {
            buffer.clear();
            return;
        }
        FAUSTFLOAT* vout[2] = { impl_->voiceOut[0], impl_->voiceOut[1] };
        // B-346: resolve the gate param index once (shared by diagnostics below).
        int gateIdx = -1;
        for (size_t i = 0; i < impl_->paramNames.size(); ++i)
            if (impl_->paramNames[i] == "gate") { gateIdx = (int) i; break; }
        if (CurlopDebug::on(CurlopDebug::FAUST_RUNTIME)) {
            if (impl_->b346PreBlk++ % 50 == 0)
                CDBG_RT(FAUST_RUNTIME, "pre-loop node=%s this=%p n=%d activeVoices=%d nVoices=%d gateIdx=%d",
                        getName().toRawUTF8(), (void*) this, n, activeVoices, nVoices, gateIdx);
            if (n <= 0)
                CDBG_RT(FAUST_RUNTIME, "fastpath empty-block n=%d activeVoices=%d", n, activeVoices);
        }

        int segments = 0;
        const auto cohortDeadline = std::chrono::steady_clock::now()
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(
                    static_cast<double>(n) / impl_->preparedHostSampleRate * 0.5));
        float voicePeak = 0.0f;
        float firstGateZone = std::numeric_limits<float>::quiet_NaN();
        float firstFreqZone = std::numeric_limits<float>::quiet_NaN();
        float firstVelocityZone = std::numeric_limits<float>::quiet_NaN();
        for (int s = 0; s < n; ) {
            const int e = nextBoundary(s);
            const int len = e - s;
            ++segments;
            if (useVoiceWorkers) {
                // Delivery and voice identity are resolved before this point;
                // only independent DSP compute enters the cohort. Write every
                // voice's zones on the callback before workers start.
                for (int v = 0; v < activeVoices; ++v)
                    writeVoiceZonesAt(v, s);

                auto* callbackOutL = impl_->voiceCohortScratch.getWritePointer(0) + s;
                auto* callbackOutR = impl_->voiceCohortScratch.getWritePointer(1) + s;
                juce::FloatVectorOperations::clear(callbackOutL, len);
                juce::FloatVectorOperations::clear(callbackOutR, len);
                FAUSTFLOAT* callbackOut[2] { callbackOutL, callbackOutR };
                a->instances[0]->compute(len, nullptr, callbackOut);

                for (int v = 1; v < activeVoices; ++v) {
                    auto& job = impl_->voiceCohortJobs[static_cast<std::size_t>(v - 1)];
                    job = { a, v,
                        { impl_->voiceCohortScratch.getWritePointer(2 * v) + s,
                          impl_->voiceCohortScratch.getWritePointer(2 * v + 1) + s },
                        len };
                    juce::FloatVectorOperations::clear(job.output[0], len);
                    juce::FloatVectorOperations::clear(job.output[1], len);
                    impl_->voiceCohortWork[static_cast<std::size_t>(v - 1)] = {
                        Impl::runVoiceCohortJob, &job };
                }
                voiceTeam->dispatch(impl_->voiceCohortWork.data(), activeVoices - 1);
                while (! voiceTeam->completed()
                       && std::chrono::steady_clock::now() < cohortDeadline)
                    voiceTeam->tryRunOne();
                if (! voiceTeam->completed()) {
                    buffer.clear();
                    return;
                }
            } else for (int v = 0; v < activeVoices; ++v) {
                writeVoiceZonesAt(v, s);
                if (segments == 1 && v == 0 && CurlopDebug::on(CurlopDebug::FAUST_PARAM_INPUT)) {
                    for (int pidx = 0; pidx < np && pidx < (int) a->zones[0].size(); ++pidx) {
                        if (a->zones[0][(size_t) pidx] == nullptr) continue;
                        const auto& pn = impl_->paramNames[(size_t) pidx];
                        if (pn == "gate") firstGateZone = (float) *a->zones[0][(size_t) pidx];
                        else if (pn == "freq") firstFreqZone = (float) *a->zones[0][(size_t) pidx];
                        else if (pn == "velocity") firstVelocityZone = (float) *a->zones[0][(size_t) pidx];
                    }
                }

                if (privateVoiceSpans) {
                    vout[0] = impl_->voiceCohortScratch.getWritePointer(2 * v) + s;
                    vout[1] = impl_->voiceCohortScratch.getWritePointer(2 * v + 1) + s;
                }
                juce::FloatVectorOperations::clear(vout[0], len);
                juce::FloatVectorOperations::clear(vout[1], len);

                // B-346: sentinel-fill voiceOut[0] before compute, then read it
                // back — proves whether libfaust compute() actually wrote.
                const bool probe = (CurlopDebug::on(CurlopDebug::FAUST_RUNTIME)
                                    && v == 0 && impl_->b346ProbeBlk++ % 50 == 0);
                if (probe)
                    for (int k = 0; k < len && k < 8; ++k)
                        vout[0][k] = (FAUSTFLOAT) 9999.0f;
                a->instances[(size_t) v]->compute(len, /*in*/ nullptr, vout);
                if (probe) {
                    char zbuf[320]; int zw = 0;
                    for (size_t i = 0; i < impl_->paramNames.size() && zw < 300; ++i) {
                        const float zv = (i < a->zones[(size_t) v].size()
                                         && a->zones[(size_t) v][i])
                            ? (float) *a->zones[(size_t) v][i] : -1.0f;
                        zw += snprintf(zbuf + zw, sizeof(zbuf) - (size_t) zw,
                                       "%s%s=%g", i ? " " : "",
                                       impl_->paramNames[i].c_str(), (double) zv);
                    }
                    float probeGate = -1.0f;
                    if (gateIdx >= 0 && gateIdx < (int) a->zones[(size_t) v].size()
                        && a->zones[(size_t) v][(size_t) gateIdx] != nullptr)
                        probeGate = (float) *a->zones[(size_t) v][(size_t) gateIdx];
                    const float inCh0 = (ncc > 0) ? ctrlAt(0, 0) : -1.0f;
                    CDBG_RT(FAUST_RUNTIME,
                            "compute-probe inst=%p n=%d len=%d gate=%g inCh0=%g out[0..3]=%g %g %g %g %s",
                            (void*) a->instances[(size_t) v], n, len, probeGate, inCh0,
                            (double) vout[0][0], (double) vout[0][1],
                            (double) vout[0][2], (double) vout[0][3], zbuf);
                }
                for (int k = 0; k < len; ++k)
                    voicePeak = juce::jmax(voicePeak,
                                           juce::jmax(std::abs((float) vout[0][k]),
                                                     std::abs((float) vout[1][k])));
                if (! privateVoiceSpans) {
                    for (int k = 0; k < len; ++k) {
                        outL[s + k] += vout[0][k];
                        outR[s + k] += vout[1][k];
                    }
                }
            }
            // Reduction order is part of the polyphony contract. It stays
            // ascending by voice even once individual voice spans are rendered
            // independently, so completion order cannot alter the mix.
            for (int v = 0; privateVoiceSpans && v < activeVoices; ++v) {
                const auto* voiceL = privateVoiceSpans
                    ? impl_->voiceCohortScratch.getReadPointer(2 * v) + s
                    : impl_->voiceOut[0];
                const auto* voiceR = privateVoiceSpans
                    ? impl_->voiceCohortScratch.getReadPointer(2 * v + 1) + s
                    : impl_->voiceOut[1];
                for (int k = 0; k < len; ++k) {
                    outL[s + k] += voiceL[k];
                    outR[s + k] += voiceR[k];
                }
            }
            s = e;
        }
        if (CurlopDebug::on(CurlopDebug::FAUST_RUNTIME)) {
            const bool dense = (impl_->b346ConsecZeroBlocks < 120);
            if (dense || impl_->b346TapBlk++ % 80 == 0) {
                float gateVal = -1.0f;
                float gateChanVal = -1.0f;
                char zoneDump[256]; int wp = 0;
                wp += snprintf(zoneDump + wp, sizeof(zoneDump) - (size_t) wp, "zones[");
                for (size_t i = 0; i < impl_->paramNames.size() && wp < 240; ++i) {
                    const float zv = (i < a->zones[0].size() && a->zones[0][i])
                        ? (float) *a->zones[0][i] : -1.0f;
                    wp += snprintf(zoneDump + wp, sizeof(zoneDump) - (size_t) wp,
                                   "%s%s=%g", i ? " " : "",
                                   impl_->paramNames[i].c_str(), (double) zv);
                    if ((int) i == gateIdx) gateVal = zv;
                }
                wp += snprintf(zoneDump + wp, sizeof(zoneDump) - (size_t) wp, "]");
                if (gateIdx >= 0) {
                    const int ch = a->chanStart[(size_t) gateIdx];
                    if (ch >= 0 && ch < ncc)
                        gateChanVal = ctrlAt(ch, n - 1);
                }
                float outPeak = 0.0f;
                for (int k = 0; k < n; ++k) {
                    const float a2 = std::fabs(outL[k]);
                    if (a2 > outPeak) outPeak = a2;
                }
                float voutPeak = -1.0f;
                if (impl_->voiceOut[0] != nullptr) {
                    voutPeak = 0.0f;
                    for (int k = 0; k < n; ++k) {
                        const float a2 = std::fabs(impl_->voiceOut[0][k]);
                        if (a2 > voutPeak) voutPeak = a2;
                    }
                }
                CDBG_RT(FAUST_RUNTIME,
                        "fastpath node=%s this=%p ncc=%d nVoices=%d activeVoices=%d gateIdx=%d gateZone=%g gateChan=%g voutNil=%d voutPeak=%g outPeak=%g %s",
                        getName().toRawUTF8(), (void*) this,
                        ncc, nVoices, activeVoices, gateIdx, gateVal, gateChanVal,
                        (int) (impl_->voiceOut[0] == nullptr), voutPeak, outPeak,
                        zoneDump);
                if (outPeak > 0.0f) impl_->b346ConsecZeroBlocks = 0;
                else ++impl_->b346ConsecZeroBlocks;
            }
        }
        if (CurlopDebug::on(CurlopDebug::FAUST_PARAM_INPUT) && impl_->paramInputTapBlk == 0) {
            CDBG_RT(FAUST_PARAM_INPUT,
                    "FaustAudioFrame module=%s path=synth segments=%d active=%d gateZone=%.5f freqZone=%.5f velocityZone=%.5f voicePeak=%.5f outAbsPeakL=%.5f outAbsPeakR=%.5f out0=%.5f out1=%.5f",
                    getName().toRawUTF8(), segments, activeVoices, firstGateZone,
                    firstFreqZone, firstVelocityZone, voicePeak,
                    buffer.getMagnitude(0, 0, n), buffer.getMagnitude(1, 0, n),
                    outL[0], outR[0]);
        }
        snapshotFaceplateMeters();
        return;
    }

    // T-572/T-617 arity-generic effect/synth path. The audio-input
    // channels [ncc .. ncc+nIn-1] (ncc = expanded control count) alias the
    // output channels in the flattened buffer, so copy them into private
    // scratch BEFORE compute() writes any output. Each active voice computes
    // the same audio tail into outPtrs scratch and raw-accumulates onto every
    // source-declared audio output channel.
    // (ncc declared above with the control snapshot.) Copy the audio-input
    // channels [ncc..ncc+nIn-1] into private scratch BEFORE compute() writes any
    // output — for a control-less effect they alias output ch0/1.
    for (int c = 0; c < nIn; ++c) {
        float* dst = impl_->inPtrs[(size_t) c];
        const int src = ncc + c;
        if (src < nch)
            juce::FloatVectorOperations::copy(dst, buffer.getReadPointer(src), n);
        else
            juce::FloatVectorOperations::clear(dst, n);
    }

    for (int channel = 0; channel < nOut && channel < nch; ++channel)
        buffer.clear(channel, 0, n);
    // Gate-retrigger fix: segmented compute (split at control-value changes) so a
    // mono OR poly module's gate zone sees the edge and re-arms. Audio inputs are
    // offset to the segment start; control outputs land after the declared audio
    // output width.
    for (int s = 0; s < n; ) {
        const int e = nextBoundary(s);
        const int len = e - s;
        for (int v = 0; v < activeVoices; ++v) {
            writeVoiceZonesAt(v, s);
            for (size_t c = 0; c < impl_->inPtrs.size(); ++c)
                impl_->inPtrsSeg[c] = impl_->inPtrs[c] + s;
            a->instances[(size_t) v]->compute(len,
                                 nIn > 0 ? impl_->inPtrsSeg.data() : nullptr,
                                 impl_->outPtrs.data());
            for (int channel = 0; channel < nOut && channel < nch; ++channel) {
                const float* voiceOutput =
                    impl_->outPtrs[(size_t) channel];
                float* moduleOutput =
                    buffer.getWritePointer(channel) + s;
                for (int sample = 0; sample < len; ++sample)
                    moduleOutput[sample] += voiceOutput[sample];
            }
            // F-075: control outputs from voice 0 only — copy this segment onto
            // module-bus channels [nOut .. nOut+numControlOut-1] (DSP src = outPtrs
            // [numAudioOut + k], audio channels first).
            if (v == 0) {
                for (int k = 0; k < a->numControlOut; ++k) {
                    const int outCh = nOut + k;
                    const int srcCh = a->numAudioOut + k;
                    if (outCh < nch && srcCh < (int) impl_->outPtrs.size())
                        juce::FloatVectorOperations::copy(
                            buffer.getWritePointer(outCh) + s,
                            impl_->outPtrs[(size_t) srcCh], len);
                }
            }
        }
        s = e;
    }
    snapshotFaceplateMeters();

    // CONTROL_TAP (T-665): the SOURCE side — each [curlop:cvout] channel's value/
    // peak as it lands on the module bus (channel nOut+k), now that the whole block is
    // assembled. Throttled ~3Hz per instance (off by default).
    if (a->numControlOut > 0 && CurlopDebug::on(CurlopDebug::CONTROL_TAP)
        && ++impl_->ctrlTapBlk >= 32) {
        impl_->ctrlTapBlk = 0;
        for (int k = 0; k < a->numControlOut; ++k) {
            const int outCh = nOut + k;
            const float peak = (outCh < nch) ? buffer.getMagnitude(outCh, 0, n) : 0.0f;
            const float last = (outCh < nch) ? buffer.getSample(outCh, n - 1) : 0.0f;
            const char* pn = (k < (int) a->controlOutputs.size())
                                 ? a->controlOutputs[(size_t) k].name.c_str() : "?";
            CDBG_RT(CONTROL_TAP, "faust-cvout module=%s ch=%d name=%s last=%.5f peak=%.5f",
                    getName().toRawUTF8(), outCh, pn, last, peak);
        }
    }

}

void FaustNode::setSource(const std::string& source)
{
    {
        std::lock_guard<std::mutex> lk(impl_->inboxMu);
        impl_->pendingSource = source;
        impl_->pendingValidatedSchema.clear();
        impl_->hasPendingValidatedSchema = false;
        impl_->hasPending = true;
    }
    impl_->start();
    impl_->wake();
}

void FaustNode::setSource(const std::string& source,
                          const std::vector<ParamSchemaEntry>& validatedSchema)
{
    {
        std::lock_guard<std::mutex> lk(impl_->inboxMu);
        impl_->pendingSource = source;
        impl_->pendingValidatedSchema = validatedSchema;
        impl_->hasPendingValidatedSchema = true;
        impl_->hasPending = true;
    }
    impl_->start();
    impl_->wake();
}

std::string FaustNode::lastCompileError() const
{
    std::lock_guard<std::mutex> lk(impl_->errorMu);
    return impl_->lastError;
}

bool FaustNode::isCompilePending() const
{
    std::lock_guard<std::mutex> lk(impl_->inboxMu);
    return impl_->hasPending
        || impl_->compileInFlight.load(std::memory_order_acquire);
}

std::vector<ParamSchemaEntry> FaustNode::currentSchema() const
{
    // Prefer the installed-schema mirror (which is what the parent sub-graph
    // was built around) over the active program's schema. They are equal in
    // steady state; they only differ in the schema-change path during the
    // window between the worker producing a new program and the host posting
    // a rebuild request. Callers want "what is currently driving the faceplate
    // and ControlCore wiring" — that's installedSchema.
    std::lock_guard<std::mutex> lk(impl_->installedSchemaMu);
    return impl_->installedSchema;
}

std::vector<std::pair<std::string, float>> FaustNode::faceplateMeterValues() const
{
    std::vector<std::pair<std::string, float>> out;
    auto* a = impl_->active.load(std::memory_order_acquire);
    if (a == nullptr) return out;

    const auto count = std::min(a->faceplateMeters.size(), a->faceplateMeterValueCount);
    out.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        const float value = a->faceplateMeterValues
            ? a->faceplateMeterValues[i].load(std::memory_order_relaxed)
            : 0.0f;
        out.push_back({ a->faceplateMeters[i].id, value });
    }
    return out;
}

float FaustNode::lastControlInputValue(const std::string& paramName) const
{
    for (size_t i = 0; i < impl_->paramNames.size(); ++i)
        if (impl_->paramNames[i] == paramName)
            return i < (size_t) impl_->lastControlInputCount
                ? impl_->lastControlInputs[i].load(std::memory_order_relaxed)
                : std::numeric_limits<float>::quiet_NaN();
    return std::numeric_limits<float>::quiet_NaN();
}

ModuleOversamplingFactor FaustNode::oversamplingFactor() const noexcept
{
    return impl_->oversamplingFactor;
}

bool FaustNode::isOversamplingEligible() const noexcept
{
    auto* a = impl_->active.load(std::memory_order_acquire);
    return a != nullptr && a->numControlOut == 0
        && impl_->automaticMultiMonoLanes == 1;
}

double FaustNode::effectiveSampleRate(double hostSampleRate) const noexcept
{
    return hostSampleRate * (isOversamplingEligible()
        ? moduleOversamplingMultiplier(impl_->oversamplingFactor)
        : 1);
}

int FaustNode::oversamplingLatencySamples() const noexcept
{
    return impl_->oversampling != nullptr
        ? static_cast<int>(impl_->oversampling->getLatencyInSamples()) : 0;
}

float FaustNode::oversamplingBoundaryLoad() const noexcept
{
    return impl_->oversamplingBoundaryLoad.load(std::memory_order_relaxed);
}

bool FaustNode::hasOversamplingBoundaryLoadMeasurement() const noexcept
{
    return impl_->oversamplingBoundaryLoadMeasured.load(std::memory_order_acquire);
}

bool FaustNode::isVoiceCohortEligible() const noexcept
{
    auto* a = impl_->active.load(std::memory_order_acquire);
    return a != nullptr
        && a->processingCapabilities.independentVoiceCohorts
        && a->voices > 1
        && a->numAudioIn == 0
        && a->numAudioOut == 2
        && a->numControlOut == 0
        && impl_->automaticMultiMonoLanes == 1
        && impl_->oversamplingFactor == ModuleOversamplingFactor::X1;
}

void FaustNode::setVoiceCohortWorkerTeam(
    transport::RealtimeWorkerTeam* team) noexcept
{
    impl_->voiceCohortTeam.store(team, std::memory_order_release);
}

std::vector<ParamSchemaEntry> FaustNode::compileSchemaOnly(const std::string& source,
                                                               std::string& errOut,
                                                               FaustRuntime* runtime,
                                                               bool liveAuthoring)
{
    // T-405: the factory comes from (and STAYS in) the runtime cache — a
    // schema probe is almost always followed by a node build of the same
    // source, which now cache-hits instead of paying a second ~50ms compile.
    // Only the throwaway instance is created and freed here. The sample rate
    // is irrelevant for the schema (slider metadata is set at
    // buildUserInterface time, not at instance->init); 48000 is a stable
    // default.
    errOut.clear();
    auto& rt = runtime ? *runtime : FaustRuntime::instance();
    auto factory = liveAuthoring
        ? rt.acquireForLiveAuthoring ("schema-probe", source, errOut)
        : rt.acquire ("schema-probe", source, errOut);
    if (!factory) return {};

    // B-205: instance create + UI walk under the same global libfaust lock
    // as the compile paths — a backfill probe here must not race the
    // worker-thread setSource compiles.
    std::lock_guard<std::mutex> faustLock(faustGlobalCompileMutex());
    auto* instance = factory->raw()->createDSPInstance();
    if (!instance) {
        errOut = "createDSPInstance: null";
        return {};
    }
    instance->init(48000);

    ParamCaptureUI ui;
    instance->buildUserInterface(&ui);
    auto schema = ui.synthesiseSchema();
    applyFaustControlSourceMetadata (schema, source);

    delete instance;
    return schema;
}

std::vector<ControlOutputDecl> FaustNode::probeControlOutputs(const std::string& source,
                                                             FaustRuntime* runtime)
{
    // F-075: compile-and-walk just to count/name the [curlop:cvout] markers.
    // Mirrors compileSchemaOnly — the factory lands in the runtime cache, so
    // the node's own compileSync that follows cache-hits (one compile total).
    std::string errOut;
    auto& rt = runtime ? *runtime : FaustRuntime::instance();
    auto factory = rt.acquire("cvout-probe", source, errOut);
    if (!factory) return {};

    std::lock_guard<std::mutex> faustLock(faustGlobalCompileMutex());
    auto* instance = factory->raw()->createDSPInstance();
    if (!instance) return {};
    instance->init(48000);

    ParamCaptureUI ui;
    instance->buildUserInterface(&ui);
    auto outs = ui.synthesiseControlOutputs();

    delete instance;
    return outs;
}

std::pair<int, int> FaustNode::probeAudioIo(const std::string& source,
                                            std::string& errOut,
                                            FaustRuntime* runtime)
{
    errOut.clear();
    auto& rt = runtime ? *runtime : FaustRuntime::instance();
    auto factory = rt.acquire("audio-io-probe", source, errOut);
    if (! factory)
        return {};

    std::lock_guard<std::mutex> faustLock (faustGlobalCompileMutex());
    auto* instance = factory->raw()->createDSPInstance();
    if (instance == nullptr)
    {
        errOut = "createDSPInstance: null";
        return {};
    }
    instance->init (48000);
    ParamCaptureUI ui;
    instance->buildUserInterface (&ui);
    const auto controlOutputs = ui.synthesiseControlOutputs();
    const int inputs = juce::jmax (0, instance->getNumInputs());
    const int outputs = juce::jmax (
        0, instance->getNumOutputs() - static_cast<int> (controlOutputs.size())
            - ui.meterOutputCount());
    delete instance;
    return { inputs, outputs };
}

std::vector<FaceplateElement> FaustNode::compileFaceplateElementsOnly(
    const std::string& source,
    FaustRuntime* runtime)
{
    (void) runtime;
    return faustGuiElementsFromSource(source);
}

std::vector<SocketPresentation> FaustNode::compileSocketPresentationsOnly(
    const std::string& source,
    FaustRuntime* runtime)
{
    (void) runtime;
    return faustSocketPresentationsFromSource(source);
}

FaceplateSchema FaustNode::compileFaceplateSchemaOnly(const std::string& source,
                                                      std::string& errOut,
                                                      FaustRuntime* runtime)
{
    errOut.clear();
    auto& rt = runtime ? *runtime : FaustRuntime::instance();
    auto factory = rt.acquire("faceplate-schema-probe", source, errOut);
    if (!factory) return {};

    std::lock_guard<std::mutex> faustLock(faustGlobalCompileMutex());
    auto* instance = factory->raw()->createDSPInstance();
    if (!instance) {
        errOut = "createDSPInstance: null";
        return {};
    }
    instance->init(48000);

    ParamCaptureUI ui;
    instance->buildUserInterface(&ui);
    auto schema = ui.synthesiseFaceplateSchema();
    mergeFaustGroupSourceMetadata (schema, source);
    schema.elements = faustGuiElementsFromSource(source);

    delete instance;
    return schema;
}

void FaustNode::setSchemaChangedCallback(SchemaChangedFn cb)
{
    // The worker reads schemaChangedFn without a lock under the contract that
    // this setter is called once, before any setSource() — typically right
    // after construction, in the same code path that builds the sub-graph
    // around this node. The worker thread is lazy-started by setSource(), so a
    // setter call between construction and the first setSource is race-free.
    impl_->schemaChangedFn = std::move(cb);
}

} // namespace curlop
