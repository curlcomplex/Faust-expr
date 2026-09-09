// ═══════════════════════════════════════════════════════════════════════════
// ScriptLanguage.cpp — JSON serializer for SCRIPT_LANGUAGE_SCHEMA payload.
//
// Complex tables (generators, articulation ops, OP_ARGS, sugar→canonical)
// live inline here as literal JSON concatenation rather than constexpr
// structs — avoids nested-array constexpr pain while the schema is read-only
// at startup. When 0.D-2/0.D-3 need structured access, these graduate to
// runtime structs.
//
// Cut 0.D-1. [-r DUMB-VIEW-INV-1]
// ═══════════════════════════════════════════════════════════════════════════

#include "control/surfaces/script/ScriptLanguage.h"
#include "modules/contract/ModuleTypes.h"     // curlop::escapeJsonString

#include <initializer_list>
#include <sstream>
#include <string>

namespace curlop::script {

namespace {

// Helper: emit ["a","b","c"] from a std::array of string_view.
template <typename ArrayT>
std::string stringArrayJson(const ArrayT& arr) {
    std::string out = "[";
    bool first = true;
    for (const auto& sv : arr) {
        if (!first) out += ",";
        first = false;
        out += "\"" + curlop::escapeJsonString(std::string(sv)) + "\"";
    }
    out += "]";
    return out;
}

// Helper: emit [3,4,2,...] from a std::array of int.
template <typename ArrayT>
std::string intArrayJson(const ArrayT& arr) {
    std::string out = "[";
    bool first = true;
    for (int v : arr) {
        if (!first) out += ",";
        first = false;
        out += std::to_string(v);
    }
    out += "]";
    return out;
}

std::string charArrayJson(const std::array<char, 8>& arr) {
    std::string out = "[";
    bool first = true;
    for (char c : arr) {
        if (!first) out += ",";
        first = false;
        std::string s(1, c);
        out += "\"" + curlop::escapeJsonString(s) + "\"";
    }
    out += "]";
    return out;
}

std::string symbolsJson() {
    // Matches JS SYMBOLS shape — {"module":"#","variable":"$",...}
    return std::string("{")
        + "\"module\":\"#\","
        + "\"variable\":\"$\","
        + "\"override\":\"@\","
        + "\"generator\":\"~\","
        + "\"hold\":\"_\","
        + "\"rest\":\"-\","
        + "\"advance\":\"/\","
        + "\"assign\":\"=\","
        + "\"dot\":\".\","
        + "\"colon\":\":\","
        + "\"comma\":\",\","
        + "\"bang\":\"!\","
        + "\"plus\":\"+\""
        + "}";
}

std::string sugarToCanonicalJson() {
    // Per CurlopLanguage.js:175-194. 16 entries.
    return std::string("{")
        + "\"vel\":\"velocity\","
        + "\"prob\":\"probability\","
        + "\"step\":\"step\","
        + "\"len\":\"len\","
        + "\"poly\":\"poly\","
        + "\"stack\":\"stack\","
        + "\"ratchet\":\"ratchet\","
        + "\"bpm\":\"bpm\","
        + "\"pan\":\"pan\","
        + "\"decay\":\"decay\","
        + "\"seed\":\"seed\","
        + "\"cond\":\"cond\","
        + "\"cent\":\"cent\","
        + "\"repeat\":\"repeat\","
        + "\"porta\":\"glide\","
        + "\"portamento\":\"glide\""
        + "}";
}

// Helper to emit a single generator-like spec entry.
std::string getGeneratorEntry(const char* name, const char* desc,
                           std::initializer_list<const char*> args) {
    std::string out = "{\"name\":\"";
    out += name;
    out += "\",\"description\":\"";
    out += curlop::escapeJsonString(desc);
    out += "\",\"args\":[";
    bool first = true;
    for (const char* a : args) {
        if (!first) out += ",";
        first = false;
        out += "\"";
        out += curlop::escapeJsonString(a);
        out += "\"";
    }
    out += "]}";
    return out;
}

std::string generatorsJson() {
    // Per CurlopLanguage.js:225-241. 12 entries.
    std::string out = "[";
    out += getGeneratorEntry("lfo",       "Repeating waveform",              {"shape","rate","min","max"});
    out += ",";
    out += getGeneratorEntry("ad",        "Attack-Decay envelope",           {"attack","decay","min","max"});
    out += ",";
    out += getGeneratorEntry("adsr",      "ADSR envelope",                   {"attack","decay","sustain","release","min","max"});
    out += ",";
    out += getGeneratorEntry("auto",      "Multi-point automation curve",    {"...points"});
    out += ",";
    out += getGeneratorEntry("random",    "Random value per step",           {"min","max","slew"});
    out += ",";
    out += getGeneratorEntry("deviate",   "Random variation from value",     {"amount"});
    out += ",";
    out += getGeneratorEntry("keytrack",  "Key tracking control op",         {"..."});
    out += ",";
    out += getGeneratorEntry("midi",      "MIDI CC hardware control",        {"cc","min","max"});
    out += ",";
    out += getGeneratorEntry("bernoulli", "Weighted random selector",        {"weight","...values"});
    out += ",";
    out += getGeneratorEntry("bounce",    "Bounce physics on any param",     {"gravity","min","max"});
    out += ",";
    out += getGeneratorEntry("buzz",      "Buzz roll values on any param",   {"pressure","min","max"});
    out += ",";
    out += getGeneratorEntry("ratchet",   "Ratchet decay on any param",      {"count","max","min"});
    out += "]";
    return out;
}

std::string articulationOpsJson() {
    // Per CurlopLanguage.js:248-255. 6 entries.
    std::string out = "[";
    out += getGeneratorEntry("ratchet", "Rapid re-trigger within step",    {"count","decay"});
    out += ",";
    out += getGeneratorEntry("flam",    "Grace note before hit",           {"time","amp"});
    out += ",";
    out += getGeneratorEntry("strum",   "Strum chord notes",               {"time","direction"});
    out += ",";
    out += getGeneratorEntry("geiger",  "Poisson-distributed retriggers",  {"density"});
    out += ",";
    out += getGeneratorEntry("buzz",    "Snare buzz roll",                 {"pressure"});
    out += ",";
    out += getGeneratorEntry("bounce",  "Bouncing ball retrigger",         {"gravity"});
    out += "]";
    return out;
}

// Helper: emit one OP_ARGS op entry: "name":{"args":[{...},{...}]}
struct ArgField {
    const char* name;
    const char* type;
    const char* defaultVal;
    std::initializer_list<const char*> rec;      // empty = omit
    std::initializer_list<const char*> options;  // empty = omit
    // T-551 — authoritative widget bounds (the TRUE full domain a drag may
    // reach). hasBounds=true → emit min/max/log/unit. Defaulted so existing
    // 5-field arg initializers still compile; only bounded args set them.
    bool        hasBounds = false;
    double      minV      = 0.0;
    double      maxV      = 0.0;
    bool        logV      = false;
    const char* unit      = "";
};

std::string argFieldJson(const ArgField& a) {
    std::string out = "{\"name\":\"";
    out += curlop::escapeJsonString(a.name);
    out += "\",\"type\":\"";
    out += a.type;
    out += "\",\"default\":\"";
    out += curlop::escapeJsonString(a.defaultVal);
    out += "\"";
    if (a.rec.size() > 0) {
        out += ",\"rec\":[";
        bool first = true;
        for (const char* r : a.rec) {
            if (!first) out += ",";
            first = false;
            out += "\"";
            out += curlop::escapeJsonString(r);
            out += "\"";
        }
        out += "]";
    }
    if (a.options.size() > 0) {
        out += ",\"options\":[";
        bool first = true;
        for (const char* o : a.options) {
            if (!first) out += ",";
            first = false;
            out += "\"";
            out += curlop::escapeJsonString(o);
            out += "\"";
        }
        out += "]";
    }
    if (a.hasBounds) {
        // T-551 — authoritative drag domain. SchemaRegistry keys "has bounds"
        // off the presence of "max".
        out += ",\"min\":";  out += std::to_string(a.minV);
        out += ",\"max\":";  out += std::to_string(a.maxV);
        out += ",\"log\":";  out += (a.logV ? "true" : "false");
        out += ",\"unit\":\""; out += curlop::escapeJsonString(a.unit); out += "\"";
    }
    out += "}";
    return out;
}

std::string getOpEntry(const char* opName, std::initializer_list<ArgField> args) {
    std::string out = "\"";
    out += opName;
    out += "\":{\"args\":[";
    bool first = true;
    for (const auto& a : args) {
        if (!first) out += ",";
        first = false;
        out += argFieldJson(a);
    }
    out += "]}";
    return out;
}

std::string opArgsJson() {
    // Per CurlopLanguage.js:292-354. 35 ops.
    std::string out = "{";

    // T-551 — authoritative widget bounds (TRUE full domain) relocated from the
    // WidgetSpec.cpp placeholder table. velocity is the normalised VEL implicit
    // param: 0..1, default 1.0 (CURLOP-SCRIPTING_SYNTAX_V1.md §VEL line 1897 —
    // "VEL… Default 1.0"; every spec example is 0..1). The earlier 0..127/"100"
    // here was MIDI-modelled drift; corrected s508 to match canon.
    out += getOpEntry("velocity",    {{"value",     "number", "1.0",   {"0.25","0.5","0.75","1.0"},                              {}, true, 0, 1, false, ""}});
    out += "," + getOpEntry("probability", {{"percent",   "number", "50",    {"10","25","50","75","90"},                           {}, true, 0, 100, false, "%"}});
    out += "," + getOpEntry("step",        {{"position",  "number", "0.5",   {"0","0.25","0.5","0.75","1"},                        {}, true, 0, 1, false, ""}});
    out += "," + getOpEntry("len",         {{"duration",  "number", "0.5",   {"0.25","0.5","0.75","1","2"},                        {}, true, 0, 64, false, ""}});
    out += "," + getOpEntry("poly",        {{"voices",    "number", "4",     {"2","3","4","6","8"},                                {}, true, 1, 256, false, ""}});
    out += "," + getOpEntry("stack",       {{"count",     "number", "2",     {"2","3","4"},                                        {}, true, 1, 64, false, ""}});
    out += "," + getOpEntry("bpm",         {{"tempo",     "number", "120",   {"80","100","120","140","160","180"},                 {}, true, 1, 100000, true, "BPM"}});
    out += "," + getOpEntry("pan",         {{"position",  "number", "0.5",   {"0","0.25","0.5","0.75","1"},                        {}, true, 0, 1, false, ""}});
    out += "," + getOpEntry("decay",       {{"ms",        "number", "500",   {"100","250","500","1000","2000"},                    {}, true, 0, 60000, true, "ms"}});
    out += "," + getOpEntry("seed",        {{"value",     "number", "42",    {"1","42","100","999"},                               {}, true, 0, 999999, false, ""}});
    out += "," + getOpEntry("cent",        {{"cents",     "number", "50",    {"-50","-25","25","50","100"},                        {}, true, -9600, 9600, false, "c"}});
    out += "," + getOpEntry("repeat",      {{"count",     "number", "4",     {"2","3","4","8","16"},                               {}}});
    out += "," + getOpEntry("onset",       {{"offset",    "number", "0.1",   {"0","0.1","0.25","0.5"},                             {}, true, 0, 1, false, ""}});
    out += "," + getOpEntry("transpose",   {{"semitones", "number", "12",    {"-12","-7","-5","5","7","12"},                       {}, true, -60, 60, false, "st"}});
    out += "," + getOpEntry("octave",      {{"octaves",   "number", "1",     {"-2","-1","1","2","3"},                              {}, true, -10, 10, false, ""}});
    out += "," + getOpEntry("invert",      {{"axis",      "number", "60",    {"48","60","72"},                                     {}}});
    out += "," + getOpEntry("reverse",     {});
    out += "," + getOpEntry("rotate",      {{"steps",     "number", "1",     {"-2","-1","1","2","3"},                              {}, true, -64, 64, false, ""}});
    out += "," + getOpEntry("shuffle",     {});
    out += "," + getOpEntry("sort",        {{"direction", "select", "up",    {},                                                   {"up","down"}}});
    out += "," + getOpEntry("timescale",   {{"factor",    "number", "2",     {"0.25","0.5","1","2","4"},                           {}, true, 0.03125, 32, true, "x"}});
    out += "," + getOpEntry("grid",        {{"division",  "string", "1/16",  {"1/4","1/8","1/16","1/32"},                          {}},
                                         {"strength",  "number", "0.5",   {"0.25","0.5","0.75","1"},                            {}}});
    out += "," + getOpEntry("ratchet",     {{"count",     "number", "4",     {"2","3","4","6","8"},                                {}, true, 1, 20000, true, ""},
                                         {"decay",     "number", "0.5",   {"-0.5","0","0.3","0.5","0.8"},                       {}}});
    out += "," + getOpEntry("flam",        {{"time",      "string", "20ms",  {"5ms","10ms","20ms","30ms","50ms"},                  {}},
                                         {"amp",       "number", "0.6",   {"0.3","0.5","0.6","0.8","1.0"},                      {}}});
    out += "," + getOpEntry("strum",       {{"time",      "string", "50ms",  {"10ms","20ms","30ms","50ms","100ms"},                {}},
                                         {"direction", "select", "up",    {},                                                   {"up","down"}}});
    out += "," + getOpEntry("geiger",      {{"density",   "number", "0.5",   {"0.1","0.3","0.5","0.7","0.9"},                      {}, true, 0, 1, false, ""}});
    out += "," + getOpEntry("bounce",      {{"gravity",   "number", "0.5",   {"0.1","0.3","0.5","0.7","0.9"},                      {}, true, 0, 1, false, ""}});
    out += "," + getOpEntry("buzz",        {{"pressure",  "number", "0.5",   {"0.1","0.3","0.5","0.7","0.9"},                      {}, true, 0, 1, false, ""}});
    out += "," + getOpEntry("arp",         {{"type",      "select", "up",    {},                                                   {"up","down","updown","pingpong","converge","diverge","spiral","random"}},
                                         {"speed",     "string", "1/16",  {"1/8","1/16","1/32","2","4"},                        {}}});
    out += "," + getOpEntry("groove",      {{"template",  "select", "swing", {},                                                   {"swing","shuffle","push","quint","sept","elastic","samba","fractal"}},
                                         {"amount",    "number", "50",    {"20","40","50","70","90"},                           {}},
                                         {"gainLow",   "number", "0.3",   {"0.1","0.2","0.3","0.5"},                            {}},
                                         {"gainHigh",  "number", "1.0",   {"0.7","0.8","0.9","1.0"},                            {}}});
    out += "," + getOpEntry("bernoulli",   {{"weight",    "number", "0.5",   {"0.2","0.3","0.5","0.7","0.8"},                      {}, true, 0, 1, false, ""},
                                         {"optionA",   "string", "optionA", {},                                                  {}},
                                         {"optionB",   "string", "optionB", {},                                                  {}}});
    out += "," + getOpEntry("deviate",     {{"amount",    "number", "0.05",  {"0.01","0.05","0.1","0.2","0.5"},                    {}, true, 0, 1, false, ""}});
    out += "," + getOpEntry("glide",       {{"time",      "string", "100ms", {"10ms","50ms","100ms","200ms","500ms"},              {}}});
    out += "," + getOpEntry("cond",        {{"condition", "select", "first", {},                                                   {"first","!first","even","odd","prime","fib","mod","every","once","after","previous","!previous","silence","held","changed"}}});
    out += "," + getOpEntry("scale",       {{"root",      "string", "C",     {"C","D","E","F","G","A","B"},                        {}},
                                         {"scale",     "string", "minor", {"major","minor","dorian","phrygian","lydian","mixolydian","blues","chromatic"}, {}}});

    out += "}";
    return out;
}

std::string faustKeywordsJson() {
    static constexpr std::array<std::string_view, 28> keywords = {
        "process", "mgroup", "vgroup", "hgroup", "tgroup", "import", "declare",
        "with", "letrec", "environment", "library", "component", "ffunction",
        "fconstant", "fvariable", "route", "waveform", "soundfile", "seq", "par",
        "sum", "prod", "int", "float", "min", "max", "case", "mem"
    };
    return stringArrayJson(keywords);
}

std::string faustLibrariesJson() {
    // Versioned native-owned catalogue used by every Web editor. Entries cover
    // the stdfaust namespaces and calls exercised by CURLOP's bundled module
    // library; the Web layer never carries a divergent keyword/function table.
    return R"json({
        "ba":[{"name":"db2linear","signature":"ba.db2linear(db)"},{"name":"tau2pole","signature":"ba.tau2pole(tau)"}],
        "co":[{"name":"compressor_stereo","signature":"co.compressor_stereo(ratio, threshold, attack, release)"},{"name":"limiter_1176_R4_stereo","signature":"co.limiter_1176_R4_stereo(inputGain, ratio, attack, release)"}],
        "de":[{"name":"delay","signature":"de.delay(maxDelay, delay)"},{"name":"fdelay","signature":"de.fdelay(maxDelay, delay)"}],
        "ef":[{"name":"gate_stereo","signature":"ef.gate_stereo(threshold, attack, hold, release)"}],
        "en":[{"name":"ad","signature":"en.ad(attack, decay, gate)"},{"name":"adsr","signature":"en.adsr(attack, decay, sustain, release, gate)"},{"name":"ar","signature":"en.ar(attack, release, gate)"}],
        "fi":[{"name":"high_shelf","signature":"fi.high_shelf(gain, cutoff)"},{"name":"highpass","signature":"fi.highpass(order, cutoff)"},{"name":"low_shelf","signature":"fi.low_shelf(gain, cutoff)"},{"name":"lowpass","signature":"fi.lowpass(order, cutoff)"},{"name":"peak_eq_cq","signature":"fi.peak_eq_cq(gain, frequency, q)"},{"name":"resonbp","signature":"fi.resonbp(cutoff, q, gain)"},{"name":"resonhp","signature":"fi.resonhp(cutoff, q, gain)"},{"name":"resonlp","signature":"fi.resonlp(cutoff, q, gain)"}],
        "ma":[{"name":"PI","signature":"ma.PI"},{"name":"SR","signature":"ma.SR"},{"name":"tanh","signature":"ma.tanh(x)"}],
        "no":[{"name":"noise","signature":"no.noise"},{"name":"pink_noise","signature":"no.pink_noise"}],
        "os":[{"name":"osc","signature":"os.osc(freq)"},{"name":"phasor","signature":"os.phasor(freq)"},{"name":"sawtooth","signature":"os.sawtooth(freq)"},{"name":"sinwaveform","signature":"os.sinwaveform"},{"name":"square","signature":"os.square(freq)"},{"name":"triangle","signature":"os.triangle(freq)"}],
        "pf":[{"name":"phaser2_stereo","signature":"pf.phaser2_stereo(notchCount, speed, ratio, depth, feedback)"}],
        "pm":[{"name":"churchBell","signature":"pm.churchBell(strike, freq)"}],
        "re":[{"name":"mono_freeverb","signature":"re.mono_freeverb(fb1, fb2, damp, spread)"},{"name":"stereo_freeverb","signature":"re.stereo_freeverb(fb1, fb2, damp, spread)"},{"name":"zita_rev1_stereo","signature":"re.zita_rev1_stereo(rdel, f1, f2, t60dc, t60m, fsmax)"}],
        "si":[{"name":"bus","signature":"si.bus(channels)"},{"name":"smooth","signature":"si.smooth(tau)"},{"name":"smoothAndH","signature":"si.smoothAndH(tau)"}],
        "ve":[{"name":"moog_vcf","signature":"ve.moog_vcf(resonance, cutoff)"}]
    })json";
}

} // anonymous namespace

std::string buildScriptLanguageSchemaJson() {
    std::string out;
    out.reserve(8192);
    out += "\"symbols\":";            out += symbolsJson();
    out += ",\"triggerCharacters\":"; out += charArrayJson(kTriggerCharacters);
    out += ",\"standardProcessors\":"; out += stringArrayJson(kStandardProcessors);
    out += ",\"sugarToCanonical\":";  out += sugarToCanonicalJson();
    out += ",\"generators\":";        out += generatorsJson();
    out += ",\"articulationOps\":";   out += articulationOpsJson();
    out += ",\"condValues\":";        out += stringArrayJson(kCondValues);
    out += ",\"condArgValues\":";     out += stringArrayJson(kCondArgValues);
    out += ",\"noArgOps\":";          out += stringArrayJson(kNoArgOps);
    out += ",\"opArgs\":";            out += opArgsJson();
    out += ",\"curveTypes\":";        out += stringArrayJson(kCurveTypes);
    out += ",\"outputName\":\"";      out += curlop::escapeJsonString(std::string(kOutputName)); out += "\"";
    out += ",\"noteNames\":";         out += stringArrayJson(kNoteNames);
    out += ",\"octavePriority\":";    out += intArrayJson(kOctavePriority);
    out += ",\"chordNames\":";        out += stringArrayJson(kChordNames);
    out += ",\"faustSchemaVersion\":\"curlop-stdfaust-2026.07\"";
    out += ",\"faustKeywords\":";      out += faustKeywordsJson();
    out += ",\"faustLibraries\":";     out += faustLibrariesJson();
    return out;
}

} // namespace curlop::script
