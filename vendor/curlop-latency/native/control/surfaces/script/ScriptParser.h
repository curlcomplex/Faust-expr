#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// ScriptParser.h — C++ DSL script parser.
// Cut 0.D-2a: full tokenizer + minimal parser (Script/Assignment/Sequence).
// Cut 0.D-2b will complete parser body (bernoulli, voice stacks, chord, freq
// notes, nested tuplets, modifier chains, processors, control ops, etc.).
// Token set mirrors src/sequencer/CurlopLanguage.js `T`.
// [-r DUMB-VIEW-INV-1]
// ═══════════════════════════════════════════════════════════════════════════

#include <juce_core/juce_core.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace curlop::script {

enum class TokenType {
    NEWLINE,
    SEMICOLON,
    EOF_,

    LBRACKET, RBRACKET,
    LPAREN,   RPAREN,
    LBRACE,   RBRACE,

    AT, COLON, DOT, SLASH, COMMA,
    UNDERSCORE, HYPHEN, ASSIGN,
    BANG, TILDE, PLUS, PERCENT, DOLLAR, HASH, GT,

    NOTE,    // c3, d#4, gb2
    FREQ,    // 440hz, 1khz
    CENTS,   // +10c, -5c, 23c
    NUMBER,  // 80, 0.5
    CHORD,   // (deprecated — kept for enum symmetry; tokenizer emits IDENT)
    IDENT,   // kick, decay, my_pattern

    UNKNOWN,
};

struct Token {
    TokenType   type = TokenType::UNKNOWN;
    juce::String value;
    int         line = 0;
    int         col  = 0;
};

enum class Severity { Error, Warning };

struct ScriptDiagnostic {
    Severity     severity = Severity::Error;
    juce::String message;
    int          line = 0;
    int          col  = 0;
    juce::String token;
    juce::String expected;
    juce::String suggestion;
};

// ── AST — rich structs (Cut 0.D-2b). Fat-struct Option A (Spike S1). ───────
// All node kinds share a single AstNode shape; the `type` tag discriminates.
// Unused fields stay at default value. ParseValue is a tagged union for
// processor / generator args. See CUT-0D2b-RESEARCH.md S1/S2.

struct AstNode;
using AstNodePtr = std::unique_ptr<AstNode>;

struct GeneratorNode;
struct SignalChainNode;
struct CondNode;

struct PitchChain {
    juce::String note;    // empty = absent
    juce::String freq;
    juce::String chord;
    juce::String cents;
    // B-274 — explicit voicing list (absolute MIDI semitones). Never set by
    // the parser; compile-time pitch transforms (quantizeStepPitches) write
    // it so a per-voicing rewrite survives to NOTE_CHORD emission instead of
    // collapsing to a single root freq. resolvePitch returns it verbatim.
    std::vector<double> explicitVoicings;
    bool isEmpty() const
    {
        return note.isEmpty() && freq.isEmpty() && chord.isEmpty()
            && cents.isEmpty() && explicitVoicings.empty();
    }
};

struct ParseValue {
    enum class Kind { Null, Number, String, UnitNumber, Generator, SignalChain, PolyList, Cond, NegatedLoops, Rest };
    Kind         kind = Kind::Null;
    double       number = 0.0;
    juce::String str;
    juce::String unit;        // "ms" | "s" | "%"
    bool         offset = false;   // prefix-+ marker
    std::shared_ptr<GeneratorNode> gen;
    std::shared_ptr<SignalChainNode> signalChain;
    std::vector<ParseValue>         list;
    std::shared_ptr<CondNode>      cond;
    std::vector<double>            negLoops;
};

struct Processor {
    juce::String                                   name;
    juce::String                                   canonicalName;
    std::vector<ParseValue>                        args;
    std::vector<std::pair<juce::String, ParseValue>> named;
    bool                                           persistent = false;
    bool                                           isDotParam = false;
    bool                                           isVoiceStack = false;
    int                                            commaGroup = 0;
    int                                            afterEventCount = -1; // -1 = untagged
    // B-279 — token index where this op starts; total source order across
    // step/event processor vectors (right-dominant ordering needs it).
    int                                            sourceOrdinal = -1;
};

struct GeneratorNode {
    juce::String                                   generatorType; // lfo|ad|adsr|random|deviate|keytrack|glide|accum|midi
    std::vector<ParseValue>                        args;
    std::vector<std::pair<juce::String, ParseValue>> named;
    bool                                           isOffset = false;  // %~ prefix
    int                                            line = 0;
    int                                            col  = 0;
};

struct SignalChainOp {
    juce::String            name;
    std::vector<ParseValue> args;
};

struct SignalChainNode {
    std::shared_ptr<GeneratorNode> source;
    std::vector<SignalChainOp>     ops;
};

struct CondNode {
    juce::String        condType;  // first|!first|even|odd|prime|fib|silence|held|changed|
                                   // previous|!previous|mod|every|once|after|loops|!loops|expr
    std::vector<double> loops;
    int                 cycle = 0;
    ParseValue          expr;      // cond(expr): fire-time truthy signal expression
};

// Rich AstNode — covers all Parser.js node shapes. `type` is the tag.
struct AstNode {
    juce::String            type;       // Assignment|Sequence|Step|Event|VariableRef|
                                        // PersistentSetter|BernoulliSelector
    juce::String            name;       // Assignment/VariableRef var name
    juce::String            scopeType;  // Sequence: "parallel"|"tuplet"
    juce::String            eventType;  // Event: "trigger"|"rest"|"hold"|"update"
    juce::String            module;     // Event: module name (empty = bare)
    juce::String            outputSocket; // Event: local script/sequencer output socket
    PitchChain              pitch;      // Event: pitch chain
    std::vector<Processor>  processors;
    std::vector<AstNodePtr> steps;      // Sequence body
    std::vector<AstNodePtr> events;     // Step contents
    std::vector<AstNodePtr> options;    // BernoulliSelector options
    double                  weight = 0.0; // BernoulliSelector weight
    AstNodePtr              value;      // Assignment RHS
    AstNodePtr              setterEvent;      // PersistentSetter (event form)
    Processor               setterProcessor;  // PersistentSetter (processor form)
    bool                    isSetterProcessor = false;
    int                     commaGroup = 0;
    int                     line = 0;
    int                     col  = 0;
    juce::String            stateKey; // Stable identity for cloned stateful V2 events.
    AstNode*                resolved = nullptr; // VariableRef: non-owning ptr into Assignment's value subtree
    int                     stackVoices = 1; // V2 stack(n): repeated trigger voices; 1 = ordinary event

    // Cut 0.D-3e₁: per-step timing, populated by resolveTimings after parse().
    double                  duration    = 1.0; // beats within containing Sequence
    double                  beat_offset = 0.0; // offset within containing Sequence
    ParseValue              stepUnitArg;       // raw unit-arg (3e₂); default Kind::Null
};

struct ScriptAst {
    std::vector<AstNodePtr>       statements;
    std::vector<juce::String>     variableNames;   // sorted at end of parse
    std::vector<juce::String>     errors;
    std::vector<ScriptDiagnostic> diagnostics;
    juce::String                  shape;
};

struct ParseOptions {
    std::unordered_set<juce::String> moduleRegistry; // empty this cut (lock-order — Spike S6)
    std::unordered_set<juce::String> chordRegistry;  // seeded from kChordNames
};

// Never throws. All errors collected in result.
ScriptAst parse(const juce::String& source, const ParseOptions& opts = {});

// Statement-shape fingerprint. Matches JS `computeJsAstShape(jsResult)` format:
//   "<nStmts>:<stmtTypesCommaJoined>:<nVars>:<sortedVarNamesCommaJoined>"
juce::String computeShapeFingerprint(const ScriptAst& ast);

// JSON serializer — consumed by SCRIPT_DIAGNOSTICS wire emit.
juce::String buildScriptDiagnosticsJson(int                                   clipId,
                                        const std::vector<ScriptDiagnostic>&  diags,
                                        const juce::String&                   astShape);

// Deep clone ScriptAst (required because ScriptAst holds std::vector<AstNodePtr>
// which is move-only). Used by ClipStateContainer::getClipScriptAst to hand out
// a self-contained copy with no reliance on container lock lifetime. Cut 0.D-3a.
ScriptAst copyAst(const ScriptAst& src);

// Cut 0.D-3e₁: resolve per-step timing across every Sequence. Populates
// AstNode::duration + beat_offset + stepUnitArg on every Step node reachable
// from ast.statements. Called by parse() after diagnostics merge, before
// shape fingerprint. Safe to call independently in tests. Mirrors
// Parser.js:1859 resolveTimings.
void resolveTimings(ScriptAst& ast);

// V2 syntax cut B follow-on (T-253, stub): reserved-name registry.
// Returns true if `name` collides with any structural keyword the DSL relies
// on for grammar disambiguation. Three bands covered:
//   1. Load-bearing keywords:           `set` (cut A persistent-override marker)
//   2. Control-op names (V2 §1.7):      `lfo`, `ad`, `adsr`, `auto`, `random`,
//                                       `deviate`, `keytrack`, `bernoulli`,
//                                       `shepard`, `accum`, `glide`, `midi`
//   3. Sequencer op names:              `reverse`, `shuffle`, `transpose`,
//                                       `invert`, `rotate`, `prob`, `len`,
//                                       `onset`, `step`, `groove`, `timescale`,
//                                       `sort`, `grid`, `octave`, `cond`,
//                                       `repeat`, `ratchet`, `flam`, `bounce`,
//                                       `buzz`, `geiger`, `strum`
//   4. Future-reserved (V2 cut D):      `x`, `X`, `out`, `rest`, `hold`
//
// STUB STATUS: this helper is exposed but NOT yet wired into declaration sites.
// Cut C ($-removal) will call it from the variable-declaration path to reject
// reserved-name collisions before they reach the parser's disambiguation logic.
// The native script editor + autocomplete may also call it to block trigger
// suggestions for reserved tokens.
bool isReservedName(const juce::String& name);

} // namespace curlop::script
