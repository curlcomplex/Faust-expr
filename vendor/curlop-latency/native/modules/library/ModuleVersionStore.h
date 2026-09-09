#pragma once
//
// ModuleVersionStore — T-364 (F-069 module library + versioning).
//
// The project-local version store for AUTHORED modules. Layout (per the
// rollout plan, f-069-module-library-versioning-rollout-plan.md):
//
//   <project>/modules/<lineageUuid>/
//       v1.fdsp  v1.bc        # Faust source (canonical) + compiled bytecode (cache)
//       v2.fdsp  v2.bc
//       manifest.json         # { lineageUuid, displayName, tier, currentPin, versions[] }
//
// The source .fdsp is the canonical, portable form — it recompiles on any
// backend. The bytecode (.bc LLVM bitcode on desktop / .fbc interpreter on iOS)
// is a per-backend reload cache copied from FaustRuntime's content-addressed
// store; it is best-effort and never load-bearing (absent → recompile from .fdsp).
//
// Pure file I/O, header-only, no engine state — so CurlopTests drives it offline
// and EventRouter (save-version) + ProjectPersistence (load-resolve) + the future
// library commit/recall paths all share one implementation. No app-driven git:
// these are plain files the user version-controls externally (Neo decision s501).
//
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>
#include "modules/backend/FaustSourceMetadata.h"
#include "modules/contract/ModuleTypes.h"
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace curlop { namespace modlib {

inline bool isSafeModuleLineagePathComponent(const std::string& lineageUuid)
{
    const juce::String id(lineageUuid);
    if (id.isEmpty() || id != id.trim())
        return false;
    if (id == "." || id == "..")
        return false;
    return ! id.containsChar('/') && ! id.containsChar('\\');
}

// <project>/modules/<lineageUuid>/
inline juce::File moduleDir(const juce::File& projectFolder,
                            const std::string& lineageUuid)
{
    auto root = projectFolder.getChildFile("modules");
    if (! isSafeModuleLineagePathComponent(lineageUuid))
        return root.getChildFile(".invalid-lineage");
    return root.getChildFile(juce::String(lineageUuid));
}

inline juce::File manifestFile(const juce::File& projectFolder,
                               const std::string& lineageUuid)
{
    return moduleDir(projectFolder, lineageUuid).getChildFile("manifest.json");
}

inline bool replaceManifestText(const juce::File& file, const juce::String& text)
{
    if (file.exists() && ! file.existsAsFile())
        return false;
    return file.replaceWithText(text);
}

inline bool readManifestIntValue(const juce::var& value, int& out)
{
    if (value.isVoid() || value.isUndefined() || value.isBool())
        return false;

    if (value.isInt() || value.isInt64()) {
        const auto parsed = static_cast<juce::int64>(value);
        if (parsed < std::numeric_limits<int>::min()
            || parsed > std::numeric_limits<int>::max())
            return false;
        out = static_cast<int>(parsed);
        return true;
    }

    if (value.isDouble()) {
        const double parsed = static_cast<double>(value);
        if (! std::isfinite(parsed)
            || std::floor(parsed) != parsed
            || parsed < static_cast<double>(std::numeric_limits<int>::min())
            || parsed > static_cast<double>(std::numeric_limits<int>::max()))
            return false;
        out = static_cast<int>(parsed);
        return true;
    }

    if (value.isString()) {
        const auto text = value.toString().trim().toStdString();
        if (text.empty())
            return false;

        errno = 0;
        char* end = nullptr;
        const long parsed = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != '\0' || errno == ERANGE
            || parsed < std::numeric_limits<int>::min()
            || parsed > std::numeric_limits<int>::max())
            return false;

        out = static_cast<int>(parsed);
        return true;
    }

    return false;
}

inline bool readManifestIntProperty(const juce::var& object, const char* name, int& out)
{
    return readManifestIntValue(object.getProperty(name, juce::var()), out);
}

inline int manifestIntProperty(const juce::var& object, const char* name, int fallback)
{
    int parsed = fallback;
    return readManifestIntProperty(object, name, parsed) ? parsed : fallback;
}

inline int nextVersionAfterManifestPin(const juce::var& manifest)
{
    int currentPin = 0;
    if (! readManifestIntProperty(manifest, "currentPin", currentPin))
        currentPin = 0;
    if (currentPin < 0 || currentPin >= std::numeric_limits<int>::max())
        return 0;
    return currentPin + 1;
}

inline bool hasLoadableModuleManifest(const juce::File& moduleDirectory);

// Highest saved version (== manifest currentPin), or 0 if the module has none.
inline int latestVersion(const juce::File& projectFolder,
                         const std::string& lineageUuid)
{
    const auto dir = moduleDir(projectFolder, lineageUuid);
    if (! hasLoadableModuleManifest(dir)) return 0;
    auto mf = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return 0;
    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    return manifestIntProperty(parsed, "currentPin", 0);
}

inline bool isSafeModuleSourceFileName(const juce::String& sourceName)
{
    if (sourceName.isEmpty() || sourceName != sourceName.trim())
        return false;
    if (sourceName == "." || sourceName == "..")
        return false;
    if (sourceName.containsChar('/') || sourceName.containsChar('\\'))
        return false;
    return sourceName.endsWithIgnoreCase(".fdsp");
}

inline bool isSafeModuleBytecodeFileName(const juce::String& fileName)
{
    if (fileName.isEmpty() || fileName != fileName.trim())
        return false;
    if (fileName == "." || fileName == "..")
        return false;
    if (fileName.containsChar('/') || fileName.containsChar('\\'))
        return false;
    return fileName.endsWithIgnoreCase(".bc") || fileName.endsWithIgnoreCase(".fbc");
}

inline juce::String bytecodeCacheFileNameForVersion(int version,
                                                    const juce::String& extension)
{
    if (version <= 0)
        return {};

    const auto ext = extension.toLowerCase();
    if (ext != ".bc" && ext != ".fbc")
        return {};

    const auto fileName = "v" + juce::String(version) + ext;
    return isSafeModuleBytecodeFileName(fileName) ? fileName : juce::String();
}

inline bool isBytecodeCacheFileNameForVersion(const juce::String& fileName,
                                              int version)
{
    if (! isSafeModuleBytecodeFileName(fileName))
        return false;

    return fileName.equalsIgnoreCase(bytecodeCacheFileNameForVersion(version, ".bc"))
        || fileName.equalsIgnoreCase(bytecodeCacheFileNameForVersion(version, ".fbc"));
}

inline juce::String sourceNameForVersionEntry(const juce::var& entry, int version)
{
    auto sourceName = entry.getProperty("source", "").toString();
    if (sourceName.isNotEmpty())
        return isSafeModuleSourceFileName(sourceName) ? sourceName : juce::String();
    return version > 0 ? "v" + juce::String(version) + ".fdsp" : juce::String();
}

inline juce::File sourceFileForVersionEntry(const juce::File& dir,
                                            const juce::var& entry,
                                            int version)
{
    const auto sourceName = sourceNameForVersionEntry(entry, version);
    return sourceName.isEmpty() ? juce::File() : dir.getChildFile(sourceName);
}

inline bool hasLoadableModuleManifest(const juce::File& moduleDirectory)
{
    auto mf = moduleDirectory.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return false;

    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    if (parsed.getDynamicObject() == nullptr) return false;

    const auto manifestLineage = parsed.getProperty("lineageUuid", "").toString();
    if (! isSafeModuleLineagePathComponent(manifestLineage.toStdString())
        || manifestLineage != moduleDirectory.getFileName())
        return false;

    int currentPin = 0;
    if (! readManifestIntProperty(parsed, "currentPin", currentPin) || currentPin <= 0)
        return false;

    auto* versions = parsed.getProperty("versions", juce::var()).getArray();
    if (versions == nullptr || versions->isEmpty())
        return false;

    for (const auto& entry : *versions)
    {
        int rowVersion = 0;
        if (! readManifestIntProperty(entry, "version", rowVersion) || rowVersion != currentPin)
            continue;

        const auto sourceFile = sourceFileForVersionEntry(moduleDirectory, entry, rowVersion);
        return sourceFile != juce::File() && sourceFile.existsAsFile();
    }

    return false;
}

inline bool hasModuleManifestForLineage(const juce::File& moduleDirectory)
{
    auto mf = moduleDirectory.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return false;

    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    if (parsed.getDynamicObject() == nullptr) return false;

    const auto manifestLineage = parsed.getProperty("lineageUuid", "").toString();
    return isSafeModuleLineagePathComponent(manifestLineage.toStdString())
        && manifestLineage == moduleDirectory.getFileName();
}

inline juce::var versionEntry(const juce::File& projectFolder,
                              const std::string& lineageUuid,
                              int version);

// Read a version's canonical source (.fdsp). Empty string on miss.
inline std::string readVersionSource(const juce::File& projectFolder,
                                     const std::string& lineageUuid,
                                     int version)
{
    const auto dir = moduleDir(projectFolder, lineageUuid);
    if (! hasModuleManifestForLineage(dir)) return {};
    const auto entry = versionEntry(projectFolder, lineageUuid, version);
    if (entry.getDynamicObject() == nullptr) return {};
    auto f = sourceFileForVersionEntry(dir, entry, version);
    return f.existsAsFile() ? f.loadFileAsString().toStdString() : std::string();
}

inline FaustSourceDisplayMetadata displayMetadataFromVersionEntry(const juce::File& dir,
                                                                  const juce::var& entry)
{
    const int version = manifestIntProperty(entry, "version", 0);
    auto sourceName = sourceNameForVersionEntry(entry, version);
    if (sourceName.isEmpty()) return {};
    auto f = sourceFileForVersionEntry(dir, entry, version);
    return f.existsAsFile()
        ? faustSourceDisplayMetadataFromSource(f.loadFileAsString().toStdString())
        : FaustSourceDisplayMetadata {};
}

// SF-077: the manifest build entry for a given version (void if absent). The
// entry caches build metadata + knob values written by writeVersion.
inline juce::var versionEntry(const juce::File& projectFolder,
                              const std::string& lineageUuid,
                              int version)
{
    const auto dir = moduleDir(projectFolder, lineageUuid);
    if (! hasModuleManifestForLineage(dir)) return {};
    auto mf = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return {};
    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    if (auto* arr = parsed.getProperty("versions", juce::var()).getArray())
        for (const auto& v : *arr) {
            int rowVersion = -1;
            if (readManifestIntProperty(v, "version", rowVersion) && rowVersion == version)
                return v;
        }
    return {};
}

// SF-077: the display/build name for a version, or "" if unnamed.
inline std::string readVersionName(const juce::File& projectFolder,
                                   const std::string& lineageUuid,
                                   int version)
{
    const auto entry = versionEntry(projectFolder, lineageUuid, version);
    const auto sourceMeta = displayMetadataFromVersionEntry(moduleDir(projectFolder, lineageUuid), entry);
    if (! sourceMeta.name.empty()) return sourceMeta.name;
    return entry.getProperty("name", "").toString().toStdString();
}

// SF-077: the {paramName: value} knob snapshot for a version (void if none).
inline juce::var readVersionKnobs(const juce::File& projectFolder,
                                  const std::string& lineageUuid,
                                  int version)
{
    return versionEntry(projectFolder, lineageUuid, version)
               .getProperty("knobs", juce::var());
}

inline juce::var knobsToManifestVar(const juce::var& knobs)
{
    if (knobs.isVoid() || knobs.isArray())
        return knobs;

    if (auto* obj = knobs.getDynamicObject()) {
        juce::Array<juce::var> arr;
        arr.ensureStorageAllocated(obj->getProperties().size());
        for (const auto& prop : obj->getProperties()) {
            juce::DynamicObject::Ptr entry = new juce::DynamicObject();
            entry->setProperty("name", prop.name.toString());
            entry->setProperty("value", prop.value);
            arr.add(juce::var(entry.get()));
        }
        return juce::var(arr);
    }

    return knobs;
}

inline juce::var knobSnapshotFromParamValues(
    const std::unordered_map<std::string, float>& paramValues)
{
    juce::DynamicObject::Ptr knobs = new juce::DynamicObject();
    for (const auto& [name, value] : paramValues)
        if (! name.empty() && std::isfinite(value))
            knobs->setProperty(juce::String(name), value);
    return juce::var(knobs.get());
}

template <typename Fn>
int forEachKnobValue(const juce::var& knobs, Fn&& fn)
{
    int count = 0;
    if (auto* arr = knobs.getArray()) {
        for (const auto& kv : *arr) {
            const auto name = kv.getProperty("name", "").toString();
            if (name.isEmpty())
                continue;
            fn(name, kv.getProperty("value", 0.0));
            ++count;
        }
        return count;
    }
    if (auto* obj = knobs.getDynamicObject()) {
        for (const auto& prop : obj->getProperties()) {
            fn(prop.name.toString(), prop.value);
            ++count;
        }
    }
    return count;
}

inline bool sourceAndKnobsMatchStoredVersion(
    const std::string& currentSource,
    const std::string& storedSource,
    const std::vector<ParamSchemaEntry>& schema,
    const std::unordered_map<std::string, float>& paramValues,
    const juce::var& storedKnobs)
{
    const auto normaliseLineEndings = [] (const std::string& source)
    {
        return juce::String(source)
            .replace("\r\n", "\n")
            .replace("\r", "\n")
            .toStdString();
    };
    if (storedSource.empty()
        || normaliseLineEndings(currentSource) != normaliseLineEndings(storedSource))
        return false;

    std::unordered_map<std::string, double> expected;
    expected.reserve(schema.size());
    for (const auto& parameter : schema)
        if (! parameter.name.empty())
            expected.emplace(parameter.name, parameter.defaultValue);

    bool valid = true;
    forEachKnobValue(storedKnobs,
        [&] (const juce::String& name, const juce::var& value)
        {
            double parsed = 0.0;
            const auto parameter = std::find_if(
                schema.begin(), schema.end(), [&name] (const auto& candidate)
                {
                    return candidate.name == name.toStdString();
                });
            if (parameter == schema.end()
                || value.isVoid() || value.isUndefined() || value.isBool())
            {
                valid = false;
                return;
            }
            if (value.isInt() || value.isInt64() || value.isDouble())
                parsed = static_cast<double>(value);
            else if (value.isString())
            {
                const auto text = value.toString().trim().toStdString();
                char* end = nullptr;
                errno = 0;
                parsed = std::strtod(text.c_str(), &end);
                if (text.empty() || end == text.c_str() || *end != '\0' || errno == ERANGE)
                {
                    valid = false;
                    return;
                }
            }
            else
            {
                valid = false;
                return;
            }
            if (! std::isfinite(parsed)
                || parsed < parameter->min || parsed > parameter->max)
            {
                valid = false;
                return;
            }
            expected[name.toStdString()] = parsed;
        });
    if (! valid)
        return false;

    for (const auto& [name, value] : expected)
    {
        const auto current = paramValues.find(name);
        const double currentValue = current == paramValues.end() ? value : current->second;
        if (! std::isfinite(currentValue) || std::abs(currentValue - value) > 0.000001)
            return false;
    }
    return true;
}

inline juce::StringArray exposedParamInputsFromEntry(const juce::var& entry)
{
    juce::StringArray out;
    auto v = entry.getProperty("exposedParamInputs", juce::var());
    if (v.isString()) {
        out.addLines(v.toString());
        return out;
    }
    if (auto* arr = v.getArray())
        for (const auto& name : *arr)
            if (auto s = name.toString(); s.isNotEmpty()) out.addIfNotAlreadyThere(s);
    return out;
}

inline bool entryHasExposedParamInputs(const juce::var& entry)
{
    return entry.hasProperty("exposedParamInputs");
}

inline juce::StringArray targetableParamInputsFromSchema(const std::vector<ParamSchemaEntry>& schema)
{
    juce::StringArray out;
    for (const auto& p : schema)
        if (!p.name.empty() && p.type != ParamType::Display)
            out.addIfNotAlreadyThere(juce::String(p.name));
    return out;
}

inline juce::StringArray exposedParamInputsForRecall(const juce::var& entry,
                                                     const std::vector<ParamSchemaEntry>& schema)
{
    juce::StringArray sourceNames;
    for (const auto& name : exposedParamInputsFromSchemaMetadata(schema))
        sourceNames.addIfNotAlreadyThere(juce::String(name));
    if (!sourceNames.isEmpty()) return sourceNames;

    if (entryHasExposedParamInputs(entry))
        return exposedParamInputsFromEntry(entry);

    return {};
}

inline juce::StringArray readVersionExposedParamInputs(const juce::File& projectFolder,
                                                       const std::string& lineageUuid,
                                                       int version)
{
    return exposedParamInputsFromEntry(versionEntry(projectFolder, lineageUuid, version));
}

inline juce::File canonicalUserLibraryRoot()
{
#if JUCE_IOS
    // iOS shares Documents with Files.app. Keep the canonical user library
    // visible there, under the same CURLOP/User Library spelling as desktop.
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
               .getChildFile("CURLOP")
               .getChildFile("User Library");
#else
    auto root = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    if (root.getFileName() != "Application Support")
        root = root.getChildFile("Application Support");
   #endif
    return root.getChildFile("CURLOP").getChildFile("User Library");
#endif
}

inline juce::File legacyDocumentsUserLibraryRoot()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
               .getChildFile("CURLOP")
               .getChildFile("user library");
}

inline juce::File legacyApplicationSupportUserLibraryRoot()
{
    auto root = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    if (root.getFileName() != "Application Support")
        root = root.getChildFile("Application Support");
   #endif
    return root.getChildFile("CURLOP").getChildFile("user library");
}

inline juce::File legacyIOSApplicationDataUserLibraryRoot()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("CURLOP")
               .getChildFile("User Library");
}

inline juce::File libraryModuleDir(const juce::File& libraryRoot,
                                   const std::string& lineageUuid)
{
    if (! isSafeModuleLineagePathComponent(lineageUuid))
        return libraryRoot.getChildFile(".invalid-lineage");
    return libraryRoot.getChildFile(juce::String(lineageUuid));
}

inline juce::var libraryVersionEntry(const juce::File& libraryRoot,
                                     const std::string& lineageUuid,
                                     int version)
{
    const auto dir = libraryModuleDir(libraryRoot, lineageUuid);
    if (! hasModuleManifestForLineage(dir)) return {};
    auto mf = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return {};
    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    if (auto* versions = parsed.getProperty("versions", juce::var()).getArray())
        for (const auto& v : *versions) {
            int rowVersion = -1;
            if (readManifestIntProperty(v, "version", rowVersion) && rowVersion == version)
                return v;
        }
    return {};
}

inline std::string readLibraryVersionName(const juce::File& libraryRoot,
                                          const std::string& lineageUuid,
    int version)
{
    const auto entry = libraryVersionEntry(libraryRoot, lineageUuid, version);
    const auto sourceMeta = displayMetadataFromVersionEntry(
        libraryModuleDir(libraryRoot, lineageUuid), entry);
    if (! sourceMeta.name.empty()) return sourceMeta.name;
    return entry.getProperty("name", "").toString().toStdString();
}

inline juce::StringArray readLibraryVersionExposedParamInputs(const juce::File& libraryRoot,
                                                              const std::string& lineageUuid,
                                                              int version)
{
    return exposedParamInputsFromEntry(libraryVersionEntry(libraryRoot, lineageUuid, version));
}

struct BuildListEntry
{
    int version = 0;
    juce::String name;
    juce::String category;
    juce::String description;
    juce::String author;
    juce::String sourceFingerprint;
    bool sourceAvailable = false;
};

inline std::vector<BuildListEntry> buildListForModuleDirectory(const juce::File& dir)
{
    std::vector<BuildListEntry> out;
    if (! hasModuleManifestForLineage(dir)) return out;
    auto mf = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return out;

    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    if (auto* versions = parsed.getProperty("versions", juce::var()).getArray())
    {
        out.reserve((size_t) versions->size());
        for (const auto& v : *versions)
        {
            const int version = manifestIntProperty(v, "version", 0);
            if (version <= 0) continue;
            const auto sourceMeta = displayMetadataFromVersionEntry(dir, v);
            const auto sourceFile = sourceFileForVersionEntry(dir, v, version);
            const auto source = sourceFile.existsAsFile()
                ? sourceFile.loadFileAsString() : juce::String();
            BuildListEntry entry;
            entry.version = version;
            entry.name = sourceMeta.name.empty()
                ? v.getProperty("name", "").toString() : juce::String(sourceMeta.name);
            entry.category = sourceMeta.category.empty()
                ? v.getProperty("category", "").toString() : juce::String(sourceMeta.category);
            entry.description = sourceMeta.description.empty()
                ? v.getProperty("description", "").toString() : juce::String(sourceMeta.description);
            entry.author = v.getProperty("author", "").toString();
            entry.sourceAvailable = source.isNotEmpty();
            if (entry.sourceAvailable)
                entry.sourceFingerprint = juce::String::toHexString(
                    static_cast<juce::uint64> (source.hashCode64()));
            out.push_back(std::move(entry));
        }
    }
    std::sort(out.begin(), out.end(), [] (const BuildListEntry& a, const BuildListEntry& b)
    {
        return a.version < b.version;
    });
    return out;
}

inline std::vector<BuildListEntry> libraryBuildList(const juce::File& libraryRoot,
                                                    const std::string& lineageUuid)
{
    return buildListForModuleDirectory(libraryModuleDir(libraryRoot, lineageUuid));
}

inline std::vector<BuildListEntry> projectBuildList(const juce::File& projectFolder,
                                                    const std::string& lineageUuid)
{
    return buildListForModuleDirectory(moduleDir(projectFolder, lineageUuid));
}

// SF-077: true if any saved build in the module's library list already uses
// `name` (case-insensitive). Kept for compatibility with older authoring UI.
inline bool libraryPresetNameExists(const juce::File& libraryRoot,
                                    const std::string& lineageUuid,
                                    const std::string& name)
{
    const auto dir = libraryModuleDir(libraryRoot, lineageUuid);
    if (! hasModuleManifestForLineage(dir)) return false;
    auto mf = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return false;
    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    if (auto* arr = parsed.getProperty("versions", juce::var()).getArray())
        for (const auto& v : *arr) {
            int rowVersion = 0;
            if (! readManifestIntProperty(v, "version", rowVersion) || rowVersion <= 0)
                continue;
            if (v.getProperty("name", "").toString().equalsIgnoreCase(juce::String(name)))
                return true;
        }
    return false;
}

// ── SF-077 authored builds ───────────────────────────────────────────────────
// A build = a full {Faust script + knob values} snapshot. Names + metadata live
// in the .fdsp `declare` header (portable: share the file and the authored
// display/category/author travel with it); the manifest caches them + knob
// values because Faust has no declare for runtime knob values.
struct PresetMeta
{
    std::string name;          // build/display name; "" → caller leaves manifest "name" empty
    std::string category;      // e.g. "Drums" — organizes modules in the browser
    std::string description;
    std::string author;        // Bitwig-style creator stamp (username)
    juce::var   knobs;         // object or fixed-key array; void = none captured
    juce::StringArray exposedParamInputs; // user-selected left-edge param sockets
    bool exposedParamInputsProvided = false; // true even when the explicit selection is empty
};

// Prepend Faust `declare` metadata lines to a source, stripping any existing
// leading name/author/category/description declares first so a re-save does not
// accumulate duplicates. Returns the source UNCHANGED when no metadata is set —
// keeps unnamed/legacy writes byte-identical (and the T-364 tests green).
inline std::string withDeclares(const std::string& source, const PresetMeta& meta)
{
    if (meta.name.empty() && meta.category.empty()
        && meta.description.empty() && meta.author.empty())
        return source;

    juce::StringArray kept;
    {
        juce::StringArray lines;
        lines.addLines(juce::String(source));
        for (const auto& ln : lines) {
            const auto t = ln.trim();
            if (t.startsWith("declare name ")    || t.startsWith("declare author ")
             || t.startsWith("declare category ")|| t.startsWith("declare description "))
                continue;
            kept.add(ln);
        }
    }
    auto esc = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            const auto uc = static_cast<unsigned char>(c);
            if (c == '\\') out += "\\\\";
            else if (c == '"') out += "\\\"";
            else if (uc < 0x20) out += ' ';
            else out += c;
        }
        return juce::String(out);
    };
    juce::String hdr;
    if (! meta.name.empty())        hdr << "declare name \""        << esc(meta.name)        << "\";\n";
    if (! meta.author.empty())      hdr << "declare author \""      << esc(meta.author)      << "\";\n";
    if (! meta.category.empty())    hdr << "declare category \""    << esc(meta.category)    << "\";\n";
    if (! meta.description.empty()) hdr << "declare description \"" << esc(meta.description) << "\";\n";
    return (hdr + kept.joinIntoString("\n")).toStdString();
}

// Append a new version: write v<N>.fdsp (source, with declare headers injected
// from `meta`), optionally copy the compiled bytecode (bytecodeSrc, content-
// addressed in FaustRuntime's store) to v<N>.<ext>, and rewrite manifest.json
// with currentPin = N. The manifest version entry caches build display name +
// metadata + knob values (SF-077). Returns N, or 0 on failure. bytecodeSrc may
// be a non-existent File — then no bytecode is stored (recompile from source on
// load), which is correct, just slower. `meta` defaults empty → an unnamed,
// knob-less version, byte-identical to the pre-SF-077 behavior.
inline int writeVersion(const juce::File& projectFolder,
                        const std::string& lineageUuid,
                        const std::string& displayName,
                        const std::string& source,
                        const juce::File& bytecodeSrc,
                        const PresetMeta& meta = {})
{
    if (! isSafeModuleLineagePathComponent(lineageUuid)) return 0;

    auto dir = moduleDir(projectFolder, lineageUuid);
    if (! dir.createDirectory()) return 0;

    auto mf = manifestFile(projectFolder, lineageUuid);
    juce::var prevManifest;
    if (mf.existsAsFile()) {
        if (! hasModuleManifestForLineage(dir)) return 0;
        prevManifest = juce::JSON::parse(mf.loadFileAsString());
    }
    const int next = nextVersionAfterManifestPin(prevManifest);
    if (next <= 0) return 0;

    auto srcFile = dir.getChildFile("v" + juce::String(next) + ".fdsp");
    if (! srcFile.replaceWithText(juce::String(withDeclares(source, meta)))) {
        srcFile.deleteFile();
        return 0;
    }
    juce::Array<juce::File> createdFiles;
    createdFiles.add(srcFile);

    juce::String bytecodeName;
    if (bytecodeSrc.existsAsFile()) {
        const auto dstName = bytecodeCacheFileNameForVersion(next, bytecodeSrc.getFileExtension());
        auto dst = dstName.isEmpty() ? juce::File() : dir.getChildFile(dstName);
        if (dst != juce::File() && bytecodeSrc.copyFileTo(dst)) {
            bytecodeName = dst.getFileName();
            createdFiles.add(dst);
        }
    }

    // Read-modify-write the manifest, preserving prior version entries.
    juce::Array<juce::var> versions;
    if (! prevManifest.isVoid()) {
        if (auto* prevArr = prevManifest.getProperty("versions", juce::var()).getArray())
            versions = *prevArr;
    }

    juce::DynamicObject::Ptr entry = new juce::DynamicObject();
    entry->setProperty("version", next);
    entry->setProperty("source",  srcFile.getFileName());
    if (bytecodeName.isNotEmpty())
        entry->setProperty("bytecode", bytecodeName);
    // SF-077: cache the build name + metadata + knob values on the version
    // entry. The .fdsp declare header is the portable source of truth for
    // name/category/description/author; this cache lets the strip + browser
    // read names without parsing every source, and is the only home for the
    // runtime knob values (Faust has no declare for them).
    if (! meta.name.empty())        entry->setProperty("name",        juce::String(meta.name));
    if (! meta.category.empty())    entry->setProperty("category",    juce::String(meta.category));
    if (! meta.description.empty()) entry->setProperty("description", juce::String(meta.description));
    if (! meta.author.empty())      entry->setProperty("author",      juce::String(meta.author));
    if (! meta.knobs.isVoid())      entry->setProperty("knobs",       knobsToManifestVar(meta.knobs));
    if (meta.exposedParamInputsProvided || ! meta.exposedParamInputs.isEmpty()) {
        juce::Array<juce::var> exposed;
        for (const auto& s : meta.exposedParamInputs) exposed.add(s);
        entry->setProperty("exposedParamInputs", juce::var(exposed));
    }
    versions.add(juce::var(entry.get()));

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("lineageUuid", juce::String(lineageUuid));
    root->setProperty("displayName", juce::String(displayName));
    root->setProperty("tier",        "draft");   // promoted to "released" on commit (T-505)
    root->setProperty("currentPin",  next);
    root->setProperty("versions",    versions);

    if (! replaceManifestText(mf, juce::JSON::toString(juce::var(root.get())))) {
        for (const auto& f : createdFiles)
            if (f.existsAsFile())
                f.deleteFile();
        return 0;
    }
    return next;
}

// SF-077: overwrite an EXISTING build (version) in place — its source (with
// declares), bytecode, and cached name/metadata/knobs — WITHOUT appending or
// bumping currentPin. Empty meta fields keep the existing cached
// value; a non-empty name also refreshes the module displayName. Returns true,
// or false if the manifest / version doesn't exist. Rebuilds the version array
// explicitly (no in-place var mutation through getProperty).
inline bool updateVersionInModuleDirectory(const juce::File& dir,
                                           const std::string& lineageUuid,
                                           int version,
                                           const std::string& source,
                                           const juce::File& bytecodeSrc,
                                           const PresetMeta& meta)
{
    if (! isSafeModuleLineagePathComponent(lineageUuid)) return false;

    auto mf = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return false;
    if (! hasModuleManifestForLineage(dir)) return false;
    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    auto* arr = parsed.getProperty("versions", juce::var()).getArray();
    if (arr == nullptr) return false;

    bool versionExists = false;
    juce::var existingVersionEntry;
    for (const auto& v : *arr) {
        int rowVersion = -1;
        if (readManifestIntProperty(v, "version", rowVersion) && rowVersion == version) {
            versionExists = true;
            existingVersionEntry = v;
            break;
        }
    }
    if (! versionExists) return false;

    auto srcFile = sourceFileForVersionEntry(dir, existingVersionEntry, version);
    if (srcFile == juce::File()) return false;
    const auto rewrittenSource = juce::String(withDeclares(source, meta));
    const bool oldSourceExisted = srcFile.existsAsFile();
    const auto oldSource = oldSourceExisted ? srcFile.loadFileAsString() : juce::String();
    const bool sourceChanged = !srcFile.existsAsFile()
        || srcFile.loadFileAsString() != rewrittenSource;
    if (! srcFile.replaceWithText(rewrittenSource)) return false;

    juce::String bytecodeName;
    juce::File copiedBytecodeFile;
    bool oldBytecodeExisted = false;
    juce::MemoryBlock oldBytecode;
    if (bytecodeSrc.existsAsFile()) {
        const auto dstName = bytecodeCacheFileNameForVersion(version, bytecodeSrc.getFileExtension());
        auto dst = dstName.isEmpty() ? juce::File() : dir.getChildFile(dstName);
        copiedBytecodeFile = dst;
        oldBytecodeExisted = dst.existsAsFile();
        if (oldBytecodeExisted)
            dst.loadFileAsData(oldBytecode);
        if (dst != juce::File() && bytecodeSrc.copyFileTo(dst)) bytecodeName = dst.getFileName();
    }

    // Carry a field from the prior entry when meta leaves it empty/void.
    auto keep = [] (juce::DynamicObject* e, const juce::var& prev, const char* key,
                    const juce::String& fresh) {
        if (fresh.isNotEmpty())            e->setProperty(key, fresh);
        else if (prev.hasProperty(key))    e->setProperty(key, prev.getProperty(key, {}));
    };

    bool found = false;
    juce::Array<juce::File> staleBytecodeFiles;
    juce::Array<juce::var> rebuilt;
    for (const auto& v : *arr) {
        int rowVersion = -1;
        if (! readManifestIntProperty(v, "version", rowVersion) || rowVersion != version) {
            rebuilt.add(v);
            continue;
        }
        found = true;
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty("version", version);
        e->setProperty("source",  srcFile.getFileName());
        if (bytecodeName.isNotEmpty())     e->setProperty("bytecode", bytecodeName);
        else if (!sourceChanged && v.hasProperty("bytecode")) {
            const auto oldBytecode = v.getProperty("bytecode", "").toString();
            if (isBytecodeCacheFileNameForVersion(oldBytecode, version))
                e->setProperty("bytecode", oldBytecode);
        }
        else {
            if (auto oldBytecode = v.getProperty("bytecode", "").toString(); oldBytecode.isNotEmpty())
                if (isBytecodeCacheFileNameForVersion(oldBytecode, version))
                    staleBytecodeFiles.add(dir.getChildFile(oldBytecode));
            staleBytecodeFiles.add(dir.getChildFile("v" + juce::String(version) + ".bc"));
            staleBytecodeFiles.add(dir.getChildFile("v" + juce::String(version) + ".fbc"));
        }
        keep (e.get(), v, "name",        juce::String(meta.name));
        keep (e.get(), v, "category",    juce::String(meta.category));
        keep (e.get(), v, "description", juce::String(meta.description));
        keep (e.get(), v, "author",      juce::String(meta.author));
        if (! meta.knobs.isVoid())     e->setProperty("knobs", knobsToManifestVar(meta.knobs));
        else if (v.hasProperty("knobs")) e->setProperty("knobs", v.getProperty("knobs", {}));
        if (meta.exposedParamInputsProvided || ! meta.exposedParamInputs.isEmpty()) {
            juce::Array<juce::var> exposed;
            for (const auto& s : meta.exposedParamInputs) exposed.add(s);
            e->setProperty("exposedParamInputs", exposed);
        } else if (v.hasProperty("exposedParamInputs"))
            e->setProperty("exposedParamInputs", v.getProperty("exposedParamInputs", {}));
        rebuilt.add(juce::var(e.get()));
    }
    if (! found) return false;

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("lineageUuid", juce::String(lineageUuid));
    root->setProperty("displayName", meta.name.empty()
                          ? parsed.getProperty("displayName", "")
                          : juce::var(juce::String(meta.name)));
    root->setProperty("tier",       parsed.getProperty("tier", "draft"));
    root->setProperty("currentPin", manifestIntProperty(parsed, "currentPin", version));
    root->setProperty("versions",   rebuilt);
    if (! replaceManifestText(mf, juce::JSON::toString(juce::var(root.get()))))
    {
        if (oldSourceExisted)
            srcFile.replaceWithText(oldSource);
        else if (srcFile.existsAsFile())
            srcFile.deleteFile();

        if (bytecodeName.isNotEmpty() && copiedBytecodeFile != juce::File()) {
            if (oldBytecodeExisted)
                copiedBytecodeFile.replaceWithData(oldBytecode.getData(), oldBytecode.getSize());
            else if (copiedBytecodeFile.existsAsFile())
                copiedBytecodeFile.deleteFile();
        }
        return false;
    }
    for (const auto& f : staleBytecodeFiles)
        if (f.existsAsFile()) f.deleteFile();
    return true;
}

inline bool updateVersion(const juce::File& projectFolder,
                          const std::string& lineageUuid,
                          int version,
                          const std::string& source,
                          const juce::File& bytecodeSrc,
                          const PresetMeta& meta)
{
    return updateVersionInModuleDirectory(moduleDir(projectFolder, lineageUuid),
                                          lineageUuid, version, source, bytecodeSrc, meta);
}

inline bool updateLibraryVersion(const juce::File& libraryRoot,
                                 const std::string& lineageUuid,
                                 int version,
                                 const std::string& source,
                                 const juce::File& bytecodeSrc,
                                 const PresetMeta& meta)
{
    return updateVersionInModuleDirectory(libraryModuleDir(libraryRoot, lineageUuid),
                                          lineageUuid, version, source, bytecodeSrc, meta);
}

// ── User library (commit target — T-505) ────────────────────────────────────
// ~/Library/Application Support/CURLOP/User Library/ — the cross-project module
// library. Runtime library reads must stay out of ~/Documents so macOS TCC does
// not block recall behind a Documents permission prompt. A "commit" promotes a
// project-local module (all its versions + manifest) up here and flips the
// manifest tier draft→released. Recall (T-506) reads it back.
struct LegacyMigrationReport
{
    bool legacyExists = false;
    int copied = 0;
    int skippedExisting = 0;
    int skippedInvalid = 0;
};

inline LegacyMigrationReport migrateLegacyUserLibrary(const juce::File& canonicalRoot,
                                                      const juce::File& legacyRoot)
{
    LegacyMigrationReport r;
    r.legacyExists = legacyRoot.isDirectory();
    if (! r.legacyExists) return r;

    canonicalRoot.createDirectory();
    for (const auto& dir : legacyRoot.findChildFiles(juce::File::findDirectories, false))
    {
        if (! isSafeModuleLineagePathComponent(dir.getFileName().toStdString())) {
            ++r.skippedInvalid;
            continue;
        }
        if (! hasLoadableModuleManifest(dir)) {
            ++r.skippedInvalid;
            continue;
        }
        const auto dst = canonicalRoot.getChildFile(dir.getFileName());
        if (dst.exists()) {
            ++r.skippedExisting;
            continue;
        }
        const auto suffix = juce::Uuid().toDashedString();
        auto staging = canonicalRoot.getChildFile("." + dir.getFileName() + ".migration-stage-" + suffix);
        staging.deleteRecursively();
        if (! dir.copyDirectoryTo(staging)) {
            staging.deleteRecursively();
            continue;
        }
        if (staging.moveFileTo(dst))
            ++r.copied;
        else
            staging.deleteRecursively();
    }
    return r;
}

inline void migrateLegacyUserLibraryIfNeeded(const juce::File& canonicalRoot)
{
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    migrateLegacyUserLibrary(canonicalRoot, legacyApplicationSupportUserLibraryRoot());
    migrateLegacyUserLibrary(canonicalRoot, legacyDocumentsUserLibraryRoot());
   #if JUCE_IOS
    // Builds before B-1810 used Library/CURLOP/User Library on iOS. Import it
    // once into the Files-visible canonical root without deleting either source.
    migrateLegacyUserLibrary(canonicalRoot, legacyIOSApplicationDataUserLibraryRoot());
   #endif
}

inline juce::File userLibraryRoot()
{
    auto root = canonicalUserLibraryRoot();
    migrateLegacyUserLibraryIfNeeded(root);
    return root;
}

// Copy <project>/modules/<uuid>/ into <libraryRoot>/<uuid>/ (overwriting a prior
// commit) and set the library manifest's tier to "released". libraryRoot is a
// parameter so CurlopTests can target a temp dir instead of ~/Documents.
// Returns false if the module has no project-local version dir.
inline bool commitToLibrary(const juce::File& projectFolder,
                            const juce::File& libraryRoot,
                            const std::string& lineageUuid)
{
    if (! isSafeModuleLineagePathComponent(lineageUuid)) return false;

    auto src = moduleDir(projectFolder, lineageUuid);
    if (! src.isDirectory()) return false;
    if (! hasLoadableModuleManifest(src)) return false;

    auto dst = libraryRoot.getChildFile(juce::String(lineageUuid));
    if (! libraryRoot.createDirectory()) return false;

    const auto suffix = juce::Uuid().toDashedString();
    auto staging = libraryRoot.getChildFile("." + juce::String(lineageUuid) + ".stage-" + suffix);
    auto backup = libraryRoot.getChildFile("." + juce::String(lineageUuid) + ".backup-" + suffix);
    staging.deleteRecursively();
    backup.deleteRecursively();

    if (! src.copyDirectoryTo(staging)) {
        staging.deleteRecursively();
        return false;
    }

    auto mf = staging.getChildFile("manifest.json");
    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    if (auto* obj = parsed.getDynamicObject()) {
        obj->setProperty("tier", "released");
        if (! replaceManifestText(mf, juce::JSON::toString(parsed))) {
            staging.deleteRecursively();
            return false;
        }
    } else {
        staging.deleteRecursively();
        return false;
    }

    if (dst.exists()) {
        if (! dst.moveFileTo(backup)) {
            staging.deleteRecursively();
            backup.deleteRecursively();
            return false;
        }
    }

    if (! staging.moveFileTo(dst)) {
        if (backup.exists())
            backup.moveFileTo(dst);
        staging.deleteRecursively();
        return false;
    }

    backup.deleteRecursively();
    return true;
}

struct CommitLibraryBuildResult
{
    bool published = false;
    int version = 0;
    juce::String sourceFingerprint;
    juce::File sourceFile;
};

inline CommitLibraryBuildResult commitLibraryBuildFromSource(const juce::File& libraryRoot,
                                                             const std::string& lineageUuid,
                                                             const std::string& displayName,
                                                             const std::string& source,
                                                             const juce::File& bytecodeSrc,
                                                             const PresetMeta& meta = {})
{
    CommitLibraryBuildResult r;
    if (! isSafeModuleLineagePathComponent(lineageUuid)) return r;

    auto dir = libraryModuleDir(libraryRoot, lineageUuid);
    if (! dir.createDirectory()) return r;

    auto mf = dir.getChildFile("manifest.json");
    juce::var prevManifest;
    if (mf.existsAsFile()) {
        if (! hasModuleManifestForLineage(dir)) return r;
        prevManifest = juce::JSON::parse(mf.loadFileAsString());
    }
    const int next = nextVersionAfterManifestPin(prevManifest);
    if (next <= 0) return r;
    auto srcFile = dir.getChildFile("v" + juce::String(next) + ".fdsp");
    const auto finalSource = withDeclares(source, meta);
    juce::Array<juce::File> createdFiles;
    {
        srcFile.deleteFile();
        juce::FileOutputStream out(srcFile);
        if (! out.openedOk()) {
            srcFile.deleteFile();
            return r;
        }
        if (! out.write(finalSource.data(), finalSource.size())) {
            srcFile.deleteFile();
            return r;
        }
        out.flush();
        createdFiles.add(srcFile);
    }

    juce::String bytecodeName;
    if (bytecodeSrc.existsAsFile()) {
        const auto dstName = bytecodeCacheFileNameForVersion(next, bytecodeSrc.getFileExtension());
        auto dst = dstName.isEmpty() ? juce::File() : dir.getChildFile(dstName);
        if (dst != juce::File() && bytecodeSrc.copyFileTo(dst)) {
            bytecodeName = dst.getFileName();
            createdFiles.add(dst);
        }
    }

    juce::Array<juce::var> versions;
    if (! prevManifest.isVoid()) {
        if (auto* prevArr = prevManifest.getProperty("versions", juce::var()).getArray())
            versions = *prevArr;
    }

    juce::DynamicObject::Ptr entry = new juce::DynamicObject();
    entry->setProperty("version", next);
    entry->setProperty("source", srcFile.getFileName());
    if (bytecodeName.isNotEmpty())      entry->setProperty("bytecode", bytecodeName);
    if (! meta.name.empty())            entry->setProperty("name", juce::String(meta.name));
    if (! meta.category.empty())        entry->setProperty("category", juce::String(meta.category));
    if (! meta.description.empty())     entry->setProperty("description", juce::String(meta.description));
    if (! meta.author.empty())          entry->setProperty("author", juce::String(meta.author));
    if (! meta.knobs.isVoid())          entry->setProperty("knobs", knobsToManifestVar(meta.knobs));
    if (meta.exposedParamInputsProvided || ! meta.exposedParamInputs.isEmpty()) {
        juce::Array<juce::var> exposed;
        for (const auto& s : meta.exposedParamInputs) exposed.add(s);
        entry->setProperty("exposedParamInputs", juce::var(exposed));
    }
    versions.add(juce::var(entry.get()));

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("lineageUuid", juce::String(lineageUuid));
    root->setProperty("displayName", juce::String(displayName));
    root->setProperty("tier", "released");
    root->setProperty("currentPin", next);
    root->setProperty("versions", versions);
    if (! replaceManifestText(mf, juce::JSON::toString(juce::var(root.get())))) {
        for (const auto& f : createdFiles)
            if (f.existsAsFile())
                f.deleteFile();
        return r;
    }

    r.published = true;
    r.version = next;
    r.sourceFile = srcFile;
    r.sourceFingerprint = juce::String::toHexString((juce::uint64) juce::String(finalSource).hashCode64());
    return r;
}

inline int libraryCurrentPin(const juce::File& libraryRoot,
                             const std::string& lineageUuid)
{
    const auto dir = libraryModuleDir(libraryRoot, lineageUuid);
    if (! hasLoadableModuleManifest(dir)) return 0;
    auto mf = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return 0;
    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    return manifestIntProperty(parsed, "currentPin", 0);
}

inline std::string readLibraryVersionSource(const juce::File& libraryRoot,
                                            const std::string& lineageUuid,
                                            int version)
{
    if (version <= 0) return {};
    const auto dir = libraryModuleDir(libraryRoot, lineageUuid);
    if (! hasModuleManifestForLineage(dir)) return {};
    const auto entry = libraryVersionEntry(libraryRoot, lineageUuid, version);
    if (entry.getDynamicObject() == nullptr) return {};
    auto f = sourceFileForVersionEntry(dir, entry, version);
    return f.existsAsFile() ? f.loadFileAsString().toStdString() : std::string();
}

// Read-only lookup for admission paths: callers supply every root explicitly so
// checking legacy content never triggers a library migration.
inline std::string readLibraryVersionSourceFromRoots(
    const juce::File& canonicalRoot,
    const std::vector<juce::File>& legacyRoots,
    const std::string& lineageUuid,
    int version)
{
    auto source = readLibraryVersionSource(canonicalRoot, lineageUuid, version);
    if (!source.empty()) return source;
    for (const auto& root : legacyRoots) {
        source = readLibraryVersionSource(root, lineageUuid, version);
        if (!source.empty()) return source;
    }
    return {};
}

inline void copyIfPresent(juce::DynamicObject& dst, const juce::var& src, const char* key)
{
    if (src.hasProperty(key))
        dst.setProperty(key, src.getProperty(key, {}));
}

inline juce::String moduleStoreFingerprintForDir(const juce::File& dir)
{
    if (! hasLoadableModuleManifest(dir)) return {};

    auto mf = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return {};

    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    auto* versions = parsed.getProperty("versions", juce::var()).getArray();
    if (versions == nullptr) return {};

    juce::Array<juce::var> canonicalVersions;
    juce::String sources;

    for (const auto& v : *versions) {
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        copyIfPresent(*e, v, "version");
        copyIfPresent(*e, v, "source");
        copyIfPresent(*e, v, "name");
        copyIfPresent(*e, v, "category");
        copyIfPresent(*e, v, "description");
        copyIfPresent(*e, v, "author");
        copyIfPresent(*e, v, "knobs");
        copyIfPresent(*e, v, "exposedParamInputs");
        canonicalVersions.add(juce::var(e.get()));

        const int version = manifestIntProperty(v, "version", 0);
        auto sourceName = sourceNameForVersionEntry(v, version);
        sources << "\n@@source:" << sourceName << "\n";
        const auto sourceFile = sourceFileForVersionEntry(dir, v, version);
        sources << (sourceFile.existsAsFile() ? sourceFile.loadFileAsString()
                                              : juce::String("<missing>"));
    }

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    copyIfPresent(*root, parsed, "lineageUuid");
    copyIfPresent(*root, parsed, "displayName");
    copyIfPresent(*root, parsed, "category");
    copyIfPresent(*root, parsed, "description");
    copyIfPresent(*root, parsed, "currentPin");
    root->setProperty("versions", juce::var(canonicalVersions));

    const auto canonical = juce::JSON::toString(juce::var(root.get())) + sources;
    return juce::String::toHexString((juce::uint64) canonical.hashCode64());
}

inline juce::String projectModuleStoreFingerprint(const juce::File& projectFolder,
                                                  const std::string& lineageUuid)
{
    return moduleStoreFingerprintForDir(moduleDir(projectFolder, lineageUuid));
}

inline juce::String libraryModuleStoreFingerprint(const juce::File& libraryRoot,
                                                  const std::string& lineageUuid)
{
    return moduleStoreFingerprintForDir(libraryModuleDir(libraryRoot, lineageUuid));
}

inline bool moduleStoresEquivalent(const juce::File& projectFolder,
                                   const juce::File& libraryRoot,
                                   const std::string& lineageUuid)
{
    const auto projectHash = projectModuleStoreFingerprint(projectFolder, lineageUuid);
    const auto libraryHash = libraryModuleStoreFingerprint(libraryRoot, lineageUuid);
    return projectHash.isNotEmpty() && projectHash == libraryHash;
}

struct ModuleStoreStatus
{
    juce::String state;       // session | project | library | diverged
    int projectPin = -1;
    int libraryPin = -1;
    juce::String projectHash;
    juce::String libraryHash;
};

inline ModuleStoreStatus moduleStoreStatus(const juce::File& projectFolder,
                                           const juce::File& libraryRoot,
                                           const std::string& lineageUuid)
{
    ModuleStoreStatus s;
    if (lineageUuid.empty()) {
        s.state = "session";
        return s;
    }

    s.projectHash = projectFolder == juce::File()
        ? juce::String()
        : projectModuleStoreFingerprint(projectFolder, lineageUuid);
    s.libraryHash = libraryModuleStoreFingerprint(libraryRoot, lineageUuid);
    s.projectPin = projectFolder == juce::File() ? -1 : latestVersion(projectFolder, lineageUuid);
    s.libraryPin = libraryCurrentPin(libraryRoot, lineageUuid);

    if (s.projectHash.isNotEmpty() && s.libraryHash.isNotEmpty())
        s.state = (s.projectHash == s.libraryHash) ? "library" : "diverged";
    else if (s.projectHash.isNotEmpty())
        s.state = "project";
    else if (s.libraryHash.isNotEmpty())
        s.state = "library";
    else
        s.state = "session";
    return s;
}

struct RecallResolution
{
    int projectPin = -1;
    int libraryPin = -1;
    bool copiedFromLibrary = false;
    bool useLibraryRuntime = false;

    int runtimePin() const { return useLibraryRuntime ? libraryPin : projectPin; }
};

inline RecallResolution recallFromLibraryResolving(const juce::File& libraryRoot,
                                                   const juce::File& projectFolder,
                                                   const std::string& lineageUuid)
{
    RecallResolution r;
    if (! isSafeModuleLineagePathComponent(lineageUuid)) return r;

    auto libDir = libraryModuleDir(libraryRoot, lineageUuid);
    if (! libDir.isDirectory()) return r;
    if (! hasLoadableModuleManifest(libDir)) return r;

    auto projDir = moduleDir(projectFolder, lineageUuid);
    const bool alreadyLocal =
        projDir.isDirectory() && manifestFile(projectFolder, lineageUuid).existsAsFile();
    if (! alreadyLocal) {
        auto modulesRoot = projectFolder.getChildFile("modules");
        if (! modulesRoot.createDirectory()) return r;

        const auto suffix = juce::Uuid().toDashedString();
        auto staging = modulesRoot.getChildFile("." + juce::String(lineageUuid) + ".recall-stage-" + suffix);
        auto backup = modulesRoot.getChildFile("." + juce::String(lineageUuid) + ".recall-backup-" + suffix);
        staging.deleteRecursively();
        backup.deleteRecursively();

        if (! libDir.copyDirectoryTo(staging)) {
            staging.deleteRecursively();
            return r;
        }

        if (projDir.exists()) {
            if (! projDir.moveFileTo(backup)) {
                staging.deleteRecursively();
                backup.deleteRecursively();
                return r;
            }
        }

        if (! staging.moveFileTo(projDir)) {
            if (backup.exists())
                backup.moveFileTo(projDir);
            staging.deleteRecursively();
            return r;
        }

        backup.deleteRecursively();
        r.copiedFromLibrary = true;
    }

    r.projectPin = latestVersion(projectFolder, lineageUuid);
    r.libraryPin = libraryCurrentPin(libraryRoot, lineageUuid);
    r.useLibraryRuntime = moduleStoresEquivalent(projectFolder, libraryRoot, lineageUuid);
    return r;
}

// Recall a committed module from the library into a project (T-506).
// <libraryRoot>/<uuid>/ → <project>/modules/<uuid>/. If the project already has
// the module, leaves it untouched (no clobber of local edits). Returns the
// recalled currentPin, or -1 if the library has no such module.
inline int recallFromLibrary(const juce::File& libraryRoot,
                             const juce::File& projectFolder,
                             const std::string& lineageUuid)
{
    return recallFromLibraryResolving(libraryRoot, projectFolder, lineageUuid).projectPin;
}

// ── Library enumeration (T-515 browser library view) ────────────────────────
// One committed library module, hydrated from its manifest.json. The browser's
// Library mode lists these as cards; drag/recall keys off lineageUuid.
struct LibraryModuleInfo
{
    std::string lineageUuid;   // dir name + manifest lineageUuid (the recall key)
    std::string displayName;   // manifest displayName (falls back to uuid)
    int         version = 0;   // manifest currentPin (latest)
    std::string tier;          // "released" (committed) | "draft"
    std::string category;
    std::string description;
    bool metadataOverride = false;
};

// Enumerate every committed module under libraryRoot — scan for <uuid>/ dirs and
// read each manifest.json. libraryRoot is a parameter so CurlopTests can target a
// temp dir; production passes userLibraryRoot(). Empty when the library is absent
// or empty. Skips dirs without a manifest (partial / in-flight copies).
inline std::vector<LibraryModuleInfo> listLibraryModules(const juce::File& libraryRoot)
{
    std::vector<LibraryModuleInfo> out;
    std::set<juce::String> seen;
    const auto& root = libraryRoot;
    if (root.isDirectory())
    {
        for (const auto& dir : root.findChildFiles(juce::File::findDirectories, false))
        {
            auto mf = dir.getChildFile("manifest.json");
            if (! mf.existsAsFile()) continue;
            if (! hasLoadableModuleManifest(dir)) continue;
            auto parsed = juce::JSON::parse(mf.loadFileAsString());

            const auto lineage = parsed.getProperty("lineageUuid", dir.getFileName()).toString();
            if (! isSafeModuleLineagePathComponent(lineage.toStdString())) continue;
            if (lineage.isEmpty() || seen.count(lineage) > 0) continue;
            seen.insert(lineage);

            LibraryModuleInfo info;
            info.lineageUuid = lineage.toStdString();
            info.displayName = parsed.getProperty("displayName", "").toString().toStdString();
            info.version     = manifestIntProperty(parsed, "currentPin", 0);
            info.tier        = parsed.getProperty("tier", "draft").toString().toStdString();
            info.category    = parsed.getProperty("category", "").toString().toStdString();
            info.description = parsed.getProperty("description", "").toString().toStdString();
            info.metadataOverride = static_cast<bool>(parsed.getProperty("metadataOverride", false));
            const auto entry = libraryVersionEntry(root, info.lineageUuid, info.version);
            const auto sourceMeta = displayMetadataFromVersionEntry(dir, entry);
            if (! info.metadataOverride) {
                if (! sourceMeta.name.empty())        info.displayName = sourceMeta.name;
                if (! sourceMeta.category.empty())    info.category = sourceMeta.category;
                if (! sourceMeta.description.empty()) info.description = sourceMeta.description;
                if (info.category.empty())    info.category = entry.getProperty("category", "").toString().toStdString();
                if (info.description.empty()) info.description = entry.getProperty("description", "").toString().toStdString();
            }
            if (info.displayName.empty()) info.displayName = info.lineageUuid;
            out.push_back(std::move(info));
        }
    }
    return out;
}

// The display name recorded in a module's manifest (for the recalled instance's
// dslName / label). Empty if no manifest.
inline std::string manifestDisplayName(const juce::File& projectFolder,
                                       const std::string& lineageUuid)
{
    const auto dir = moduleDir(projectFolder, lineageUuid);
    if (! hasLoadableModuleManifest(dir)) return {};
    auto mf = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return {};
    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    return parsed.getProperty("displayName", "").toString().toStdString();
}

// ── Library module rename / delete (SF-077 browser context menu) ─────────────
// The module's library-level metadata, for pre-filling the rename dialog. The
// root-level category/description are written by renameLibraryModule (empty for
// modules saved before that — fall back to the current build's per-version copy).
struct LibraryModuleMeta { std::string name, category, description; };

inline LibraryModuleMeta readLibraryModuleMeta(const juce::File& libraryRoot,
                                               const std::string& lineageUuid)
{
    LibraryModuleMeta m;
    auto dir = libraryModuleDir(libraryRoot, lineageUuid);
    if (! hasLoadableModuleManifest(dir)) return m;
    auto mf = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return m;
    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    const bool metadataOverride = static_cast<bool>(parsed.getProperty("metadataOverride", false));
    m.name        = parsed.getProperty("displayName", "").toString().toStdString();
    m.category    = parsed.getProperty("category", "").toString().toStdString();
    m.description = parsed.getProperty("description", "").toString().toStdString();
    // Fall back to the current build's metadata for modules saved before the
    // module-level fields existed.
    if (! metadataOverride && (m.category.empty() || m.description.empty())) {
        const int pin = manifestIntProperty(parsed, "currentPin", 0);
        if (auto* arr = parsed.getProperty("versions", juce::var()).getArray())
            for (const auto& v : *arr) {
                int rowVersion = -1;
                if (readManifestIntProperty(v, "version", rowVersion) && rowVersion == pin) {
                    if (m.category.empty())    m.category    = v.getProperty("category", "").toString().toStdString();
                    if (m.description.empty()) m.description = v.getProperty("description", "").toString().toStdString();
                    break;
                }
            }
    }
    if (! metadataOverride) {
        const int pin = manifestIntProperty(parsed, "currentPin", 0);
        const auto entry = libraryVersionEntry(libraryRoot, lineageUuid, pin);
        const auto sourceMeta = displayMetadataFromVersionEntry(dir, entry);
        if (! sourceMeta.name.empty())        m.name = sourceMeta.name;
        if (! sourceMeta.category.empty())    m.category = sourceMeta.category;
        if (! sourceMeta.description.empty()) m.description = sourceMeta.description;
    }
    return m;
}

// Read a committed library module's current source directly from the library
// (versions[].source, or v<currentPin>.fdsp for legacy rows) — for recalling into an UNSAVED
// project, where there's no project folder to copy the store into. The module
// loads with this source embedded; the project-local store is created later, on
// the first save-version. Empty on miss; outPin receives the pinned version.
inline std::string readLibraryModuleSource(const juce::File& libraryRoot,
                                           const std::string& lineageUuid,
                                           int& outPin)
{
    outPin = 0;
    auto dir = libraryModuleDir(libraryRoot, lineageUuid);
    if (! hasLoadableModuleManifest(dir)) return {};
    auto mf  = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return {};
    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    outPin = manifestIntProperty(parsed, "currentPin", 0);
    if (outPin <= 0) return {};
    auto f = sourceFileForVersionEntry(dir, libraryVersionEntry(libraryRoot, lineageUuid, outPin), outPin);
    return f.existsAsFile() ? f.loadFileAsString().toStdString() : std::string();
}

struct LibraryBuildSource
{
    std::string source;
    int version = 0;
    juce::String sourceFingerprint;
    juce::File sourceFile;
};

inline LibraryBuildSource readLibraryBuildForBrowserInsert(const juce::File& libraryRoot,
                                                           const std::string& lineageUuid,
                                                           int requestedVersion = 0)
{
    LibraryBuildSource r;
    int pin = requestedVersion;
    if (requestedVersion > 0)
        r.source = readLibraryVersionSource(libraryRoot, lineageUuid, requestedVersion);
    else
        r.source = readLibraryModuleSource(libraryRoot, lineageUuid, pin);
    r.version = pin;
    if (! r.source.empty()) {
        r.sourceFingerprint = juce::String::toHexString(
            (juce::uint64) juce::String(r.source).hashCode64());
        const auto dir = libraryModuleDir(libraryRoot, lineageUuid);
        r.sourceFile = sourceFileForVersionEntry(dir, libraryVersionEntry(libraryRoot, lineageUuid, pin), pin);
    }
    return r;
}

// Rename a library module + edit its module-level metadata in place (manifest
// root displayName/category/description). Empty fields are left unchanged.
// Returns false if the module has no manifest.
inline juce::String libraryManifestAuthorityToken(const juce::File& libraryRoot,
                                                  const std::string& lineageUuid)
{
    if (! isSafeModuleLineagePathComponent(lineageUuid)) return {};
    const auto file = libraryModuleDir(libraryRoot, lineageUuid).getChildFile("manifest.json");
    return file.existsAsFile() ? juce::String::toHexString((juce::uint64) file.loadFileAsString().hashCode64())
                               : juce::String();
}

inline bool renameLibraryModule(const juce::File& libraryRoot,
                                const std::string& lineageUuid,
                                const std::string& name,
                                const std::string& category,
                                const std::string& description,
                                int expectedPin = 0,
                                const juce::String& expectedAuthorityToken = {})
{
    if (! isSafeModuleLineagePathComponent(lineageUuid)) return false;

    auto dir = libraryRoot.getChildFile(juce::String(lineageUuid));
    if (! hasLoadableModuleManifest(dir)) return false;
    auto mf = dir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return false;
    if (expectedAuthorityToken.isNotEmpty()
        && libraryManifestAuthorityToken(libraryRoot, lineageUuid) != expectedAuthorityToken) return false;

    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr) return false;
    const int pin = manifestIntProperty(parsed, "currentPin", 0);
    if (expectedPin > 0 && pin != expectedPin) return false;
    if (name.empty()) return false;

    const juce::String finalName(name), finalCategory(category), finalDescription(description);

    obj->setProperty("displayName", finalName);
    obj->setProperty("category", finalCategory);
    obj->setProperty("description", finalDescription);
    obj->setProperty("metadataOverride", true);

    if (auto* versions = parsed.getProperty("versions", juce::var()).getArray())
        for (auto& v : *versions) {
            int rowVersion = -1;
            if (readManifestIntProperty(v, "version", rowVersion) && rowVersion == pin) {
                if (auto* ve = v.getDynamicObject()) {
                    ve->setProperty("name", finalName);
                    ve->setProperty("category", finalCategory);
                    ve->setProperty("description", finalDescription);
                }
                break;
            }
        }
    return replaceManifestText(mf, juce::JSON::toString(parsed));
}

// Delete a committed module from the library (the whole <uuid>/ dir). Returns
// false if it wasn't there. Project-local copies + in-graph instances are
// untouched — this only removes the shared library artifact.
inline bool deleteLibraryModule(const juce::File& libraryRoot,
                                const std::string& lineageUuid,
                                int expectedPin = 0,
                                const juce::String& expectedAuthorityToken = {})
{
    if (! isSafeModuleLineagePathComponent(lineageUuid)) return false;

    auto dir = libraryRoot.getChildFile(juce::String(lineageUuid));
    if (expectedAuthorityToken.isNotEmpty()
        && libraryManifestAuthorityToken(libraryRoot, lineageUuid) != expectedAuthorityToken) return false;
    if (expectedPin > 0) {
        const auto manifest = juce::JSON::parse(dir.getChildFile("manifest.json").loadFileAsString());
        if (manifestIntProperty(manifest, "currentPin", 0) != expectedPin) return false;
    }
    return dir.isDirectory() && dir.deleteRecursively();
}

inline bool materializeSanitizedModuleStoreCopy(const juce::File& srcDir,
                                                const juce::File& dstDir,
                                                const std::string& lineageUuid)
{
    if (! isSafeModuleLineagePathComponent(lineageUuid)) return false;
    if (! srcDir.isDirectory()) return false;

    auto mf = srcDir.getChildFile("manifest.json");
    if (! mf.existsAsFile()) return false;

    auto parsed = juce::JSON::parse(mf.loadFileAsString());
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr) return false;

    int currentPin = 0;
    if (! readManifestIntProperty(parsed, "currentPin", currentPin) || currentPin <= 0)
        return false;

    auto* versions = parsed.getProperty("versions", juce::var()).getArray();
    if (versions == nullptr || versions->isEmpty())
        return false;

    if (dstDir.exists() || ! dstDir.createDirectory())
        return false;

    juce::Array<juce::var> filteredVersions;
    bool copiedPinnedSource = false;

    for (const auto& v : *versions)
    {
        int rowVersion = 0;
        if (! readManifestIntProperty(v, "version", rowVersion) || rowVersion <= 0)
            continue;

        const auto sourceName = sourceNameForVersionEntry(v, rowVersion);
        const auto srcFile = sourceFileForVersionEntry(srcDir, v, rowVersion);
        if (sourceName.isEmpty() || ! srcFile.existsAsFile())
            continue;

        auto dstSource = dstDir.getChildFile(sourceName);
        if (! srcFile.copyFileTo(dstSource)) {
            dstDir.deleteRecursively();
            return false;
        }

        juce::DynamicObject::Ptr entry = new juce::DynamicObject();
        if (auto* sourceObj = v.getDynamicObject())
            for (const auto& prop : sourceObj->getProperties())
            {
                const auto name = prop.name.toString();
                if (name != "source" && name != "bytecode" && name != "version")
                    entry->setProperty(prop.name, prop.value);
            }

        entry->setProperty("version", rowVersion);
        entry->setProperty("source", sourceName);

        const auto bytecodeName = v.getProperty("bytecode", "").toString();
        if (isBytecodeCacheFileNameForVersion(bytecodeName, rowVersion))
        {
            auto srcBytecode = srcDir.getChildFile(bytecodeName);
            if (srcBytecode.existsAsFile())
            {
                auto dstBytecode = dstDir.getChildFile(bytecodeName);
                if (srcBytecode.copyFileTo(dstBytecode))
                    entry->setProperty("bytecode", bytecodeName);
            }
        }

        filteredVersions.add(juce::var(entry.get()));
        if (rowVersion == currentPin)
            copiedPinnedSource = true;
    }

    if (! copiedPinnedSource || filteredVersions.isEmpty()) {
        dstDir.deleteRecursively();
        return false;
    }

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    for (const auto& prop : obj->getProperties())
    {
        const auto name = prop.name.toString();
        if (name != "lineageUuid" && name != "versions" && name != "currentPin")
            root->setProperty(prop.name, prop.value);
    }

    root->setProperty("lineageUuid", juce::String(lineageUuid));
    root->setProperty("currentPin", currentPin);
    root->setProperty("versions", filteredVersions);

    if (! replaceManifestText(dstDir.getChildFile("manifest.json"),
                              juce::JSON::toString(juce::var(root.get())))) {
        dstDir.deleteRecursively();
        return false;
    }

    return true;
}

// First Save As for an unsaved project can encounter an instance recalled
// directly from the user library: it already carries an exact historical pin
// and embedded source, but no project-local store exists yet. Materialise the
// complete, sanitised library history without renumbering versions so the first
// save preserves build identity and later time travel remains deterministic.
inline bool materializeLibraryStoreForProject(const juce::File& libraryRoot,
                                              const juce::File& projectFolder,
                                              const std::string& lineageUuid,
                                              int requiredVersion = 0)
{
    if (! isSafeModuleLineagePathComponent(lineageUuid)) return false;
    const auto srcDir = libraryModuleDir(libraryRoot, lineageUuid);
    if (! hasLoadableModuleManifest(srcDir)) return false;

    const auto dstDir = moduleDir(projectFolder, lineageUuid);
    if (hasLoadableModuleManifest(dstDir))
    {
        if (requiredVersion > 0
            && readVersionSource (projectFolder, lineageUuid, requiredVersion).empty())
            return false;
#if JUCE_IOS
        // iOS: skip the dotted byte-for-byte compare dirs (they fail over the
        // iCloud-synced Documents root). The portable store is already here
        // and carries the requested version; it is re-derivable from the
        // library store on the next save.
        (void) requiredVersion;
        return true;
#else
        // Build the same sanitised copy used for a fresh destination and
        // compare every file byte-for-byte. Source-only comparison is not
        // enough: historical names, knob snapshots, exposed inputs, currentPin,
        // and cached bytecode are all part of deterministic recall.
        const auto modulesRoot = projectFolder.getChildFile("modules");
        const auto expectedRoot = modulesRoot.getChildFile(
            ".library-compare-" + juce::Uuid().toDashedString());
        const auto actualRoot = modulesRoot.getChildFile(
            ".project-compare-" + juce::Uuid().toDashedString());
        const auto expectedComparison = expectedRoot.getChildFile(
            juce::String(lineageUuid));
        const auto actualComparison = actualRoot.getChildFile(
            juce::String(lineageUuid));
        expectedRoot.deleteRecursively();
        actualRoot.deleteRecursively();
        if (! expectedRoot.createDirectory() || ! actualRoot.createDirectory())
        {
            expectedRoot.deleteRecursively();
            actualRoot.deleteRecursively();
            return false;
        }
        if (! materializeSanitizedModuleStoreCopy(
                srcDir, expectedComparison, lineageUuid)
            || ! materializeSanitizedModuleStoreCopy(
                dstDir, actualComparison, lineageUuid))
        {
            expectedRoot.deleteRecursively();
            actualRoot.deleteRecursively();
            return false;
        }
        auto expectedFiles = expectedComparison.findChildFiles(
            juce::File::findFiles, true);
        auto actualFiles = actualComparison.findChildFiles(
            juce::File::findFiles, true);
        auto relativeNames = [] (const juce::Array<juce::File>& files,
                                 const juce::File& root)
        {
            juce::StringArray names;
            for (const auto& file : files)
                names.add(file.getRelativePathFrom(root));
            names.sort(true);
            return names;
        };
        const auto expectedNames = relativeNames(expectedFiles, expectedComparison);
        const auto actualNames = relativeNames(actualFiles, actualComparison);
        const auto expectedFingerprint = moduleStoreFingerprintForDir(
            expectedComparison);
        const auto actualFingerprint = moduleStoreFingerprintForDir(
            actualComparison);
        bool identical = expectedNames == actualNames
            && expectedFingerprint.isNotEmpty()
            && expectedFingerprint == actualFingerprint;
        for (const auto& name : expectedNames)
        {
            if (! identical)
                break;
            if (name == "manifest.json")
                continue; // semantic fingerprint above is property-order independent
            juce::MemoryBlock expectedData;
            juce::MemoryBlock actualData;
            identical = expectedComparison.getChildFile(name).loadFileAsData(expectedData)
                && actualComparison.getChildFile(name).loadFileAsData(actualData)
                && expectedData == actualData;
        }
        expectedRoot.deleteRecursively();
        actualRoot.deleteRecursively();
        return identical;
#endif
    }
    if (dstDir.exists()) return false;

    const auto modulesRoot = projectFolder.getChildFile("modules");
    if (! modulesRoot.createDirectory()) return false;
#if JUCE_IOS
    // iOS: dotted .library-stage-* staging + rename fails over the iCloud-
    // synced Documents root. Materialise the store DIRECTLY into its final
    // non-hidden directory, exactly like the desktop layout.
    return materializeSanitizedModuleStoreCopy(srcDir, dstDir, lineageUuid);
#else
    const auto suffix = juce::Uuid().toDashedString();
    auto staging = modulesRoot.getChildFile(
        "." + juce::String(lineageUuid) + ".library-stage-" + suffix);
    staging.deleteRecursively();
    if (! materializeSanitizedModuleStoreCopy(srcDir, staging, lineageUuid))
    {
        staging.deleteRecursively();
        return false;
    }
    if (staging.moveFileTo(dstDir))
        return true;
    staging.deleteRecursively();
    return false;
#endif
}

// ── Cross-project import (F-076 MergeToLibrary) ──────────────────────────────
// Outcome of materialising one authored module's carried version from a source
// project's store into a destination project's store.
enum class ImportVersionOutcome {
    Reused,        // dst already had this (uuid, version) — no-op (dedup)
    AddedVersion,  // dst had the module at other versions — this version added additively
    AddedModule,   // dst had no such module — its whole version dir copied in
    SourceMissing  // src has no v<version>.fdsp for this uuid — nothing copied
};

// Materialise version `version` of authored module `lineageUuid` from
// srcProjectFolder into dstProjectFolder, PRESERVING the version number (unlike
// writeVersion, which auto-increments — the imported clip pins the number it
// carried, so it must not be renumbered; ADR adr-clips ¶31). Dedup by
// (uuid, version): an already-present version is a no-op (Reused). A module
// absent from the destination is copied whole — its full history ("different
// designId → just add"); a known module missing only this version gets the
// version added additively (AddedVersion). The embedded snapshot on the instance
// still carries the sound, so SourceMissing is non-fatal — the caller warns and
// keeps the instance. Pure file I/O so CurlopTests drives it with temp dirs.
inline ImportVersionOutcome importModuleVersionFromProject(
    const juce::File& srcProjectFolder,
    const juce::File& dstProjectFolder,
    const std::string& lineageUuid,
    int version)
{
    if (version <= 0 || ! isSafeModuleLineagePathComponent(lineageUuid))
        return ImportVersionOutcome::SourceMissing;

    auto dstDir = moduleDir(dstProjectFolder, lineageUuid);
    auto dstVFile = sourceFileForVersionEntry(
        dstDir, versionEntry(dstProjectFolder, lineageUuid, version), version);
    if (dstVFile.existsAsFile())
        return ImportVersionOutcome::Reused;                       // dedup

    auto srcDir = moduleDir(srcProjectFolder, lineageUuid);
    auto srcEntry = versionEntry(srcProjectFolder, lineageUuid, version);
    auto srcVFile = sourceFileForVersionEntry(srcDir, srcEntry, version);
    if (! srcVFile.existsAsFile())
        return ImportVersionOutcome::SourceMissing;

    auto dstManifest = manifestFile(dstProjectFolder, lineageUuid);
    const bool dstHasManifest = dstManifest.existsAsFile();
    if (dstHasManifest && ! hasModuleManifestForLineage(dstDir))
        return ImportVersionOutcome::SourceMissing;

    const bool dstHadModule = dstHasManifest || dstDir.isDirectory();

    if (! dstHadModule && srcDir.isDirectory() && hasLoadableModuleManifest(srcDir))
    {
        auto modulesRoot = dstProjectFolder.getChildFile("modules");
        if (! modulesRoot.createDirectory())
            return ImportVersionOutcome::SourceMissing;

        const auto suffix = juce::Uuid().toDashedString();
        auto staging = modulesRoot.getChildFile("." + juce::String(lineageUuid) + ".import-stage-" + suffix);
        staging.deleteRecursively();

        if (materializeSanitizedModuleStoreCopy(srcDir, staging, lineageUuid))
        {
            if (staging.moveFileTo(dstDir))
                return ImportVersionOutcome::AddedModule;          // unknown id → copy whole history

            staging.deleteRecursively();
            return ImportVersionOutcome::SourceMissing;
        }

        staging.deleteRecursively();
    }

    // Known module (or whole-copy fallback): bring in just this version's files,
    // then merge a manifest entry for it (number preserved, currentPin bumped).
    if (! dstDir.createDirectory())     return ImportVersionOutcome::SourceMissing;
    dstVFile = dstDir.getChildFile(srcVFile.getFileName());
    if (! srcVFile.copyFileTo(dstVFile)) return ImportVersionOutcome::SourceMissing;
    juce::Array<juce::File> copiedFiles;
    copiedFiles.add(dstVFile);

    // Best-effort: copy any sibling bytecode for this version (v<N>.bc / v<N>.fbc).
    for (const auto& f : srcDir.findChildFiles(juce::File::findFiles, false,
                                               "v" + juce::String(version) + ".*"))
        if (isBytecodeCacheFileNameForVersion(f.getFileName(), version))
        {
            auto copied = dstDir.getChildFile(f.getFileName());
            if (f.copyFileTo(copied))
                copiedFiles.add(copied);
        }

    juce::Array<juce::var> versions;
    int          currentPin  = 0;
    juce::String displayName;
    if (dstManifest.existsAsFile()) {
        auto prev   = juce::JSON::parse(dstManifest.loadFileAsString());
        displayName = prev.getProperty("displayName", "").toString();
        if (auto* arr = prev.getProperty("versions", juce::var()).getArray())
            for (const auto& v : *arr) {
                int rowVersion = -1;
                if (! readManifestIntProperty(v, "version", rowVersion) || rowVersion == version)
                    continue;

                if (sourceFileForVersionEntry(dstDir, v, rowVersion).existsAsFile()) {
                    versions.add(v);
                    currentPin = juce::jmax(currentPin, rowVersion);
                }
            }
    }
    // Carry the source's cached version entry (name/knobs/etc.) when present.
    if (srcEntry.getDynamicObject() != nullptr) {
        versions.add(srcEntry);
    } else {
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty("version", version);
        e->setProperty("source",  dstVFile.getFileName());
        versions.add(juce::var(e.get()));
    }
    if (displayName.isEmpty())
        displayName = juce::String(manifestDisplayName(srcProjectFolder, lineageUuid));

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("lineageUuid", juce::String(lineageUuid));
    root->setProperty("displayName", displayName);
    root->setProperty("tier",        "draft");
    root->setProperty("currentPin",  juce::jmax(currentPin, version));
    root->setProperty("versions",    juce::var(versions));
    if (! replaceManifestText(dstManifest,
                              juce::JSON::toString(juce::var(root.get()))))
    {
        for (const auto& f : copiedFiles)
            if (f.existsAsFile())
                f.deleteFile();
        return ImportVersionOutcome::SourceMissing;
    }

    return ImportVersionOutcome::AddedVersion;
}

}} // namespace curlop::modlib
