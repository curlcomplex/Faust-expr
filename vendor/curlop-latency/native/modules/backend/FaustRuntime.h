#pragma once
//
// FaustRuntime — the shared Faust compilation service (T-404, SF-058 P1;
// ADR-0009). One internally-polymorphic runtime behind every Faust module:
//
//   - Factory cache: compiled programs are shared, keyed by
//     (source SHA, backend). Identical source acquires the SAME factory —
//     N instances of one module compile once. Warmed at project load (P6),
//     so a clip switch is createDSPInstance + atomic ApgBundle swap, never
//     a compile.
//   - Backend policy: LLVM JIT on desktop, interpreter on iOS (P5 — no
//     runtime codegen on device). forceBackend() is the test/debug override
//     that drives the P3 interp-vs-JIT parity probe.
//   - Bytecode store: factories serialize to disk (LLVM bitcode for the
//     JIT, .fbc for the interpreter) and reload ~3.25x faster than a
//     recompile (s477 bench, faust-jit-perf-data.md). This is the
//     "smart Faust modules save as bitcode" persistence layer.
//
// All compile/serialize entry points hold compileMutex() — the process-wide
// libfaust serialization lock (B-205: libfaust's LLVM internals are not
// concurrency-safe; two racing compiles EXC_BAD_ACCESS in DataLayout).
// FaustNode shares this same mutex; there is exactly one.
//
// Header is libfaust-free (same constraint as FaustNode.h): libfaust
// declares global-namespace `class dsp` / `class UI`, which collide with
// the stub in FaustPrecompiledAdapter.h if both reach one TU. `dsp_factory`
// has no local stub, so the forward declaration below is safe everywhere;
// TUs that call through it (FaustNode in P2, tests) include
// <faust/dsp/llvm-dsp.h> themselves for the complete type.
//
// Nothing here runs on the audio thread. acquire/load/write are message- or
// worker-thread calls; the audio thread only ever sees dsp instances handed
// over via the existing atomic-swap patterns.
//
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

class dsp_factory; // libfaust polymorphic factory base (faust/dsp/dsp.h)

namespace curlop {

// LLVM's AArch64 code generator needs more than the small default stack used
// by some worker launchers. Keep every in-process Faust compiler worker on
// the same explicit budget.
inline constexpr std::size_t kFaustCompilerThreadStackBytes =
    8u * 1024u * 1024u;

class FaustRuntime
{
public:
    enum class Backend { Jit, Interpreter };

    // A cached, shared compilation result. Handles are reference-counted;
    // the runtime keeps factories resident (the warm pool) until
    // evictUnused() drops the ones nobody else holds. Destruction releases
    // the libfaust factory under compileMutex() (B-205: factory teardown
    // touches the same LLVM state as compiles).
    class Factory
    {
    public:
        ~Factory();
        Backend backend() const { return backend_; }
        const std::string& key() const { return key_; }       // source SHA1
        const std::string& name() const { return name_; }
        // The libfaust factory. Complete type requires <faust/dsp/llvm-dsp.h>
        // (or interpreter-dsp.h) in the calling TU. createDSPInstance() off
        // this is the P2 FaustNode path.
        dsp_factory* raw() const { return raw_; }

    private:
        friend class FaustRuntime;
        Factory(Backend b, std::string key, std::string name, dsp_factory* raw)
            : backend_(b), key_(std::move(key)), name_(std::move(name)), raw_(raw) {}
        Backend      backend_;
        std::string  key_;
        std::string  name_;
        dsp_factory* raw_;
    };

    using FactoryPtr = std::shared_ptr<Factory>;

    FaustRuntime();
    ~FaustRuntime();

    FaustRuntime(const FaustRuntime&) = delete;
    FaustRuntime& operator=(const FaustRuntime&) = delete;

    // The app-wide service instance. Tests construct their own runtimes for
    // isolation; production code shares this one so the cache actually shares.
    static FaustRuntime& instance();

    // ── Backend policy ──────────────────────────────────────────────────
    // Platform default: JIT on desktop, interpreter on iOS (P5 lands the
    // iOS leg; the policy seam exists from P1 so nothing else branches).
    Backend defaultBackend() const;
    void forceBackend(Backend b);   // test/debug override (P3 parity probe)
    void clearBackendOverride();

    // ── Factory cache ───────────────────────────────────────────────────
    // Cache hit returns the SAME handle for identical (source, backend).
    // Miss compiles under compileMutex() and inserts. On compile failure
    // returns null with the libfaust diagnostic in errOut; failures are
    // never cached. `name` labels the factory (diagnostics); it is NOT part
    // of the cache key — the program is the source text.
    FactoryPtr acquire(const std::string& name, const std::string& source,
                       std::string& errOut);
    FactoryPtr acquire(const std::string& name, const std::string& source,
                       Backend backend, std::string& errOut);
    // Desktop live-authoring path: compile a cold JIT factory in the bundled
    // background helper process, then load its bitcode into this process.
    // Cache hits remain in-process and immediate. Falls back to acquire()
    // when the helper is unavailable (tests and non-Standalone targets).
    FactoryPtr acquireForLiveAuthoring(const std::string& name,
                                       const std::string& source,
                                       std::string& errOut);

    std::size_t cacheSize() const;
    // Drop every cache entry no caller still holds. Returns the count
    // dropped. Held handles stay valid and cache-resident.
    std::size_t evictUnused();

    // Free every cached factory NOW — call once during controlled app shutdown,
    // after the graph/nodes are gone, while libfaust + LLVM are still alive.
    // The singleton instance() is deliberately leaked, so without this its
    // cached JIT factories are never deleteDSPFactory'd during the run; they
    // then sit in libfaust's GLOBAL dsp_factory_table until ITS static
    // destructor frees them at __cxa_finalize — which runs AFTER LLVM's statics,
    // so MCJIT::~MCJIT locks an already-finalized GDB-JIT mutex and aborts
    // (SIGABRT on every quit with a faust_jit module present). Freeing here,
    // while LLVM is alive, empties libfaust's table so exit has nothing to do.
    void releaseAllFactories();

    // ── Bytecode store ──────────────────────────────────────────────────
    // Directory of serialized factories: <sha1>.bc (LLVM bitcode, JIT) /
    // <sha1>.fbc (interpreter bytecode). Default: <app-data>/FaustStore;
    // tests point at a temp dir.
    void setStoreDirectory(const std::string& absolutePath);
    std::string storeDirectory() const;

    // Serialize a factory into the store under its source key. Overwrites.
    bool writeToStore(const FactoryPtr& factory, std::string& errOut);

    bool isInStore(const std::string& source, Backend backend) const;

    // Read a stored program back into a live factory and register it in the
    // cache (a later acquire of the same source cache-hits instead of
    // compiling — the P6 warm-load path). Cache hit short-circuits the disk
    // read. Null + errOut on a store miss or a corrupt file.
    FactoryPtr loadFromStore(const std::string& source, Backend backend,
                             std::string& errOut);

    // ── libfaust serialization (B-205) ──────────────────────────────────
    // THE process-wide libfaust lock. Every libfaust entry point anywhere in
    // CURLOP (compile, factory delete, serialize) holds this.
    static std::mutex& compileMutex();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace curlop
