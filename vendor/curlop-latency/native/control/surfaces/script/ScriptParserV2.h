#pragma once

#include "control/surfaces/script/ScriptCompiler.h"
#include <juce_core/juce_core.h>
#include <memory>
#include <vector>

namespace curlop::script::v2 {

struct Diagnostic {
    juce::String severity;
    juce::String code;
    juce::String message;
    int line = 0;
    int col = 0;
};

enum class MaterialKind { Sequence, Ref };
enum class StatementKind { Unknown, Definition, Function, Route, SocketExport, SocketRoute, SessionTarget };
enum class StepItemKind {
    Atom,
    Note,
    PolyphonicChord,
    Rest,
    Hold,
    Receiver,
    GroupedReceiver,
    Tuplet,
    Markov,
    Stack,
    Preset,
    Select
};
enum class SocketSelectorKind { Stream, DerivedStream };
enum class PostfixKind { StructuralOp, GroupReceiver, InvalidBareSignalGenerator };
enum class MaterialFlowJoin { Chain, Break };

struct SignalExpr {
    juce::String source;
    std::vector<juce::String> chain;
    std::vector<juce::String> inputReads;

    bool isSimpleValue() const noexcept
    {
        return chain.size() <= 1
            && ! source.containsChar('(')
            && ! source.containsChar(',')
            && ! source.containsChar('+')
            && ! source.containsChar('[')
            && ! source.containsChar(']')
            && ! source.containsChar('{')
            && ! source.containsChar('}');
    }
};

struct ReceiverCall {
    juce::String target;
    bool foreign = false;
    SignalExpr expr;
};

struct GroupedReceiver {
    std::vector<juce::String> targets;
    SignalExpr expr;
};

struct Material;

struct MaterialFlowSegment {
    MaterialFlowJoin join = MaterialFlowJoin::Chain;
    std::shared_ptr<Material> material;
};

struct StepItem {
    StepItemKind kind = StepItemKind::Atom;
    juce::String value;
    ReceiverCall receiver;
    GroupedReceiver grouped;
    std::shared_ptr<Material> nested;
    std::vector<std::shared_ptr<Material>> blockOptions;
};

struct Step {
    std::vector<StepItem> items;
};

struct PostfixOp {
    PostfixKind kind = PostfixKind::StructuralOp;
    juce::String name;
    SignalExpr expr;
};

struct Material {
    MaterialKind kind = MaterialKind::Sequence;
    bool implicit = false;
    bool explicitMaterialFlow = false;
    bool explicitPerItemGroupReceivers = false;
    juce::String refName;
    std::vector<Step> steps;
    std::vector<PostfixOp> postfix;
    std::vector<PostfixOp> perItemGroupReceivers;
    std::vector<MaterialFlowSegment> flow;
};

struct SocketSelector {
    SocketSelectorKind kind = SocketSelectorKind::Stream;
    juce::String name;
    bool qualified = false;
    SignalExpr expr;
};

struct Statement {
    StatementKind kind = StatementKind::Unknown;
    juce::String name;
    juce::String defineName;
    juce::String token;
    std::vector<juce::String> targets;
    std::vector<juce::String> params;
    SignalExpr body;
    Material material;
    bool socketSelected = false;
    std::vector<SocketSelector> selectors;
};

struct Program {
    std::vector<Statement> statements;
    std::vector<Diagnostic> diagnostics;
};

struct ChannelReference {
    curlop::vm::ChannelSourceKind sourceKind =
        curlop::vm::ChannelSourceKind::LocalEmission;
    juce::String signal;
    std::uint32_t channel = 0;
};

struct ChannelRoute {
    juce::String output;
    std::vector<ChannelReference> references;
};

struct ParseResult {
    Program typedProgram;
    juce::var program;
    std::vector<Diagnostic> diagnostics;
    juce::String reportJson;
    juce::String reportText;

    bool ok() const noexcept { return diagnostics.empty(); }
};

struct LowerResult {
    curlop::script::ScriptAst ast;
    std::vector<Diagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

Program parseProgramV2(const juce::String& source);
ParseResult parseV2(const juce::String& source);
LowerResult lowerV2ToScriptAst(const Program& program, double bpm = 120.0);
bool isChannelRouteSyntax(const Statement& statement);
bool parseChannelRoute(const Statement& statement, ChannelRoute& route,
                       juce::String& error);
curlop::script::CompileResultV2 compileV2Syntax(const juce::String& source,
                                                const curlop::script::CompileOptions& opts = {});

// Authoritative, machine-readable inventory for the Script V2 reference
// manual. This describes the V2 parser/lowerer surface only; legacy
// ScriptLanguage schema metadata is deliberately excluded.
juce::var referenceManifestV2();

} // namespace curlop::script::v2
