#include "control/surfaces/script/ScriptCompiler.h"
#include "control/surfaces/script/ScriptOutputSockets.h"
#include "control/surfaces/script/music/PitchUtils.h"
#include "control/surfaces/script/music/MusicData.h"
#include "modules/contract/ParameterValue.h"
#include "shell/CurlopDebug.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <cstring>
#include <numeric>
#include <string>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <vector>

namespace curlop::script {

namespace {

bool parseStrictFiniteDouble(const juce::String& raw, double& out)
{
    const auto trimmed = raw.trim();
    if (trimmed.isEmpty()) return false;

    const auto text = trimmed.toStdString();
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()
        || end == nullptr
        || *end != '\0'
        || errno == ERANGE
        || ! std::isfinite(value))
        return false;

    out = value;
    return true;
}

bool parseFrequencyTextHz(const juce::String& raw, double& hzOut)
{
    const auto lower = raw.trim().toLowerCase();
    if (lower.endsWith("khz")) {
        double value = 0.0;
        if (! parseStrictFiniteDouble(lower.dropLastCharacters(3), value))
            return false;
        hzOut = value * 1000.0;
        return std::isfinite(hzOut) && hzOut > 0.0;
    }
    if (lower.endsWith("hz")) {
        double value = 0.0;
        if (! parseStrictFiniteDouble(lower.dropLastCharacters(2), value))
            return false;
        hzOut = value;
        return std::isfinite(hzOut) && hzOut > 0.0;
    }
    return false;
}

bool parseTimeTextSeconds(const juce::String& raw, double& secondsOut)
{
    const auto lower = raw.trim().toLowerCase();
    if (lower.endsWith("ms")) {
        double ms = 0.0;
        if (! parseStrictFiniteDouble(lower.dropLastCharacters(2), ms))
            return false;
        secondsOut = ms / 1000.0;
        return std::isfinite(secondsOut);
    }
    if (lower.endsWith("s")) {
        double seconds = 0.0;
        if (! parseStrictFiniteDouble(lower.dropLastCharacters(1), seconds))
            return false;
        secondsOut = seconds;
        return std::isfinite(secondsOut);
    }
    return false;
}

// ── ModuleRegistry — dslName → index, seeded from graph. ──────────────────
class ModuleRegistry {
public:
    explicit ModuleRegistry(const std::vector<std::pair<juce::String, int>>& seed)
    {
        for (const auto& [name, idx] : seed) {
            // B-346: the parser lowercases every #ref identifier
            // (ScriptParser.cpp IDENT path), so the lookup must be
            // case-insensitive — else a recalled library module named "Cymbal"
            // never matches the lowercased ref "cymbal" and a dead phantom
            // module is minted (no audio). Keys are folded; ordered_ keeps the
            // original case so moduleNames metadata round-trips unchanged.
            indexByName_[name.toLowerCase()] = idx;
            ordered_.emplace_back(name, idx);
            if (idx >= next_) next_ = idx + 1;
        }
    }
    int indexOf(const juce::String& name) {
        const juce::String key = name.toLowerCase();
        auto it = indexByName_.find(key);
        if (it != indexByName_.end()) return it->second;
        int idx = next_++;
        indexByName_[key] = idx;
        ordered_.emplace_back(name, idx);
        return idx;
    }
    std::vector<std::pair<juce::String, int>> take() { return std::move(ordered_); }
private:
    std::unordered_map<juce::String, int>     indexByName_;
    std::vector<std::pair<juce::String, int>> ordered_;
    int                                       next_ = 0;
};

double noteSemisFromHz(double hz)
{
    if (hz <= 0.0) return 9.0;
    return 69.0 + 12.0 * std::log2(hz / 440.0) - 60.0;
}

// ── resolveValue: JS BytecodeCompiler.js:173 port. ────────────────────────
// Domains: time, count, rate, pitch, param, tempo, unit, raw.
double resolveValue(const ParseValue& v, const juce::String& domain, double bpm, double fallback = 0.0)
{
    if (v.kind == ParseValue::Kind::Null) return fallback;

    // Mirror JS BytecodeCompiler.js:2386 — non-numeric object args (generator,
    // cond, etc.) extract `.value` which is undefined → parseFloat → NaN.
    // Downstream `+`, `*`, emitF32 propagate NaN.
    if (v.kind == ParseValue::Kind::Generator
        || v.kind == ParseValue::Kind::SignalChain
        || v.kind == ParseValue::Kind::PolyList
        || v.kind == ParseValue::Kind::Cond
        || v.kind == ParseValue::Kind::NegatedLoops
        || v.kind == ParseValue::Kind::Rest)
        return std::numeric_limits<double>::quiet_NaN();

    if (v.kind == ParseValue::Kind::UnitNumber) {
        const double val = v.number;
        const juce::String& u = v.unit;
        if (u == "ms") {
            if (domain == "time" || domain == "count") return val * bpm / 60000.0;
            if (domain == "tempo") return 60000.0 / val;
            if (domain == "lforate") return val > 0 ? 1000.0 / val : 0.0;
            return val;
        }
        if (u == "s") {
            if (domain == "time" || domain == "count") return val * bpm / 60.0;
            if (domain == "tempo") return 60.0 / val;
            if (domain == "param") return val * 1000.0;
            if (domain == "lforate") return val > 0 ? 1.0 / val : 0.0;
            return val;
        }
        if (u == "%") {
            // lforate: % of a step — 50% = half-step cycle.
            if (domain == "lforate")
                return val > 0 ? bpm / (60.0 * (val / 100.0)) : 0.0;
            return val / 100.0;
        }
        if (u == "hz") {
            if (domain == "time") return (1.0 / val) * bpm / 60.0;
            if (domain == "tempo") return val * 60.0;
            if (domain == "count") return val * 60.0 / bpm;
            if (domain == "note") return noteSemisFromHz(val);
            return val;
        }
        if (u == "steps" || u == "step") {
            // Explicit step-cycle unit (spec ~lfo example `4steps`).
            if (domain == "lforate")
                return val > 0 ? bpm / (60.0 * val) : 0.0;
            return val;
        }
        return val;
    }

    if (v.kind == ParseValue::Kind::Number) {
        // Universal Op Rule 6 (SYNTAX_V1): bare numbers are STEP times —
        // for cycle-rate args a bare N means an N-step cycle. Explicit
        // hz/khz/ms/s opt out of BPM scaling. Fractions arrive as plain
        // numbers (1/16 -> 0.0625 -> a sixteenth-step cycle).
        if (domain == "lforate")
            return v.number > 0 ? bpm / (60.0 * v.number) : 0.0;
        return v.number;
    }

    if (v.kind == ParseValue::Kind::String) {
        // String could be a note name (c3, d#4), freq (440hz), or identifier.
        const juce::String lower = v.str.toLowerCase();
        double hz = 0.0;
        if (parseFrequencyTextHz(lower, hz)) {
            if (domain == "time") return (1.0 / hz) * bpm / 60.0;
            if (domain == "tempo") return hz * 60.0;
            if (domain == "count") return hz * 60.0 / bpm;
            if (domain == "note") return noteSemisFromHz(hz);
            return hz;
        }
        // Note name detection.
        if (lower.length() >= 2) {
            const char c0 = lower[0];
            if (c0 >= 'a' && c0 <= 'g') {
                const int midi = curlop::pitch::noteToMidi(v.str);
                hz = midi >= 0 ? curlop::pitch::semiToHz(midi) : -1.0;
                if (midi >= 0 && hz > 0) {
                    if (domain == "note") return midi - 60.0;
                    if (domain == "time") return (1.0 / hz) * bpm / 60.0;
                    if (domain == "tempo") return hz * 60.0;
                    return hz;
                }
            }
        }
        return fallback;  // identifier strings return fallback in numeric contexts
    }
    return fallback;
}

double getNumVal(const ParseValue& v, double fallback = 0.0)
{
    if (v.kind == ParseValue::Kind::Number || v.kind == ParseValue::Kind::UnitNumber)
        return v.number;
    return fallback;
}

// Mirror JS `getNumVal(<obj>)` returning the object → emitF32 coerces to NaN.
// Use at every emit site where JS does `getNumVal(op.args[i] ?? default)`
// without an explicit type-check guard: if the supplied arg is non-null but
// non-numeric (generator, cond, etc.), JS emits NaN — C++ must too.
double getNumValOrNaN(const ParseValue& v, double fallback = 0.0)
{
    if (v.kind == ParseValue::Kind::Null) return fallback;
    if (v.kind == ParseValue::Kind::Number || v.kind == ParseValue::Kind::UnitNumber)
        return v.number;
    return std::numeric_limits<double>::quiet_NaN();
}

// ── resolveStepUnitScale: mirror BytecodeCompiler.js:21-49. ───────────────
// Returns scale factor real_at_bpm / ref_at_120. Sentinel -1.0 from inner
// resolve = "unknown unit" → caller falls back to 1.0.
double resolveStepUnitScale(const AstNode& step, double bpm)
{
    if (step.stepUnitArg.kind == ParseValue::Kind::Null) return 1.0;
    if (bpm == 120.0) return 1.0;

    constexpr double REF = 120.0;
    auto resolveAt = [](const ParseValue& a, double b) -> double {
        if (a.kind == ParseValue::Kind::UnitNumber) {
            const juce::String& u = a.unit;
            if (u == "ms") return a.number * (b / 60000.0);
            if (u == "s")  return a.number * b / 60.0;
            if (u == "hz") return (1000.0 / a.number) * (b / 60000.0);
            if (u == "%")  return a.number / 100.0;
            return a.number;
        }
        if (a.kind == ParseValue::Kind::String) {
            const juce::String l = a.str.toLowerCase();
            double hz = 0.0;
            if (parseFrequencyTextHz(l, hz))
                return (1000.0 / hz) * (b / 60000.0);
            if (l.length() >= 2 && l.length() <= 3) {
                const auto c0    = l[0];
                const auto cLast = l[l.length() - 1];
                const bool isNote = (c0 >= 'a' && c0 <= 'g')
                                 && (cLast >= '0' && cLast <= '9');
                if (isNote) {
                    const int midi = curlop::pitch::noteToMidi(a.str);
                    if (midi >= 0) {
                        hz = curlop::pitch::semiToHz(midi);
                        if (hz > 0) return (1000.0 / hz) * (b / 60000.0);
                    }
                }
            }
            return -1.0;
        }
        return -1.0;
    };

    const double real = resolveAt(step.stepUnitArg, bpm);
    const double ref  = resolveAt(step.stepUnitArg, REF);
    if (real >= 0.0 && ref > 0.0) return real / ref;
    return 1.0;
}

// ── stretchByStepBpm: mirror BytecodeCompiler.js:1356-1359 + :1010-1014.
// Non-persistent step-level `bpm:X` processor.
double applyStepBpmStretch(const AstNode& step, double globalBpm)
{
    for (const auto& p : step.processors) {
        if (p.persistent) continue;
        if (p.name != "bpm") continue;
        if (p.args.empty()) break;
        const double stepBpm = getNumVal(p.args[0], 0.0);
        if (stepBpm > 0.0) return globalBpm / stepBpm;
        break;
    }
    return 1.0;
}

// ── NON_MODULE_PARAMS — params that never become PARAM_LOCK. ──────────────
const std::unordered_set<juce::String>& nonModuleParams()
{
    static const std::unordered_set<juce::String> s = { "BPM", "PROB", "STEP", "LEN", "ONSET",
                                                        "DEVIATE" };
    return s;
}

// ── Scope op names. ────────────────────────────────────────────────────────
const std::unordered_set<juce::String>& scopeOpNames()
{
    static const std::unordered_set<juce::String> s = {
        "transpose", "octave", "reverse", "rotate", "shuffle", "groove",
        "invert", "timescale", "sort", "grid", "scale", "fit",
        // F-071 T-571b — `bpm` on a NESTED bracket is a scope-local tempo op
        // (emits SCOPE_BPM). The ROOT $out-sequence bpm is skipped in compileV2's
        // seqScopeOps loop (it's the global tempo, handled by extractGlobalSetters
        // before this set is consulted), so adding it here only affects nested
        // brackets via collectImplicitScopeOps.
        "bpm",
    };
    return s;
}

const std::unordered_set<juce::String>& nonInheritableOps()
{
    static const std::unordered_set<juce::String> s = {
        "transpose", "octave", "reverse", "rotate", "shuffle", "groove",
        "invert", "timescale", "sort", "grid", "scale", "fit",
        "bpm", "seed", "step", "repeat",
    };
    return s;
}

const std::unordered_set<juce::String>& nonLockRouteOps()
{
    static const std::unordered_set<juce::String> s = {
        "ratchet", "flam", "buzz", "bounce", "geiger", "len", "glide",
        "prob", "probability", "cond", "bernoulli", "arp", "strum",
        "onset", "vel", "velocity", "repeat",
        "gate", "note", "freq",
    };
    return s;
}

// T-052 (0.D-3c): collect outer step processors that should propagate to
// inner scopes. JS BytecodeCompiler.js:602. Filters persistent/dot-param/
// non-inheritable ops.
std::vector<Processor> collectInheritableOps(const AstNode& step)
{
    std::vector<Processor> out;
    for (const auto& p : step.processors) {
        if (p.persistent || p.isDotParam) continue;
        if (nonInheritableOps().count(p.name)) continue;
        out.push_back(p);
    }
    return out;
}

// ── matchesScope — right-dominant hierarchy. ──────────────────────────────
bool matchesScope(const Processor& p, int eventIndex, int commaGroup)
{
    if (p.afterEventCount >= 0 && eventIndex >= 0 && eventIndex >= p.afterEventCount) return false;
    if (commaGroup >= 0 && p.commaGroup >= 0 && p.commaGroup != commaGroup) return false;
    return true;
}

// ── extractProcessor: findLast matching name(s) in scope. ─────────────────
const Processor* extractProcessor(const std::vector<Processor>& procs,
                                  std::initializer_list<const char*> names,
                                  int commaGroup = -1, int eventIndex = -1)
{
    for (auto it = procs.rbegin(); it != procs.rend(); ++it) {
        bool match = false;
        for (const char* n : names) {
            if (it->name == n || it->canonicalName == n) { match = true; break; }
        }
        if (!match) continue;
        if (!matchesScope(*it, eventIndex, commaGroup)) continue;
        return &(*it);
    }
    return nullptr;
}

// ── Processor arg helpers. ─────────────────────────────────────────────────
ParseValue firstArg(const Processor& p)
{
    if (p.args.empty()) return ParseValue{};
    return p.args[0];
}

// ── Extractors. ───────────────────────────────────────────────────────────
struct ProbResult { bool present; double value; };
ProbResult extractProb(const std::vector<Processor>& procs, int cg = -1, int ei = -1)
{
    const Processor* p = extractProcessor(procs, {"prob", "probability"}, cg, ei);
    if (!p) return {false, 0.0};
    const ParseValue arg = firstArg(*p);
    return {true, getNumVal(arg, 1.0)};
}

struct RatchetResult { bool present; int count; };
RatchetResult extractRatchet(const std::vector<Processor>& procs, int cg, double bpm, int ei)
{
    const Processor* p = extractProcessor(procs, {"ratchet"}, cg, ei);
    if (!p) return {false, 0};
    const auto arg = p->args.empty() ? ParseValue{} : p->args[0];
    const double raw = (arg.kind == ParseValue::Kind::Null) ? 2.0 : resolveValue(arg, "count", bpm, 2.0);
    if (!std::isfinite(raw)) return {false, 0};   // B-307 — partial arg mid-typing
    return {true, std::max(1, static_cast<int>(std::floor(raw)))};
}

struct FlamResult { bool present; double offset; double gain; };
FlamResult extractFlam(const std::vector<Processor>& procs, int cg, double bpm, int ei)
{
    const Processor* p = extractProcessor(procs, {"flam"}, cg, ei);
    if (!p) return {false, 0.0, 0.0};
    const auto arg0 = p->args.size() > 0 ? p->args[0] : ParseValue{};
    const auto arg1 = p->args.size() > 1 ? p->args[1] : ParseValue{};
    const double offset = (arg0.kind == ParseValue::Kind::Null) ? 0.05
                        : resolveValue(arg0, "time", bpm, 0.05);
    if (!std::isfinite(offset) || offset == 0.0) return {false, 0.0, 0.0};   // B-307
    const double gain = (arg1.kind == ParseValue::Kind::Null) ? 0.5
                      : resolveValue(arg1, "unit", bpm, 0.5);
    if (!std::isfinite(gain)) return {false, 0.0, 0.0};                      // B-307
    return {true, offset, gain};
}

// F-071 T-516 — bounce gains an authorable interval (arg 1) with a unit domain:
// 0 = none (runtime derives the legacy 0.4×stepDur, BPM-relative); 1 = beats
// (BPM-relative, scales w/ timescale); 2 = seconds (absolute physics-time,
// immune to timescale — the real-ball-bounce bypass). ms/s → absolute seconds;
// bare/fraction/% → BPM-relative beats.
struct BounceResult { bool present; double gravity; float interval; uint8_t intervalDomain; };
BounceResult extractBounce(const std::vector<Processor>& procs, int cg, int ei)
{
    const Processor* p = extractProcessor(procs, {"bounce"}, cg, ei);
    if (!p) return {false, 0.0, 0.0f, 0};
    const auto arg = p->args.empty() ? ParseValue{} : p->args[0];
    const double g = (arg.kind == ParseValue::Kind::Null) ? 0.5 : resolveValue(arg, "unit", 120.0, 0.5);
    // B-307 — a partial arg mid-typing ('bounce:-') resolves to NaN; a NaN
    // gravity operand spun the audio thread forever. Non-finite = absent.
    if (!std::isfinite(g) || g == 0.0) return {false, 0.0, 0.0f, 0};

    float interval = 0.0f; uint8_t domain = 0;
    const ParseValue iv = p->args.size() > 1 ? p->args[1] : ParseValue{};
    if (iv.kind == ParseValue::Kind::UnitNumber && (iv.unit == "ms" || iv.unit == "s")) {
        interval = (float) (iv.unit == "ms" ? iv.number / 1000.0 : iv.number);  // seconds
        domain = 2;                                                             // absolute
    } else if (iv.kind == ParseValue::Kind::UnitNumber && iv.unit == "%") {
        interval = (float) (iv.number / 100.0);                                 // beats
        domain = 1;
    } else if (iv.kind == ParseValue::Kind::Number || iv.kind == ParseValue::Kind::UnitNumber) {
        interval = (float) iv.number;                                           // bare/fraction beats
        domain = 1;
    }
    if (domain != 0 && (!std::isfinite(interval) || interval <= 0.0f)) { interval = 0.0f; domain = 0; }
    return {true, g, interval, domain};
}

struct BuzzResult { bool present; double pressure; double durationBeats; };
BuzzResult extractBuzz(const std::vector<Processor>& procs, int cg, double bpm, int ei)
{
    const Processor* p = extractProcessor(procs, {"buzz"}, cg, ei);
    if (!p) return {false, 0.0, 0.0};
    const auto arg = p->args.empty() ? ParseValue{} : p->args[0];
    const double pressure = (arg.kind == ParseValue::Kind::Null) ? 0.5 : resolveValue(arg, "unit", bpm, 0.5);
    if (!std::isfinite(pressure) || pressure == 0.0) return {false, 0.0, 0.0};   // B-307
    const double durationBeats = 300.0 * bpm / 60000.0;
    return {true, pressure, durationBeats};
}

struct GeigerResult { bool present; double density; uint32_t seed; };
GeigerResult extractGeiger(const std::vector<Processor>& procs, double bpm, int cg, int ei)
{
    const Processor* p = extractProcessor(procs, {"geiger"}, cg, ei);
    if (!p) return {false, 0.0, 0};
    const auto arg = p->args.empty() ? ParseValue{} : p->args[0];
    const double density = (arg.kind == ParseValue::Kind::Null) ? 0.5 : resolveValue(arg, "unit", bpm, 0.5);
    if (density == 0.0) return {false, 0.0, 0};
    // Note: JS uses Math.random() — non-deterministic. C++ uses step offset as
    // pseudo-seed for oracle-purposes; actual audio path seeds separately.
    const uint32_t seed = 1;  // placeholder; divergence expected vs JS random seed.
    return {true, density, seed};
}

struct ArpResult { bool present; juce::String type; double speed; };
ArpResult extractArp(const std::vector<Processor>& procs, double stepDuration,
                     int cg, int ei, double bpm)
{
    const Processor* p = extractProcessor(procs, {"arp"}, cg, ei);
    if (!p) return {false, "up", 0.0};
    const auto arg0 = p->args.size() > 0 ? p->args[0] : ParseValue{};
    const auto arg1 = p->args.size() > 1 ? p->args[1] : ParseValue{};
    juce::String type = "up";
    if (arg0.kind == ParseValue::Kind::String) type = arg0.str;
    double speed = stepDuration / 4.0;
    if (arg1.kind != ParseValue::Kind::Null) {
        // Rule 6: bare = steps (raw beats through "time"); ms/s/hz convert
        // against the REAL tempo (was hardcoded 120 — units mis-scaled at
        // any other bpm; s491 universal bare-as-steps sweep).
        speed = resolveValue(arg1, "time", bpm, stepDuration / 4.0);
    }
    return {true, type, speed};
}

struct LenResult { bool present; double beats; };
LenResult extractLen(const std::vector<Processor>& procs, double stepDuration, double bpm, int cg, int ei)
{
    const Processor* p = extractProcessor(procs, {"len"}, cg, ei);
    if (!p) return {false, 0.0};
    const auto arg = p->args.empty() ? ParseValue{} : p->args[0];
    if (arg.kind == ParseValue::Kind::UnitNumber) {
        return {true, resolveValue(arg, "time", bpm, 0.0)};
    }
    // Mirror JS extractLen — `resolveValue(raw, "raw", bpm) * stepDuration`.
    // For Generator arg, resolveValue → NaN → NaN * stepDuration = NaN.
    return {true, getNumValOrNaN(arg, 1.0) * stepDuration};
}

double extractOnset(const std::vector<Processor>& eventProcs,
                    const std::vector<Processor>& stepProcs, double bpm)
{
    const Processor* p = extractProcessor(eventProcs, {"onset"});
    if (!p) p = extractProcessor(stepProcs, {"onset"});
    if (!p) return 0.0;
    const auto arg = p->args.empty() ? ParseValue{} : p->args[0];
    return resolveValue(arg, "time", bpm, 0.0);
}

struct GlideResult { bool present; double timeBeats; };
GlideResult extractGlide(const std::vector<Processor>& procs, double bpm, int cg, int ei)
{
    const Processor* p = extractProcessor(procs, {"glide"}, cg, ei);
    if (!p) return {false, 0.0};
    const auto arg = p->args.empty() ? ParseValue{} : p->args[0];
    return {true, resolveValue(arg, "time", bpm, 0.1)};
}

double extractVelocity(const std::vector<Processor>& procs)
{
    for (const auto& p : procs) {
        const juce::String upper = p.name.toUpperCase();
        if ((upper == "VELOCITY" || upper == "VEL") && !p.args.empty()) {
            // Mirror JS BytecodeCompiler.js:3646 — `getNumVal(proc.args[0])` with
            // no fallback; non-numeric (generator) → NaN at emit.
            return getNumValOrNaN(p.args[0], 0.7);
        }
    }
    return 0.7;
}

int getRepeatCount(const std::vector<Processor>& procs)
{
    for (const auto& p : procs) {
        if (p.name == "repeat" && !p.args.empty()) {
            return std::max(1, static_cast<int>(std::floor(getNumVal(p.args[0], 1.0))));
        }
    }
    return 1;
}

// ── extractCond: full COND type coverage. ─────────────────────────────────
struct CondResult {
    bool present = false;
    bool dynamic = false;
    juce::String type;           // "mod" | "first" | "!first" | "even" | "odd" | "prime" |
                                  // "fib" | "loops" | "!loops" | "after" | "once" (internal) |
                                  // "expr" | "previous" | "!previous"
    int cycle = 0;
    double value = 0.0;           // mod divisor / after threshold / single loop
    std::vector<double> loops;    // for loops / !loops / loopSet
    ParseValue expr;              // dynamic cond(expr) fire-time signal expression
};

CondResult extractCond(const std::vector<Processor>& procs, int cg = -1, int ei = -1)
{
    const Processor* p = extractProcessor(procs, {"cond"}, cg, ei);
    if (!p || p->args.empty()) return {};
    const ParseValue& arg = p->args[0];
    if (arg.kind != ParseValue::Kind::Cond || !arg.cond) return {};
    const CondNode& cn = *arg.cond;

    CondResult r;
    r.present = true;
    r.cycle = cn.cycle;
    const juce::String& ct = cn.condType;

    if (ct == "expr") {
        r.type = "expr";
        r.dynamic = true;
        r.expr = cn.expr;
        return r;
    }

    if (ct == "first" || ct == "!first" || ct == "even" || ct == "odd"
        || ct == "prime" || ct == "fib" || ct == "previous"
        || ct == "!previous") {
        r.type = ct;
        return r;
    }
    if (ct == "once") {
        if (cn.loops.empty()) { r.present = false; return r; }
        r.type = "loops";
        r.loops = { cn.loops[0] };
        return r;
    }
    if (ct == "after") {
        if (cn.loops.empty()) { r.present = false; return r; }
        r.type = "after";
        r.value = cn.loops[0];
        return r;
    }
    if (ct == "loops") {
        if (cn.loops.size() == 1) {
            r.type = "mod";
            r.value = cn.loops[0];
            return r;
        }
        r.type = "loops";
        r.loops = cn.loops;
        return r;
    }
    if (ct == "!loops") {
        r.type = "!loops";
        r.loops = cn.loops;
        return r;
    }
    r.present = false;
    return r;
}

// ── ParamLock + GeneratorParamLock extraction. ────────────────────────────
struct ParamLock { double value; int presence; };

std::vector<std::pair<juce::String, ParamLock>>
extractParamLocks(const AstNode& event, double bpm = 120.0)
{
    std::vector<std::pair<juce::String, ParamLock>> out;
    for (const auto& p : event.processors) {
        if (!p.isDotParam || p.args.empty()) continue;
        const juce::String name = p.name.toUpperCase();
        if (nonModuleParams().count(name)) continue;
        const ParseValue& v = p.args[0];
        if (v.kind == ParseValue::Kind::Generator
            || v.kind == ParseValue::Kind::SignalChain)
            continue;
        const int presence = (v.kind == ParseValue::Kind::UnitNumber && v.unit == "%") ? 2 : 1;
        out.push_back({name, {resolveValue(v, "param", bpm), presence}});
    }
    return out;
}

static bool isStreamRouteValue(const ParseValue& v)
{
    return (v.kind == ParseValue::Kind::Generator && v.gen)
        || (v.kind == ParseValue::Kind::SignalChain && v.signalChain);
}

std::vector<std::pair<juce::String, ParseValue>>
extractStreamParamLocks(const AstNode& event)
{
    std::vector<std::pair<juce::String, ParseValue>> out;
    for (const auto& p : event.processors) {
        if (!p.isDotParam || p.args.empty()) continue;
        const juce::String name = p.name.toUpperCase();
        if (nonModuleParams().count(name)) continue;
        const ParseValue& v = p.args[0];
        if (isStreamRouteValue(v))
            out.push_back({name, v});
    }
    return out;
}

// ── Generator emit — 10 types. ────────────────────────────────────────────
int getLfoWaveformId(const juce::String& s)
{
    if (s == "sine") return 0;
    if (s == "triangle" || s == "tri") return 1;
    if (s == "saw") return 2;
    if (s == "square") return 3;
    if (s == "random" || s == "sh") return 4;
    return 0;
}

// ── makeGenKey — stable key for dedup (JSON-like). ────────────────────────
juce::String buildGenKey(const GeneratorNode& g);
juce::String buildSignalChainKey(const SignalChainNode& chain);

juce::var buildParseValueKeyVar(const ParseValue& a)
{
    if (a.kind == ParseValue::Kind::Number)
        return a.number;
    if (a.kind == ParseValue::Kind::UnitNumber) {
        juce::var ua = new juce::DynamicObject();
        ua.getDynamicObject()->setProperty("v", a.number);
        ua.getDynamicObject()->setProperty("u", a.unit);
        return ua;
    }
    if (a.kind == ParseValue::Kind::String)
        return a.str;
    if (a.kind == ParseValue::Kind::Generator && a.gen)
        return buildGenKey(*a.gen);
    if (a.kind == ParseValue::Kind::SignalChain && a.signalChain)
        return buildSignalChainKey(*a.signalChain);
    return {};
}

juce::String buildGenKey(const GeneratorNode& g)
{
    juce::var obj = new juce::DynamicObject();
    obj.getDynamicObject()->setProperty("type", g.generatorType);
    juce::Array<juce::var> argsArr;
    for (const auto& a : g.args)
        argsArr.add(buildParseValueKeyVar(a));
    obj.getDynamicObject()->setProperty("args", argsArr);
    return juce::JSON::toString(obj, true);
}

juce::String buildSignalChainKey(const SignalChainNode& chain)
{
    juce::var obj = new juce::DynamicObject();
    obj.getDynamicObject()->setProperty("type", "signalChain");
    obj.getDynamicObject()->setProperty("source",
        chain.source ? buildGenKey(*chain.source) : juce::String());
    juce::Array<juce::var> opsArr;
    for (const auto& op : chain.ops) {
        juce::var opObj = new juce::DynamicObject();
        opObj.getDynamicObject()->setProperty("name", op.name);
        juce::Array<juce::var> argsArr;
        for (const auto& a : op.args)
            argsArr.add(buildParseValueKeyVar(a));
        opObj.getDynamicObject()->setProperty("args", argsArr);
        opsArr.add(opObj);
    }
    obj.getDynamicObject()->setProperty("ops", opsArr);
    return juce::JSON::toString(obj, true);
}

// ── Collect trigger events incl. inside Sequence events. ──────────────────
std::vector<const AstNode*> collectTriggerEvents(const std::vector<const AstNode*>& stepEvents)
{
    std::vector<const AstNode*> triggers;
    for (const auto* e : stepEvents) {
        if (e && e->eventType == "trigger" && e->module.isNotEmpty())
            triggers.push_back(e);
    }
    if (triggers.size() <= 1) {
        const AstNode* seqEv = nullptr;
        for (const auto* e : stepEvents) if (e && e->type == "Sequence") { seqEv = e; break; }
        if (seqEv) {
            std::vector<const AstNode*> inner;
            for (const auto& innerStep : seqEv->steps) {
                if (!innerStep) continue;
                for (const auto& innerEv : innerStep->events) {
                    if (innerEv && innerEv->eventType == "trigger" && innerEv->module.isNotEmpty())
                        inner.push_back(innerEv.get());
                }
            }
            if (inner.size() > 1) triggers = std::move(inner);
        }
    }
    return triggers;
}

// ── applyArpOrder — reorder a source list per arp(type). ──────────────────
// up (default / unknown): as-is. down: reversed. updown: ping-pong
// (a,b,c,d → a,b,c,d,c,b). random: deterministic Fisher-Yates seeded by `seed`.
// Shared by the multi-trigger walk and the single-trigger chord-voicing walk
// (T-237) so the two paths can't drift.
template <typename T>
void applyArpOrder(std::vector<T>& v, const juce::String& arpType, uint32_t seed)
{
    if (v.size() <= 1) return;
    if (arpType == "down") {
        std::reverse(v.begin(), v.end());
    } else if (arpType == "updown") {
        if (v.size() > 2) {
            for (size_t i = v.size() - 2; i >= 1; --i) {
                const T elem = v[i];   // copy first — v.push_back may reallocate
                v.push_back(elem);
                if (i == 1) break;
            }
        }
    } else if (arpType == "random") {
        for (size_t ri = v.size() - 1; ri > 0; --ri) {
            const uint64_t h = (static_cast<uint64_t>(seed)
                              + static_cast<uint64_t>(ri) * 2654435761ULL) % (ri + 1);
            std::swap(v[ri], v[static_cast<size_t>(h)]);
        }
    }
}

// ── Compile-time ops. ─────────────────────────────────────────────────────
// (B-288 retired the last compile-time scope-op machinery — sort is the
// SORT machine op now; SPEC-016 forbids compile-time AST mutation.)

// ── buildMergedEventList — T-051 (0.D-3c) update→trigger fold. ────────────
// Merges step.events such that update-events with `.dotparam` processors are
// folded onto the preceding trigger event matching (module, commaGroup).
// AstNode is non-copyable, so merged leaves are held as owned unique_ptrs in
// `mergedOwned` and pointed-to from `outMerged`. JS BytecodeCompiler.js:1490-1512.
void buildMergedEventList(const std::vector<AstNodePtr>& stepEvents,
                          std::vector<std::unique_ptr<AstNode>>& mergedOwned,
                          std::vector<const AstNode*>& outMerged)
{
    outMerged.reserve(stepEvents.size());
    std::unordered_map<juce::String, size_t> lastTriggerByModuleCg;
    for (const auto& evPtr : stepEvents) {
        if (!evPtr) continue;
        const AstNode& ev = *evPtr;
        if (ev.eventType == "trigger" && ev.module.isNotEmpty()) {
            const juce::String key = ev.module + ":" + juce::String(ev.commaGroup);
            lastTriggerByModuleCg[key] = outMerged.size();
            outMerged.push_back(evPtr.get());
        } else if (ev.eventType == "update" && ev.module.isNotEmpty()) {
            const juce::String key = ev.module + ":" + juce::String(ev.commaGroup);
            auto it = lastTriggerByModuleCg.find(key);
            if (it != lastTriggerByModuleCg.end()) {
                const AstNode& base = *outMerged[it->second];
                auto merged = std::make_unique<AstNode>();
                merged->type       = base.type;
                merged->eventType  = base.eventType;
                merged->module     = base.module;
                merged->pitch      = base.pitch;
                merged->stackVoices = base.stackVoices;
                merged->commaGroup = base.commaGroup;
                merged->line       = base.line;
                merged->col        = base.col;
                merged->processors = base.processors;
                for (const auto& proc : ev.processors) {
                    // SF-052 — a `{}` voice stack on a second #ref of the same
                    // module (`#synth.c3.maj #synth.cutoff{...}`) is a per-voice
                    // lock, not a dot-param; keep it on the merge or the
                    // perVoiceParams flag never reaches the trigger event.
                    if (proc.isDotParam || proc.isVoiceStack)
                        merged->processors.push_back(proc);
                }
                outMerged[it->second] = merged.get();
                mergedOwned.push_back(std::move(merged));
            } else {
                outMerged.push_back(evPtr.get());
            }
        } else {
            outMerged.push_back(evPtr.get());
        }
    }
}

// B-299 — a step's own timescale postfix scales its SLOT (§5.5a: "scales
// ALL timing: step positions, durations, AND loop length"): the walk
// cursor and the loop length advance by duration x f, matching the scope
// span the emitters produce (duration x dslTimescaleOf + the machine op
// compressing the content inside).
double stepPostfixTimescale(const AstNode& step, double bpm)
{
    // Slot scaling applies to BAR-shaped steps only (Sequence/VariableRef
    // events — the whole step IS the scope the op covers). A timescale as
    // a bare trigger postfix binds to that trigger's implicit frame and
    // must not move the shared slot (§5 LAW 4: other modules' projections
    // stay byte-identical).
    const bool barShaped = std::any_of(step.events.begin(), step.events.end(),
        [](const AstNodePtr& e) {
            return e && (e->type == "Sequence" || e->type == "VariableRef");
        });
    if (! barShaped) return 1.0;
    auto scan = [&](const std::vector<Processor>& procs) -> double {
        for (const auto& p : procs)
            if (p.name == "timescale" && !p.args.empty()) {
                const double f = resolveValue(p.args[0], "rate", bpm, 1.0);
                if (f > 0.0) return f;
            }
        return 0.0;
    };
    const double fromStep = scan(step.processors);
    if (fromStep > 0.0) return fromStep;
    for (const auto& evPtr : step.events) {
        if (!evPtr) continue;
        const double f = scan(evPtr->processors);
        if (f > 0.0) return f;
    }
    return 1.0;
}

// B-299b — THE slot-length authority. Every place that asks "how long is
// this step's slot" (loop length, walk cursor, bracket/varref content
// sums) goes through here; computing it independently per site is how the
// timescale-doesn't-move-the-slot family kept reappearing (each fix
// covered some sites and the next nested shape leaked).
double effectiveStepSlotBeats(const AstNode& step, double bpm)
{
    const double scale = resolveStepUnitScale(step, bpm)
                       * applyStepBpmStretch(step, bpm);
    // Nested bracket shapes recurse: the parser's step.duration cannot see
    // INNER timescales ([$a / $a timescale:0.35] repeat:2 — the emitted
    // scope spans the compressed sum, but the parser duration spans the
    // raw one, leaving a silent tail in the loop). Brackets always parse
    // as Sequence EVENTS (parseSeqBody wraps them in a Step), so the
    // recursion lives on the event branch. Varrefs keep the parser slot:
    // their ratio mechanism fits the var into whatever slot is authored.
    double concurrentSequenceSpan = 0.0;
    bool hasSequenceEvent = false;
    for (const auto& evPtr : step.events) {
        if (evPtr && evPtr->type == "Sequence" && ! evPtr->steps.empty()) {
            hasSequenceEvent = true;
            double inner = 0.0;
            for (const auto& child : evPtr->steps)
                inner += child ? effectiveStepSlotBeats(*child, bpm) : 1.0;
            const int repN = std::max(getRepeatCount(step.processors),
                                      getRepeatCount(evPtr->processors));
            concurrentSequenceSpan = std::max(concurrentSequenceSpan,
                                              inner * repN);
        }
    }
    if (hasSequenceEvent)
        return concurrentSequenceSpan * scale * stepPostfixTimescale(step, bpm);
    return step.duration * scale * stepPostfixTimescale(step, bpm);
}

double computeLoopLength(const std::vector<AstNodePtr>& steps, double bpm = 120.0)
{
    double total = 0.0;
    for (const auto& s : steps) {
        if (!s) { total += 1.0; continue; }
        // Mirror JS oracle: `step.duration ?? 1` — only null/undefined falls
        // back. AstNode::duration defaults to 1.0 in-struct so explicit step:0
        // surfaces here as 0.0 and must propagate (compiler-bundle.js:2950).
        total += effectiveStepSlotBeats(*s, bpm);
    }
    return total;
}

// T-223 phase helpers for script::compile.

// H1: Locate the $out = [...] Sequence node, or nullptr if missing/wrong.
// Encapsulates the 3-condition match (Assignment + name=="out" + Sequence
// value). Returns the Sequence node directly; orchestrator handles HALT.
const AstNode* findOutSequenceNode(const ScriptAst& ast)
{
    for (const auto& s : ast.statements) {
        if (s && s->type == "Assignment" && s->name == "out"
            && s->value && s->value->type == "Sequence") {
            return s->value.get();
        }
    }
    return nullptr;
}

// H2: Walk PersistentSetters, classifying each into 4 buckets:
//   - @bpm/@seed: mutate scalars by reference (first-wins semantic)
//   - scope-ops:  globals.scopeOps  (matched against scopeOpNames())
//   - step-ops:   globals.stepOps   (proc setters not bpm/seed/scope)
//   - event-form: globals.moduleParams  (T-049 @#mod.param:value)
// Mirrors JS BytecodeCompiler.js:864-877 + :2843-2853.
struct GlobalSetters {
    std::vector<Processor>                                                stepOps;
    std::vector<std::pair<juce::String, std::vector<ParseValue>>>         scopeOps;
    std::vector<const AstNode*>                                           moduleParams;
    bool                                                                  bpmExplicit = false;
    BpmModulator                                                          bpmMod;  // T-519
    juce::String                                                          bpmModError;  // T-519 loud error
};

GlobalSetters extractGlobalSetters(const ScriptAst& ast,
                                   const CompileOptions& /*opts*/,
                                   double& bpm,
                                   uint32_t& seed)
{
    GlobalSetters g;
    bool seedSet = false;
    for (const auto& s : ast.statements) {
        if (!s || s->type != "PersistentSetter") continue;
        if (s->isSetterProcessor) {
            const auto& proc = s->setterProcessor;
            if (proc.name == "bpm") {
                if (!proc.args.empty() && !g.bpmExplicit) {
                    const ParseValue& a0 = proc.args[0];
                    if (a0.kind == ParseValue::Kind::Generator && a0.gen) {
                        // T-519 — bpm:~lfo(...) modulates the per-script tempo,
                        // LFO clocked by the master (CON-027: clock != target ->
                        // no feedback). ~lfo only for now; other gens fail loudly.
                        const GeneratorNode& gn = *a0.gen;
                        const auto sub = [&] (size_t i, const char* dom, double fb) {
                            return i < gn.args.size()
                                ? resolveValue(gn.args[i], dom, 120.0, fb) : fb;
                        };
                        if (gn.generatorType == "lfo") {
                            g.bpmMod.kind      = 1;
                            g.bpmMod.waveform  = (uint8_t) getLfoWaveformId(
                                gn.args.empty() ? juce::String("sine") : gn.args[0].str);
                            g.bpmMod.rateBeats = sub(1, "time",  1.0);
                            g.bpmMod.minBpm    = sub(2, "tempo", 120.0);
                            g.bpmMod.maxBpm    = sub(3, "tempo", 120.0);
                            bpm = 0.5 * (g.bpmMod.minBpm + g.bpmMod.maxBpm);  // fixed fallback
                        } else {
                            g.bpmModError = "bpm:~" + gn.generatorType
                                + " is not supported yet — only ~lfo modulates tempo (T-519)";
                        }
                    } else {
                        bpm = resolveValue(a0, "tempo", 120.0, bpm);
                    }
                    g.bpmExplicit = true;
                }
            } else if (proc.name == "seed") {
                if (!proc.args.empty() && !seedSet) {
                    seed = static_cast<uint32_t>(getNumVal(proc.args[0], static_cast<double>(seed)));
                    seedSet = true;
                }
            } else if (scopeOpNames().count(proc.name)) {
                g.scopeOps.emplace_back(proc.name, proc.args);
            } else {
                g.stepOps.push_back(proc);
            }
        } else if (s->setterEvent) {
            g.moduleParams.push_back(s->setterEvent.get());
        }
    }
    return g;
}

// H3: Resolve top-level timescale factor (JS BytecodeCompiler.js:2878-2889).
// Two-tier fallback: scan seqScopeOps for "timescale" then fallback to first
// step.processors carrying a timescale (mirrors the JS parser quirk where
// postfix `] timescale:N` sometimes lands on the inner step rather than the
// sequence). Returns 1.0 if no timescale is present.
double resolveTimescaleFactor(
    const std::vector<std::pair<juce::String, std::vector<ParseValue>>>& seqScopeOps,
    const AstNode& sequence,
    double bpm)
{
    const std::vector<ParseValue>* tsArgs = nullptr;
    for (const auto& [name, args] : seqScopeOps) {
        if (name == "timescale") { tsArgs = &args; break; }
    }
    // B-299 — no step-level fallback: a step's timescale scales that
    // step's SLOT (stepPostfixTimescale feeds computeLoopLength + the walk
    // cursor), which covers the promoted-bracket shape too. Promoting it
    // here double-applied the factor to the whole loop.
    if (!tsArgs) return 1.0;
    const double f = tsArgs->empty() ? 1.0 : resolveValue((*tsArgs)[0], "rate", bpm, 1.0);
    return f > 0 ? f : 1.0;
}

// T-569 — detect a top-level (root-scope) `timescale:~gen` and project it to a
// TimescaleModulator (a per-script tempo multiplier riding the clock, SPEC-018
// §4.6). Mirrors the @bpm:~lfo detection: ~lfo only for now, others fail loudly.
// Returns: 0 = no modulated timescale (static path), 1 = lfo modulator filled,
// -1 = unsupported gen (errMsg set). min/max are MULTIPLIERS (1.0 = unchanged).
int detectTimescaleModulator(
    const std::vector<std::pair<juce::String, std::vector<ParseValue>>>& seqScopeOps,
    double bpm, TimescaleModulator& mod, juce::String& errMsg)
{
    const std::vector<ParseValue>* tsArgs = nullptr;
    for (const auto& [name, args] : seqScopeOps)
        if (name == "timescale") { tsArgs = &args; break; }
    if (!tsArgs || tsArgs->empty()) return 0;
    const ParseValue& a0 = (*tsArgs)[0];
    if (a0.kind != ParseValue::Kind::Generator || !a0.gen) return 0;

    const GeneratorNode& gn = *a0.gen;
    const auto sub = [&] (size_t i, const char* dom, double fb) {
        return i < gn.args.size() ? resolveValue(gn.args[i], dom, bpm, fb) : fb;
    };
    if (gn.generatorType == "lfo") {
        mod.kind      = 1;
        mod.waveform  = (uint8_t) getLfoWaveformId(
            gn.args.empty() ? juce::String("sine") : gn.args[0].str);
        mod.rateBeats = sub(1, "time", 1.0);
        mod.minMul    = sub(2, "rate", 1.0);
        mod.maxMul    = sub(3, "rate", 1.0);
        return 1;
    }
    errMsg = "timescale:~" + gn.generatorType
           + " is not supported yet — only ~lfo modulates timescale (T-569)";
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════════
// F-066 Phase 5 (T-462) — machine-ISA emission (compileV2).
//
// Projects the script into one vm::Program per referenced module. The
// extract/resolve layer above is shared; only the EMIT layer differs:
//   - vm::ProgramBuilder instead of raw bytes (schema-validated)
//   - no jumps: conditionals/prob guard the next STEP; REPEAT unrolls
//   - pitch in semitones from middle C (Signal.h value model)
//   - Value-lane payloads in normalized ±1 wire units
//   - per-source-processor stamped roll seeds (shared across projections)
//   - generator routes are scope-clocked Ctrl* instructions + lane bindings;
//     ~midi is metadata; generator args to artics/vel/arp are errors
// ═══════════════════════════════════════════════════════════════════════════

namespace v2 {

using vm::ProgramBuilder;

// MIDI float from Hz (A4=440=MIDI 69); semis-from-middle-C = midi - 60.
double midiFromHz(double hz)
{
    if (hz <= 0.0) return 69.0;
    return 69.0 + 12.0 * std::log2(hz / 440.0);
}

// Resolve a pitch chain to semitones from middle C (fractional, carries cents).
double semisOfChain(const curlop::script::PitchChain& chain)
{
    const auto r = curlop::pitch::resolvePitch(chain);
    if (r.hasFreq && r.freq_hz > 0.0) return midiFromHz(r.freq_hz) - 60.0;
    if (r.root_semitone >= 0) return (double) r.root_semitone + r.cents_offset / 100.0 - 60.0;
    return 9.0;   // 440 Hz fallback, as the old emitter used
}

// Shared-per-compile state: seed stamping + module discovery.
struct SharedV2 {
    uint32_t globalSeed = 42;
    uint32_t counter = 0;
    std::unordered_map<const void*, uint32_t> seedByIdentity;
    std::unordered_map<std::string, uint32_t> seedByKey;   // keyless sites
    juce::String error;

    uint32_t stamp(const void* identity)
    {
        auto it = seedByIdentity.find(identity);
        if (it != seedByIdentity.end()) return it->second;
        const uint32_t s = mix(++counter);
        seedByIdentity[identity] = s;
        return s;
    }
    uint32_t stampKey(const std::string& key)
    {
        auto it = seedByKey.find(key);
        if (it != seedByKey.end()) return it->second;
        const uint32_t s = mix(++counter);
        seedByKey[key] = s;
        return s;
    }
    uint32_t mix(uint32_t n) const
    {
        uint64_t x = (uint64_t) globalSeed ^ ((uint64_t) n << 32);
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        x =  x ^ (x >> 31);
        const uint32_t s = (uint32_t) (x >> 32);
        return s == 0 ? 1u : s;
    }
    void fail(const juce::String& msg)
    {
        if (error.isNotEmpty()) error += "\n";
        error += msg;
    }
};

// Per-projection (per-module) emit context.
struct V2Ctx {
    juce::String target;
    ProgramBuilder* pb = nullptr;
    SharedV2* shared = nullptr;
    std::vector<juce::String>* laneParams = nullptr;
    std::vector<uint8_t>* laneInternal = nullptr;
    const std::unordered_map<juce::String, ModuleSchemaLite>* schemas = nullptr;
    double bpm = 120.0;
    const std::vector<Processor>*      globalStepOps = nullptr;
    const std::vector<const AstNode*>* globalModuleParams = nullptr;
    // B-314 — an arp/strum postfixed on the top-level $out sequence (`[...]
    // arp(up,1/16)`) lands on the sequence's processors, not a scope op nor a
    // global setter. It covers every step's content (§5 one-law); threaded here
    // so each step's arp dispatch can consult it as the lowest-priority source.
    const Processor* sequenceArp   = nullptr;
    double outerScale = 1.0;
    int walkDepth = 0;                 // 0 = top-level sequence steps
    int topStepOrdinal = -1;           // current top-level source step index
    std::vector<int>* stepOrdinals = nullptr;   // SF-059 FOLLOW map (per Step emit)
    std::vector<vm::Program::TimelineStep>* stepTimeline = nullptr;
    // Scope identity for ctrl-op dedup (SF-061 scope-as-clock: one Ctrl*
    // instruction per (scope, generator, lane)). Serial advances identically
    // in every projection (same walk over the same AST).
    int scopeSerial = 0;
    std::unordered_set<std::string>* ctrlEmitted = nullptr;   // per-projection
    // B-279 — routes written BEFORE an articulation op already emitted once
    // with the full step span; sub-step bodies must not re-emit them.
    const std::unordered_set<juce::String>* suppressRouteParams = nullptr;
    // B-298 — route tokens consumed per-copy inside a repeat unroll
    // (keyed param|sourceOrdinal: the same written token must not re-emit
    // through the sibling-event path). Per module projection.
    std::unordered_set<std::string> consumedRouteTokens;
    // SF-052 — accumulates the ALL_CAPS base names this module voice-stacks
    // ({}), copied to ModuleProgramV2::perVoiceParams after emission so the
    // declaration build expands per-voice rows. nullptr-safe.
    std::set<std::string>* perVoiceParams = nullptr;
    int* minVoices = nullptr;
    bool localOutputMode = false;
    const std::vector<juce::String>* inputSocketNames = nullptr;
    int hiddenLaneCounter = 0;
};

void recordTopLevelStepSpan(V2Ctx& c, double beatOffset, double slotBeats)
{
    if (c.walkDepth != 0 || c.stepTimeline == nullptr || c.topStepOrdinal < 0)
        return;
    if (!std::isfinite(beatOffset) || !std::isfinite(slotBeats) || slotBeats <= 0.0)
        return;

    c.stepTimeline->push_back({ (float) beatOffset, (float) slotBeats, c.topStepOrdinal });
}

uint16_t laneOf(V2Ctx& c, const juce::String& paramName)
{
    auto& lanes = *c.laneParams;
    for (size_t i = 0; i < lanes.size(); ++i)
        if (lanes[i] == paramName) return (uint16_t) i;
    lanes.push_back(paramName);
    if (c.laneInternal != nullptr)
        c.laneInternal->push_back(0);
    return (uint16_t) (lanes.size() - 1);
}

int inputSocketIndexOf(const V2Ctx& c, const juce::String& name)
{
    if (c.inputSocketNames == nullptr)
        return -1;
    for (size_t i = 0; i < c.inputSocketNames->size(); ++i)
        if ((*c.inputSocketNames)[i] == name)
            return (int) i;
    return -1;
}

uint16_t hiddenLaneOf(V2Ctx& c)
{
    const uint16_t lane = laneOf(c, "__V2_ARG_" + juce::String(c.hiddenLaneCounter++));
    if (c.laneInternal != nullptr && lane < c.laneInternal->size())
        (*c.laneInternal)[(size_t) lane] = 1;
    return lane;
}

void schemaRange(const V2Ctx& c, const juce::String& paramName,
                 double& mn, double& mx)
{
    mn = 0.0; mx = 1.0;
    if (c.schemas == nullptr) return;
    auto it = c.schemas->find(c.target);
    if (it == c.schemas->end()) return;
    for (const auto& pd : it->second.paramDescs)
        if (pd.name == paramName) { mn = pd.min; mx = pd.max; return; }
}

// T-468 — the target param's real-world unit from the source-derived schema.
juce::String findSchemaUnit(const V2Ctx& c, const juce::String& paramName)
{
    if (c.schemas == nullptr) return {};
    auto it = c.schemas->find(c.target);
    if (it == c.schemas->end()) return {};
    for (const auto& pd : it->second.paramDescs)
        if (pd.name == paramName) return pd.unit;
    return {};
}

bool schemaHasParameter(const V2Ctx& c, const juce::String& paramName)
{
    if (c.schemas == nullptr) return false;
    const auto it = c.schemas->find(c.target);
    if (it == c.schemas->end()) return false;
    for (const auto& pd : it->second.paramDescs)
        if (pd.name.equalsIgnoreCase(paramName)) return true;
    return false;
}

bool parseFrequencyHz(const ParseValue& v, double& hzOut)
{
    if (v.kind == ParseValue::Kind::UnitNumber && v.unit == "hz") {
        hzOut = v.number;
        return hzOut > 0.0;
    }
    if (v.kind != ParseValue::Kind::String)
        return false;

    return parseFrequencyTextHz(v.str, hzOut);
}

// B-304 — the target param's declared scale ("log"/"logarithmic" = curved).
bool schemaScaleIsLog(const V2Ctx& c, const juce::String& paramName)
{
    if (c.schemas == nullptr) return false;
    auto it = c.schemas->find(c.target);
    if (it == c.schemas->end()) return false;
    for (const auto& pd : it->second.paramDescs)
        if (pd.name == paramName) return pd.scale.startsWith("log");
    return false;
}

// Absolute param value → normalized ±1 against the declared range.
// B-304 — the wire is KNOB-POSITION domain (ADR adr-vm "the declaration is
// the converter"): Delivery's Exp edge maps position → min·(max/min)^pos for
// log params, so an absolute value inverts through the curve here. % offsets
// (normDelta) stay positional fractions and need no curve — summing on the
// normalized wire and converting once at the edge IS the curved-space sum.
float normAbs(const V2Ctx& c, const juce::String& paramName, double v)
{
    double mn, mx;
    schemaRange(c, paramName, mn, mx);
    if (mx <= mn) return 0.0f;
    if (schemaScaleIsLog(c, paramName) && mn > 0.0) {
        const double vv = std::max(v, mn);
        return (float) (std::log(vv / mn) / std::log(mx / mn) * 2.0 - 1.0);
    }
    return (float) ((v - mn) / (mx - mn) * 2.0 - 1.0);
}

// Fraction-of-range delta → normalized delta (full range spans 2.0).
float normDelta(double frac) { return (float) (frac * 2.0); }

double noteHzOf(const ParseValue& v)
{
    if (v.kind != ParseValue::Kind::String) return -1.0;
    const int midi = curlop::pitch::noteToMidi(v.str);
    if (midi < 0) return -1.0;
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

float normSignalArg(const V2Ctx& c, const juce::String& paramName,
                    const ParseValue& v, double fallbackRaw)
{
    if (v.kind == ParseValue::Kind::Null)
        return (float) fallbackRaw;
    if (v.kind == ParseValue::Kind::UnitNumber && v.unit == "%")
        return normDelta(resolveValue(v, "param", c.bpm, 0.0));
    const double hz = noteHzOf(v);
    if (hz > 0.0)
        return normAbs(c, paramName, hz);
    const juce::String s = v.kind == ParseValue::Kind::String ? v.str.toLowerCase()
                                                              : juce::String();
    if (s.endsWith("hz") || s.endsWith("khz"))
        return normAbs(c, paramName, resolveValue(v, "param", c.bpm, fallbackRaw));
    return (float) resolveValue(v, "unit", c.bpm, fallbackRaw);
}

float normSignalRangeArg(const V2Ctx& c, const juce::String& paramName,
                         const ParseValue& v, double fallbackRaw)
{
    if (v.kind == ParseValue::Kind::Null)
        return (float) fallbackRaw;
    const double hz = noteHzOf(v);
    if (hz > 0.0)
        return normAbs(c, paramName, hz);
    const juce::String s = v.kind == ParseValue::Kind::String ? v.str.toLowerCase()
                                                              : juce::String();
    if (s.endsWith("hz") || s.endsWith("khz"))
        return normAbs(c, paramName, resolveValue(v, "param", c.bpm, fallbackRaw));
    return (float) resolveValue(v, "unit", c.bpm, fallbackRaw);
}

float signalTimeSeconds(const ParseValue& v, double bpm, double fallbackSeconds)
{
    if (v.kind == ParseValue::Kind::UnitNumber) {
        if (v.unit == "ms") return (float) (v.number / 1000.0);
        if (v.unit == "s")  return (float) v.number;
    }
    if (v.kind == ParseValue::Kind::String) {
        double seconds = 0.0;
        if (parseTimeTextSeconds(v.str, seconds))
            return (float) seconds;
    }
    const double beats = resolveValue(v, "time", bpm, fallbackSeconds * bpm / 60.0);
    return (float) (beats * 60.0 / bpm);
}

// DSL timescale factor (spec: 2 = twice as long) → machine divide factor.
double machineTimescale(double dslFactor)
{
    return dslFactor > 0.0 ? 1.0 / dslFactor : 1.0;
}

bool isGenArg(const ParseValue& v)
{
    return v.kind == ParseValue::Kind::Generator && v.gen != nullptr;
}

bool isSignalExprArg(const ParseValue& v)
{
    return (v.kind == ParseValue::Kind::Generator && v.gen != nullptr)
        || (v.kind == ParseValue::Kind::SignalChain && v.signalChain != nullptr);
}

const Processor* dynamicRepeatProcessorOf(const std::vector<Processor>& procs,
                                          int cg = -1, int ei = -1)
{
    const Processor* p = extractProcessor(procs, { "repeat" }, cg, ei);
    if (p == nullptr || p->args.empty() || !isSignalExprArg(p->args[0]))
        return nullptr;
    return p;
}

bool hasDynamicRepeatProcessor(const AstNode& step, const AstNode& event,
                               int cg = -1, int ei = -1)
{
    return dynamicRepeatProcessorOf(step.processors, cg, ei) != nullptr
        || dynamicRepeatProcessorOf(event.processors, -1, -1) != nullptr;
}

bool hasStatefulTrainProcessor(const AstNode& step, const AstNode& event,
                               int cg, int ei)
{
    static const std::unordered_set<juce::String> kTrainOps = {
        "ratchet", "arp", "buzz", "bounce", "geiger"
    };
    const auto scan = [&] (const std::vector<Processor>& procs,
                           bool matchScope) {
        for (const auto& p : procs)
            if (kTrainOps.count(p.name)
                && (!matchScope || matchesScope(p, ei, cg)))
                return true;
        return false;
    };
    return scan(step.processors, true) || scan(event.processors, false);
}

ParseValue numericParseValue(double v)
{
    ParseValue out;
    out.kind = ParseValue::Kind::Number;
    out.number = v;
    return out;
}

bool autoCurveIdOf(const juce::String& name, int& outCurveId)
{
    static const std::unordered_map<juce::String, int> kCurves = {
        {"lin", 0}, {"exp", 1}, {"log", 2}, {"eqpow", 3}
    };
    auto it = kCurves.find(name.toLowerCase());
    if (it == kCurves.end())
        return false;
    outCurveId = it->second;
    return true;
}

bool autoStringIsNumericValue(const juce::String& s)
{
    return curlop::pitch::noteToMidi(s) >= 0;
}

bool failUnsupportedAutoString(V2Ctx& c, const ParseValue& v)
{
    if (v.kind != ParseValue::Kind::String)
        return false;
    int ignored = 0;
    if (autoCurveIdOf(v.str, ignored) || autoStringIsNumericValue(v.str))
        return false;
    c.shared->fail("compile-error: V2_UNSUPPORTED_AUTO_ARG auto(...) string '"
                   + v.str
                   + "' is neither a supported curve nor a note value");
    return true;
}

// Diagnostic for the dropped generator-operand surface (B-186/B-187
// disposition, Neo s489): honest error instead of silent emptiness.
void failGenOperand(V2Ctx& c, const char* opName)
{
    c.shared->fail(juce::String("compile-error: generator argument to '")
                   + opName + "' is not supported yet — arrives with the "
                   "uniform operand-kind work (B-186/B-187).");
}

bool failIfDynamicSignalArgs(V2Ctx& c, const std::vector<ParseValue>& args,
                             const juce::String& owner)
{
    for (const auto& arg : args) {
        if (isSignalExprArg(arg)) {
            c.shared->fail("compile-error: dynamic expression argument to '" + owner
                           + "' is not supported by the current VM operand model yet");
            return true;
        }
    }
    return false;
}

// Reject generator args across an op's step→event→global chain.
bool anyGenArgInChain(V2Ctx& c, const AstNode& step, const AstNode& event,
                      int eventIdx, int cg,
                      std::initializer_list<const char*> names)
{
    auto probe = [&](const std::vector<Processor>& procs, int pcg, int pei) -> bool {
        const Processor* p = extractProcessor(procs, names, pcg, pei);
        if (!p) return false;
        for (const auto& a : p->args)
            if (isSignalExprArg(a)) return true;
        return false;
    };
    if (probe(step.processors, cg, eventIdx)) return true;
    if (probe(event.processors, -1, -1)) return true;
    if (c.globalStepOps && probe(*c.globalStepOps, -1, -1)) return true;
    return false;
}

struct FireExprV2 {
    std::vector<uint16_t> code;
    std::vector<float> immediates;
    bool dynamic = false;
    bool overflow = false;
    float midpoint = 0.0f;
};

bool compileFireExprV2(V2Ctx& c, const ParseValue& value, const char* domain,
                       double fallback, FireExprV2& out);

// ── Conds + prob (guard the NEXT step; no jumps, no patches). ─────────────
void emitCondV2(V2Ctx& c, const CondResult& cond)
{
    if (!cond.present) return;
    ProgramBuilder& pb = *c.pb;
    const juce::String& t = cond.type;
    if (cond.dynamic || t == "expr") {
        FireExprV2 expr;
        if (!compileFireExprV2(c, cond.expr, "unit", 0.0, expr))
            return;
        pb.condExpr(expr.code, expr.immediates);
    }
    else if (t == "mod")    pb.condMod((uint16_t) cond.value);
    else if (t == "first")  pb.condFirst((uint16_t) cond.cycle);
    else if (t == "!first") pb.condNotFirst((uint16_t) cond.cycle);
    else if (t == "even")   pb.condEven((uint16_t) cond.cycle);
    else if (t == "odd")    pb.condOdd((uint16_t) cond.cycle);
    else if (t == "prime")  pb.condPrime((uint16_t) cond.cycle);
    else if (t == "fib")    pb.condFib((uint16_t) cond.cycle);
    else if (t == "after")  pb.condAfter((uint16_t) cond.value);
    else if (t == "previous")  pb.condPrevious();
    else if (t == "!previous") pb.condNotPrevious();
    else if (t == "loops") {
        if (cond.loops.size() == 1) pb.condLoopEq((uint16_t) cond.loops[0]);
        else {
            std::vector<uint16_t> ls;
            for (double v : cond.loops) ls.push_back((uint16_t) v);
            pb.condLoopSet(ls);
        }
    } else if (t == "!loops") {
        std::vector<uint16_t> ls;
        for (double v : cond.loops) ls.push_back((uint16_t) v);
        pb.condNotLoopSet(ls);
    }
}

// Extract cond+prob through the chain and emit guards. The prob seed is
// stamped per source PROCESSOR — identical in every projection of the same
// source, distinct between two prob sites.
void emitTriggerCondProbV2(V2Ctx& c, const AstNode& step, const AstNode& event,
                           int eventIdx, int cg)
{
    auto cond = extractCond(step.processors, cg, eventIdx);
    if (!cond.present) cond = extractCond(event.processors);
    if (!cond.present && c.globalStepOps) cond = extractCond(*c.globalStepOps);
    emitCondV2(c, cond);

    const Processor* probProc = extractProcessor(step.processors, {"prob", "probability"}, cg, eventIdx);
    if (!probProc) probProc = extractProcessor(event.processors, {"prob", "probability"}, -1, -1);
    if (!probProc && c.globalStepOps)
        probProc = extractProcessor(*c.globalStepOps, {"prob", "probability"}, -1, -1);
    if (probProc) {
        const ParseValue arg = firstArg(*probProc);
        const uint32_t seed = c.shared->stamp(probProc);
        if (isSignalExprArg(arg)) {
            FireExprV2 expr;
            if (!compileFireExprV2(c, arg, "unit", 1.0, expr))
                return;
            c.pb->probDyn(expr.code, expr.immediates, seed);
        } else {
            const double v = getNumVal(arg, 1.0);
            if (v < 1.0)
                c.pb->prob((float) v, seed);
        }
    }
}

// SF-052 — the module's declared voice capacity (>=1), from the schema.
int moduleVoicesOf(const V2Ctx& c)
{
    if (c.schemas == nullptr) return 1;
    auto it = c.schemas->find(c.target);
    return it == c.schemas->end() ? 1 : std::max(1, it->second.voices);
}

// SF-052 — normalise a single lock value against param `name`'s schema. Shared
// by the scalar add() path and the {} voice-stack expansion so both obey the
// same T-468 time-typing + B-304 curve + % offset rules. isOffsetOut = true
// for `%` (offset) values.
float normLockValue(const V2Ctx& c, const juce::String& name, const ParseValue& v,
                    double stepDurationBeats, bool& isOffsetOut,
                    param::AuthoredValue* typedOut = nullptr)
{
    const bool isPct = (v.kind == ParseValue::Kind::UnitNumber && v.unit == "%");
    isOffsetOut = isPct;
    if (isPct)
        return normDelta(resolveValue(v, "param", c.bpm, 0.0));

    double mn = 0.0, mx = 1.0;
    schemaRange(c, name, mn, mx);
    const auto unit = findSchemaUnit(c, name).trim().toLowerCase();
    const bool targetIsTimingQuantity = unit == "ms" || unit == "s" || unit == "hz";
    const param::ParameterDeclaration target {
        mn,
        mx,
        unit.toStdString(),
        schemaScaleIsLog(c, name) ? param::Scale::Logarithmic : param::Scale::Linear,
    };

    const double legacyRaw = resolveValue(v, "param", c.bpm, 0.0);
    param::AuthoredValue authored {
        legacyRaw,
        param::AuthoredBasis::TargetUnit,
    };
    bool typedSyntax = false;
    if (v.kind == ParseValue::Kind::Number && targetIsTimingQuantity) {
        authored = { v.number, param::AuthoredBasis::ScriptSteps };
        typedSyntax = true;
    } else if (v.kind == ParseValue::Kind::UnitNumber
               && (v.unit == "step" || v.unit == "steps")) {
        authored = { v.number, param::AuthoredBasis::ScriptSteps };
        typedSyntax = true;
    } else if (v.kind == ParseValue::Kind::UnitNumber
               && (v.unit == "beat" || v.unit == "beats")) {
        authored = { v.number, param::AuthoredBasis::TempoBeats };
        typedSyntax = true;
    } else if (v.kind == ParseValue::Kind::UnitNumber && v.unit == "ms") {
        authored = { v.number / 1000.0, param::AuthoredBasis::WallClockSeconds };
        typedSyntax = true;
    } else if (v.kind == ParseValue::Kind::UnitNumber && v.unit == "s") {
        authored = { v.number, param::AuthoredBasis::WallClockSeconds };
        typedSyntax = true;
    } else {
        double hz = 0.0;
        double seconds = 0.0;
        if (parseFrequencyHz(v, hz)) {
            authored = { hz, param::AuthoredBasis::FrequencyHz };
            typedSyntax = true;
        } else if (v.kind == ParseValue::Kind::String
                   && parseTimeTextSeconds(v.str, seconds)) {
            authored = { seconds, param::AuthoredBasis::WallClockSeconds };
            typedSyntax = true;
        }
    }

    // An authoritative per-instance schema, including an intentionally empty
    // one, must reject typed writes to undeclared controls. Without this guard
    // laneOf would create an unbound symbolic lane and silently drop it.
    if (typedSyntax && c.schemas != nullptr
        && c.schemas->find(c.target) != c.schemas->end()
        && ! schemaHasParameter(c, name)) {
        if (c.shared != nullptr)
            c.shared->fail("compile-error: parameter '" + name
                           + "' is not declared by #" + c.target);
        return 0.0f;
    }

    const auto converted = param::convertSet(
        target, authored, param::TimingContext { c.bpm, stepDurationBeats });
    if (converted.ok)
    {
        if (typedOut != nullptr && typedSyntax)
            *typedOut = authored;
        return static_cast<float>(converted.normalized);
    }
    if (typedSyntax && c.shared != nullptr) {
        c.shared->fail("compile-error: parameter '" + name + "' on #" + c.target
                       + " rejects typed value: " + converted.error);
        return 0.0f;
    }
    return normAbs(c, name, legacyRaw);
}

// ── Param locks (normalized) + generator routes. ──────────────────────────
struct LockV2 {
    juce::String param; float value; bool isOffset;
    bool typed = false; param::AuthoredValue authored; float stepBeats = 1.0f;
};

std::vector<LockV2> collectLocksV2(V2Ctx& c, const AstNode& step,
                                   const AstNode& event, int eventIdx, int cg,
                                   double stepDurationBeats)
{
    std::vector<LockV2> out;
    if (event.stackVoices
        > static_cast<int>(
            std::numeric_limits<std::uint16_t>::max())) {
        c.shared->error =
            "compile-error: stack(...) count exceeds the current "
            "NOTE_CHORD encoding limit of 65535 voices";
        return out;
    }
    // B-280 (spec §5 rule 2): rightmost wins among duplicates of the same
    // param. Inline locks (event dot-form + step postfix) compete by
    // sourceOrdinal — a shared token-index ordinal, so written position
    // governs across both lists. Globals (@-prefix declarations) sit below
    // every inline lock; among themselves later declarations win.
    std::unordered_map<std::string, std::pair<size_t, int>> best;  // name -> (out idx, ord)
    // Rightmost-wins write into `out`, keyed by lane name (B-280).
    auto put = [&](const juce::String& laneName, float val, bool isOffset,
                   int ord, bool typed, param::AuthoredValue authored) {
        const std::string key = laneName.toStdString();
        const auto it = best.find(key);
        if (it != best.end() && ord <= it->second.second) return;
        if (it != best.end()) {
            out[it->second.first] = { laneName, val, isOffset, typed, authored,
                                      (float) stepDurationBeats };
            it->second.second = ord;
        } else {
            best.emplace(key, std::make_pair(out.size(), ord));
            out.push_back({ laneName, val, isOffset, typed, authored,
                            (float) stepDurationBeats });
        }
    };
    auto add = [&](const juce::String& name, const ParseValue& v, int ord) {
        if (isStreamRouteValue(v)) return;   // routes handle
        bool isPct = false;
        param::AuthoredValue authored;
        const float converted = normLockValue(c, name, v, stepDurationBeats, isPct, &authored);
        put(name, converted, isPct, ord, authored.basis != param::AuthoredBasis::TargetUnit, authored);
    };
    // SF-052 / V2 C18 — a `{}` polyphonic expression on `baseName`: one
    // per-voice lock per existing module voice on lane <BASE>_V<k>, value =
    // stack[k % L]. Short lists cycle; long lists prune to module voice
    // capacity. The shared base lane is intentionally NOT written (it would
    // sweep every voice). Marks the base param per-voice for this clip so the
    // declaration expands its rows.
    const int modVoices = std::max(moduleVoicesOf(c), event.stackVoices);
    auto addStack = [&](const juce::String& baseName,
                        const std::vector<ParseValue>& stack, int ord) {
        if (stack.empty()) return;
        const int L = (int) stack.size();
        const int n = std::max(1, modVoices);
        for (int k = 0; k < n; ++k) {
            const ParseValue& v = stack[(size_t) (k % L)];
            if (isStreamRouteValue(v)) continue;   // route per voice — later slice
            bool isPct = false;
            param::AuthoredValue authored;
            const float converted = normLockValue(c, baseName, v, stepDurationBeats, isPct, &authored);
            put(baseName + "_V" + juce::String(k), converted, isPct, ord,
                authored.basis != param::AuthoredBasis::TargetUnit, authored);
        }
        if (c.perVoiceParams != nullptr) c.perVoiceParams->insert(baseName.toStdString());
    };
    for (const auto& p : event.processors) {
        if (p.args.empty()) continue;
        // T-469 — decoupled step ops are never locks (also in dot form,
        // e.g. `#synth.note:e3`).
        if (nonLockRouteOps().count(p.name)) continue;
        const juce::String name = p.name.toUpperCase();
        if (nonModuleParams().count(name)) continue;
        if (p.isVoiceStack) { addStack(name, p.args, std::max(0, p.sourceOrdinal)); continue; }
        if (!p.isDotParam) continue;
        add(name, p.args[0], std::max(0, p.sourceOrdinal));
    }
    for (const auto& p : step.processors) {
        if (p.persistent || p.args.empty()) continue;
        const juce::String upper = p.name.toUpperCase();
        if (nonModuleParams().count(upper)) continue;
        if (scopeOpNames().count(p.name)) continue;
        if (!matchesScope(p, eventIdx, cg)) continue;
        if (nonLockRouteOps().count(p.name)) continue;
        add(upper, p.args[0], std::max(0, p.sourceOrdinal));
    }
    if (c.globalModuleParams) {
        int gOrd = -1000000;
        for (const AstNode* gEv : *c.globalModuleParams) {
            if (!gEv || gEv->module != c.target) continue;
            for (const auto& proc : gEv->processors) {
                if (!proc.isDotParam || proc.args.empty()) continue;
                add(proc.name.toUpperCase(), proc.args[0], ++gOrd);
            }
        }
    }
    return out;
}

void emitLocksV2(V2Ctx& c, const std::vector<LockV2>& locks)
{
    for (const auto& lk : locks)
        if (lk.typed)
            c.pb->paramLockTyped(laneOf(c, lk.param), (float) lk.authored.value,
                                 (uint8_t) lk.authored.basis, lk.stepBeats, lk.isOffset);
        else
            c.pb->paramLock(laneOf(c, lk.param), lk.value, lk.isOffset);
}

// Persistent module setters are source-level state, not event decorations:
// `@#delay.time:0.5s` must reach an audio-only delay even when the Script
// triggers only an upstream source. Emit them in the source's first step so
// PARAM_LOCK has its required fire context. Inline event locks are emitted
// later and therefore remain the deterministic winner.
void emitPersistentGlobalLocksV2(V2Ctx& c, double sourceStepDurationBeats,
                                 double holdDurationBeats)
{
    if (c.globalModuleParams == nullptr)
        return;
    bool openedSourceStep = false;
    for (const AstNode* event : *c.globalModuleParams) {
        if (event == nullptr || event->module != c.target)
            continue;
        for (const auto& proc : event->processors) {
            if (!proc.isDotParam || proc.args.empty()
                || nonModuleParams().count(proc.name.toUpperCase())
                || nonLockRouteOps().count(proc.name)
                || isStreamRouteValue(proc.args.front()))
                continue;
            if (!openedSourceStep) {
                c.pb->step(0.0, holdDurationBeats);
                openedSourceStep = true;
            }
            const auto name = proc.name.toUpperCase();
            bool isOffset = false;
            param::AuthoredValue authored;
            const auto value = normLockValue(c, name, proc.args.front(),
                                             sourceStepDurationBeats, isOffset,
                                             &authored);
            if (authored.basis != param::AuthoredBasis::TargetUnit)
                c.pb->paramLockTyped(laneOf(c, name), (float) authored.value,
                                     (uint8_t) authored.basis,
                                     (float) sourceStepDurationBeats, isOffset);
            else
                c.pb->paramLock(laneOf(c, name), value, isOffset);
        }
    }
}

// SF-061 scope lifetime: the scope a route is APPLIED to is its clock. A
// step-applied lock (event dot-form or step postfix) passes its step's span
// here; the route is wrapped in a step-sized scope so the machine's span
// clipping (renderSpanOf) anchors the op at step entry and releases it at
// step end (B-272). Scope/sequence-level routes pass nullptr and inherit
// the enclosing scope (streams across empty/rest steps — B-246 — and
// re-anchors at loop wrap — B-239).
struct StepRouteSpan { double beatOffset; double durBeats; };

enum class SignalExprDomain {
    ParamNorm,
    UnitRaw,
    PitchSignal,
    SemitoneOffset,
    OctaveOffset,
    Seconds,
    TimeBeats,
    LfoRateHz,
};

float signalExprScalarForDomain(const V2Ctx& c, const juce::String& paramName,
                                const ParseValue& v, SignalExprDomain domain,
                                double fallbackRaw)
{
    switch (domain) {
        case SignalExprDomain::ParamNorm:
            return normSignalRangeArg(c, paramName, v, fallbackRaw);
        case SignalExprDomain::PitchSignal: {
            if (v.kind == ParseValue::Kind::Null)
                return (float) fallbackRaw;
            const double hz = noteHzOf(v);
            if (hz > 0.0)
                return curlop::vm::noteToSignal(
                    (float) (curlop::pitch::noteToMidi(v.str) - 60));
            const juce::String s = v.kind == ParseValue::Kind::String
                ? v.str.toLowerCase() : juce::String();
            if (s.endsWith("hz") || s.endsWith("khz")) {
                const double h = resolveValue(v, "param", c.bpm, fallbackRaw);
                const double midi = 69.0 + 12.0 * std::log2(std::max(h, 1.0e-9) / 440.0);
                return curlop::vm::noteToSignal((float) (midi - 60.0));
            }
            return (float) resolveValue(v, "unit", c.bpm, fallbackRaw);
        }
        case SignalExprDomain::SemitoneOffset:
            return (float) resolveValue(v, "unit", c.bpm, fallbackRaw)
                 * curlop::vm::kSemitone;
        case SignalExprDomain::OctaveOffset:
            return (float) resolveValue(v, "unit", c.bpm, fallbackRaw)
                 * curlop::vm::kOctave;
        case SignalExprDomain::Seconds:
            return signalTimeSeconds(v, c.bpm, fallbackRaw);
        case SignalExprDomain::TimeBeats:
            return (float) resolveValue(v, "time", c.bpm, fallbackRaw);
        case SignalExprDomain::LfoRateHz:
            return (float) resolveValue(v, "lforate", c.bpm, fallbackRaw);
        case SignalExprDomain::UnitRaw:
            return (float) resolveValue(v, "unit", c.bpm, fallbackRaw);
    }
    return (float) fallbackRaw;
}

bool isSignalRangeLiteral(const ParseValue& v)
{
    if (v.kind == ParseValue::Kind::Number || v.kind == ParseValue::Kind::UnitNumber)
        return true;
    if (v.kind != ParseValue::Kind::String)
        return false;

    const juce::String s = v.str.trim().toLowerCase();
    if (s.endsWith("hz") || s.endsWith("khz"))
        return true;
    if (s.isEmpty() || s.length() > 4)
        return false;

    const juce::juce_wchar c0 = s[0];
    if (c0 < 'a' || c0 > 'g')
        return false;

    int idx = 1;
    if (idx < s.length() && (s[idx] == '#' || s[idx] == 'b'))
        ++idx;
    if (idx == s.length())
        return true; // root-only note name, e.g. `c`
    return idx + 1 == s.length() && s[idx] >= '0' && s[idx] <= '9';
}

bool isMusicalScaleSignalOp(const SignalChainOp& op)
{
    if (op.name != "scale" || op.args.size() < 2)
        return false;
    return op.args[1].kind == ParseValue::Kind::String
        && ! isSignalRangeLiteral(op.args[1]);
}

void emitCtrlSignalRouteV2(V2Ctx& c, const juce::String& paramName, const ParseValue& value,
                           const StepRouteSpan* stepScope = nullptr, bool perHit = false,
                           uint16_t forcedLane = 0xffff,
                           SignalExprDomain domain = SignalExprDomain::ParamNorm);

uint16_t emitDynamicArgLaneV2(V2Ctx& c, const juce::String& paramName,
                              const ParseValue& value, SignalExprDomain domain,
                              const StepRouteSpan* stepScope, bool perHit)
{
    if (! isSignalExprArg(value))
        return 0xffff;
    const uint16_t lane = hiddenLaneOf(c);
    if (lane == 0xffff)
        return lane;
    emitCtrlSignalRouteV2(c, paramName, value, stepScope, perHit, lane, domain);
    return lane;
}

uint16_t fireExprToken(vm::FireExprTag tag, uint8_t aux = 0)
{
    return (uint16_t) (((uint16_t) tag << 8) | aux);
}

uint8_t appendFireImmediateV2(FireExprV2& out, float value)
{
    if (out.immediates.size() >= 255) {
        out.overflow = true;
        return 0;
    }
    const uint8_t idx = (uint8_t) out.immediates.size();
    out.immediates.push_back(value);
    out.code.push_back(fireExprToken(vm::FireExprTag::Lit, idx));
    return idx;
}

bool failIfFireExprOverflow(V2Ctx& c, const FireExprV2& expr)
{
    if (!expr.overflow && expr.code.size() <= 255u)
        return false;
    c.shared->fail("compile-error: dynamic fire-time expression is too large "
                   "for the current bytecode operand format");
    return true;
}

bool compileFireExprV2(V2Ctx& c, const ParseValue& value, const char* domain,
                       double fallback, FireExprV2& out);

bool compileFireExprGeneratorV2(V2Ctx& c, const GeneratorNode& g, const char* domain,
                                double fallback, FireExprV2& out)
{
    const auto argAt = [&](size_t i) -> ParseValue {
        return i < g.args.size() ? g.args[i] : ParseValue{};
    };
    const auto compileArg = [&](const ParseValue& v, const char* dom,
                                double fb) -> bool {
        return compileFireExprV2(c, v, dom, fb, out);
    };

    if (g.generatorType == "lfo") {
        const juce::String wf = argAt(0).kind == ParseValue::Kind::String
                              ? argAt(0).str : juce::String();
        if (!compileArg(argAt(1), "lforate", c.bpm / 60.0)) return false;
        if (!compileArg(argAt(2), domain, 0.0)) return false;
        if (!compileArg(argAt(3), domain, 1.0)) return false;
        if (!compileArg(argAt(4), "unit", 0.0)) return false;
        out.code.push_back(fireExprToken(vm::FireExprTag::Lfo,
                                         (uint8_t) getLfoWaveformId(wf)));
        out.dynamic = true;
        out.midpoint = (float) ((resolveValue(argAt(2), domain, c.bpm, 0.0)
                               + resolveValue(argAt(3), domain, c.bpm, 1.0)) * 0.5);
        return true;
    }
    if (g.generatorType == "auto") {
        int curveId = 0;
        bool curveFound = false;
        std::vector<ParseValue> numeric;
        for (const auto& a : g.args) {
            if (failUnsupportedAutoString(c, a))
                return false;
            if (a.kind == ParseValue::Kind::String) {
                int candidateCurve = 0;
                if (autoCurveIdOf(a.str, candidateCurve)) {
                    if (!curveFound) { curveId = candidateCurve; curveFound = true; }
                } else if (autoStringIsNumericValue(a.str)) {
                    numeric.push_back(a);
                }
            } else {
                numeric.push_back(a);
            }
        }
        const ParseValue a = numeric.size() > 0 ? numeric[0] : ParseValue{};
        const ParseValue b = numeric.size() > 1 ? numeric[1] : ParseValue{};
        if (!compileArg(a, domain, 0.0)) return false;
        if (!compileArg(b, domain, 1.0)) return false;
        out.code.push_back(fireExprToken(vm::FireExprTag::Auto, (uint8_t) curveId));
        out.dynamic = true;
        out.midpoint = (float) ((resolveValue(a, domain, c.bpm, 0.0)
                               + resolveValue(b, domain, c.bpm, 1.0)) * 0.5);
        return true;
    }
    if (g.generatorType == "random") {
        if (!compileArg(argAt(0), domain, 0.0)) return false;
        if (!compileArg(argAt(1), domain, 1.0)) return false;
        if (!compileArg(argAt(2), "time", 0.0)) return false;
        out.code.push_back(fireExprToken(vm::FireExprTag::Random));
        out.dynamic = true;
        out.midpoint = (float) ((resolveValue(argAt(0), domain, c.bpm, 0.0)
                               + resolveValue(argAt(1), domain, c.bpm, 1.0)) * 0.5);
        return true;
    }
    if (g.generatorType == "bernoulli") {
        if (!compileArg(argAt(0), "unit", 0.5)) return false;
        if (!compileArg(argAt(1), domain, 0.0)) return false;
        if (!compileArg(argAt(2), domain, 1.0)) return false;
        out.code.push_back(fireExprToken(vm::FireExprTag::Bernoulli));
        out.dynamic = true;
        out.midpoint = (float) ((resolveValue(argAt(1), domain, c.bpm, 0.0)
                               + resolveValue(argAt(2), domain, c.bpm, 1.0)) * 0.5);
        return true;
    }
    if (g.generatorType == "__input") {
        const ParseValue nameArg = argAt(0);
        if (nameArg.kind != ParseValue::Kind::String) {
            c.shared->fail("compile-error: input socket read has no input name");
            return false;
        }
        const int idx = inputSocketIndexOf(c, nameArg.str);
        if (idx < 0) {
            c.shared->fail("compile-error: input socket '" + nameArg.str + "' is not exposed");
            return false;
        }
        if (idx > 255) {
            c.shared->fail("compile-error: fire-time input socket index exceeds current token format");
            return false;
        }
        out.code.push_back(fireExprToken(vm::FireExprTag::Input, (uint8_t) idx));
        out.dynamic = true;
        out.midpoint = (float) fallback;
        return true;
    }

    c.shared->fail("compile-error: dynamic fire-time expression does not support "
                   + g.generatorType + " yet");
    return false;
}

bool compileFireExprV2(V2Ctx& c, const ParseValue& value, const char* domain,
                       double fallback, FireExprV2& out)
{
    if (value.kind == ParseValue::Kind::Generator && value.gen) {
        if (! compileFireExprGeneratorV2(c, *value.gen, domain, fallback, out))
            return false;
        return ! failIfFireExprOverflow(c, out);
    }

    if (value.kind == ParseValue::Kind::SignalChain && value.signalChain) {
        const SignalChainNode& chain = *value.signalChain;
        if (!chain.source) {
            c.shared->fail("compile-error: fire-time signal chain has no source");
            return false;
        }
        if (!compileFireExprGeneratorV2(c, *chain.source, domain, fallback, out))
            return false;
        const auto argAt = [](const SignalChainOp& op, size_t i) -> ParseValue {
            return i < op.args.size() ? op.args[i] : ParseValue{};
        };
        for (const auto& op : chain.ops) {
            if (op.name == "clip") {
                if (!compileFireExprV2(c, argAt(op, 0), domain, -1.0, out)) return false;
                if (!compileFireExprV2(c, argAt(op, 1), domain,  1.0, out)) return false;
                out.code.push_back(fireExprToken(vm::FireExprTag::Clip));
            } else if (op.name == "invert") {
                if (!compileFireExprV2(c, argAt(op, 0), domain, 0.5, out)) return false;
                out.code.push_back(fireExprToken(vm::FireExprTag::Invert));
            } else if (op.name == "scale") {
                if (isMusicalScaleSignalOp(op)) {
                    c.shared->fail("compile-error: musical scale signal chain is render-domain only");
                    return false;
                }
                if (!compileFireExprV2(c, argAt(op, 0), domain, 0.0, out)) return false;
                if (!compileFireExprV2(c, argAt(op, 1), domain, 1.0, out)) return false;
                out.code.push_back(fireExprToken(vm::FireExprTag::Scale));
            } else if (op.name == "offset") {
                if (!compileFireExprV2(c, argAt(op, 0), domain, 0.0, out)) return false;
                out.code.push_back(fireExprToken(vm::FireExprTag::Offset));
            } else if (op.name == "transpose") {
                if (!compileFireExprV2(c, argAt(op, 0), "unit", 0.0, out)) return false;
                appendFireImmediateV2(out, curlop::vm::kSemitone);
                out.code.push_back(fireExprToken(vm::FireExprTag::Gain));
                out.code.push_back(fireExprToken(vm::FireExprTag::Offset));
            } else if (op.name == "octave") {
                if (!compileFireExprV2(c, argAt(op, 0), "unit", 0.0, out)) return false;
                appendFireImmediateV2(out, curlop::vm::kOctave);
                out.code.push_back(fireExprToken(vm::FireExprTag::Gain));
                out.code.push_back(fireExprToken(vm::FireExprTag::Offset));
            } else if (op.name == "gain") {
                if (!compileFireExprV2(c, argAt(op, 0), "unit", 1.0, out)) return false;
                out.code.push_back(fireExprToken(vm::FireExprTag::Gain));
            } else if (op.name == "abs") {
                out.code.push_back(fireExprToken(vm::FireExprTag::Abs));
            } else if (op.name == "smooth" || op.name == "slew") {
                c.shared->fail("compile-error: V2_UNSUPPORTED_FIRE_SMOOTH "
                               "fire-time signal-chain op '" + op.name
                               + "' needs stateful fire-expression VM support");
                return false;
            } else {
                c.shared->fail("compile-error: unknown fire-time signal-chain op '"
                               + op.name + "'");
                return false;
            }
        }
        out.dynamic = true;
        return ! failIfFireExprOverflow(c, out);
    }

    appendFireImmediateV2(out, (float) resolveValue(value, domain, c.bpm, fallback));
    out.midpoint = out.immediates.empty() ? (float) fallback : out.immediates.back();
    return ! failIfFireExprOverflow(c, out);
}

bool compileFireCountIntervalExprV2(V2Ctx& c, const ParseValue& countValue,
                                    double durationBeats, double fallbackCount,
                                    FireExprV2& out)
{
    appendFireImmediateV2(out, (float) durationBeats);
    if (failIfFireExprOverflow(c, out))
        return false;
    if (!compileFireExprV2(c, countValue, "count", fallbackCount, out))
        return false;
    out.code.push_back(fireExprToken(vm::FireExprTag::Divide));
    const double count = resolveValue(countValue, "count", c.bpm, fallbackCount);
    out.midpoint = (float) (count > 0.0 ? durationBeats / count : 0.0);
    return ! failIfFireExprOverflow(c, out);
}

// One Ctrl* instruction per (scope, generator, lane) — except step-scoped
// routes, which emit per step unconditionally: each step is its own scope,
// so two steps locking the same param each need their own instruction (and
// REPEAT unrolls must keep every iteration's copy).
void emitCtrlRouteV2(V2Ctx& c, const juce::String& paramName, const GeneratorNode& g,
                     const StepRouteSpan* stepScope = nullptr, bool perHit = false,
                     uint16_t forcedLane = 0xffff,
                     SignalExprDomain exprDomain = SignalExprDomain::ParamNorm)
{
    const uint16_t lane = forcedLane == 0xffff ? laneOf(c, paramName) : forcedLane;
    if (stepScope == nullptr && forcedLane == 0xffff) {
        const juce::String genKey = buildGenKey(g);
        const std::string dedup = std::to_string(c.scopeSerial) + "|"
                                + genKey.toStdString() + "|" + std::to_string(lane);
        if (c.ctrlEmitted->count(dedup)) return;
        c.ctrlEmitted->insert(dedup);
    }

    ProgramBuilder& pb = *c.pb;
    if (stepScope != nullptr)
        pb.scopeStart(stepScope->beatOffset, stepScope->durBeats);
    const auto argAt = [&](size_t i) -> ParseValue {
        return i < g.args.size() ? g.args[i] : ParseValue{};
    };
    auto isPct = [](const ParseValue& v) {
        return v.kind == ParseValue::Kind::UnitNumber && v.unit == "%";
    };
    // B-297 — §5.12 universal values: a NOTE-NAME range arg resolves to its
    // frequency in Hz (~auto(c5, c9) sweeps between those pitches; the
    // spec's own example is pitch automation). Non-note strings return <0.
    auto noteHz = [](const ParseValue& v) -> double {
        if (v.kind != ParseValue::Kind::String) return -1.0;
        const int midi = curlop::pitch::noteToMidi(v.str);
        if (midi < 0) return -1.0;
        return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
    };
    // min/max in normalized units: % → range-fraction delta; abs → ±1 map.
    auto normArg = [&](const ParseValue& v, double fallbackAbs) -> float {
        if (forcedLane != 0xffff || exprDomain != SignalExprDomain::ParamNorm)
            return signalExprScalarForDomain(c, paramName, v, exprDomain, fallbackAbs);
        if (v.kind == ParseValue::Kind::Null)
            return normAbs(c, paramName, fallbackAbs);
        if (isPct(v)) return normDelta(resolveValue(v, "param", c.bpm, 0.0));
        const double hz = noteHz(v);
        if (hz > 0.0) return normAbs(c, paramName, hz);
        return normAbs(c, paramName, resolveValue(v, "param", c.bpm, fallbackAbs));
    };
    // SPEC-014 lock layering + B-269: % range args make the route an OFFSET
    // (delta summed over the base — dotted-arc GUI layer); absolute args
    // make it the BASE (overrides the knob — abs-pin GUI layer). Mirrors
    // the ~midi r.isOffset rule: any % arg flips the route to offset mode.
    auto offsetModeOf = [&](std::initializer_list<size_t> rangeArgIdx) -> uint8_t {
        if (forcedLane != 0xffff) return 0;
        if (g.isOffset) return 1;
        for (size_t i : rangeArgIdx)
            if (i < g.args.size() && isPct(g.args[i])) return 1;
        return 0;
    };
    const juce::String& type = g.generatorType;

    if (type == "__input") {
        const ParseValue nameArg = argAt(0);
        if (nameArg.kind != ParseValue::Kind::String) {
            c.shared->fail("compile-error: input socket read has no input name");
            if (stepScope != nullptr)
                pb.scopeEnd();
            return;
        }
        const int idx = inputSocketIndexOf(c, nameArg.str);
        if (idx < 0) {
            c.shared->fail("compile-error: input socket '" + nameArg.str + "' is not exposed");
            if (stepScope != nullptr)
                pb.scopeEnd();
            return;
        }
        pb.ctrlInput(lane, (uint16_t) idx);
        if (stepScope != nullptr)
            pb.scopeEnd();
        return;
    }

    // F-071 T-495 — unit-domain tagging (SPEC-018 §1.2). `ms`/`s`/`hz`/`khz`
    // are ABSOLUTE (wall-clock, immune to timescale + tempo); bare numbers,
    // `%`, fractions, `steps` are BPM-relative (scale with the scope's
    // cumulative timescale). Returns true if the arg is authored absolute.
    auto isAbsoluteUnit = [](const ParseValue& v) -> bool {
        if (v.kind == ParseValue::Kind::UnitNumber) {
            const juce::String& u = v.unit;
            return u == "ms" || u == "s" || u == "hz" || u == "khz";
        }
        if (v.kind == ParseValue::Kind::String) {
            const juce::String l = v.str.toLowerCase();
            return l.endsWith("hz") || l.endsWith("khz");
        }
        return false;
    };
    // Envelope time arg → (value, absolute-flag). Absolute = stored in SECONDS;
    // BPM-relative = stored in BEATS (today's resolveValue "time" path).
    auto envTime = [&](const ParseValue& v, double fallbackBeats,
                       bool& absOut) -> float {
        if (v.kind == ParseValue::Kind::UnitNumber
            && (v.unit == "ms" || v.unit == "s")) {
            absOut = true;
            return (float) (v.unit == "ms" ? v.number / 1000.0 : v.number);
        }
        absOut = false;
        return (float) resolveValue(v, "time", c.bpm, fallbackBeats);
    };
    auto dynLane = [&](const ParseValue& v, SignalExprDomain domain) -> uint16_t {
        return emitDynamicArgLaneV2(c, paramName, v, domain, stepScope, perHit);
    };

    if (type == "__markov_value") {
        const int stateCount = (int) g.args.size();
        if (stateCount <= 0) {
            c.shared->fail("compile-error: Markov value source has no states");
            if (stepScope != nullptr)
                pb.scopeEnd();
            return;
        }
        if (stateCount > 255) {
            c.shared->fail("compile-error: Markov value source has too many states for the current bytecode operand");
            if (stepScope != nullptr)
                pb.scopeEnd();
            return;
        }

        std::vector<float> immediates;
        std::vector<uint16_t> lanes;
        immediates.reserve((size_t) stateCount);
        lanes.reserve((size_t) stateCount);
        bool offsetMode = g.isOffset;
        for (int i = 0; i < stateCount; ++i) {
            const ParseValue stateValue = argAt((size_t) i);
            if (isPct(stateValue))
                offsetMode = true;
            immediates.push_back(normArg(stateValue, 0.0));
            lanes.push_back(dynLane(stateValue, exprDomain));
        }

        pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::MarkovValue,
                   (uint8_t) stateCount, 0, c.shared->stamp(&g),
                   (uint8_t) (offsetMode ? 1 : 0),
                   immediates, lanes);
        if (stepScope != nullptr)
            pb.scopeEnd();
        return;
    }

    if (type == "__preset") {
        const bool interp =
            argAt(1).kind == ParseValue::Kind::String
            && (argAt(1).str.equalsIgnoreCase("interp")
                || argAt(1).str.equalsIgnoreCase("interpolate"));
        const int stateCount = std::max(0, (int) g.args.size() - 2);
        if (stateCount <= 0) {
            c.shared->fail("compile-error: preset value source has no states");
            if (stepScope != nullptr)
                pb.scopeEnd();
            return;
        }
        if (stateCount > 255) {
            c.shared->fail("compile-error: preset value source has too many states for the current bytecode operand");
            if (stepScope != nullptr)
                pb.scopeEnd();
            return;
        }

        std::vector<float> immediates;
        std::vector<uint16_t> lanes;
        immediates.reserve((size_t) stateCount + 1);
        lanes.reserve((size_t) stateCount + 1);
        immediates.push_back(signalExprScalarForDomain(c, paramName, argAt(0),
                                                       SignalExprDomain::UnitRaw, 0.0));
        lanes.push_back(dynLane(argAt(0), SignalExprDomain::UnitRaw));
        for (int i = 0; i < stateCount; ++i) {
            const ParseValue stateValue = argAt((size_t) i + 2);
            immediates.push_back(signalExprScalarForDomain(c, paramName, stateValue,
                                                           exprDomain, 0.0));
            lanes.push_back(dynLane(stateValue, exprDomain));
        }

        pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Preset,
                   (uint8_t) stateCount, interp ? 1 : 0, 0,
                   offsetModeOf({}),
                   immediates, lanes);
        if (stepScope != nullptr)
            pb.scopeEnd();
        return;
    }

    if (type == "lfo") {
        const juce::String wf = argAt(0).kind == ParseValue::Kind::String ? argAt(0).str : juce::String();
        // Optional 5th arg: phase as a cycle fraction (0.75 starts a sine at
        // its minimum). Bare number = fraction; % works too (25% = 0.25).
        const ParseValue& phaseArg = argAt(4);
        const float phase01 = (float) resolveValue(phaseArg, "unit", c.bpm, 0.0);
        const ParseValue& rateArg = argAt(1);
        const double hz = resolveValue(rateArg, "lforate", c.bpm,
                                       /*fallback: 1-step cycle*/ c.bpm / 60.0);
        const bool   rateAbs = isAbsoluteUnit(rateArg);
        // Absolute → emit Hz. BPM-relative → emit cycles-per-beat (tempo-
        // independent): cyc/beat = Hz × 60 / bpm (bare N → 1/N). The runtime
        // advances phase in beats × cumulative timescale (T-495).
        const float rateField = rateAbs ? (float) hz
                                        : (float) (hz * 60.0 / c.bpm);
        const uint16_t rateLane = dynLane(rateArg, SignalExprDomain::LfoRateHz);
        const uint16_t minLane  = dynLane(argAt(2), exprDomain);
        const uint16_t maxLane  = dynLane(argAt(3), exprDomain);
        const uint16_t phaseLane = dynLane(phaseArg, SignalExprDomain::UnitRaw);
        if (rateLane != 0xffff || minLane != 0xffff || maxLane != 0xffff
            || phaseLane != 0xffff) {
            const uint8_t flags = (uint8_t) (offsetModeOf({ 2, 3 })
                                  | ((rateLane == 0xffff && !rateAbs) ? 0x04 : 0));
            pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Lfo,
                       (uint8_t) getLfoWaveformId(wf), 0, 0, flags,
                       { rateLane == 0xffff ? rateField : (float) hz,
                         normArg(argAt(2), 0.0), normArg(argAt(3), 1.0), phase01 },
                       { rateLane, minLane, maxLane, phaseLane });
        } else {
            pb.ctrlLfo(lane, (uint8_t) getLfoWaveformId(wf), rateField,
                       normArg(argAt(2), 0.0), normArg(argAt(3), 1.0),
                       offsetModeOf({ 2, 3 }), phase01,
                       /*rateDomain*/ rateAbs ? 0 : 1);
        }
    } else if (type == "ad") {
        // Envelopes RETRIGGER per gate within their scope (T-474 ruling,
        // Neo s493) — sustain/decay are gate-shaped by definition; scoped
        // ramps are ~auto's job. Scope-anchored mode stays in the bytecode,
        // unexposed.
        bool aAbs = false, dAbs = false;
        const float aVal = envTime(argAt(0), 0.01, aAbs);
        const float dVal = envTime(argAt(1), 0.1,  dAbs);
        const uint8_t mask = (uint8_t) ((aAbs ? 0x1 : 0) | (dAbs ? 0x2 : 0));
        const uint16_t aLane = dynLane(argAt(0), SignalExprDomain::Seconds);
        const uint16_t dLane = dynLane(argAt(1), SignalExprDomain::Seconds);
        const uint16_t minLane = dynLane(argAt(2), exprDomain);
        const uint16_t maxLane = dynLane(argAt(3), exprDomain);
        if (aLane != 0xffff || dLane != 0xffff || minLane != 0xffff || maxLane != 0xffff) {
            pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Ad, 0, 0, 0,
                       offsetModeOf({ 2, 3 }),
                       { signalTimeSeconds(argAt(0), c.bpm, 0.01),
                         signalTimeSeconds(argAt(1), c.bpm, 0.1),
                         normArg(argAt(2), 0.0), normArg(argAt(3), 1.0) },
                       { aLane, dLane, minLane, maxLane });
        } else {
            pb.ctrlAd(lane, aVal, dVal,
                      normArg(argAt(2), 0.0), normArg(argAt(3), 1.0),
                      /*retrigger=*/true,
                      offsetModeOf({ 2, 3 }), mask);
        }
    } else if (type == "adsr") {
        bool aAbs = false, dAbs = false, rAbs = false;
        const float aVal = envTime(argAt(0), 0.01, aAbs);
        const float dVal = envTime(argAt(1), 0.1,  dAbs);
        const float rVal = envTime(argAt(3), 0.2,  rAbs);
        const uint8_t mask = (uint8_t) ((aAbs ? 0x1 : 0) | (dAbs ? 0x2 : 0)
                                       | (rAbs ? 0x8 : 0));
        const uint16_t aLane = dynLane(argAt(0), SignalExprDomain::Seconds);
        const uint16_t dLane = dynLane(argAt(1), SignalExprDomain::Seconds);
        const uint16_t susLane = dynLane(argAt(2), SignalExprDomain::UnitRaw);
        const uint16_t rLane = dynLane(argAt(3), SignalExprDomain::Seconds);
        const uint16_t minLane = dynLane(argAt(4), exprDomain);
        const uint16_t maxLane = dynLane(argAt(5), exprDomain);
        if (aLane != 0xffff || dLane != 0xffff || susLane != 0xffff
            || rLane != 0xffff || minLane != 0xffff || maxLane != 0xffff) {
            pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Adsr, 0, 0, 0,
                       offsetModeOf({ 4, 5 }),
                       { signalTimeSeconds(argAt(0), c.bpm, 0.01),
                         signalTimeSeconds(argAt(1), c.bpm, 0.1),
                         (float) resolveValue(argAt(2), "unit", c.bpm, 0.7),
                         signalTimeSeconds(argAt(3), c.bpm, 0.2),
                         normArg(argAt(4), 0.0), normArg(argAt(5), 1.0) },
                       { aLane, dLane, susLane, rLane, minLane, maxLane });
        } else {
            pb.ctrlAdsr(lane, aVal, dVal,
                        (float) resolveValue(argAt(2), "unit", c.bpm, 0.7),
                        rVal,
                        normArg(argAt(4), 0.0), normArg(argAt(5), 1.0),
                        offsetModeOf({ 4, 5 }),
                        /*retrigger=*/true, mask);
        }
    } else if (type == "auto") {
        int curveId = 0;
        bool curveFound = false;
        std::vector<ParseValue> numeric;
        for (const auto& a : g.args) {
            if (failUnsupportedAutoString(c, a)) {
                if (stepScope != nullptr)
                    pb.scopeEnd();
                return;
            }
            if (a.kind == ParseValue::Kind::String) {
                // B-297 — a string that names a curve is a curve; a string
                // that parses as a NOTE is a value (normArg resolves it to
                // Hz). Anything else is a loud V2 unsupported diagnostic.
                int candidateCurve = 0;
                if (autoCurveIdOf(a.str, candidateCurve)) {
                    if (!curveFound) { curveId = candidateCurve; curveFound = true; }
                } else if (curlop::pitch::noteToMidi(a.str) >= 0) {
                    numeric.push_back(a);
                }
            } else numeric.push_back(a);
        }
        const ParseValue n0 = numeric.size() > 0 ? numeric[0] : ParseValue{};
        const ParseValue n1 = numeric.size() > 1 ? numeric[1] : ParseValue{};
        const uint8_t autoMode = (g.isOffset || isPct(n0) || isPct(n1)) ? 1 : 0;
        const uint16_t startLane = dynLane(n0, exprDomain);
        const uint16_t endLane = dynLane(n1, exprDomain);
        if (startLane != 0xffff || endLane != 0xffff) {
            pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Auto, (uint8_t) curveId, 0,
                       0, autoMode,
                       { normArg(n0, 0.0), normArg(n1, 1.0) },
                       { startLane, endLane });
        } else {
            pb.ctrlAuto(lane, normArg(n0, 0.0), normArg(n1, 1.0), (uint8_t) curveId,
                        autoMode);
        }
    } else if (type == "random") {
        // 3rd arg = de-click slew time, through the STANDARD time protocol —
        // the same `resolveValue(.., "time", ..)` that ~glide / ~ad / ~adsr
        // / flam use: ms/s/hz, fractions (1/16), and bare beats all honoured;
        // resolved to BEATS and converted to frames at runtime with the live
        // tempo. Default 10ms (de-clicks a random-driven cutoff "ping" — the
        // snappy cutoff jump pings a resonant filter); `~random(min,max,0)`
        // opts back into the fully-snappy block-fill. The slew blends from
        // the param's PREVIOUS value (lane-keyed in the machine), so per-step
        // routes glide between draws instead of snapping.
        const double defBeats = 10.0 * c.bpm / 60000.0;   // 10ms in beats
        const double slewBeats = juce::jmax(0.0,
            resolveValue(argAt(2), "time", c.bpm, defBeats));
        const uint16_t minLane = dynLane(argAt(0), exprDomain);
        const uint16_t maxLane = dynLane(argAt(1), exprDomain);
        const uint16_t slewLane = dynLane(argAt(2), SignalExprDomain::TimeBeats);
        if (minLane != 0xffff || maxLane != 0xffff || slewLane != 0xffff) {
            const uint8_t flags = (uint8_t) (offsetModeOf({ 0, 1 }) | (perHit ? 0x02 : 0));
            pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Random, 0, 0,
                       c.shared->stamp(&g), flags,
                       { normArg(argAt(0), 0.0), normArg(argAt(1), 1.0),
                         (float) slewBeats },
                       { minLane, maxLane, slewLane });
        } else {
            pb.ctrlRandom(lane, normArg(argAt(0), 0.0), normArg(argAt(1), 1.0),
                          c.shared->stamp(&g), offsetModeOf({ 0, 1 }),
                          perHit ? 1 : 0, (float) slewBeats);
        }
    } else if (type == "deviate") {
        const uint16_t amountLane = dynLane(argAt(0), SignalExprDomain::UnitRaw);
        const float amount = normDelta(resolveValue(argAt(0), "unit", c.bpm, 0.1));
        if (amountLane != 0xffff) {
            pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Deviate, 0, 0,
                       c.shared->stamp(&g), (uint8_t) (perHit ? 0x02 : 0),
                       { amount }, { amountLane });
        } else {
            pb.ctrlDeviate(lane, amount, c.shared->stamp(&g), perHit ? 1 : 0);
        }
    } else if (type == "bernoulli") {
        const uint16_t weightLane = dynLane(argAt(0), SignalExprDomain::UnitRaw);
        const uint16_t aLane = dynLane(argAt(1), exprDomain);
        const uint16_t bLane = dynLane(argAt(2), exprDomain);
        if (weightLane != 0xffff || aLane != 0xffff || bLane != 0xffff) {
            const uint8_t flags = (uint8_t) (offsetModeOf({ 1, 2 }) | (perHit ? 0x02 : 0));
            pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Bernoulli, 0, 0,
                       c.shared->stamp(&g), flags,
                       { (float) resolveValue(argAt(0), "unit", c.bpm, 0.5),
                         normArg(argAt(1), 0.0), normArg(argAt(2), 1.0) },
                       { weightLane, aLane, bLane });
        } else {
            pb.ctrlBernoulli(lane,
                             (float) resolveValue(argAt(0), "unit", c.bpm, 0.5),
                             normArg(argAt(1), 0.0), normArg(argAt(2), 1.0),
                             c.shared->stamp(&g), offsetModeOf({ 1, 2 }),
                             perHit ? 1 : 0);
        }
    } else if (type == "keytrack") {
        const uint16_t minLane = dynLane(argAt(0), exprDomain);
        const uint16_t maxLane = dynLane(argAt(1), exprDomain);
        if (minLane != 0xffff || maxLane != 0xffff) {
            pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Keytrack, 0, 0, 0, 0,
                       { normArg(argAt(0), 0.0), normArg(argAt(1), 1.0) },
                       { minLane, maxLane });
        } else {
            pb.ctrlKeytrack(lane, normArg(argAt(0), 0.0), normArg(argAt(1), 1.0));
        }
    } else if (type == "accum") {
        // F-007: accum emission — start + n*increment per gate-on, ceiling.
        double mn, mx;
        schemaRange(c, paramName, mn, mx);
        const double range = (mx > mn) ? (mx - mn) : 1.0;
        const double inc = resolveValue(argAt(1), "param", c.bpm, 0.0);
        const uint16_t startLane = dynLane(argAt(0), exprDomain);
        const uint16_t incLane = dynLane(argAt(1), SignalExprDomain::UnitRaw);
        const uint16_t ceilLane = dynLane(argAt(2), exprDomain);
        if (startLane != 0xffff || incLane != 0xffff || ceilLane != 0xffff) {
            pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Accum, 0, 0, 0, 0,
                       { normArg(argAt(0), 0.0), normDelta(inc / range),
                         normArg(argAt(2), 1.0) },
                       { startLane, incLane, ceilLane });
        } else {
            pb.accum(lane, normArg(argAt(0), 0.0),
                     normDelta(inc / range), normArg(argAt(2), 1.0));
        }
    } else if (type == "midi") {
        // B-275: ~midi is an op — it renders in the machine so the scope rule
        // clips it like every other route (was: MidiRouteV2 metadata feeding a
        // permanently-live host lane). Only the CC VALUES stay host-side
        // (Machine::setMidiCCSource); NaN (never received) keeps the row
        // inactive = route released. Named args (cc:/channel:/min:/max:) win
        // over the positional (cc, min, max) form.
        auto findNamed = [&g](const juce::String& key) -> const ParseValue* {
            for (const auto& kv : g.named) if (kv.first == key) return &kv.second;
            return nullptr;
        };
        const ParseValue* ccArg  = findNamed("cc");
        const ParseValue* chArg  = findNamed("channel");
        const ParseValue* minArg = findNamed("min");
        const ParseValue* maxArg = findNamed("max");
        const auto& pos = g.args;
        const bool dynamicAddress =
            (ccArg && isSignalExprArg(*ccArg))
            || (chArg && isSignalExprArg(*chArg))
            || (!ccArg && pos.size() > 0 && isSignalExprArg(pos[0]));
        if (dynamicAddress) {
            c.shared->fail("compile-error: dynamic expression argument to 'midi' "
                           "is only supported for min/max, not cc/channel");
            return;
        }
        const int cc = ccArg ? (int) getNumVal(*ccArg, 1.0)
                             : (pos.size() > 0 ? (int) getNumVal(pos[0], 1.0) : 1);
        const int channel = chArg ? (int) getNumVal(*chArg, 0.0) : 0;
        const ParseValue rawMin = minArg ? *minArg : (pos.size() > 1 ? pos[1] : ParseValue{});
        const ParseValue rawMax = maxArg ? *maxArg : (pos.size() > 2 ? pos[2] : ParseValue{});
        const uint8_t offsetMode = (g.isOffset || isPct(rawMin) || isPct(rawMax)) ? 1 : 0;
        const uint16_t minLane = dynLane(rawMin, exprDomain);
        const uint16_t maxLane = dynLane(rawMax, exprDomain);
        if (minLane != 0xffff || maxLane != 0xffff) {
            pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Midi,
                       (uint8_t) juce::jlimit(0, 15, channel),
                       (uint8_t) juce::jlimit(0, 127, cc),
                       0, offsetMode,
                       { normArg(rawMin, 0.0), normArg(rawMax, 1.0) },
                       { minLane, maxLane });
        } else {
            pb.ctrlMidi(lane, (uint8_t) juce::jlimit(0, 15, channel),
                        (uint8_t) juce::jlimit(0, 127, cc),
                        normArg(rawMin, 0.0), normArg(rawMax, 1.0), offsetMode);
        }
    } else if (type == "glide") {
        pb.glide((float) resolveValue(argAt(0), "time", c.bpm, 0.1));
    } else {
        c.shared->fail("compile-error: unknown control op '~" + type + "'");
    }

    if (stepScope != nullptr)
        pb.scopeEnd();
}

void emitCtrlSignalRouteV2(V2Ctx& c, const juce::String& paramName, const ParseValue& value,
                           const StepRouteSpan* stepScope, bool perHit,
                           uint16_t forcedLane, SignalExprDomain domain)
{
    if (value.kind == ParseValue::Kind::Generator && value.gen) {
        emitCtrlRouteV2(c, paramName, *value.gen, stepScope, perHit, forcedLane, domain);
        return;
    }

    if (value.kind != ParseValue::Kind::SignalChain || ! value.signalChain) {
        c.shared->fail("compile-error: unsupported control signal route for " + paramName);
        return;
    }

    const SignalChainNode& chain = *value.signalChain;
    if (! chain.source) {
        c.shared->fail("compile-error: signal chain for " + paramName + " has no source");
        return;
    }

    const uint16_t lane = forcedLane == 0xffff ? laneOf(c, paramName) : forcedLane;
    if (stepScope == nullptr && forcedLane == 0xffff) {
        const juce::String key = buildSignalChainKey(chain);
        const std::string dedup = std::to_string(c.scopeSerial) + "|"
                                + key.toStdString() + "|" + std::to_string(lane);
        if (c.ctrlEmitted->count(dedup)) return;
        c.ctrlEmitted->insert(dedup);
    }

    emitCtrlRouteV2(c, paramName, *chain.source, stepScope, perHit, forcedLane, domain);

    ProgramBuilder& pb = *c.pb;
    const auto argAt = [](const SignalChainOp& op, size_t i) -> ParseValue {
        return i < op.args.size() ? op.args[i] : ParseValue{};
    };
    auto dynLane = [&](const ParseValue& v, SignalExprDomain argDomain) -> uint16_t {
        return emitDynamicArgLaneV2(c, paramName, v, argDomain, stepScope, perHit);
    };
    for (const auto& op : chain.ops) {
        if (stepScope != nullptr)
            pb.scopeStart(stepScope->beatOffset, stepScope->durBeats);

        if (op.name == "clip") {
            const float mn = signalExprScalarForDomain(c, paramName, argAt(op, 0),
                                                       domain, -1.0);
            const float mx = signalExprScalarForDomain(c, paramName, argAt(op, 1),
                                                       domain, 1.0);
            const uint16_t mnLane = dynLane(argAt(op, 0), domain);
            const uint16_t mxLane = dynLane(argAt(op, 1), domain);
            if (mnLane != 0xffff || mxLane != 0xffff)
                pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Clip, 0, 0, 0, 0,
                           { mn, mx }, { mnLane, mxLane });
            else
                pb.ctrlClip(lane, mn, mx);
        } else if (op.name == "invert") {
            const float pivot = signalExprScalarForDomain(c, paramName, argAt(op, 0),
                                                          domain, 0.5);
            const uint16_t pivotLane = dynLane(argAt(op, 0), domain);
            if (pivotLane != 0xffff)
                pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Invert, 0, 0, 0, 0,
                           { pivot }, { pivotLane });
            else
                pb.ctrlInvert(lane, pivot);
        } else if (op.name == "scale") {
            if (isMusicalScaleSignalOp(op)) {
                const ParseValue rootArg = argAt(op, 0);
                const ParseValue modeArg = argAt(op, 1);
                if (rootArg.kind != ParseValue::Kind::String
                    || modeArg.kind != ParseValue::Kind::String) {
                    c.shared->fail("compile-error: musical scale signal chain needs static root and mode");
                    if (stepScope != nullptr)
                        pb.scopeEnd();
                    return;
                }
                const float root = (float) curlop::music::noteChroma(
                    rootArg.str.toLowerCase().toStdString());
                const auto& iv = curlop::music::scaleIntervals(
                    modeArg.str.toLowerCase().toStdString());
                std::vector<float> intervals(iv.begin(), iv.end());
                pb.ctrlQuantizeScale(lane, root, intervals);
            } else {
                const float mn = signalExprScalarForDomain(c, paramName, argAt(op, 0),
                                                           domain, 0.0);
                const float mx = signalExprScalarForDomain(c, paramName, argAt(op, 1),
                                                           domain, 1.0);
                const uint16_t mnLane = dynLane(argAt(op, 0), domain);
                const uint16_t mxLane = dynLane(argAt(op, 1), domain);
                if (mnLane != 0xffff || mxLane != 0xffff)
                    pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Scale, 0, 0, 0, 0,
                               { mn, mx }, { mnLane, mxLane });
                else
                    pb.ctrlScale(lane, mn, mx);
            }
        } else if (op.name == "offset") {
            const float amount = signalExprScalarForDomain(c, paramName, argAt(op, 0),
                                                           domain, 0.0);
            const uint16_t amountLane = dynLane(argAt(op, 0), domain);
            if (amountLane != 0xffff)
                pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Offset, 0, 0, 0, 0,
                           { amount }, { amountLane });
            else
                pb.ctrlOffset(lane, amount);
        } else if (op.name == "transpose" || op.name == "octave") {
            const SignalExprDomain offsetDomain =
                op.name == "transpose" ? SignalExprDomain::SemitoneOffset
                                       : SignalExprDomain::OctaveOffset;
            const float amount = signalExprScalarForDomain(c, paramName, argAt(op, 0),
                                                           offsetDomain, 0.0);
            const uint16_t amountLane = dynLane(argAt(op, 0), offsetDomain);
            if (amountLane != 0xffff)
                pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Offset, 0, 0, 0, 0,
                           { amount }, { amountLane });
            else
                pb.ctrlOffset(lane, amount);
        } else if (op.name == "gain") {
            const float amount = normSignalArg(c, paramName, argAt(op, 0), 1.0);
            const uint16_t amountLane = dynLane(argAt(op, 0), SignalExprDomain::UnitRaw);
            if (amountLane != 0xffff)
                pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Gain, 0, 0, 0, 0,
                           { amount }, { amountLane });
            else
                pb.ctrlGain(lane, amount);
        } else if (op.name == "quantize") {
            const float step = signalExprScalarForDomain(c, paramName, argAt(op, 0),
                                                         SignalExprDomain::UnitRaw, 0.1);
            const uint16_t stepLane = dynLane(argAt(op, 0), SignalExprDomain::UnitRaw);
            if (stepLane != 0xffff)
                pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Quantize, 0, 0, 0, 0,
                           { step }, { stepLane });
            else
                pb.ctrlQuantize(lane, step);
        } else if (op.name == "abs") {
            pb.ctrlAbs(lane);
        } else if (op.name == "smooth") {
            const float seconds = signalTimeSeconds(argAt(op, 0), c.bpm, 0.02);
            const uint16_t secondsLane = dynLane(argAt(op, 0), SignalExprDomain::Seconds);
            if (secondsLane != 0xffff)
                pb.ctrlDyn(lane, (uint8_t) vm::CtrlDynKind::Smooth, 0, 0, 0, 0,
                           { seconds }, { secondsLane });
            else
                pb.ctrlSmooth(lane, seconds);
        } else {
            c.shared->fail("compile-error: unknown signal-chain op '" + op.name + "'");
        }

        if (stepScope != nullptr)
            pb.scopeEnd();
    }
}

void emitTriggerCtrlRoutesV2(V2Ctx& c, const AstNode& step, const AstNode& event,
                             int eventIdx, int cg, double beatOffset, double duration)
{
    // Step-applied routes (event dot-form + step postfix) clock to the step;
    // sequence/global routes clock to the enclosing scope.
    //
    // B-280 (spec §5 rule 2): rightmost wins among duplicate routes on the
    // same param. Candidates collect first (inline ones compete by
    // sourceOrdinal — the shared token-index ordinal; globals sit below
    // every inline route, later globals beating earlier); only the winners
    // emit. B-279 pre-articulation suppression stays absolute.
    const StepRouteSpan span { beatOffset, duration };
    struct RouteCand {
        juce::String name;
        ParseValue value;
        const StepRouteSpan* scope;
        int ord;
    };
    std::vector<RouteCand> winners;
    std::unordered_map<std::string, size_t> winnerIdx;
    auto consider = [&](const juce::String& pname,
                        const ParseValue& value,
                        const StepRouteSpan* sc, int ord) {
        if (c.suppressRouteParams != nullptr
            && c.suppressRouteParams->count(pname))
            return;
        if (c.consumedRouteTokens.count(pname.toStdString() + "|" + std::to_string(ord)))
            return;   // B-298 — already emitted per-copy inside a repeat
        const std::string key = pname.toStdString();
        const auto it = winnerIdx.find(key);
        if (it == winnerIdx.end()) {
            winnerIdx.emplace(key, winners.size());
            winners.push_back({ pname, value, sc, ord });
        } else if (ord > winners[it->second].ord) {
            winners[it->second] = { pname, value, sc, ord };
        }
    };
    auto considerVoiceStack = [&](const Processor& p, const StepRouteSpan* sc, int ord) {
        if (! p.isVoiceStack || p.args.empty()) return;
        if (nonLockRouteOps().count(p.name)) return;
        const juce::String name = p.name.toUpperCase();
        if (nonModuleParams().count(name)) return;

        const int L = (int) p.args.size();
        const int voices = std::max(moduleVoicesOf(c), event.stackVoices);
        bool emittedRoute = false;
        for (int k = 0; k < voices; ++k) {
            const ParseValue& v = p.args[(size_t) (k % L)];
            if (! isStreamRouteValue(v)) continue;
            consider(name + "_V" + juce::String(k), v, sc, ord);
            emittedRoute = true;
        }
        if (emittedRoute && c.perVoiceParams != nullptr)
            c.perVoiceParams->insert(name.toStdString());
    };
    for (const auto& p : event.processors) {
        considerVoiceStack(p, &span, std::max(0, p.sourceOrdinal));
        if (!p.isDotParam || p.args.empty()) continue;
        if (nonLockRouteOps().count(p.name)) continue;
        const juce::String name = p.name.toUpperCase();
        if (nonModuleParams().count(name)) continue;
        const ParseValue& v = p.args[0];
        if (isStreamRouteValue(v))
            consider(name, v, &span, std::max(0, p.sourceOrdinal));
    }
    for (const auto& p : step.processors) {
        if (matchesScope(p, eventIdx, cg))
            considerVoiceStack(p, &span, std::max(0, p.sourceOrdinal));
        if (p.args.empty()) continue;
        const ParseValue& v = p.args[0];
        if (!isStreamRouteValue(v)) continue;
        if (!matchesScope(p, eventIdx, cg)) continue;
        const juce::String upper = p.name.toUpperCase();
        if (nonModuleParams().count(upper)) continue;
        if (nonLockRouteOps().count(p.name)) continue;
        consider(upper, v, &span, std::max(0, p.sourceOrdinal));
    }
    if (c.globalModuleParams) {
        int gOrd = -1000000;
        for (const AstNode* gEv : *c.globalModuleParams) {
            if (!gEv || gEv->module != c.target) continue;
            for (const auto& [pname, value] : extractStreamParamLocks(*gEv))
                consider(pname, value, nullptr, ++gOrd);
        }
    }

    // T-484 (§5 one-law, Neo s496): under a gate-multiplying articulation
    // (machine trains — ratchet/buzz/bounce/geiger/flam), WRITTEN POSITION
    // selects the stochastic clock: a route right of the train re-rolls per
    // machine-generated hit; left rolls once per step, value then
    // articulated. Compile-time expansions (arp/strum) get this for free
    // via per-sub-step scopes (B-279); trains generate gates inside ONE
    // step scope, so the per-hit mode rides the bytecode operand.
    int trainOrd = -1;
    auto considerTrain = [&](const Processor& p) {
        static const std::unordered_set<juce::String> trainOps = {
            "ratchet", "buzz", "bounce", "geiger", "flam",
        };
        if (! trainOps.count(p.name) || p.sourceOrdinal < 0) return;
        if (trainOrd < 0 || p.sourceOrdinal < trainOrd) trainOrd = p.sourceOrdinal;
    };
    for (const auto& p : step.processors)
        if (matchesScope(p, eventIdx, cg)) considerTrain(p);
    for (const auto& p : event.processors) considerTrain(p);

    for (const auto& w : winners)
        emitCtrlSignalRouteV2(c, w.name, w.value, w.scope,
                              /*perHit=*/trainOrd >= 0 && w.ord > trainOrd);
}

// ── B-284 — §5 one-law: implicit scope for plain-step ops. ────────────────
// "Scope op" is compiler plumbing (scopeOpNames() / machine scope-frame
// mechanics) — the taxonomy must never surface in language behavior. An op
// written as a bare trigger postfix binds left like every op: the trigger
// body wraps in an implicit machine scope frame, the same mechanism
// brackets use. One position-ordered emission path — the note-attached
// transpose fold this replaces applied transpose/octave only and missed
// every other op at this position.
void emitScopeOpsV2(V2Ctx& c,
                    const std::vector<std::pair<juce::String, std::vector<ParseValue>>>& ops);
double dslTimescaleOf(const std::vector<std::pair<juce::String, std::vector<ParseValue>>>& ops,
                      double bpm);

bool scopeOpZeroDisabled(const std::vector<ParseValue>& args);

std::vector<std::pair<juce::String, std::vector<ParseValue>>>
collectImplicitScopeOps(const AstNode& step, const AstNode& event, int eventIdx, int cg)
{
    std::vector<const Processor*> hits;
    for (const auto& p : step.processors)
        if (scopeOpNames().count(p.name) && !scopeOpZeroDisabled(p.args)
            && matchesScope(p, eventIdx, cg))
            hits.push_back(&p);
    for (const auto& p : event.processors)
        if (scopeOpNames().count(p.name) && !scopeOpZeroDisabled(p.args))
            hits.push_back(&p);
    std::stable_sort(hits.begin(), hits.end(),
        [](const Processor* a, const Processor* b) {
            if (a->sourceOrdinal < 0 || b->sourceOrdinal < 0) return false;
            return a->sourceOrdinal < b->sourceOrdinal;
        });
    std::vector<std::pair<juce::String, std::vector<ParseValue>>> ops;
    ops.reserve(hits.size());
    for (const Processor* p : hits) ops.emplace_back(p->name, p->args);
    return ops;
}

const Processor* findArticProcessorV2(V2Ctx& c, const AstNode& step,
                                      const AstNode& event, int eventIdx, int cg,
                                      std::initializer_list<const char*> names)
{
    const Processor* p = extractProcessor(step.processors, names, cg, eventIdx);
    if (!p) p = extractProcessor(event.processors, names, -1, -1);
    if (!p && c.globalStepOps) p = extractProcessor(*c.globalStepOps, names, -1, -1);
    return p;
}

struct GenArgEmitV2 { bool present = false; double mid = 0.0; };

GenArgEmitV2 emitGenArgV2(V2Ctx& c, const Processor* p, size_t argIdx,
                          vm::GenTarget target, const char* domain)
{
    if (p == nullptr || p->args.size() <= argIdx || !isSignalExprArg(p->args[argIdx]))
        return {};
    ProgramBuilder& pb = *c.pb;
    if (! isGenArg(p->args[argIdx])) {
        FireExprV2 expr;
        if (!compileFireExprV2(c, p->args[argIdx], domain, 0.0, expr))
            return {};
        FireExprV2 unused;
        appendFireImmediateV2(unused, 0.0f);
        pb.genOperandDyn(target, 0, c.shared->stamp(p),
                         expr.code, expr.immediates,
                         expr.code, expr.immediates,
                         unused.code, unused.immediates);
        return { true, expr.midpoint };
    }
    const GeneratorNode& g = *p->args[argIdx].gen;
    const auto sub = [&] (size_t i, const char* dom, double fb) {
        return i < g.args.size() ? resolveValue(g.args[i], dom, c.bpm, fb) : fb;
    };
    const bool hasDynamicArg =
        std::any_of(g.args.begin(), g.args.end(),
                    [] (const ParseValue& v) { return isSignalExprArg(v); });
    auto emitDyn = [&](uint8_t kind, const ParseValue& a, const ParseValue& b,
                       const ParseValue& w, double afb, double bfb,
                       double wfb) -> GenArgEmitV2 {
        FireExprV2 ae, be, we;
        if (!compileFireExprV2(c, a, domain, afb, ae)) return {};
        if (!compileFireExprV2(c, b, domain, bfb, be)) return {};
        if (!compileFireExprV2(c, w, "unit", wfb, we)) return {};
        pb.genOperandDyn(target, kind, c.shared->stamp(&g),
                         ae.code, ae.immediates,
                         be.code, be.immediates,
                         we.code, we.immediates);
        return { true, (ae.midpoint + be.midpoint) * 0.5 };
    };
    if (g.generatorType == "random") {
        if (hasDynamicArg)
            return emitDyn(0,
                           g.args.size() > 0 ? g.args[0] : ParseValue{},
                           g.args.size() > 1 ? g.args[1] : ParseValue{},
                           ParseValue{}, 0.0, 1.0, 0.0);
        const double a = sub(0, domain, 0.0), b = sub(1, domain, 1.0);
        pb.genOperand(target, 0, (float) a, (float) b, 0.0f,
                      c.shared->stamp(&g));
        return { true, (a + b) * 0.5 };
    }
    if (g.generatorType == "bernoulli") {
        if (hasDynamicArg)
            return emitDyn(1,
                           g.args.size() > 1 ? g.args[1] : ParseValue{},
                           g.args.size() > 2 ? g.args[2] : ParseValue{},
                           g.args.size() > 0 ? g.args[0] : ParseValue{},
                           0.0, 1.0, 0.5);
        const double w = sub(0, "unit", 0.5);
        const double a = sub(1, domain, 0.0), b = sub(2, domain, 1.0);
        pb.genOperand(target, 1, (float) a, (float) b, (float) w,
                      c.shared->stamp(&g));
        return { true, (a + b) * 0.5 };
    }
    if (g.generatorType == "auto") {
        std::vector<ParseValue> numeric;
        for (const auto& a : g.args)
            if (a.kind != ParseValue::Kind::String)
                numeric.push_back(a);
        const ParseValue a = numeric.size() > 0 ? numeric[0] : ParseValue{};
        const ParseValue b = numeric.size() > 1 ? numeric[1] : ParseValue{};
        if (hasDynamicArg)
            return emitDyn(2, a, b, ParseValue{}, 0.0, 1.0, 0.0);
        const double av = resolveValue(a, domain, c.bpm, 0.0);
        const double bv = resolveValue(b, domain, c.bpm, 1.0);
        pb.genOperand(target, 2, (float) av, (float) bv, 0.0f,
                      c.shared->stamp(&g));
        return { true, (av + bv) * 0.5 };
    }
    if (g.generatorType == "lfo") {
        FireExprV2 expr;
        if (!compileFireExprGeneratorV2(c, g, domain, 0.0, expr))
            return {};
        FireExprV2 unused;
        appendFireImmediateV2(unused, 0.0f);
        pb.genOperandDyn(target, 0, c.shared->stamp(&g),
                         expr.code, expr.immediates,
                         expr.code, expr.immediates,
                         unused.code, unused.immediates);
        return { true, expr.midpoint };
    }
    failGenOperand(c, (juce::String(p->name) + ":~" + g.generatorType)
                          .toRawUTF8());
    return {};
}

bool emitGateTrainPreludeV2(V2Ctx& c, const AstNode& step, const AstNode& event,
                            int eventIdx, int cg, double duration)
{
    ProgramBuilder& pb = *c.pb;

    // Gate-length + train articulations (precede NOTE/GATE_ONLY).
    const Processor* lenProc =
        findArticProcessorV2(c, step, event, eventIdx, cg, { "len" });
    bool lenGenEmitted = false;
    if (lenProc != nullptr && !lenProc->args.empty() && isSignalExprArg(lenProc->args[0])) {
        const auto lg = emitGenArgV2(c, lenProc, 0, vm::GenTarget::GateLen, "time");
        if (lg.present) {
            pb.gateLen((float) lg.mid);
            lenGenEmitted = true;
        } else {
            return false;
        }
    }
    if (!lenGenEmitted) {
        auto len = extractLen(step.processors, duration, c.bpm, cg, eventIdx);
        if (!len.present) len = extractLen(event.processors, duration, c.bpm, -1, -1);
        if (!len.present && c.globalStepOps)
            len = extractLen(*c.globalStepOps, duration, c.bpm, -1, -1);
        if (len.present && std::isfinite(len.beats)) pb.gateLen((float) len.beats);
    }

    const Processor* devP =
        findArticProcessorV2(c, step, event, eventIdx, cg, { "deviate" });
    if (devP != nullptr && !devP->args.empty()) {
        if (isSignalExprArg(devP->args[0])) {
            const auto dg = emitGenArgV2(c, devP, 0,
                                         vm::GenTarget::DeviateAmount, "unit");
            if (!dg.present)
                return false;
            if (dg.mid > 0.0)
                pb.deviate((float) dg.mid, c.shared->stamp(devP));
        } else {
            const double amount = resolveValue(devP->args[0], "unit", c.bpm, 0.0);
            if (amount > 0.0)
                pb.deviate((float) amount, c.shared->stamp(devP));
        }
    }

    const Processor* ratP =
        findArticProcessorV2(c, step, event, eventIdx, cg, { "ratchet" });
    bool ratchetRamp = false;
    if (ratP != nullptr && !ratP->args.empty() && isGenArg(ratP->args[0])
        && ratP->args[0].gen->generatorType == "auto") {
        const GeneratorNode& g = *ratP->args[0].gen;
        const auto sub = [&] (size_t i, double fb) {
            return i < g.args.size() ? resolveValue(g.args[i], "count", c.bpm, fb) : fb;
        };
        std::vector<ParseValue> numeric;
        for (const auto& a : g.args)
            if (a.kind != ParseValue::Kind::String)
                numeric.push_back(a);
        const ParseValue a = numeric.size() > 0 ? numeric[0] : ParseValue{};
        const ParseValue b = numeric.size() > 1 ? numeric[1] : ParseValue{};
        const double cntA = sub(0, 2.0), cntB = sub(g.args.size() - 1, 8.0);
        FireExprV2 iaExpr, ibExpr, wExpr;
        const bool hasDynamicArg =
            std::any_of(g.args.begin(), g.args.end(),
                        [] (const ParseValue& v) { return isSignalExprArg(v); });
        const double iA = (cntA > 0.0) ? duration / cntA : 0.0;
        const double iB = (cntB > 0.0) ? duration / cntB : 0.0;
        if (iA > 0.0 && iB > 0.0 && std::isfinite(iA) && std::isfinite(iB)) {
            if (hasDynamicArg) {
                if (!compileFireCountIntervalExprV2(c, a, duration, 2.0, iaExpr)
                    || !compileFireCountIntervalExprV2(c, b, duration, 8.0, ibExpr)
                    || !compileFireExprV2(c, ParseValue{}, "unit", 0.0, wExpr))
                    return false;
                pb.genOperandDyn(vm::GenTarget::ArpSpeedBeats, 2,
                                 c.shared->stamp(&g),
                                 iaExpr.code, iaExpr.immediates,
                                 ibExpr.code, ibExpr.immediates,
                                 wExpr.code, wExpr.immediates);
            } else {
                pb.genOperand(vm::GenTarget::ArpSpeedBeats, 2, (float) iA, (float) iB,
                              0.0f, c.shared->stamp(&g));
            }
            pb.ratchet((uint32_t) std::max(1.0, std::round(cntA)));
            ratchetRamp = true;
        }
    }
    if (!ratchetRamp) {
        const auto rg = emitGenArgV2(c, ratP, 0, vm::GenTarget::RatchetCount, "count");
        if (rg.present) {
            pb.ratchet((uint32_t) std::max(1.0, std::floor(rg.mid)));
        } else {
            auto ratchet = extractRatchet(step.processors, cg, c.bpm, eventIdx);
            if (!ratchet.present) ratchet = extractRatchet(event.processors, -1, c.bpm, -1);
            if (!ratchet.present && c.globalStepOps)
                ratchet = extractRatchet(*c.globalStepOps, -1, c.bpm, -1);
            if (ratchet.present && ratchet.count > 1) pb.ratchet((uint32_t) ratchet.count);
        }
    }

    const auto bg = emitGenArgV2(c,
                                 findArticProcessorV2(c, step, event, eventIdx, cg, { "buzz" }),
                                 0, vm::GenTarget::BuzzPressure, "unit");
    if (bg.present) {
        pb.buzz((float) bg.mid, (float) (300.0 * c.bpm / 60000.0));
    } else {
        auto buzz = extractBuzz(step.processors, cg, c.bpm, eventIdx);
        if (!buzz.present) buzz = extractBuzz(event.processors, -1, c.bpm, -1);
        if (!buzz.present && c.globalStepOps)
            buzz = extractBuzz(*c.globalStepOps, -1, c.bpm, -1);
        if (buzz.present) pb.buzz((float) buzz.pressure, (float) buzz.durationBeats);
    }

    auto bounce = extractBounce(step.processors, cg, eventIdx);
    if (!bounce.present) bounce = extractBounce(event.processors, -1, -1);
    if (!bounce.present && c.globalStepOps)
        bounce = extractBounce(*c.globalStepOps, -1, -1);
    const auto gg = emitGenArgV2(c,
                                 findArticProcessorV2(c, step, event, eventIdx, cg, { "bounce" }),
                                 0, vm::GenTarget::BounceGravity, "unit");
    if (gg.present) {
        pb.bounce((float) gg.mid, bounce.interval, bounce.intervalDomain);
    } else if (bounce.present) {
        pb.bounce((float) bounce.gravity, bounce.interval, bounce.intervalDomain);
    }

    const Processor* geigerProc =
        findArticProcessorV2(c, step, event, eventIdx, cg, { "geiger" });
    if (geigerProc) {
        const auto dg = emitGenArgV2(c, geigerProc, 0,
                                     vm::GenTarget::GeigerDensity, "unit");
        if (dg.present) {
            pb.geiger((float) dg.mid, c.shared->stamp(geigerProc));
        } else {
            const auto arg = geigerProc->args.empty() ? ParseValue{} : geigerProc->args[0];
            const double density = (arg.kind == ParseValue::Kind::Null)
                                 ? 0.5 : resolveValue(arg, "unit", c.bpm, 0.5);
            if (std::isfinite(density) && density != 0.0)
                pb.geiger((float) density, c.shared->stamp(geigerProc));
        }
    }

    return true;
}

// ── Articulations + note body, in the machine step-body order. ────────────
// [implicit scope], STEP, [gate-len + trains], NOTE/NOTE_CHORD/REST,
// [flam], [locks].
void emitTriggerBodyV2(V2Ctx& c, const AstNode& step, const AstNode& event,
                       int eventIdx, int cg, double beatOffset, double duration,
                       double overridePitchSemis = std::numeric_limits<double>::quiet_NaN(),
                       const Processor* dynamicRepeat = nullptr)
{
    ProgramBuilder& pb = *c.pb;
    if (event.stackVoices
        > static_cast<int>(
            std::numeric_limits<std::uint16_t>::max())) {
        c.shared->error =
            "compile-error: stack(...) count exceeds the current "
            "NOTE_CHORD encoding limit of 65535 voices";
        return;
    }
    if (event.stackVoices > 1 && c.minVoices != nullptr)
        *c.minVoices = std::max(*c.minVoices, event.stackVoices);

    // B-284 — implicit scope frame when scope ops cover this trigger.
    // Inside the frame every offset is frame-relative (scope bases
    // accumulate in the machine), exactly as in the bracket path.
    const int enclosingSerial = c.scopeSerial;
    const auto implicitOps = collectImplicitScopeOps(step, event, eventIdx, cg);
    const bool wrapped = !implicitOps.empty();
    if (wrapped) {
        ++c.scopeSerial;
        pb.scopeStart(beatOffset, duration * dslTimescaleOf(implicitOps, c.bpm));
        emitScopeOpsV2(c, implicitOps);
    }
    const double bodyOffset = wrapped ? 0.0 : beatOffset;

    // STEP header (ms-duration steps → STEP_ABS).
    if (c.stepOrdinals) c.stepOrdinals->push_back(c.topStepOrdinal);
    if (step.stepUnitArg.kind == ParseValue::Kind::UnitNumber
        && (step.stepUnitArg.unit == "ms" || step.stepUnitArg.unit == "s")) {
        const double ms = step.stepUnitArg.unit == "s"
                        ? step.stepUnitArg.number * 1000.0 : step.stepUnitArg.number;
        pb.stepAbs(bodyOffset, (float) ms);
    } else {
        pb.step(bodyOffset, duration);
    }

    if (dynamicRepeat != nullptr && !dynamicRepeat->args.empty()) {
        FireExprV2 expr;
        if (!compileFireExprV2(c, dynamicRepeat->args[0], "count", 1.0, expr))
            return;
        pb.repeatExpr(expr.code, expr.immediates);
    }

    if (!emitGateTrainPreludeV2(c, step, event, eventIdx, cg, duration))
        return;

    // NOTE / NOTE_CHORD / decoupled gate/pitch / REST. Pitch transforms
    // (transpose/octave/invert) ride the implicit scope frame above —
    // resolvePitch applies them per voice at fire time, chords included.
    const bool hasOverride = !std::isnan(overridePitchSemis);

    // T-469 — decoupled gate/pitch step ops (Neo rulings s492, spec §3/§6):
    //   gate / gate:x  → GATE_ONLY at the held pitch (x = gate length via the
    //                    universal value system: bare = N × step, % = step
    //                    fraction, ms/s absolute — extractLen semantics).
    //   note:x freq:x  → PITCH_SET, legato — pitch update without a gate
    //                    edge. Dot sugar (.c3) keeps trigger semantics.
    const Processor* gateProc = extractProcessor(event.processors, {"gate"}, -1, -1);
    if (!gateProc) gateProc = extractProcessor(step.processors, {"gate"}, cg, eventIdx);
    const Processor* pitchProc = extractProcessor(event.processors, {"note", "freq"}, -1, -1);
    if (!pitchProc) pitchProc = extractProcessor(step.processors, {"note", "freq"}, cg, eventIdx);
    const bool gateStreamProc = gateProc != nullptr
        && !gateProc->args.empty()
        && isStreamRouteValue(gateProc->args[0]);

    // F-071 T-531 — vel/velocity as a per-loop GEN_OPERAND(NoteVel) draw. Emitted as
    // a prefix (after the trains, before NOTE/GATE_ONLY); the note op carries the
    // range midpoint as its never-consumed immediate while the prefix arms.
    // Only when a note/gate actually follows, so a rest doesn't arm a dead bit.
    const bool willEmitNote =
        (event.eventType == "trigger" && (!event.pitch.isEmpty() || hasOverride))
        || gateProc != nullptr;
    const Processor* velProc = willEmitNote
        ? extractProcessor(event.processors, {"vel", "velocity"}, -1, -1)
        : nullptr;
    const auto velGen = emitGenArgV2(c, velProc, 0, vm::GenTarget::NoteVel, "unit");
    const float velImm = velGen.present ? (float) velGen.mid
                                        : (float) extractVelocity(event.processors);

    if (event.eventType == "trigger" && (!event.pitch.isEmpty() || hasOverride)) {
        const float vel = velImm;
        if (gateStreamProc) {
            if (hasOverride) {
                pb.pitchSet((float) overridePitchSemis);
            } else {
                const auto resolved = curlop::pitch::resolvePitch(event.pitch);
                const double semis = resolved.voicings.empty()
                    ? semisOfChain(event.pitch)
                    : resolved.voicings.front() - 60.0;
                pb.pitchSet((float) semis);
            }
        } else if (hasOverride) {
            pb.note((float) overridePitchSemis, vel);
        } else {
            const auto resolved = curlop::pitch::resolvePitch(event.pitch);
            if (resolved.voicings.size() > 1) {
                std::vector<float> semis;
                semis.reserve(resolved.voicings.size());
                for (double v : resolved.voicings)
                    semis.push_back((float) (v - 60.0));
                pb.noteChord(semis, vel);
            } else if (event.stackVoices > 1) {
                std::vector<float> semis;
                semis.assign((size_t) event.stackVoices,
                             (float) semisOfChain(event.pitch));
                pb.noteChord(semis, vel);
            } else {
                pb.note((float) semisOfChain(event.pitch), vel);
            }
        }
    } else if (gateProc != nullptr || pitchProc != nullptr) {
        if (pitchProc != nullptr && !pitchProc->args.empty()) {
            curlop::script::PitchChain chain;
            const ParseValue& a = pitchProc->args[0];
            juce::String raw = a.kind == ParseValue::Kind::String
                ? a.str
                : (a.kind == ParseValue::Kind::UnitNumber
                       ? juce::String(a.number) + a.unit
                       : juce::String(a.number));
            if (pitchProc->name == "freq") {
                if (!raw.endsWithIgnoreCase("hz")) raw << "hz";
                chain.freq = raw;
            } else {
                chain.note = raw;
            }
            pb.pitchSet((float) semisOfChain(chain));
        }
        if (gateProc != nullptr) {
            if (!gateStreamProc
                && !gateProc->args.empty()
                && gateProc->args[0].kind != ParseValue::Kind::Null) {
                const ParseValue& a = gateProc->args[0];
                const double beats = (a.kind == ParseValue::Kind::UnitNumber)
                    ? resolveValue(a, "time", c.bpm, 0.0)
                    : getNumValOrNaN(a, 1.0) * duration;
                if (std::isfinite(beats) && beats > 0.0)
                    pb.gateLen((float) beats);
            }
            if (!gateStreamProc)
                pb.gateOnly(velImm);
        }
    } else if (event.eventType == "update") {
        pb.rest();
    }

    if (gateStreamProc) {
        const StepRouteSpan gateSpan { bodyOffset, duration };
        emitCtrlSignalRouteV2(c, "GATE", gateProc->args[0], &gateSpan,
                              /*perHit=*/false, 0xffff,
                              SignalExprDomain::UnitRaw);
    }

    // Flam (reads the note's pitch/vel — follows NOTE).
    {
        const Processor* flamP =
            findArticProcessorV2(c, step, event, eventIdx, cg, { "flam" });
        const auto fo = emitGenArgV2(c, flamP, 0, vm::GenTarget::FlamOffset, "time");
        const auto fg = emitGenArgV2(c, flamP, 1, vm::GenTarget::FlamGain, "unit");
        if (fo.present || fg.present) {
            const double gainImm = (flamP != nullptr && flamP->args.size() > 1
                                    && !isGenArg(flamP->args[1]))
                ? resolveValue(flamP->args[1], "unit", c.bpm, 0.5)
                : (fg.present ? fg.mid : 0.5);
            const double offImm = (flamP != nullptr && !flamP->args.empty()
                                   && !isGenArg(flamP->args[0]))
                ? resolveValue(flamP->args[0], "time", c.bpm, 0.05)
                : (fo.present ? fo.mid : 0.05);
            pb.flam((float) offImm, (float) gainImm);
        } else {
            auto flam = extractFlam(step.processors, cg, c.bpm, eventIdx);
            if (!flam.present) flam = extractFlam(event.processors, -1, c.bpm, -1);
            if (!flam.present && c.globalStepOps) flam = extractFlam(*c.globalStepOps, -1, c.bpm, -1);
            if (flam.present) pb.flam((float) flam.offset, (float) flam.gain);
        }
    }

    // Param locks.
    emitLocksV2(c, collectLocksV2(c, step, event, eventIdx, cg, duration));

    // Generator routes — step-applied locks clock to this step (SF-061).
    emitTriggerCtrlRoutesV2(c, step, event, eventIdx, cg, bodyOffset, duration);

    if (wrapped) pb.scopeEnd();

    // Glide (scope-level pitch stream — SF-061 scope lifecycle). It ramps
    // BETWEEN notes across steps, so it lives in the ENCLOSING scope —
    // emitted past the implicit frame, deduped on the enclosing serial.
    auto glide = extractGlide(step.processors, c.bpm, cg, eventIdx);
    if (!glide.present) glide = extractGlide(event.processors, c.bpm, -1, -1);
    if (!glide.present && c.globalStepOps) glide = extractGlide(*c.globalStepOps, c.bpm, -1, -1);
    if (glide.present) {
        const std::string key = std::to_string(enclosingSerial) + "|glide";
        if (!c.ctrlEmitted->count(key)) {
            c.ctrlEmitted->insert(key);
            pb.glide((float) glide.timeBeats);
        }
    }
}

// ── Scope ops (new scope model). ──────────────────────────────────────────
double dslTimescaleOf(const std::vector<std::pair<juce::String, std::vector<ParseValue>>>& ops,
                      double bpm)
{
    for (const auto& [name, args] : ops)
        if (name == "timescale" && !args.empty()) {
            const double f = resolveValue(args[0], "rate", bpm, 1.0);
            if (f > 0.0) return f;
        }
    return 1.0;
}

// §5 rule 7 — `op:0` universally disables, and a missing/mid-edit argument
// (`reverse:` with the value deleted) is a no-op, never an activation. A
// scope op whose first argument is numeric zero (or absent) emits nothing.
// String args (groove templates, invert note pivots) are never "zero".
bool scopeOpZeroDisabled(const std::vector<ParseValue>& args)
{
    if (args.empty() || args[0].kind == ParseValue::Kind::Null) return true;
    if (args[0].kind == ParseValue::Kind::String) return false;
    // F-071 T-571a — a control-op arg (`timescale:~lfo`, etc.) is a live value,
    // never "off": getNumVal reads a generator as 0.0, which would wrongly
    // §5-rule-7-disable the op and drop it before emission.
    if (isSignalExprArg(args[0])) return false;
    return getNumVal(args[0], 0.0) == 0.0;
}

void emitScopeOpsV2(V2Ctx& c,
                    const std::vector<std::pair<juce::String, std::vector<ParseValue>>>& ops)
{
    ProgramBuilder& pb = *c.pb;
    for (const auto& [name, args] : ops) {
        if (scopeOpZeroDisabled(args)) continue;   // §5 rule 7: op:0 = off
        bool hasDynamicArg = false;
        bool hasSignalChainArg = false;
        for (const auto& arg : args) {
            hasDynamicArg = hasDynamicArg || isSignalExprArg(arg);
            hasSignalChainArg = hasSignalChainArg
                             || (arg.kind == ParseValue::Kind::SignalChain
                                 && arg.signalChain != nullptr);
        }

        if (hasDynamicArg) {
            const bool supportedTimescaleExpr =
                name == "timescale"
                && args.size() == 1
                && isSignalExprArg(args[0]);
            const bool supportedBpmExpr =
                name == "bpm"
                && args.size() == 1
                && isSignalExprArg(args[0]);
            const bool supportedTransposeExpr =
                (name == "transpose" || name == "octave")
                && args.size() == 1
                && isSignalExprArg(args[0]);
            const bool supportedRotateExpr =
                name == "rotate"
                && args.size() == 1
                && isSignalExprArg(args[0]);
            const bool supportedInvertExpr =
                name == "invert"
                && args.size() == 1
                && isSignalExprArg(args[0]);
            const bool supportedGrooveExpr =
                name == "groove"
                && args.size() <= 4
                && std::any_of(args.begin() + (args.empty() ? 0 : 1),
                               args.end(), [] (const ParseValue& arg) {
                                   return isSignalExprArg(arg);
                               });
            const bool supportedGridExpr =
                name == "grid"
                && !args.empty()
                && args.size() <= 2;
            const bool supportedFitExpr =
                name == "fit"
                && args.size() == 1
                && isSignalExprArg(args[0]);
            const ParseValue* scaleModeArg =
                name == "scale"
                    ? ((args.size() >= 2) ? &args[1]
                       : (args.size() == 1 ? &args[0] : nullptr))
                    : nullptr;
            const bool supportedScaleModeGen =
                scaleModeArg != nullptr
                && isGenArg(*scaleModeArg)
                && !hasSignalChainArg;
            const bool supportedScaleRootExpr =
                name == "scale"
                && args.size() >= 2
                && isSignalExprArg(args[0])
                && scaleModeArg != nullptr
                && !isSignalExprArg(*scaleModeArg);
            if (!supportedTimescaleExpr && !supportedBpmExpr
                && !supportedTransposeExpr && !supportedRotateExpr
                && !supportedInvertExpr && !supportedGrooveExpr
                && !supportedGridExpr && !supportedFitExpr && !supportedScaleModeGen
                && !supportedScaleRootExpr) {
                c.shared->fail("compile-error: dynamic expression argument to structural op '"
                               + name
                               + "' is not supported by the current VM operand model yet");
                return;
            }
        }

        if (name == "transpose") {
            if (!args.empty() && isSignalExprArg(args[0])) {
                FireExprV2 expr;
                if (!compileFireExprV2(c, args[0], "param", 0.0, expr))
                    return;
                pb.transposeExpr(expr.code, expr.immediates, 1.0f);
            } else {
                pb.transpose((float) getNumVal(args[0], 0.0));
            }
        } else if (name == "octave") {
            if (!args.empty() && isSignalExprArg(args[0])) {
                FireExprV2 expr;
                if (!compileFireExprV2(c, args[0], "param", 0.0, expr))
                    return;
                pb.transposeExpr(expr.code, expr.immediates, 12.0f);
            } else {
                pb.transpose((float) (getNumVal(args[0], 0.0) * 12.0));
            }
        } else if (name == "reverse") {
            pb.reverse();
        } else if (name == "rotate") {
            if (!args.empty() && isSignalExprArg(args[0])) {
                FireExprV2 expr;
                if (!compileFireExprV2(c, args[0], "count", 0.0, expr))
                    return;
                pb.rotateExpr(expr.code, expr.immediates);
            } else {
                pb.rotate((int16_t) (int) getNumVal(args[0], 0.0));
            }
        } else if (name == "shuffle") {
            // §5.5: shuffle:1 = ON with the default (auto) seed;
            // shuffle:N = that seed.
            const double a = getNumVal(args[0], 0.0);
            const uint32_t seed = (a == 1.0)
                ? c.shared->stampKey("shuffle@" + std::to_string(c.scopeSerial))
                : (uint32_t) a;
            pb.shuffle(seed);
        } else if (name == "groove") {
            const juce::String tplName = (!args.empty() && args[0].kind == ParseValue::Kind::String)
                ? args[0].str : juce::String("swing");
            int tplId = 0;
            if      (tplName == "swing")   tplId = 0;
            else if (tplName == "shuffle") tplId = 1;
            else if (tplName == "push")    tplId = 2;
            else if (tplName == "quint")   tplId = 3;
            else if (tplName == "sept")    tplId = 4;
            else if (tplName == "elastic") tplId = 5;
            else if (tplName == "samba")   tplId = 6;
            else if (tplName == "fractal") tplId = 7;
            const bool dynGroove =
                std::any_of(args.begin() + (args.empty() ? 0 : 1),
                            args.end(), [] (const ParseValue& arg) {
                                return isSignalExprArg(arg);
                            });
            if (dynGroove) {
                const ParseValue amount = args.size() > 1 ? args[1] : numericParseValue(50.0);
                const ParseValue velLo  = args.size() > 2 ? args[2] : numericParseValue(0.5);
                const ParseValue velHi  = args.size() > 3 ? args[3] : numericParseValue(1.0);
                FireExprV2 amountExpr, velLoExpr, velHiExpr;
                if (!compileFireExprV2(c, amount, "unit", 50.0, amountExpr)
                    || !compileFireExprV2(c, velLo, "unit", 0.5, velLoExpr)
                    || !compileFireExprV2(c, velHi, "unit", 1.0, velHiExpr))
                    return;
                pb.grooveExpr((uint8_t) tplId,
                              amountExpr.code, amountExpr.immediates,
                              velLoExpr.code, velLoExpr.immediates,
                              velHiExpr.code, velHiExpr.immediates);
            } else {
                pb.groove((uint8_t) tplId,
                          (float) (args.size() > 1 ? getNumVal(args[1], 50.0) : 50.0),
                          (float) (args.size() > 2 ? getNumVal(args[2], 0.5)  : 0.5),
                          (float) (args.size() > 3 ? getNumVal(args[3], 1.0)  : 1.0));
            }
        } else if (name == "invert") {
            if (!args.empty() && isSignalExprArg(args[0])) {
                FireExprV2 expr;
                if (!compileFireExprV2(c, args[0], "param", 0.0, expr))
                    return;
                pb.invertExpr(expr.code, expr.immediates);
            } else {
                // §5.1: invert:0 = off (zero-gated above); invert:1 = pivot on
                // the first note; invert:c4 = note pivot; invert:N (2-127) =
                // pivot on the Nth content item's pitch (B-288 — runtime
                // content-index mode; wraps modulo the content count). Other
                // numerics (negative / fractional) = semitone pivot.
                if (args[0].kind == ParseValue::Kind::String) {
                    const int midi = curlop::pitch::noteToMidi(args[0].str);
                    if (midi >= 0) pb.invert((float) (midi - 60), 0);
                    else           pb.invert(0.0f, 1);
                } else {
                    const double v = getNumVal(args[0], 0.0);
                    if (v == 1.0)
                        pb.invert(0.0f, 1);
                    else if (v >= 2.0 && v <= 127.0 && v == std::floor(v))
                        pb.invert((float) v, 2);
                    else
                        pb.invert((float) v, 0);
                }
            }
        } else if (name == "timescale") {
            // Dynamic `timescale(expr)` samples the expression per covered step in
            // evalStepCommon, then converts the authored DSL factor to the VM's
            // inverse machine factor at fire time.
            if (!args.empty() && isSignalExprArg(args[0])) {
                FireExprV2 expr;
                if (!compileFireExprV2(c, args[0], "rate", 1.0, expr))
                    return;
                pb.timescaleExpr(expr.code, expr.immediates);
            } else {
                const double f = args.empty() ? 1.0 : resolveValue(args[0], "rate", c.bpm, 1.0);
                if (f > 0.0 && f != 1.0) pb.timescale((float) machineTimescale(f));
            }
        } else if (name == "fit") {
            if (!args.empty() && isSignalExprArg(args[0])) {
                FireExprV2 expr;
                if (!compileFireExprV2(c, args[0], "count", 1.0, expr))
                    return;
                pb.scopeFitExpr(expr.code, expr.immediates);
            }
        } else if (name == "bpm") {
            // F-071 T-571b — nested `bpm:N` = a scope-local tempo override
            // (SCOPE_BPM). The kernel sets frame.timescale = N / scriptBpm so the
            // inner offsets compress to play at N (fixes the collide-at-0 bug where
            // the slot shrank but offsets didn't). ROOT bpm never reaches here
            // (skipped in compileV2's seqScopeOps loop). Dynamic `bpm(expr)` lowers
            // through TIMESCALE_EXPR mode 1, using the authored tempo directly.
            if (!args.empty() && isSignalExprArg(args[0])) {
                FireExprV2 expr;
                if (!compileFireExprV2(c, args[0], "tempo", c.bpm, expr))
                    return;
                pb.timescaleExpr(expr.code, expr.immediates, 1);
            } else {
                // B-335 — convert the arg through the SAME unit domain as root @bpm
                // (resolveValue "tempo"): a bare number is the bpm; ms/s/hz/khz and a
                // pitch name convert to a tempo (500ms→120, 2hz→120, 2s→30, c3→Hz×60).
                // getNumVal dropped the unit, emitting the raw number (500ms→500).
                const double n = args.empty() ? 0.0
                                              : resolveValue(args[0], "tempo", c.bpm, 0.0);
                if (n > 0.0) pb.scopeBpm((float) n);
            }
        } else if (name == "scale") {
            // B-289 — runtime SCALE frame op (SPEC-016). Registry resolution
            // (name → intervals) is compile-time TRANSLATION; quantization
            // happens in resolvePitch at fire time, covering varref content
            // and inner-frame transposes the deleted quantizeStepPitches
            // path could never see. Arg forms mirror the v1 grammar:
            //   scale(mode) | scale(root, mode)
            // Forms: scale(mode) | scale(root, mode). The MODE slot (trailing
            // arg) accepts a control op (F-071 T-539) — a stochastic gen draws
            // one scale per loop. The root slot accepts a fire-time scalar
            // expression when the mode is static.
            const ParseValue* rootArg = (args.size() >= 2) ? &args[0] : nullptr;
            const ParseValue* modeArg = (args.size() >= 2) ? &args[1]
                                      : (args.size() == 1 ? &args[0] : nullptr);
            juce::String root = (rootArg && rootArg->kind == ParseValue::Kind::String)
                              ? rootArg->str : juce::String("c");
            const bool dynamicRoot = rootArg != nullptr && isSignalExprArg(*rootArg);

            if (modeArg && isGenArg(*modeArg)) {
                // scale(c, ~bernoulli(w, minor, major)) | scale(~random(minor, major, ...))
                const GeneratorNode& g = *modeArg->gen;
                uint8_t kind = 0; float weight = 0.5f; size_t optStart = 0;
                bool dynamicWeight = false;
                FireExprV2 weightExpr;
                if (g.generatorType == "bernoulli") {
                    kind = 1; optStart = 1;
                    const ParseValue weightArg =
                        g.args.empty() ? ParseValue{} : g.args[0];
                    dynamicWeight = isSignalExprArg(weightArg);
                    if (dynamicWeight) {
                        if (!compileFireExprV2(c, weightArg, "unit", 0.5, weightExpr))
                            return;
                    } else {
                        weight = (float) resolveValue(weightArg, "unit", c.bpm, 0.5);
                    }
                } else if (g.generatorType == "random") {
                    if (failIfDynamicSignalArgs(c, g.args,
                                                juce::String("scale:~") + g.generatorType))
                        return;
                    kind = 0; optStart = 0;
                } else {
                    // ~lfo/~auto = continuous morph; needs the control-expr
                    // substrate (T-513). Loud-fail, never silent (LIVE-ARG-INV-4).
                    failGenOperand(c, (juce::String("scale:~") + g.generatorType).toRawUTF8());
                    return;
                }
                std::vector<std::vector<float>> candidates;
                for (size_t i = optStart; i < g.args.size(); ++i) {
                    if (g.args[i].kind != ParseValue::Kind::String) continue;
                    const auto& iv = curlop::music::scaleIntervals(
                        g.args[i].str.toLowerCase().toStdString());
                    candidates.emplace_back(iv.begin(), iv.end());
                }
                if (candidates.size() < 2) {
                    c.shared->fail("compile-error: scale with a control op needs "
                                   ">= 2 named scale options (e.g. scale(c, "
                                   "~bernoulli(0.5, minor, major)))");
                    return;
                }
                const float rootChroma =
                    (float) curlop::music::noteChroma(root.toLowerCase().toStdString());
                const uint32_t seed = c.shared->stamp(&g);
                if (dynamicRoot) {
                    FireExprV2 rootExpr;
                    if (!compileFireExprV2(c, *rootArg, "param",
                                           (double) rootChroma, rootExpr))
                        return;
                    const std::vector<uint16_t> emptyWeightExpr;
                    const std::vector<float> staticWeightImm { weight };
                    pb.scaleGenRootDyn(rootExpr.code, rootExpr.immediates, kind,
                                       dynamicWeight ? weightExpr.code : emptyWeightExpr,
                                       dynamicWeight ? weightExpr.immediates : staticWeightImm,
                                       seed, candidates);
                } else if (dynamicWeight) {
                    pb.scaleGenDyn(rootChroma, kind, weightExpr.code,
                                   weightExpr.immediates, seed, candidates);
                } else {
                    pb.scaleGen(rootChroma, kind, weight, seed, candidates);
                }
            } else {
                const juce::String mode = (modeArg && modeArg->kind == ParseValue::Kind::String)
                                        ? modeArg->str : juce::String("chromatic");
                const auto& iv = curlop::music::scaleIntervals(
                    mode.toLowerCase().toStdString());
                std::vector<float> intervals(iv.begin(), iv.end());
                if (dynamicRoot) {
                    FireExprV2 rootExpr;
                    if (!compileFireExprV2(c, *rootArg, "param",
                                           (double) curlop::music::noteChroma(
                                               root.toLowerCase().toStdString()),
                                           rootExpr))
                        return;
                    pb.scaleExpr(rootExpr.code, rootExpr.immediates, intervals);
                } else {
                    pb.scale((float) curlop::music::noteChroma(
                                 root.toLowerCase().toStdString()),
                             intervals);
                }
            }
        } else if (name == "grid") {
            // B-291 — runtime GRID frame op (SPEC-016): every hit firing
            // inside the frame snaps its fire-time onset (onset:/groove
            // shifts included) toward the grid, lerped by strength. The
            // compile-time quantizeStepsToGrid AST rewrite is deleted —
            // it never saw varref content or nested frames.
            if (!args.empty()
                && (isSignalExprArg(args[0])
                    || (args.size() > 1 && isSignalExprArg(args[1])))) {
                FireExprV2 sizeExpr, strengthExpr;
                if (!compileFireExprV2(c, args[0], "time", 0.25, sizeExpr))
                    return;
                if (!compileFireExprV2(c,
                                       args.size() > 1 ? args[1] : ParseValue{},
                                       "unit", 1.0, strengthExpr))
                    return;
                pb.gridExpr(sizeExpr.code, sizeExpr.immediates,
                            strengthExpr.code, strengthExpr.immediates);
            } else {
                const double size = args.empty()
                    ? 0.25 : resolveValue(args[0], "time", c.bpm, 0.25);
                const double strength = args.size() > 1
                    ? resolveValue(args[1], "unit", c.bpm, 1.0) : 1.0;
                if (size > 0.0)
                    pb.grid((float) size, (float) juce::jlimit(0.0, 1.0, strength));
            }
        } else if (name == "sort") {
            // B-288 — runtime SORT frame op (SPEC-016): the frame's content
            // (steps AND child scopes, B-292) plays in pitch order of each
            // item's first note. Args mirror the v1 grammar:
            //   sort:pitch | sort(pitch, desc)  (pitch is the only v1 key)
            const bool desc = args.size() > 1
                && args[1].kind == ParseValue::Kind::String
                && args[1].str == "desc";
            pb.sort(desc ? (uint8_t) 1 : (uint8_t) 0);
        }
    }
}

// ── The projected walk. ───────────────────────────────────────────────────
bool walkAndEmitV2(const std::vector<AstNodePtr>& steps, V2Ctx& c, double parentOffset);

// B-277 — the arp/strum NOTE LIST is per-module-projection: this module's
// triggers (already scope-filtered by the caller's coveredBy) contribute
// their notes, and a chord trigger contributes its VOICINGS as individual
// elements (never the whole chord per sub-step). Returns true if it emitted
// for this projection (false → caller falls through to the plain step body).
bool emitArpOrStrumV2(V2Ctx& c, const AstNode& step,
                      const std::vector<const AstNode*>& triggers,
                      bool isArp, const juce::String& arpType, double subDurOrSpacing,
                      double beatOffset, double duration,
                      const ParseValue* speedExpr = nullptr,
                      bool forceTrainForm = false)
{
    struct ArpElement {
        const AstNode* ev;
        double semis = 0.0;
        bool   hasOverride = false;
    };
    std::vector<ArpElement> elems;
    for (const AstNode* ev : triggers) {
        if (ev->module != c.target) continue;
        bool expanded = false;
        if (! ev->pitch.isEmpty()) {
            const auto resolved = curlop::pitch::resolvePitch(ev->pitch);
            if (resolved.voicings.size() > 1) {
                for (double v : resolved.voicings)
                    elems.push_back({ ev, v - 60.0, true });
                expanded = true;
            }
        }
        if (! expanded)
            elems.push_back({ ev, 0.0, false });
    }
    if (elems.empty())
        return false;

    constexpr double kNoOverride = std::numeric_limits<double>::quiet_NaN();

    if (isArp && (speedExpr != nullptr || forceTrainForm)) {
        // The one-STEP pitched-train form: ONE step + (optional GEN_OPERAND) +
        // RATCHET_PITCHED cycling the arp-ordered voicings; the machine derives
        // the retrigger count from the speed (count = stepDur / speed).
        //   • B-186 — generator-driven arp speed: GEN_OPERAND draws the speed
        //     per loop iteration (arm time).
        //   • B-313/B-314 — a CONSTANT-speed arp inside a bernoulli option (or
        //     covering the selector) MUST stay one STEP to honour the machine's
        //     option-countdown contract; forceTrainForm routes it here instead
        //     of the compile-time N-sub-step expansion below. count is constant.
        ProgramBuilder& pb = *c.pb;
        const AstNode& event = *elems[0].ev;
        const int cg = event.commaGroup;

        applyArpOrder(elems, arpType, c.shared->globalSeed);
        std::vector<float> semis;
        semis.reserve(elems.size());
        for (const auto& el : elems)
            semis.push_back(el.hasOverride ? (float) el.semis
                            : (float) semisOfChain(el.ev->pitch));

        const auto implicitOps = collectImplicitScopeOps(step, event, 0, cg);
        const bool wrapped = !implicitOps.empty();
        if (wrapped) {
            ++c.scopeSerial;
            pb.scopeStart(beatOffset, duration * dslTimescaleOf(implicitOps, c.bpm));
            emitScopeOpsV2(c, implicitOps);
        }
        const double off = wrapped ? 0.0 : beatOffset;

        if (c.stepOrdinals) c.stepOrdinals->push_back(c.topStepOrdinal);
        pb.step(off, duration);

        double speed;   // beats per arp note
        if (speedExpr != nullptr && isGenArg(*speedExpr)) {
            const auto& g = *speedExpr->gen;
            const bool hasDynamicArg =
                std::any_of(g.args.begin(), g.args.end(),
                            [] (const ParseValue& v) { return isSignalExprArg(v); });
            const auto sub = [&] (size_t i, const char* dom, double fb) {
                return i < g.args.size() ? resolveValue(g.args[i], dom, c.bpm, fb) : fb;
            };
            if (g.generatorType == "lfo") {
                FireExprV2 expr, unused;
                if (!compileFireExprGeneratorV2(c, g, "time", duration / 4.0, expr))
                    return false;
                appendFireImmediateV2(unused, 0.0f);
                pb.genOperandDyn(vm::GenTarget::ArpSpeedBeats, 0,
                                 c.shared->stamp(&g),
                                 expr.code, expr.immediates,
                                 expr.code, expr.immediates,
                                 unused.code, unused.immediates);
                speed = expr.midpoint;
            } else if (g.generatorType == "random") {
                const ParseValue aArg = g.args.size() > 0 ? g.args[0] : ParseValue{};
                const ParseValue bArg = g.args.size() > 1 ? g.args[1] : ParseValue{};
                FireExprV2 ae, be, we;
                const double a = sub(0, "time", duration / 4.0);
                const double b = sub(1, "time", duration / 4.0);
                if (hasDynamicArg) {
                    if (!compileFireExprV2(c, aArg, "time", duration / 4.0, ae)
                        || !compileFireExprV2(c, bArg, "time", duration / 4.0, be)
                        || !compileFireExprV2(c, ParseValue{}, "unit", 0.0, we))
                        return false;
                    pb.genOperandDyn(vm::GenTarget::ArpSpeedBeats, 0,
                                     c.shared->stamp(&g),
                                     ae.code, ae.immediates,
                                     be.code, be.immediates,
                                     we.code, we.immediates);
                } else {
                    pb.genOperand(vm::GenTarget::ArpSpeedBeats, 0, (float) a, (float) b,
                                  0.0f, c.shared->stamp(&g));
                }
                speed = (a + b) * 0.5;
            } else if (g.generatorType == "auto") {
                // F-071 Phase 1 (T-511) — ramp arp speed: kind 2 carries BOTH
                // endpoints; the machine's variable-interval cursor interpolates
                // the inter-hit interval per hit across the span (accelerando).
                const ParseValue aArg = g.args.size() > 0 ? g.args[0] : ParseValue{};
                const ParseValue bArg = g.args.size() > 1 ? g.args[1] : ParseValue{};
                const double a = sub(0, "time", duration / 4.0);
                const double b = sub(1, "time", duration / 16.0);
                if (hasDynamicArg) {
                    FireExprV2 ae, be, we;
                    if (!compileFireExprV2(c, aArg, "time", duration / 4.0, ae)
                        || !compileFireExprV2(c, bArg, "time", duration / 16.0, be)
                        || !compileFireExprV2(c, ParseValue{}, "unit", 0.0, we))
                        return false;
                    pb.genOperandDyn(vm::GenTarget::ArpSpeedBeats, 2,
                                     c.shared->stamp(&g),
                                     ae.code, ae.immediates,
                                     be.code, be.immediates,
                                     we.code, we.immediates);
                } else {
                    pb.genOperand(vm::GenTarget::ArpSpeedBeats, 2, (float) a, (float) b,
                                  0.0f, c.shared->stamp(&g));
                }
                speed = (a + b) * 0.5;   // fallback trainCount; the ramp ignores it
            } else {  // bernoulli — random/auto handled above
                const ParseValue wArg = g.args.size() > 0 ? g.args[0] : ParseValue{};
                const ParseValue aArg = g.args.size() > 1 ? g.args[1] : ParseValue{};
                const ParseValue bArg = g.args.size() > 2 ? g.args[2] : ParseValue{};
                const double w = sub(0, "unit", 0.5);
                const double a = sub(1, "time", duration / 4.0);
                const double b = sub(2, "time", duration / 4.0);
                if (hasDynamicArg) {
                    FireExprV2 ae, be, we;
                    if (!compileFireExprV2(c, aArg, "time", duration / 4.0, ae)
                        || !compileFireExprV2(c, bArg, "time", duration / 4.0, be)
                        || !compileFireExprV2(c, wArg, "unit", 0.5, we))
                        return false;
                    pb.genOperandDyn(vm::GenTarget::ArpSpeedBeats, 1,
                                     c.shared->stamp(&g),
                                     ae.code, ae.immediates,
                                     be.code, be.immediates,
                                     we.code, we.immediates);
                } else {
                    pb.genOperand(vm::GenTarget::ArpSpeedBeats, 1, (float) a, (float) b,
                                  (float) w, c.shared->stamp(&g));
                }
                speed = (a + b) * 0.5;
            }
        } else if (speedExpr != nullptr && isSignalExprArg(*speedExpr)) {
            FireExprV2 expr, unused;
            if (!compileFireExprV2(c, *speedExpr, "time", duration / 4.0, expr))
                return false;
            appendFireImmediateV2(unused, 0.0f);
            const uint32_t seed = speedExpr->kind == ParseValue::Kind::SignalChain
                                ? c.shared->stamp(speedExpr->signalChain.get())
                                : c.shared->stamp(speedExpr);
            pb.genOperandDyn(vm::GenTarget::ArpSpeedBeats, 0,
                             seed,
                             expr.code, expr.immediates,
                             expr.code, expr.immediates,
                             unused.code, unused.immediates);
            speed = expr.midpoint;
        } else {
            // B-313/B-314 — constant arp speed; count derives at compile time,
            // no GEN_OPERAND. subDurOrSpacing is the arp speed in beats.
            speed = subDurOrSpacing;
        }
        const int trainCount = (int) std::min(20000.0,
            std::max(1.0, std::floor(duration / std::max(1e-6, speed))));
        pb.ratchetPitched((uint32_t) trainCount, semis);
        pb.note(semis.empty() ? 0.0f : semis[0],
                (float) extractVelocity(event.processors));
        emitLocksV2(c, collectLocksV2(c, step, event, 0, cg, duration));
        if (wrapped) pb.scopeEnd();
        return true;
    }

    if (isArp) {
        static constexpr int kMaxArpSubSteps = 20000;   // audible retrigger ceiling (shared with ratchet)
        double subStepDur = subDurOrSpacing;
        int total = 1;
        if (subStepDur > 0.0 && std::isfinite(subStepDur)) {
            const double raw = std::floor(duration / subStepDur);
            if (raw >= 1.0)
                total = (int) std::min(raw, (double) kMaxArpSubSteps);
        } else {
            subStepDur = duration;
        }
        applyArpOrder(elems, arpType, c.shared->globalSeed);
        for (int si = 0; si < total; ++si) {
            const ArpElement& el = elems[(size_t) si % elems.size()];
            const double sbo = beatOffset + si * subStepDur;
            emitTriggerBodyV2(c, step, *el.ev, 0, el.ev->commaGroup, sbo, subStepDur,
                              el.hasOverride ? el.semis : kNoOverride);
        }
        return true;
    }

    // Strum: each element staggers once (low/appearance order first), holding
    // toward the step end like a real strum.
    for (size_t si = 0; si < elems.size(); ++si) {
        const ArpElement& el = elems[si];
        const double sbo = beatOffset + (double) si * subDurOrSpacing;
        const double gd = std::max(duration - (double) si * subDurOrSpacing,
                                   duration * 0.1);
        emitTriggerBodyV2(c, step, *el.ev, (int) si, el.ev->commaGroup, sbo, gd,
                          el.hasOverride ? el.semis : kNoOverride);
    }
    return true;
}

// True when the pitch chain resolves to more than one simultaneous voice
// (a chord) — arp over a single note is just that note, no train (B-313/B-314).
bool isChordPitch(const curlop::script::PitchChain& pitch)
{
    if (pitch.isEmpty()) return false;
    return curlop::pitch::resolvePitch(pitch).voicings.size() > 1;
}

// Arp type + constant speed read straight off a known arp Processor (mirrors
// extractArp's arg parsing). Used when the proc is already in hand and its
// scope can't be re-matched against the inner option index (B-314 postfix arp).
ArpResult arpSpecOf(const Processor& p, double stepDuration, double bpm)
{
    const auto arg0 = p.args.size() > 0 ? p.args[0] : ParseValue{};
    const auto arg1 = p.args.size() > 1 ? p.args[1] : ParseValue{};
    juce::String type = "up";
    if (arg0.kind == ParseValue::Kind::String) type = arg0.str;
    double speed = stepDuration / 4.0;
    const ParseValue speedArg = p.args.size() > 1 ? arg1
                             : (arg0.kind == ParseValue::Kind::String ? ParseValue{} : arg0);
    if (speedArg.kind != ParseValue::Kind::Null && ! isSignalExprArg(speedArg))
        speed = resolveValue(speedArg, "time", bpm, stepDuration / 4.0);
    return {true, type, speed};
}

// The expression driving an arp's speed, if any. Bare ~random/~bernoulli draw
// once per loop iteration (arm time); bare ~auto is a RAMP — the inter-hit
// interval is re-evaluated per hit across the step span (F-071 Phase 1 /
// T-511, SPEC-018 §1.1). A full signal chain is lowered as a sampled speed
// expression so transforms such as `arp(up, lfo(...) : clip(...))` do not bake
// to the default speed.
const ParseValue* arpSpeedExprV2(const Processor* p, V2Ctx& c)
{
    if (p == nullptr) return nullptr;
    const ParseValue* speedArg = nullptr;
    if (p->args.size() > 1)
        speedArg = &p->args[1];
    else if (p->args.size() == 1 && p->args[0].kind != ParseValue::Kind::String)
        speedArg = &p->args[0];
    if (speedArg == nullptr)
        return nullptr;

    if (speedArg->kind == ParseValue::Kind::SignalChain && speedArg->signalChain)
        return speedArg;

    if (isGenArg(*speedArg)) {
        const auto& a = *speedArg;
        if (a.gen->generatorType == "random" || a.gen->generatorType == "bernoulli"
            || a.gen->generatorType == "auto" || a.gen->generatorType == "lfo") {
            return speedArg;
        }
        failGenOperand(c, (juce::String("arp:~") + a.gen->generatorType).toRawUTF8());
        return nullptr;
    }
    return nullptr;
}

void emitBernoulliOptionsV2(V2Ctx& c, const AstNode& step, const AstNode& event,
                            int eventIdx, double beatOffset, double stepDur,
                            const AstNode* original = nullptr,
                            const Processor* coveringArp = nullptr)
{
    // Top-level BernoulliSelector: option positions must align across
    // projections (the shared seed picks the same index everywhere), so
    // non-target options become REST steps.
    ProgramBuilder& pb = *c.pb;

    // B-314 — a step-level arp postfixed over the selector (`coveringArp`,
    // found + scoped by the caller) covers ALL options (§5 one-law); each chord
    // option arpeggiates, mirroring how B-284 distributes scope ops per option.
    const Processor* postfixArpProc = coveringArp;

    pb.bernoulli((float) event.weight, (uint8_t) event.options.size(),
                 c.shared->stamp(&event));
    for (const auto& opt : event.options) {
        const AstNode* o = opt.get();
        if (o != nullptr && o->eventType == "keep")
            o = original;   // §5.8 — empty option replays the written event
        if (o == nullptr || o->eventType == "rest" || o->module != c.target) {
            if (c.stepOrdinals) c.stepOrdinals->push_back(c.topStepOrdinal);
            pb.step(beatOffset, stepDur);
            pb.rest();
            continue;
        }

        // B-313/B-314 — an arp on the option itself, or the selector-covering
        // postfix arp, over a CHORD emits the pitched-train form: exactly ONE
        // STEP per option (the option-countdown contract), voicings cycling.
        // arp over a single note = that note → plain emit below.
        const Processor* optArpProc = extractProcessor(o->processors, {"arp"}, -1, -1);
        const Processor* arpProc    = optArpProc ? optArpProc : postfixArpProc;
        if (arpProc != nullptr && isChordPitch(o->pitch)) {
            const ParseValue* speedExpr = arpSpeedExprV2(arpProc, c);
            if (c.shared->error.isNotEmpty()) return;
            const ArpResult ar = arpSpecOf(*arpProc, stepDur, c.bpm);
            std::vector<const AstNode*> one { o };
            emitArpOrStrumV2(c, step, one, true, ar.type, ar.speed,
                             beatOffset, stepDur, speedExpr, /*forceTrainForm*/ true);
            continue;
        }

        // T-500 — every other option is FULL step content, routed through the
        // one real step emitter: articulations, chords, locks, and scope ops on
        // the option (or covering the selector at step level — B-284's
        // every-option-plays-transformed law lands per option, same audible
        // result, one mechanism) all work. Exactly one STEP header per option.
        emitTriggerBodyV2(c, step, *o, eventIdx, o->commaGroup,
                          beatOffset, stepDur);
    }
}

bool emitBareBernoulliV2(V2Ctx& c, const AstNode& step, const AstNode& event,
                         int eventIdx, int cg, double beatOffset, double stepDur)
{
    const Processor* bernP = extractProcessor(step.processors, {"bernoulli"}, cg, eventIdx);
    if (!bernP) bernP = extractProcessor(event.processors, {"bernoulli"});
    if (!bernP || bernP->args.size() < 3) return false;

    ProgramBuilder& pb = *c.pb;

    // B-284 — scope ops covering this trigger wrap the bernoulli block too
    // (§5 one-law: whichever option fires, it fires transformed).
    const auto implicitOps = collectImplicitScopeOps(step, event, eventIdx, cg);
    const bool wrapped = !implicitOps.empty();
    if (wrapped) {
        ++c.scopeSerial;
        pb.scopeStart(beatOffset, stepDur * dslTimescaleOf(implicitOps, c.bpm));
        emitScopeOpsV2(c, implicitOps);
    }
    const double off = wrapped ? 0.0 : beatOffset;

    const bool dynamicWeight = isSignalExprArg(bernP->args[0]);
    FireExprV2 weightExpr;
    const double w = dynamicWeight
        ? 0.0
        : getNumVal(bernP->args[0], 0.5);
    if (dynamicWeight && !compileFireExprV2(c, bernP->args[0], "unit", 0.5, weightExpr))
        return false;
    const int optCount = (int) bernP->args.size() - 1;
    const uint32_t seed = c.shared->stamp(bernP);
    if (dynamicWeight)
        pb.bernoulliDyn(weightExpr.code, weightExpr.immediates,
                        (uint8_t) optCount, seed);
    else
        pb.bernoulli((float) w, (uint8_t) optCount, seed);
    auto locks = collectLocksV2(c, step, event, eventIdx, cg, stepDur);
    const float vel = (float) extractVelocity(event.processors);
    const double fallbackSemis = !event.pitch.isEmpty() ? semisOfChain(event.pitch) : 9.0;
    for (int oi = 0; oi < optCount; ++oi) {
        const ParseValue& opt = bernP->args[1 + oi];
        if (c.stepOrdinals) c.stepOrdinals->push_back(c.topStepOrdinal);
        pb.step(off, stepDur);
        if (opt.kind == ParseValue::Kind::String) {
            if (opt.str == "-" || opt.str == "rest") {
                pb.rest();
            } else {
                const int midi = curlop::pitch::noteToMidi(opt.str);
                pb.note((float) (midi >= 0 ? midi - 60 : fallbackSemis), vel);
                emitLocksV2(c, locks);
            }
        } else if (isSignalExprArg(opt)) {
            FireExprV2 pitchExpr;
            if (!compileFireExprV2(c, opt, "note", fallbackSemis, pitchExpr))
                return false;
            pb.noteExpr(pitchExpr.code, pitchExpr.immediates, pitchExpr.midpoint, vel);
            emitLocksV2(c, locks);
        } else {
            pb.note((float) fallbackSemis, vel);
            emitLocksV2(c, locks);
        }
    }
    if (wrapped) pb.scopeEnd();
    return true;
}

struct MarkovLockSpec {
    bool present = false;
    juce::String param;
    float value = 0.0f;
    bool isOffset = false;
};

bool extractMarkovLockSpec(V2Ctx& c, const AstNode& event, double stepDur,
                           MarkovLockSpec& out)
{
    int lockCount = 0;
    for (const auto& p : event.processors) {
        if (p.args.empty())
            continue;
        const juce::String upper = p.name.toUpperCase();
        if (nonModuleParams().count(upper) || nonLockRouteOps().count(p.name))
            continue;
        if (! p.isDotParam || isStreamRouteValue(p.args[0]))
            continue;
        bool isPct = false;
        const float val = normLockValue(c, upper, p.args[0], stepDur, isPct);
        if (isPct) {
            c.shared->fail("V2_UNSUPPORTED_MARKOV_LOCKS: Markov param locks currently lower absolute values only");
            return false;
        }
        out = { true, upper, val, isPct };
        ++lockCount;
        if (lockCount > 1) {
            c.shared->fail("V2_UNSUPPORTED_MARKOV_LOCKS: Markov runtime currently lowers one same-name param lock per state");
            return false;
        }
    }
    return true;
}

void emitMarkovSelectorV2(V2Ctx& c, const AstNode& step, const AstNode& event,
                          int eventIdx, double beatOffset, double stepDur)
{
    if (event.module != c.target)
        return;
    if (event.options.empty()) {
        c.shared->fail("V2_UNSUPPORTED_MARKOV_CORPUS: Markov has no corpus states");
        return;
    }

    std::vector<float> pitches;
    std::vector<float> velocities;
    std::vector<float> values;
    pitches.reserve(event.options.size());
    velocities.reserve(event.options.size());
    values.reserve(event.options.size());

    bool hasLock = false;
    juce::String lockParam;
    bool lockIsOffset = false;

    for (const auto& optPtr : event.options) {
        const AstNode* opt = optPtr.get();
        if (opt == nullptr || opt->eventType != "trigger" || opt->module != c.target) {
            c.shared->fail("V2_UNSUPPORTED_MARKOV_CORPUS: Markov states must be trigger events for the routed target");
            return;
        }
        const auto resolved = curlop::pitch::resolvePitch(opt->pitch);
        if (resolved.voicings.size() > 1) {
            c.shared->fail("V2_UNSUPPORTED_MARKOV_CHORD: Markov chord states need whole-event polyphonic selection semantics");
            return;
        }
        pitches.push_back((float) semisOfChain(opt->pitch));
        velocities.push_back((float) extractVelocity(opt->processors));

        MarkovLockSpec lock;
        if (! extractMarkovLockSpec(c, *opt, stepDur, lock))
            return;
        if (lock.present) {
            if (! hasLock) {
                hasLock = true;
                lockParam = lock.param;
                lockIsOffset = lock.isOffset;
            } else if (! lock.param.equalsIgnoreCase(lockParam)
                       || lock.isOffset != lockIsOffset) {
                c.shared->fail("V2_UNSUPPORTED_MARKOV_LOCKS: Markov states must share one param-lock lane in this slice");
                return;
            }
            values.push_back(lock.value);
        } else if (hasLock) {
            c.shared->fail("V2_UNSUPPORTED_MARKOV_LOCKS: Markov states must either all provide the same lock or none do");
            return;
        }
    }

    if (hasLock && values.size() != pitches.size()) {
        c.shared->fail("V2_UNSUPPORTED_MARKOV_LOCKS: Markov states must all provide the selected lock");
        return;
    }

    const uint16_t lane = hasLock ? laneOf(c, lockParam) : 0xffff;
    if (c.stepOrdinals) c.stepOrdinals->push_back(c.topStepOrdinal);
    c.pb->step(beatOffset, stepDur);
    const uint32_t seed = event.stateKey.isNotEmpty()
        ? c.shared->stampKey(event.stateKey.toStdString())
        : c.shared->stamp(&event);
    c.pb->markovEvent(pitches, velocities, lane, values, hasLock,
                      seed);
    emitLocksV2(c, collectLocksV2(c, step, event, eventIdx, event.commaGroup, stepDur));
}

void emitMarkovGateSelectorV2(V2Ctx& c, const AstNode& event,
                              double beatOffset, double stepDur)
{
    if (event.module != c.target)
        return;
    if (event.options.empty()) {
        c.shared->fail("V2_UNSUPPORTED_MARKOV_CORPUS: Markov gate projection has no corpus states");
        return;
    }

    std::vector<float> gates;
    std::vector<float> velocities;
    gates.reserve(event.options.size());
    velocities.reserve(event.options.size());

    for (const auto& optPtr : event.options) {
        const AstNode* opt = optPtr.get();
        if (opt == nullptr || opt->module != c.target) {
            c.shared->fail("V2_UNSUPPORTED_MARKOV_CORPUS: Markov gate states must target the routed module");
            return;
        }
        if (opt->eventType == "trigger") {
            gates.push_back(1.0f);
            velocities.push_back((float) extractVelocity(opt->processors));
        } else if (opt->eventType == "rest") {
            gates.push_back(0.0f);
            velocities.push_back(0.0f);
        } else {
            c.shared->fail("V2_UNSUPPORTED_MARKOV_PROJECTION: Markov gate projection supports note/rest states only");
            return;
        }
    }

    if (c.stepOrdinals) c.stepOrdinals->push_back(c.topStepOrdinal);
    c.pb->step(beatOffset, stepDur);
    const uint32_t seed = event.stateKey.isNotEmpty()
        ? c.shared->stampKey(event.stateKey.toStdString())
        : c.shared->stamp(&event);
    c.pb->markovGate(gates, velocities, seed);
}

void emitStepEventBodyV2(V2Ctx& c, const AstNode& step, double beatOffset, double duration);

struct LocalOutputGateSpec
{
    bool emitGate = true;
    double beats = 0.0;
};

LocalOutputGateSpec localOutputGateSpec(const AstNode& event, double stepDur, double bpm)
{
    LocalOutputGateSpec spec { true, stepDur };
    if (event.processors.empty()) return spec;
    const auto& p = event.processors.front();
    if (! p.name.equalsIgnoreCase("gate")
        && ! p.canonicalName.equalsIgnoreCase("gate"))
        return spec;
    if (p.args.empty() || p.args[0].kind == ParseValue::Kind::Null) return spec;
    const ParseValue& a = p.args[0];
    const double beats = (a.kind == ParseValue::Kind::UnitNumber)
        ? resolveValue(a, "time", bpm, stepDur)
        : getNumValOrNaN(a, 1.0) * stepDur;
    if (! std::isfinite(beats))
        return spec;
    if (beats <= 0.0)
        return { false, 0.0 };
    return { true, beats };
}

float localOutputValueNorm(V2Ctx& c, const juce::String& socketName,
                           const ParseValue& v, double fallback)
{
    if (v.kind == ParseValue::Kind::Null)
        return normAbs(c, socketName, fallback);
    if (v.kind == ParseValue::Kind::String) {
        const int midi = curlop::pitch::noteToMidi(v.str);
        if (midi >= 0)
            return normAbs(c, socketName, curlop::pitch::semiToHz((double) midi));
    }
    return normAbs(c, socketName, resolveValue(v, "param", c.bpm, fallback));
}

float localOutputVelocityValue(const ParseValue& v, double fallback)
{
    if (v.kind == ParseValue::Kind::Null)
        return (float) fallback;
    if (v.kind == ParseValue::Kind::UnitNumber && v.unit == "%")
        return (float) std::clamp(v.number / 100.0, 0.0, 1.0);
    if (v.kind == ParseValue::Kind::Number || v.kind == ParseValue::Kind::UnitNumber)
        return (float) std::clamp(v.number, 0.0, 1.0);
    return (float) fallback;
}

float localOutputPitchSignal(const Processor* proc)
{
    curlop::script::PitchChain chain;
    if (proc != nullptr && !proc->args.empty()) {
        const auto& a = proc->args[0];
        const juce::String raw = a.kind == ParseValue::Kind::String
            ? a.str
            : (a.kind == ParseValue::Kind::UnitNumber
                   ? juce::String(a.number) + a.unit
                   : juce::String(a.number));
        if (proc->name.equalsIgnoreCase("freq") || proc->canonicalName.equalsIgnoreCase("freq")
            || raw.endsWithIgnoreCase("hz")) {
            chain.freq = raw.endsWithIgnoreCase("hz") ? raw : raw + "hz";
        } else if (raw.endsWithIgnoreCase("c")) {
            chain.note = "c4";
            chain.cents = raw;
        } else {
            chain.note = raw;
        }
    }
    return vm::noteToSignal((float) semisOfChain(chain));
}

SignalExprDomain localOutputSignalExprDomain(vm::SignalType type)
{
    switch (type) {
        case vm::SignalType::Pitch:    return SignalExprDomain::PitchSignal;
        case vm::SignalType::Gate:
        case vm::SignalType::Velocity: return SignalExprDomain::UnitRaw;
        case vm::SignalType::Value:
        case vm::SignalType::Phase:
        case vm::SignalType::Audio:
            break;
    }
    return SignalExprDomain::UnitRaw;
}

void emitLocalOutputEventV2(V2Ctx& c, const AstNode& step, const AstNode& event,
                            int eventIdx, double beatOffset, double duration)
{
    if (! c.localOutputMode || event.eventType != "local_output"
        || event.outputSocket.isEmpty())
        return;

    ProgramBuilder& pb = *c.pb;
    const juce::String socketName = event.outputSocket;
    const auto signalType = scriptOutputSignalTypeFromEvent(event);
    const auto channelCount = std::max(1, event.stackVoices);
    if (channelCount > 1 && c.minVoices != nullptr)
        *c.minVoices = std::max(*c.minVoices, channelCount);
    const Processor* proc = event.processors.empty() ? nullptr
                                                     : &event.processors.front();
    const Processor* dynamicRepeat =
        dynamicRepeatProcessorOf(event.processors);
    if (dynamicRepeat == nullptr)
        dynamicRepeat = dynamicRepeatProcessorOf(step.processors, event.commaGroup, eventIdx);
    auto emitDynamicRepeat = [&] {
        if (dynamicRepeat == nullptr || dynamicRepeat->args.empty())
            return true;
        if (hasStatefulTrainProcessor(step, event, event.commaGroup, eventIdx)) {
            c.shared->fail("compile-error: dynamic repeat cannot cover "
                           "stateful train articulations yet");
            return false;
        }
        FireExprV2 expr;
        if (!compileFireExprV2(c, dynamicRepeat->args[0], "count", 1.0, expr))
            return false;
        pb.repeatExpr(expr.code, expr.immediates);
        return true;
    };

    if (proc != nullptr && !proc->args.empty()
        && isStreamRouteValue(proc->args.front())) {
        const StepRouteSpan span { beatOffset, duration };
        emitCtrlSignalRouteV2(c, socketName, proc->args.front(), &span,
                              false, 0xffff,
                              localOutputSignalExprDomain(signalType));
        return;
    }

    if (signalType == vm::SignalType::Pitch) {
        if (c.stepOrdinals) c.stepOrdinals->push_back(c.topStepOrdinal);
        pb.step(beatOffset, duration);
        if (!emitDynamicRepeat()) return;
        const auto lane = laneOf(c, socketName);
        const auto value = localOutputPitchSignal(proc);
        for (int channel = 0; channel < channelCount; ++channel)
            pb.localOutput(lane, static_cast<std::uint32_t>(channel),
                           vm::SignalType::Pitch, value);
        return;
    }

    if (signalType == vm::SignalType::Gate) {
        const auto gate = localOutputGateSpec(event, duration, c.bpm);
        if (!gate.emitGate)
            return;
        if (c.stepOrdinals) c.stepOrdinals->push_back(c.topStepOrdinal);
        pb.step(beatOffset, duration);
        if (!emitDynamicRepeat()) return;
        const double gateBeats = gate.beats;
        if (std::isfinite(gateBeats) && gateBeats > 0.0 && gateBeats < duration)
            pb.gateLen((float) gateBeats);
        if (!emitGateTrainPreludeV2(c, step, event, eventIdx, event.commaGroup, duration))
            return;
        if (socketName.equalsIgnoreCase("gate") && channelCount == 1)
            pb.gateOnly(1.0f);
        else {
            const auto lane = laneOf(c, socketName);
            for (int channel = 0; channel < channelCount; ++channel)
                pb.localGateOnly(lane, static_cast<std::uint32_t>(channel), 1.0f);
        }
        return;
    }

    if (signalType == vm::SignalType::Velocity) {
        if (c.stepOrdinals) c.stepOrdinals->push_back(c.topStepOrdinal);
        pb.step(beatOffset, duration);
        if (!emitDynamicRepeat()) return;
        const float value = proc != nullptr && !proc->args.empty()
            ? localOutputVelocityValue(proc->args.front(), 0.7)
            : 0.7f;
        const auto lane = laneOf(c, socketName);
        for (int channel = 0; channel < channelCount; ++channel)
            pb.localOutput(lane, static_cast<std::uint32_t>(channel),
                           vm::SignalType::Velocity, value);
        return;
    }

    if (c.stepOrdinals) c.stepOrdinals->push_back(c.topStepOrdinal);
    const bool procIsGate = proc != nullptr
        && (proc->name.equalsIgnoreCase("gate") || proc->canonicalName.equalsIgnoreCase("gate"));
    const auto gateSpec = procIsGate ? localOutputGateSpec(event, duration, c.bpm)
                                     : LocalOutputGateSpec { true, duration };
    const double stepDur = procIsGate && gateSpec.emitGate ? gateSpec.beats : duration;
    pb.step(beatOffset, stepDur);
    if (!emitDynamicRepeat()) return;

    float value = 0.0f;
    if (proc != nullptr) {
        if (procIsGate)
            value = gateSpec.emitGate ? 1.0f : 0.0f;
        else if (!proc->args.empty())
            value = localOutputValueNorm(c, socketName, proc->args.front(),
                                         socketName.equalsIgnoreCase("velocity") ? 0.7 : 0.0);
        else
            value = localOutputValueNorm(c, socketName, ParseValue{},
                                         socketName.equalsIgnoreCase("velocity") ? 0.7 : 0.0);
    }
    const auto lane = laneOf(c, socketName);
    for (int channel = 0; channel < channelCount; ++channel)
        c.pb->localOutput(lane, static_cast<std::uint32_t>(channel),
                          vm::SignalType::Value, value);
}

// B-293 — articulations and per-note step ops written on a varref/bracket
// postfix process the notes INSIDE ("all variables are processable like any
// other type"): they distribute to the inner triggers through the existing
// globalStepOps fallback the articulation extracts already consult. The
// inner step's own op wins (extract order: step -> event -> globals).
// Conservative whitelist: per-note semantics are unambiguous for these. CON-005
// V2 makes condition/probability parity known-ready, so bracket-level guards
// distribute to the inner triggers through the same globalStepOps fallback.
const std::unordered_set<juce::String>& inheritablePerNoteOps()
{
    static const std::unordered_set<juce::String> s = {
        "ratchet", "flam", "buzz", "bounce", "geiger", "len", "deviate",
        "prob", "probability", "cond",
    };
    return s;
}

std::vector<Processor> collectInheritedPerNoteOps(const V2Ctx& c,
                                                  const AstNode& step,
                                                  const AstNode& event)
{
    std::vector<Processor> merged;
    for (const auto& p : step.processors)
        if (inheritablePerNoteOps().count(p.name)) merged.push_back(p);
    for (const auto& p : event.processors)
        if (inheritablePerNoteOps().count(p.name)) merged.push_back(p);
    if (merged.empty()) return merged;
    if (c.globalStepOps != nullptr)
        for (const auto& p : *c.globalStepOps) merged.push_back(p);
    return merged;
}

// B-297 — param:~gen postfix on a bracket/varref scope routes with THAT
// scope as its clock. Two parse shapes land here: module params in the
// legacy kStandardProcessors list (decay, pan) classify as step-level
// procs instead of route events and were silently dropped; and a route
// written directly on a varref step (\$b cutoff:~random(...)) rides the
// step's processors. Mirrors emitTriggerCtrlRoutesV2's filters; rightmost
// wins per param. Call INSIDE the scope (after scopeStart).
struct ScopeRouteCand {
    juce::String name;
    ParseValue value;
    int ord;
};

// Collect param:~gen candidates covering a bracket/varref scope: the
// scope's own postfix processors (B-297 — kStandardProcessors residue
// classifies module params like decay as step procs) AND sibling update
// events of the same step (the sequence-trailing route shape). Rightmost
// wins per param.
std::vector<ScopeRouteCand> collectScopeRouteCands(const AstNode& step,
                                                   const AstNode& event)
{
    std::vector<ScopeRouteCand> winners;
    std::unordered_map<std::string, size_t> winnerIdx;
    auto consider = [&](const Processor& p) {
        if (p.args.empty()) return;
        const ParseValue& v = p.args[0];
        if (! isStreamRouteValue(v)) return;
        if (scopeOpNames().count(p.name) || nonLockRouteOps().count(p.name)) return;
        const juce::String upper = p.name.toUpperCase();
        if (nonModuleParams().count(upper)) return;
        const int ord = std::max(0, p.sourceOrdinal);
        const std::string key = upper.toStdString();
        const auto it = winnerIdx.find(key);
        if (it == winnerIdx.end()) {
            winnerIdx.emplace(key, winners.size());
            winners.push_back({ upper, v, ord });
        } else if (ord > winners[it->second].ord) {
            winners[it->second] = { upper, v, ord };
        }
    };
    for (const auto& p : step.processors)  consider(p);
    for (const auto& p : event.processors) consider(p);
    for (const auto& evPtr : step.events) {
        if (!evPtr || evPtr.get() == &event) continue;
        if (evPtr->eventType != "update") continue;
        for (const auto& p : evPtr->processors) consider(p);
    }
    return winners;
}

// The ordinal of the scope's repeat op, or -1 if none (B-298 ordering).
int repeatOrdinalOf(const AstNode& step, const AstNode& event)
{
    int ord = -1;
    auto scan = [&](const std::vector<Processor>& procs) {
        for (const auto& p : procs)
            if (p.name == "repeat" && p.sourceOrdinal >= 0
                && (ord < 0 || p.sourceOrdinal < ord))
                ord = p.sourceOrdinal;
    };
    scan(step.processors);
    scan(event.processors);
    return ord;
}

// ═══════════════════════════════════════════════════════════════════════════
// T-491 — ONE parameterized inner-scope emitter.
//
// Two source shapes produce an inner scope — a Sequence event (every
// bracket/tuplet parses as one: parseSeqBody wraps [..] in a Step whose
// event is the Sequence) and a VariableRef — and they mean the same thing
// to the machine: a scope frame over walked content, possibly repeated,
// with scope ops, routes, and inherited per-note ops. Before this, each
// shape had its own near-identical emitter (plus a third for a
// Step-with-steps shape the parser never produces — deleted) and every
// mechanism fix had to be applied per emitter (B-293/294/295 each lived
// in the gap between two of them). The shape adapters below only GATHER
// inputs; everything that EMITS lives here, once. SPEC-008: same-TU
// decomposition, mechanism/shape split.
// ═══════════════════════════════════════════════════════════════════════════
struct ScopeBodySpec {
    const std::vector<AstNodePtr>* content = nullptr;  // steps the body walks
    std::vector<std::pair<juce::String, std::vector<ParseValue>>> scopeOps;
    int    repN         = 1;
    double copyLen      = 0.0;   // one pass through content, emitted beats
    double contentScale = 1.0;   // outerScale multiplier (varref slot ratio)
    const Processor* dynamicRepeat = nullptr;
    std::vector<Processor>      inherited;     // B-293 per-note ops
    std::vector<ScopeRouteCand> perCopy;       // B-298 routes left of repeat
    std::vector<ScopeRouteCand> wholeScope;    // routes covering all copies
};

// B-298 — routes and repeat STACK by written order (§5 one-law): a route
// written LEFT of repeat:N is covered by it — it emits inside the unroll
// body in its own per-copy scope, re-anchoring every copy (the auto is as
// long as one pass). Written right of repeat, the route covers the
// repeated whole (one sweep over all copies). Both partitions register
// their consumed tokens so the trigger-route path doesn't double-emit.
void partitionScopeRoutes(V2Ctx& c, const AstNode& step, const AstNode& event,
                          int repN, ScopeBodySpec& spec)
{
    const int repOrd = repeatOrdinalOf(step, event);
    for (auto& cand : collectScopeRouteCands(step, event)) {
        if (repN > 1 && repOrd >= 0 && cand.ord < repOrd) spec.perCopy.push_back(cand);
        else                                              spec.wholeScope.push_back(cand);
    }
    for (const auto& cand : spec.perCopy)
        c.consumedRouteTokens.insert(cand.name.toStdString() + "|"
                                     + std::to_string(cand.ord));
    for (const auto& cand : spec.wholeScope)
        c.consumedRouteTokens.insert(cand.name.toStdString() + "|"
                                     + std::to_string(cand.ord));
}

void emitScopeBodyV2(V2Ctx& c, double beatOffset, const ScopeBodySpec& spec)
{
    // B-293 — inherited per-note ops distribute to the inner triggers via
    // the globalStepOps fallback the articulation extracts consult; the
    // inner step's own op wins (extract order: step -> event -> globals).
    const std::vector<Processor>* savedGlobals = c.globalStepOps;
    if (! spec.inherited.empty()) c.globalStepOps = &spec.inherited;

    ++c.scopeSerial;
    c.pb->scopeStart(beatOffset,
                     spec.copyLen * spec.repN * dslTimescaleOf(spec.scopeOps, c.bpm));
    if (spec.dynamicRepeat != nullptr && !spec.dynamicRepeat->args.empty()) {
        FireExprV2 expr;
        if (!compileFireExprV2(c, spec.dynamicRepeat->args[0], "count", 1.0, expr)) {
            c.pb->scopeEnd();
            c.globalStepOps = savedGlobals;
            return;
        }
        c.pb->scopeRepeatExpr(expr.code, expr.immediates);
    }
    emitScopeOpsV2(c, spec.scopeOps);
    for (const auto& cand : spec.wholeScope)
        emitCtrlSignalRouteV2(c, cand.name, cand.value);

    const double savedScale = c.outerScale;
    c.outerScale *= spec.contentScale;
    ++c.walkDepth;
    auto body = [&](vm::ProgramBuilder& sub) {
        vm::ProgramBuilder* saved = c.pb;
        c.pb = &sub;
        if (! spec.perCopy.empty()) {
            // Per-copy route scope as the body's FIRST bytes: the unroll
            // shifts this depth-0 scope per copy — each copy re-anchors.
            ++c.scopeSerial;
            sub.scopeStart(0.0, spec.copyLen);
            for (const auto& cand : spec.perCopy)
                emitCtrlSignalRouteV2(c, cand.name, cand.value);
            sub.scopeEnd();
        }
        walkAndEmitV2(*spec.content, c, 0.0);
        c.pb = saved;
    };
    if (spec.repN > 1) c.pb->repeat((uint32_t) spec.repN, spec.copyLen, body);
    else               walkAndEmitV2(*spec.content, c, 0.0);
    --c.walkDepth;
    c.outerScale = savedScale;
    c.pb->scopeEnd();
    c.globalStepOps = savedGlobals;
}

void emitSelectSelectorV2(V2Ctx& c, const AstNode& event, double beatOffset)
{
    if (event.setterProcessor.args.empty()) {
        c.shared->fail("V2_UNSUPPORTED_SELECT: select(...) has no selector");
        return;
    }

    FireExprV2 selector;
    if (!compileFireExprV2(c, event.setterProcessor.args[0], "unit", 0.0, selector))
        return;

    std::vector<uint16_t> branchStepCounts;
    branchStepCounts.reserve(event.options.size());
    for (const auto& option : event.options) {
        if (! option || option->type != "Sequence") {
            c.shared->fail("V2_UNSUPPORTED_SELECT: select(...) options must be sequence material");
            return;
        }
        if (option->steps.empty()) {
            c.shared->fail("V2_UNSUPPORTED_SELECT: select(...) options must contain steps");
            return;
        }
        if (option->steps.size() > 65535u) {
            c.shared->fail("V2_UNSUPPORTED_SELECT: select(...) option has too many steps");
            return;
        }
        branchStepCounts.push_back((uint16_t) option->steps.size());
    }
    if (branchStepCounts.empty() || branchStepCounts.size() > 255u) {
        c.shared->fail("V2_UNSUPPORTED_SELECT: select(...) needs 1-255 options");
        return;
    }

    c.pb->selectDyn(selector.code, selector.immediates, branchStepCounts);

    for (const auto& option : event.options) {
        ScopeBodySpec spec;
        spec.content = &option->steps;
        spec.repN = 1;
        for (const auto& s : option->steps)
            spec.copyLen += s ? effectiveStepSlotBeats(*s, c.bpm) : 1.0;
        emitScopeBodyV2(c, beatOffset, spec);
    }
}


// Sequence written as an event. B-294 — when the line carries a trailing
// generator route, the parser attaches the bracket's postfix processors to
// the bracket EVENT (the route becomes a second event, so the
// Sequence-as-event promotion doesn't fire), so both step and event
// processors feed the scope.
void emitInnerSequenceV2(V2Ctx& c, const AstNode& step, const AstNode& event,
                         int eventIdx, double beatOffset)
{
    const Processor* dynamicRepeat =
        dynamicRepeatProcessorOf(step.processors, event.commaGroup, eventIdx);
    if (dynamicRepeat == nullptr)
        dynamicRepeat = dynamicRepeatProcessorOf(event.processors, -1, -1);
    ScopeBodySpec spec;
    spec.content = &event.steps;
    for (const auto& p : step.processors)
        if (scopeOpNames().count(p.name) && matchesScope(p, eventIdx, event.commaGroup))
            spec.scopeOps.emplace_back(p.name, p.args);
    for (const auto& p : event.processors)
        if (scopeOpNames().count(p.name)) spec.scopeOps.emplace_back(p.name, p.args);
    spec.repN = std::max(getRepeatCount(step.processors),
                         getRepeatCount(event.processors));
    if (dynamicRepeat != nullptr)
        spec.repN = 1;
    for (const auto& s : event.steps)
        spec.copyLen += s ? effectiveStepSlotBeats(*s, c.bpm) : 1.0;
    spec.dynamicRepeat = dynamicRepeat;
    spec.inherited = collectInheritedPerNoteOps(c, step, event);
    partitionScopeRoutes(c, step, event, dynamicRepeat != nullptr ? 2 : spec.repN, spec);
    emitScopeBodyV2(c, beatOffset, spec);
}

// Variable reference ($a as a step). The var's content keeps its authored
// slot ratio: the walk runs under outerScale x durationRatio.
void emitInnerVarRefV2(V2Ctx& c, const AstNode& step, const AstNode& event,
                       double beatOffset, double duration)
{
    const AstNode* varDef = event.resolved;
    if (!varDef || varDef->type != "Sequence") return;
    const Processor* dynamicRepeat =
        dynamicRepeatProcessorOf(step.processors, event.commaGroup, -1);
    if (dynamicRepeat == nullptr)
        dynamicRepeat = dynamicRepeatProcessorOf(event.processors, -1, -1);
    double varLen = 0.0;
    for (const auto& s : varDef->steps)
        varLen += s ? effectiveStepSlotBeats(*s, c.bpm) : 1.0;
    // B-295 — repeat:N on a varref REPEATS the var's contents additively.
    // The parser already widened the step's slot by N; without the unroll
    // that widened slot flowed into durationRatio and time-stretched the
    // var (the arp played once, N x slower). Per-copy slot = duration/N.
    const int repN = std::max(getRepeatCount(step.processors),
                              getRepeatCount(event.processors));
    const double slotPerCopy = dynamicRepeat != nullptr
        ? duration : duration / (double) repN;
    const double durationRatio = (varLen > 0.0) ? slotPerCopy / varLen : 1.0;

    ScopeBodySpec spec;
    spec.content = &varDef->steps;
    for (const auto& p : step.processors)
        if (scopeOpNames().count(p.name)) spec.scopeOps.emplace_back(p.name, p.args);
    for (const auto& p : event.processors)
        if (scopeOpNames().count(p.name)) spec.scopeOps.emplace_back(p.name, p.args);
    spec.repN         = dynamicRepeat != nullptr ? 1 : repN;
    spec.copyLen      = varLen * durationRatio;
    spec.contentScale = durationRatio;
    spec.dynamicRepeat = dynamicRepeat;
    spec.inherited = collectInheritedPerNoteOps(c, step, event);
    partitionScopeRoutes(c, step, event, dynamicRepeat != nullptr ? 2 : repN, spec);
    emitScopeBodyV2(c, beatOffset, spec);
}

void emitStepEventBodyV2(V2Ctx& c, const AstNode& step, double beatOffset, double duration)
{
    // B-289 — scale as a bare trigger postfix now rides the B-284 implicit
    // machine frame (collectImplicitScopeOps no longer filters it); the
    // per-event compile-time quantize that lived here is deleted. sort/grid
    // order a step LIST — over a single plain step they are trivially
    // applied (a 1-element reorder), nothing to emit.

    std::vector<std::unique_ptr<AstNode>> mergedOwned;
    std::vector<const AstNode*>           mergedStepEvents;
    buildMergedEventList(step.events, mergedOwned, mergedStepEvents);

    // STRUM / ARP expansions (compile-time; arp gen-speed is B-186 → error).
    // B-277: the comma anchor is THIS module's first trigger — a global
    // first-trigger cg let one module's op leak into (or fail to bind in)
    // other projections of a multi-module step.
    int firstTriggerCg = 0;
    bool cgFound = false;
    for (const auto& ev : step.events)
        if (ev && ev->eventType == "trigger" && ev->module == c.target) {
            firstTriggerCg = ev->commaGroup;
            cgFound = true;
            break;
        }
    if (! cgFound)
        for (const auto& ev : step.events)
            if (ev && ev->eventType == "trigger") { firstTriggerCg = ev->commaGroup; break; }

    auto triggers = collectTriggerEvents(mergedStepEvents);

    // T-492 §5.8 — a BernoulliSelector postfixes the nearest preceding trigger
    // in its comma group: that trigger is the selector's "original" (empty
    // options replay it), NOT an additional parallel event. The pick replaces
    // the step's content, so the original never plain-emits — and a covering
    // arp/strum must NOT arpeggiate it (it distributes into the options below).
    // Computed up here so coveredBy can exclude consumed originals.
    std::unordered_map<const AstNode*, const AstNode*> bernOriginalOf;
    std::unordered_set<const AstNode*> consumedByBernoulli;
    for (size_t ei = 0; ei < mergedStepEvents.size(); ++ei) {
        const AstNode* sel = mergedStepEvents[ei];
        if (sel == nullptr || sel->type != "BernoulliSelector") continue;
        for (int pi = (int) ei - 1; pi >= 0; --pi) {
            const AstNode* prev = mergedStepEvents[pi];
            if (prev == nullptr || prev->eventType != "trigger") continue;
            if (prev->commaGroup != sel->commaGroup) continue;
            if (consumedByBernoulli.count(prev)) continue;
            bernOriginalOf[sel] = prev;
            consumedByBernoulli.insert(prev);
            break;
        }
    }

    // B-274 (s494): an op written between its trigger and a following call
    // ('#saw.c3.min7 strum:0.13 #saw.CUTOFF:...') parses onto the EVENT's
    // processors, not the step's. Ops are additive — a lock call must never
    // strip a preceding articulation — so extraction falls back to the
    // trigger events' own processors. B-277: the owning event scopes the
    // articulation to itself (its module + comma group) in every projection.
    const auto eventLevelProc = [&] (std::initializer_list<const char*> names,
                                     const AstNode** ownerOut)
        -> const Processor* {
        for (const AstNode* ev : triggers) {
            if (ev == nullptr || ev->eventType != "trigger") continue;
            if (const auto* p = extractProcessor(ev->processors, names, -1, -1)) {
                if (ownerOut != nullptr) *ownerOut = ev;
                return p;
            }
        }
        return nullptr;
    };

    const AstNode* arpOwner = nullptr;
    const Processor* arpProc = extractProcessor(step.processors, {"arp"}, firstTriggerCg, -1);
    if (! arpProc) arpProc = eventLevelProc({ "arp" }, &arpOwner);
    // The arp SPEED expression: bare ~random/~bernoulli draw per loop iteration,
    // ~auto is a per-hit ramp (F-071 Phase 1 / T-511), and signal chains sample
    // a transformed speed value. Single source of truth — arpSpeedExprV2 owns
    // the routing law.
    const ParseValue* arpSpeedExpr = arpSpeedExprV2(arpProc, c);
    if (c.shared->error.isNotEmpty()) return;

    // B-277 — which triggers an articulation COVERS. Event-bound op → that
    // event's (module, comma group). Step-level op → the parser's positional
    // scope: afterEventCount ("events written before me") + comma group,
    // honored via matchesScope — '#saw.c3.min7 arp:up #tone.a2' covers the
    // saw only, with or without a comma.
    const auto coveredBy = [&] (const Processor* p, const AstNode* ownerEv) {
        std::vector<const AstNode*> out;
        for (size_t ei = 0; ei < mergedStepEvents.size(); ++ei) {
            const AstNode* ev = mergedStepEvents[ei];
            if (ev == nullptr || ev->eventType != "trigger") continue;
            if (consumedByBernoulli.count(ev)) continue;   // B-314 — selector owns it
            if (ownerEv != nullptr) {
                if (ev->module == ownerEv->module
                    && ev->commaGroup == ownerEv->commaGroup)
                    out.push_back(ev);
            } else if (p == nullptr || matchesScope(*p, (int) ei, ev->commaGroup)) {
                out.push_back(ev);
            }
        }
        return out;
    };

    // Plain emission for the triggers the articulation does NOT cover (the
    // rest of the step still plays normally — ops are additive).
    const auto emitRemainder = [&] (const std::vector<const AstNode*>& covered) {
        for (size_t ei = 0; ei < mergedStepEvents.size(); ++ei) {
            const AstNode& event = *mergedStepEvents[ei];
            if (event.eventType != "trigger" || event.module != c.target) continue;
            if (std::find(covered.begin(), covered.end(), &event) != covered.end())
                continue;
            const int cg = event.commaGroup;
            emitTriggerCondProbV2(c, step, event, (int) ei, cg);
            const double onsetShift = extractOnset(event.processors, step.processors, c.bpm);
            if (emitBareBernoulliV2(c, step, event, (int) ei, cg,
                                    beatOffset + onsetShift, duration))
                continue;
            emitTriggerBodyV2(c, step, event, (int) ei, cg,
                              beatOffset + onsetShift, duration);
        }
    };

    // B-279 — right-dominant ordering for routes vs articulations (SYNTAX_V1
    // §5 + §5.3 "notes sample the op, never retrigger it"). A generator
    // route written BEFORE the articulation binds to the STEP: emit once
    // with the full step span (one scope anchor — ~random draws once, ~auto
    // ramps once across all sub-steps). Routes written AFTER ride each
    // sub-step (own scope per note — per-note draws, opted in by ordering).
    // Partition by parse position: index order within the op's own vector;
    // event procs precede a step-level op; step procs follow an event-level
    // op.
    const auto preArticulationRoutes = [&] (const Processor* artProc,
                                            const AstNode* ownerEv) {
        std::vector<std::pair<juce::String, ParseValue>> pre;
        // "Before" = smaller token ordinal than the articulation op. The
        // parser stamps sourceOrdinal at construction precisely because the
        // step/event processor split loses relative written order.
        const int artOrd = artProc->sourceOrdinal;
        const auto scan = [&] (const std::vector<Processor>& procs) {
            for (const Processor& p : procs) {
                if (artOrd >= 0 && p.sourceOrdinal >= 0
                    && p.sourceOrdinal >= artOrd)
                    continue;
                if (p.args.empty()) continue;
                const ParseValue& v = p.args[0];
                if (! isStreamRouteValue(v)) continue;
                const juce::String upper = p.name.toUpperCase();
                if (nonModuleParams().count(upper)) continue;
                if (nonLockRouteOps().count(p.name)) continue;
                pre.push_back({ upper, v });
            }
        };
        if (ownerEv != nullptr) {
            scan(ownerEv->processors);
        } else {
            for (const AstNode* ev : triggers)
                if (ev != nullptr && ev->module == c.target)
                    scan(ev->processors);
            scan(step.processors);
        }
        return pre;
    };
    const auto emitPreRoutes = [&] (const Processor* artProc, const AstNode* ownerEv,
                                    std::unordered_set<juce::String>& preNames) {
        const StepRouteSpan fullSpan { beatOffset, duration };
        for (const auto& [pname, value] : preArticulationRoutes(artProc, ownerEv)) {
            if (preNames.count(pname)) continue;
            preNames.insert(pname);
            emitCtrlSignalRouteV2(c, pname, value, &fullSpan);
        }
    };

    const AstNode* strumOwner = nullptr;
    const Processor* strumP = extractProcessor(step.processors, {"strum"}, firstTriggerCg);
    if (! strumP) strumP = eventLevelProc({ "strum" }, &strumOwner);
    if (strumP) {
        const auto arg = strumP->args.empty() ? ParseValue{} : strumP->args[0];
        const double spacing = resolveValue(arg, "time", c.bpm, 0.05);
        const auto covered = coveredBy(strumP, strumOwner);
        const bool targetsUs = std::any_of(covered.begin(), covered.end(),
            [&] (const AstNode* ev) { return ev != nullptr && ev->module == c.target; });
        if (targetsUs) {
            std::unordered_set<juce::String> preNames;
            emitPreRoutes(strumP, strumOwner, preNames);
            c.suppressRouteParams = preNames.empty() ? nullptr : &preNames;
            const bool emitted = emitArpOrStrumV2(c, step, covered, false, {}, spacing,
                                                  beatOffset, duration);
            c.suppressRouteParams = nullptr;
            if (emitted) {
                emitRemainder(covered);
                return;
            }
        }
    }
    auto arp = extractArp(step.processors, duration, firstTriggerCg, -1, c.bpm);
    if (! arp.present)   // B-274 — same event-level fallback as strum above
        for (const AstNode* ev : triggers) {
            if (ev == nullptr || ev->eventType != "trigger") continue;
            arp = extractArp(ev->processors, duration, -1, -1, c.bpm);
            if (arp.present) { arpOwner = ev; break; }
        }
    if (! arp.present && c.sequenceArp != nullptr) {
        // B-314 — an arp on the $out sequence ('[...] arp(up,1/16)') covers
        // every step's content; lowest priority, behaves as a step-level arp.
        arpProc     = c.sequenceArp;
        arpOwner    = nullptr;
        arp         = arpSpecOf(*c.sequenceArp, duration, c.bpm);
        arpSpeedExpr = arpSpeedExprV2(c.sequenceArp, c);
        if (c.shared->error.isNotEmpty()) return;
    }
    if (arp.present) {
        const auto covered = coveredBy(arpProc, arpOwner);
        const bool targetsUs = std::any_of(covered.begin(), covered.end(),
            [&] (const AstNode* ev) { return ev != nullptr && ev->module == c.target; });
        if (targetsUs) {
            std::unordered_set<juce::String> preNames;
            emitPreRoutes(arpProc, arpOwner, preNames);
            c.suppressRouteParams = preNames.empty() ? nullptr : &preNames;
            const bool emitted = emitArpOrStrumV2(c, step, covered, true, arp.type,
                                                  arp.speed, beatOffset, duration,
                                                  arpSpeedExpr);
            c.suppressRouteParams = nullptr;
            if (emitted) {
                emitRemainder(covered);
                return;
            }
        }
    }

    // B-314 — a step-level (not event-bound) arp present here covered no plain
    // trigger (the step's content is a bernoulli selector, which coveredBy
    // can't collect). It still binds the selector: thread it into the option
    // emission so each chord option arpeggiates.
    const Processor* selectorArp =
        (arp.present && arpOwner == nullptr) ? arpProc : nullptr;

    const int plainRepeatN = getRepeatCount(step.processors);
    const bool hasDynamicStepRepeat =
        dynamicRepeatProcessorOf(step.processors) != nullptr;
    const bool hasSeqOrVar = std::any_of(mergedStepEvents.begin(), mergedStepEvents.end(),
        [](const AstNode* e){ return e && (e->type == "Sequence" || e->type == "VariableRef"); });
    const bool hasPlainRepeat = !hasDynamicStepRepeat
                             && plainRepeatN > 1 && step.steps.empty() && !hasSeqOrVar;

    auto emitEvents = [&](vm::ProgramBuilder* pb, double off, double stepDur) {
        vm::ProgramBuilder* saved = c.pb;
        if (pb) c.pb = pb;
        for (size_t ei = 0; ei < mergedStepEvents.size(); ++ei) {
            const AstNode& event = *mergedStepEvents[ei];
            const int eventIdx = (int) ei;
            if (c.localOutputMode && event.eventType == "local_output") {
                emitLocalOutputEventV2(c, step, event, eventIdx, off, stepDur);
                continue;
            }
            if (event.type == "Sequence") {
                emitInnerSequenceV2(c, step, event, eventIdx, off);
                continue;
            }
            if (event.type == "VariableRef") {
                emitInnerVarRefV2(c, step, event, off, duration);
                continue;
            }
            if (event.type == "BernoulliSelector") {
                const auto origIt = bernOriginalOf.find(&event);
                const AstNode* original =
                    origIt != bernOriginalOf.end() ? origIt->second : nullptr;
                bool targetsUs = original != nullptr && original->module == c.target;
                for (const auto& opt : event.options)
                    if (opt && opt->module == c.target) { targetsUs = true; break; }
                if (targetsUs) {
                    auto cond = extractCond(step.processors, event.commaGroup, eventIdx);
                    emitCondV2(c, cond);
                    emitBernoulliOptionsV2(c, step, event, eventIdx, off, stepDur,
                                           original, selectorArp);
                }
                continue;
            }
            if (event.type == "MarkovSelector") {
                auto cond = extractCond(step.processors, event.commaGroup, eventIdx);
                emitCondV2(c, cond);
                if (event.eventType == "markov_gate")
                    emitMarkovGateSelectorV2(c, event, off, stepDur);
                else
                    emitMarkovSelectorV2(c, step, event, eventIdx, off, stepDur);
                continue;
            }
            if (event.type == "SelectSelector") {
                auto cond = extractCond(step.processors, event.commaGroup, eventIdx);
                emitCondV2(c, cond);
                emitSelectSelectorV2(c, event, off);
                continue;
            }
            if (event.eventType == "rest") {
                if (event.module == c.target) {
                    c.pb->step(off, stepDur);
                    c.pb->rest();
                }
                continue;
            }
            if (event.eventType == "hold") continue;
            if (event.module != c.target) continue;
            if (consumedByBernoulli.count(&event)) continue;   // T-492

            const int cg = event.commaGroup;
            emitTriggerCondProbV2(c, step, event, eventIdx, cg);

            const double onsetShift = extractOnset(event.processors, step.processors, c.bpm);
            if (emitBareBernoulliV2(c, step, event, eventIdx, cg, off + onsetShift, stepDur))
                continue;
            const Processor* dynamicRepeat =
                dynamicRepeatProcessorOf(event.processors);
            if (dynamicRepeat == nullptr)
                dynamicRepeat = dynamicRepeatProcessorOf(step.processors, cg, eventIdx);
            emitTriggerBodyV2(c, step, event, eventIdx, cg,
                              off + onsetShift, stepDur,
                              std::numeric_limits<double>::quiet_NaN(),
                              dynamicRepeat);
        }
        c.pb = saved;
    };

    // B-314 — a step-level arp here ('[...] arp' reparses to a Step carrying the
    // arp over a nested Sequence event) covers everything inside, including a
    // nested sequence's selector. Propagate it into the inner walk; restored
    // after so siblings without the arp stay clean.
    const Processor* savedSeqArp = c.sequenceArp;
    if (selectorArp != nullptr) c.sequenceArp = selectorArp;

    if (hasPlainRepeat) {
        const double subDur = duration / plainRepeatN;
        c.pb->repeat((uint32_t) plainRepeatN, subDur,
                     [&](vm::ProgramBuilder& sub) { emitEvents(&sub, beatOffset, subDur); });
    } else {
        emitEvents(nullptr, beatOffset, duration);
    }

    c.sequenceArp = savedSeqArp;
}

bool walkAndEmitV2(const std::vector<AstNodePtr>& steps, V2Ctx& c, double parentOffset)
{
    double beatCursor = 0.0;
    int stepIdx = -1;
    for (const auto& stepPtr : steps) {
        ++stepIdx;
        if (c.walkDepth == 0) c.topStepOrdinal = stepIdx;   // SF-059 FOLLOW
        if (!stepPtr) {
            recordTopLevelStepSpan(c, parentOffset + beatCursor, 1.0 * c.outerScale);
            beatCursor += 1.0;
            continue;
        }
        const AstNode& step = *stepPtr;
        const double beatOffset = parentOffset + beatCursor;
        const double scale      = resolveStepUnitScale(step, c.bpm)
                                * applyStepBpmStretch(step, c.bpm);
        const double duration   = step.duration * scale * c.outerScale;
        const double slotBeats  = effectiveStepSlotBeats(step, c.bpm) * c.outerScale;

        recordTopLevelStepSpan(c, beatOffset, slotBeats);

        if (!step.events.empty())
            emitStepEventBodyV2(c, step, beatOffset, duration);
        // B-299 — timescale scales the SLOT: the next sibling starts where
        // this step's (compressed/stretched) scope actually ends.
        beatCursor += slotBeats;
    }
    return true;
}

std::vector<vm::Program::TimelineStep>
expandTimelineRepeats(const std::vector<vm::Program::TimelineStep>& base,
                      double loopLength, int repeatCount)
{
    if (repeatCount <= 1 || loopLength <= 0.0 || base.empty())
        return base;

    std::vector<vm::Program::TimelineStep> out;
    out.reserve(base.size() * (size_t) repeatCount);
    for (int r = 0; r < repeatCount; ++r)
        for (auto step : base)
        {
            step.offsetBeats += (float) (r * loopLength);
            out.push_back(step);
        }
    return out;
}

std::vector<vm::Program::TimelineStep>
scaleTimeline(const std::vector<vm::Program::TimelineStep>& base,
              double factor)
{
    if (factor <= 0.0 || factor == 1.0 || base.empty())
        return base;

    std::vector<vm::Program::TimelineStep> out;
    out.reserve(base.size());
    for (auto step : base)
    {
        step.offsetBeats   = (float) ((double) step.offsetBeats * factor);
        step.durationBeats = (float) ((double) step.durationBeats * factor);
        out.push_back(step);
    }
    return out;
}

struct IndependentLaneBranch {
    const AstNode* branch = nullptr;
    double spanBeats = 0.0;
    int repeats = 1;
};

struct IndependentLanePlan {
    bool enabled = false;
    double loopLengthBeats = 0.0;
    std::vector<IndependentLaneBranch> branches;
};

bool containsTarget(const AstNode& n, const juce::String& target,
                    bool includeLocalOutputs,
                    std::set<const AstNode*>& seen)
{
    if (! seen.insert(&n).second)
        return false;
    if (n.module == target)
        return true;
    if (includeLocalOutputs && n.eventType == "local_output")
        return true;
    for (const auto& ev : n.events)
        if (ev && containsTarget(*ev, target, includeLocalOutputs, seen))
            return true;
    for (const auto& step : n.steps)
        if (step && containsTarget(*step, target, includeLocalOutputs, seen))
            return true;
    for (const auto& opt : n.options)
        if (opt && containsTarget(*opt, target, includeLocalOutputs, seen))
            return true;
    if (n.resolved != nullptr && containsTarget(*n.resolved, target,
                                                includeLocalOutputs, seen))
        return true;
    return false;
}

bool containsTarget(const AstNode& n, const juce::String& target,
                    bool includeLocalOutputs = false)
{
    std::set<const AstNode*> seen;
    return containsTarget(n, target, includeLocalOutputs, seen);
}

std::vector<const AstNode*> topLevelLaneBranches(const AstNode& sequence)
{
    std::vector<const AstNode*> branches;
    if (sequence.steps.size() != 1 || ! sequence.steps.front())
        return branches;

    const auto& rootStep = *sequence.steps.front();
    if (rootStep.events.size() < 2)
        return branches;

    for (const auto& ev : rootStep.events) {
        if (! ev || ev->type != "Sequence" || ev->steps.empty())
            return {};
        branches.push_back(ev.get());
    }
    return branches;
}

double branchSpanBeats(const AstNode& branch, double bpm)
{
    if (branch.type == "Sequence") {
        const double span = computeLoopLength(branch.steps, bpm);
        const int repeats = std::max(1, getRepeatCount(branch.processors));
        return span * repeats;
    }
    return 0.0;
}

bool toTicks(double beats, int64_t& ticks)
{
    static constexpr double ticksPerBeat = 960.0;
    const double raw = beats * ticksPerBeat;
    const auto rounded = (int64_t) std::llround(raw);
    if (beats <= 0.0 || std::abs(raw - (double) rounded) > 1.0e-6)
        return false;
    ticks = rounded;
    return ticks > 0;
}

double commonLanePeriod(std::vector<double> spans)
{
    if (spans.empty())
        return 0.0;
    if (spans.size() == 1)
        return spans.front();

    int64_t periodTicks = 0;
    bool tickExact = true;
    for (double span : spans) {
        int64_t ticks = 0;
        if (! toTicks(span, ticks)) {
            tickExact = false;
            break;
        }
        periodTicks = periodTicks == 0 ? ticks : std::lcm(periodTicks, ticks);
    }
    if (tickExact && periodTicks > 0)
        return (double) periodTicks / 960.0;

    return *std::max_element(spans.begin(), spans.end());
}

int branchRepeatCount(double commonPeriod, double span)
{
    int64_t commonTicks = 0;
    int64_t spanTicks = 0;
    if (toTicks(commonPeriod, commonTicks) && toTicks(span, spanTicks)
        && spanTicks > 0 && commonTicks % spanTicks == 0)
        return (int) std::max<int64_t>(1, commonTicks / spanTicks);

    const double ratio = span > 0.0 ? commonPeriod / span : 1.0;
    const int rounded = (int) std::llround(ratio);
    if (rounded > 1 && std::abs(ratio - (double) rounded) < 1.0e-6)
        return rounded;
    return 1;
}

IndependentLanePlan makeIndependentLanePlan(const AstNode& sequence,
                                            const juce::String& target,
                                            double bpm,
                                            bool includeLocalOutputs = false)
{
    IndependentLanePlan plan;
    const auto branches = topLevelLaneBranches(sequence);
    if (branches.empty())
        return plan;

    std::vector<double> spans;
    for (const auto* branch : branches) {
        if (branch == nullptr
            || ! containsTarget(*branch, target, includeLocalOutputs))
            continue;
        const double span = branchSpanBeats(*branch, bpm);
        if (span <= 0.0)
            continue;
        IndependentLaneBranch lane;
        lane.branch = branch;
        lane.spanBeats = span;
        plan.branches.push_back(lane);
        spans.push_back(span);
    }

    if (plan.branches.empty())
        return plan;

    plan.enabled = true;
    plan.loopLengthBeats = commonLanePeriod(spans);
    for (auto& lane : plan.branches)
        lane.repeats = branchRepeatCount(plan.loopLengthBeats, lane.spanBeats);
    return plan;
}

void emitIndependentLanePlan(const IndependentLanePlan& plan, V2Ctx& ctx)
{
    for (const auto& lane : plan.branches) {
        if (lane.branch == nullptr)
            continue;
        for (int r = 0; r < lane.repeats; ++r)
            walkAndEmitV2(lane.branch->steps, ctx, (double) r * lane.spanBeats);
    }
}

// Recursive module-name discovery (insertion order).
void discoverModules(const std::vector<AstNodePtr>& steps,
                     std::vector<juce::String>& order,
                     std::unordered_set<juce::String>& seen)
{
    auto addName = [&](const juce::String& m) {
        if (m.isEmpty() || seen.count(m)) return;
        seen.insert(m);
        order.push_back(m);
    };
    for (const auto& sp : steps) {
        if (!sp) continue;
        for (const auto& evPtr : sp->events) {
            if (!evPtr) continue;
            const AstNode& ev = *evPtr;
            addName(ev.module);
            if (ev.type == "Sequence" && !ev.steps.empty())
                discoverModules(ev.steps, order, seen);
            if (ev.type == "VariableRef" && ev.resolved
                && ev.resolved->type == "Sequence")
                discoverModules(ev.resolved->steps, order, seen);
            for (const auto& opt : ev.options) {
                if (! opt) continue;
                addName(opt->module);
                if (opt->type == "Sequence")
                    discoverModules(opt->steps, order, seen);
            }
        }
    }
}

bool containsDynamicRepeatArg(const AstNode& n, std::set<const AstNode*>& seen)
{
    if (! seen.insert(&n).second)
        return false;
    const auto scanProcessors = [] (const std::vector<Processor>& procs) {
        for (const auto& p : procs)
            if (p.name == "repeat" && !p.args.empty() && isSignalExprArg(p.args[0]))
                return true;
        return false;
    };
    if (scanProcessors(n.processors))
        return true;
    for (const auto& ev : n.events)
        if (ev && containsDynamicRepeatArg(*ev, seen))
            return true;
    for (const auto& step : n.steps)
        if (step && containsDynamicRepeatArg(*step, seen))
            return true;
    for (const auto& opt : n.options)
        if (opt && containsDynamicRepeatArg(*opt, seen))
            return true;
    if (n.resolved != nullptr && containsDynamicRepeatArg(*n.resolved, seen))
        return true;
    return false;
}

bool containsDynamicRepeatArg(const AstNode& n)
{
    std::set<const AstNode*> seen;
    return containsDynamicRepeatArg(n, seen);
}

} // namespace v2

// T-491 — byte-identity corpus tap. With CURLOP_BYTECORPUS_OUT=<abs path>
// set, every compileV2 call appends its full output (per-module program
// bytes + lane bindings + step ordinals) to the file. Test-suite call order
// is deterministic, so two runs diff line-for-line: the refactor acceptance
// "byte-identical bytecode across the golden corpus" is `diff before after`.
void dumpBytecorpus(const CompileResultV2& out)
{
    static const char* path = std::getenv("CURLOP_BYTECORPUS_OUT");
    if (path == nullptr) return;
    static int ordinal = 0;
    juce::String s;
    s << "### compile " << ++ordinal
      << " modules=" << (int) out.modulePrograms.size()
      << " loop=" << juce::String(out.loopLength, 6);
    if (out.compileError.isNotEmpty()) s << " ERROR=" << out.compileError;
    s << "\n";
    for (const auto& mp : out.modulePrograms) {
        s << mp.dslName << " bytes=" << (int) mp.program.code.size() << " lanes=";
        for (size_t i = 0; i < mp.laneParams.size(); ++i)
            if (i >= mp.laneInternal.size() || mp.laneInternal[i] == 0)
                s << mp.laneParams[i] << ",";
        s << " stepOrds=";
        for (int so : mp.stepSourceOrdinals) s << so << ",";
        s << "\n"
          << juce::String::toHexString(mp.program.code.data(),
                                       (int) mp.program.code.size(), 0)
          << "\n";
    }
    juce::File(juce::String(path)).appendText(s);
}

struct BytecorpusDumpGuard {
    const CompileResultV2& r;
    ~BytecorpusDumpGuard() { dumpBytecorpus(r); }
};

} // namespace (anonymous)

// ═══════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════════════════
// F-066 Phase 5 (T-462) — compileV2: per-module machine-ISA projection.
// ═══════════════════════════════════════════════════════════════════════════
CompileResultV2 compileV2(const ScriptAst& ast, const CompileOptions& opts)
{
    CompileResultV2 out;
    BytecorpusDumpGuard corpusGuard { out };   // T-491 — no-op unless env set

    const AstNode* seq = findOutSequenceNode(ast);
    if (!seq) return out;   // no $out → no programs (silence, by contract)
    const auto& sequence = *seq;

    double bpm = (opts.bpmOverride > 0) ? opts.bpmOverride : 120.0;
    uint32_t seed = opts.seed != 0.0 ? (uint32_t) opts.seed : 42u;
    auto globals = extractGlobalSetters(ast, opts, bpm, seed);

    std::vector<std::pair<juce::String, std::vector<ParseValue>>> seqScopeOps = globals.scopeOps;
    if (v2::dynamicRepeatProcessorOf(sequence.processors) != nullptr) {
        out.compileError = "compile-error: dynamic repeat on root material needs "
                           "dynamic loop-length support; plain-step dynamic repeat "
                           "is supported";
        return out;
    }
    for (const auto& p : sequence.processors)
        // T-571b — `bpm` is in scopeOpNames for NESTED brackets, but on the ROOT
        // sequence it is the global tempo (handled by extractGlobalSetters), not a
        // scope op. Skip it here so it never emits a root SCOPE_BPM.
        if (scopeOpNames().count(p.name) && p.name != "bpm")
            seqScopeOps.emplace_back(p.name, p.args);

    // T-569 — a top-level `timescale:~lfo(...)` rides the per-script clock as a
    // tempo multiplier (SPEC-018 §4.6), NOT the compile-time loop-length path.
    // Project it to a TimescaleModulator, strip it from the emitted scope ops
    // (the live srcBpm multiplier replaces the static frame.timescale), and
    // leave the loop length at base (timescaleFactor stays 1.0 below).
    {
        juce::String tsModErr;
        const int r = detectTimescaleModulator(seqScopeOps, bpm, out.tsMod, tsModErr);
        if (r == -1) { out.compileError = tsModErr; return out; }
        if (r == 1) {
            std::vector<std::pair<juce::String, std::vector<ParseValue>>> kept;
            kept.reserve(seqScopeOps.size());
            for (auto& e : seqScopeOps)
                if (e.first != "timescale") kept.push_back(std::move(e));
            seqScopeOps.swap(kept);
        }
    }

    const double timescaleFactor = resolveTimescaleFactor(seqScopeOps, sequence, bpm);

    const double loopLength = computeLoopLength(sequence.steps, bpm);
    const int repeatCount = getRepeatCount(sequence.processors);
    const double totalLoopLength = loopLength * timescaleFactor * repeatCount;
    const double globalLockSourceStepDuration = sequence.steps.empty()
        ? 1.0
        : effectiveStepSlotBeats(*sequence.steps.front(), bpm) * timescaleFactor;

    // Module discovery: graph seed first (stable indices), then any modules
    // the script references that the seed missed.
    ModuleRegistry modules(opts.moduleIndicesSeed);
    std::vector<juce::String> targets;
    std::unordered_set<juce::String> seen;
    v2::discoverModules(sequence.steps, targets, seen);
    for (const auto* gEv : std::vector<const AstNode*>(globals.moduleParams))
        if (gEv && gEv->module.isNotEmpty() && !seen.count(gEv->module)) {
            seen.insert(gEv->module);
            targets.push_back(gEv->module);
        }

    v2::SharedV2 shared;
    shared.globalSeed = seed;

    for (const auto& target : targets) {
        ModuleProgramV2 mp;
        mp.dslName   = target;
        mp.moduleIdx = modules.indexOf(target);

        const auto lanePlan = v2::makeIndependentLanePlan(sequence, target, bpm);
        const double targetLoopLength = lanePlan.enabled ? lanePlan.loopLengthBeats
                                                         : loopLength;
        const double targetTotalLoopLength = targetLoopLength * timescaleFactor * repeatCount;

        vm::ProgramBuilder pb;
        pb.loop(targetTotalLoopLength);

        std::unordered_set<std::string> ctrlEmitted;
        std::set<std::string> perVoiceSet;   // SF-052 — {}-stacked base params
        std::vector<vm::Program::TimelineStep> stepTimeline;
        v2::V2Ctx ctx;
        ctx.target             = target;
        ctx.pb                 = &pb;
        ctx.shared             = &shared;
        ctx.laneParams         = &mp.laneParams;
        ctx.laneInternal       = &mp.laneInternal;
        ctx.perVoiceParams     = &perVoiceSet;
        ctx.minVoices          = &mp.minVoices;
        ctx.schemas            = &opts.schemas;
        ctx.bpm                = bpm;
        ctx.globalStepOps      = globals.stepOps.empty() ? nullptr : &globals.stepOps;
        ctx.globalModuleParams = globals.moduleParams.empty() ? nullptr : &globals.moduleParams;
        ctx.ctrlEmitted        = &ctrlEmitted;
        ctx.scopeSerial        = 0;
        ctx.stepOrdinals       = &mp.stepSourceOrdinals;
        ctx.stepTimeline       = &stepTimeline;
        ctx.inputSocketNames   = &opts.inputSocketNames;

        std::set<const AstNode*> targetSearch;
        if (!v2::containsTarget(sequence, target, false, targetSearch))
            emitPersistentGlobalLocksV2(ctx, globalLockSourceStepDuration,
                                        targetTotalLoopLength);

        const bool hasSeqScope = !seqScopeOps.empty();
        if (hasSeqScope) {
            ++ctx.scopeSerial;
            pb.scopeStart(0.0, targetTotalLoopLength);
            v2::emitScopeOpsV2(ctx, seqScopeOps);
        }

        if (repeatCount > 1) {
            pb.repeat((uint32_t) repeatCount, targetLoopLength,
                      [&](vm::ProgramBuilder& sub) {
                          vm::ProgramBuilder* saved = ctx.pb;
                          ctx.pb = &sub;
                          if (lanePlan.enabled)
                              v2::emitIndependentLanePlan(lanePlan, ctx);
                          else
                              v2::walkAndEmitV2(sequence.steps, ctx, 0.0);
                          ctx.pb = saved;
                      });
        } else {
            if (lanePlan.enabled)
                v2::emitIndependentLanePlan(lanePlan, ctx);
            else
                v2::walkAndEmitV2(sequence.steps, ctx, 0.0);
        }

        if (hasSeqScope) pb.scopeEnd();
        pb.halt();

        if (shared.error.isNotEmpty()) {
            out.compileError = shared.error;
            out.modulePrograms.clear();
            return out;
        }

        mp.program = pb.build();
        const auto scaledTimeline = v2::scaleTimeline(stepTimeline, timescaleFactor);
        mp.program.timeline = v2::expandTimelineRepeats(scaledTimeline,
                                                        targetLoopLength * timescaleFactor,
                                                        repeatCount);
        mp.perVoiceParams.assign(perVoiceSet.begin(), perVoiceSet.end());   // SF-052
        out.modulePrograms.push_back(std::move(mp));
    }

    if (opts.localOutputTarget.isNotEmpty()) {
        const bool pitchDefaults = !opts.localOutputTarget.containsIgnoreCase("trigger");
        std::vector<ScriptOutputSocketDecl> decls;
        std::set<std::string> seen;
        addScriptOutputSocket(decls, seen, "gate", 1.0f, false, vm::SignalType::Gate);
        if (pitchDefaults) {
            addScriptOutputSocket(decls, seen, "pitch", 0.0f, false, vm::SignalType::Pitch);
            addScriptOutputSocket(decls, seen, "velocity", 0.7f, false, vm::SignalType::Velocity);
        }
        for (const auto& stmt : ast.statements)
            collectScriptOutputSocketsFromNode(stmt.get(), decls, seen);

        ModuleProgramV2 mp;
        mp.dslName = opts.localOutputTarget;
        mp.moduleIdx = modules.indexOf(opts.localOutputTarget);
        mp.localOutputOwner = true;
        mp.localOutputs.reserve(decls.size());
        for (const auto& d : decls) {
            ModuleProgramV2::LocalOutputDecl od;
            od.name = juce::String(d.name);
            od.type = d.type;
            od.min = d.name == "pitch" ? 0.0f : 0.0f;
            od.max = d.name == "pitch" ? 0.0f : 1.0f;
            od.def = d.value;
            mp.localOutputs.push_back(std::move(od));
        }

        const auto lanePlan = v2::makeIndependentLanePlan(sequence,
                                                          opts.localOutputTarget,
                                                          bpm,
                                                          true);
        const double targetLoopLength = lanePlan.enabled ? lanePlan.loopLengthBeats
                                                         : loopLength;
        const double targetTotalLoopLength = targetLoopLength * timescaleFactor * repeatCount;

        vm::ProgramBuilder pb;
        pb.loop(targetTotalLoopLength);
        std::unordered_set<std::string> ctrlEmitted;
        std::set<std::string> perVoiceSet;
        std::vector<vm::Program::TimelineStep> stepTimeline;
        v2::V2Ctx ctx;
        ctx.target             = opts.localOutputTarget;
        ctx.pb                 = &pb;
        ctx.shared             = &shared;
        ctx.laneParams         = &mp.laneParams;
        ctx.laneInternal       = &mp.laneInternal;
        ctx.perVoiceParams     = &perVoiceSet;
        ctx.schemas            = &opts.schemas;
        ctx.bpm                = bpm;
        ctx.globalStepOps      = globals.stepOps.empty() ? nullptr : &globals.stepOps;
        ctx.globalModuleParams = nullptr;
        ctx.ctrlEmitted        = &ctrlEmitted;
        ctx.scopeSerial        = 0;
        ctx.stepOrdinals       = &mp.stepSourceOrdinals;
        ctx.stepTimeline       = &stepTimeline;
        ctx.localOutputMode    = true;
        ctx.inputSocketNames   = &opts.inputSocketNames;
        ctx.minVoices          = &mp.minVoices;

        const bool hasSeqScope = !seqScopeOps.empty();
        if (hasSeqScope) {
            ++ctx.scopeSerial;
            pb.scopeStart(0.0, targetTotalLoopLength);
            v2::emitScopeOpsV2(ctx, seqScopeOps);
        }

        if (repeatCount > 1) {
            pb.repeat((uint32_t) repeatCount, targetLoopLength,
                      [&](vm::ProgramBuilder& sub) {
                          vm::ProgramBuilder* saved = ctx.pb;
                          ctx.pb = &sub;
                          if (lanePlan.enabled)
                              v2::emitIndependentLanePlan(lanePlan, ctx);
                          else
                              v2::walkAndEmitV2(sequence.steps, ctx, 0.0);
                          ctx.pb = saved;
                      });
        } else {
            if (lanePlan.enabled)
                v2::emitIndependentLanePlan(lanePlan, ctx);
            else
                v2::walkAndEmitV2(sequence.steps, ctx, 0.0);
        }

        if (hasSeqScope) pb.scopeEnd();
        pb.halt();

        if (shared.error.isNotEmpty()) {
            out.compileError = shared.error;
            out.modulePrograms.clear();
            return out;
        }

        mp.program = pb.build();
        const auto scaledTimeline = v2::scaleTimeline(stepTimeline, timescaleFactor);
        mp.program.timeline = v2::expandTimelineRepeats(scaledTimeline,
                                                        targetLoopLength * timescaleFactor,
                                                        repeatCount);
        out.modulePrograms.push_back(std::move(mp));
    }

    out.moduleIndices = modules.take();
    out.bpm           = bpm;
    out.bpmExplicit   = globals.bpmExplicit;
    out.bpmMod        = globals.bpmMod;                 // T-519
    out.loopLength    = totalLoopLength;
    out.globalSeed    = seed;
    if (out.compileError.isEmpty() && globals.bpmModError.isNotEmpty())
        out.compileError = globals.bpmModError;          // T-519 — loud, per §5.12
    return out;
}

} // namespace curlop::script
