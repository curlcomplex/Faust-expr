#include "clip-launcher/state/ClipStateContainer.h"
#include "graph/state/GraphStateLimits.h"
#include "shell/CurlopDebug.h"
#include "shell/PayloadNumber.h"
#include "shell/ProjectRuntimeAdmission.h"
#include "graph/engine/registry/ModuleRegistryGen.h"   // T-100: factory lineageUuid lookup for identity-first load
#include "modules/library/ModuleVersionStore.h"        // F-076: MergeToLibrary version materialise
#include "modules/visual/VisualShaderModule.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace curlop {

static bool readOptionalFloatProperty(const juce::var& object,
                                      const char* propertyName,
                                      float fallback,
                                      float& out)
{
    out = fallback;
    if (! object.hasProperty(propertyName))
        return true;
    return curlop::payload::readFloatProperty(object, propertyName, out);
}

static bool readOptionalIntProperty(const juce::var& object,
                                    const char* propertyName,
                                    int fallback,
                                    int& out)
{
    out = fallback;
    if (! object.hasProperty(propertyName))
        return true;
    return curlop::payload::readIntProperty(object, propertyName, out);
}

static bool readOptionalBoolProperty(const juce::var& object,
                                     const char* propertyName,
                                     bool fallback,
                                     bool& out)
{
    out = fallback;
    if (! object.hasProperty(propertyName))
        return true;

    const auto value = object.getProperty(propertyName, juce::var());
    if (! value.isBool())
        return false;

    out = static_cast<bool>(value);
    return true;
}

static bool readRequiredStringProperty(const juce::var& object,
                                       const char* propertyName,
                                       std::string& out)
{
    if (! object.hasProperty(propertyName))
        return false;

    const auto value = object.getProperty(propertyName, juce::var());
    if (! value.isString())
        return false;

    out = value.toString().toStdString();
    return ! out.empty();
}

static bool readOptionalStringProperty(const juce::var& object,
                                       const char* propertyName,
                                       std::string& out)
{
    out.clear();
    if (! object.hasProperty(propertyName))
        return true;

    const auto value = object.getProperty(propertyName, juce::var());
    if (! value.isString())
        return false;

    out = value.toString().toStdString();
    return true;
}

static bool readOptionalJuceStringProperty(const juce::var& object,
                                           const char* propertyName,
                                           const juce::String& fallback,
                                           juce::String& out)
{
    out = fallback;
    if (! object.hasProperty(propertyName))
        return true;

    const auto value = object.getProperty(propertyName, juce::var());
    if (! value.isString())
        return false;

    out = value.toString();
    return true;
}

static bool readOptionalFontSizeProperty(const juce::var& object, float fallback, float& out)
{
    out = fallback;
    if (object.hasProperty("fontSize"))
        return curlop::payload::readFloatProperty(object, "fontSize", out);
    if (object.hasProperty("labelsize"))
        return curlop::payload::readFloatProperty(object, "labelsize", out);
    return true;
}

static const char* signalRateToString(transport::SignalRate rate)
{
    switch (rate) {
        case transport::SignalRate::Audio: return "audio";
        case transport::SignalRate::FullRateControl: return "full_rate_control";
        case transport::SignalRate::BlockControl: return "block_control";
        case transport::SignalRate::Event: return "event";
        case transport::SignalRate::Midi: return "midi";
    }
    return "";
}

static bool signalRateFromVar(const juce::var& value, transport::SignalRate& out)
{
    if (! value.isString()) return false;
    const auto text = value.toString();
    if (text == "audio") out = transport::SignalRate::Audio;
    else if (text == "full_rate_control") out = transport::SignalRate::FullRateControl;
    else if (text == "block_control") out = transport::SignalRate::BlockControl;
    else if (text == "event") out = transport::SignalRate::Event;
    else if (text == "midi") out = transport::SignalRate::Midi;
    else return false;
    return true;
}

juce::var signalDescriptorToVar(const transport::SignalDescriptor& descriptor)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("schemaVersion", static_cast<int>(descriptor.schemaVersion()));
    object->setProperty("width", static_cast<int64_t>(descriptor.width()));
    object->setProperty("semanticRole", juce::String(descriptor.semanticRole()));
    object->setProperty("rate", signalRateToString(descriptor.rate()));

    auto* capabilities = new juce::DynamicObject();
    capabilities->setProperty("packet", descriptor.capabilities().packet);
    capabilities->setProperty("lifecycle", descriptor.capabilities().lifecycle);
    capabilities->setProperty("identity", descriptor.capabilities().identity);
    object->setProperty("capabilities", juce::var(capabilities));

    juce::Array<juce::var> channels;
    for (const auto& channel : descriptor.channels()) {
        auto* row = new juce::DynamicObject();
        row->setProperty("stableId", juce::String(std::to_string(channel.stableId)));
        row->setProperty("name", juce::String(channel.name));
        row->setProperty("role", juce::String(channel.role));
        channels.add(juce::var(row));
    }
    object->setProperty("channels", juce::var(channels));
    if (descriptor.layout())
        object->setProperty("layout", juce::String(*descriptor.layout()));

    juce::Array<juce::var> groups;
    for (const auto& group : descriptor.groups()) {
        auto* row = new juce::DynamicObject();
        row->setProperty("id", juce::String(group.id));
        row->setProperty("name", juce::String(group.name));
        juce::Array<juce::var> ids;
        for (auto id : group.channelIds)
            ids.add(juce::String(std::to_string(id)));
        row->setProperty("channelIds", juce::var(ids));
        groups.add(juce::var(row));
    }
    object->setProperty("groups", juce::var(groups));

    if (descriptor.hoa()) {
        auto* hoa = new juce::DynamicObject();
        hoa->setProperty("order", static_cast<int>(descriptor.hoa()->order));
        hoa->setProperty("dimension",
                         descriptor.hoa()->dimension == transport::HoaDimension::Planar2D
                             ? "2d" : "3d");
        hoa->setProperty("ordering", juce::String(descriptor.hoa()->ordering));
        hoa->setProperty("normalization", juce::String(descriptor.hoa()->normalization));
        object->setProperty("hoa", juce::var(hoa));
    }
    return juce::var(object);
}

static bool readSignalChannelId(const juce::var& value,
                                transport::SignalChannelId& out)
{
    std::string text;
    if (value.isString())
        text = value.toString().toStdString();
    else if (value.isInt() || value.isInt64())
        text = value.toString().toStdString();
    else
        return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool readPersistedSignalWidth(
    const juce::var& value,
    transport::SignalWidth& out)
{
    juce::int64 parsed = 0;
    if (! payload::readInt64(value, parsed)
        || parsed < 0
        || static_cast<std::uint64_t>(parsed)
            > std::numeric_limits<transport::SignalWidth>::max())
        return false;
    out = static_cast<transport::SignalWidth>(parsed);
    return true;
}

bool signalDescriptorFromVar(const juce::var& value,
                             transport::SignalDescriptor& out)
{
    if (! value.isObject()) return false;
    transport::SignalDescriptorDefinition definition;
    int schemaVersion = 0;
    if (! curlop::payload::readIntProperty(value, "schemaVersion", schemaVersion)
        || schemaVersion < 0)
        return false;
    definition.schemaVersion = static_cast<std::uint32_t>(schemaVersion);
    if (! readRequiredStringProperty(value, "semanticRole", definition.semanticRole)
        || ! signalRateFromVar(value.getProperty("rate", {}), definition.rate))
        return false;

    const auto capabilities = value.getProperty("capabilities", {});
    if (! capabilities.isObject()
        || ! readOptionalBoolProperty(capabilities, "packet", false,
                                     definition.capabilities.packet)
        || ! readOptionalBoolProperty(capabilities, "lifecycle", false,
                                     definition.capabilities.lifecycle)
        || ! readOptionalBoolProperty(capabilities, "identity", false,
                                     definition.capabilities.identity))
        return false;

    const auto channels = value.getProperty("channels", {});
    if (! channels.isArray()) return false;
    for (const auto& channelValue : *channels.getArray()) {
        if (! channelValue.isObject()) return false;
        transport::SignalChannel channel;
        if (! readSignalChannelId(channelValue.getProperty("stableId", {}), channel.stableId)
            || ! readRequiredStringProperty(channelValue, "name", channel.name)
            || ! readRequiredStringProperty(channelValue, "role", channel.role))
            return false;
        definition.channels.push_back(std::move(channel));
    }
    if (value.hasProperty("width")) {
        transport::SignalWidth width = 0;
        if (! readPersistedSignalWidth(value.getProperty("width", {}), width)
            || static_cast<std::uint64_t>(width)
                != static_cast<std::uint64_t>(definition.channels.size()))
            return false;
    }

    if (value.hasProperty("layout")) {
        std::string layout;
        if (! readOptionalStringProperty(value, "layout", layout)) return false;
        definition.layout = std::move(layout);
    }
    const auto groups = value.getProperty("groups", {});
    if (! groups.isVoid()) {
        if (! groups.isArray()) return false;
        for (const auto& groupValue : *groups.getArray()) {
            if (! groupValue.isObject()) return false;
            transport::SignalGroup group;
            if (! readRequiredStringProperty(groupValue, "id", group.id)
                || ! readRequiredStringProperty(groupValue, "name", group.name))
                return false;
            const auto channelIds = groupValue.getProperty("channelIds", {});
            if (! channelIds.isArray()) return false;
            for (const auto& idValue : *channelIds.getArray()) {
                transport::SignalChannelId id = 0;
                if (! readSignalChannelId(idValue, id)) return false;
                group.channelIds.push_back(id);
            }
            definition.groups.push_back(std::move(group));
        }
    }
    const auto hoaValue = value.getProperty("hoa", {});
    if (! hoaValue.isVoid()) {
        if (! hoaValue.isObject()) return false;
        transport::HoaMetadata hoa;
        int order = 0;
        std::string dimension;
        if (! curlop::payload::readIntProperty(hoaValue, "order", order) || order < 0
            || ! readRequiredStringProperty(hoaValue, "dimension", dimension)
            || ! readRequiredStringProperty(hoaValue, "ordering", hoa.ordering)
            || ! readRequiredStringProperty(hoaValue, "normalization", hoa.normalization))
            return false;
        hoa.order = static_cast<std::uint32_t>(order);
        if (dimension == "2d") hoa.dimension = transport::HoaDimension::Planar2D;
        else if (dimension == "3d") hoa.dimension = transport::HoaDimension::Periphonic3D;
        else return false;
        definition.hoa = std::move(hoa);
    }

    auto result = transport::SignalDescriptor::create(std::move(definition));
    if (! result.ok()) return false;
    out = std::move(*result.value);
    return true;
}

juce::var signalPortDeclarationsToVar(
    const std::vector<SignalPortDeclaration>& ports)
{
    juce::Array<juce::var> result;
    for (const auto& port : ports) {
        auto* row = new juce::DynamicObject();
        row->setProperty("name", juce::String(port.name));
        row->setProperty("descriptor", signalDescriptorToVar(port.descriptor));
        result.add(juce::var(row));
    }
    return juce::var(result);
}

static std::vector<SignalPortDeclaration> legacySignalInputs(const ModuleEntry& module)
{
    std::vector<SignalPortDeclaration> result;
    for (const auto& name : module.audioInputs)
        result.push_back({ name, transport::SignalDescriptor::legacyStereo() });
    for (const auto& name : module.controlInputs)
        result.push_back({ name, transport::SignalDescriptor::legacyScalar() });
    for (const auto& name : module.exposedParamInputs)
        if (std::none_of(result.begin(), result.end(),
                         [&name](const auto& port) { return port.name == name; }))
            result.push_back({ name, transport::SignalDescriptor::legacyScalar() });
    return result;
}

static std::vector<SignalPortDeclaration> legacySignalOutputs(const ModuleEntry& module)
{
    std::vector<SignalPortDeclaration> result;
    for (const auto& name : module.audioOutputs)
        result.push_back({ name, transport::SignalDescriptor::legacyStereo() });
    for (const auto& name : module.controlOutputs)
        result.push_back({ name, transport::SignalDescriptor::legacyScalar() });
    return result;
}

static const transport::SignalDescriptor* findPortDescriptor(
    const std::vector<SignalPortDeclaration>& ports,
    const std::string& name)
{
    const auto it = std::find_if(
        ports.begin(), ports.end(),
        [&name](const auto& port) {
            return juce::String(port.name).equalsIgnoreCase(
                juce::String(name));
        });
    return it == ports.end() ? nullptr : &it->descriptor;
}

static transport::SignalDescriptor migrateLegacyEdgeDescriptor(
    const EdgeEntry& edge,
    const std::vector<ModuleEntry>& modules)
{
    const auto source = std::find_if(modules.begin(), modules.end(),
                                     [&edge](const auto& module) {
                                         return module.index == edge.srcIndex;
                                     });
    if (source != modules.end()) {
        const auto ports = source->signalOutputs.empty()
            ? legacySignalOutputs(*source) : source->signalOutputs;
        if (const auto* descriptor = findPortDescriptor(ports, edge.srcPort))
            return *descriptor;
        if (edge.srcPort.empty() && ! ports.empty())
            return ports.front().descriptor;
    }
    // Named legacy ports were control lanes; the empty primary path was the
    // historical stereo graph bus.
    return edge.srcPort.empty() && edge.tgtPort.empty()
        ? transport::SignalDescriptor::legacyStereo()
        : transport::SignalDescriptor::legacyScalar();
}

bool signalPortDeclarationsFromVar(
    const juce::var& value,
    std::vector<SignalPortDeclaration>& out)
{
    if (! value.isArray()) return false;
    std::vector<SignalPortDeclaration> parsed;
    for (const auto& row : *value.getArray()) {
        if (! row.isObject()) return false;
        SignalPortDeclaration port;
        if (! readRequiredStringProperty(row, "name", port.name)
            || ! signalDescriptorFromVar(row.getProperty("descriptor", {}), port.descriptor))
            return false;
        parsed.push_back(std::move(port));
    }
    out = std::move(parsed);
    return true;
}

// ── Compile-output reset (Cut 0.D-4) ────────────────────────────────────────
// Called wherever a clip's script is created or mutated. The embed compile
// path (CurlopProcessor::compileClipAndEmit) repopulates bytecode +
// diagnosticsJson afterwards. Until then, these fields must read as
// "nothing compiled yet" — empty bytecode, "[]" diagnostics, no error.
static void resetCompileOutputs(ClipData& c)
{
    c.bytecode.clear();
    c.diagnosticsJson     = "[]";
    c.compileError.clear();
    c.blockedModules.clear();
    c.compileMetadataJson = "null";
}

// ── Bulk replace ────────────────────────────────────────────────────────────

void ClipStateContainer::replace(std::vector<ClipData>&& clips,
                                 int viewedIdx,
                                 juce::String projectName,
                                 int nextClipId)
{
    juce::ScopedLock lk(lock_);
    clips_ = std::move(clips);
    for (auto& c : clips_) resetCompileOutputs(c);
    viewedClipIndex_    = clips_.empty()
        ? 0
        : juce::jlimit(0, (int)clips_.size() - 1, viewedIdx);
    projectName_        = projectName.isNotEmpty() ? projectName : juce::String("Untitled Project");
    nextClipId_         = juce::jmax(nextClipId, 1);
    previewClipId_      = -1;
}

// ── Authority mutators ──────────────────────────────────────────────────────

int ClipStateContainer::addClip(juce::String name)
{
    juce::ScopedLock lk(lock_);
    if (nextClipId_ == std::numeric_limits<int>::max())
        return -1;
    int newId = nextClipId_++;
    ClipData c;
    c.id     = newId;
    c.name   = std::move(name);
    resetCompileOutputs(c);
    // T-578: keep real clips contiguous before any ephemeral preview clip, so
    // viewedClipIndex_ / getClipIdAtIndex address a stable real-clip prefix.
    auto at = std::find_if(clips_.begin(), clips_.end(),
                           [](const ClipData& x) { return x.ephemeral; });
    auto it = clips_.insert(at, std::move(c));
    CDBG(PERSISTENCE, "clip added id=%d name=%s totalClips=%zu",
         newId, it->name.toRawUTF8(), clips_.size());
    return newId;
}

void ClipStateContainer::removeClip(int id)
{
    juce::ScopedLock lk(lock_);
    auto it = std::find_if(clips_.begin(), clips_.end(),
                           [id](const ClipData& c) { return ! c.ephemeral && c.id == id; });
    if (it == clips_.end()) return;
    int removedIdx = (int)std::distance(clips_.begin(), it);
    clips_.erase(it);
    const int realClipCount = (int) std::count_if (
        clips_.begin(), clips_.end(), [] (const ClipData& clip) { return ! clip.ephemeral; });
    if (realClipCount == 0) {
        viewedClipIndex_ = 0;
    } else if (viewedClipIndex_ >= realClipCount) {
        viewedClipIndex_ = realClipCount - 1;
    } else if (viewedClipIndex_ > removedIdx) {
        --viewedClipIndex_;
    }
}

bool ClipStateContainer::renameClip(int id, juce::String name)
{
    juce::ScopedLock lk(lock_);
    for (auto& c : clips_)
        if (! c.ephemeral && c.id == id) { c.name = std::move(name); return true; }
    return false;
}

// F-076 / ADR adr-clips ¶21: per-clip presentation colour (hex; "" clears).
void ClipStateContainer::setClipColour(int id, juce::String colour)
{
    juce::ScopedLock lk(lock_);
    for (auto& c : clips_) if (c.id == id) { c.colour = std::move(colour); return; }
}

int ClipStateContainer::duplicateClip(int sourceId)
{
    juce::ScopedLock lk(lock_);
    const int previouslyViewedId =
        (viewedClipIndex_ >= 0 && viewedClipIndex_ < (int) clips_.size()
         && ! clips_[(size_t) viewedClipIndex_].ephemeral)
            ? clips_[(size_t) viewedClipIndex_].id
            : -1;
    auto it = std::find_if(clips_.begin(), clips_.end(),
                           [sourceId](const ClipData& c) {
                               return ! c.ephemeral && c.id == sourceId;
                           });
    if (it == clips_.end() || nextClipId_ == std::numeric_limits<int>::max())
        return -1;
    int newId = nextClipId_++;
    ClipData copy;
    copy.id     = newId;
    copy.name   = it->name + " copy";
    copy.colour = it->colour;
    resetCompileOutputs(copy);
    auto insertPos = it + 1;
    clips_.insert(insertPos, std::move(copy));
    if (previouslyViewedId >= 0) {
        const auto viewedIt = std::find_if(
            clips_.begin(), clips_.end(),
            [previouslyViewedId](const ClipData& clip) {
                return ! clip.ephemeral && clip.id == previouslyViewedId;
            });
        if (viewedIt != clips_.end())
            viewedClipIndex_ = (int) std::distance(clips_.begin(), viewedIt);
    }
    return newId;
}

void ClipStateContainer::reorderClips(const std::vector<int>& clipIdOrder)
{
    juce::ScopedLock lk(lock_);
    const size_t realClipCount = (size_t) std::count_if (
        clips_.begin(), clips_.end(), [] (const ClipData& clip) { return ! clip.ephemeral; });
    if (clipIdOrder.size() != realClipCount) return;

    std::vector<size_t> sourceIndexes;
    sourceIndexes.reserve(realClipCount);
    for (int id : clipIdOrder)
    {
        const auto it = std::find_if (
            clips_.begin(), clips_.end(),
            [id] (const ClipData& clip) { return ! clip.ephemeral && clip.id == id; });
        if (it == clips_.end()) return;
        const size_t sourceIndex = (size_t) std::distance (clips_.begin(), it);
        if (std::find (sourceIndexes.begin(), sourceIndexes.end(), sourceIndex)
            != sourceIndexes.end())
            return;
        sourceIndexes.push_back(sourceIndex);
    }

    const int prevViewedId =
        (viewedClipIndex_ >= 0 && viewedClipIndex_ < (int) clips_.size()
         && ! clips_[(size_t) viewedClipIndex_].ephemeral)
            ? clips_[(size_t) viewedClipIndex_].id
            : -1;
    std::vector<ClipData> reordered;
    reordered.reserve(clips_.size());
    for (size_t sourceIndex : sourceIndexes)
        reordered.push_back(std::move(clips_[sourceIndex]));
    for (auto& clip : clips_)
        if (clip.ephemeral) reordered.push_back(std::move(clip));
    clips_ = std::move(reordered);
    viewedClipIndex_ = 0;
    for (size_t i = 0; i < realClipCount; ++i)
        if (clips_[i].id == prevViewedId) { viewedClipIndex_ = (int) i; break; }
}

// F-076 / T-578 — audition scratch clip. Allocated once via the normal id
// sequence (so it never collides), flagged ephemeral, kept LAST. It is in
// clips_ so the play path (compileClipAndEmit + USER_PLAY_CLIP guard) can target
// it, but toClipsStateVar / toJson skip it — invisible in the launcher, never
// persisted. Idempotent: a second call returns the live preview id.
int ClipStateContainer::ensurePreviewClip()
{
    juce::ScopedLock lk(lock_);
    if (previewClipId_ > 0)
        for (const auto& c : clips_)
            if (c.id == previewClipId_ && c.ephemeral) return previewClipId_;

    if (nextClipId_ == std::numeric_limits<int>::max())
        return -1;
    int id = nextClipId_++;
    ClipData c;
    c.id        = id;
    c.name      = "(preview)";
    c.ephemeral = true;
    resetCompileOutputs(c);
    clips_.push_back(std::move(c));   // ephemeral lives at the back
    previewClipId_ = id;
    CDBG(PERSISTENCE, "preview clip ensured id=%d", id);
    return id;
}

void ClipStateContainer::clearPreviewClip()
{
    juce::ScopedLock lk(lock_);
    if (previewClipId_ <= 0) return;
    auto it = std::find_if(clips_.begin(), clips_.end(),
                           [this](const ClipData& c) {
                               return c.ephemeral && c.id == previewClipId_;
                           });
    if (it != clips_.end()) {
        clips_.erase(it);
        // The preview clip is always last + never viewed, but clamp defensively.
        if (viewedClipIndex_ >= (int) clips_.size())
            viewedClipIndex_ = clips_.empty() ? 0 : (int) clips_.size() - 1;
    }
    CDBG(PERSISTENCE, "preview clip cleared id=%d", previewClipId_);
    previewClipId_ = -1;
}

int ClipStateContainer::getPreviewClipId() const
{
    juce::ScopedLock lk(lock_);
    return previewClipId_;
}

void ClipStateContainer::setViewedClipIndex(int idx)
{
    juce::ScopedLock lk(lock_);
    const int realClipCount = (int) std::count_if (
        clips_.begin(), clips_.end(), [] (const ClipData& clip) { return ! clip.ephemeral; });
    viewedClipIndex_ = realClipCount == 0
        ? 0
        : juce::jlimit (0, realClipCount - 1, idx);
}

void ClipStateContainer::setProjectName(juce::String name)
{
    juce::ScopedLock lk(lock_);
    projectName_ = name.isNotEmpty() ? std::move(name) : juce::String("Untitled Project");
}

void ClipStateContainer::setVisualSettings(VisualSettings settings)
{
    juce::ScopedLock lk(lock_);
    settings.displayIndex = juce::jmax(0, settings.displayIndex);
    settings.width = juce::jlimit(64, 8192, settings.width);
    settings.height = juce::jlimit(64, 8192, settings.height);
    settings.resolutionScale = juce::jlimit(0.1f, 2.0f, settings.resolutionScale);
    settings.targetFps = juce::jlimit(1.0f, 240.0f, settings.targetFps);
    visualSettings_ = settings;
}

VisualSettings ClipStateContainer::getVisualSettings() const
{
    juce::ScopedLock lk(lock_);
    return visualSettings_;
}

// ── Accessors ───────────────────────────────────────────────────────────────

int ClipStateContainer::getViewedClipIndex() const
{
    juce::ScopedLock lk(lock_);
    return viewedClipIndex_;
}

int ClipStateContainer::getViewedClipId() const
{
    juce::ScopedLock lk(lock_);
    if (viewedClipIndex_ < 0 || viewedClipIndex_ >= (int) clips_.size())
        return -1;
    const auto& viewed = clips_[(size_t) viewedClipIndex_];
    return viewed.ephemeral ? -1 : viewed.id;
}

int ClipStateContainer::getClipIdAtIndex(int idx) const
{
    juce::ScopedLock lk(lock_);
    if (idx < 0 || idx >= (int)clips_.size()) return -1;
    return clips_[idx].id;
}

int ClipStateContainer::getClipIndexById(int id) const
{
    juce::ScopedLock lk(lock_);
    for (size_t i = 0; i < clips_.size(); ++i)
        if (clips_[i].id == id) return (int)i;
    return -1;
}

int ClipStateContainer::getRealClipIndexById(int id) const
{
    juce::ScopedLock lk(lock_);
    for (size_t i = 0; i < clips_.size(); ++i)
        if (! clips_[i].ephemeral && clips_[i].id == id) return (int) i;
    return -1;
}

juce::String ClipStateContainer::getProjectName() const
{
    juce::ScopedLock lk(lock_);
    return projectName_;
}

std::vector<ClipData> ClipStateContainer::getClipsCopy() const
{
    juce::ScopedLock lk(lock_);
    // T-578: the audition scratch clip is invisible to every enumeration surface
    // (launcher list, cue-next, active-clip query, name lookup). The play path
    // reaches it by id (getClipIndexById / getClipCompileResult), never here.
    std::vector<ClipData> out;
    out.reserve(clips_.size());
    for (const auto& c : clips_)
        if (! c.ephemeral) out.push_back(c);
    return out;
}

size_t ClipStateContainer::size() const
{
    juce::ScopedLock lk(lock_);
    return clips_.size();
}

void ClipStateContainer::setClipCompileResult(int id,
                                              std::vector<uint8_t> bytecode,
                                              juce::String diagnosticsJson,
                                              juce::String compileError,
                                              juce::String metadataJson)
{
    juce::ScopedLock lk(lock_);
    for (auto& c : clips_) if (c.id == id) {
        const bool hasError = compileError.isNotEmpty();
        const auto bcLen    = bytecode.size();
        c.bytecode            = std::move(bytecode);
        c.diagnosticsJson     = diagnosticsJson.isNotEmpty() ? std::move(diagnosticsJson) : juce::String("[]");
        c.compileError        = std::move(compileError);
        c.compileMetadataJson = metadataJson.isNotEmpty()    ? std::move(metadataJson)    : juce::String("null");
        CDBG(EMBED_COMPILE, "clip %d compile result: error=%s bytecodeLen=%zu",
             id, hasError ? "Y" : "N", bcLen);
        return;
    }
}

bool ClipStateContainer::setClipCompileError(int id, juce::String compileError)
{
    juce::ScopedLock lk(lock_);
    for (auto& c : clips_) if (c.id == id) {
        if (c.compileError == compileError) return false;
        c.compileError = std::move(compileError);
        return true;
    }
    return false;
}

bool ClipStateContainer::setClipBlockedModules(
    int id, std::vector<BlockedModuleInfo> blockedModules)
{
    juce::ScopedLock lk(lock_);
    for (auto& c : clips_) if (c.id == id) {
        if (c.blockedModules.size() == blockedModules.size() && std::equal(
                c.blockedModules.begin(), c.blockedModules.end(),
                blockedModules.begin(),
                [] (const BlockedModuleInfo& a, const BlockedModuleInfo& b) {
                    return a.nodeId == b.nodeId && a.lineageId == b.lineageId
                        && a.reason == b.reason;
                }))
            return false;
        c.blockedModules = std::move(blockedModules);
        return true;
    }
    return false;
}

bool ClipStateContainer::getClipCompileResult(int id,
                                              std::vector<uint8_t>& outBytecode,
                                              juce::String& outDiagnosticsJson,
                                              juce::String& outCompileError,
                                              juce::String& outMetadataJson) const
{
    juce::ScopedLock lk(lock_);
    for (const auto& c : clips_) if (c.id == id) {
        outBytecode        = c.bytecode;
        outDiagnosticsJson = c.diagnosticsJson;
        outCompileError    = c.compileError;
        outMetadataJson    = c.compileMetadataJson;
        return true;
    }
    return false;
}

// ── Serialization ───────────────────────────────────────────────────────────

// T357-5 (ADR-009): per-instance FaceplateLayout ↔ juce::var. The layout half
// of the faceplate (positions / widget picks / panel style) is the only part
// that cannot re-derive from source, so it persists with the clip state.
// Kept JUCE-free in FaceplateTypes.h — the juce::var bridge lives here, beside
// the other entry serializers (edgeEntryToVar / viewportToVar).

// A FaceplateLayout with no controls and a default panel carries nothing a
// reload could not reconstruct from schema defaults — eliding it keeps
// untouched projects byte-identical to pre-T357-5 saves.
static bool faceplateLayoutIsEmpty(const FaceplateLayout& fp)
{
    return fp.controls.empty()
        && fp.panel.accent.empty()
        && fp.panel.gridColumns == 0;
}

static juce::var faceplateLayoutToVar(const FaceplateLayout& fp)
{
    auto* obj = new juce::DynamicObject();

    auto* panel = new juce::DynamicObject();
    panel->setProperty("accent",      juce::String(fp.panel.accent));
    panel->setProperty("gridColumns", fp.panel.gridColumns);
    obj->setProperty("panel", juce::var(panel));

    juce::Array<juce::var> controlsArr;
    controlsArr.ensureStorageAllocated((int) fp.controls.size());
    for (const auto& c : fp.controls) {
        auto* cObj = new juce::DynamicObject();
        cObj->setProperty("controlId",      juce::String(c.controlId));
        cObj->setProperty("column",         c.column);
        cObj->setProperty("row",            c.row);
        cObj->setProperty("width",          c.width);
        cObj->setProperty("height",         c.height);
        cObj->setProperty("widgetOverride", juce::String(c.widgetOverride));
        cObj->setProperty("orientation",    juce::String(c.orientation));
        cObj->setProperty("mode",           juce::String(c.mode));
        cObj->setProperty("interactive",    c.interactive);
        cObj->setProperty("accent",         juce::String(c.accent));
        cObj->setProperty("size",           c.size);
        cObj->setProperty("skin",           juce::String(c.skin));
        cObj->setProperty("fontSize",       c.fontSize);
        cObj->setProperty("labelPosition",  juce::String(c.labelPosition));
        cObj->setProperty("xParam",         juce::String(c.xParam));
        cObj->setProperty("yParam",         juce::String(c.yParam));
        controlsArr.add(juce::var(cObj));
    }
    obj->setProperty("controls", juce::var(controlsArr));
    return juce::var(obj);
}

static bool faceplateLayoutFromVar(const juce::var& v, FaceplateLayout& fp)
{
    if (!v.isObject()) return true;

    auto panel = v.getProperty("panel", juce::var());
    if (! readOptionalStringProperty(panel, "accent", fp.panel.accent))
        return false;
    if (! readOptionalIntProperty(panel, "gridColumns", 0, fp.panel.gridColumns))
        return false;

    auto controls = v.getProperty("controls", juce::var());
    if (auto* arr = controls.getArray()) {
        fp.controls.reserve((size_t) arr->size());
        for (const auto& cv : *arr) {
            FaceplateLayoutEntry e;
            if (! readRequiredStringProperty(cv, "controlId", e.controlId)) {
                CDBG(PERSISTENCE, "faceplate layout skipped malformed control identity");
                continue;
            }
            if (! readOptionalFloatProperty(cv, "column", -1.0f, e.column)
                || ! readOptionalFloatProperty(cv, "row", -1.0f, e.row)
                || ! readOptionalFloatProperty(cv, "width", 1.0f, e.width)
                || ! readOptionalFloatProperty(cv, "height", 1.0f, e.height)
                || ! readOptionalBoolProperty(cv, "interactive", true, e.interactive)
                || ! readOptionalFloatProperty(cv, "size", 1.0f, e.size)
                || ! readOptionalFontSizeProperty(cv, 0.0f, e.fontSize)
                || ! readOptionalStringProperty(cv, "widgetOverride", e.widgetOverride)
                || ! readOptionalStringProperty(cv, "orientation", e.orientation)
                || ! readOptionalStringProperty(cv, "mode", e.mode)
                || ! readOptionalStringProperty(cv, "accent", e.accent)
                || ! readOptionalStringProperty(cv, "skin", e.skin)
                || ! readOptionalStringProperty(cv, "xParam", e.xParam)
                || ! readOptionalStringProperty(cv, "yParam", e.yParam)) {
                CDBG(PERSISTENCE, "faceplate layout skipped malformed control controlId=%s",
                     e.controlId.c_str());
                continue;
            }
            if (cv.hasProperty("labelPosition")) {
                if (! readOptionalStringProperty(cv, "labelPosition", e.labelPosition)) {
                    CDBG(PERSISTENCE, "faceplate layout skipped malformed labelPosition controlId=%s",
                         e.controlId.c_str());
                    continue;
                }
            } else if (! readOptionalStringProperty(cv, "labelpos", e.labelPosition)) {
                CDBG(PERSISTENCE, "faceplate layout skipped malformed labelpos controlId=%s",
                     e.controlId.c_str());
                continue;
            }
            fp.controls.push_back(std::move(e));
        }
    }
    return true;
}

static juce::var stringArrayVar(const std::vector<std::string>& names)
{
    juce::Array<juce::var> arr;
    arr.ensureStorageAllocated((int) names.size());
    for (const auto& name : names) arr.add(juce::var(juce::String(name)));
    return juce::var(arr);
}

static void appendStringArrayEntries(const juce::var& v,
                                     std::vector<std::string>& out,
                                     const char* owner,
                                     const char* fieldName)
{
    if (auto* arr = v.getArray()) {
        out.reserve(out.size() + (size_t) arr->size());
        for (int i = 0; i < arr->size(); ++i) {
            const auto& item = arr->getReference(i);
            if (! item.isString()) {
                CDBG(PERSISTENCE, "graph import skipped malformed string array owner=%s field=%s row=%d",
                     owner != nullptr ? owner : "", fieldName != nullptr ? fieldName : "", i);
                continue;
            }
            auto s = item.toString().toStdString();
            if (! s.empty())
                out.push_back(std::move(s));
        }
    }
}

static bool wirePresentationIsEmpty(const WirePresentation& w)
{
    return w.role.empty()
        && w.color.empty()
        && w.style.empty()
        && w.polarity.empty()
        && w.minValue == 0.0f
        && w.maxValue == 1.0f;
}

static juce::var socketPresentationToVar(const SocketPresentation& s)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("name",       juce::String(s.name));
    obj->setProperty("direction",  juce::String(s.direction));
    obj->setProperty("role",       juce::String(s.role));
    obj->setProperty("label",      juce::String(s.label));
    obj->setProperty("shape",      juce::String(s.shape));
    obj->setProperty("color",      juce::String(s.color));
    obj->setProperty("wireColor",  juce::String(s.wireColor));
    obj->setProperty("wireStyle",  juce::String(s.wireStyle));
    obj->setProperty("polarity",   juce::String(s.polarity));
    obj->setProperty("min",        s.minValue);
    obj->setProperty("max",        s.maxValue);
    obj->setProperty("anchor",     juce::String(s.anchor));
    return juce::var(obj);
}

static bool socketPresentationFromVar(const juce::var& v, SocketPresentation& s)
{
    if (! v.isObject())
        return false;
    if (! readRequiredStringProperty(v, "name", s.name)
        || ! readOptionalStringProperty(v, "direction", s.direction)
        || ! readOptionalStringProperty(v, "role", s.role)
        || ! readOptionalStringProperty(v, "label", s.label)
        || ! readOptionalStringProperty(v, "shape", s.shape)
        || ! readOptionalStringProperty(v, "color", s.color)
        || ! readOptionalStringProperty(v, "wireColor", s.wireColor)
        || ! readOptionalStringProperty(v, "wireStyle", s.wireStyle)
        || ! readOptionalStringProperty(v, "polarity", s.polarity)
        || ! readOptionalStringProperty(v, "anchor", s.anchor))
        return false;
    return readOptionalFloatProperty(v, "min", 0.0f, s.minValue)
        && readOptionalFloatProperty(v, "max", 1.0f, s.maxValue);
}

static juce::var wirePresentationToVar(const WirePresentation& w)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("role",     juce::String(w.role));
    obj->setProperty("color",    juce::String(w.color));
    obj->setProperty("style",    juce::String(w.style));
    obj->setProperty("polarity", juce::String(w.polarity));
    obj->setProperty("min",      w.minValue);
    obj->setProperty("max",      w.maxValue);
    return juce::var(obj);
}

static bool wirePresentationFromVar(const juce::var& v, WirePresentation& w)
{
    if (! v.isObject())
        return false;
    if (! readOptionalStringProperty(v, "role", w.role)
        || ! readOptionalStringProperty(v, "color", w.color)
        || ! readOptionalStringProperty(v, "style", w.style)
        || ! readOptionalStringProperty(v, "polarity", w.polarity))
        return false;
    return readOptionalFloatProperty(v, "min", 0.0f, w.minValue)
        && readOptionalFloatProperty(v, "max", 1.0f, w.maxValue);
}

static const char* signalTypeToString(vm::SignalType t) noexcept
{
    switch (t) {
        case vm::SignalType::Pitch:    return "pitch";
        case vm::SignalType::Gate:     return "gate";
        case vm::SignalType::Phase:    return "phase";
        case vm::SignalType::Velocity: return "velocity";
        case vm::SignalType::Audio:    return "audio";
        case vm::SignalType::Value:
        default:                       return "value";
    }
}

static vm::SignalType signalTypeFromString(const juce::String& s) noexcept
{
    if (s == "pitch")    return vm::SignalType::Pitch;
    if (s == "gate")     return vm::SignalType::Gate;
    if (s == "phase")    return vm::SignalType::Phase;
    if (s == "velocity") return vm::SignalType::Velocity;
    if (s == "audio")    return vm::SignalType::Audio;
    return vm::SignalType::Value;
}

static juce::var signalTypeArrayVar(const std::vector<vm::SignalType>& types)
{
    juce::Array<juce::var> arr;
    arr.ensureStorageAllocated((int) types.size());
    for (auto type : types)
        arr.add(juce::String(signalTypeToString(type)));
    return juce::var(arr);
}

static std::vector<vm::SignalType> signalTypeArrayFromVar(const juce::var& v)
{
    std::vector<vm::SignalType> out;
    if (auto* arr = v.getArray()) {
        out.reserve((size_t) arr->size());
        for (int i = 0; i < arr->size(); ++i) {
            const auto& item = arr->getReference(i);
            if (! item.isString()) {
                CDBG(PERSISTENCE, "graph import skipped malformed controlOutputTypes row=%d", i);
                continue;
            }
            out.push_back(signalTypeFromString(item.toString()));
        }
    }
    return out;
}

static const char* groupKindToString(GroupKind kind) noexcept
{
    switch (kind) {
        case GroupKind::Horizontal: return "horizontal";
        case GroupKind::Tab:        return "tab";
        case GroupKind::Vertical:
        default:                    return "vertical";
    }
}

static GroupKind groupKindFromString(const juce::String& s) noexcept
{
    if (s == "horizontal") return GroupKind::Horizontal;
    if (s == "tab")        return GroupKind::Tab;
    return GroupKind::Vertical;
}

static juce::var faceplateMeterToVar(const FaceplateMeter& m)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("id",          juce::String(m.id));
    obj->setProperty("name",        juce::String(m.name));
    obj->setProperty("min",         m.minValue);
    obj->setProperty("max",         m.maxValue);
    obj->setProperty("unit",        juce::String(m.unit));
    obj->setProperty("scale",       juce::String(scaleToStr(m.scale)));
    obj->setProperty("widget",      juce::String(m.widgetHint));
    obj->setProperty("orientation", juce::String(m.orientation));
    obj->setProperty("tooltip",     juce::String(m.tooltip));
    obj->setProperty("order",       m.order);
    obj->setProperty("column",      m.column);
    obj->setProperty("row",         m.row);
    obj->setProperty("width",       m.width);
    obj->setProperty("height",      m.height);
    return juce::var(obj);
}

static bool faceplateMeterFromVar(const juce::var& v, FaceplateMeter& m)
{
    if (! v.isObject())
        return false;
    if (! readRequiredStringProperty(v, "id", m.id)
        || ! readOptionalStringProperty(v, "name", m.name))
        return false;
    if (! readOptionalFloatProperty(v, "min", 0.0f, m.minValue)
        || ! readOptionalFloatProperty(v, "max", 1.0f, m.maxValue)
        || ! readOptionalFloatProperty(v, "order", -1.0f, m.order)
        || ! readOptionalFloatProperty(v, "column", -1.0f, m.column)
        || ! readOptionalFloatProperty(v, "row", -1.0f, m.row)
        || ! readOptionalFloatProperty(v, "width", 1.0f, m.width)
        || ! readOptionalFloatProperty(v, "height", 1.0f, m.height))
        return false;
    if (! readOptionalStringProperty(v, "unit", m.unit)
        || ! readOptionalStringProperty(v, "widget", m.widgetHint)
        || ! readOptionalStringProperty(v, "orientation", m.orientation)
        || ! readOptionalStringProperty(v, "tooltip", m.tooltip))
        return false;
    std::string scaleString;
    if (! readOptionalStringProperty(v, "scale", scaleString))
        return false;
    const auto scale = juce::String(scaleString.empty() ? "linear" : scaleString);
    m.scale       = (scale == "logarithmic" || scale == "log") ? Scale::Logarithmic
                  : (scale == "exponential" || scale == "exp") ? Scale::Exponential
                  : Scale::Linear;
    return true;
}

static juce::var faceplateElementToVar(const FaceplateElement& e)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("id",          juce::String(e.id));
    obj->setProperty("kind",        juce::String(e.kind));
    obj->setProperty("label",       juce::String(e.label));
    obj->setProperty("variant",     juce::String(e.variant));
    obj->setProperty("group",       juce::String(e.group));
    obj->setProperty("source",      juce::String(e.source));
    obj->setProperty("column",      e.column);
    obj->setProperty("row",         e.row);
    obj->setProperty("width",       e.width);
    obj->setProperty("height",      e.height);
    obj->setProperty("orientation", juce::String(e.orientation));
    obj->setProperty("accent",      juce::String(e.accent));
    obj->setProperty("fontSize",    e.fontSize);
    obj->setProperty("labelPosition", juce::String(e.labelPosition));
    obj->setProperty("hidden",      e.hidden);
    obj->setProperty("min",         e.minValue);
    obj->setProperty("max",         e.maxValue);
    obj->setProperty("threshold",   e.threshold);
    return juce::var(obj);
}

static bool faceplateElementFromVar(const juce::var& v, FaceplateElement& e)
{
    if (! v.isObject())
        return false;
    if (! readRequiredStringProperty(v, "id", e.id)
        || ! readOptionalStringProperty(v, "kind", e.kind)
        || ! readOptionalStringProperty(v, "label", e.label)
        || ! readOptionalStringProperty(v, "variant", e.variant)
        || ! readOptionalStringProperty(v, "group", e.group)
        || ! readOptionalStringProperty(v, "source", e.source))
        return false;
    if (! readOptionalFloatProperty(v, "column", -1.0f, e.column)
        || ! readOptionalFloatProperty(v, "row", -1.0f, e.row)
        || ! readOptionalFloatProperty(v, "width", 1.0f, e.width)
        || ! readOptionalFloatProperty(v, "height", 1.0f, e.height)
        || ! readOptionalFontSizeProperty(v, 0.0f, e.fontSize)
        || ! readOptionalBoolProperty(v, "hidden", false, e.hidden)
        || ! readOptionalFloatProperty(v, "min", 0.0f, e.minValue)
        || ! readOptionalFloatProperty(v, "max", 1.0f, e.maxValue)
        || ! readOptionalFloatProperty(v, "threshold", 0.5f, e.threshold))
        return false;
    if (! readOptionalStringProperty(v, "orientation", e.orientation))
        return false;
    if (v.hasProperty("accent")) {
        if (! readOptionalStringProperty(v, "accent", e.accent))
            return false;
    } else if (! readOptionalStringProperty(v, "color", e.accent)) {
        return false;
    }
    if (v.hasProperty("labelPosition")) {
        if (! readOptionalStringProperty(v, "labelPosition", e.labelPosition))
            return false;
    } else if (! readOptionalStringProperty(v, "labelpos", e.labelPosition)) {
        return false;
    }
    return true;
}

static juce::var faceplateGroupToVar(const FaceplateGroup& g)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("name",  juce::String(g.name));
    obj->setProperty("kind",  juce::String(groupKindToString(g.kind)));
    obj->setProperty("order", g.order);
    obj->setProperty("column", g.column);
    obj->setProperty("row",    g.row);
    obj->setProperty("width",  g.width);
    obj->setProperty("height", g.height);
    obj->setProperty("accent", juce::String(g.accent));
    obj->setProperty("fontSize", g.fontSize);
    obj->setProperty("labelPosition", juce::String(g.labelPosition));
    obj->setProperty("paramIds", stringArrayVar(g.paramIds));
    obj->setProperty("meterIds", stringArrayVar(g.meterIds));
    juce::Array<juce::var> children;
    children.ensureStorageAllocated((int) g.children.size());
    for (const auto& child : g.children)
        children.add(faceplateGroupToVar(child));
    obj->setProperty("children", juce::var(children));
    return juce::var(obj);
}

static bool faceplateGroupHasState(const FaceplateGroup& g)
{
    if (! g.name.empty()
        || g.kind != GroupKind::Vertical
        || g.order != -1.0f
        || g.column != -1.0f
        || g.row != -1.0f
        || g.width != -1.0f
        || g.height != -1.0f
        || ! g.accent.empty()
        || g.fontSize != 0.0f
        || ! g.labelPosition.empty()
        || ! g.paramIds.empty()
        || ! g.meterIds.empty())
        return true;

    for (const auto& child : g.children)
        if (faceplateGroupHasState(child))
            return true;
    return false;
}

static bool faceplateGroupFromVar(const juce::var& v, FaceplateGroup& g)
{
    if (! v.isObject()) return true;
    if (! readOptionalStringProperty(v, "name", g.name))
        return false;
    std::string kindString;
    if (! readOptionalStringProperty(v, "kind", kindString))
        return false;
    g.kind = groupKindFromString(kindString.empty() ? "vertical" : juce::String(kindString));
    if (! readOptionalFloatProperty(v, "order", -1.0f, g.order)
        || ! readOptionalFloatProperty(v, "column", -1.0f, g.column)
        || ! readOptionalFloatProperty(v, "row", -1.0f, g.row)
        || ! readOptionalFloatProperty(v, "width", -1.0f, g.width)
        || ! readOptionalFloatProperty(v, "height", -1.0f, g.height)
        || ! readOptionalFontSizeProperty(v, 0.0f, g.fontSize))
        return false;
    if (! readOptionalStringProperty(v, "accent", g.accent))
        return false;
    if (v.hasProperty("labelPosition")) {
        if (! readOptionalStringProperty(v, "labelPosition", g.labelPosition))
            return false;
    } else if (! readOptionalStringProperty(v, "labelpos", g.labelPosition)) {
        return false;
    }
    appendStringArrayEntries(v.getProperty("paramIds", juce::var()), g.paramIds, g.name.c_str(), "paramIds");
    appendStringArrayEntries(v.getProperty("meterIds", juce::var()), g.meterIds, g.name.c_str(), "meterIds");
    if (auto* arr = v.getProperty("children", juce::var()).getArray())
        for (const auto& item : *arr) {
            FaceplateGroup child;
            if (! faceplateGroupFromVar(item, child)) {
                CDBG(PERSISTENCE, "faceplate group skipped malformed child parent=%s", g.name.c_str());
                continue;
            }
            g.children.push_back(std::move(child));
        }
    return true;
}

static juce::var paramSchemaEntryToVar(const ParamSchemaEntry& p)
{
    auto* pObj = new juce::DynamicObject();
    pObj->setProperty("sourceId",     juce::String(p.sourceId));
    pObj->setProperty("name",         juce::String(p.name));
    pObj->setProperty("type",         juce::String(paramTypeToStr(p.type)));
    pObj->setProperty("min",          p.min);
    pObj->setProperty("max",          p.max);
    pObj->setProperty("defaultValue", p.defaultValue);
    pObj->setProperty("unit",         juce::String(p.unit));
    pObj->setProperty("scale",        juce::String(scaleToStr(p.scale)));
    pObj->setProperty("widget",       juce::String(p.widget));
    pObj->setProperty("tooltip",      juce::String(p.tooltip));
    pObj->setProperty("order",        p.order);
    pObj->setProperty("column",       p.column);
    pObj->setProperty("row",          p.row);
    pObj->setProperty("width",        p.width);
    pObj->setProperty("height",       p.height);
    pObj->setProperty("orientation",  juce::String(p.orientation));
    pObj->setProperty("hidden",       p.hidden);
    pObj->setProperty("graphInput",   p.graphInput);
    pObj->setProperty("mode",         juce::String(p.mode));
    pObj->setProperty("interactive",  p.interactive);
    pObj->setProperty("accent",       juce::String(p.accent));
    pObj->setProperty("size",         p.size);
    pObj->setProperty("skin",         juce::String(p.skin));
    pObj->setProperty("fontSize",     p.fontSize);
    pObj->setProperty("labelPosition", juce::String(p.labelPosition));
    pObj->setProperty("xParam",       juce::String(p.xParam));
    pObj->setProperty("yParam",       juce::String(p.yParam));
    pObj->setProperty("acceptance",    juce::String(paramAcceptanceToStr(p.acceptance)));
    if (! p.options.empty()) {
        juce::Array<juce::var> opts;
        for (const auto& opt : p.options) {
            auto* o = new juce::DynamicObject();
            o->setProperty("label", juce::String(opt.label));
            o->setProperty("value", opt.value);
            opts.add(juce::var(o));
        }
        pObj->setProperty("options", juce::var(opts));
    }
    return juce::var(pObj);
}

static bool paramSchemaEntryFromVar(const juce::var& pv, ParamSchemaEntry& p)
{
    if (! pv.isObject())
        return false;
    if (! readOptionalStringProperty(pv, "sourceId", p.sourceId)
        || ! readRequiredStringProperty(pv, "name", p.name))
        return false;
    std::string typeString;
    if (! readOptionalStringProperty(pv, "type", typeString))
        return false;
    const auto typeStr = typeString.empty() ? std::string("continuous") : typeString;
    if      (typeStr == "integer")   p.type = ParamType::Integer;
    else if (typeStr == "boolean")   p.type = ParamType::Boolean;
    else if (typeStr == "select")    p.type = ParamType::Select;
    else if (typeStr == "display")   p.type = ParamType::Display;
    else                              p.type = ParamType::Continuous;
    if (! readOptionalFloatProperty(pv, "min", 0.0f, p.min)
        || ! readOptionalFloatProperty(pv, "max", 1.0f, p.max)
        || ! readOptionalFloatProperty(pv, "defaultValue", 0.0f, p.defaultValue))
        return false;
    if (! readOptionalStringProperty(pv, "unit", p.unit)
        || ! readOptionalStringProperty(pv, "widget", p.widget)
        || ! readOptionalStringProperty(pv, "tooltip", p.tooltip))
        return false;
    std::string scaleString;
    if (! readOptionalStringProperty(pv, "scale", scaleString))
        return false;
    const auto scaleStr = scaleString.empty() ? std::string("linear") : scaleString;
    if      (scaleStr == "logarithmic" || scaleStr == "log") p.scale = Scale::Logarithmic;
    else if (scaleStr == "exponential" || scaleStr == "exp") p.scale = Scale::Exponential;
    else                                                      p.scale = Scale::Linear;
    if (! readOptionalFloatProperty(pv, "order", -1.0f, p.order)
        || ! readOptionalFloatProperty(pv, "column", -1.0f, p.column)
        || ! readOptionalFloatProperty(pv, "row", -1.0f, p.row)
        || ! readOptionalFloatProperty(pv, "width", 1.0f, p.width)
        || ! readOptionalFloatProperty(pv, "height", 1.0f, p.height)
        || ! readOptionalBoolProperty(pv, "hidden", false, p.hidden)
        || ! readOptionalBoolProperty(pv, "graphInput", false, p.graphInput)
        || ! readOptionalBoolProperty(pv, "interactive", true, p.interactive)
        || ! readOptionalFloatProperty(pv, "size", 1.0f, p.size)
        || ! readOptionalFontSizeProperty(pv, 0.0f, p.fontSize))
        return false;
    if (! readOptionalStringProperty(pv, "orientation", p.orientation)
        || ! readOptionalStringProperty(pv, "mode", p.mode)
        || ! readOptionalStringProperty(pv, "accent", p.accent)
        || ! readOptionalStringProperty(pv, "skin", p.skin)
        || ! readOptionalStringProperty(pv, "xParam", p.xParam)
        || ! readOptionalStringProperty(pv, "yParam", p.yParam))
        return false;
    if (pv.hasProperty("labelPosition")) {
        if (! readOptionalStringProperty(pv, "labelPosition", p.labelPosition))
            return false;
    } else if (! readOptionalStringProperty(pv, "labelpos", p.labelPosition)) {
        return false;
    }
    std::string acceptanceString;
    if (! readOptionalStringProperty(pv, "acceptance", acceptanceString))
        return false;
    const auto acceptance = juce::String(acceptanceString.empty() ? "control_value" : acceptanceString);
    if (acceptance == "audio_only" || acceptance == "audioOnly" || acceptance == "audio")
        p.acceptance = ParamAcceptance::AudioOnly;
    else if (acceptance == "control_set" || acceptance == "controlSet" || acceptance == "set")
        p.acceptance = ParamAcceptance::ControlSet;
    else
        p.acceptance = ParamAcceptance::ControlValue;
    if (auto* opts = pv.getProperty("options", juce::var()).getArray())
        for (const auto& ov : *opts) {
            ParamOption opt;
            if (! readOptionalStringProperty(ov, "label", opt.label)) {
                CDBG(PERSISTENCE, "param schema skipped malformed option label param=%s",
                     p.name.c_str());
                continue;
            }
            if (! readOptionalFloatProperty(ov, "value", (float) p.options.size(), opt.value)) {
                CDBG(PERSISTENCE, "param schema skipped malformed option param=%s label=%s",
                     p.name.c_str(), opt.label.c_str());
                continue;
            }
            p.options.push_back(std::move(opt));
        }
    return true;
}

static juce::var moduleEntryToVar(const ModuleEntry& m)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("index",     m.index);
    obj->setProperty("dslName",   juce::String(m.dslName));
    obj->setProperty("moduleId",  juce::String(m.moduleId));
    obj->setProperty("lineageId", juce::String(m.lineageId));

    // T-100 / T-364: stable identity. An AUTHORED module carries its own
    // per-instance lineageUuid (assigned at first version-save, F-069); a
    // factory module has none on the instance and resolves its UUID from the
    // registry by lineageId. Prefer the instance field, fall back to the
    // registry. Written only when one resolves — untouched projects round-trip
    // unchanged.
    if (!m.lineageUuid.empty())
        obj->setProperty("lineageUuid", juce::String(m.lineageUuid));
    else if (const auto* sc = curlop::factoryModuleSchema(m.lineageId))
        if (!sc->lineageUuid.empty())
            obj->setProperty("lineageUuid", juce::String(sc->lineageUuid));

    // T-364: version pin for an authored module (0 = unversioned, use `code`).
    // Only written when set, so untouched projects round-trip byte-identical.
    if (m.moduleVersion > 0)
        obj->setProperty("moduleVersion", m.moduleVersion);
    if (!m.presetName.empty())
        obj->setProperty("presetName", juce::String(m.presetName));
    if (!m.moduleStoreStatus.empty())
        obj->setProperty("moduleStoreStatus", juce::String(m.moduleStoreStatus));

    auto* pos = new juce::DynamicObject();
    pos->setProperty("x", m.position.x);
    pos->setProperty("y", m.position.y);
    obj->setProperty("position", juce::var(pos));

    // T-292: user-resized node body size (0 = none). Only written when set,
    // so untouched projects round-trip byte-identical.
    if (m.nodeSize.x > 0.0f && m.nodeSize.y > 0.0f) {
        obj->setProperty("width",  m.nodeSize.x);
        obj->setProperty("height", m.nodeSize.y);
    }

    juce::Array<juce::var> params;
    params.ensureStorageAllocated((int) m.paramValues.size());
    for (const auto& kv : m.paramValues) {
        auto* p = new juce::DynamicObject();
        p->setProperty("name", juce::String(kv.first));
        p->setProperty("value", kv.second);
        params.add(juce::var(p));
    }
    obj->setProperty("paramValues", juce::var(params));
    if (! m.authoredParamValues.empty()) {
        juce::Array<juce::var> authoredValues;
        authoredValues.ensureStorageAllocated((int) m.authoredParamValues.size());
        for (const auto& entry : m.authoredParamValues) {
            auto* authored = new juce::DynamicObject();
            authored->setProperty("paramId", juce::String(entry.paramId));
            authored->setProperty("value", entry.value.value);
            authored->setProperty("basis",
                juce::String(param::authoredBasisToString(entry.value.basis)));
            if (! entry.timingSourceId.empty())
                authored->setProperty("timingSourceId", juce::String(entry.timingSourceId));
            authoredValues.add(juce::var(authored));
        }
        obj->setProperty("authoredParamValues", juce::var(authoredValues));
    }
    obj->setProperty("code", juce::String(m.code));
    if (!m.visualTarget.empty())
        obj->setProperty("visualTarget", juce::String(m.visualTarget));
    if (!m.authoredPackageId.empty())
        obj->setProperty("authoredPackageId", juce::String(m.authoredPackageId));
    if (!m.sourceUnitId.empty())
        obj->setProperty("sourceUnitId", juce::String(m.sourceUnitId));
    if (!m.sourceKey.empty())
        obj->setProperty("sourceKey", juce::String(m.sourceKey));
    if (!m.sourceLanguage.empty())
        obj->setProperty("sourceLanguage", juce::String(m.sourceLanguage));
    if (!m.midiInputRouteId.empty())
        obj->setProperty("midiInputRouteId", juce::String(m.midiInputRouteId));
    if (!m.midiOutputRouteId.empty())
        obj->setProperty("midiOutputRouteId", juce::String(m.midiOutputRouteId));
    if (!m.bufferAssetPath.empty())
        obj->setProperty("bufferAssetPath", juce::String(m.bufferAssetPath));

    // T-480: per-instance voice policy. Only written when set, so untouched
    // projects round-trip byte-identical (absent → registry default).
    if (m.physicalVoices > 0)
        obj->setProperty("physicalVoices", m.physicalVoices);
    if (m.stealPolicy != 0)
        obj->setProperty("stealPolicy", (int) m.stealPolicy);
    if (m.processingMode != ModuleProcessingMode::Native)
        obj->setProperty(
            "processingMode",
            moduleProcessingModeToString(m.processingMode));
    if (m.oversamplingFactor != ModuleOversamplingFactor::X1)
        obj->setProperty(
            "oversamplingFactor",
            moduleOversamplingFactorToString(m.oversamplingFactor));
    if (m.lineageId == "core.audio_input")
        obj->setProperty(
            "audioInputChannelIndex",
            static_cast<juce::int64>(m.audioInputChannelIndex));

    // T-318: per-instance param schema (CON-007 Phase 2b). Only written when
    // populated, so static-schema modules round-trip byte-identical to
    // pre-T-318 projects.
    if (!m.params.empty()) {
        juce::Array<juce::var> schemaArr;
        schemaArr.ensureStorageAllocated((int) m.params.size());
        for (const auto& p : m.params)
            schemaArr.add(paramSchemaEntryToVar(p));
        obj->setProperty("paramSchema", juce::var(schemaArr));
    }

    // T357-5 (ADR-009): per-instance faceplate layout overrides. Only written
    // when non-empty, so untouched projects round-trip byte-identical to
    // pre-T357-5 projects.
    if (!faceplateLayoutIsEmpty(m.faceplate))
        obj->setProperty("faceplate", faceplateLayoutToVar(m.faceplate));

    if (!m.exposedParamInputs.empty() || m.exposedParamInputsExplicit)
        obj->setProperty("exposedParamInputs", stringArrayVar(m.exposedParamInputs));

    if (!m.socketPresentation.empty()) {
        juce::Array<juce::var> sockets;
        sockets.ensureStorageAllocated((int) m.socketPresentation.size());
        for (const auto& s : m.socketPresentation)
            sockets.add(socketPresentationToVar(s));
        obj->setProperty("socketPresentation", juce::var(sockets));
    }

    if (!m.faceplateElements.empty()) {
        juce::Array<juce::var> elements;
        elements.ensureStorageAllocated((int) m.faceplateElements.size());
        for (const auto& e : m.faceplateElements)
            elements.add(faceplateElementToVar(e));
        obj->setProperty("faceplateElements", juce::var(elements));
    }

    if (!m.faceplateMeters.empty()) {
        juce::Array<juce::var> meters;
        meters.ensureStorageAllocated((int) m.faceplateMeters.size());
        for (const auto& meter : m.faceplateMeters)
            meters.add(faceplateMeterToVar(meter));
        obj->setProperty("faceplateMeters", juce::var(meters));
    }

    if (faceplateGroupHasState(m.faceplateGroup))
        obj->setProperty("faceplateGroup", faceplateGroupToVar(m.faceplateGroup));

    if (!m.audioInputs.empty())
        obj->setProperty("audioInputs", stringArrayVar(m.audioInputs));
    if (!m.audioOutputs.empty())
        obj->setProperty("audioOutputs", stringArrayVar(m.audioOutputs));
    if (!m.visualInputs.empty())
        obj->setProperty("visualInputs", stringArrayVar(m.visualInputs));
    if (!m.visualOutputs.empty())
        obj->setProperty("visualOutputs", stringArrayVar(m.visualOutputs));
    if (!m.controlInputs.empty())
        obj->setProperty("controlInputs", stringArrayVar(m.controlInputs));
    if (!m.controlOutputs.empty())
        obj->setProperty("controlOutputs", stringArrayVar(m.controlOutputs));
    if (!m.controlOutputTypes.empty())
        obj->setProperty("controlOutputTypes", signalTypeArrayVar(m.controlOutputTypes));
    const auto signalInputs = (! m.signalInputsExplicit && m.signalInputs.empty())
        ? legacySignalInputs(m) : m.signalInputs;
    const auto signalOutputs = (! m.signalOutputsExplicit && m.signalOutputs.empty())
        ? legacySignalOutputs(m) : m.signalOutputs;
    if (m.signalInputsExplicit || ! signalInputs.empty())
        obj->setProperty("signalInputs", signalPortDeclarationsToVar(signalInputs));
    if (m.signalOutputsExplicit || ! signalOutputs.empty())
        obj->setProperty("signalOutputs", signalPortDeclarationsToVar(signalOutputs));

    return juce::var(obj);
}

static juce::var edgeEntryToVar(const EdgeEntry& e)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("srcIndex", e.srcIndex);
    obj->setProperty("tgtIndex", e.tgtIndex);
    obj->setProperty("srcPort",  juce::String(e.srcPort));
    obj->setProperty("tgtPort",  juce::String(e.tgtPort));
    if (!e.signalRole.empty())
        obj->setProperty("signalRole", juce::String(e.signalRole));
    if (e.signalDescriptor)
        obj->setProperty("signalDescriptor",
                         signalDescriptorToVar(*e.signalDescriptor));
    if (e.feedbackBoundary == FeedbackBoundary::OneSample)
        obj->setProperty("feedbackBoundary", "one-sample");
    if (e.modulation) {
        auto* mapping = new juce::DynamicObject();
        mapping->setProperty("routeId", juce::String(e.modulation->routeId));
        mapping->setProperty("sourceId", juce::String(e.modulation->sourceId));
        mapping->setProperty("targetParamId", juce::String(e.modulation->targetParamId));
        mapping->setProperty("depth", e.modulation->depth);
        mapping->setProperty("polarity",
            e.modulation->polarity == ModulationPolarity::Bipolar
                ? "bipolar" : "unipolar");
        obj->setProperty("modulation", juce::var(mapping));
    }
    obj->setProperty("gain",     e.gain);
    obj->setProperty("pan",      e.pan);
    obj->setProperty("muted",    e.muted);
    obj->setProperty("soloed",   e.soloed);
    if (!wirePresentationIsEmpty(e.presentation))
        obj->setProperty("wirePresentation", wirePresentationToVar(e.presentation));
    return juce::var(obj);
}

static juce::var viewportToVar(const Viewport& v)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("x",    v.x);
    obj->setProperty("y",    v.y);
    obj->setProperty("zoom", v.zoom);
    return juce::var(obj);
}

juce::String ClipStateContainer::toJson(const GraphStateContainer& gc) const
{
    juce::ScopedLock lk(lock_);

    juce::Array<juce::var> clipsArr;
    for (const auto& c : clips_) {
        if (c.ephemeral) continue;   // T-578: the audition scratch clip never persists
        auto* clipObj = new juce::DynamicObject();
        clipObj->setProperty("id",   c.id);
        clipObj->setProperty("name", c.name);
        // F-076: per-clip colour (ADR ¶21). Only written when set, so untouched
        // projects round-trip byte-identical to pre-F-076 saves.
        if (c.colour.isNotEmpty())
            clipObj->setProperty("colour", c.colour);
        if (c.legacyScriptPresent)
            clipObj->setProperty("script", c.legacyScriptPayload);

        auto* graph = new juce::DynamicObject();
        if (auto* gs = gc.tryForClip(c.id)) {
            juce::Array<juce::var> modsArr;
            for (const auto& m : gs->modules()) modsArr.add(moduleEntryToVar(m));
            juce::Array<juce::var> edgesArr;
            for (const auto& e : gs->edges())   edgesArr.add(edgeEntryToVar(e));
            graph->setProperty("modules",  juce::var(modsArr));
            graph->setProperty("edges",    juce::var(edgesArr));
            graph->setProperty("viewport", viewportToVar(gs->viewport()));
        } else {
            graph->setProperty("modules",  juce::var(juce::Array<juce::var>{}));
            graph->setProperty("edges",    juce::var(juce::Array<juce::var>{}));
            graph->setProperty("viewport", viewportToVar(Viewport{}));
        }
        clipObj->setProperty("graph", juce::var(graph));
        clipsArr.add(juce::var(clipObj));
    }

    auto* envelope = new juce::DynamicObject();
    envelope->setProperty("clips",            juce::var(clipsArr));
    envelope->setProperty("viewedClipIndex",  viewedClipIndex_);
    // T357-5: the `moduleRegistry` envelope blob is gone (DRIFT-PL-2). It was
    // a JSON copy of a registry never reconciled with the live moduleRegistry_
    // — written here, read into an inert field on load, applied nowhere. The
    // live registry re-seeds from populateFactoryModuleRegistry() at every
    // boot; the GUI's MODULE_REGISTRY_STATE is built from that live map, never
    // from this blob. Removing it leaves one registry store, not two.
    envelope->setProperty("projectName",      projectName_);
    envelope->setProperty("nextClipId",       nextClipId_);
    auto* visual = new juce::DynamicObject();
    visual->setProperty("backgroundEnabled", visualSettings_.backgroundEnabled);
    visual->setProperty("windowEnabled", visualSettings_.windowEnabled);
    visual->setProperty("displayIndex", visualSettings_.displayIndex);
    visual->setProperty("fullscreen", visualSettings_.fullscreen);
    visual->setProperty("width", visualSettings_.width);
    visual->setProperty("height", visualSettings_.height);
    visual->setProperty("resolutionScale", visualSettings_.resolutionScale);
    visual->setProperty("targetFps", visualSettings_.targetFps);
    visual->setProperty("captureEnabled", visualSettings_.captureEnabled);
    visual->setProperty("mode", visualSettings_.mode);
    envelope->setProperty("visualSettings", juce::var(visual));

    return juce::JSON::toString(juce::var(envelope));
}

// T-266 (Debt-2/3/12, Phase E): slim CLIPS_STATE wire payload. Drops the
// per-clip graph walk and the script body — the Web snapshot consumer
// only consumes id/name + viewedClipId + projectName. Emits viewedClipId
// directly so the index round-trip in applyClipsState goes away.
juce::var ClipStateContainer::toClipsStateVar() const
{
    juce::ScopedLock lk(lock_);

    juce::Array<juce::var> clipsArr;
    clipsArr.ensureStorageAllocated((int) clips_.size());
    for (const auto& c : clips_) {
        if (c.ephemeral) continue;   // T-578: the audition scratch clip stays out of the launcher
        auto* clipObj = new juce::DynamicObject();
        clipObj->setProperty("id",           c.id);
        clipObj->setProperty("name",         c.name);
        clipObj->setProperty("compileError", c.compileError);
        if (! c.blockedModules.empty()) {
            juce::Array<juce::var> blockedArr;
            blockedArr.ensureStorageAllocated((int) c.blockedModules.size());
            for (const auto& b : c.blockedModules) {
                auto* entry = new juce::DynamicObject();
                entry->setProperty("nodeId",    b.nodeId);
                entry->setProperty("lineageId", b.lineageId);
                entry->setProperty("reason",    b.reason);
                blockedArr.add(juce::var(entry));
            }
            clipObj->setProperty("blockedModules", juce::var(blockedArr));
        }
        clipsArr.add(juce::var(clipObj));
    }

    const int viewedId =
        (viewedClipIndex_ >= 0 && viewedClipIndex_ < (int) clips_.size())
            && ! clips_[(size_t) viewedClipIndex_].ephemeral
            ? clips_[(size_t) viewedClipIndex_].id
            : -1;

    auto* envelope = new juce::DynamicObject();
    envelope->setProperty("clips",        juce::var(clipsArr));
    envelope->setProperty("viewedClipId", viewedId);
    envelope->setProperty("projectName",  projectName_);
    auto* visual = new juce::DynamicObject();
    visual->setProperty("backgroundEnabled", visualSettings_.backgroundEnabled);
    visual->setProperty("windowEnabled", visualSettings_.windowEnabled);
    visual->setProperty("displayIndex", visualSettings_.displayIndex);
    visual->setProperty("fullscreen", visualSettings_.fullscreen);
    visual->setProperty("width", visualSettings_.width);
    visual->setProperty("height", visualSettings_.height);
    visual->setProperty("resolutionScale", visualSettings_.resolutionScale);
    visual->setProperty("targetFps", visualSettings_.targetFps);
    visual->setProperty("captureEnabled", visualSettings_.captureEnabled);
    visual->setProperty("mode", visualSettings_.mode);
    envelope->setProperty("visualSettings", juce::var(visual));
    return juce::var(envelope);
}

static bool moduleEntryFromVar(const juce::var& v, ModuleEntry& m)
{
    if (! v.isObject())
        return false;

    if (! curlop::payload::readIntProperty(v, "index", m.index) || m.index < 0)
        return false;

    if (! readOptionalStringProperty(v, "dslName", m.dslName)
        || ! readRequiredStringProperty(v, "moduleId", m.moduleId)
        || ! readRequiredStringProperty(v, "lineageId", m.lineageId))
        return false;
    // T-100: identity-first resolution. If the project carries a stable
    // lineageUuid still owned by a factory module, adopt that module's
    // current dotted lineageId — so the project survives a registry-path
    // recategorize. Absent / unknown uuid (pre-T-100 projects, user
    // modules) → keep the persisted lineageId; that fallback is the migration.
    {
        std::string uuid;
        if (! readOptionalStringProperty(v, "lineageUuid", uuid))
            return false;
        if (const auto* sc = curlop::factoryModuleByUuid(uuid))
            m.lineageId = sc->lineageId;   // factory module — registry is identity authority
        else if (!uuid.empty())
            m.lineageUuid = uuid;          // T-364: authored module — uuid lives on the instance
    }
    auto pos = v.getProperty("position", juce::var());
    if (pos.isObject()) {
        if (! readOptionalFloatProperty(pos, "x", 0.0f, m.position.x)
            || ! readOptionalFloatProperty(pos, "y", 0.0f, m.position.y))
            return false;
    }

    // T-292: user-resized node body size (absent in older projects → {0,0}).
    if (! readOptionalFloatProperty(v, "width", 0.0f, m.nodeSize.x)
        || ! readOptionalFloatProperty(v, "height", 0.0f, m.nodeSize.y))
        return false;

    auto params = v.getProperty("paramValues", juce::var());
    if (params.isVoid())
        params = v.getProperty("paramDefaults", juce::var());
    if (auto* arr = params.getArray()) {
        for (const auto& pv : *arr) {
            std::string name;
            if (! readOptionalStringProperty(pv, "name", name)) {
                CDBG(PERSISTENCE, "graph import skipped malformed param value name");
                continue;
            }
            if (name.empty())
                continue;

            float value = 0.0f;
            if (! readOptionalFloatProperty(pv, "value", 0.0f, value)) {
                CDBG(PERSISTENCE, "graph import skipped malformed param value name=%s", name.c_str());
                continue;
            }
            m.paramValues[name] = value;
        }
    } else if (auto* obj = params.getDynamicObject()) {
        for (auto& prop : obj->getProperties()) {
            double parsed = 0.0;
            if (! curlop::payload::readFiniteDouble(prop.value, parsed)) {
                CDBG(PERSISTENCE, "graph import skipped malformed param value name=%s",
                     prop.name.toString().toRawUTF8());
                continue;
            }
            m.paramValues[prop.name.toString().toStdString()] = static_cast<float>(parsed);
        }
    }
    const auto authoredValues = v.getProperty("authoredParamValues", juce::var());
    if (! authoredValues.isVoid()) {
        const auto* entries = authoredValues.getArray();
        if (entries == nullptr)
            return false;
        for (const auto& av : *entries) {
            AuthoredParameterState entry;
            std::string basis;
            double value = 0.0;
            if (! av.isObject()
                || ! readRequiredStringProperty(av, "paramId", entry.paramId)
                || ! readRequiredStringProperty(av, "basis", basis)
                || ! curlop::payload::readFiniteDouble(av.getProperty("value", {}), value)
                || ! readOptionalStringProperty(av, "timingSourceId", entry.timingSourceId)
                || entry.paramId.empty()
                || ! param::authoredBasisFromString(basis, entry.value.basis)
                || (entry.value.basis == param::AuthoredBasis::ScriptSteps
                    && entry.timingSourceId.empty()))
                return false;
            entry.value.value = value;
            m.authoredParamValues.push_back(std::move(entry));
        }
    }
    if (! readOptionalStringProperty(v, "code", m.code)
        || ! readOptionalStringProperty(v, "visualTarget", m.visualTarget)
        || ! readOptionalStringProperty(v, "authoredPackageId", m.authoredPackageId)
        || ! readOptionalStringProperty(v, "sourceUnitId", m.sourceUnitId)
        || ! readOptionalStringProperty(v, "sourceKey", m.sourceKey)
        || ! readOptionalStringProperty(v, "sourceLanguage", m.sourceLanguage)
        || ! readOptionalStringProperty(v, "midiInputRouteId", m.midiInputRouteId)
        || ! readOptionalStringProperty(v, "midiOutputRouteId", m.midiOutputRouteId)
        || ! readOptionalStringProperty(v, "bufferAssetPath", m.bufferAssetPath)
        || ! readOptionalStringProperty(v, "presetName", m.presetName)
        || ! readOptionalStringProperty(v, "moduleStoreStatus", m.moduleStoreStatus))
        return false;

    // T-480: per-instance voice policy (absent → 0 = registry default).
    int physicalVoices = 0;
    int stealPolicy = 0;
    if (! readOptionalIntProperty(v, "physicalVoices", 0, physicalVoices)
        || ! readOptionalIntProperty(v, "stealPolicy", 0, stealPolicy))
        return false;
    m.physicalVoices = clampPhysicalVoices(physicalVoices);
    m.stealPolicy    = static_cast<uint8_t>(clampStealPolicy(stealPolicy));
    std::string processingMode;
    if (! readOptionalStringProperty(v, "processingMode", processingMode)
        || ! moduleProcessingModeFromString(processingMode, m.processingMode))
        return false;
    std::string oversamplingFactor;
    if (! readOptionalStringProperty(v, "oversamplingFactor", oversamplingFactor)
        || ! moduleOversamplingFactorFromString(
            oversamplingFactor, m.oversamplingFactor))
        return false;
    if (v.hasProperty("audioInputChannelIndex")) {
        if (m.lineageId != "core.audio_input")
            return false;
        transport::SignalWidth channelIndex = 0;
        if (! readPersistedSignalWidth(
                v.getProperty("audioInputChannelIndex", {}), channelIndex))
            return false;
        m.audioInputChannelIndex = channelIndex;
    }

    // T-364: authored-module version pin (absent → 0 = unversioned, use `code`).
    if (! readOptionalIntProperty(v, "moduleVersion", 0, m.moduleVersion))
        return false;

    // T-318: per-instance param schema. Absent in older projects → empty
    // vector → static-schema modules behave unchanged.
    auto schema = v.getProperty("paramSchema", juce::var());
    if (auto* arr = schema.getArray()) {
        m.params.reserve((size_t) arr->size());
        for (const auto& pv : *arr) {
            ParamSchemaEntry entry;
            if (! paramSchemaEntryFromVar(pv, entry)) {
                CDBG(PERSISTENCE, "graph import skipped malformed param schema module=%s",
                     m.dslName.c_str());
                continue;
            }
            m.params.push_back(std::move(entry));
        }
    }

    // T357-5: per-instance faceplate layout overrides. Absent in pre-T357-5
    // projects → default-constructed (empty) FaceplateLayout.
    if (! faceplateLayoutFromVar(v.getProperty("faceplate", juce::var()), m.faceplate))
        CDBG(PERSISTENCE, "graph import skipped malformed faceplate layout module=%s", m.dslName.c_str());

    if (auto* arr = v.getProperty("exposedParamInputs", juce::var()).getArray()) {
        m.exposedParamInputsExplicit = true;
        appendStringArrayEntries(v.getProperty("exposedParamInputs", juce::var()),
                                 m.exposedParamInputs,
                                 m.dslName.c_str(),
                                 "exposedParamInputs");
    }

    if (auto* arr = v.getProperty("socketPresentation", juce::var()).getArray()) {
        m.socketPresentation.reserve((size_t) arr->size());
        for (const auto& sv : *arr) {
            SocketPresentation entry;
            if (! socketPresentationFromVar(sv, entry)) {
                CDBG(PERSISTENCE, "graph import skipped malformed socket presentation module=%s",
                     m.dslName.c_str());
                continue;
            }
            m.socketPresentation.push_back(std::move(entry));
        }
    }

    if (auto* arr = v.getProperty("faceplateElements", juce::var()).getArray()) {
        m.faceplateElements.reserve((size_t) arr->size());
        for (const auto& ev : *arr) {
            FaceplateElement entry;
            if (! faceplateElementFromVar(ev, entry)) {
                CDBG(PERSISTENCE, "graph import skipped malformed faceplate element module=%s",
                     m.dslName.c_str());
                continue;
            }
            m.faceplateElements.push_back(std::move(entry));
        }
    }

    if (auto* arr = v.getProperty("faceplateMeters", juce::var()).getArray()) {
        m.faceplateMeters.reserve((size_t) arr->size());
        for (const auto& mv : *arr) {
            FaceplateMeter entry;
            if (! faceplateMeterFromVar(mv, entry)) {
                CDBG(PERSISTENCE, "graph import skipped malformed faceplate meter module=%s",
                     m.dslName.c_str());
                continue;
            }
            m.faceplateMeters.push_back(std::move(entry));
        }
    }

    auto group = v.getProperty("faceplateGroup", juce::var());
    if (! group.isVoid()) {
        FaceplateGroup parsedGroup;
        if (faceplateGroupFromVar(group, parsedGroup))
            m.faceplateGroup = std::move(parsedGroup);
        else
            CDBG(PERSISTENCE, "graph import skipped malformed faceplate group module=%s", m.dslName.c_str());
    }

    appendStringArrayEntries(v.getProperty("audioInputs", juce::var()), m.audioInputs,
                             m.dslName.c_str(), "audioInputs");
    appendStringArrayEntries(v.getProperty("audioOutputs", juce::var()), m.audioOutputs,
                             m.dslName.c_str(), "audioOutputs");
    appendStringArrayEntries(v.getProperty("visualInputs", juce::var()), m.visualInputs,
                             m.dslName.c_str(), "visualInputs");
    appendStringArrayEntries(v.getProperty("visualOutputs", juce::var()), m.visualOutputs,
                             m.dslName.c_str(), "visualOutputs");
    appendStringArrayEntries(v.getProperty("controlInputs", juce::var()), m.controlInputs,
                             m.dslName.c_str(), "controlInputs");
    appendStringArrayEntries(v.getProperty("controlOutputs", juce::var()), m.controlOutputs,
                             m.dslName.c_str(), "controlOutputs");
    m.controlOutputTypes = signalTypeArrayFromVar(v.getProperty("controlOutputTypes", juce::var()));
    const auto signalInputs = v.getProperty("signalInputs", juce::var());
    if (! signalInputs.isVoid()) {
        if (! signalPortDeclarationsFromVar(signalInputs, m.signalInputs))
            return false;
        m.signalInputsExplicit = true;
    }
    const auto signalOutputs = v.getProperty("signalOutputs", juce::var());
    if (! signalOutputs.isVoid()) {
        if (! signalPortDeclarationsFromVar(signalOutputs, m.signalOutputs))
            return false;
        m.signalOutputsExplicit = true;
    }
    if (! m.signalInputsExplicit)
        m.signalInputs = legacySignalInputs(m);
    if (! m.signalOutputsExplicit)
        m.signalOutputs = legacySignalOutputs(m);

    if (m.lineageId == curlop::visual::kLineageId)
    {
        const auto schema = curlop::visual::parseSourceSchema(m.code);
        if (m.visualTarget != curlop::visual::kGraphBackgroundTarget
            || m.authoredPackageId != curlop::visual::kAuthoredPackageId
            || m.sourceUnitId != curlop::visual::kSourceUnitId
            || m.sourceKey != curlop::visual::kSourceKey
            || m.sourceLanguage != curlop::visual::kSourceLanguage
            || ! schema.ok()
            || m.params.size() != schema.params.size()
            || m.exposedParamInputs != schema.exposedParamInputs
            || m.audioInputs != schema.audioInputs
            || m.visualInputs != schema.visualInputs
            || m.visualOutputs != schema.visualOutputs
            || ! m.audioOutputs.empty()
            || m.paramValues.size() != schema.params.size())
            return false;
        for (size_t i = 0; i < schema.params.size(); ++i)
        {
            const auto& expected = schema.params[i];
            const auto& persisted = m.params[i];
            if (persisted.sourceId != expected.sourceId
                || persisted.name != expected.name
                || persisted.min != expected.min
                || persisted.max != expected.max
                || persisted.defaultValue != expected.defaultValue)
                return false;
            const auto value = m.paramValues.find(expected.name);
            if (value == m.paramValues.end() || ! std::isfinite(value->second)
                || value->second < expected.min || value->second > expected.max)
                return false;
        }
    }

    return true;
}

static bool edgeEntryFromVar(const juce::var& v, EdgeEntry& e)
{
    if (! v.isObject())
        return false;

    if (! readOptionalIntProperty(v, "srcIndex", -1, e.srcIndex)
        || ! readOptionalIntProperty(v, "tgtIndex", -1, e.tgtIndex))
        return false;

    if (! readOptionalStringProperty(v, "srcPort", e.srcPort)
        || ! readOptionalStringProperty(v, "tgtPort", e.tgtPort)
        || ! readOptionalStringProperty(v, "signalRole", e.signalRole))
        return false;
    const auto feedbackBoundary =
        v.getProperty("feedbackBoundary", juce::var()).toString();
    if (! feedbackBoundary.isEmpty()
        && feedbackBoundary != "none"
        && feedbackBoundary != "one-sample")
        return false;
    e.feedbackBoundary = feedbackBoundary == "one-sample"
        ? FeedbackBoundary::OneSample : FeedbackBoundary::None;
    const auto modulation = v.getProperty("modulation", juce::var());
    if (! modulation.isVoid()) {
        ModulationMapping parsed;
        std::string polarity;
        if (! modulation.isObject()
            || ! readRequiredStringProperty(modulation, "routeId", parsed.routeId)
            || ! readRequiredStringProperty(modulation, "sourceId", parsed.sourceId)
            || ! readRequiredStringProperty(modulation, "targetParamId", parsed.targetParamId)
            || ! readOptionalFloatProperty(modulation, "depth", 0.0f, parsed.depth)
            || ! readOptionalStringProperty(modulation, "polarity", polarity)
            || parsed.routeId.empty() || parsed.sourceId.empty() || parsed.targetParamId.empty()
            || ! std::isfinite(parsed.depth) || parsed.depth < -1.0f || parsed.depth > 1.0f
            || (polarity != "unipolar" && polarity != "bipolar"))
            return false;
        parsed.polarity = polarity == "bipolar"
            ? ModulationPolarity::Bipolar : ModulationPolarity::Unipolar;
        e.modulation = std::move(parsed);
    }
    const auto descriptor = v.getProperty("signalDescriptor", juce::var());
    if (! descriptor.isVoid()) {
        transport::SignalDescriptor parsed = transport::SignalDescriptor::legacyScalar();
        if (! signalDescriptorFromVar(descriptor, parsed))
            return false;
        e.signalDescriptor = std::move(parsed);
    }
    if (! readOptionalFloatProperty(v, "gain", 1.0f, e.gain)
        || ! readOptionalFloatProperty(v, "pan", 0.0f, e.pan)
        || ! readOptionalBoolProperty(v, "muted", false, e.muted)
        || ! readOptionalBoolProperty(v, "soloed", false, e.soloed))
        return false;

    auto presentation = v.getProperty("wirePresentation", juce::var());
    if (! presentation.isVoid()) {
        WirePresentation parsedPresentation;
        if (wirePresentationFromVar(presentation, parsedPresentation))
            e.presentation = std::move(parsedPresentation);
        else
            CDBG(PERSISTENCE, "graph import skipped malformed wire presentation src=%d tgt=%d",
                 e.srcIndex, e.tgtIndex);
    }
    return true;
}

static bool viewportFromVar(const juce::var& v, Viewport& vp)
{
    if (! v.isObject())
        return true;

    Viewport parsed = vp;
    if (! readOptionalFloatProperty(v, "x", 0.0f, parsed.x)
        || ! readOptionalFloatProperty(v, "y", 0.0f, parsed.y)
        || ! readOptionalFloatProperty(v, "zoom", 1.0f, parsed.zoom))
        return false;

    if (! std::isfinite(parsed.zoom) || parsed.zoom <= 0.0f)
        return false;

    parsed.zoom = juce::jlimit(0.05f, 50.0f, parsed.zoom);
    vp = parsed;
    return true;
}

static void reconcilePersistedGraphIndices(std::vector<ModuleEntry>& modules,
                                           std::vector<EdgeEntry>& edges)
{
    std::vector<int> persistedIndices;
    persistedIndices.reserve(modules.size());
    for (const auto& module : modules)
        persistedIndices.push_back(module.index);
    std::sort(persistedIndices.begin(), persistedIndices.end());

    std::unordered_map<int, int> canonical;
    canonical.reserve(persistedIndices.size());
    for (size_t i = 0; i < persistedIndices.size(); ++i)
        canonical.emplace(persistedIndices[i], static_cast<int>(i));

    for (auto& module : modules)
        module.index = canonical[module.index];
    edges.erase(std::remove_if(edges.begin(), edges.end(), [&](EdgeEntry& edge) {
        const auto src = canonical.find(edge.srcIndex);
        const auto tgt = canonical.find(edge.tgtIndex);
        if (src == canonical.end() || tgt == canonical.end())
            return true;
        edge.srcIndex = src->second;
        edge.tgtIndex = tgt->second;
        return false;
    }), edges.end());
}

bool ClipStateContainer::validatePersistedGraphVar(const juce::var& graph)
{
    if (! graph.isObject())
        return false;

    std::set<int> moduleIndices;
    std::set<std::string> moduleIds;
    std::unordered_map<int, ModuleEntry> parsedModules;
    GraphState visualContract;
    GraphState graphContract;
    const auto modules = graph.getProperty("modules", juce::var());
    if (! modules.isVoid() && ! modules.isArray())
        return false;
    if (modules.isArray()) {
        for (int i = 0; i < modules.size(); ++i) {
            ModuleEntry entry;
            if (! moduleEntryFromVar(modules[i], entry)
                || ! moduleIndices.insert(entry.index).second
                || ! moduleIds.insert(entry.moduleId).second
                || entry.dslName.empty())
                return false;
#if !CURLOP_ENABLE_LEGACY_APG_ORACLE
            if (entry.oversamplingFactor != ModuleOversamplingFactor::X1)
                return false;
#endif
            if (entry.lineageId == "core.visual_output"
                && visualContract.addModule(entry).empty())
                return false;
            if (entry.lineageId != "core.visual_output")
                visualContract.addModule(entry);
            graphContract.addModule(entry);
            parsedModules.emplace(entry.index, std::move(entry));
        }
    }

    const auto edges = graph.getProperty("edges", juce::var());
    if (! edges.isVoid() && ! edges.isArray())
        return false;
    if (edges.isArray()) {
        for (int i = 0; i < edges.size(); ++i) {
            EdgeEntry entry;
            if (! edgeEntryFromVar(edges[i], entry)
                || entry.srcIndex < 0
                || entry.tgtIndex < 0
                || moduleIndices.count(entry.srcIndex) == 0
                || moduleIndices.count(entry.tgtIndex) == 0)
                return false;
            if (entry.signalRole == "visual-texture"
                && ! visualContract.addEdge(entry))
                return false;
            if (! graphContract.addEdge(entry))
                return false;
            const auto target = parsedModules.find(entry.tgtIndex);
            if (target != parsedModules.end()
                && target->second.lineageId == curlop::visual::kLineageId)
            {
                const auto schema = curlop::visual::parseSourceSchema(target->second.code);
                const bool exactTarget = schema.ok()
                    && std::any_of(schema.bindings.begin(), schema.bindings.end(),
                        [&] (const curlop::visual::InputBinding& binding) {
                            return binding.surfaceId == entry.tgtPort;
                        });
                if (! exactTarget) return false;
            }
        }
    }

    const auto viewport = graph.getProperty("viewport", juce::var());
    if (! viewport.isVoid()) {
        Viewport parsed;
        if (! viewport.isObject() || ! viewportFromVar(viewport, parsed))
            return false;
    }
    return true;
}

static bool validatePersistedSignalDescriptors(const juce::var& graph)
{
    if (! graph.isObject()) return true;
    const auto modules = graph.getProperty("modules", juce::var());
    if (modules.isArray()) {
        for (const auto& module : *modules.getArray()) {
            for (const char* property : { "signalInputs", "signalOutputs" }) {
                const auto ports = module.getProperty(property, juce::var());
                if (ports.isVoid()) continue;
                std::vector<SignalPortDeclaration> parsed;
                if (! signalPortDeclarationsFromVar(ports, parsed))
                    return false;
            }
        }
    }
    const auto edges = graph.getProperty("edges", juce::var());
    if (edges.isArray()) {
        for (const auto& edge : *edges.getArray()) {
            const auto descriptor = edge.getProperty("signalDescriptor", juce::var());
            if (descriptor.isVoid()) continue;
            auto parsed = transport::SignalDescriptor::legacyScalar();
            if (! signalDescriptorFromVar(descriptor, parsed))
                return false;
        }
    }
    return true;
}

// F-078 (T-580) — graph-fragment clipboard (de)serialization. Reuses the same
// entry serializers as the save/load path (moduleEntryToVar/FromVar,
// edgeEntryToVar/FromVar) so a pasted module is byte-identical to a saved one
// and the two formats can never silently diverge.
juce::String ClipStateContainer::serializeFragment(const std::vector<ModuleEntry>& modules,
                                                   const std::vector<EdgeEntry>&   edgesLocal)
{
    juce::Array<juce::var> modsArr;
    modsArr.ensureStorageAllocated((int) modules.size());
    for (const auto& m : modules) modsArr.add(moduleEntryToVar(m));

    juce::Array<juce::var> edgesArr;
    edgesArr.ensureStorageAllocated((int) edgesLocal.size());
    for (const auto& e : edgesLocal) edgesArr.add(edgeEntryToVar(e));

    auto* env = new juce::DynamicObject();
    env->setProperty("curlop_fragment", kFragmentVersion);
    env->setProperty("modules", juce::var(modsArr));
    env->setProperty("edges",   juce::var(edgesArr));
    return juce::JSON::toString(juce::var(env));
}

bool ClipStateContainer::parseFragment(const juce::String& text,
                                      std::vector<ModuleEntry>& outModules,
                                      std::vector<EdgeEntry>&   outEdges)
{
    auto parsed = juce::JSON::parse(text);
    if (! parsed.isObject()) return false;

    // Marker + version gate — distinguishes a CURLOP fragment from arbitrary
    // clipboard text. Forward-compatible: an unknown future version is rejected
    // here rather than mis-parsed (paste is then a no-op, not a corruption).
    int ver = 0;
    if (! curlop::payload::readIntProperty(parsed, "curlop_fragment", ver))
        return false;
    if (ver < 1 || ver > kFragmentVersion) return false;

    auto modsVar  = parsed.getProperty("modules", juce::var());
    auto edgesVar = parsed.getProperty("edges",   juce::var());
    if (! modsVar.isArray()) return false;   // a fragment with no modules is meaningless

    std::vector<ModuleEntry> mods;
    mods.reserve((size_t) modsVar.size());
    for (int i = 0; i < modsVar.size(); ++i) {
        ModuleEntry entry;
        if (! moduleEntryFromVar(modsVar[i], entry)) {
            CDBG(PERSISTENCE, "parseFragment rejected malformed module row=%d", i);
            return false;
        }
        mods.push_back(std::move(entry));
    }
    if (mods.empty()) return false;

    std::vector<EdgeEntry> edges;
    if (edgesVar.isArray()) {
        edges.reserve((size_t) edgesVar.size());
        for (int i = 0; i < edgesVar.size(); ++i) {
            EdgeEntry entry;
            if (! edgeEntryFromVar(edgesVar[i], entry)) {
                if (edgesVar[i].hasProperty("signalDescriptor")) {
                    CDBG(PERSISTENCE,
                         "parseFragment rejected malformed signal descriptor row=%d", i);
                    return false;
                }
                CDBG(PERSISTENCE, "parseFragment skipped malformed legacy edge row=%d", i);
                continue;
            }
            if (! entry.signalDescriptor)
                entry.signalDescriptor = migrateLegacyEdgeDescriptor(entry, mods);
            edges.push_back(std::move(entry));
        }
    }

    outModules = std::move(mods);
    outEdges   = std::move(edges);
    return true;
}

// F-078 — whole-clip clipboard. A clip fragment is the same module/edge payload
// as a module fragment plus the clip name, under a distinct `curlop_clip` marker
// so clip-paste and module-paste never cross-trigger. (No bpm field — tempo
// lives in the core.script modules' @bpm, which travels in their `code`.)
juce::String ClipStateContainer::serializeClipFragment(const juce::String& name,
                                                      const std::vector<ModuleEntry>& modules,
                                                      const std::vector<EdgeEntry>&   edgesLocal)
{
    juce::Array<juce::var> modsArr;
    modsArr.ensureStorageAllocated((int) modules.size());
    for (const auto& m : modules) modsArr.add(moduleEntryToVar(m));

    juce::Array<juce::var> edgesArr;
    edgesArr.ensureStorageAllocated((int) edgesLocal.size());
    for (const auto& e : edgesLocal) edgesArr.add(edgeEntryToVar(e));

    auto* env = new juce::DynamicObject();
    env->setProperty("curlop_clip", kFragmentVersion);
    env->setProperty("name",    name);
    env->setProperty("modules", juce::var(modsArr));
    env->setProperty("edges",   juce::var(edgesArr));
    return juce::JSON::toString(juce::var(env));
}

bool ClipStateContainer::parseClipFragment(const juce::String& text,
                                          juce::String& outName,
                                          std::vector<ModuleEntry>& outModules,
                                          std::vector<EdgeEntry>&   outEdges)
{
    auto parsed = juce::JSON::parse(text);
    if (! parsed.isObject()) return false;

    int ver = 0;
    if (! curlop::payload::readIntProperty(parsed, "curlop_clip", ver))
        return false;
    if (ver < 1 || ver > kFragmentVersion) return false;

    auto modsVar  = parsed.getProperty("modules", juce::var());
    auto edgesVar = parsed.getProperty("edges",   juce::var());
    if (! modsVar.isArray()) return false;

    std::vector<ModuleEntry> mods;
    mods.reserve((size_t) modsVar.size());
    for (int i = 0; i < modsVar.size(); ++i) {
        ModuleEntry entry;
        if (! moduleEntryFromVar(modsVar[i], entry)) {
            CDBG(PERSISTENCE, "parseClipFragment rejected malformed module row=%d", i);
            return false;
        }
        mods.push_back(std::move(entry));
    }
    if (mods.empty()) return false;

    std::vector<EdgeEntry> edges;
    if (edgesVar.isArray()) {
        edges.reserve((size_t) edgesVar.size());
        for (int i = 0; i < edgesVar.size(); ++i) {
            EdgeEntry entry;
            if (! edgeEntryFromVar(edgesVar[i], entry)) {
                if (edgesVar[i].hasProperty("signalDescriptor")) {
                    CDBG(PERSISTENCE,
                         "parseClipFragment rejected malformed signal descriptor row=%d", i);
                    return false;
                }
                CDBG(PERSISTENCE, "parseClipFragment skipped malformed legacy edge row=%d", i);
                continue;
            }
            if (! entry.signalDescriptor)
                entry.signalDescriptor = migrateLegacyEdgeDescriptor(entry, mods);
            edges.push_back(std::move(entry));
        }
    }

    if (! readOptionalJuceStringProperty(parsed, "name", "Pasted Clip", outName)) {
        CDBG(PERSISTENCE, "parseClipFragment defaulted malformed clip name");
        outName = "Pasted Clip";
    }
    outModules = std::move(mods);
    outEdges   = std::move(edges);
    return true;
}

void ClipStateContainer::fromJson(const juce::String& json,
                                  GraphStateContainer& gc,
                                  bool canonicalizeGraphIndices)
{
    auto parsed = juce::JSON::parse(json);
    if (!parsed.isObject()) return;

    std::vector<ClipData> newClips;
    auto clipsVar = parsed.getProperty("clips", juce::var());
    // Descriptor/schema failures reject the complete project before either
    // clip or graph authority is mutated. Legacy payloads with no descriptor
    // remain valid and are migrated deterministically below.
    if (clipsVar.isArray()) {
        for (const auto& clipValue : *clipsVar.getArray()) {
            const auto graphValue = clipValue.getProperty("graph", juce::var());
            if (! graphValue.isVoid()
                && ! validatePersistedSignalDescriptors(graphValue)) {
                CDBG(PERSISTENCE,
                     "fromJson rejected project atomically: malformed graph descriptor");
                return;
            }
        }
    }
    int persistedViewedRow = 0;
    if (! readOptionalIntProperty(parsed, "viewedClipIndex", 0, persistedViewedRow)) {
        CDBG(PERSISTENCE, "fromJson defaulted malformed viewedClipIndex");
        persistedViewedRow = 0;
    }
    int persistedViewedId = -1;
    if (clipsVar.isArray()
        && persistedViewedRow >= 0
        && persistedViewedRow < clipsVar.size()) {
        int candidateId = -1;
        if (curlop::payload::readIntProperty(
                clipsVar[persistedViewedRow], "id", candidateId)
            && candidateId >= 0)
            persistedViewedId = candidateId;
    }
    if (clipsVar.isArray()) {
        for (int i = 0; i < clipsVar.size(); ++i) {
            auto& cv = clipsVar[i];
            ClipData cd;
            if (! curlop::payload::readIntProperty(cv, "id", cd.id) || cd.id < 0) {
                CDBG(PERSISTENCE, "fromJson skipped clip with malformed id row=%d", i);
                continue;
            }
            if (std::find_if(newClips.begin(), newClips.end(),
                             [&cd](const ClipData& existing) {
                                 return existing.id == cd.id;
                             }) != newClips.end()) {
                CDBG(PERSISTENCE,
                     "fromJson skipped duplicate clip id row=%d clipId=%d",
                     i, cd.id);
                continue;
            }
            if (! readOptionalJuceStringProperty(cv, "name", "", cd.name)) {
                CDBG(PERSISTENCE, "fromJson defaulted malformed clip name row=%d clipId=%d", i, cd.id);
                cd.name = "";
            }
            if (! readOptionalJuceStringProperty(cv, "colour", "", cd.colour)) {   // F-076 (absent → empty)
                CDBG(PERSISTENCE, "fromJson defaulted malformed clip colour row=%d clipId=%d", i, cd.id);
                cd.colour = "";
            }
            if (cv.hasProperty("script")) {
                cd.legacyScriptPresent = true;
                cd.legacyScriptPayload = cv.getProperty("script", juce::var());
            }

            auto graphVar = cv.getProperty("graph", juce::var());

            std::vector<ModuleEntry> modules;
            std::vector<EdgeEntry>   edges;
            Viewport vp{};
            if (graphVar.isObject()) {
                auto modsVar = graphVar.getProperty("modules", juce::var());
                if (modsVar.isArray())
                    for (int m = 0; m < modsVar.size(); ++m) {
                        ModuleEntry entry;
                        if (! moduleEntryFromVar(modsVar[m], entry)) {
                            CDBG(PERSISTENCE, "fromJson skipped malformed module row=%d clipId=%d", m, cd.id);
                            continue;
                        }
                        modules.push_back(std::move(entry));
                    }
                auto edgesVar = graphVar.getProperty("edges", juce::var());
                if (edgesVar.isArray())
                    for (int e = 0; e < edgesVar.size(); ++e) {
                        EdgeEntry entry;
                        if (! edgeEntryFromVar(edgesVar[e], entry)) {
                            CDBG(PERSISTENCE, "fromJson skipped malformed edge row=%d clipId=%d", e, cd.id);
                            continue;
                        }
                        if (! entry.signalDescriptor)
                            entry.signalDescriptor =
                                migrateLegacyEdgeDescriptor(entry, modules);
                        edges.push_back(std::move(entry));
                    }
                auto vpVar = graphVar.getProperty("viewport", juce::var());
                if (vpVar.isObject() && ! viewportFromVar(vpVar, vp))
                    CDBG(PERSISTENCE, "fromJson skipped malformed viewport clipId=%d", cd.id);
            }

            if (canonicalizeGraphIndices)
                reconcilePersistedGraphIndices(modules, edges);
            if (cd.legacyScriptPresent)
                CDBG(PERSISTENCE,
                     "fromJson preserved retired clips[].script payload for clipId=%d; Script V2 graph nodes are required",
                     cd.id);

            newClips.push_back(std::move(cd));

            gc.forClip(cd.id)
              .replace(std::move(modules), std::move(edges), vp);
        }
    }

    int viewedIdx = 0;
    if (persistedViewedId >= 0) {
        const auto viewedIt = std::find_if(
            newClips.begin(), newClips.end(),
            [persistedViewedId](const ClipData& clip) {
                return clip.id == persistedViewedId;
            });
        if (viewedIt != newClips.end())
            viewedIdx = (int) std::distance(newClips.begin(), viewedIt);
    }
    // T357-5: no `moduleRegistry` read — the envelope no longer carries one
    // (DRIFT-PL-2). A stale .curlop still bearing the key loads fine: an
    // unknown property is simply ignored here.
    juce::String projName;
    if (! readOptionalJuceStringProperty(parsed, "projectName", "Untitled Project", projName)) {
        CDBG(PERSISTENCE, "fromJson defaulted malformed projectName");
        projName = "Untitled Project";
    }
    int nextId = 1;
    if (! readOptionalIntProperty(parsed, "nextClipId", 1, nextId)) {
        CDBG(PERSISTENCE, "fromJson defaulted malformed nextClipId");
        nextId = 1;
    }

    // Re-derive nextClipId from max existing id if envelope's value is stale.
    int maxId = 0;
    for (const auto& c : newClips) maxId = juce::jmax(maxId, c.id);
    const int derivedNextId = maxId == std::numeric_limits<int>::max()
        ? maxId : maxId + 1;
    nextId = juce::jmax(nextId, derivedNextId);

    VisualSettings visualSettings;
    const auto visual = parsed.getProperty("visualSettings", juce::var());
    if (visual.isObject()) {
        (void) readOptionalBoolProperty(visual, "backgroundEnabled", true,
                                        visualSettings.backgroundEnabled);
        (void) readOptionalBoolProperty(visual, "windowEnabled", false,
                                        visualSettings.windowEnabled);
        (void) readOptionalIntProperty(visual, "displayIndex", 0,
                                       visualSettings.displayIndex);
        (void) readOptionalBoolProperty(visual, "fullscreen", false,
                                        visualSettings.fullscreen);
        (void) readOptionalIntProperty(visual, "width", 1280, visualSettings.width);
        (void) readOptionalIntProperty(visual, "height", 720, visualSettings.height);
        (void) readOptionalFloatProperty(visual, "resolutionScale", 1.0f,
                                         visualSettings.resolutionScale);
        (void) readOptionalFloatProperty(visual, "targetFps", 60.0f,
                                         visualSettings.targetFps);
        (void) readOptionalBoolProperty(visual, "captureEnabled", false,
                                        visualSettings.captureEnabled);
        (void) readOptionalJuceStringProperty(visual, "mode", "auto",
                                              visualSettings.mode);
    }
    replace(std::move(newClips), viewedIdx, projName, nextId);
    setVisualSettings(visualSettings);
}

// ═══════════════════════════════════════════════════════════════════════════
// Cross-project clip import (F-076) — generalises GraphStateContainer::
// duplicateClip to a foreign JSON source. Contract + dep-mode semantics in
// ClipStateContainer.h; design: clip-portability-cross-project-import-design.md.
//
// buildClipGraphFromPayload is the resolution core (parse + identity transforms
// + dependency resolution + graph write) shared by import and T-578 audition —
// "the same resolution path minus the append" (ADR ¶43). It touches only the
// GraphStateContainer at `targetClipId`; the clip-stack append (addClip + colour)
// lives in importClipFromPayload, which delegates here.
// ═══════════════════════════════════════════════════════════════════════════
ClipImportResult buildClipGraphFromPayload(const juce::var& clipJson,
                                           int targetClipId,
                                           GraphStateContainer& dstGraph,
                                           ImportDepMode depMode,
                                           const juce::File& srcProjectFolder,
                                           const juce::File& dstProjectFolder)
{
    ClipImportResult result;

    if (! clipJson.isObject()) return result;                  // ok=false, nothing written
    auto graphVar = clipJson.getProperty("graph", juce::var());
    auto modsVar  = graphVar.getProperty("modules", juce::var());
    if (! graphVar.isObject() || ! modsVar.isArray()) return result;

    result.newClipId = targetClipId;

    // 1. Parse foreign modules with the load-path parser (preserves dslName, code,
    //    params, voice policy, …); the import identity transforms layer on top.
    struct ParsedModuleRow {
        int sourceIndex = -1;
        ModuleEntry entry;
    };

    std::vector<ParsedModuleRow> parsed;
    parsed.reserve((size_t) modsVar.size());
    for (int i = 0; i < modsVar.size(); ++i) {
        ModuleEntry entry;
        if (! moduleEntryFromVar(modsVar[i], entry)) {
            result.warnings.push_back("Malformed module row " + juce::String(i) + " skipped.");
            CDBG(PERSISTENCE, "F-076 build-graph: skipped malformed module row=%d", i);
            continue;
        }
        parsed.push_back({ i, std::move(entry) });
    }
    if (parsed.empty()) {
        result.warnings.push_back("No parseable module rows.");
        CDBG(PERSISTENCE, "F-076 build-graph: rejected clip id=%d with no parseable modules",
             targetClipId);
        return result;
    }

    // 2. Per-module identity transforms + dependency resolution. Survivors keep
    //    source order; an unknown factory type is dropped (warning), while a
    //    retired precompiled Faust identity is retained for project recovery and
    //    rejected only when publication attempts to construct an audio path.
    //    The old→new index map remaps the index-keyed edges in step 3.
    std::vector<ModuleEntry> survivors;
    survivors.reserve(parsed.size());
    std::vector<int> oldToNew((size_t) modsVar.size(), -1);
    std::vector<std::pair<std::string, int>> pendingVersionImports;

    auto dslTaken = [&survivors](const std::string& n) {
        for (const auto& m : survivors) if (m.dslName == n) return true;
        return false;
    };

    for (const auto& row : parsed) {
        ModuleEntry m = row.entry;
        const bool authored = ! m.lineageUuid.empty();

        if (! authored) {
            // Factory module — its type must exist in this build's registry.
            if (curlop::factoryModuleSchema(m.lineageId) == nullptr
                && ! curlop::isRetiredPrecompiledFaustLineage(m.lineageId)) {
                result.warnings.push_back("Unknown module type '" + juce::String(m.lineageId)
                                          + "' (dslName '" + juce::String(m.dslName)
                                          + "') — skipped.");
                continue;                                    // oldToNew[i] stays -1
            }
            if (curlop::isRetiredPrecompiledFaustLineage(m.lineageId))
                result.warnings.push_back(
                    "Retired precompiled Faust module '" + juce::String(m.lineageId)
                    + "' was preserved for recovery but cannot publish audio; replace it with a source-backed Faust library module.");
        } else if (depMode == ImportDepMode::ClipPrivate) {
            m.lineageUuid.clear();                           // fork to fresh unversioned identity
            m.moduleVersion = 0;
            m.moduleStoreStatus.clear();
        } else {
            // MergeToLibrary: keep (designId, version); materialise the carried
            // version only after the complete graph candidate has been
            // admitted below. The embedded snapshot carries the sound
            // regardless, so a missing source is a warning, not failure.
            if (srcProjectFolder != juce::File() && dstProjectFolder != juce::File()
                && m.moduleVersion > 0)
                pendingVersionImports.emplace_back(m.lineageUuid, m.moduleVersion);
        }

        // dslName: preserve; disambiguate only on self-collision within the import.
        if (dslTaken(m.dslName)) {
            const std::string base = m.dslName;
            int suffix = 2;
            while (dslTaken(base + std::to_string(suffix))) ++suffix;
            m.dslName = base + std::to_string(suffix);
        }

        // moduleId: re-stamp fresh + unique (self-contained unit — B-255).
        m.moduleId = m.lineageId + "_c" + std::to_string(targetClipId)
                   + "_" + juce::Uuid().toDashedString().substring(0, 8).toStdString();

        if (! embeddedFaustSourceIsBuildable(m.lineageId, m.code)) {
            result.warnings.push_back("Embedded Faust source failed admission for '"
                                      + juce::String(m.dslName) + "'.");
            CDBG(PERSISTENCE,
                 "F-076 build-graph: rejected invalid embedded Faust source clip=%d module=%s",
                 targetClipId, m.dslName.c_str());
            return result;
        }

        oldToNew[(size_t) row.sourceIndex] = (int) survivors.size();
        m.index     = (int) survivors.size();                // contiguous 0..N-1
        survivors.push_back(std::move(m));
    }

    // 3. Edges (index-keyed): keep only edges whose BOTH endpoints survived; remap
    //    to the new contiguous indices. Viewport copies verbatim.
    std::vector<EdgeEntry> edges;
    auto edgesVar = graphVar.getProperty("edges", juce::var());
    if (edgesVar.isArray()) {
        for (int i = 0; i < edgesVar.size(); ++i) {
            EdgeEntry e;
            if (! edgeEntryFromVar(edgesVar[i], e)) {
                if (edgesVar[i].hasProperty("signalDescriptor")) {
                    result.warnings.push_back(
                        "Malformed signal descriptor in edge row "
                        + juce::String(i) + " rejected the import.");
                    CDBG(PERSISTENCE,
                         "F-076 build-graph: rejected malformed signal descriptor row=%d",
                         i);
                    return result;
                }
                result.warnings.push_back("Malformed edge row " + juce::String(i)
                                          + " skipped.");
                CDBG(PERSISTENCE, "F-076 build-graph: skipped malformed edge row=%d", i);
                continue;
            }
            if (e.srcIndex < 0 || e.srcIndex >= (int) oldToNew.size()) continue;
            if (e.tgtIndex < 0 || e.tgtIndex >= (int) oldToNew.size()) continue;
            const int ns = oldToNew[(size_t) e.srcIndex];
            const int nt = oldToNew[(size_t) e.tgtIndex];
            if (ns < 0 || nt < 0) continue;                  // an endpoint was skipped → drop
            e.srcIndex = ns;
            e.tgtIndex = nt;
            if (! e.signalDescriptor)
                e.signalDescriptor = migrateLegacyEdgeDescriptor(e, survivors);
            edges.push_back(e);
        }
    }

    Viewport vp{};
    if (! viewportFromVar(graphVar.getProperty("viewport", juce::var()), vp)) {
        result.warnings.push_back("Malformed viewport skipped.");
        CDBG(PERSISTENCE, "F-076 build-graph: skipped malformed viewport");
    }
    const int      modCount = (int) survivors.size();
    if (! dstGraph.forClip(targetClipId).replace(
            std::move(survivors), std::move(edges), vp)) {
        result.warnings.push_back("Graph edge contract rejected.");
        CDBG(PERSISTENCE,
             "F-076 build-graph: rejected invalid edge contract for clip id=%d",
             targetClipId);
        return result;
    }

    for (const auto& [uuid, version] : pendingVersionImports) {
        const auto outcome = modlib::importModuleVersionFromProject(
            srcProjectFolder, dstProjectFolder, uuid, version);
        if (outcome == modlib::ImportVersionOutcome::SourceMissing)
            result.warnings.push_back("A carried module v"
                + juce::String(version)
                + " source is not in the imported project — plays from its embedded snapshot.");
    }

    result.ok = true;
    CDBG(PERSISTENCE, "F-076 build-graph: clip id=%d (%d modules, %d warnings)",
         targetClipId, modCount, (int) result.warnings.size());
    return result;
}

ClipImportResult importClipFromPayload(const juce::var& clipJson,
                                       ClipStateContainer& dstClips,
                                       GraphStateContainer& dstGraph,
                                       ImportDepMode depMode,
                                       const juce::File& srcProjectFolder,
                                       const juce::File& dstProjectFolder)
{
    ClipImportResult result;

    // Validate the payload BEFORE allocating, so a bad payload leaves no orphan
    // clip in the stack (the build core re-validates harmlessly).
    if (! clipJson.isObject()) return result;                  // ok=false, nothing allocated
    auto graphVar = clipJson.getProperty("graph", juce::var());
    auto modsVar  = graphVar.getProperty("modules", juce::var());
    if (! graphVar.isObject() || ! modsVar.isArray()) return result;

    // Allocate the destination clipId (append — never collides, B-182); carry
    // name + colour (ADR ¶21). The foreign clipId is ignored.
    juce::String name;
    if (! readOptionalJuceStringProperty(clipJson, "name", "Imported Clip", name)) {
        CDBG(PERSISTENCE, "F-076 import defaulted malformed clip name");
        name = "Imported Clip";
    }
    if (name.isEmpty()) name = "Imported Clip";
    const int newClipId = dstClips.addClip(name);
    juce::String colour;
    if (! readOptionalJuceStringProperty(clipJson, "colour", "", colour)) {
        CDBG(PERSISTENCE, "F-076 import defaulted malformed clip colour");
        colour = "";
    }
    dstClips.setClipColour(newClipId, colour);

    result = buildClipGraphFromPayload(clipJson, newClipId, dstGraph,
                                       depMode, srcProjectFolder, dstProjectFolder);
    if (! result.ok) {
        dstClips.removeClip(newClipId);
        dstGraph.eraseClip(newClipId);
        result.newClipId = -1;
        return result;
    }
    result.newClipId = newClipId;   // echo the allocated id even if the graph was empty
    return result;
}

} // namespace curlop
