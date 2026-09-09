// FaustRuntime — shared Faust compilation service implementation (T-404,
// SF-058 P1). libfaust headers are confined to this TU (the FaustNode.cpp
// pattern): <faust/dsp/llvm-dsp.h> + <faust/dsp/interpreter-dsp.h> declare
// global-namespace `class dsp` / `class UI` that collide with the stub in
// FaustPrecompiledAdapter.h if both reach one TU.
//
// Every libfaust call here holds compileMutex() — see B-205 in
// FaustNode.cpp: libfaust's LLVM internals (DataLayout, EarlyCSE) are not
// safe under concurrent invocation, and factory deletion tears down shared
// LLVM module state. One process-wide mutex serializes all of it.
//
#include "modules/backend/FaustRuntime.h"
#include "shell/CurlopDebug.h" // CDBG FAUST_RUNTIME taps — cache hit/miss/store IO

#include <faust/dsp/interpreter-dsp.h>
#if CURLOP_IOS
#include <libfaust.h>
#else
#include <faust/dsp/libfaust.h> // generateSHA1
#endif
#if CURLOP_ENABLE_FAUST_JIT
#include <faust/dsp/llvm-dsp.h>
#endif

#include <juce_core/juce_core.h>

#if JUCE_WINDOWS
 #include <windows.h>
#elif JUCE_LINUX
 #include <dlfcn.h>
#endif

#include <algorithm>
#include <cstdlib>  // getenv — CURLOP_FAUST_BACKEND force flag (T-406)
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace curlop {

namespace {

inline const char* backendTag(FaustRuntime::Backend b)
{
    return b == FaustRuntime::Backend::Jit ? "jit" : "interp";
}

// Store extension per backend: LLVM bitcode for the JIT, Faust interpreter
// bytecode (.fbc) for the interpreter — the two serialized forms the s477
// bench measured (bitcode reload ~3.25x faster than recompile).
inline const char* storeExtension(FaustRuntime::Backend b)
{
    return b == FaustRuntime::Backend::Jit ? ".bc" : ".fbc";
}

struct FaustCompileArgs
{
    std::vector<std::string> storage;
    std::vector<const char*> argv;

    int argc() const { return (int) argv.size(); }
    const char** data() { return argv.empty() ? nullptr : argv.data(); }
};

void addFaustLibPath(FaustCompileArgs& args, const juce::File& dir)
{
    if (! dir.getChildFile("stdfaust.lib").existsAsFile())
        return;

    const auto path = dir.getFullPathName().toStdString();
    if (std::find(args.storage.begin(), args.storage.end(), path) != args.storage.end())
        return;

    args.storage.push_back("-I");
    args.storage.push_back(path);
}

juce::File currentModuleDirectory()
{
#if JUCE_IOS
    // iOS resources are copied to the flat application bundle.  Unlike the
    // desktop executable probes below, this must use JUCE's bundle-aware path:
    // otherwise the interpreter receives no -I path for stdfaust.lib.
    return juce::File::getSpecialLocation(
        juce::File::SpecialLocationType::currentApplicationFile);
#elif JUCE_WINDOWS
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&currentModuleDirectory),
                           &module)) {
        char path[MAX_PATH] = {};
        const auto n = GetModuleFileNameA(module, path, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            return juce::File(path).getParentDirectory();
    }
#elif JUCE_LINUX
    Dl_info info {};
    if (dladdr(reinterpret_cast<void*>(&currentModuleDirectory), &info) != 0
        && info.dli_fname != nullptr)
        return juce::File(info.dli_fname).getParentDirectory();
#endif
    return {};
}

void addBundledFaustLibPaths(FaustCompileArgs& args)
{
    const auto moduleDir = currentModuleDirectory();
    if (moduleDir == juce::File())
        return;

    addFaustLibPath(args, moduleDir.getChildFile("faust"));
    addFaustLibPath(args, moduleDir.getChildFile("Resources").getChildFile("faust"));
    addFaustLibPath(args, moduleDir.getParentDirectory()
                              .getChildFile("Resources")
                              .getChildFile("faust"));
}

FaustCompileArgs makeCompileArgs()
{
    FaustCompileArgs args;

    if (const char* env = std::getenv("CURLOP_FAUST_LIB_DIR"))
        addFaustLibPath(args, juce::File(env));

    addBundledFaustLibPaths(args);

#ifdef CURLOP_FAUST_SHARE_DIR
    addFaustLibPath(args, juce::File(CURLOP_FAUST_SHARE_DIR));
#endif

    addFaustLibPath(args, juce::File("/opt/homebrew/share/faust"));
    addFaustLibPath(args, juce::File("/usr/local/share/faust"));
    addFaustLibPath(args, juce::File("/usr/share/faust"));

    args.argv.reserve(args.storage.size());
    for (const auto& s : args.storage)
        args.argv.push_back(s.c_str());
    return args;
}

// Compile `source` on `backend`. Caller holds compileMutex().
dsp_factory* compileLocked(FaustRuntime::Backend backend,
                           const std::string& name,
                           const std::string& source,
                           std::string& errOut)
{
    errOut.clear();
    std::string err;
    auto args = makeCompileArgs();
    if (backend == FaustRuntime::Backend::Jit) {
#if CURLOP_ENABLE_FAUST_JIT
        auto* f = createDSPFactoryFromString(name, source, args.argc(), args.data(),
                                             /*target*/ "", err, /*opt*/ -1);
        if (f == nullptr) errOut = "createDSPFactoryFromString: " + err;
        return f;
#else
        errOut = "Faust JIT is unavailable in this build";
        return nullptr;
#endif
    }
    auto* f = createInterpreterDSPFactoryFromString(name, source, args.argc(), args.data(), err);
    if (f == nullptr) errOut = "createInterpreterDSPFactoryFromString: " + err;
    return f;
}

// Release a factory through its backend-specific deleter. Caller holds
// compileMutex().
void deleteLocked(FaustRuntime::Backend backend, dsp_factory* raw)
{
    if (raw == nullptr) return;
    if (backend == FaustRuntime::Backend::Jit) {
#if CURLOP_ENABLE_FAUST_JIT
        deleteDSPFactory(static_cast<llvm_dsp_factory*>(raw));
#endif
    } else {
        deleteInterpreterDSPFactory(static_cast<interpreter_dsp_factory*>(raw));
    }
}

} // anonymous namespace

// ── Factory ──────────────────────────────────────────────────────────────

FaustRuntime::Factory::~Factory()
{
    std::lock_guard<std::mutex> faustLock(FaustRuntime::compileMutex());
    deleteLocked(backend_, raw_);
    raw_ = nullptr;
}

// ── Impl ─────────────────────────────────────────────────────────────────

struct FaustRuntime::Impl
{
    // Cache key: (source SHA1, backend). Source text itself is hashed once
    // per distinct source; the SHA1 doubles as the store filename stem.
    using Key = std::pair<std::string, Backend>;

    mutable std::mutex                    mu; // covers all fields below
    std::map<Key, FactoryPtr>             cache;
    std::unordered_map<std::string, std::string> sourceShaCache;
    std::string                           storeDir;
    bool                                  hasOverride = false;
    Backend                               backendOverride = Backend::Jit;

    static std::string sha1(const std::string& source)
    {
        // libfaust's own content hash — stable across runs and matches the
        // key scheme libfaust uses for its internal factory registry.
        std::lock_guard<std::mutex> faustLock(FaustRuntime::compileMutex());
        return generateSHA1(source);
    }

    std::string keyForSource(const std::string& source)
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            auto it = sourceShaCache.find(source);
            if (it != sourceShaCache.end())
                return it->second;
        }

        const std::string key = sha1(source);
        std::lock_guard<std::mutex> lk(mu);
        auto [it, inserted] = sourceShaCache.emplace(source, key);
        (void) inserted;
        return it->second;
    }

    juce::File storeFileLocked(const std::string& key, Backend b) const
    {
        return juce::File(storeDir)
            .getChildFile(juce::String(key) + storeExtension(b));
    }
};

// ── FaustRuntime ─────────────────────────────────────────────────────────

FaustRuntime::FaustRuntime() : impl_(std::make_unique<Impl>())
{
    impl_->storeDir = juce::File::getSpecialLocation(
                          juce::File::userApplicationDataDirectory)
                          .getChildFile("CURLOP")
                          .getChildFile("FaustStore")
                          .getFullPathName()
                          .toStdString();

    // T-406 (SF-058 P3): force-interp test flag. CURLOP_FAUST_BACKEND=interp
    // (or =jit) overrides the platform default for every runtime in the
    // process — the P3 parity probe launches the app with it and compares
    // AUDIO_TAP bit-exactness against a JIT run. Read once at construction;
    // forceBackend()/clearBackendOverride() still win afterwards.
    if (const char* env = std::getenv("CURLOP_FAUST_BACKEND")) {
        const std::string v(env);
        if (v == "interp" || v == "interpreter") {
            impl_->hasOverride     = true;
            impl_->backendOverride = Backend::Interpreter;
        } else if (v == "jit") {
            impl_->hasOverride     = true;
            impl_->backendOverride = Backend::Jit;
        }
    }
}

FaustRuntime::~FaustRuntime() = default;

FaustRuntime& FaustRuntime::instance()
{
    // Intentionally leaked (never destroyed). A function-local static would
    // run ~FaustRuntime during __cxa_finalize at process exit — AFTER
    // libfaust's own globals are finalized — and the cache teardown's
    // deleteDSPFactory then aborts (SIGABRT on every app quit; five .ips
    // reports on 2026-06-12 before this fix). Factories cached here live
    // for the process lifetime by design (the warm pool); the OS reclaims
    // at exit. Stack-local runtimes (tests) destruct mid-process where
    // libfaust is alive and are unaffected.
    static FaustRuntime* rt = new FaustRuntime();
    return *rt;
}

std::mutex& FaustRuntime::compileMutex()
{
    static std::mutex m;
    return m;
}

FaustRuntime::Backend FaustRuntime::defaultBackend() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->hasOverride) return impl_->backendOverride;
#if CURLOP_IOS
    return Backend::Interpreter; // no runtime codegen on device (P5)
#else
    return Backend::Jit;
#endif
}

void FaustRuntime::forceBackend(Backend b)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->hasOverride    = true;
    impl_->backendOverride = b;
}

void FaustRuntime::clearBackendOverride()
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->hasOverride = false;
}

FaustRuntime::FactoryPtr FaustRuntime::acquire(const std::string& name,
                                               const std::string& source,
                                               std::string& errOut)
{
    return acquire(name, source, defaultBackend(), errOut);
}

FaustRuntime::FactoryPtr FaustRuntime::acquire(const std::string& name,
                                               const std::string& source,
                                               Backend backend,
                                               std::string& errOut)
{
    errOut.clear();
    const std::string key = impl_->keyForSource(source);
    const Impl::Key cacheKey { key, backend };

    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto it = impl_->cache.find(cacheKey);
        if (it != impl_->cache.end()) {
            CDBG(FAUST_RUNTIME, "cache-hit name=%s backend=%s key=%.12s",
                 name.c_str(), backendTag(backend), key.c_str());
            return it->second;
        }
    }

    dsp_factory* raw = nullptr;
    {
        std::lock_guard<std::mutex> faustLock(compileMutex());
        raw = compileLocked(backend, name, source, errOut);
    }
    if (raw == nullptr) {
        CDBG(FAUST_RUNTIME, "compile-fail name=%s backend=%s err=%s",
             name.c_str(), backendTag(backend), errOut.c_str());
        return nullptr;
    }

    FactoryPtr made(new Factory(backend, key, name, raw));
    std::lock_guard<std::mutex> lk(impl_->mu);
    // A concurrent acquire of the same program may have won the compile
    // race while we held no lock — the first insert stays canonical so
    // every caller shares one factory; `made` then just releases its copy.
    auto [it, inserted] = impl_->cache.emplace(cacheKey, made);
    CDBG(FAUST_RUNTIME, "%s name=%s backend=%s key=%.12s cache=%d",
         inserted ? "compiled" : "compile-race-lost", name.c_str(),
         backendTag(backend), key.c_str(), (int) impl_->cache.size());
    return it->second;
}

FaustRuntime::FactoryPtr FaustRuntime::acquireForLiveAuthoring(
    const std::string& name, const std::string& source, std::string& errOut)
{
    errOut.clear();
    const auto backend = defaultBackend();
    if (backend != Backend::Jit)
        return acquire (name, source, backend, errOut);

    const std::string key = impl_->keyForSource (source);
    const Impl::Key cacheKey { key, backend };
    {
        std::lock_guard<std::mutex> lk (impl_->mu);
        const auto found = impl_->cache.find (cacheKey);
        if (found != impl_->cache.end())
            return found->second;
    }

#if JUCE_MAC && ! JUCE_IOS
    const auto moduleDir = currentModuleDirectory();
    auto helper = moduleDir.getChildFile ("CurlopFaustCompilerHelper");
    if (! helper.existsAsFile())
        helper = moduleDir.getParentDirectory().getChildFile ("Resources")
                          .getChildFile ("CurlopFaustCompilerHelper");
    if (helper.existsAsFile())
    {
        const auto storeDir = juce::File (impl_->storeDir);
        storeDir.createDirectory();
        juce::File output;
        {
            std::lock_guard<std::mutex> lk (impl_->mu);
            output = impl_->storeFileLocked (key, backend);
        }
        const auto sourceFile = juce::File::getSpecialLocation (
            juce::File::tempDirectory).getNonexistentChildFile (
                "curlop-faust-live", ".dsp", false);
        if (sourceFile.replaceWithText (juce::String::fromUTF8 (
                source.data(), static_cast<int> (source.size()))))
        {
            juce::StringArray arguments;
            if (juce::File ("/usr/bin/taskpolicy").existsAsFile())
            {
                arguments.add ("/usr/bin/taskpolicy");
                arguments.add ("-b");
            }
            arguments.add (helper.getFullPathName());
            arguments.add (sourceFile.getFullPathName());
            arguments.add (output.getFullPathName());

            juce::ChildProcess child;
            const bool started = child.start (arguments);
            const bool finished = started && child.waitForProcessToFinish (160000);
            const auto outputText = started ? child.readAllProcessOutput() : juce::String();
            const auto exitCode = finished ? child.getExitCode() : 1u;
            sourceFile.deleteFile();
            if (finished && exitCode == 0 && output.existsAsFile())
            {
                auto loaded = loadFromStore (source, backend, errOut);
                if (loaded != nullptr)
                    return loaded;
            }
            else if (started)
            {
                errOut = outputText.toStdString();
                if (errOut.empty()) errOut = "Faust compiler helper failed";
                return nullptr;
            }
        }
    }
#endif

    return acquire (name, source, backend, errOut);
}

std::size_t FaustRuntime::cacheSize() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->cache.size();
}

void FaustRuntime::releaseAllFactories()
{
    // Clearing the cache drops the last FactoryPtr ref to each factory →
    // Factory::~Factory → deleteDSPFactory (under compileMutex), removing it
    // from libfaust's global table. Must run while LLVM is alive (controlled
    // shutdown), NOT at static-destruction time — see the header comment.
    std::lock_guard<std::mutex> lk(impl_->mu);
    const auto n = impl_->cache.size();
    impl_->cache.clear();
    impl_->sourceShaCache.clear();
    CDBG(FAUST_RUNTIME, "releaseAllFactories cleared=%d", (int) n);
}

std::size_t FaustRuntime::evictUnused()
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::size_t dropped = 0;
    for (auto it = impl_->cache.begin(); it != impl_->cache.end();) {
        if (it->second.use_count() == 1) {
            it = impl_->cache.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    if (dropped > 0)
        impl_->sourceShaCache.clear();
    if (dropped > 0)
        CDBG(FAUST_RUNTIME, "evict-unused dropped=%d cache=%d",
             (int) dropped, (int) impl_->cache.size());
    return dropped;
}

void FaustRuntime::setStoreDirectory(const std::string& absolutePath)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->storeDir = absolutePath;
}

std::string FaustRuntime::storeDirectory() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->storeDir;
}

bool FaustRuntime::writeToStore(const FactoryPtr& factory, std::string& errOut)
{
    errOut.clear();
    if (factory == nullptr || factory->raw() == nullptr) {
        errOut = "writeToStore: null factory";
        return false;
    }

    juce::File file;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        file = impl_->storeFileLocked(factory->key(), factory->backend());
    }
    if (auto res = file.getParentDirectory().createDirectory(); res.failed()) {
        errOut = "writeToStore: " + res.getErrorMessage().toStdString();
        return false;
    }

    const std::string path = file.getFullPathName().toStdString();
    bool ok = false;
    {
        std::lock_guard<std::mutex> faustLock(compileMutex());
        if (factory->backend() == Backend::Jit) {
#if CURLOP_ENABLE_FAUST_JIT
            ok = writeDSPFactoryToBitcodeFile(
                static_cast<llvm_dsp_factory*>(factory->raw()), path);
#else
            errOut = "writeToStore: Faust JIT is unavailable in this build";
#endif
        } else {
            ok = writeInterpreterDSPFactoryToBitcodeFile(
                static_cast<interpreter_dsp_factory*>(factory->raw()), path);
        }
    }
    if (! ok) {
        errOut = "writeToStore: serialization failed for " + path;
        return false;
    }
    CDBG(FAUST_RUNTIME, "store-write backend=%s key=%.12s",
         backendTag(factory->backend()), factory->key().c_str());
    return true;
}

bool FaustRuntime::isInStore(const std::string& source, Backend backend) const
{
    const std::string key = impl_->keyForSource(source);
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->storeFileLocked(key, backend).existsAsFile();
}

FaustRuntime::FactoryPtr FaustRuntime::loadFromStore(const std::string& source,
                                                     Backend backend,
                                                     std::string& errOut)
{
    errOut.clear();
    const std::string key = impl_->keyForSource(source);
    const Impl::Key cacheKey { key, backend };

    juce::File file;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto it = impl_->cache.find(cacheKey);
        if (it != impl_->cache.end()) return it->second;
        file = impl_->storeFileLocked(key, backend);
    }
    if (! file.existsAsFile()) {
        errOut = "loadFromStore: no stored program at "
               + file.getFullPathName().toStdString();
        return nullptr;
    }

    const std::string path = file.getFullPathName().toStdString();
    dsp_factory* raw = nullptr;
    std::string err;
    {
        std::lock_guard<std::mutex> faustLock(compileMutex());
        if (backend == Backend::Jit) {
#if CURLOP_ENABLE_FAUST_JIT
            raw = readDSPFactoryFromBitcodeFile(path, /*target*/ "", err, /*opt*/ -1);
#else
            err = "Faust JIT is unavailable in this build";
#endif
        } else {
            raw = readInterpreterDSPFactoryFromBitcodeFile(path, err);
        }
    }
    if (raw == nullptr) {
        errOut = "loadFromStore: " + (err.empty() ? "read failed: " + path : err);
        CDBG(FAUST_RUNTIME, "store-read-fail backend=%s key=%.12s err=%s",
             backendTag(backend), key.c_str(), errOut.c_str());
        return nullptr;
    }

    FactoryPtr made(new Factory(backend, key, key, raw));
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto [it, inserted] = impl_->cache.emplace(cacheKey, made);
    CDBG(FAUST_RUNTIME, "store-read backend=%s key=%.12s cache=%d",
         backendTag(backend), key.c_str(), (int) impl_->cache.size());
    return it->second;
}

} // namespace curlop
