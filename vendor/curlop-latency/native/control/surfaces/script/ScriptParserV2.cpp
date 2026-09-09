#include "ScriptParserV2.h"

#include "control/surfaces/script/ScriptLanguage.h"
#include "control/surfaces/script/music/MusicData.h"
#include "control/surfaces/script/music/PitchUtils.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

namespace curlop::script::v2 {
namespace {

static bool parseStrictFloatLiteral(const juce::String& raw, float* out = nullptr)
{
    const auto t = raw.trim();
    if (t.isEmpty())
        return false;

    const auto text = t.toStdString();
    char* end = nullptr;
    errno = 0;
    const float v = std::strtof(text.c_str(), &end);
    const bool ok = end != text.c_str()
        && end != nullptr
        && *end == '\0'
        && errno != ERANGE
        && std::isfinite(v);
    if (! ok)
        return false;

    if (out != nullptr)
        *out = v;
    return true;
}

static bool parseStrictDoubleLiteral(const juce::String& raw, double* out = nullptr)
{
    const auto t = raw.trim();
    if (t.isEmpty())
        return false;

    const auto text = t.toStdString();
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(text.c_str(), &end);
    const bool ok = end != text.c_str()
        && end != nullptr
        && *end == '\0'
        && errno != ERANGE
        && std::isfinite(v);
    if (! ok)
        return false;

    if (out != nullptr)
        *out = v;
    return true;
}

static bool parseStrictRouteNumberLiteral(const juce::String& raw, float* out = nullptr)
{
    const auto t = raw.trim();
    const int slash = t.indexOfChar('/');
    if (slash > 0 && slash < t.length() - 1
        && t.indexOfChar(slash + 1, '/') < 0) {
        float num = 0.0f;
        float den = 0.0f;
        if (! parseStrictFloatLiteral(t.substring(0, slash), &num)
            || ! parseStrictFloatLiteral(t.substring(slash + 1), &den)
            || den == 0.0f)
            return false;
        const float value = num / den;
        if (! std::isfinite(value))
            return false;
        if (out != nullptr)
            *out = value;
        return true;
    }
    return parseStrictFloatLiteral(t, out);
}

enum class Tok {
    eof,
    newline,
    semicolon,
    ident,
    atom,
    lbracket,
    rbracket,
    lparen,
    rparen,
    lbrace,
    rbrace,
    colon,
    dot,
    slash,
    comma,
    amp,
    gt,
    lt,
    eq,
    plus,
    bang
};

struct Token {
    Tok kind = Tok::eof;
    juce::String text;
    int line = 1;
    int col = 1;
    int start = 0;
    int end = 0;
};

static bool isIdentStart(juce::juce_wchar c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isIdentPart(juce::juce_wchar c)
{
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

static bool isDigit(juce::juce_wchar c)
{
    return c >= '0' && c <= '9';
}

static juce::String lower(juce::String s)
{
    return s.toLowerCase();
}

static juce::String trim(juce::String s)
{
    return s.trim();
}

static bool isNoteLike(const juce::String& s)
{
    auto x = lower(s);
    if (x.length() < 2) return false;
    auto c = x[0];
    if (c < 'a' || c > 'g') return false;

    int i = 1;
    while (i < x.length() && (x[i] == '#' || x[i] == 'b')) ++i;
    if (i >= x.length() || ! isDigit(x[i])) return false;
    while (i < x.length() && isDigit(x[i])) ++i;
    return i == x.length();
}

static bool isPolyphonicChordLike(const juce::String& s)
{
    const auto dot = s.indexOfChar('.');
    if (dot <= 0 || dot >= s.length() - 1)
        return false;
    return isNoteLike(s.substring(0, dot));
}

static bool isCataloguedV2Name(const char* family, const juce::String& source)
{
    const auto name = lower(source);
#define V2_CATALOGUE_ENTRY(id, entryName, category, syntax, contexts, support, parser, compiler, label, insert, summary, kind) \
    if (juce::String(category) == family && name == juce::String(entryName).toLowerCase()) return true;
#include "ScriptV2Catalogue.generated.inc"
#undef V2_CATALOGUE_ENTRY
    return false;
}

static bool isSignalGenerator(const juce::String& s)
{
    return isCataloguedV2Name("signal_generators", s)
        || isCataloguedV2Name("parsed_only_generators", s);
}

static bool isLowerableVmGenerator(const juce::String& s)
{
    return isCataloguedV2Name("signal_generators", s)
        || isCataloguedV2Name("generators", s);
}

static bool isKnownReceiver(const juce::String& s)
{
    static const char* names[] = {
        "pitch", "gate", "velocity", "cutoff", "drive", "res", "mix", "pan", "decay"
    };
    auto x = lower(s);
    for (auto* n : names)
        if (x == n) return true;
    return false;
}

static bool isLowerableStructuralPostfix(const juce::String& s)
{
    return isCataloguedV2Name("structural_processors", s);
}

static bool isStepProcessorCall(const juce::String& s)
{
    return isCataloguedV2Name("structural_processors", s)
        || isCataloguedV2Name("step_processors", s);
}

static bool isBoundary(Tok k)
{
    return k == Tok::eof || k == Tok::newline || k == Tok::semicolon;
}

static juce::String normalizeSource(juce::String s)
{
    s = s.trim();
    while (s.contains("  "))
        s = s.replace("  ", " ");
    s = s.replace("\n", " ").replace("\t", " ");
    while (s.contains("  "))
        s = s.replace("  ", " ");
    return s.trim();
}

static juce::String canonicalParam(const juce::String& s)
{
    const auto sv = ::curlop::script::canonicalize(
        std::string_view(s.toRawUTF8(), (size_t) s.getNumBytesAsUTF8()));
    return juce::String::fromUTF8(sv.data(), (int) sv.size());
}

static std::vector<juce::String> collectInputReads(const juce::String& source)
{
    std::vector<juce::String> out;
    std::set<juce::String> seen;

    for (int i = 0; i < source.length(); ++i) {
        if (source[i] != '<')
            continue;

        int j = i + 1;
        while (j < source.length() && (source[j] == ' ' || source[j] == '\t'))
            ++j;
        if (j >= source.length() || ! isIdentStart(source[j]))
            continue;

        juce::String path;
        while (j < source.length()) {
            if (! isIdentStart(source[j]))
                break;

            const int start = j;
            ++j;
            while (j < source.length() && isIdentPart(source[j]))
                ++j;
            if (path.isNotEmpty())
                path << ".";
            path << source.substring(start, j);

            if (j < source.length() && source[j] == '.') {
                ++j;
                continue;
            }
            break;
        }

        if (path.isNotEmpty() && seen.insert(path).second)
            out.push_back(path);
    }

    return out;
}

static juce::var makeObject()
{
    return juce::var(new juce::DynamicObject());
}

static juce::var makeArray()
{
    return juce::var(juce::Array<juce::var>());
}

static void set(juce::var& obj, const juce::Identifier& key, const juce::var& value)
{
    if (auto* dyn = obj.getDynamicObject())
        dyn->setProperty(key, value);
}

static void append(juce::var& arr, const juce::var& value)
{
    if (auto* a = arr.getArray())
        a->add(value);
}

static juce::var stringsToVar(const std::vector<juce::String>& values)
{
    auto arr = makeArray();
    for (const auto& v : values)
        append(arr, v);
    return arr;
}

static juce::String statementKindName(StatementKind k)
{
    switch (k) {
        case StatementKind::Definition:   return "definition";
        case StatementKind::Function:     return "function";
        case StatementKind::Route:        return "route";
        case StatementKind::SocketExport: return "socket_export";
        case StatementKind::SocketRoute:  return "socket_route";
        case StatementKind::SessionTarget:return "session_target";
        case StatementKind::Unknown:      break;
    }
    return "unknown";
}

static SignalExpr makeSignalExprFromSource(const juce::String& rawSource)
{
    SignalExpr expr;
    expr.source = normalizeSource(rawSource);

    juce::String current;
    int parens = 0;
    int brackets = 0;
    int braces = 0;

    for (int i = 0; i < expr.source.length(); ++i) {
        auto c = expr.source[i];
        if (c == '(') ++parens;
        else if (c == ')') --parens;
        else if (c == '[') ++brackets;
        else if (c == ']') --brackets;
        else if (c == '{') ++braces;
        else if (c == '}') --braces;

        if (c == ':' && parens == 0 && brackets == 0 && braces == 0) {
            expr.chain.push_back(trim(current));
            current = {};
        } else {
            current << juce::String::charToString(c);
        }
    }
    if (current.isNotEmpty())
        expr.chain.push_back(trim(current));
    expr.inputReads = collectInputReads(expr.source);

    return expr;
}

static juce::String postfixKindName(PostfixKind k)
{
    switch (k) {
        case PostfixKind::StructuralOp:                return "structural_op";
        case PostfixKind::GroupReceiver:               return "group_receiver";
        case PostfixKind::InvalidBareSignalGenerator:  return "invalid_bare_signal_generator";
    }
    return "structural_op";
}

static juce::String materialFlowJoinName(MaterialFlowJoin k)
{
    switch (k) {
        case MaterialFlowJoin::Chain: return "chain";
        case MaterialFlowJoin::Break: return "break";
    }
    return "chain";
}

class Lexer {
public:
    explicit Lexer(juce::String input) : src(std::move(input)) {}

    std::vector<Token> run()
    {
        while (pos < src.length()) {
            auto c = src[pos];
            const int start = pos;
            const int tokenLine = line;
            const int tokenCol = col;

            if (c == ' ' || c == '\t' || c == '\r') {
                advance(c);
                continue;
            }

            if (c == '\n') {
                add(Tok::newline, "\n", start, start + 1, tokenLine, tokenCol);
                advance(c);
                continue;
            }

            switch (c) {
                case ';': addOne(Tok::semicolon); continue;
                case '[': addOne(Tok::lbracket); continue;
                case ']': addOne(Tok::rbracket); continue;
                case '(': addOne(Tok::lparen); continue;
                case ')': addOne(Tok::rparen); continue;
                case '{': addOne(Tok::lbrace); continue;
                case '}': addOne(Tok::rbrace); continue;
                case ':': addOne(Tok::colon); continue;
                case '.': addOne(Tok::dot); continue;
                case '/': addOne(Tok::slash); continue;
                case ',': addOne(Tok::comma); continue;
                case '&': addOne(Tok::amp); continue;
                case '>': addOne(Tok::gt); continue;
                case '<': addOne(Tok::lt); continue;
                case '=': addOne(Tok::eq); continue;
                case '+': addOne(Tok::plus); continue;
                case '!': addOne(Tok::bang); continue;
                default: break;
            }

            if (isIdentStart(c)) {
                while (pos < src.length() && isIdentPart(src[pos]))
                    advance(src[pos]);
                add(Tok::ident, src.substring(start, pos), start, pos, tokenLine, tokenCol);
                continue;
            }

            if (isDigit(c) || c == '-' || c == '%' || c == '#') {
                advance(c);
                while (pos < src.length()) {
                    auto n = src[pos];
                    if (isIdentPart(n) || n == '%' || n == '#') {
                        advance(n);
                        continue;
                    }
                    if (n == '/' && pos + 1 < src.length() && isDigit(src[pos + 1])) {
                        advance(n);
                        continue;
                    }
                    break;
                }
                add(Tok::atom, src.substring(start, pos), start, pos, tokenLine, tokenCol);
                continue;
            }

            advance(c);
            add(Tok::atom, src.substring(start, pos), start, pos, tokenLine, tokenCol);
        }

        add(Tok::eof, "", pos, pos, line, col);
        return tokens;
    }

private:
    juce::String src;
    std::vector<Token> tokens;
    int pos = 0;
    int line = 1;
    int col = 1;

    void advance(juce::juce_wchar c)
    {
        ++pos;
        if (c == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
    }

    void add(Tok kind, juce::String text, int start, int end, int tokenLine, int tokenCol)
    {
        tokens.push_back(Token{ kind, std::move(text), tokenLine, tokenCol, start, end });
    }

    void addOne(Tok kind)
    {
        const int start = pos;
        const int tokenLine = line;
        const int tokenCol = col;
        auto text = src.substring(pos, pos + 1);
        advance(src[pos]);
        add(kind, text, start, pos, tokenLine, tokenCol);
    }
};

class Parser {
public:
    Parser(juce::String sourceText, std::vector<Token> tokenList)
        : source(std::move(sourceText)), tokens(std::move(tokenList)) {}

    Program parse()
    {
        Program out;
        skipStatementBreaks();
        while (! at(Tok::eof)) {
            out.statements.push_back(parseStatement(out));
            skipStatementBreaks();
        }
        out.diagnostics = diagnostics;
        return out;
    }

private:
    juce::String source;
    std::vector<Token> tokens;
    size_t index = 0;
    std::vector<Diagnostic> diagnostics;

    const Token& peek(int offset = 0) const
    {
        auto i = juce::jlimit<size_t>(0, tokens.size() - 1, index + (size_t) offset);
        return tokens[i];
    }

    bool at(Tok kind, int offset = 0) const { return peek(offset).kind == kind; }

    const Token& consume()
    {
        const auto& t = tokens[index];
        if (index + 1 < tokens.size()) ++index;
        return t;
    }

    bool match(Tok kind)
    {
        if (! at(kind)) return false;
        consume();
        return true;
    }

    void skipNewlines()
    {
        while (at(Tok::newline)) consume();
    }

    void skipStatementBreaks()
    {
        while (at(Tok::newline) || at(Tok::semicolon)) consume();
    }

    void addDiagnostic(juce::String code, juce::String message, const Token& t,
                       juce::String severity = "error")
    {
        diagnostics.push_back(Diagnostic{ std::move(severity), std::move(code), std::move(message), t.line, t.col });
    }

    juce::String expectIdent(juce::String context)
    {
        if (at(Tok::ident)) return consume().text;
        addDiagnostic("V2_EXPECTED_IDENT", "Expected identifier for " + context, peek());
        return {};
    }

    Statement parseStatement(Program&)
    {
        if (at(Tok::gt))
            return parseSocketStatement();

        if (at(Tok::ident) && at(Tok::lparen, 1))
            return parseFunctionDefinition();

        if (looksLikeRoute())
            return parseRoute();

        if (at(Tok::ident) && at(Tok::eq, 1))
            return parseDefinition();

        if (looksLikeSessionTarget())
            return parseSessionTarget();

        Statement err;
        err.kind = StatementKind::Unknown;
        err.token = peek().text;
        addDiagnostic("V2_UNKNOWN_STATEMENT", "Could not classify top-level statement", peek());
        consumeUntilBoundary();
        return err;
    }

    Statement parseSocketStatement()
    {
        Statement s;
        s.kind = StatementKind::SocketExport;
        consume(); // >
        s.name = expectIdent("socket name");

        if (match(Tok::lparen)) {
            s.socketSelected = true;
            s.selectors = parseSocketSelectors();
            if (! match(Tok::rparen))
                addDiagnostic("V2_EXPECTED_RPAREN", "Expected ')' after socket selectors", peek());
        }

        if (match(Tok::lt)) {
            if (s.socketSelected)
                addDiagnostic("V2_SOCKET_ROUTE_SELECTORS",
                              "Socket-to-socket routes use '<' with one output socket name; selector lists belong to '=' exports",
                              peek());
            s.kind = StatementKind::SocketRoute;
            s.body = parseSignalExpressionUntilBoundary();
            return s;
        }

        if (! match(Tok::eq))
            addDiagnostic("V2_EXPECTED_EQUALS", "Expected '=' in socket export", peek());

        s.material = parseMaterial();
        return s;
    }

    std::vector<SocketSelector> parseSocketSelectors()
    {
        std::vector<SocketSelector> selectors;
        while (! at(Tok::rparen) && ! at(Tok::eof)) {
            if (! at(Tok::ident)) {
                addDiagnostic("V2_EXPECTED_IDENT", "Expected socket selector name", peek());
                consume();
                match(Tok::comma);
                continue;
            }

            SocketSelector selector;
            selector.name = parseQualifiedName();
            selector.qualified = selector.name.contains(".");
            if (match(Tok::eq)) {
                selector.kind = SocketSelectorKind::DerivedStream;
                selector.expr = parseSelectorExpression();
            }
            selectors.push_back(std::move(selector));

            if (! match(Tok::comma))
                break;
        }
        return selectors;
    }

    bool looksLikeRoute() const
    {
        size_t i = index;
        if (tokens[i].kind != Tok::ident) return false;
        ++i;
        while (i + 1 < tokens.size() && tokens[i].kind == Tok::amp && tokens[i + 1].kind == Tok::ident)
            i += 2;
        return i < tokens.size() && tokens[i].kind == Tok::lt;
    }

    bool looksLikeSessionTarget() const
    {
        size_t i = index;
        if (i >= tokens.size())
            return false;
        if (tokens[i].kind != Tok::ident || ! tokens[i].text.equalsIgnoreCase("session"))
            return false;
        ++i;
        bool sawDot = false;
        while (i + 1 < tokens.size() && tokens[i].kind == Tok::dot && tokens[i + 1].kind == Tok::ident) {
            sawDot = true;
            i += 2;
        }
        return sawDot && i < tokens.size() && tokens[i].kind == Tok::lparen;
    }

    Statement parseSessionTarget()
    {
        Statement s;
        s.kind = StatementKind::SessionTarget;
        s.name = parseQualifiedName();
        const auto lp = peek();
        if (! match(Tok::lparen))
            addDiagnostic("V2_EXPECTED_CALL", "Expected expression call after session target", peek());
        s.body = parseBalancedExpression(lp);
        return s;
    }

    Statement parseDefinition()
    {
        Statement s;
        s.kind = StatementKind::Definition;
        s.name = consume().text;
        match(Tok::eq);
        s.material = parseMaterial();
        return s;
    }

    Statement parseFunctionDefinition()
    {
        Statement s;
        s.kind = StatementKind::Function;
        s.name = consume().text;
        match(Tok::lparen);

        while (! at(Tok::rparen) && ! at(Tok::eof)) {
            if (at(Tok::ident)) s.params.push_back(consume().text);
            else consume();
            match(Tok::comma);
        }
        match(Tok::rparen);

        if (! match(Tok::eq))
            addDiagnostic("V2_EXPECTED_EQUALS", "Expected '=' after function signature", peek());

        skipNewlines();
        s.body = parseSignalExpressionUntilBoundary();
        return s;
    }

    Statement parseRoute()
    {
        Statement s;
        s.kind = StatementKind::Route;
        s.targets.push_back(expectIdent("route target"));
        while (match(Tok::amp))
            s.targets.push_back(expectIdent("route target"));

        if (! match(Tok::lt))
            addDiagnostic("V2_EXPECTED_ROUTE", "Expected '<' in route statement", peek());

        if (at(Tok::ident) && at(Tok::eq, 1)) {
            s.defineName = consume().text;
            match(Tok::eq);
            s.material = parseMaterial();
        } else {
            s.material = parseMaterial();
        }

        return s;
    }

    Material parseMaterial()
    {
        return parseMaterial(false);
    }

    Material parseMaterial(bool commaTerminates)
    {
        skipNewlines();
        if (match(Tok::lbracket)) {
            auto mat = parseSequence(Tok::rbracket, false);
            if (! match(Tok::rbracket))
                addDiagnostic("V2_EXPECTED_RBRACKET", "Expected closing ']'", peek());
            if (at(Tok::lparen)) {
                const auto lp = consume();
                auto expr = parseBalancedExpression(lp);
                mat.explicitPerItemGroupReceivers = true;
                appendPerItemGroupReceiver(mat, expr, lp);
            }
            mat.postfix = parsePostfixOps();
            parseMaterialFlowTail(mat);
            return mat;
        }

        if (at(Tok::ident) && ! isNoteLike(peek().text)
            && ! isPolyphonicChordLike(peek().text)
            && (isBoundary(peek(1).kind) || at(Tok::colon, 1) || at(Tok::bang, 1))) {
            Material ref;
            ref.kind = MaterialKind::Ref;
            ref.refName = consume().text;
            parseMaterialFlowTail(ref);
            return ref;
        }

        if (at(Tok::ident) && ! isNoteLike(peek().text)
            && at(Tok::ident, 1) && at(Tok::lparen, 2)) {
            Material ref;
            ref.kind = MaterialKind::Ref;
            ref.refName = consume().text;
            ref.postfix = parsePostfixOps();
            parseMaterialFlowTail(ref);
            return ref;
        }

        auto mat = parseSequence(commaTerminates ? Tok::rbrace : Tok::eof,
                                 true,
                                 commaTerminates);
        parseMaterialFlowTail(mat);
        return mat;
    }

    void parseMaterialFlowTail(Material& mat)
    {
        while (at(Tok::colon) || at(Tok::bang)) {
            mat.explicitMaterialFlow = true;
            MaterialFlowSegment segment;
            segment.join = at(Tok::bang) ? MaterialFlowJoin::Break
                                         : MaterialFlowJoin::Chain;
            consume();
            segment.material = std::make_shared<Material>(parseMaterialFlowSegment());
            mat.flow.push_back(std::move(segment));
        }
    }

    Material parseMaterialFlowSegment()
    {
        skipNewlines();

        if (match(Tok::lbracket)) {
            auto mat = parseSequence(Tok::rbracket, false);
            if (! match(Tok::rbracket))
                addDiagnostic("V2_EXPECTED_RBRACKET", "Expected closing ']'", peek());
            if (at(Tok::lparen)) {
                const auto lp = consume();
                auto expr = parseBalancedExpression(lp);
                mat.explicitPerItemGroupReceivers = true;
                appendPerItemGroupReceiver(mat, expr, lp);
            }
            mat.postfix = parsePostfixOps();
            return mat;
        }

        if (at(Tok::ident) && ! isNoteLike(peek().text) && at(Tok::lparen, 1)) {
            Material mat;
            mat.kind = MaterialKind::Sequence;
            mat.implicit = true;
            mat.postfix = parsePostfixOps();
            return mat;
        }

        if (at(Tok::ident) && ! isNoteLike(peek().text)
            && ! isPolyphonicChordLike(peek().text)
            && (isBoundary(peek(1).kind) || at(Tok::colon, 1) || at(Tok::bang, 1))) {
            Material ref;
            ref.kind = MaterialKind::Ref;
            ref.refName = consume().text;
            return ref;
        }

        auto mat = parseSequence(Tok::eof, true);
        mat.postfix = parsePostfixOps();
        return mat;
    }

    Material parseSequence(Tok closeKind, bool implicit, bool commaTerminates = false)
    {
        Material mat;
        mat.kind = MaterialKind::Sequence;
        mat.implicit = implicit;
        while (! at(Tok::eof) && ! at(closeKind)
               && ! (commaTerminates && at(Tok::comma))
               && ! (implicit && (isBoundary(peek().kind) || at(Tok::colon) || at(Tok::bang)))) {
            skipNewlines();
            if (at(closeKind) || at(Tok::eof)
                || (commaTerminates && at(Tok::comma))
                || (implicit && (isBoundary(peek().kind) || at(Tok::colon) || at(Tok::bang)))) break;
            mat.steps.push_back(parseStep(closeKind, implicit, commaTerminates));
            if (match(Tok::slash)) {
                skipNewlines();
                if (at(closeKind) || at(Tok::eof)
                    || (commaTerminates && at(Tok::comma))
                    || (implicit && (isBoundary(peek().kind) || at(Tok::colon) || at(Tok::bang)))) {
                    mat.steps.push_back(Step{});
                }
            }
        }
        return mat;
    }

    Step parseStep(Tok closeKind, bool implicit, bool commaTerminates = false)
    {
        Step step;
        while (! at(Tok::eof) && ! at(Tok::slash) && ! at(closeKind)
               && ! (commaTerminates && at(Tok::comma))
               && ! (implicit && (isBoundary(peek().kind) || at(Tok::colon) || at(Tok::bang)))) {
            if (at(Tok::newline)) {
                consume();
                continue;
            }
            step.items.push_back(parseStepItem());
        }
        return step;
    }

    StepItem parseStepItem()
    {
        if (at(Tok::lparen))
            return parseTupletItem();

        if (at(Tok::lbracket))
            return parseGroupedReceiver();

        if (at(Tok::ident)) {
            auto name = parseQualifiedName();
            if (name == "_") {
                StepItem item;
                item.kind = StepItemKind::Hold;
                item.value = name;
                return item;
            }
            if (name.equalsIgnoreCase("markov") && at(Tok::lparen))
                return parseMarkovCall(name);
            if (name.equalsIgnoreCase("stack") && at(Tok::lparen))
                return parseStackCall(name);
            if ((name.equalsIgnoreCase("preset") || name.equalsIgnoreCase("select"))
                && at(Tok::lparen))
                return parseBlockCall(name);
            if (at(Tok::lparen))
                return parseReceiverCall(name);

            StepItem item;
            if (isNoteLike(name))
                item.kind = StepItemKind::Note;
            else if (isPolyphonicChordLike(name))
                item.kind = StepItemKind::PolyphonicChord;
            else
                item.kind = StepItemKind::Atom;
            item.value = name;
            return item;
        }

        StepItem item;
        item.value = consume().text;
        item.kind = item.value == "-" ? StepItemKind::Rest : StepItemKind::Atom;
        return item;
    }

    StepItem parseTupletItem()
    {
        consume(); // (
        StepItem item;
        item.kind = StepItemKind::Tuplet;
        item.nested = std::make_shared<Material>(parseSequence(Tok::rparen, false));
        item.nested->implicit = false;
        if (! match(Tok::rparen))
            addDiagnostic("V2_EXPECTED_RPAREN", "Expected closing ')' after tuplet", peek());
        return item;
    }

    juce::String parseQualifiedName()
    {
        auto out = expectIdent("qualified name");
        while (match(Tok::dot)) {
            out << ".";
            out << expectIdent("qualified name segment");
        }
        return out;
    }

    StepItem parseGroupedReceiver()
    {
        const auto start = consume();
        StepItem item;
        item.kind = StepItemKind::GroupedReceiver;
        item.grouped.targets.push_back(expectIdent("grouped receiver target"));
        while (match(Tok::amp))
            item.grouped.targets.push_back(expectIdent("grouped receiver target"));

        if (! match(Tok::rbracket))
            addDiagnostic("V2_EXPECTED_RBRACKET", "Expected ']' after grouped receiver targets", peek());
        const auto callToken = peek();
        if (! match(Tok::lparen))
            addDiagnostic("V2_EXPECTED_CALL", "Expected expression call after grouped receiver", peek());

        item.grouped.expr = parseBalancedExpression(callToken.kind == Tok::lparen ? callToken : start);
        return item;
    }

    StepItem parseReceiverCall(const juce::String& target)
    {
        const auto callStart = consume(); // LPAREN
        StepItem item;
        item.kind = StepItemKind::Receiver;
        item.receiver.target = target;
        item.receiver.foreign = target.contains(".");
        item.receiver.expr = parseBalancedExpression(callStart);
        return item;
    }

    StepItem parseMarkovCall(const juce::String& target)
    {
        const auto callStart = consume(); // LPAREN
        StepItem item;
        item.kind = StepItemKind::Markov;
        item.receiver.target = target;
        item.receiver.expr = parseBalancedExpression(callStart);
        return item;
    }

    StepItem parseStackCall(const juce::String& target)
    {
        const auto callStart = consume(); // LPAREN
        StepItem item;
        item.kind = StepItemKind::Stack;
        item.value = target.toLowerCase();
        item.receiver.target = target;
        item.receiver.expr = parseBalancedExpression(callStart);

        skipNewlines();
        if (! match(Tok::lbrace)) {
            addDiagnostic("V2_EXPECTED_BLOCK",
                          "stack(...) expects a '{ ... }' material block",
                          peek());
            return item;
        }

        item.nested = std::make_shared<Material>(parseSequence(Tok::rbrace, true));
        item.nested->implicit = true;
        skipNewlines();
        if (! match(Tok::rbrace))
            addDiagnostic("V2_EXPECTED_RBRACE",
                          "Expected closing '}' after stack material block",
                          peek());
        return item;
    }

    StepItem parseBlockCall(const juce::String& name)
    {
        const auto callStart = consume(); // LPAREN
        StepItem item;
        item.kind = name.equalsIgnoreCase("preset") ? StepItemKind::Preset
                                                     : StepItemKind::Select;
        item.value = name.toLowerCase();
        item.receiver.target = item.value;
        item.receiver.expr = parseBalancedExpression(callStart);

        skipNewlines();
        if (! match(Tok::lbrace)) {
            addDiagnostic("V2_EXPECTED_BLOCK",
                          item.value + "(...) expects a '{ ... }' material block",
                          peek());
            return item;
        }

        while (! at(Tok::eof) && ! at(Tok::rbrace)) {
            skipNewlines();
            if (at(Tok::rbrace))
                break;
            item.blockOptions.push_back(std::make_shared<Material>(parseMaterial(true)));
            skipNewlines();
            if (! match(Tok::comma))
                break;
        }

        skipNewlines();
        if (! match(Tok::rbrace))
            addDiagnostic("V2_EXPECTED_RBRACE",
                          "Expected closing '}' after " + item.value + " material block",
                          peek());
        return item;
    }

    std::vector<PostfixOp> parsePostfixOps()
    {
        std::vector<PostfixOp> ops;
        while (at(Tok::ident) && at(Tok::lparen, 1)) {
            auto name = consume().text;
            const auto lp = consume();
            PostfixOp op;
            op.name = name;
            if (isSignalGenerator(name)) {
                op.kind = PostfixKind::InvalidBareSignalGenerator;
                addDiagnostic("V2_BARE_SIGNAL_GENERATOR",
                              "Bare signal generator after material has no receiver",
                              lp,
                              "warning");
            } else {
                op.kind = name.equalsIgnoreCase("chord")
                              ? PostfixKind::StructuralOp
                              : isKnownReceiver(name) ? PostfixKind::GroupReceiver
                                                      : PostfixKind::StructuralOp;
            }
            op.expr = parseBalancedExpression(lp);
            ops.push_back(std::move(op));
        }
        return ops;
    }

    void appendPerItemGroupReceiver(Material& material, const SignalExpr& expr,
                                    const Token& token)
    {
        const auto source = expr.source.trim();
        const int lp = source.indexOfChar('(');
        if (lp <= 0 || ! source.endsWithChar(')')) {
            addDiagnostic("V2_EXPECTED_CALL",
                          "Expected receiver call inside per-item group receiver block",
                          token);
            return;
        }

        PostfixOp op;
        op.kind = PostfixKind::GroupReceiver;
        op.name = source.substring(0, lp).trim();
        op.expr = signalExprFromSource(source.substring(lp + 1, source.length() - 1));
        if (! isKnownReceiver(op.name)) {
            addDiagnostic("V2_EXPECTED_CALL",
                          "Per-item group receiver block must name a known receiver",
                          token,
                          "warning");
        }
        material.perItemGroupReceivers.push_back(std::move(op));
    }

    SignalExpr parseBalancedExpression(const Token& lparenToken)
    {
        const int startOffset = at(Tok::rparen) ? lparenToken.end : peek().start;
        int depth = 1;
        int endOffset = lparenToken.end;

        while (! at(Tok::eof) && depth > 0) {
            if (at(Tok::lparen)) ++depth;
            else if (at(Tok::rparen)) {
                --depth;
                if (depth == 0) {
                    endOffset = peek().start;
                    consume();
                    break;
                }
            }
            if (depth > 0) {
                endOffset = peek().end;
                consume();
            }
        }

        if (depth != 0)
            addDiagnostic("V2_EXPECTED_RPAREN", "Expected closing ')'", lparenToken);

        return signalExprFromSource(source.substring(startOffset, endOffset));
    }

    SignalExpr parseSignalExpressionUntilBoundary()
    {
        const int startOffset = peek().start;
        int endOffset = startOffset;
        int parens = 0;
        int brackets = 0;
        int braces = 0;

        while (! at(Tok::eof)) {
            if (parens == 0 && brackets == 0 && braces == 0 && isBoundary(peek().kind))
                break;

            if (at(Tok::lparen)) ++parens;
            else if (at(Tok::rparen)) --parens;
            else if (at(Tok::lbracket)) ++brackets;
            else if (at(Tok::rbracket)) --brackets;
            else if (at(Tok::lbrace)) ++braces;
            else if (at(Tok::rbrace)) --braces;

            endOffset = peek().end;
            consume();
        }

        return signalExprFromSource(source.substring(startOffset, endOffset));
    }

    SignalExpr parseSelectorExpression()
    {
        const int startOffset = peek().start;
        int endOffset = startOffset;
        int parens = 0;
        int brackets = 0;
        int braces = 0;

        while (! at(Tok::eof)) {
            if (parens == 0 && brackets == 0 && braces == 0
                && (at(Tok::comma) || at(Tok::rparen)))
                break;

            if (at(Tok::lparen)) ++parens;
            else if (at(Tok::rparen)) --parens;
            else if (at(Tok::lbracket)) ++brackets;
            else if (at(Tok::rbracket)) --brackets;
            else if (at(Tok::lbrace)) ++braces;
            else if (at(Tok::rbrace)) --braces;

            endOffset = peek().end;
            consume();
        }

        return signalExprFromSource(source.substring(startOffset, endOffset));
    }

    SignalExpr signalExprFromSource(const juce::String& rawSource)
    {
        return makeSignalExprFromSource(rawSource);
    }

    void consumeUntilBoundary()
    {
        while (! at(Tok::eof) && ! isBoundary(peek().kind))
            consume();
    }
};

static juce::var signalExprToVar(const SignalExpr& expr)
{
    auto obj = makeObject();
    set(obj, "kind", "signal_expr");
    set(obj, "source", expr.source);
    set(obj, "chain", stringsToVar(expr.chain));
    set(obj, "inputReads", stringsToVar(expr.inputReads));
    return obj;
}

static juce::var materialToVar(const Material& mat);

static juce::var flowToVar(const std::vector<MaterialFlowSegment>& flow)
{
    auto arr = makeArray();
    for (const auto& segment : flow) {
        auto f = makeObject();
        set(f, "join", materialFlowJoinName(segment.join));
        if (segment.material)
            set(f, "material", materialToVar(*segment.material));
        append(arr, f);
    }
    return arr;
}

static juce::var stepItemToVar(const StepItem& item)
{
    auto obj = makeObject();
    switch (item.kind) {
        case StepItemKind::Note:
            set(obj, "kind", "note");
            set(obj, "value", item.value);
            break;
        case StepItemKind::PolyphonicChord:
            set(obj, "kind", "polyphonic_chord");
            set(obj, "value", item.value);
            break;
        case StepItemKind::Atom:
            set(obj, "kind", "atom");
            set(obj, "value", item.value);
            break;
        case StepItemKind::Rest:
            set(obj, "kind", "rest");
            set(obj, "value", item.value);
            break;
        case StepItemKind::Hold:
            set(obj, "kind", "hold");
            set(obj, "value", item.value);
            break;
        case StepItemKind::Receiver:
            set(obj, "kind", "receiver");
            set(obj, "target", item.receiver.target);
            set(obj, "scope", item.receiver.foreign ? "foreign" : "context");
            set(obj, "expr", signalExprToVar(item.receiver.expr));
            break;
        case StepItemKind::GroupedReceiver:
            set(obj, "kind", "grouped_receiver");
            set(obj, "targets", stringsToVar(item.grouped.targets));
            set(obj, "expr", signalExprToVar(item.grouped.expr));
            break;
        case StepItemKind::Tuplet:
            set(obj, "kind", "tuplet");
            if (item.nested)
                set(obj, "material", materialToVar(*item.nested));
            break;
        case StepItemKind::Markov:
            set(obj, "kind", "markov");
            set(obj, "expr", signalExprToVar(item.receiver.expr));
            break;
        case StepItemKind::Stack:
            set(obj, "kind", "stack");
            set(obj, "expr", signalExprToVar(item.receiver.expr));
            if (item.nested)
                set(obj, "material", materialToVar(*item.nested));
            break;
        case StepItemKind::Preset:
        case StepItemKind::Select:
        {
            const bool isPreset = item.kind == StepItemKind::Preset;
            set(obj, "kind", isPreset ? "preset" : "select");
            set(obj, "expr", signalExprToVar(item.receiver.expr));
            auto options = makeArray();
            for (const auto& option : item.blockOptions)
                if (option)
                    append(options, materialToVar(*option));
            set(obj, "options", options);
            break;
        }
    }
    return obj;
}

static juce::var materialToVar(const Material& mat)
{
    auto obj = makeObject();
    if (mat.kind == MaterialKind::Ref) {
        set(obj, "kind", "ref");
        set(obj, "name", mat.refName);
        set(obj, "explicitMaterialFlow", mat.explicitMaterialFlow);
        set(obj, "explicitPerItemGroupReceivers", mat.explicitPerItemGroupReceivers);
        auto postfix = makeArray();
        for (const auto& op : mat.postfix) {
            auto p = makeObject();
            set(p, "kind", postfixKindName(op.kind));
            set(p, "name", op.name);
            set(p, "expr", signalExprToVar(op.expr));
            append(postfix, p);
        }
        set(obj, "postfix", postfix);
        auto perItem = makeArray();
        for (const auto& op : mat.perItemGroupReceivers) {
            auto p = makeObject();
            set(p, "kind", postfixKindName(op.kind));
            set(p, "name", op.name);
            set(p, "expr", signalExprToVar(op.expr));
            append(perItem, p);
        }
        set(obj, "perItemGroupReceivers", perItem);
        set(obj, "flow", flowToVar(mat.flow));
        return obj;
    }

    set(obj, "kind", "sequence");
    set(obj, "implicit", mat.implicit);
    set(obj, "explicitMaterialFlow", mat.explicitMaterialFlow);
    set(obj, "explicitPerItemGroupReceivers", mat.explicitPerItemGroupReceivers);
    auto steps = makeArray();
    for (const auto& step : mat.steps) {
        auto s = makeObject();
        set(s, "kind", "step");
        auto items = makeArray();
        for (const auto& item : step.items)
            append(items, stepItemToVar(item));
        set(s, "items", items);
        append(steps, s);
    }
    set(obj, "steps", steps);

    auto postfix = makeArray();
    for (const auto& op : mat.postfix) {
        auto p = makeObject();
        set(p, "kind", postfixKindName(op.kind));
        set(p, "name", op.name);
        set(p, "expr", signalExprToVar(op.expr));
        append(postfix, p);
    }
    set(obj, "postfix", postfix);
    auto perItem = makeArray();
    for (const auto& op : mat.perItemGroupReceivers) {
        auto p = makeObject();
        set(p, "kind", postfixKindName(op.kind));
        set(p, "name", op.name);
        set(p, "expr", signalExprToVar(op.expr));
        append(perItem, p);
    }
    set(obj, "perItemGroupReceivers", perItem);
    set(obj, "flow", flowToVar(mat.flow));
    return obj;
}

static juce::var diagnosticsVar(const std::vector<Diagnostic>& diagnostics)
{
    auto arr = makeArray();
    for (const auto& d : diagnostics) {
        auto obj = makeObject();
        set(obj, "severity", d.severity);
        set(obj, "code", d.code);
        set(obj, "message", d.message);
        set(obj, "line", d.line);
        set(obj, "col", d.col);
        append(arr, obj);
    }
    return arr;
}

static juce::var programToVar(const Program& program)
{
    auto obj = makeObject();
    auto statements = makeArray();

    for (const auto& stmt : program.statements) {
        auto s = makeObject();
        set(s, "kind", statementKindName(stmt.kind));
        if (stmt.name.isNotEmpty()) set(s, "name", stmt.name);
        if (stmt.token.isNotEmpty()) set(s, "token", stmt.token);
        if (! stmt.targets.empty()) set(s, "targets", stringsToVar(stmt.targets));
        if (stmt.defineName.isNotEmpty()) set(s, "define", stmt.defineName);

        if (stmt.kind == StatementKind::Function) {
            set(s, "params", stringsToVar(stmt.params));
            set(s, "body", signalExprToVar(stmt.body));
            set(s, "binding", "bind-at-call-site");
        } else if (stmt.kind == StatementKind::SocketRoute) {
            set(s, "body", signalExprToVar(stmt.body));
        } else if (stmt.kind == StatementKind::SessionTarget) {
            set(s, "body", signalExprToVar(stmt.body));
        } else if (stmt.kind == StatementKind::SocketExport) {
            set(s, "mode", stmt.socketSelected ? "selected" : "whole_bundle");
            auto selectors = makeArray();
            for (const auto& selector : stmt.selectors) {
                auto sel = makeObject();
                set(sel, "kind", selector.kind == SocketSelectorKind::DerivedStream
                                    ? "derived_stream" : "stream");
                set(sel, "name", selector.name);
                if (selector.kind == SocketSelectorKind::DerivedStream)
                    set(sel, "expr", signalExprToVar(selector.expr));
                else
                    set(sel, "qualified", selector.qualified);
                append(selectors, sel);
            }
            set(s, "selectors", selectors);
            set(s, "material", materialToVar(stmt.material));
        } else if (stmt.kind == StatementKind::Definition || stmt.kind == StatementKind::Route) {
            set(s, "material", materialToVar(stmt.material));
        }

        append(statements, s);
    }

    set(obj, "version", "v2-parser-report-spike");
    set(obj, "statements", statements);
    set(obj, "diagnostics", diagnosticsVar(program.diagnostics));
    return obj;
}

static juce::String buildReportText(const Program& program)
{
    juce::String out;
    out << "V2 parser report: " << (int) program.statements.size() << " statement(s)\n";
    for (const auto& stmt : program.statements) {
        out << "- " << statementKindName(stmt.kind);
        if (stmt.name.isNotEmpty()) out << " " << stmt.name;
        if (! stmt.targets.empty()) out << " targets=" << juce::JSON::toString(stringsToVar(stmt.targets), false);
        if (stmt.defineName.isNotEmpty()) out << " define=" << stmt.defineName;
        out << "\n";
    }

    if (! program.diagnostics.empty()) {
        out << "Diagnostics:\n";
        for (const auto& d : program.diagnostics)
            out << "- " << d.severity << " " << d.code << " @" << d.line << ":" << d.col
                << " " << d.message << "\n";
    }
    return out;
}

static bool hasErrorDiagnostic(const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& d) { return d.severity == "error"; });
}

static std::unique_ptr<AstNode> makeAstNode(juce::String type)
{
    auto n = std::make_unique<AstNode>();
    n->type = std::move(type);
    return n;
}

static std::unique_ptr<AstNode> makeSequence()
{
    auto n = makeAstNode("Sequence");
    n->scopeType = "parallel";
    return n;
}

static std::unique_ptr<AstNode> makeStep()
{
    return makeAstNode("Step");
}

static Processor makeProcessor(const juce::String& name, ParseValue value, int ordinal)
{
    Processor p;
    p.name = name;
    p.canonicalName = canonicalParam(name);
    p.sourceOrdinal = ordinal;
    p.isDotParam = true;
    p.args.push_back(std::move(value));
    return p;
}

static Processor makeProcessor(const juce::String& name, std::vector<ParseValue> args,
                               int ordinal)
{
    Processor p;
    p.name = name;
    p.canonicalName = canonicalParam(name);
    p.sourceOrdinal = ordinal;
    p.args = std::move(args);
    return p;
}

static Processor makeVoiceStackProcessor(const juce::String& name,
                                         std::vector<ParseValue> args,
                                         int ordinal)
{
    auto p = makeProcessor(name, std::move(args), ordinal);
    p.isVoiceStack = true;
    return p;
}

static std::unique_ptr<AstNode> makePersistentProcessorSetter(Processor proc)
{
    auto n = makeAstNode("PersistentSetter");
    n->isSetterProcessor = true;
    proc.persistent = true;
    n->setterProcessor = std::move(proc);
    return n;
}

static bool condKeywordIs(std::string_view keyword, const juce::String& value)
{
    return value == juce::String::fromUTF8(keyword.data(), (int) keyword.size());
}

static bool isStaticCondKeyword(const juce::String& value)
{
    for (auto const& c : kCondArgValues)
        if (condKeywordIs(c, value)) return true;
    static constexpr std::string_view backed[] {
        "first", "!first", "even", "odd", "prime", "fib",
        "previous", "!previous"
    };
    for (auto const& c : backed)
        if (condKeywordIs(c, value)) return true;
    return false;
}

static bool normalizeCondArgs(std::vector<ParseValue>& args, juce::String& error)
{
    if (args.empty()) {
        error = "cond(...) needs a condition";
        return false;
    }

    CondNode c;
    auto requireNumberArg = [&] (size_t index, const char* label, double& out) {
        if (index >= args.size()
            || args[index].kind != ParseValue::Kind::Number
            || ! std::isfinite(args[index].number)) {
            error = juce::String("cond(...) ") + label + " argument must be a finite number";
            return false;
        }
        const double n = args[index].number;
        if (n < 0.0 || n > 65535.0 || std::floor(n) != n) {
            error = juce::String("cond(...) ") + label
                  + " argument must be a whole number in the VM u16 range 0..65535";
            return false;
        }
        out = n;
        return true;
    };
    const auto& first = args.front();
    if (first.kind == ParseValue::Kind::String) {
        const juce::String s = first.str.toLowerCase();
        if (! isStaticCondKeyword(s)) {
            error = "cond(...) value '" + s + "' is not lowerable by the current VM";
            return false;
        } else if (s == "first" || s == "!first" || s == "even" || s == "odd"
                   || s == "prime" || s == "fib") {
            c.condType = s;
            if (args.size() > 2) {
                error = "cond(...) " + s + " accepts at most one cycle argument";
                return false;
            }
            if (args.size() > 1) {
                double cycle = 0.0;
                if (! requireNumberArg(1, "cycle", cycle))
                    return false;
                c.cycle = (int) cycle;
            }
        } else if (s == "mod" || s == "every") {
            c.condType = "loops";
            if (args.size() != 2) {
                error = "cond(...) " + s + " needs exactly one loop argument";
                return false;
            }
            double loop = 0.0;
            if (! requireNumberArg(1, "loop", loop))
                return false;
            c.loops.push_back(loop);
        } else if (s == "once" || s == "after") {
            c.condType = s;
            if (args.size() != 2) {
                error = "cond(...) " + s + " needs exactly one loop argument";
                return false;
            }
            double loop = 0.0;
            if (! requireNumberArg(1, "loop", loop))
                return false;
            c.loops.push_back(loop);
        } else {
            c.condType = s;
            if (args.size() > 1) {
                error = "cond(...) " + s + " does not accept arguments";
                return false;
            }
        }
    } else if (first.kind == ParseValue::Kind::Number) {
        c.condType = "loops";
        for (size_t i = 0; i < args.size(); ++i) {
            double loop = 0.0;
            if (! requireNumberArg(i, "loop", loop))
                return false;
            c.loops.push_back(loop);
        }
    } else if (first.kind == ParseValue::Kind::NegatedLoops) {
        c.condType = "!loops";
        c.loops = first.negLoops;
    } else if (first.kind == ParseValue::Kind::Generator
               || first.kind == ParseValue::Kind::SignalChain) {
        c.condType = "expr";
        c.expr = first;
    } else {
        error = "Dynamic cond(...) arguments are not lowerable in this VM slice";
        return false;
    }

    ParseValue wrapped;
    wrapped.kind = ParseValue::Kind::Cond;
    wrapped.cond = std::make_shared<CondNode>(std::move(c));
    args.clear();
    args.push_back(std::move(wrapped));
    return true;
}

static bool normalizeProcessorArgsForV2(const juce::String& name,
                                        std::vector<ParseValue>& args,
                                        juce::String& error)
{
    if (name.equalsIgnoreCase("cond"))
        return normalizeCondArgs(args, error);

    const auto isExprArg = [] (const ParseValue& v) {
        return (v.kind == ParseValue::Kind::Generator && v.gen)
            || (v.kind == ParseValue::Kind::SignalChain && v.signalChain);
    };
    const auto isNumericExprArg = [&] (const ParseValue& v) {
        return v.kind == ParseValue::Kind::Null
            || v.kind == ParseValue::Kind::Number
            || v.kind == ParseValue::Kind::UnitNumber
            || isExprArg(v);
    };
    const auto failNumeric = [&] (const char* processor,
                                  size_t argIndex,
                                  const ParseValue& v) {
        if (isNumericExprArg(v))
            return false;
        error = juce::String(processor);
        error << "(...) argument "
              << (int) argIndex + 1
              << " must be a numeric/unit value or dynamic expression";
        if (v.kind == ParseValue::Kind::String && v.str.isNotEmpty())
            error << ", not '" << v.str << "'";
        return true;
    };
    const auto rejectAllNumeric = [&] (const char* processor) {
        for (size_t i = 0; i < args.size(); ++i)
            if (failNumeric(processor, i, args[i]))
                return false;
        return true;
    };

    const auto n = name.toLowerCase();
    if (n == "repeat" || n == "timescale" || n == "len"
        || n == "deviate" || n == "transpose" || n == "octave"
        || n == "vel" || n == "velocity"
        || n == "ratchet" || n == "flam" || n == "buzz"
        || n == "bounce" || n == "geiger" || n == "grid"
        || n == "fit"
        || n == "prob" || n == "probability")
        return rejectAllNumeric(name.toRawUTF8());

    if (n == "strum")
        return rejectAllNumeric("strum");

    if (n == "arp") {
        if (args.size() > 1 && failNumeric("arp", 1, args[1]))
            return false;
        return true;
    }

    if (n == "bernoulli") {
        if (! args.empty() && failNumeric("bernoulli", 0, args[0]))
            return false;
        return true;
    }

    if (n == "groove") {
        for (size_t i = 1; i < args.size(); ++i)
            if (failNumeric("groove", i, args[i]))
                return false;
        return true;
    }

    if (n == "scale" && args.size() >= 2
        && args[0].kind == ParseValue::Kind::String
        && args[1].kind != ParseValue::Kind::String
        && args[1].kind != ParseValue::Kind::Generator) {
        error = "scale(root, mode) mode argument must be a named scale or supported dynamic scale generator";
        return false;
    }

    return true;
}

static juce::String unsupportedProcessorArgsCode(const juce::String& name,
                                                 const juce::String& fallback,
                                                 const juce::String& error)
{
    if (error.contains("argument") || error.contains("mode argument"))
        return "V2_UNSUPPORTED_PROCESSOR_ARG";
    if (! name.equalsIgnoreCase("cond"))
        return fallback;
    return error.contains("not lowerable by the current VM")
        ? "V2_UNSUPPORTED_COND_VALUE"
        : fallback;
}

static std::unique_ptr<AstNode> makeTriggerEvent(const juce::String& module,
                                                 const juce::String& note)
{
    auto ev = makeAstNode("Event");
    ev->eventType = "trigger";
    ev->module = module;
    const int chordDot = note.indexOfChar('.');
    if (chordDot > 0 && chordDot < note.length() - 1) {
        ev->pitch.note = note.substring(0, chordDot);
        ev->pitch.chord = note.substring(chordDot + 1);
    } else {
        ev->pitch.note = note;
    }
    return ev;
}

static std::unique_ptr<AstNode> makeUpdateEvent(const juce::String& module,
                                                Processor proc)
{
    auto ev = makeAstNode("Event");
    ev->eventType = "update";
    ev->module = module;
    ev->processors.push_back(std::move(proc));
    return ev;
}

static std::unique_ptr<AstNode> makeRestEvent(const juce::String& module)
{
    auto ev = makeAstNode("Event");
    ev->eventType = "rest";
    ev->module = module;
    return ev;
}

static std::unique_ptr<AstNode> makeHoldEvent(const juce::String& module)
{
    auto ev = makeAstNode("Event");
    ev->eventType = "hold";
    ev->module = module;
    return ev;
}

static std::unique_ptr<AstNode> makeLocalOutputEvent(const juce::String& socketName,
                                                     Processor proc)
{
    auto ev = makeAstNode("Event");
    ev->eventType = "local_output";
    ev->outputSocket = socketName;
    ev->processors.push_back(std::move(proc));
    return ev;
}

static std::unique_ptr<AstNode> makeMarkovSelector(const juce::String& module,
                                                   double order,
                                                   std::vector<AstNodePtr> options,
                                                   int ordinal,
                                                   const juce::String& stateKey)
{
    auto ev = makeAstNode("MarkovSelector");
    ev->eventType = "markov";
    ev->module = module;
    ev->weight = order;
    ev->commaGroup = 0;
    ev->line = ordinal;
    ev->stateKey = stateKey;
    ev->options = std::move(options);
    return ev;
}

static std::unique_ptr<AstNode> makeMarkovGateSelector(const juce::String& module,
                                                       double order,
                                                       std::vector<AstNodePtr> options,
                                                       int ordinal,
                                                       const juce::String& stateKey)
{
    auto ev = makeMarkovSelector(module, order, std::move(options), ordinal, stateKey);
    ev->eventType = "markov_gate";
    return ev;
}

static std::unique_ptr<AstNode> makeSelectSelector(ParseValue selector,
                                                   std::vector<AstNodePtr> options)
{
    auto ev = makeAstNode("SelectSelector");
    ev->eventType = "select";
    ev->setterProcessor.name = "select";
    ev->setterProcessor.args.push_back(std::move(selector));
    ev->options = std::move(options);
    return ev;
}

static std::unique_ptr<AstNode> makeAssignment(const juce::String& name,
                                               std::unique_ptr<AstNode> value)
{
    auto a = makeAstNode("Assignment");
    a->name = name;
    a->value = std::move(value);
    return a;
}

static bool isNumericLiteral(const juce::String& raw)
{
    const auto s = raw.trim();
    if (s.isEmpty()) return false;

    bool sawDigit = false;
    bool sawDot = false;
    for (int i = 0; i < s.length(); ++i) {
        const auto c = s[i];
        if (c >= '0' && c <= '9') {
            sawDigit = true;
            continue;
        }
        if (c == '.' && ! sawDot) {
            sawDot = true;
            continue;
        }
        if (i == 0 && c == '-')
            continue;
        return false;
    }
    return sawDigit;
}

static bool parseStaticPositiveInt(const SignalExpr& expr, int& out)
{
    if (expr.chain.size() != 1)
        return false;
    const auto s = expr.source.trim();
    if (! isNumericLiteral(s))
        return false;
    const double number = s.getDoubleValue();
    if (! std::isfinite(number) || number < 1.0)
        return false;
    const auto rounded = (int) std::floor(number);
    if (std::abs(number - (double) rounded) > 0.000001)
        return false;
    out = rounded;
    return true;
}

static bool parseStaticNonNegativeInt(const SignalExpr& expr, int& out)
{
    if (expr.chain.size() != 1)
        return false;
    const auto s = expr.source.trim();
    if (! isNumericLiteral(s))
        return false;
    const double number = s.getDoubleValue();
    if (! std::isfinite(number) || number < 0.0)
        return false;
    const auto rounded = (int) std::floor(number);
    if (std::abs(number - (double) rounded) > 0.000001)
        return false;
    out = rounded;
    return true;
}

static ParseValue parseSimpleValue(const juce::String& raw)
{
    ParseValue out;
    const auto s = raw.trim();
    const auto l = s.toLowerCase();
    auto unitNumber = [&out] (const juce::String& numberText, const char* unit) {
        double number = 0.0;
        if (! parseStrictDoubleLiteral(numberText, &number))
            return false;
        out.kind = ParseValue::Kind::UnitNumber;
        out.number = number;
        out.unit = unit;
        return true;
    };

    if (s.isEmpty()) return out;
    if (l.endsWith("hz") || l.endsWith("khz") || isNoteLike(s)) {
        out.kind = ParseValue::Kind::String;
        out.str = s;
        return out;
    }
    if (l.endsWith("steps") && s.length() > 5
        && unitNumber(s.dropLastCharacters(5), "steps"))
        return out;
    if (l.endsWith("step") && s.length() > 4
        && unitNumber(s.dropLastCharacters(4), "step"))
        return out;
    if (l.endsWith("beats") && s.length() > 5
        && unitNumber(s.dropLastCharacters(5), "beats"))
        return out;
    if (l.endsWith("beat") && s.length() > 4
        && unitNumber(s.dropLastCharacters(4), "beat"))
        return out;
    if (l.endsWith("ms") && s.length() > 2
        && unitNumber(s.dropLastCharacters(2), "ms"))
        return out;
    if (l.endsWith("s") && s.length() > 1
        && unitNumber(s.dropLastCharacters(1), "s"))
        return out;
    if (l.endsWith("%") && s.length() > 1
        && unitNumber(s.dropLastCharacters(1), "%"))
        return out;

    const int slash = s.indexOfChar('/');
    if (slash > 0 && slash < s.length() - 1 && s.indexOfChar(slash + 1, '/') < 0) {
        const auto num = s.substring(0, slash);
        const auto den = s.substring(slash + 1);
        if (isNumericLiteral(num) && isNumericLiteral(den) && den.getDoubleValue() != 0.0) {
            out.kind = ParseValue::Kind::Number;
            out.number = num.getDoubleValue() / den.getDoubleValue();
            return out;
        }
    }

    if (isNumericLiteral(s)) {
        out.kind = ParseValue::Kind::Number;
        out.number = s.getDoubleValue();
        return out;
    }

    out.kind = ParseValue::Kind::String;
    out.str = s;
    return out;
}

static bool validateFiniteSimpleValue(const ParseValue& value,
                                      const juce::String& source,
                                      juce::String& error)
{
    if ((value.kind == ParseValue::Kind::Number
         || value.kind == ParseValue::Kind::UnitNumber)
        && ! std::isfinite(value.number)) {
        error = "Non-finite numeric literal '" + source.trim()
              + "' does not lower in the VM-first subset";
        return false;
    }
    if (value.kind == ParseValue::Kind::String) {
        const auto lower = value.str.toLowerCase();
        if (lower.endsWith("hz") || lower.endsWith("khz")) {
            const bool khz = lower.endsWith("khz");
            const int suffixLength = khz ? 3 : 2;
            double hz = 0.0;
            if (! parseStrictDoubleLiteral(lower.dropLastCharacters(suffixLength), &hz)) {
                error = "Non-finite numeric literal '" + source.trim()
                      + "' does not lower in the VM-first subset";
                return false;
            }
            if (khz)
                hz *= 1000.0;
            if (! std::isfinite(hz)) {
                error = "Non-finite numeric literal '" + source.trim()
                      + "' does not lower in the VM-first subset";
                return false;
            }
        }
    }
    return true;
}

static bool validateTimeDomainValue(const ParseValue& value,
                                    const juce::String& context,
                                    juce::String& error)
{
    if (value.kind == ParseValue::Kind::Number
        || value.kind == ParseValue::Kind::UnitNumber)
        return std::isfinite(value.number);

    if (value.kind == ParseValue::Kind::Generator
        || value.kind == ParseValue::Kind::SignalChain)
        return true;

    if (value.kind == ParseValue::Kind::PolyList) {
        for (const auto& slot : value.list)
            if (! validateTimeDomainValue(slot, context, error))
                return false;
        return true;
    }

    if (value.kind != ParseValue::Kind::String) {
        error = context + " time argument must be numeric, unit, note, frequency, or dynamic";
        return false;
    }

    const auto text = value.str.trim();
    const auto lower = text.toLowerCase();
    if (isNoteLike(text))
        return true;

    auto strictSuffixNumber = [&] (const char* suffix) {
        const juce::String suffixText(suffix);
        return lower.endsWith(suffixText)
            && text.length() > suffixText.length()
            && parseStrictDoubleLiteral(text.dropLastCharacters(suffixText.length()));
    };

    if (strictSuffixNumber("khz")
        || strictSuffixNumber("hz")
        || strictSuffixNumber("ms")
        || strictSuffixNumber("steps")
        || strictSuffixNumber("step")
        || strictSuffixNumber("beats")
        || strictSuffixNumber("beat")
        || strictSuffixNumber("s")
        || strictSuffixNumber("%"))
        return true;

    error = context + " time argument '" + text
          + "' is not a strict numeric/unit, note, frequency, or dynamic value";
    return false;
}

static bool validateIndexedTimeArg(const std::vector<ParseValue>& args,
                                   size_t index,
                                   const juce::String& context,
                                   juce::String& error)
{
    if (index >= args.size())
        return true;
    return validateTimeDomainValue(args[index], context, error);
}

static bool stringStartsLikeNumber(const juce::String& text)
{
    if (text.isEmpty())
        return false;
    const juce::juce_wchar c = text[0];
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')
        return true;

    const auto lowerText = text.toLowerCase();
    return lowerText.startsWith("nan") || lowerText.startsWith("inf");
}

static bool isStrictFrequencyString(const juce::String& raw)
{
    const auto text = raw.trim();
    const auto lower = text.toLowerCase();
    const bool khz = lower.endsWith("khz");
    const bool hz = ! khz && lower.endsWith("hz");
    if (! khz && ! hz)
        return false;

    const int suffixLength = khz ? 3 : 2;
    double value = 0.0;
    if (! parseStrictDoubleLiteral(text.dropLastCharacters(suffixLength), &value))
        return false;
    if (khz)
        value *= 1000.0;
    return std::isfinite(value) && value > 0.0;
}

static bool validateReceiverStaticValue(const ParseValue& value,
                                        const juce::String& target,
                                        juce::String& error)
{
    if (value.kind == ParseValue::Kind::Number
        || value.kind == ParseValue::Kind::UnitNumber)
        return std::isfinite(value.number);

    if (value.kind == ParseValue::Kind::Generator
        || value.kind == ParseValue::Kind::SignalChain)
        return true;

    if (value.kind == ParseValue::Kind::PolyList) {
        for (const auto& slot : value.list)
            if (! validateReceiverStaticValue(slot, target, error))
                return false;
        return true;
    }

    if (value.kind == ParseValue::Kind::String) {
        const auto text = value.str.trim();
        if (isNoteLike(text) || isStrictFrequencyString(text))
            return true;
        error = "Receiver '" + target + "' value '" + text
              + "' is not a strict numeric/unit, note, frequency, or dynamic value";
        return false;
    }

    error = "Receiver '" + target + "' value is not lowerable";
    return false;
}

static bool validateValueArgDoesNotLookMalformedNumeric(const ParseValue& value,
                                                        const juce::String& context,
                                                        juce::String& error)
{
    if (value.kind == ParseValue::Kind::Number
        || value.kind == ParseValue::Kind::UnitNumber)
        return std::isfinite(value.number);

    if (value.kind == ParseValue::Kind::Generator
        || value.kind == ParseValue::Kind::SignalChain)
        return true;

    if (value.kind == ParseValue::Kind::PolyList) {
        for (const auto& slot : value.list)
            if (! validateValueArgDoesNotLookMalformedNumeric(slot, context, error))
                return false;
        return true;
    }

    if (value.kind != ParseValue::Kind::String)
        return true;

    const auto text = value.str.trim();
    const auto lowerText = text.toLowerCase();
    double strict = 0.0;
    if (isNoteLike(text) || isStrictFrequencyString(text)
        || (parseStrictDoubleLiteral(text, &strict) && std::isfinite(strict)))
        return true;

    if (! stringStartsLikeNumber(text)
        && ! lowerText.endsWith("hz")
        && ! lowerText.endsWith("khz"))
        return true;

    error = context + " value argument '" + text
          + "' is not a strict numeric/unit, note, frequency, dynamic, or symbolic value";
    return false;
}

static bool validateIndexedValueArg(const std::vector<ParseValue>& args,
                                    size_t index,
                                    const juce::String& context,
                                    juce::String& error)
{
    if (index >= args.size())
        return true;
    return validateValueArgDoesNotLookMalformedNumeric(args[index], context, error);
}

static bool splitSignalArgs(const juce::String& body, std::vector<juce::String>& argSources,
                            juce::String& error)
{
    juce::String current;
    int parens = 0;
    int brackets = 0;
    int braces = 0;
    for (int i = 0; i < body.length(); ++i) {
        const auto c = body[i];
        if (c == '(') ++parens;
        else if (c == ')') --parens;
        else if (c == '[') ++brackets;
        else if (c == ']') --brackets;
        else if (c == '{') ++braces;
        else if (c == '}') --braces;

        if (parens < 0 || brackets < 0 || braces < 0) {
            error = "Unbalanced signal expression args";
            return false;
        }

        if (c == ',' && parens == 0 && brackets == 0 && braces == 0) {
            const auto arg = current.trim();
            if (arg.isEmpty()) {
                error = "Empty signal args do not lower in the VM-first subset";
                return false;
            }
            argSources.push_back(arg);
            current = {};
        } else {
            current << juce::String::charToString(c);
        }
    }
    if (parens != 0 || brackets != 0 || braces != 0) {
        error = "Unbalanced signal expression args";
        return false;
    }
    const auto tail = current.trim();
    if (tail.isNotEmpty())
        argSources.push_back(tail);
    else if (body.trim().isNotEmpty() && body.trim().endsWithChar(',')) {
        error = "Empty signal args do not lower in the VM-first subset";
        return false;
    }
    return true;
}

static bool parseSignalCall(const SignalExpr& expr, juce::String& name,
                            std::vector<juce::String>& argSources, juce::String& error)
{
    if (expr.chain.size() != 1)
        return false;

    const auto source = expr.source.trim();
    const int lp = source.indexOfChar('(');
    if (lp <= 0 || ! source.endsWithChar(')'))
        return false;

    name = source.substring(0, lp).trim();
    const auto body = source.substring(lp + 1, source.length() - 1);
    return splitSignalArgs(body, argSources, error);
}

static bool parseSignalCallSource(const juce::String& source, juce::String& name,
                                  std::vector<juce::String>& argSources, juce::String& error)
{
    SignalExpr single;
    single.source = source.trim();
    single.chain.push_back(single.source);
    return parseSignalCall(single, name, argSources, error);
}

static bool parseLowerableSignalValue(const SignalExpr& expr, ParseValue& value,
                                      juce::String& error);

static bool parseSignalArgValue(const juce::String& argSource, ParseValue& value,
                                juce::String& error)
{
    SignalExpr expr = makeSignalExprFromSource(argSource);
    return parseLowerableSignalValue(expr, value, error);
}

static bool parsePolyListSignalValue(const SignalExpr& expr, ParseValue& value,
                                     juce::String& error)
{
    if (expr.chain.size() != 1)
        return false;

    const auto source = expr.source.trim();
    if (! source.startsWithChar('{') || ! source.endsWithChar('}'))
        return false;

    std::vector<juce::String> argSources;
    if (! splitSignalArgs(source.substring(1, source.length() - 1), argSources, error))
        return false;
    if (argSources.empty()) {
        error = "Polyphonic expression list needs at least one slot";
        return false;
    }

    value.kind = ParseValue::Kind::PolyList;
    for (const auto& argSource : argSources) {
        ParseValue slot;
        if (! parseSignalArgValue(argSource, slot, error))
            return false;
        if (slot.kind == ParseValue::Kind::Null) {
            error = "Empty polyphonic expression slot does not lower";
            return false;
        }
        value.list.push_back(std::move(slot));
    }
    return true;
}

static bool parseSignalChainTransformOp(const juce::String& source,
                                        SignalChainOp& op,
                                        juce::String& error)
{
    juce::String name;
    std::vector<juce::String> argSources;
    if (! parseSignalCallSource(source, name, argSources, error))
        return false;

    auto lname = name.toLowerCase();
    if (lname == "slew")
        lname = "smooth";

    const auto arityOk = [&]() {
        if (lname == "clip" || lname == "scale") return argSources.size() == 2;
        if (lname == "offset" || lname == "gain" || lname == "smooth"
            || lname == "transpose" || lname == "octave" || lname == "quantize")
            return argSources.size() == 1;
        if (lname == "invert") return argSources.size() <= 1;
        if (lname == "abs") return argSources.empty();
        return false;
    };

    if (! isCataloguedV2Name("transforms", lname)) {
        error = "Unsupported signal-chain transform '" + lname + "' in the VM-first subset";
        return false;
    }
    if (! arityOk()) {
        error = "Wrong number of args for signal-chain transform '" + lname + "'";
        return false;
    }

    op.name = lname;
    for (const auto& argSource : argSources) {
        ParseValue arg;
        if (! parseSignalArgValue(argSource, arg, error))
            return false;
        if (arg.kind == ParseValue::Kind::Null) {
            error = "Empty signal-chain args do not lower in the VM-first subset";
            return false;
        }
        op.args.push_back(std::move(arg));
    }
    if (lname == "smooth"
        && ! validateIndexedTimeArg(op.args, 0, "smooth(...)", error))
        return false;
    if (lname == "scale") {
        const auto isNumericRangeString = [] (const juce::String& raw) {
            const auto s = raw.toLowerCase();
            return s.endsWith("hz") || s.endsWith("khz") || isNoteLike(raw);
        };
        const bool musicalScale =
            op.args.size() >= 2
            && op.args[1].kind == ParseValue::Kind::String
            && ! isNumericRangeString(op.args[1].str);
        for (const auto& arg : op.args) {
            if (arg.kind == ParseValue::Kind::String) {
                const auto s = arg.str.toLowerCase();
                if (! musicalScale && ! isNumericRangeString(arg.str)) {
                    error = "Musical scale signal quantizers do not lower in the VM-first subset";
                    return false;
                }
            }
        }
    }
    return true;
}

static bool parseInputSignalValue(const SignalExpr& expr, ParseValue& value,
                                  juce::String& error)
{
    if (expr.inputReads.size() != 1 || expr.chain.size() != 1) {
        error = "Input socket reads need a single input source in the VM-first subset";
        return false;
    }
    const auto compact = expr.chain.front().removeCharacters(" \t\r\n");
    const auto expected = "<" + expr.inputReads.front();
    if (compact != expected) {
        error = "Input socket read source does not match parsed input name";
        return false;
    }

    auto gen = std::make_shared<GeneratorNode>();
    gen->generatorType = "__input";
    ParseValue name;
    name.kind = ParseValue::Kind::String;
    name.str = expr.inputReads.front();
    gen->args.push_back(std::move(name));

    value.kind = ParseValue::Kind::Generator;
    value.gen = std::move(gen);
    return true;
}

static bool parseGeneratorSignalValue(const SignalExpr& expr, ParseValue& value,
                                      juce::String& error)
{
    juce::String name;
    std::vector<juce::String> argSources;
    if (! parseSignalCall(expr, name, argSources, error))
        return false;

    const auto lname = name.toLowerCase();
    if (! isLowerableVmGenerator(lname)) {
        error = "Only VM param-lane generator expressions lower in the VM-first subset";
        return false;
    }

    auto gen = std::make_shared<GeneratorNode>();
    gen->generatorType = lname;
    for (const auto& argSource : argSources) {
        ParseValue arg;
        if (! parseSignalArgValue(argSource, arg, error))
            return false;
        if (arg.kind == ParseValue::Kind::Null) {
            error = "Empty generator args do not lower in the VM-first subset";
            return false;
        }
        gen->args.push_back(std::move(arg));
    }
    if (lname == "ad") {
        if (! validateIndexedTimeArg(gen->args, 0, "ad(...) attack", error)
            || ! validateIndexedTimeArg(gen->args, 1, "ad(...) decay", error))
            return false;
        if (! validateIndexedValueArg(gen->args, 2, "ad(...) low", error)
            || ! validateIndexedValueArg(gen->args, 3, "ad(...) high", error))
            return false;
    } else if (lname == "adsr") {
        if (! validateIndexedTimeArg(gen->args, 0, "adsr(...) attack", error)
            || ! validateIndexedTimeArg(gen->args, 1, "adsr(...) decay", error)
            || ! validateIndexedTimeArg(gen->args, 3, "adsr(...) release", error))
            return false;
        if (! validateIndexedValueArg(gen->args, 2, "adsr(...) sustain", error)
            || ! validateIndexedValueArg(gen->args, 4, "adsr(...) low", error)
            || ! validateIndexedValueArg(gen->args, 5, "adsr(...) high", error))
            return false;
    } else if (lname == "glide") {
        if (! validateIndexedTimeArg(gen->args, 0, "glide(...)", error))
            return false;
    } else if (lname == "random") {
        if (! validateIndexedValueArg(gen->args, 0, "random(...) low", error)
            || ! validateIndexedValueArg(gen->args, 1, "random(...) high", error)
            || ! validateIndexedTimeArg(gen->args, 2, "random(...) slew", error))
            return false;
    } else if (lname == "lfo") {
        if (! validateIndexedTimeArg(gen->args, 1, "lfo(...) rate", error)
            || ! validateIndexedValueArg(gen->args, 2, "lfo(...) low", error)
            || ! validateIndexedValueArg(gen->args, 3, "lfo(...) high", error)
            || ! validateIndexedValueArg(gen->args, 4, "lfo(...) phase", error))
            return false;
    } else if (lname == "auto") {
        for (size_t i = 0; i < gen->args.size(); ++i) {
            const auto& arg = gen->args[i];
            if (arg.kind == ParseValue::Kind::String) {
                const auto curve = arg.str.toLowerCase();
                if (curve == "lin" || curve == "exp" || curve == "log"
                    || curve == "eqpow")
                    continue;
            }
            if (! validateIndexedValueArg(gen->args, i, "auto(...)", error))
                return false;
        }
    } else if (lname == "bernoulli") {
        if (! validateIndexedValueArg(gen->args, 0, "bernoulli(...) weight", error)
            || ! validateIndexedValueArg(gen->args, 1, "bernoulli(...) low", error)
            || ! validateIndexedValueArg(gen->args, 2, "bernoulli(...) high", error))
            return false;
    } else if (lname == "deviate") {
        if (! validateIndexedValueArg(gen->args, 0, "deviate(...) amount", error))
            return false;
    } else if (lname == "keytrack") {
        if (! validateIndexedValueArg(gen->args, 0, "keytrack(...) low", error)
            || ! validateIndexedValueArg(gen->args, 1, "keytrack(...) high", error))
            return false;
    } else if (lname == "accum") {
        if (! validateIndexedValueArg(gen->args, 0, "accum(...) start", error)
            || ! validateIndexedValueArg(gen->args, 1, "accum(...) increment", error)
            || ! validateIndexedValueArg(gen->args, 2, "accum(...) ceiling", error))
            return false;
    }

    value.kind = ParseValue::Kind::Generator;
    value.gen = std::move(gen);
    return true;
}

static bool parseSignalChainValue(const SignalExpr& expr, ParseValue& value,
                                  juce::String& error)
{
    if (expr.chain.size() <= 1)
        return false;

    ParseValue sourceValue;
    SignalExpr sourceExpr;
    sourceExpr.source = expr.chain.front().trim();
    sourceExpr.chain.push_back(sourceExpr.source);
    if (sourceExpr.source.startsWithChar('<')) {
        sourceExpr.inputReads = expr.inputReads;
        if (! parseInputSignalValue(sourceExpr, sourceValue, error))
            return false;
    } else {
        if (! parseGeneratorSignalValue(sourceExpr, sourceValue, error))
            return false;
    }

    auto chain = std::make_shared<SignalChainNode>();
    chain->source = sourceValue.gen;

    for (size_t i = 1; i < expr.chain.size(); ++i) {
        SignalChainOp op;
        if (! parseSignalChainTransformOp(expr.chain[i], op, error))
            return false;
        chain->ops.push_back(std::move(op));
    }

    value.kind = ParseValue::Kind::SignalChain;
    value.signalChain = std::move(chain);
    return true;
}

static bool parseLowerableSignalValue(const SignalExpr& expr, ParseValue& value,
                                      juce::String& error)
{
    if (parsePolyListSignalValue(expr, value, error))
        return true;
    if (expr.isSimpleValue()) {
        if (! expr.inputReads.empty()) {
            return parseInputSignalValue(expr, value, error);
        }
        value = parseSimpleValue(expr.source);
        return value.kind != ParseValue::Kind::Null
            && validateFiniteSimpleValue(value, expr.source, error);
    }
    if (expr.chain.size() != 1)
        return parseSignalChainValue(expr, value, error);
    if (! expr.inputReads.empty())
        return parseInputSignalValue(expr, value, error);
    return parseGeneratorSignalValue(expr, value, error);
}

static bool isSimpleSocketIdentityRoute(const Statement& stmt)
{
    if (stmt.kind != StatementKind::SocketRoute
        || stmt.body.inputReads.size() != 1
        || stmt.body.chain.size() != 1)
        return false;

    const auto compact = stmt.body.source.removeCharacters(" \t\r\n");
    return compact == "<" + stmt.body.inputReads.front();
}

static bool isSupportedSocketRouteExpr(const SignalExpr& expr)
{
    if (expr.inputReads.size() != 1 || expr.chain.empty())
        return false;

    const auto compact = expr.chain.front().removeCharacters(" \t\r\n");
    if (compact != "<" + expr.inputReads.front())
        return false;

    std::function<bool(const juce::String&)> socketRouteArgSupported;
    const auto routeScalarArg = [](const juce::String& raw) {
        const auto t = raw.trim();
        if (parseStrictRouteNumberLiteral(t))
            return true;

        const auto lower = t.toLowerCase();
        const auto unitNumber = [&](const char* suffix) {
            const juce::String s(suffix);
            return lower.endsWith(s)
                && t.length() > s.length()
                && parseStrictRouteNumberLiteral(t.dropLastCharacters(s.length()));
        };
        const auto positivePitchUnit = [&](const juce::String& value) {
            const auto pitchText = value.trim();
            const auto pitchLower = pitchText.toLowerCase();
            int suffixLength = 0;
            if (pitchLower.endsWith("khz"))
                suffixLength = 3;
            else if (pitchLower.endsWith("hz"))
                suffixLength = 2;
            else
                return false;

            if (pitchText.length() <= suffixLength)
                return false;

            float number = 0.0f;
            return parseStrictRouteNumberLiteral(pitchText.dropLastCharacters(suffixLength), &number)
                && number > 0.0f;
        };
        if (positivePitchUnit(t)
            || unitNumber("%")
            || unitNumber("ms")
            || unitNumber("steps")
            || unitNumber("step")
            || unitNumber("beats")
            || unitNumber("beat")
            || unitNumber("s")
            || isNoteLike(t))
            return true;

        ParseValue value;
        juce::String error;
        if (! parseSignalArgValue(raw, value, error))
            return false;
        if (value.kind == ParseValue::Kind::Number)
            return true;
        if (value.kind == ParseValue::Kind::UnitNumber)
            return true;
        if (value.kind == ParseValue::Kind::String) {
            const auto valueLower = value.str.toLowerCase();
            if (valueLower.endsWith("hz") || valueLower.endsWith("khz"))
                return positivePitchUnit(value.str);
            return isNoteLike(value.str);
        }
        return false;
    };
    socketRouteArgSupported = [&] (const juce::String& raw) -> bool {
        if (routeScalarArg(raw))
            return true;
        const SignalExpr argExpr = makeSignalExprFromSource(raw);
        if (argExpr.chain.size() > 1) {
            if (! socketRouteArgSupported(argExpr.chain.front()))
                return false;
            for (size_t i = 1; i < argExpr.chain.size(); ++i) {
                juce::String opName;
                std::vector<juce::String> opArgs;
                juce::String opError;
                if (! parseSignalCallSource(argExpr.chain[i], opName, opArgs, opError))
                    return false;
                const auto lname = opName.toLowerCase();
                const auto argc = opArgs.size();
                const bool arityOk =
                    (lname == "invert" && argc <= 1)
                    || (lname == "abs" && argc == 0)
                    || ((lname == "gain" || lname == "offset") && argc == 1)
                    || ((lname == "clip" || lname == "scale") && argc == 2);
                if (! arityOk)
                    return false;
                for (const auto& opArg : opArgs)
                    if (! socketRouteArgSupported(opArg))
                        return false;
            }
            return true;
        }
        if (argExpr.inputReads.size() == 1 && argExpr.chain.size() == 1) {
            const auto compact = argExpr.chain.front().removeCharacters(" \t\r\n");
            return compact == "<" + argExpr.inputReads.front();
        }
        juce::String name;
        std::vector<juce::String> args;
        juce::String error;
        if (! parseSignalCall(argExpr, name, args, error))
            return false;
        const auto lname = name.toLowerCase();
        if (lname == "lfo") {
            if (args.size() != 4)
                return false;
            return socketRouteArgSupported(args[1])
                && socketRouteArgSupported(args[2])
                && socketRouteArgSupported(args[3]);
        }
        return false;
    };

    for (size_t i = 1; i < expr.chain.size(); ++i) {
        juce::String name;
        std::vector<juce::String> args;
        juce::String error;
        if (! parseSignalCallSource(expr.chain[i], name, args, error))
            return false;

        const auto lname = name.toLowerCase();
        const auto argc = args.size();
        const auto numeric = [](const juce::String& raw) {
            return parseStrictRouteNumberLiteral(raw);
        };
        const auto timeArg = [&](const juce::String& raw) {
            const auto t = raw.trim().toLowerCase();
            if (t.endsWith("ms")) return numeric(t.dropLastCharacters(2));
            if (t.endsWith("s")) return numeric(t.dropLastCharacters(1));
            return numeric(t);
        };
        if (lname == "invert" && (argc == 0 || (argc == 1 && socketRouteArgSupported(args[0])))) continue;
        if (lname == "abs" && argc == 0) continue;
        if ((lname == "gain" || lname == "offset") && argc == 1
            && socketRouteArgSupported(args[0])) continue;
        if ((lname == "transpose" || lname == "octave") && argc == 1
            && numeric(args[0])) continue;
        if ((lname == "clip" || lname == "scale") && argc == 2
            && socketRouteArgSupported(args[0]) && socketRouteArgSupported(args[1])) continue;
        if ((lname == "smooth" || lname == "slew") && argc == 1
            && (timeArg(args[0]) || socketRouteArgSupported(args[0]))) continue;
        return false;
    }
    return true;
}

static bool socketRouteExprHasDynamicArgs(const SignalExpr& expr)
{
    if (expr.inputReads.size() != 1 || expr.chain.empty())
        return false;

    const auto compact = expr.chain.front().removeCharacters(" \t\r\n");
    if (compact != "<" + expr.inputReads.front())
        return false;

    for (size_t i = 1; i < expr.chain.size(); ++i) {
        juce::String name;
        std::vector<juce::String> args;
        juce::String error;
        if (! parseSignalCallSource(expr.chain[i], name, args, error))
            continue;

        for (const auto& arg : args) {
            ParseValue value;
            juce::String argError;
            if (! parseSignalArgValue(arg, value, argError))
                continue;
            if (value.kind == ParseValue::Kind::Generator
                || value.kind == ParseValue::Kind::SignalChain)
                return true;
        }
    }

    return false;
}

static bool isSupportedSocketRoute(const Statement& stmt)
{
    ChannelRoute channelRoute;
    juce::String channelError;
    return stmt.kind == StatementKind::SocketRoute
        && (parseChannelRoute(stmt, channelRoute, channelError)
            || isSupportedSocketRouteExpr(stmt.body));
}

struct FunctionDef {
    std::vector<juce::String> params;
    SignalExpr body;
};

class Lowerer {
public:
    explicit Lowerer(double bpm = 120.0)
        : bpm_(bpm > 0.0 ? bpm : 120.0)
    {
    }

    LowerResult lower(const Program& p)
    {
        out.diagnostics.clear();
        functions.clear();
        definitions.clear();

        for (const auto& stmt : p.statements)
            if (stmt.kind == StatementKind::Function)
                functions[stmt.name] = FunctionDef{ stmt.params, stmt.body };

        std::unordered_set<juce::String> routedDefinitions;
        for (const auto& stmt : p.statements) {
            if (stmt.kind == StatementKind::Route) {
                if (stmt.defineName.isNotEmpty())
                    routedDefinitions.insert(stmt.defineName);
                if (stmt.material.kind == MaterialKind::Ref)
                    routedDefinitions.insert(stmt.material.refName);
            } else if (stmt.kind == StatementKind::SocketExport
                       && stmt.material.kind == MaterialKind::Ref) {
                routedDefinitions.insert(stmt.material.refName);
            }
        }

        auto outSequence = makeSequence();
        auto concurrentRoot = makeStep();
        bool hasRoute = false;
        const auto selfFanoutTargets = collectSelfFanoutTargets(p);
        std::vector<PostfixOp> selfGlobalPostfixes;
        collectSelfGlobalPostfixes(p, selfGlobalPostfixes);
        if (! selfGlobalPostfixes.empty() && selfFanoutTargets.empty())
            addUnsupported("V2_SELF_NO_TARGETS",
                           "self processor routes need at least one concrete route target to fan out to");

        for (const auto& stmt : p.statements) {
            switch (stmt.kind) {
                case StatementKind::Definition:
                    definitions[stmt.name] = stmt.material;
                    addDefinitionAssignment(stmt.name, stmt.material,
                                            routedDefinitions.count(stmt.name) != 0);
                    break;
                case StatementKind::Route:
                    if (stmt.defineName.isNotEmpty()) {
                        definitions[stmt.defineName] = stmt.material;
                        addDefinitionAssignment(stmt.defineName, stmt.material, true);
                    }
                    if (isSelfRoute(stmt)) {
                        if (isSelfProcessorOnlyRoute(stmt))
                            break;
                        if (appendSelfRoute(stmt, *concurrentRoot, selfFanoutTargets, selfGlobalPostfixes))
                            hasRoute = true;
                    }
                    else if (appendRouteBranch(withAdditionalPostfixes(stmt, selfGlobalPostfixes),
                                               *concurrentRoot))
                        hasRoute = true;
                    break;
                case StatementKind::Function:
                    break;
                case StatementKind::SocketExport:
                    if (appendSocketExportBranch(stmt, *concurrentRoot))
                        hasRoute = true;
                    break;
                case StatementKind::SocketRoute:
                    if (! isSupportedSocketRoute(stmt)) {
                        ChannelRoute channelRoute;
                        juce::String channelError;
                        if (isChannelRouteSyntax(stmt)) {
                            parseChannelRoute(stmt, channelRoute, channelError);
                            addUnsupported("V2_INVALID_CHANNEL_ROUTE",
                                           channelError.isNotEmpty()
                                               ? channelError
                                               : "Invalid channels(...) route");
                            break;
                        }
                        const bool dynamicRouteArg = socketRouteExprHasDynamicArgs(stmt.body);
                        addUnsupported(dynamicRouteArg
                                           ? "V2_UNSUPPORTED_SOCKET_ROUTE_DYNAMIC_ARG"
                                           : "V2_UNSUPPORTED_SOCKET_ROUTE",
                                       dynamicRouteArg
                                           ? "Socket route transform arguments accept scalar values only until routes are VM-backed"
                                           : "Only simple or supported processed socket routes lower in the VM-first subset");
                    }
                    break;
                case StatementKind::SessionTarget:
                    appendSessionTarget(stmt.name, stmt.body, 0);
                    break;
                case StatementKind::Unknown:
                    addUnsupported("V2_UNSUPPORTED_STATEMENT",
                                   "Unknown V2 statement cannot lower");
                    break;
            }
        }

        if (hasRoute) {
            if (concurrentRoot->events.size() == 1
                && concurrentRoot->events.front() != nullptr
                && concurrentRoot->events.front()->type == "Sequence") {
                auto branch = std::move(concurrentRoot->events.front());
                for (auto& step : branch->steps)
                    outSequence->steps.push_back(std::move(step));
            } else {
                outSequence->steps.push_back(std::move(concurrentRoot));
            }
            out.ast.statements.push_back(makeAssignment("out", std::move(outSequence)));
        }

        std::sort(out.ast.variableNames.begin(), out.ast.variableNames.end(),
                  [](const juce::String& a, const juce::String& b) { return a.compare(b) < 0; });
        resolveTimings(out.ast);
        out.ast.shape = computeShapeFingerprint(out.ast);
        return std::move(out);
    }

private:
    LowerResult out;
    std::unordered_map<juce::String, Material> definitions;
    std::unordered_map<juce::String, FunctionDef> functions;

    void addUnsupported(juce::String code, juce::String message)
    {
        out.diagnostics.push_back(Diagnostic{ "error", std::move(code), std::move(message), 0, 0 });
    }

    bool appendRouteBranch(const Statement& stmt, AstNode& rootStep)
    {
        auto branch = makeSequence();
        if (! appendRoute(stmt, *branch))
            return false;
        rootStep.events.push_back(std::move(branch));
        return true;
    }

    static bool isSelfTarget(const juce::String& target)
    {
        return target.trim().equalsIgnoreCase("self");
    }

    static bool isSelfRoute(const Statement& stmt)
    {
        if (stmt.kind != StatementKind::Route)
            return false;
        for (const auto& target : stmt.targets)
            if (isSelfTarget(target))
                return true;
        return false;
    }

    static std::vector<juce::String> collectSelfFanoutTargets(const Program& p)
    {
        std::vector<juce::String> targets;
        std::set<juce::String> seen;
        for (const auto& stmt : p.statements) {
            if (stmt.kind != StatementKind::Route)
                continue;
            for (const auto& target : stmt.targets) {
                if (isSelfTarget(target))
                    continue;
                const auto key = target.trim().toLowerCase();
                if (key.isEmpty() || seen.count(key) != 0)
                    continue;
                seen.insert(key);
                targets.push_back(target.trim());
            }
        }
        return targets;
    }

    bool appendSelfRoute(const Statement& stmt,
                         AstNode& rootStep,
                         const std::vector<juce::String>& fanoutTargets,
                         const std::vector<PostfixOp>& globalPostfixes)
    {
        if (stmt.targets.size() != 1) {
            addUnsupported("V2_UNSUPPORTED_SELF_MIXED_TARGET",
                           "self must be the only target in a self route");
            return false;
        }

        if (fanoutTargets.empty()) {
            addUnsupported("V2_SELF_NO_TARGETS",
                           "self routes need at least one concrete route target to fan out to");
            return false;
        }

        Material loweredMaterial;
        if (! normalizeMaterialForUse(stmt.material, loweredMaterial))
            return false;
        if (! validatePostfixes(loweredMaterial))
            return false;

        Statement fanout = stmt;
        fanout.targets = fanoutTargets;
        fanout = withAdditionalPostfixes(fanout, globalPostfixes);
        return appendRouteBranch(fanout, rootStep);
    }

    bool isSelfProcessorOnlyRoute(const Statement& stmt)
    {
        if (! isSelfRoute(stmt))
            return false;

        Material loweredMaterial;
        if (! normalizeMaterialForUse(stmt.material, loweredMaterial))
            return false;
        if (! validatePostfixes(loweredMaterial))
            return false;

        return ! materialHasContent(loweredMaterial)
            && (hasStructuralPostfix(loweredMaterial)
                || hasWholeScopeGroupReceiverPostfix(loweredMaterial));
    }

    void collectSelfGlobalPostfixes(const Program& p, std::vector<PostfixOp>& outPostfixes)
    {
        for (const auto& stmt : p.statements) {
            if (! isSelfRoute(stmt))
                continue;
            if (stmt.targets.size() != 1)
                continue;

            Material loweredMaterial;
            if (! normalizeMaterialForUse(stmt.material, loweredMaterial))
                continue;
            if (! validatePostfixes(loweredMaterial))
                continue;
            if (materialHasContent(loweredMaterial))
                continue;
            if (! hasStructuralPostfix(loweredMaterial)
                && ! hasWholeScopeGroupReceiverPostfix(loweredMaterial))
                continue;

            outPostfixes.insert(outPostfixes.end(),
                                loweredMaterial.postfix.begin(),
                                loweredMaterial.postfix.end());
        }
    }

    static Statement withAdditionalPostfixes(Statement stmt,
                                             const std::vector<PostfixOp>& postfixes)
    {
        if (! postfixes.empty())
            stmt.material.postfix.insert(stmt.material.postfix.end(),
                                         postfixes.begin(),
                                         postfixes.end());
        return stmt;
    }

    bool appendSocketExportBranch(const Statement& stmt, AstNode& rootStep)
    {
        auto branch = makeSequence();
        if (! appendSocketExport(stmt, *branch))
            return false;
        rootStep.events.push_back(std::move(branch));
        return true;
    }

    bool parseReceiverValue(const SignalExpr& expr, ParseValue& value, juce::String& error)
    {
        if (isStepSelectorExpression(expr))
            return parseStepSelectorValue(expr, {}, value, error);

        {
            juce::String callName;
            std::vector<juce::String> callArgs;
            juce::String callError;
            if (parseSignalCall(expr, callName, callArgs, callError)
                && callName.equalsIgnoreCase("markov")) {
                error = "V2_UNSUPPORTED_MARKOV_PROJECTION: Markov projected streams inside receiver expressions need projected-state runtime support";
                return false;
            }
        }

        if (parseLowerableSignalValue(expr, value, error))
            return true;

        if (expr.chain.size() > 1) {
            SignalExpr head;
            head.source = expr.chain.front().trim();
            head.chain.push_back(head.source);

            SignalExpr expandedHead;
            juce::String functionName;
            juce::String expandError;
            if (expandFunctionCall(head, expandedHead, functionName, expandError)) {
                juce::String expandedSource = expandedHead.source;
                for (size_t i = 1; i < expr.chain.size(); ++i)
                    expandedSource << " : " << expr.chain[i];

                SignalExpr expanded = makeSignalExprFromSource(expandedSource);
                juce::String expandedError;
                if (parseLowerableSignalValue(expanded, value, expandedError))
                    return true;

                error = "Function '" + functionName + "' expands to unsupported signal expression '"
                    + expanded.source + "'"
                    + (expandedError.isEmpty() ? juce::String() : ": " + expandedError);
                return false;
            }
        }

        SignalExpr expanded;
        juce::String functionName;
        juce::String expandError;
        if (! expandFunctionCall(expr, expanded, functionName, expandError)) {
            if (expandError.isNotEmpty())
                error = expandError;
            return false;
        }

        juce::String expandedError;
        if (! parseLowerableSignalValue(expanded, value, expandedError)) {
            error = "Function '" + functionName + "' expands to unsupported signal expression '"
                + expanded.source + "'"
                + (expandedError.isEmpty() ? juce::String() : ": " + expandedError);
            return false;
        }
        return true;
    }

    bool parseMarkovProjectedValueForTarget(const juce::String& target,
                                            const SignalExpr& expr,
                                            ParseValue& value,
                                            juce::String& error)
    {
        juce::String callName;
        std::vector<juce::String> args;
        juce::String callError;
        if (! parseSignalCall(expr, callName, args, callError)
            || ! callName.equalsIgnoreCase("markov"))
            return false;

        if (args.size() != 2) {
            error = "V2_UNSUPPORTED_MARKOV_PROJECTION: markov(...) projected value expects corpus projection and order";
            return false;
        }

        const auto corpusProjection = args[0].trim();
        const int projectionDot = corpusProjection.lastIndexOfChar('.');
        if (projectionDot <= 0
            || corpusProjection.containsChar(':')
            || corpusProjection.startsWithChar('[')
            || corpusProjection.startsWithChar('(')) {
            error = "V2_UNSUPPORTED_MARKOV_PROJECTION: projected value Markov needs a simple corpus stream such as part.cutoff";
            return false;
        }

        const auto base = corpusProjection.substring(0, projectionDot).trim();
        const auto projection = corpusProjection.substring(projectionDot + 1).trim();
        if (base.isEmpty() || projection.isEmpty()) {
            error = "V2_UNSUPPORTED_MARKOV_PROJECTION: projected value Markov needs base and stream names";
            return false;
        }
        if (! projection.equalsIgnoreCase(target.trim())) {
            error = "V2_UNSUPPORTED_MARKOV_PROJECTION: receiver target '"
                + target + "' cannot consume Markov projection '" + projection + "'";
            return false;
        }

        ParseValue orderValue;
        if (! parseSignalArgValue(args[1], orderValue, error)) {
            error = "V2_UNSUPPORTED_MARKOV_ORDER: markov(...) order argument is not lowerable"
                + (error.isEmpty() ? juce::String() : ": " + error);
            return false;
        }
        if (isDynamicParseValue(orderValue)) {
            error = "V2_UNSUPPORTED_MARKOV_ORDER_DYNAMIC: markov(...) order must be static until runtime transition-table order changes are designed";
            return false;
        }
        int order = 0;
        if (! staticPositiveIntegerValue(orderValue, order)) {
            error = "V2_UNSUPPORTED_MARKOV_ORDER: markov(...) order must be a static positive integer";
            return false;
        }
        if (order != 1) {
            error = "V2_UNSUPPORTED_MARKOV_ORDER: Only order-1 markov(...) lowers in this VM slice";
            return false;
        }

        Program parsed = parseMaterialExpression("__markov_projection_tmp", base);
        if (! parsed.diagnostics.empty()) {
            error = "V2_UNSUPPORTED_MARKOV_CORPUS: Markov corpus did not parse: "
                + parsed.diagnostics.front().message;
            return false;
        }
        if (parsed.statements.size() != 1
            || parsed.statements.front().kind != StatementKind::Definition) {
            error = "V2_UNSUPPORTED_MARKOV_CORPUS: Markov first argument must be material";
            return false;
        }

        Material material;
        if (! normalizeMaterialForUse(parsed.statements.front().material, material)) {
            if (error.isEmpty())
                error = "V2_UNSUPPORTED_MARKOV_CORPUS: Markov corpus material did not normalize";
            return false;
        }
        if (material.steps.empty()) {
            error = "V2_UNSUPPORTED_MARKOV_CORPUS: Markov corpus has no states";
            return false;
        }

        auto gen = std::make_shared<GeneratorNode>();
        gen->generatorType = "__markov_value";
        for (int i = 0; i < (int) material.steps.size(); ++i) {
            ParseValue stateValue;
            if (! extractStepSelectedReceiverValue(material, i, projection,
                                                   stateValue, error)) {
                error = "V2_UNSUPPORTED_MARKOV_PROJECTION: Markov projected value corpus does not lower"
                    + (error.isEmpty() ? juce::String() : ": " + error);
                return false;
            }
            gen->args.push_back(std::move(stateValue));
        }

        value.kind = ParseValue::Kind::Generator;
        value.gen = std::move(gen);
        return true;
    }

    bool parseReceiverValueForTarget(const juce::String& target, const SignalExpr& expr,
                                     ParseValue& value, juce::String& error)
    {
        if (isStepSelectorExpression(expr))
            return parseStepSelectorValue(expr, target, value, error);
        {
            juce::String callName;
            std::vector<juce::String> callArgs;
            juce::String callError;
            if (parseSignalCall(expr, callName, callArgs, callError)
                && callName.equalsIgnoreCase("markov")) {
                if (parseMarkovProjectedValueForTarget(target, expr, value, error))
                    return true;
                return false;
            }
        }
        if (! parseReceiverValue(expr, value, error))
            return false;
        return validateReceiverStaticValue(value, target, error);
    }

    static bool containsPolyList(const ParseValue& value)
    {
        if (value.kind == ParseValue::Kind::PolyList)
            return true;
        if (value.kind == ParseValue::Kind::Generator && value.gen) {
            for (const auto& arg : value.gen->args)
                if (containsPolyList(arg))
                    return true;
            for (const auto& named : value.gen->named)
                if (containsPolyList(named.second))
                    return true;
        }
        if (value.kind == ParseValue::Kind::SignalChain && value.signalChain) {
            if (value.signalChain->source) {
                ParseValue source;
                source.kind = ParseValue::Kind::Generator;
                source.gen = value.signalChain->source;
                if (containsPolyList(source))
                    return true;
            }
            for (const auto& op : value.signalChain->ops)
                for (const auto& arg : op.args)
                    if (containsPolyList(arg))
                        return true;
        }
        if (value.kind == ParseValue::Kind::Cond && value.cond)
            return containsPolyList(value.cond->expr);
        return false;
    }

    static int polySlotCount(const ParseValue& value)
    {
        int n = 0;
        if (value.kind == ParseValue::Kind::PolyList)
            n = std::max(n, (int) value.list.size());
        if (value.kind == ParseValue::Kind::Generator && value.gen) {
            for (const auto& arg : value.gen->args)
                n = std::max(n, polySlotCount(arg));
            for (const auto& named : value.gen->named)
                n = std::max(n, polySlotCount(named.second));
        }
        if (value.kind == ParseValue::Kind::SignalChain && value.signalChain) {
            if (value.signalChain->source) {
                ParseValue source;
                source.kind = ParseValue::Kind::Generator;
                source.gen = value.signalChain->source;
                n = std::max(n, polySlotCount(source));
            }
            for (const auto& op : value.signalChain->ops)
                for (const auto& arg : op.args)
                    n = std::max(n, polySlotCount(arg));
        }
        if (value.kind == ParseValue::Kind::Cond && value.cond)
            n = std::max(n, polySlotCount(value.cond->expr));
        return n;
    }

    static ParseValue selectPolySlot(ParseValue value, int slot)
    {
        if (value.kind == ParseValue::Kind::PolyList) {
            if (value.list.empty())
                return {};
            return selectPolySlot(value.list[(size_t) (slot % (int) value.list.size())], slot);
        }
        if (value.kind == ParseValue::Kind::Generator && value.gen) {
            auto gen = std::make_shared<GeneratorNode>(*value.gen);
            for (auto& arg : gen->args)
                arg = selectPolySlot(arg, slot);
            for (auto& named : gen->named)
                named.second = selectPolySlot(named.second, slot);
            value.gen = std::move(gen);
            return value;
        }
        if (value.kind == ParseValue::Kind::SignalChain && value.signalChain) {
            auto chain = std::make_shared<SignalChainNode>(*value.signalChain);
            if (chain->source) {
                ParseValue source;
                source.kind = ParseValue::Kind::Generator;
                source.gen = chain->source;
                source = selectPolySlot(std::move(source), slot);
                chain->source = source.gen;
            }
            for (auto& op : chain->ops)
                for (auto& arg : op.args)
                    arg = selectPolySlot(arg, slot);
            value.signalChain = std::move(chain);
            return value;
        }
        if (value.kind == ParseValue::Kind::Cond && value.cond) {
            auto cond = std::make_shared<CondNode>(*value.cond);
            cond->expr = selectPolySlot(cond->expr, slot);
            value.cond = std::move(cond);
            return value;
        }
        return value;
    }

    static std::vector<ParseValue> expandPolySlots(ParseValue value)
    {
        const int slots = polySlotCount(value);
        std::vector<ParseValue> out;
        if (slots <= 0)
            return out;
        out.reserve((size_t) slots);
        for (int i = 0; i < slots; ++i)
            out.push_back(selectPolySlot(value, i));
        return out;
    }

    static bool isStepSelectorExpression(const SignalExpr& expr)
    {
        juce::String name;
        std::vector<juce::String> args;
        juce::String error;
        return parseSignalCall(expr, name, args, error)
            && name.equalsIgnoreCase("step_sel");
    }

    bool parseStepSelectorValue(const SignalExpr& expr, const juce::String& receiverTarget,
                                ParseValue& value, juce::String& error)
    {
        juce::String name;
        std::vector<juce::String> args;
        juce::String callError;
        if (! parseSignalCall(expr, name, args, callError)
            || ! name.equalsIgnoreCase("step_sel"))
            return false;

        if (args.size() != 2) {
            error = "step_sel(...) expects material and index arguments";
            return false;
        }

        Program parsed = parseMaterialExpression("__step_sel_tmp", args[0]);
        if (! parsed.diagnostics.empty()) {
            error = "step_sel material did not parse: " + parsed.diagnostics.front().message;
            return false;
        }
        if (parsed.statements.size() != 1
            || parsed.statements.front().kind != StatementKind::Definition) {
            error = "step_sel first argument must be material";
            return false;
        }

        Material material;
        if (! normalizeMaterialForUse(parsed.statements.front().material, material)) {
            if (error.isEmpty())
                error = "step_sel material did not normalize";
            return false;
        }
        if (material.steps.empty()) {
            error = "step_sel material has no steps";
            return false;
        }

        ParseValue indexValue;
        if (! parseSignalArgValue(args[1], indexValue, error))
            return false;
        if (isDynamicParseValue(indexValue)) {
            error = "V2_UNSUPPORTED_STEP_SEL_DYNAMIC_INDEX: step_sel(...) dynamic index needs runtime material selection support";
            return false;
        }
        const int selected = wrapStepIndex(staticIndexValue(indexValue), (int) material.steps.size());
        if (selected < 0) {
            error = "step_sel could not select a material step";
            return false;
        }

        const juce::String target = receiverTarget.trim();
        if (target.isEmpty()) {
            error = "step_sel needs a receiver context in the VM-first subset";
            return false;
        }

        return extractStepSelectedReceiverValue(material, selected, target, value, error);
    }

    Program parseMaterialExpression(const juce::String& name, const juce::String& materialSource) const
    {
        Lexer lexer(name + " = " + materialSource);
        Parser parser(name + " = " + materialSource, lexer.run());
        return parser.parse();
    }

    static double simpleStaticValue(const ParseValue& v, double fallback = 0.0)
    {
        if (v.kind == ParseValue::Kind::Number || v.kind == ParseValue::Kind::UnitNumber)
            return v.number;
        if (v.kind == ParseValue::Kind::String)
            return v.str.getDoubleValue();
        return fallback;
    }

    static bool staticPositiveIntegerValue(const ParseValue& v, int& out)
    {
        if (v.kind != ParseValue::Kind::Number || ! std::isfinite(v.number) || v.number < 1.0)
            return false;

        const auto rounded = (int) std::floor(v.number);
        if (std::abs(v.number - (double) rounded) > 0.000001)
            return false;

        out = rounded;
        return true;
    }

    static double staticStepCountValue(const ParseValue& v,
                                       double bpm,
                                       double fallback = 0.0)
    {
        if (v.kind == ParseValue::Kind::Number)
            return v.number;
        if (v.kind == ParseValue::Kind::UnitNumber) {
            if (v.unit == "ms")    return v.number * bpm / 60000.0;
            if (v.unit == "s")     return v.number * bpm / 60.0;
            if (v.unit == "hz")    return v.number * 60.0 / bpm;
            if (v.unit == "steps" || v.unit == "step") return v.number;
            return v.number;
        }
        if (v.kind == ParseValue::Kind::String) {
            const auto s = v.str.toLowerCase();
            if (s.endsWith("khz")) {
                double khz = 0.0;
                if (! parseStrictDoubleLiteral(s.dropLastCharacters(3), &khz))
                    return fallback;
                const double hz = khz * 1000.0;
                return hz > 0.0 ? hz * 60.0 / bpm : fallback;
            }
            if (s.endsWith("hz")) {
                double hz = 0.0;
                if (! parseStrictDoubleLiteral(s.dropLastCharacters(2), &hz))
                    return fallback;
                return hz > 0.0 ? hz * 60.0 / bpm : fallback;
            }
            if (isNoteLike(v.str)) {
                const double midi = curlop::pitch::noteToMidi(v.str);
                const double hz = curlop::pitch::semiToHz(midi);
                return hz > 0.0 ? hz * 60.0 / bpm : fallback;
            }
            double number = 0.0;
            return parseStrictDoubleLiteral(v.str, &number) ? number : fallback;
        }
        return fallback;
    }

    static int positiveRoundedStepCount(double raw)
    {
        if (! std::isfinite(raw) || raw <= 0.0)
            return 0;
        return std::max(1, (int) std::round(raw));
    }

    static std::optional<double> staticStepCountUpper(const ParseValue& v, double bpm)
    {
        if (v.kind == ParseValue::Kind::Number || v.kind == ParseValue::Kind::UnitNumber
            || v.kind == ParseValue::Kind::String)
            return staticStepCountValue(v, bpm, std::numeric_limits<double>::quiet_NaN());

        if (v.kind == ParseValue::Kind::Generator && v.gen) {
            const auto& g = *v.gen;
            const auto argAt = [&] (size_t i) -> ParseValue {
                return i < g.args.size() ? g.args[i] : ParseValue{};
            };
            if (g.generatorType == "random" || g.generatorType == "lfo"
                || g.generatorType == "auto") {
                const auto a = staticStepCountUpper(g.generatorType == "lfo" ? argAt(2) : argAt(0),
                                                    bpm);
                const auto b = staticStepCountUpper(g.generatorType == "lfo" ? argAt(3) : argAt(1),
                                                    bpm);
                if (a && b) return std::max(*a, *b);
            }
            if (g.generatorType == "bernoulli") {
                const auto a = staticStepCountUpper(argAt(1), bpm);
                const auto b = staticStepCountUpper(argAt(2), bpm);
                if (a && b) return std::max(*a, *b);
            }
            return std::nullopt;
        }

        if (v.kind == ParseValue::Kind::SignalChain && v.signalChain) {
            ParseValue source;
            source.kind = ParseValue::Kind::Generator;
            source.gen = v.signalChain->source;
            auto upper = staticStepCountUpper(source, bpm);
            if (! upper) return std::nullopt;

            const auto argAt = [] (const SignalChainOp& op, size_t i) -> ParseValue {
                return i < op.args.size() ? op.args[i] : ParseValue{};
            };
            for (const auto& op : v.signalChain->ops) {
                if (op.name == "clip") {
                    const auto lo = staticStepCountUpper(argAt(op, 0), bpm);
                    const auto hi = staticStepCountUpper(argAt(op, 1), bpm);
                    if (!lo || !hi) return std::nullopt;
                    const double mn = std::min(*lo, *hi);
                    const double mx = std::max(*lo, *hi);
                    *upper = std::min(std::max(*upper, mn), mx);
                } else if (op.name == "scale") {
                    const auto lo = staticStepCountUpper(argAt(op, 0), bpm);
                    const auto hi = staticStepCountUpper(argAt(op, 1), bpm);
                    if (!lo || !hi) return std::nullopt;
                    *upper = std::max(*lo, *hi);
                } else if (op.name == "gain") {
                    const auto g = staticStepCountUpper(argAt(op, 0), bpm);
                    if (!g) return std::nullopt;
                    *upper *= *g;
                } else if (op.name == "offset" || op.name == "transpose"
                           || op.name == "octave") {
                    const auto add = staticStepCountUpper(argAt(op, 0), bpm);
                    if (!add) return std::nullopt;
                    *upper += *add;
                } else if (op.name == "abs") {
                    *upper = std::abs(*upper);
                } else {
                    return std::nullopt;
                }
            }
            return upper;
        }

        return std::nullopt;
    }

    static bool isDynamicParseValue(const ParseValue& v)
    {
        if (v.kind == ParseValue::Kind::Generator || v.kind == ParseValue::Kind::SignalChain)
            return true;
        if (v.kind == ParseValue::Kind::Cond && v.cond)
            return isDynamicParseValue(v.cond->expr);
        return false;
    }

    static double staticIndexValue(const ParseValue& v)
    {
        if (v.kind == ParseValue::Kind::Number || v.kind == ParseValue::Kind::UnitNumber
            || v.kind == ParseValue::Kind::String)
            return simpleStaticValue(v);

        if (v.kind == ParseValue::Kind::Generator && v.gen) {
            const auto& g = *v.gen;
            const auto argAt = [&] (size_t i) -> ParseValue {
                return i < g.args.size() ? g.args[i] : ParseValue{};
            };
            if (g.generatorType == "lfo" || g.generatorType == "random")
                return (simpleStaticValue(argAt(2), 0.0) + simpleStaticValue(argAt(3), 1.0)) * 0.5;
            if (g.generatorType == "auto")
                return (simpleStaticValue(argAt(0), 0.0) + simpleStaticValue(argAt(1), 1.0)) * 0.5;
            if (g.generatorType == "bernoulli")
                return (simpleStaticValue(argAt(1), 0.0) + simpleStaticValue(argAt(2), 1.0)) * 0.5;
        }

        if (v.kind == ParseValue::Kind::SignalChain && v.signalChain) {
            double current = 0.0;
            ParseValue source;
            source.kind = ParseValue::Kind::Generator;
            source.gen = v.signalChain->source;
            current = staticIndexValue(source);
            for (const auto& op : v.signalChain->ops) {
                const auto argAt = [&] (size_t i) -> ParseValue {
                    return i < op.args.size() ? op.args[i] : ParseValue{};
                };
                if (op.name == "clip") {
                    const double lo = simpleStaticValue(argAt(0), -1.0);
                    const double hi = simpleStaticValue(argAt(1), 1.0);
                    current = juce::jlimit(std::min(lo, hi), std::max(lo, hi), current);
                } else if (op.name == "invert") {
                    const double pivot = simpleStaticValue(argAt(0), 0.5);
                    current = pivot - (current - pivot);
                } else if (op.name == "scale") {
                    const double lo = simpleStaticValue(argAt(0), 0.0);
                    const double hi = simpleStaticValue(argAt(1), 1.0);
                    current = lo + current * (hi - lo);
                } else if (op.name == "offset") {
                    current += simpleStaticValue(argAt(0), 0.0);
                } else if (op.name == "gain") {
                    current *= simpleStaticValue(argAt(0), 1.0);
                } else if (op.name == "abs") {
                    current = std::abs(current);
                }
            }
            return current;
        }

        return 0.0;
    }

    static int wrapStepIndex(double raw, int stepCount)
    {
        if (stepCount <= 0 || ! std::isfinite(raw))
            return -1;
        int idx = (int) std::floor(raw);
        idx %= stepCount;
        if (idx < 0)
            idx += stepCount;
        return idx;
    }

    bool extractStepSelectedReceiverValue(const Material& material, int selected,
                                          const juce::String& receiverTarget,
                                          ParseValue& value, juce::String& error)
    {
        const Step& step = material.steps[(size_t) selected];
        const juce::String target = receiverTarget.trim();

        for (const auto& item : step.items) {
            if (item.kind == StepItemKind::Receiver
                && item.receiver.target.equalsIgnoreCase(target))
                return parseReceiverValueForTarget(target, item.receiver.expr, value, error);

            if (item.kind == StepItemKind::GroupedReceiver) {
                for (const auto& groupedTarget : item.grouped.targets)
                    if (groupedTarget.equalsIgnoreCase(target))
                        return parseReceiverValueForTarget(target, item.grouped.expr, value, error);
            }

            if (target.equalsIgnoreCase("pitch") && item.kind == StepItemKind::Note) {
                value = parseSimpleValue(item.value);
                return value.kind != ParseValue::Kind::Null;
            }
            if (target.equalsIgnoreCase("gate") && item.kind == StepItemKind::Note) {
                value.kind = ParseValue::Kind::Number;
                value.number = 1.0;
                return true;
            }
            if (target.equalsIgnoreCase("velocity") && item.kind == StepItemKind::Note) {
                value.kind = ParseValue::Kind::Number;
                value.number = 0.7;
                return true;
            }
        }

        for (const auto& op : material.postfix) {
            if (op.kind == PostfixKind::GroupReceiver
                && op.name.equalsIgnoreCase(target))
                return parseReceiverValueForTarget(target, op.expr, value, error);
        }

        error = "step_sel selected step has no receiver value for '" + target + "'";
        return false;
    }

    bool lowerMarkovReceiverOntoState(const ReceiverCall& receiver, AstNode& state,
                                      int ordinal, juce::String& error)
    {
        if (receiver.foreign || receiver.target.contains(".")) {
            error = "Markov corpus states do not lower foreign receiver targets yet";
            return false;
        }
        if (isStepProcessorCall(receiver.target)) {
            error = "Markov corpus states do not lower step processors yet";
            return false;
        }
        ParseValue value;
        if (! parseReceiverValueForTarget(receiver.target, receiver.expr, value, error))
            return false;
        state.processors.push_back(makeProcessor(receiver.target, std::move(value), ordinal));
        return true;
    }

    bool lowerMarkovCorpusStep(const Step& step, const Material& material,
                               const juce::String& target, int ordinal,
                               bool includeReceiverState,
                               std::vector<AstNodePtr>& options,
                               juce::String& error)
    {
        auto state = makeTriggerEvent(target, {});
        bool foundNote = false;
        int receiverOrdinal = 0;

        for (const auto& item : step.items) {
            if (item.kind == StepItemKind::Note) {
                if (foundNote) {
                    error = "Markov corpus states currently lower one note per state";
                    return false;
                }
                state->pitch.note = item.value;
                foundNote = true;
                continue;
            }
            if (item.kind == StepItemKind::Receiver) {
                if (! includeReceiverState)
                    continue;
                if (! lowerMarkovReceiverOntoState(item.receiver, *state,
                                                   receiverOrdinal++, error))
                    return false;
                continue;
            }
            if (item.kind == StepItemKind::GroupedReceiver) {
                if (! includeReceiverState)
                    continue;
                for (const auto& groupedTarget : item.grouped.targets) {
                    ReceiverCall receiver;
                    receiver.target = groupedTarget;
                    receiver.foreign = groupedTarget.contains(".");
                    receiver.expr = item.grouped.expr;
                    if (! lowerMarkovReceiverOntoState(receiver, *state,
                                                       receiverOrdinal++, error))
                        return false;
                }
                continue;
            }
            if (item.kind == StepItemKind::Rest || item.kind == StepItemKind::Hold) {
                error = "Markov corpus rest/hold states need explicit runtime semantics";
                return false;
            }
            error = "Markov corpus contains unsupported material";
            return false;
        }

        if (! foundNote) {
            error = "Markov corpus state has no note";
            return false;
        }

        for (const auto& op : material.postfix) {
            if (! includeReceiverState)
                continue;
            if (op.kind != PostfixKind::GroupReceiver) {
                error = "Markov corpus structural postfix processors need material-flow support";
                return false;
            }
            ReceiverCall receiver;
            receiver.target = op.name;
            receiver.expr = op.expr;
            if (! lowerMarkovReceiverOntoState(receiver, *state, receiverOrdinal++, error))
                return false;
        }

        state->line = ordinal;
        options.push_back(std::move(state));
        return true;
    }

    bool lowerMarkovGateCorpusStep(const Step& step, const juce::String& target,
                                   int ordinal, std::vector<AstNodePtr>& options,
                                   juce::String& error)
    {
        bool foundGateState = false;
        bool gateOn = false;

        for (const auto& item : step.items) {
            if (item.kind == StepItemKind::Note) {
                if (foundGateState) {
                    error = "Markov gate corpus states currently lower one gate state per step";
                    return false;
                }
                foundGateState = true;
                gateOn = true;
                continue;
            }
            if (item.kind == StepItemKind::Rest) {
                if (foundGateState) {
                    error = "Markov gate corpus states currently lower one gate state per step";
                    return false;
                }
                foundGateState = true;
                gateOn = false;
                continue;
            }
            if (item.kind == StepItemKind::Hold) {
                error = "Markov gate corpus hold states need explicit sustain semantics";
                return false;
            }
            if (item.kind == StepItemKind::Receiver
                || item.kind == StepItemKind::GroupedReceiver)
                continue;

            error = "Markov gate corpus contains unsupported material";
            return false;
        }

        if (! foundGateState) {
            error = "Markov gate corpus state has no note or rest";
            return false;
        }

        auto state = makeAstNode("Event");
        state->eventType = gateOn ? "trigger" : "rest";
        state->module = target;
        state->weight = gateOn ? 1.0 : 0.0;
        state->line = ordinal;
        options.push_back(std::move(state));
        return true;
    }

    bool lowerMarkovItem(const StepItem& item, const juce::String& target,
                         AstNode& astStep, int ordinal,
                         const juce::String& lexicalStatePrefix)
    {
        std::vector<juce::String> args;
        juce::String error;
        if (! splitSignalArgs(item.receiver.expr.source, args, error)) {
            addUnsupported("V2_UNSUPPORTED_MARKOV",
                           "markov(...) arguments did not parse"
                               + (error.isEmpty() ? juce::String() : ": " + error));
            return false;
        }
        if (args.size() != 2) {
            addUnsupported("V2_UNSUPPORTED_MARKOV",
                           "markov(...) expects corpus and order arguments");
            return false;
        }

        auto corpusArg = args[0].trim();
        bool includeReceiverState = true;
        bool gateProjection = false;
        const int projectionDot = corpusArg.lastIndexOfChar('.');
        if (projectionDot > 0
            && ! corpusArg.containsChar(':')
            && ! corpusArg.startsWithChar('[')
            && ! corpusArg.startsWithChar('(')) {
            const auto base = corpusArg.substring(0, projectionDot).trim();
            const auto projection = corpusArg.substring(projectionDot + 1).trim().toLowerCase();
            if (base.isNotEmpty()
                && projection.isNotEmpty()
                && ! isNoteLike(base)) {
                if (projection == "pitch") {
                    corpusArg = base;
                    includeReceiverState = false;
                } else if (projection == "gate") {
                    corpusArg = base;
                    includeReceiverState = false;
                    gateProjection = true;
                } else {
                addUnsupported("V2_UNSUPPORTED_MARKOV_PROJECTION",
                               "Markov projected streams such as "
                                   + corpusArg
                                   + " need projected-state runtime support");
                return false;
                }
            }
        }

        ParseValue orderValue;
        if (! parseSignalArgValue(args[1], orderValue, error)) {
            addUnsupported("V2_UNSUPPORTED_MARKOV_ORDER",
                           "markov(...) order argument is not lowerable"
                               + (error.isEmpty() ? juce::String() : ": " + error));
            return false;
        }
        if (isDynamicParseValue(orderValue)) {
            addUnsupported("V2_UNSUPPORTED_MARKOV_ORDER_DYNAMIC",
                           "markov(...) order must be static until runtime transition-table order changes are designed");
            return false;
        }
        int order = 0;
        if (! staticPositiveIntegerValue(orderValue, order)) {
            addUnsupported("V2_UNSUPPORTED_MARKOV_ORDER",
                           "markov(...) order must be a static positive integer");
            return false;
        }
        if (order != 1) {
            addUnsupported("V2_UNSUPPORTED_MARKOV_ORDER",
                           "Only order-1 markov(...) lowers in this VM slice");
            return false;
        }

        Program parsed = parseMaterialExpression("__markov_tmp", corpusArg);
        if (! parsed.diagnostics.empty()) {
            addUnsupported("V2_UNSUPPORTED_MARKOV_CORPUS",
                           "Markov corpus did not parse: " + parsed.diagnostics.front().message);
            return false;
        }
        if (parsed.statements.size() != 1
            || parsed.statements.front().kind != StatementKind::Definition) {
            addUnsupported("V2_UNSUPPORTED_MARKOV_CORPUS",
                           "Markov first argument must be material");
            return false;
        }

        Material material;
        if (! normalizeMaterialForUse(parsed.statements.front().material, material))
            return false;
        if (material.steps.empty()) {
            addUnsupported("V2_UNSUPPORTED_MARKOV_CORPUS",
                           "Markov corpus has no states");
            return false;
        }

        std::vector<AstNodePtr> options;
        int stateOrdinal = 0;
        for (const auto& stateStep : material.steps) {
            const bool lowered = gateProjection
                ? lowerMarkovGateCorpusStep(stateStep, target, stateOrdinal++,
                                            options, error)
                : lowerMarkovCorpusStep(stateStep, material, target, stateOrdinal++,
                                        includeReceiverState, options, error);
            if (! lowered) {
                addUnsupported("V2_UNSUPPORTED_MARKOV_CORPUS",
                               "Markov corpus does not lower in the VM-first subset"
                                   + (error.isEmpty() ? juce::String() : ": " + error));
                return false;
            }
        }

        juce::String stateKey;
        if (lexicalStatePrefix.isNotEmpty())
            stateKey = "markov:" + lexicalStatePrefix + ":" + juce::String(ordinal);
        astStep.events.push_back(gateProjection
            ? makeMarkovGateSelector(target, (double) order, std::move(options), ordinal, stateKey)
            : makeMarkovSelector(target, (double) order, std::move(options), ordinal, stateKey));
        return true;
    }

    bool expandFunctionCall(const SignalExpr& expr, SignalExpr& expanded,
                            juce::String& functionName, juce::String& error) const
    {
        juce::String name;
        std::vector<juce::String> args;
        if (! parseSignalCall(expr, name, args, error))
            return false;

        auto it = functions.find(name);
        if (it == functions.end()) {
            error = "Unknown V2 function '" + name + "'";
            return false;
        }

        const auto& def = it->second;
        if (args.size() != def.params.size()) {
            error = "Function '" + name + "' expects " + juce::String((int) def.params.size())
                + " arg(s), got " + juce::String((int) args.size());
            return false;
        }

        functionName = name;
        expanded = makeSignalExprFromSource(substituteFunctionParams(def, args));
        return true;
    }

    static juce::String substituteFunctionParams(const FunctionDef& def,
                                                 const std::vector<juce::String>& args)
    {
        juce::String out;
        const auto& src = def.body.source;
        for (int i = 0; i < src.length();) {
            const auto c = src[i];
            if (isIdentStart(c)) {
                const int start = i;
                ++i;
                while (i < src.length() && isIdentPart(src[i]))
                    ++i;
                const auto ident = src.substring(start, i);
                bool replaced = false;
                for (size_t p = 0; p < def.params.size(); ++p) {
                    if (ident == def.params[p]) {
                        out << args[p];
                        replaced = true;
                        break;
                    }
                }
                if (! replaced)
                    out << ident;
                continue;
            }
            out << juce::String::charToString(c);
            ++i;
        }
        return out;
    }

    static bool materialNeedsRouteContext(const Material& material)
    {
        if (material.kind != MaterialKind::Sequence)
            return false;

        for (const auto& step : material.steps) {
            for (const auto& item : step.items) {
                if (item.kind == StepItemKind::Receiver
                    || item.kind == StepItemKind::GroupedReceiver
                    || item.kind == StepItemKind::Atom
                    || item.kind == StepItemKind::Markov)
                    return true;
                if (item.kind == StepItemKind::Tuplet
                    && item.nested
                    && materialNeedsRouteContext(*item.nested))
                    return true;
            }
        }
        return false;
    }

    static Material materialWithoutFlow(Material material)
    {
        material.explicitMaterialFlow = false;
        material.flow.clear();
        return material;
    }

    static bool materialHasContent(const Material& material)
    {
        if (material.kind == MaterialKind::Ref)
            return material.refName.isNotEmpty();
        return ! material.steps.empty();
    }

    bool normalizeMaterialForUse(const Material& material, Material& outMaterial,
                                 std::set<juce::String>& resolving)
    {
        Material base;
        if (material.kind == MaterialKind::Ref) {
            if (material.refName.isEmpty()) {
                addUnsupported("V2_UNRESOLVED_REF", "Empty V2 material ref");
                return false;
            }
            if (! resolving.insert(material.refName).second) {
                addUnsupported("V2_CYCLIC_REF",
                               "Cyclic V2 material ref '" + material.refName + "'");
                return false;
            }
            auto it = definitions.find(material.refName);
            if (it == definitions.end()) {
                resolving.erase(material.refName);
                addUnsupported("V2_UNRESOLVED_REF",
                               "Undefined V2 material ref '" + material.refName + "'");
                return false;
            }
            if (! normalizeMaterialForUse(it->second, base, resolving)) {
                resolving.erase(material.refName);
                return false;
            }
            resolving.erase(material.refName);

            base.postfix.insert(base.postfix.end(),
                                material.postfix.begin(), material.postfix.end());
            base.perItemGroupReceivers.insert(base.perItemGroupReceivers.end(),
                                              material.perItemGroupReceivers.begin(),
                                              material.perItemGroupReceivers.end());
            base.flow.insert(base.flow.end(), material.flow.begin(), material.flow.end());
            base.explicitMaterialFlow = base.explicitMaterialFlow
                                     || material.explicitMaterialFlow;
            base.explicitPerItemGroupReceivers = base.explicitPerItemGroupReceivers
                                              || material.explicitPerItemGroupReceivers;
        } else {
            base = material;
        }

        std::vector<Material> branches;
        if (! expandMaterialFlowBranches(base, branches))
            return false;
        if (branches.size() != 1) {
            addUnsupported("V2_UNSUPPORTED_DEFINITION_FLOW_BRANCHES",
                           "Material refs with '!' branches need explicit route expansion before lowering");
            return false;
        }

        outMaterial = std::move(branches.front());
        return true;
    }

    bool normalizeMaterialForUse(const Material& material, Material& outMaterial)
    {
        std::set<juce::String> resolving;
        return normalizeMaterialForUse(material, outMaterial, resolving);
    }

    void addDefinitionAssignment(const juce::String& name, const Material& material,
                                 bool)
    {
        if (name.isEmpty()) return;
        if (materialNeedsRouteContext(material)) {
            return;
        }
        auto seq = lowerDefinitionMaterial(material);
        if (seq == nullptr) return;
        out.ast.variableNames.push_back(name);
        out.ast.statements.push_back(makeAssignment(name, std::move(seq)));
    }

    std::unique_ptr<AstNode> lowerDefinitionMaterial(const Material& material)
    {
        Material normalized;
        if (! normalizeMaterialForUse(material, normalized))
            return nullptr;
        if (! validatePostfixes(normalized))
            return nullptr;
        if (! applyStaticMaterialPostfixes(normalized))
            return nullptr;

        auto seq = makeSequence();
        for (const auto& step : normalized.steps) {
            auto astStep = makeStep();
            for (const auto& item : step.items) {
                if (item.kind == StepItemKind::Note
                    || item.kind == StepItemKind::PolyphonicChord)
                    astStep->events.push_back(makeTriggerEvent({}, item.value));
                else if (item.kind == StepItemKind::Rest)
                    astStep->events.push_back(makeRestEvent({}));
                else if (item.kind == StepItemKind::Hold)
                    astStep->events.push_back(makeHoldEvent({}));
                else if (item.kind == StepItemKind::Tuplet && item.nested) {
                    auto nested = lowerDefinitionMaterial(*item.nested);
                    if (nested) {
                        nested->scopeType = "tuplet";
                        astStep->events.push_back(std::move(nested));
                    }
                }
            }
            seq->steps.push_back(std::move(astStep));
        }
        return seq;
    }

    bool appendRoute(const Statement& stmt, AstNode& outSequence)
    {
        if (stmt.targets.empty()) {
            addUnsupported("V2_ROUTE_NO_TARGET", "V2 route has no target");
            return false;
        }

        std::vector<Material> branches;
        if (! expandMaterialFlowBranches(stmt.material, branches))
            return false;
        if (branches.size() > 1
            || (branches.size() == 1 && stmt.material.explicitMaterialFlow)) {
            bool emitted = false;
            for (const auto& branch : branches) {
                Statement branchStmt = stmt;
                branchStmt.defineName = {};
                branchStmt.material = branch;
                emitted = appendRouteNoFlow(branchStmt, outSequence) || emitted;
            }
            return emitted;
        }

        return appendRouteNoFlow(stmt, outSequence);
    }

    bool appendRouteNoFlow(const Statement& stmt, AstNode& outSequence)
    {
        Material loweredMaterial;
        juce::String lexicalStatePrefix = stmt.defineName;
        if (stmt.material.kind == MaterialKind::Ref) {
            if (! normalizeMaterialForUse(stmt.material, loweredMaterial))
                return false;
            if (lexicalStatePrefix.isEmpty())
                lexicalStatePrefix = stmt.material.refName;
        } else {
            if (! normalizeMaterialForUse(stmt.material, loweredMaterial))
                return false;
        }
        if (! validatePostfixes(loweredMaterial))
            return false;
        if (! applyStaticMaterialPostfixes(loweredMaterial))
            return false;
        if (hasStructuralPostfix(loweredMaterial) || hasWholeScopeGroupReceiverPostfix(loweredMaterial))
            return appendRouteAsNestedScope(stmt, loweredMaterial, outSequence);

        bool emitted = false;
        for (const auto& srcStep : loweredMaterial.steps) {
            auto astStep = makeStep();
            for (size_t i = 0; i < stmt.targets.size(); ++i)
                emitted = lowerStepForTarget(srcStep, loweredMaterial, stmt.targets[i], *astStep,
                                             true, i == 0, lexicalStatePrefix) || emitted;
            outSequence.steps.push_back(std::move(astStep));
        }
        if (! emitted)
            addUnsupported("V2_ROUTE_EMPTY", "V2 route material has no lowerable events");
        return emitted;
    }

    bool appendSocketExport(const Statement& stmt, AstNode& outSequence)
    {
        std::vector<Material> branches;
        if (! expandMaterialFlowBranches(stmt.material, branches))
            return false;
        if (branches.size() > 1
            || (branches.size() == 1 && stmt.material.explicitMaterialFlow)) {
            bool emitted = false;
            for (const auto& branch : branches) {
                Statement branchStmt = stmt;
                branchStmt.material = branch;
                emitted = appendSocketExportNoFlow(branchStmt, outSequence) || emitted;
            }
            return emitted;
        }

        return appendSocketExportNoFlow(stmt, outSequence);
    }

    bool appendSocketExportNoFlow(const Statement& stmt, AstNode& outSequence)
    {
        Material loweredMaterial;
        if (stmt.material.kind == MaterialKind::Ref) {
            if (! normalizeMaterialForUse(stmt.material, loweredMaterial))
                return false;
        } else {
            if (! normalizeMaterialForUse(stmt.material, loweredMaterial))
                return false;
        }
        if (! validatePostfixes(loweredMaterial))
            return false;
        if (! applyStaticMaterialPostfixes(loweredMaterial))
            return false;

        const auto selectors = stmt.socketSelected ? stmt.selectors
                                                   : bundleSelectorsFor(loweredMaterial);
        if (hasStructuralPostfix(loweredMaterial))
            return appendSocketExportAsNestedScope(stmt, loweredMaterial, selectors, outSequence);

        bool emitted = false;
        for (const auto& srcStep : loweredMaterial.steps) {
            auto astStep = makeStep();
            for (const auto& selector : selectors)
                emitted = lowerSocketExportSelector(stmt.name, selector, srcStep,
                                                    *astStep, &loweredMaterial) || emitted;
            outSequence.steps.push_back(std::move(astStep));
        }
        if (! emitted)
            addUnsupported("V2_SOCKET_EMPTY", "V2 socket export material has no lowerable streams");
        return emitted;
    }

    std::vector<SocketSelector> bundleSelectorsFor(const Material& material) const
    {
        std::vector<SocketSelector> selectors;
        std::set<juce::String> seen;
        auto add = [&](juce::String name) {
            if (name.isEmpty() || ! seen.insert(name).second)
                return;
            SocketSelector selector;
            selector.kind = SocketSelectorKind::Stream;
            selector.name = std::move(name);
            selector.qualified = selector.name.contains(".");
            selectors.push_back(std::move(selector));
        };

        add("gate");
        add("pitch");
        add("velocity");

        std::function<void(const Material&)> collect = [&](const Material& m) {
            if (m.kind != MaterialKind::Sequence)
                return;
            for (const auto& step : m.steps) {
                for (const auto& item : step.items) {
                    if (item.kind == StepItemKind::Receiver) {
                        add(item.receiver.target);
                    } else if (item.kind == StepItemKind::GroupedReceiver) {
                        for (const auto& target : item.grouped.targets)
                            add(target);
                    } else if (item.kind == StepItemKind::Tuplet && item.nested) {
                        collect(*item.nested);
                    }
                }
            }
            for (const auto& op : m.postfix)
                if (op.kind == PostfixKind::GroupReceiver)
                    add(op.name);
        };
        collect(material);
        return selectors;
    }

    bool lowerSocketExportSelector(const juce::String& socketRoot,
                                   const SocketSelector& selector,
                                   const Step& srcStep,
                                   AstNode& astStep,
                                   const Material* materialPostfix)
    {
        if (selector.kind != SocketSelectorKind::DerivedStream)
            return lowerSocketSelectorStep(socketRoot, selector, srcStep, astStep,
                                           materialPostfix);

        if (isSupportedSocketRouteExpr(selector.expr))
            return true;

        std::vector<juce::String> terms;
        if (! splitDerivedSelectorTerms(selector.expr.source, terms)) {
            addUnsupported("V2_UNSUPPORTED_SOCKET_DERIVED",
                           "Derived socket stream '" + selector.name
                               + "' only lowers top-level selector sums in this slice");
            return false;
        }

        bool emitted = false;
        const juce::String derivedSocketName = socketRoot + "." + selector.name;
        for (const auto& term : terms) {
            const auto termExpr = makeSignalExprFromSource(term);
            if (termExpr.chain.empty()) {
                addUnsupported("V2_UNSUPPORTED_SOCKET_DERIVED",
                               "Derived socket stream '" + selector.name
                                   + "' has an empty source term");
                return false;
            }
            SocketSelector source;
            source.kind = SocketSelectorKind::Stream;
            source.name = termExpr.chain.front().trim();
            source.qualified = source.name.contains(".");
            source.expr = termExpr;
            emitted = lowerSocketSelectorStep(socketRoot, source, srcStep, astStep,
                                              materialPostfix, derivedSocketName)
                   || emitted;
        }
        return emitted;
    }

    static bool splitDerivedSelectorTerms(const juce::String& source,
                                          std::vector<juce::String>& terms)
    {
        juce::String current;
        int parens = 0;
        int brackets = 0;
        int braces = 0;
        auto push = [&]() {
            auto t = current.trim();
            if (t.isEmpty())
                return false;
            terms.push_back(t);
            current = {};
            return true;
        };

        for (int i = 0; i < source.length(); ++i) {
            const auto c = source[i];
            if (c == '(') ++parens;
            else if (c == ')') --parens;
            else if (c == '[') ++brackets;
            else if (c == ']') --brackets;
            else if (c == '{') ++braces;
            else if (c == '}') --braces;

            if (c == '+' && parens == 0 && brackets == 0 && braces == 0) {
                if (! push())
                    return false;
                continue;
            }
            current << juce::String::charToString(c);
        }
        if (! push())
            return false;
        return ! terms.empty();
    }

    bool lowerSocketSelectorStep(const juce::String& socketRoot, const SocketSelector& selector,
                                 const Step& srcStep, AstNode& astStep,
                                 const Material* materialPostfix = nullptr,
                                 const juce::String& forcedSocketName = {})
    {
        const auto firstOutputEvent = astStep.events.size();
        int stackWidth = 1;
        for (const auto& item : srcStep.items) {
            if (item.kind != StepItemKind::Stack)
                continue;
            int count = 1;
            if (parseStaticPositiveInt(item.receiver.expr, count))
                stackWidth = std::max(stackWidth, count);
        }
        const auto applyStackWidth = [&] {
            if (stackWidth <= 1)
                return;
            for (auto index = firstOutputEvent;
                 index < astStep.events.size(); ++index)
                if (astStep.events[index]
                    && astStep.events[index]->eventType == "local_output")
                    astStep.events[index]->stackVoices =
                        std::max(astStep.events[index]->stackVoices, stackWidth);
        };
        bool emitted = false;
        const juce::String socketName = forcedSocketName.isNotEmpty()
            ? forcedSocketName : socketRoot + "." + selector.name;
        const auto name = selector.name.toLowerCase();

        bool hasAuthoredSelectorReceiver = false;
        for (const auto& item : srcStep.items) {
            if (item.kind == StepItemKind::Receiver
                && item.receiver.target.equalsIgnoreCase(selector.name))
                hasAuthoredSelectorReceiver = true;
            if (item.kind == StepItemKind::GroupedReceiver) {
                for (const auto& target : item.grouped.targets)
                    if (target.equalsIgnoreCase(selector.name))
                        hasAuthoredSelectorReceiver = true;
            }
        }
        if (materialPostfix != nullptr) {
            for (const auto& op : materialPostfix->postfix) {
                if (op.kind == PostfixKind::GroupReceiver
                    && op.name.equalsIgnoreCase(selector.name))
                    hasAuthoredSelectorReceiver = true;
            }
        }

        if (hasAuthoredSelectorReceiver) {
            int ordinal = 0;
            for (const auto& item : srcStep.items) {
                if (lowerTupletSocketSelector(socketRoot, selector, item, astStep,
                                              emitted, forcedSocketName))
                    continue;
                if (item.kind == StepItemKind::Receiver) {
                    emitted = lowerReceiverToSocket(item.receiver, selector.name, socketName,
                                                    astStep, ordinal++,
                                                    selector.expr.source.isNotEmpty()
                                                        ? &selector.expr : nullptr) || emitted;
                    continue;
                }
                if (item.kind == StepItemKind::GroupedReceiver) {
                    for (const auto& groupedTarget : item.grouped.targets) {
                        ReceiverCall receiver;
                        receiver.target = groupedTarget;
                        receiver.foreign = groupedTarget.contains(".");
                        receiver.expr = item.grouped.expr;
                        emitted = lowerReceiverToSocket(receiver, selector.name, socketName,
                                                        astStep, ordinal++,
                                                        selector.expr.source.isNotEmpty()
                                                            ? &selector.expr : nullptr) || emitted;
                    }
                }
            }
            if (materialPostfix != nullptr)
                emitted = lowerMaterialPostfixReceiverToSocket(*materialPostfix, selector,
                                                               socketName, astStep,
                                                               ordinal) || emitted;
            applyStackWidth();
            return emitted;
        }

        if (name == "gate") {
            for (const auto& item : srcStep.items) {
                if (lowerTupletSocketSelector(socketRoot, selector, item, astStep,
                                              emitted, forcedSocketName))
                    continue;
                if (item.kind != StepItemKind::Note)
                    continue;
                astStep.events.push_back(makeLocalOutputEvent(socketName,
                                                              makeProcessor("gate", ParseValue{}, 0)));
                emitted = true;
            }
            applyStackWidth();
            return emitted;
        }

        if (name == "pitch") {
            for (const auto& item : srcStep.items) {
                if (lowerTupletSocketSelector(socketRoot, selector, item, astStep,
                                              emitted, forcedSocketName))
                    continue;
                if (item.kind != StepItemKind::Note)
                    continue;
                astStep.events.push_back(makeLocalOutputEvent(socketName,
                                                              makeProcessor("pitch",
                                                                            parseSimpleValue(item.value),
                                                                            0)));
                emitted = true;
            }
            applyStackWidth();
            return emitted;
        }

        if (name == "velocity") {
            for (const auto& item : srcStep.items) {
                if (lowerTupletSocketSelector(socketRoot, selector, item, astStep,
                                              emitted, forcedSocketName))
                    continue;
                if (item.kind != StepItemKind::Note)
                    continue;
                ParseValue value;
                value.kind = ParseValue::Kind::Number;
                value.number = 0.7;
                astStep.events.push_back(makeLocalOutputEvent(socketName,
                                                              makeProcessor("velocity",
                                                                            std::move(value),
                                                                            0)));
                emitted = true;
            }
            applyStackWidth();
            return emitted;
        }

        int ordinal = 0;
        for (const auto& item : srcStep.items) {
            if (lowerTupletSocketSelector(socketRoot, selector, item, astStep,
                                          emitted, forcedSocketName))
                continue;
            if (item.kind == StepItemKind::Receiver) {
                emitted = lowerReceiverToSocket(item.receiver, selector.name, socketName,
                                                astStep, ordinal++,
                                                selector.expr.source.isNotEmpty()
                                                    ? &selector.expr : nullptr) || emitted;
                continue;
            }
            if (item.kind == StepItemKind::GroupedReceiver) {
                for (const auto& groupedTarget : item.grouped.targets) {
                    ReceiverCall receiver;
                    receiver.target = groupedTarget;
                    receiver.foreign = groupedTarget.contains(".");
                    receiver.expr = item.grouped.expr;
                    emitted = lowerReceiverToSocket(receiver, selector.name, socketName,
                                                    astStep, ordinal++,
                                                    selector.expr.source.isNotEmpty()
                                                        ? &selector.expr : nullptr) || emitted;
                }
                continue;
            }
        }
        if (materialPostfix != nullptr)
            emitted = lowerMaterialPostfixReceiverToSocket(*materialPostfix, selector,
                                                           socketName, astStep,
                                                           ordinal) || emitted;

        applyStackWidth();
        return emitted;
    }

    bool lowerMaterialPostfixReceiverToSocket(const Material& material, const SocketSelector& selector,
                                              const juce::String& socketName, AstNode& astStep,
                                              int& ordinal)
    {
        bool emitted = false;
        for (const auto& op : material.postfix) {
            if (op.kind != PostfixKind::GroupReceiver)
                continue;
            ReceiverCall receiver;
            receiver.target = op.name;
            receiver.expr = op.expr;
            emitted = lowerReceiverToSocket(receiver, selector.name, socketName,
                                            astStep, ordinal++,
                                            selector.expr.source.isNotEmpty()
                                                ? &selector.expr : nullptr) || emitted;
        }
        return emitted;
    }

    bool lowerTupletSocketSelector(const juce::String& socketRoot, const SocketSelector& selector,
                                   const StepItem& item, AstNode& astStep, bool& emitted,
                                   const juce::String& forcedSocketName = {})
    {
        if (item.kind != StepItemKind::Tuplet || ! item.nested)
            return false;

        auto nested = makeSequence();
        nested->scopeType = "tuplet";
        bool nestedEmitted = false;
        for (const auto& nestedStep : item.nested->steps) {
            auto innerStep = makeStep();
            nestedEmitted = lowerSocketSelectorStep(socketRoot, selector, nestedStep,
                                                    *innerStep, nullptr,
                                                    forcedSocketName) || nestedEmitted;
            nested->steps.push_back(std::move(innerStep));
        }
        if (nestedEmitted) {
            astStep.events.push_back(std::move(nested));
            emitted = true;
        }
        return true;
    }

    bool lowerReceiverToSocket(const ReceiverCall& receiver, const juce::String& selectorName,
                               const juce::String& socketName, AstNode& astStep, int ordinal,
                               const SignalExpr* selectorExpr = nullptr)
    {
        if (! receiver.target.equalsIgnoreCase(selectorName))
            return false;

        ParseValue value;
        juce::String signalError;
        if (! parseReceiverValueForTarget(receiver.target, receiver.expr, value, signalError)) {
            addUnsupported("V2_UNSUPPORTED_SIGNAL_EXPR",
                           "Socket signal expression '" + receiver.expr.source
                               + "' parses but does not lower in the VM-first subset"
                               + (signalError.isEmpty() ? juce::String() : ": " + signalError));
            return false;
        }

        if (selectorExpr != nullptr
            && ! appendSelectorTransforms(selectorName, *selectorExpr, value, signalError)) {
            addUnsupported("V2_UNSUPPORTED_SOCKET_DERIVED",
                           "Derived socket stream transform for '" + selectorName
                               + "' does not lower in the VM-first subset"
                               + (signalError.isEmpty() ? juce::String() : ": " + signalError));
            return false;
        }

        astStep.events.push_back(makeLocalOutputEvent(socketName,
                                                      makeProcessor(selectorName,
                                                                    std::move(value),
                                                                    ordinal)));
        return true;
    }

    static bool appendSelectorTransforms(const juce::String& selectorName,
                                         const SignalExpr& selectorExpr,
                                         ParseValue& value,
                                         juce::String& error)
    {
        if (selectorExpr.chain.size() <= 1)
            return true;

        const auto source = selectorExpr.chain.front().trim();
        if (! source.equalsIgnoreCase(selectorName)) {
            error = "Selector transform source '" + source
                + "' does not match receiver '" + selectorName + "'";
            return false;
        }

        std::shared_ptr<SignalChainNode> chain;
        if (value.kind == ParseValue::Kind::Generator && value.gen) {
            chain = std::make_shared<SignalChainNode>();
            chain->source = value.gen;
        } else if (value.kind == ParseValue::Kind::SignalChain && value.signalChain) {
            chain = std::make_shared<SignalChainNode>(*value.signalChain);
        } else {
            error = "Selector transforms currently require a signal generator source";
            return false;
        }

        for (size_t i = 1; i < selectorExpr.chain.size(); ++i) {
            SignalChainOp op;
            if (! parseSignalChainTransformOp(selectorExpr.chain[i], op, error))
                return false;
            chain->ops.push_back(std::move(op));
        }

        value.kind = ParseValue::Kind::SignalChain;
        value.signalChain = std::move(chain);
        return true;
    }

    bool appendRouteAsNestedScope(const Statement& stmt, const Material& material,
                                  AstNode& outSequence)
    {
        bool emitted = false;
        juce::String lexicalStatePrefix = stmt.defineName;
        if (lexicalStatePrefix.isEmpty() && stmt.material.kind == MaterialKind::Ref)
            lexicalStatePrefix = stmt.material.refName;
        auto outerStep = makeStep();
        auto nested = makeSequence();

        for (const auto& srcStep : material.steps) {
            auto innerStep = makeStep();
            for (size_t i = 0; i < stmt.targets.size(); ++i)
                emitted = lowerStepForTarget(srcStep, material, stmt.targets[i], *innerStep,
                                             true, i == 0, lexicalStatePrefix) || emitted;
            nested->steps.push_back(std::move(innerStep));
        }

        if (! emitted) {
            addUnsupported("V2_ROUTE_EMPTY", "V2 route material has no lowerable events");
            return false;
        }
        if (! appendRouteScopePostfixProcessors(material, *outerStep))
            return false;

        outerStep->events.push_back(std::move(nested));
        outSequence.steps.push_back(std::move(outerStep));
        return true;
    }

    bool appendSocketExportAsNestedScope(const Statement& stmt, const Material& material,
                                         const std::vector<SocketSelector>& selectors,
                                         AstNode& outSequence)
    {
        bool emitted = false;
        auto outerStep = makeStep();
        auto nested = makeSequence();

        for (const auto& srcStep : material.steps) {
            auto innerStep = makeStep();
            for (const auto& selector : selectors)
                emitted = lowerSocketExportSelector(stmt.name, selector, srcStep,
                                                    *innerStep, &material) || emitted;
            nested->steps.push_back(std::move(innerStep));
        }

        if (! emitted) {
            addUnsupported("V2_SOCKET_EMPTY", "V2 socket export material has no lowerable streams");
            return false;
        }
        if (! appendStructuralPostfixProcessors(material, *outerStep))
            return false;

        outerStep->events.push_back(std::move(nested));
        outSequence.steps.push_back(std::move(outerStep));
        return true;
    }

    bool validatePostfixes(const Material& material)
    {
        for (const auto& op : material.postfix) {
            if (op.kind == PostfixKind::GroupReceiver)
                continue;
            if (op.kind == PostfixKind::StructuralOp && isLowerableStructuralPostfix(op.name))
                continue;
            addUnsupported("V2_UNSUPPORTED_POSTFIX",
                           "Only receiver postfix ops and known structural group ops lower in the VM-first subset");
            return false;
        }
        for (const auto& op : material.perItemGroupReceivers) {
            if (op.kind == PostfixKind::GroupReceiver)
                continue;
            addUnsupported("V2_UNSUPPORTED_PER_ITEM_GROUP_RECEIVERS",
                           "Only receiver calls lower in explicit per-item group receiver blocks");
            return false;
        }
        for (const auto& segment : material.flow) {
            if (segment.material && ! validatePostfixes(*segment.material))
                return false;
        }
        return true;
    }

    bool expandMaterialFlowBranches(const Material& material, std::vector<Material>& branches)
    {
        branches.clear();
        Material current = materialWithoutFlow(material);

        for (const auto& segment : material.flow) {
            if (! segment.material)
                continue;

            Material next = materialWithoutFlow(*segment.material);
            if (segment.join == MaterialFlowJoin::Break) {
                branches.push_back(std::move(current));
                current = std::move(next);
                continue;
            }

            if (materialHasContent(next)) {
                addUnsupported("V2_UNSUPPORTED_MATERIAL_FLOW",
                               "Colon material flow currently lowers postfix processors; use '!' for a new material branch");
                return false;
            }

            current.postfix.insert(current.postfix.end(), next.postfix.begin(), next.postfix.end());
            current.perItemGroupReceivers.insert(current.perItemGroupReceivers.end(),
                                                 next.perItemGroupReceivers.begin(),
                                                 next.perItemGroupReceivers.end());
        }

        branches.push_back(std::move(current));
        return true;
    }

    static bool hasStructuralPostfix(const Material& material)
    {
        for (const auto& op : material.postfix)
            if (op.kind == PostfixKind::StructuralOp)
                return true;
        return false;
    }

    static bool hasWholeScopeGroupReceiverPostfix(const Material& material)
    {
        for (const auto& op : material.postfix)
            if (op.kind == PostfixKind::GroupReceiver)
                return true;
        return false;
    }

    bool fitReferenceOverlayStep(Material& material, int fitCount)
    {
        if (material.steps.size() != 1)
            return false;

        const auto& srcStep = material.steps.front();
        bool hasReference = false;
        for (const auto& item : srcStep.items) {
            if (item.kind == StepItemKind::Atom
                && item.value.isNotEmpty()
                && definitions.find(item.value) != definitions.end()) {
                hasReference = true;
                break;
            }
        }
        if (! hasReference)
            return false;

        struct OverlaySource {
            bool isRef = false;
            StepItem literal;
            Material material;
        };
        std::vector<OverlaySource> sources;
        sources.reserve(srcStep.items.size());

        for (const auto& item : srcStep.items) {
            auto defIt = item.kind == StepItemKind::Atom && item.value.isNotEmpty()
                ? definitions.find(item.value)
                : definitions.end();
            if (defIt == definitions.end()) {
                OverlaySource src;
                src.isRef = false;
                src.literal = item;
                sources.push_back(std::move(src));
                continue;
            }

            Material refMaterial;
            if (! normalizeMaterialForUse(defIt->second, refMaterial))
                return false;
            if (! validatePostfixes(refMaterial))
                return false;
            if (! applyStaticMaterialPostfixes(refMaterial))
                return false;
            if (refMaterial.steps.empty()) {
                addUnsupported("V2_UNSUPPORTED_FIT",
                               "fit(...) cannot sample empty material ref '" + item.value + "'");
                return false;
            }

            OverlaySource src;
            src.isRef = true;
            src.material = std::move(refMaterial);
            sources.push_back(std::move(src));
        }

        std::vector<Step> fitted;
        fitted.reserve((size_t) fitCount);
        for (int i = 0; i < fitCount; ++i) {
            Step dst;
            for (const auto& src : sources) {
                if (! src.isRef) {
                    if (i == 0)
                        dst.items.push_back(src.literal);
                    continue;
                }

                Material oneStep = src.material;
                oneStep.steps.clear();
                oneStep.steps.push_back(src.material.steps[(size_t) i % src.material.steps.size()]);

                StepItem nested;
                nested.kind = StepItemKind::Tuplet;
                nested.nested = std::make_shared<Material>(std::move(oneStep));
                dst.items.push_back(std::move(nested));
            }
            fitted.push_back(std::move(dst));
        }

        material.steps = std::move(fitted);
        return true;
    }

    bool applyChordProcessor(Material& material, const juce::String& rawChordName)
    {
        const auto chordName = rawChordName.trim().toLowerCase();
        if (chordName.isEmpty()
            || curlop::music::chordToIntervals(chordName.toStdString()).empty()) {
            addUnsupported("V2_UNSUPPORTED_CHORD",
                           "chord(...) expects a named chord from the music table");
            return false;
        }

        std::function<bool(StepItem&)> applyToItem = [&] (StepItem& item) -> bool {
            if (item.kind == StepItemKind::Note
                || item.kind == StepItemKind::PolyphonicChord) {
                const int dot = item.value.indexOfChar('.');
                const auto root = dot > 0 ? item.value.substring(0, dot) : item.value;
                item.kind = StepItemKind::PolyphonicChord;
                item.value = root + "." + chordName;
                return true;
            }

            if (item.kind == StepItemKind::Tuplet && item.nested != nullptr)
                return applyChordProcessor(*item.nested, chordName);

            return true;
        };

        for (auto& step : material.steps)
            for (auto& item : step.items)
                if (! applyToItem(item))
                    return false;

        return true;
    }

    bool applyStaticMaterialPostfixes(Material& material)
    {
        std::vector<PostfixOp> kept;
        kept.reserve(material.postfix.size());

        for (const auto& op : material.postfix) {
            if (op.kind == PostfixKind::StructuralOp
                && op.name.equalsIgnoreCase("chord")) {
                std::vector<ParseValue> args;
                juce::String error;
                if (! parsePostfixArgs(op.name, op.expr, args, error)) {
                    addUnsupported("V2_UNSUPPORTED_CHORD",
                                   "chord(...) has unsupported args"
                                       + (error.isEmpty() ? juce::String() : ": " + error));
                    return false;
                }
                if (args.size() != 1
                    || args.front().kind != ParseValue::Kind::String
                    || args.front().str.trim().isEmpty()) {
                    addUnsupported("V2_UNSUPPORTED_CHORD",
                                   "chord(...) expects exactly one static chord name");
                    return false;
                }
                if (! applyChordProcessor(material, args.front().str))
                    return false;
                continue;
            }

            if (op.kind != PostfixKind::StructuralOp
                || ! op.name.equalsIgnoreCase("fit")) {
                kept.push_back(op);
                continue;
            }

            std::vector<ParseValue> args;
            juce::String error;
            if (! parsePostfixArgs(op.name, op.expr, args, error)) {
                addUnsupported("V2_UNSUPPORTED_FIT",
                               "fit(...) has unsupported args"
                                   + (error.isEmpty() ? juce::String() : ": " + error));
                return false;
            }
            if (args.size() != 1) {
                addUnsupported("V2_UNSUPPORTED_FIT",
                               "fit(...) expects exactly one static step count");
                return false;
            }
            int fitCount = 0;
            const bool dynamicFit = isDynamicParseValue(args.front());
            if (dynamicFit) {
                const auto upper = staticStepCountUpper(args.front(), bpm_);
                if (! upper) {
                    addUnsupported("V2_UNSUPPORTED_FIT_DYNAMIC",
                                   "fit(...) dynamic length needs a bounded expression in this slice");
                    return false;
                }
                fitCount = positiveRoundedStepCount(*upper);
            } else {
                fitCount = positiveRoundedStepCount(staticStepCountValue(args.front(), bpm_, -1.0));
            }
            if (fitCount <= 0) {
                addUnsupported("V2_UNSUPPORTED_FIT",
                               "fit(...) step count must resolve to a positive step span");
                return false;
            }
            if (material.steps.empty()) {
                addUnsupported("V2_UNSUPPORTED_FIT",
                               "fit(...) cannot wrap empty material");
                return false;
            }

            if (! fitReferenceOverlayStep(material, fitCount)) {
                std::vector<Step> fitted;
                fitted.reserve((size_t) fitCount);
                for (int i = 0; i < fitCount; ++i)
                    fitted.push_back(material.steps[(size_t) i % material.steps.size()]);
                material.steps = std::move(fitted);
            }
            if (dynamicFit)
                kept.push_back(op);
        }

        material.postfix = std::move(kept);
        return true;
    }

    bool appendStructuralPostfixProcessors(const Material& material, AstNode& outerStep)
    {
        int ordinal = 100000;
        for (const auto& op : material.postfix) {
            if (op.kind != PostfixKind::StructuralOp)
                continue;

            std::vector<ParseValue> args;
            juce::String error;
            if (! parsePostfixArgs(op.name, op.expr, args, error)) {
                addUnsupported("V2_UNSUPPORTED_POSTFIX",
                               "Structural postfix '" + op.name + "' has unsupported args"
                                   + (error.isEmpty() ? juce::String() : ": " + error));
                return false;
            }
            if (! normalizeProcessorArgsForV2(op.name, args, error)) {
                addUnsupported(unsupportedProcessorArgsCode(op.name,
                                                            "V2_UNSUPPORTED_POSTFIX",
                                                            error),
                               "Structural postfix '" + op.name + "' has unsupported args"
                                   + (error.isEmpty() ? juce::String() : ": " + error));
                return false;
            }
            outerStep.processors.push_back(makeProcessor(op.name.toLowerCase(),
                                                         std::move(args),
                                                         ordinal++));
        }
        return true;
    }

    bool appendRouteScopePostfixProcessors(const Material& material, AstNode& outerStep)
    {
        int ordinal = 100000;
        for (const auto& op : material.postfix) {
            if (op.kind == PostfixKind::StructuralOp) {
                std::vector<ParseValue> args;
                juce::String error;
                if (! parsePostfixArgs(op.name, op.expr, args, error)) {
                    addUnsupported("V2_UNSUPPORTED_POSTFIX",
                                   "Structural postfix '" + op.name + "' has unsupported args"
                                       + (error.isEmpty() ? juce::String() : ": " + error));
                    return false;
                }
                if (! normalizeProcessorArgsForV2(op.name, args, error)) {
                    addUnsupported(unsupportedProcessorArgsCode(op.name,
                                                                "V2_UNSUPPORTED_POSTFIX",
                                                                error),
                                   "Structural postfix '" + op.name + "' has unsupported args"
                                       + (error.isEmpty() ? juce::String() : ": " + error));
                    return false;
                }
                outerStep.processors.push_back(makeProcessor(op.name.toLowerCase(),
                                                             std::move(args),
                                                             ordinal++));
                continue;
            }
            if (op.kind != PostfixKind::GroupReceiver)
                continue;

            ParseValue value;
            juce::String error;
            if (! parseReceiverValueForTarget(op.name, op.expr, value, error)) {
                addUnsupported("V2_UNSUPPORTED_SIGNAL_EXPR",
                               "Group receiver expression '" + op.expr.source
                                   + "' parses but does not lower in the VM-first subset"
                                   + (error.isEmpty() ? juce::String() : ": " + error));
                return false;
            }
            outerStep.processors.push_back(makeProcessor(op.name, std::move(value), ordinal++));
        }
        return true;
    }

    static bool parsePostfixArgs(const juce::String& processorName, const SignalExpr& expr,
                                 std::vector<ParseValue>& args,
                                 juce::String& error)
    {
        if (expr.source.trim().isEmpty())
            return true;
        std::vector<juce::String> argSources;
        if (! splitSignalArgs(expr.source, argSources, error))
            return false;
        for (const auto& argSource : argSources) {
            ParseValue value;
            const auto trimmedArg = argSource.trim();
            const bool simpleCondAtom =
                processorName.trim().equalsIgnoreCase("cond")
                && ! trimmedArg.containsChar('(')
                && ! trimmedArg.containsChar(',')
                && ! trimmedArg.containsChar('+')
                && ! trimmedArg.containsChar('[')
                && ! trimmedArg.containsChar(']')
                && ! trimmedArg.containsChar('{')
                && ! trimmedArg.containsChar('}');
            if (simpleCondAtom) {
                const auto first = trimmedArg.isNotEmpty() ? trimmedArg[0] : juce::juce_wchar();
                const bool symbolic = (first >= 'a' && first <= 'z')
                                   || (first >= 'A' && first <= 'Z')
                                   || first == '!';
                if (symbolic) {
                    value.kind = ParseValue::Kind::String;
                    value.str = trimmedArg;
                } else {
                    value = parseSimpleValue(trimmedArg);
                }
            }
            else if (! parseSignalArgValue(argSource, value, error))
                return false;
            if (value.kind == ParseValue::Kind::Null) {
                error = "Empty structural postfix args do not lower in the VM-first subset";
                return false;
            }
            args.push_back(std::move(value));
        }
        return true;
    }

    bool parseChordName(const SignalExpr& expr, juce::String& chordName,
                        juce::String& error)
    {
        std::vector<ParseValue> args;
        if (! parsePostfixArgs("chord", expr, args, error))
            return false;
        if (args.size() != 1
            || args.front().kind != ParseValue::Kind::String
            || args.front().str.trim().isEmpty()) {
            error = "chord(...) expects exactly one static chord name";
            return false;
        }

        chordName = args.front().str.trim().toLowerCase();
        if (curlop::music::chordToIntervals(chordName.toStdString()).empty()) {
            error = "Unknown chord '" + chordName + "'";
            return false;
        }
        return true;
    }

    bool lowerChordOntoTrigger(const ReceiverCall& receiver, AstNode* lastTrigger,
                               int& ordinal)
    {
        if (lastTrigger == nullptr) {
            addUnsupported("V2_UNSUPPORTED_CHORD",
                           "chord(...) adjacency must follow a note or chord in the same step");
            ++ordinal;
            return false;
        }

        juce::String chordName;
        juce::String error;
        if (! parseChordName(receiver.expr, chordName, error)) {
            addUnsupported("V2_UNSUPPORTED_CHORD",
                           "chord(...) has unsupported args"
                               + (error.isEmpty() ? juce::String() : ": " + error));
            ++ordinal;
            return false;
        }

        lastTrigger->pitch.chord = chordName;
        ++ordinal;
        return true;
    }

    bool lowerStackOntoTrigger(const StepItem& item, const juce::String& target,
                               AstNode* lastTrigger, AstNode& astStep, int& ordinal)
    {
        if (lastTrigger == nullptr || lastTrigger->module != target) {
            addUnsupported("V2_UNSUPPORTED_STACK",
                           "stack(...) currently lowers only when it follows a trigger in the same route scope");
            ++ordinal;
            return false;
        }
        if (lastTrigger->pitch.chord.isNotEmpty()
            || ! lastTrigger->pitch.explicitVoicings.empty()) {
            addUnsupported("V2_UNSUPPORTED_STACK",
                           "stack(...) over chord/polyphonic material needs a stack-over-polyphony engine rule");
            ++ordinal;
            return false;
        }

        int count = 0;
        if (! parseStaticPositiveInt(item.receiver.expr, count)) {
            addUnsupported("V2_UNSUPPORTED_STACK_DYNAMIC_COUNT",
                           "stack(...) count accepts expressions syntactically, but only static positive integer counts lower in this VM-first slice");
            ++ordinal;
            return false;
        }
        lastTrigger->stackVoices = std::max(1, count);

        if (! item.nested) {
            ++ordinal;
            return true;
        }
        if (item.nested->kind != MaterialKind::Sequence
            || item.nested->steps.size() != 1) {
            addUnsupported("V2_UNSUPPORTED_STACK_MATERIAL",
                           "stack(...) block currently lowers one receiver-only step");
            ++ordinal;
            return false;
        }

        bool emitted = false;
        for (const auto& nestedItem : item.nested->steps.front().items) {
            if (nestedItem.kind == StepItemKind::Receiver) {
                if (nestedItem.receiver.foreign || isStepProcessorCall(nestedItem.receiver.target)) {
                    addUnsupported("V2_UNSUPPORTED_STACK_MATERIAL",
                                   "stack(...) receiver block currently lowers local param receivers only");
                    ++ordinal;
                    return false;
                }
                emitted = lowerReceiver(nestedItem.receiver, target, lastTrigger, astStep,
                                        ordinal++, /*emitForeignReceivers=*/false)
                       || emitted;
                continue;
            }

            if (nestedItem.kind == StepItemKind::GroupedReceiver) {
                for (const auto& groupedTarget : nestedItem.grouped.targets) {
                    if (groupedTarget.contains(".") || isStepProcessorCall(groupedTarget)) {
                        addUnsupported("V2_UNSUPPORTED_STACK_MATERIAL",
                                       "stack(...) grouped receiver block currently lowers local param receivers only");
                        ++ordinal;
                        return false;
                    }
                    ReceiverCall receiver;
                    receiver.target = groupedTarget;
                    receiver.foreign = false;
                    receiver.expr = nestedItem.grouped.expr;
                    emitted = lowerReceiver(receiver, target, lastTrigger, astStep,
                                            ordinal++, /*emitForeignReceivers=*/false)
                           || emitted;
                }
                continue;
            }

            addUnsupported("V2_UNSUPPORTED_STACK_MATERIAL",
                           "stack(...) block currently lowers receiver material only");
            ++ordinal;
            return false;
        }

        if (! emitted && ! item.nested->steps.front().items.empty()) {
            addUnsupported("V2_UNSUPPORTED_STACK_MATERIAL",
                           "stack(...) block did not contain lowerable receiver material");
            ++ordinal;
            return false;
        }
        return true;
    }

    bool selectedBlockOption(const StepItem& item, bool preset, const Material*& selected)
    {
        selected = nullptr;
        if (item.blockOptions.empty()) {
            addUnsupported(preset ? "V2_UNSUPPORTED_PRESET" : "V2_UNSUPPORTED_SELECT",
                           juce::String(preset ? "preset" : "select")
                               + "(...) has no material options");
            return false;
        }

        int index = 0;
        if (! parseStaticNonNegativeInt(item.receiver.expr, index)) {
            addUnsupported(preset ? "V2_UNSUPPORTED_PRESET" : "V2_UNSUPPORTED_SELECT",
                           juce::String(preset ? "preset" : "select")
                               + "(...) dynamic selectors/interp need runtime selection support");
            return false;
        }

        index %= (int) item.blockOptions.size();
        selected = item.blockOptions[(size_t) index].get();
        return selected != nullptr;
    }

    bool lowerSelectItem(const StepItem& item, const juce::String& target,
                         AstNode& astStep, bool emitForeignReceivers,
                         const juce::String& lexicalStatePrefix)
    {
        int staticIndex = 0;
        if (! parseStaticNonNegativeInt(item.receiver.expr, staticIndex)
            || item.receiver.expr.source.containsChar(',')) {
            ParseValue selector;
            juce::String error;
            if (! parseReceiverValue(item.receiver.expr, selector, error)) {
                addUnsupported("V2_UNSUPPORTED_SELECT",
                               error.isEmpty()
                                   ? "select(...) selector does not lower"
                                   : "select(...) selector does not lower: " + error);
                return false;
            }
            if (! validateValueArgDoesNotLookMalformedNumeric(selector, "select(...) selector", error)) {
                addUnsupported("V2_UNSUPPORTED_SELECT",
                               "select(...) selector does not lower"
                                   + (error.isEmpty() ? juce::String() : ": " + error));
                return false;
            }

            std::vector<AstNodePtr> options;
            options.reserve(item.blockOptions.size());
            for (const auto& option : item.blockOptions) {
                if (! option || option->kind != MaterialKind::Sequence) {
                    addUnsupported("V2_UNSUPPORTED_SELECT",
                                   "select(...) currently lowers sequence material options only");
                    return false;
                }

                auto nested = makeSequence();
                bool nestedEmitted = false;
                for (const auto& selectedStep : option->steps) {
                    auto innerStep = makeStep();
                    nestedEmitted = lowerStepForTarget(selectedStep, *option, target,
                                                       *innerStep, true,
                                                       emitForeignReceivers,
                                                       lexicalStatePrefix) || nestedEmitted;
                    nested->steps.push_back(std::move(innerStep));
                }
                if (! nestedEmitted) {
                    addUnsupported("V2_UNSUPPORTED_SELECT",
                                   "select(...) option did not lower any material for the target");
                    return false;
                }
                options.push_back(std::move(nested));
            }

            astStep.events.push_back(makeSelectSelector(std::move(selector),
                                                        std::move(options)));
            return true;
        }

        const Material* selected = nullptr;
        if (! selectedBlockOption(item, /*preset=*/false, selected))
            return false;
        if (selected->kind != MaterialKind::Sequence) {
            addUnsupported("V2_UNSUPPORTED_SELECT",
                           "select(...) currently lowers sequence material options only");
            return false;
        }

        auto nested = makeSequence();
        bool nestedEmitted = false;
        for (const auto& selectedStep : selected->steps) {
            auto innerStep = makeStep();
            nestedEmitted = lowerStepForTarget(selectedStep, *selected, target,
                                               *innerStep, true,
                                               emitForeignReceivers,
                                               lexicalStatePrefix) || nestedEmitted;
            nested->steps.push_back(std::move(innerStep));
        }
        if (! nestedEmitted)
            return false;
        astStep.events.push_back(std::move(nested));
        return true;
    }

    bool parsePresetSelectorArgs(const StepItem& item, ParseValue& selector,
                                 juce::String& mode, juce::String& error) const
    {
        std::vector<juce::String> argSources;
        if (! splitSignalArgs(item.receiver.expr.source, argSources, error))
            return false;
        if (argSources.empty()) {
            error = "preset(...) expects a selector argument";
            return false;
        }
        if (argSources.size() > 2) {
            error = "preset(...) currently accepts selector plus optional interp flag";
            return false;
        }
        if (! parseSignalArgValue(argSources[0], selector, error))
            return false;
        if (selector.kind == ParseValue::Kind::Null) {
            error = "preset(...) selector does not lower";
            return false;
        }
        if (! validateValueArgDoesNotLookMalformedNumeric(selector, "preset(...) selector", error))
            return false;

        mode = "select";
        if (argSources.size() == 2) {
            const auto rawMode = argSources[1].trim().toLowerCase();
            if (rawMode == "interp" || rawMode == "interpolate")
                mode = "interp";
            else if (rawMode != "select") {
                error = "preset(...) second argument must be interp/interpolate or select";
                return false;
            }
        }
        return true;
    }

    bool collectPresetStateReceivers(const Material& state,
                                     std::vector<std::pair<juce::String, ParseValue>>& out,
                                     juce::String& error)
    {
        if (state.kind != MaterialKind::Sequence || state.steps.size() != 1) {
            error = "preset(...) currently lowers one receiver-state step per option";
            return false;
        }

        for (const auto& stateItem : state.steps.front().items) {
            if (stateItem.kind == StepItemKind::Receiver) {
                if (isStepProcessorCall(stateItem.receiver.target)) {
                    error = "preset(...) currently lowers receiver-state material only";
                    return false;
                }
                ParseValue value;
                if (! parseReceiverValueForTarget(stateItem.receiver.target,
                                                  stateItem.receiver.expr,
                                                  value, error))
                    return false;
                out.emplace_back(stateItem.receiver.target, std::move(value));
                continue;
            }
            if (stateItem.kind == StepItemKind::GroupedReceiver) {
                for (const auto& groupedTarget : stateItem.grouped.targets) {
                    ParseValue value;
                    if (! parseReceiverValueForTarget(groupedTarget,
                                                      stateItem.grouped.expr,
                                                      value, error))
                        return false;
                    out.emplace_back(groupedTarget, std::move(value));
                }
                continue;
            }
            error = "preset(...) currently lowers receiver-state material only";
            return false;
        }

        if (out.empty()) {
            error = "preset(...) option has no receiver-state values";
            return false;
        }
        return true;
    }

    bool lowerDynamicPresetItem(const StepItem& item, const juce::String& target,
                                AstNode* lastTrigger, AstNode& astStep,
                                int& ordinal, bool emitForeignReceivers)
    {
        if (item.blockOptions.empty()) {
            addUnsupported("V2_UNSUPPORTED_PRESET", "preset(...) has no material options");
            return false;
        }

        ParseValue selector;
        juce::String mode;
        juce::String error;
        if (! parsePresetSelectorArgs(item, selector, mode, error)) {
            addUnsupported("V2_UNSUPPORTED_PRESET",
                           "preset(...) selector does not lower"
                               + (error.isEmpty() ? juce::String() : ": " + error));
            return false;
        }

        std::vector<juce::String> targets;
        std::vector<std::vector<ParseValue>> valuesByTarget;
        for (size_t optionIndex = 0; optionIndex < item.blockOptions.size(); ++optionIndex) {
            std::vector<std::pair<juce::String, ParseValue>> stateValues;
            if (! collectPresetStateReceivers(*item.blockOptions[optionIndex],
                                              stateValues, error)) {
                addUnsupported("V2_UNSUPPORTED_PRESET",
                               juce::String("preset(...) option ")
                                   + juce::String((int) optionIndex)
                                   + " does not lower"
                                   + (error.isEmpty() ? juce::String() : ": " + error));
                return false;
            }

            if (optionIndex == 0) {
                for (auto& [stateTarget, value] : stateValues) {
                    targets.push_back(stateTarget);
                    std::vector<ParseValue> values;
                    values.push_back(std::move(value));
                    valuesByTarget.push_back(std::move(values));
                }
                continue;
            }

            if (stateValues.size() != targets.size()) {
                addUnsupported("V2_UNSUPPORTED_PRESET",
                               "preset(...) options must expose the same receiver-state targets");
                return false;
            }

            for (size_t i = 0; i < targets.size(); ++i) {
                if (! stateValues[i].first.equalsIgnoreCase(targets[i])) {
                    addUnsupported("V2_UNSUPPORTED_PRESET",
                                   "preset(...) options must expose receiver-state targets in the same order");
                    return false;
                }
                valuesByTarget[i].push_back(std::move(stateValues[i].second));
            }
        }

        bool emitted = false;
        for (size_t i = 0; i < targets.size(); ++i) {
            auto gen = std::make_shared<GeneratorNode>();
            gen->generatorType = "__preset";
            gen->args.push_back(selector);
            ParseValue modeValue;
            modeValue.kind = ParseValue::Kind::String;
            modeValue.str = mode;
            gen->args.push_back(std::move(modeValue));
            for (auto& value : valuesByTarget[i])
                gen->args.push_back(std::move(value));

            ParseValue presetValue;
            presetValue.kind = ParseValue::Kind::Generator;
            presetValue.gen = std::move(gen);
            emitted = lowerParsedReceiverValue(targets[i], std::move(presetValue),
                                               target, lastTrigger, astStep,
                                               ordinal++, emitForeignReceivers)
                   || emitted;
        }
        return emitted;
    }

    bool lowerPresetItem(const StepItem& item, const juce::String& target,
                         AstNode* lastTrigger, AstNode& astStep, int& ordinal,
                         bool emitForeignReceivers)
    {
        int staticIndex = 0;
        if (! parseStaticNonNegativeInt(item.receiver.expr, staticIndex)
            || item.receiver.expr.source.containsChar(',')) {
            return lowerDynamicPresetItem(item, target, lastTrigger, astStep,
                                          ordinal, emitForeignReceivers);
        }

        const Material* selected = nullptr;
        if (! selectedBlockOption(item, /*preset=*/true, selected))
            return false;
        if (selected->kind != MaterialKind::Sequence || selected->steps.size() != 1) {
            addUnsupported("V2_UNSUPPORTED_PRESET",
                           "preset(...) currently lowers one receiver-state step per option");
            return false;
        }

        bool emitted = false;
        for (const auto& stateItem : selected->steps.front().items) {
            if (stateItem.kind == StepItemKind::Receiver) {
                if (isStepProcessorCall(stateItem.receiver.target)) {
                    addUnsupported("V2_UNSUPPORTED_PRESET",
                                   "preset(...) currently lowers receiver-state material only");
                    return false;
                }
                emitted = lowerReceiver(stateItem.receiver, target, lastTrigger, astStep,
                                        ordinal++, emitForeignReceivers) || emitted;
                continue;
            }
            if (stateItem.kind == StepItemKind::GroupedReceiver) {
                emitted = lowerGroupedReceiver(stateItem.grouped, target, lastTrigger, astStep,
                                               ordinal, emitForeignReceivers) || emitted;
                ordinal += (int) stateItem.grouped.targets.size();
                continue;
            }
            addUnsupported("V2_UNSUPPORTED_PRESET",
                           "preset(...) currently lowers receiver-state material only");
            return false;
        }
        return emitted;
    }

    bool lowerStepForTarget(const Step& srcStep, const Material& material,
                            const juce::String& target, AstNode& astStep,
                            bool includeMaterialPostfix = true,
                            bool emitForeignReceivers = true,
                            const juce::String& lexicalStatePrefix = {})
    {
        bool emitted = false;
        AstNode* lastTrigger = nullptr;
        int ordinal = 0;

        for (const auto& item : srcStep.items) {
            if (item.kind == StepItemKind::Note
                || item.kind == StepItemKind::PolyphonicChord) {
                auto ev = makeTriggerEvent(target, item.value);
                lastTrigger = ev.get();
                astStep.events.push_back(std::move(ev));
                emitted = true;
                ++ordinal;
                continue;
            }

            if (item.kind == StepItemKind::Rest) {
                auto ev = makeRestEvent(target);
                lastTrigger = nullptr;
                astStep.events.push_back(std::move(ev));
                emitted = true;
                ++ordinal;
                continue;
            }

            if (item.kind == StepItemKind::Hold) {
                auto ev = makeHoldEvent(target);
                lastTrigger = nullptr;
                astStep.events.push_back(std::move(ev));
                emitted = true;
                ++ordinal;
                continue;
            }

            if (item.kind == StepItemKind::Receiver) {
                if (item.receiver.target.equalsIgnoreCase("chord")) {
                    emitted = lowerChordOntoTrigger(item.receiver, lastTrigger, ordinal)
                           || emitted;
                    continue;
                }
                emitted = lowerReceiver(item.receiver, target, lastTrigger, astStep,
                                        ordinal++, emitForeignReceivers) || emitted;
                continue;
            }

            if (item.kind == StepItemKind::GroupedReceiver) {
                emitted = lowerGroupedReceiver(item.grouped, target, lastTrigger, astStep,
                                               ordinal, emitForeignReceivers) || emitted;
                ordinal += (int) item.grouped.targets.size();
                continue;
            }

            if (item.kind == StepItemKind::Markov) {
                emitted = lowerMarkovItem(item, target, astStep, ordinal++,
                                          lexicalStatePrefix) || emitted;
                lastTrigger = nullptr;
                continue;
            }

            if (item.kind == StepItemKind::Stack) {
                emitted = lowerStackOntoTrigger(item, target, lastTrigger, astStep, ordinal)
                       || emitted;
                continue;
            }

            if (item.kind == StepItemKind::Preset) {
                emitted = lowerPresetItem(item, target, lastTrigger, astStep, ordinal,
                                          emitForeignReceivers) || emitted;
                continue;
            }

            if (item.kind == StepItemKind::Select) {
                emitted = lowerSelectItem(item, target, astStep, emitForeignReceivers,
                                         lexicalStatePrefix) || emitted;
                lastTrigger = nullptr;
                ++ordinal;
                continue;
            }

            if (item.kind == StepItemKind::Tuplet && item.nested) {
                Material nestedMaterial;
                if (! normalizeMaterialForUse(*item.nested, nestedMaterial))
                    return false;
                if (! validatePostfixes(nestedMaterial))
                    return false;
                if (! applyStaticMaterialPostfixes(nestedMaterial))
                    return false;

                auto nested = makeSequence();
                nested->scopeType = "tuplet";
                bool nestedEmitted = false;
                for (const auto& nestedStep : nestedMaterial.steps) {
                    auto innerStep = makeStep();
                    nestedEmitted = lowerStepForTarget(nestedStep, nestedMaterial, target,
                                                       *innerStep, false,
                                                       emitForeignReceivers,
                                                       lexicalStatePrefix) || nestedEmitted;
                    nested->steps.push_back(std::move(innerStep));
                }
                if (nestedEmitted) {
                    if (! appendRouteScopePostfixProcessors(nestedMaterial, *nested))
                        return false;
                    astStep.events.push_back(std::move(nested));
                    emitted = true;
                }
                continue;
            }

            if (item.kind == StepItemKind::Atom && item.value.isNotEmpty()) {
                auto defIt = definitions.find(item.value);
                if (defIt != definitions.end()) {
                    Material resolved;
                    if (! normalizeMaterialForUse(defIt->second, resolved))
                        return false;
                    if (! validatePostfixes(resolved))
                        return false;
                    if (! applyStaticMaterialPostfixes(resolved))
                        return false;
                    auto nested = makeSequence();
                    bool nestedEmitted = false;
                    for (const auto& nestedStep : resolved.steps) {
                        auto innerStep = makeStep();
                        nestedEmitted = lowerStepForTarget(nestedStep, resolved, target,
                                                           *innerStep, false,
                                                           emitForeignReceivers,
                                                           item.value) || nestedEmitted;
                        nested->steps.push_back(std::move(innerStep));
                    }
                    if (nestedEmitted) {
                        if (! appendRouteScopePostfixProcessors(resolved, *nested))
                            return false;
                        astStep.events.push_back(std::move(nested));
                        emitted = true;
                    }
                    continue;
                }
                addUnsupported("V2_UNSUPPORTED_ATOM",
                               "Atom '" + item.value + "' is not lowerable in routed material");
            }
        }

        if (! includeMaterialPostfix)
            return emitted;

        for (const auto& op : material.perItemGroupReceivers) {
            if (op.kind != PostfixKind::GroupReceiver)
                continue;
            ReceiverCall receiver;
            receiver.target = op.name;
            receiver.expr = op.expr;
            emitted = lowerReceiver(receiver, target, lastTrigger, astStep,
                                    ordinal++, emitForeignReceivers) || emitted;
        }
        return emitted;
    }

    bool lowerGroupedReceiver(const GroupedReceiver& grouped, const juce::String& routeTarget,
                              AstNode* lastTrigger, AstNode& astStep, int ordinal,
                              bool emitForeignReceivers = true)
    {
        bool emitted = false;
        for (const auto& groupedTarget : grouped.targets) {
            ReceiverCall receiver;
            receiver.target = groupedTarget;
            receiver.foreign = groupedTarget.contains(".");
            receiver.expr = grouped.expr;
            emitted = lowerReceiver(receiver, routeTarget, lastTrigger, astStep,
                                    ordinal++, emitForeignReceivers) || emitted;
        }
        return emitted;
    }

    bool lowerReceiver(const ReceiverCall& receiver, const juce::String& routeTarget,
                       AstNode* lastTrigger, AstNode& astStep, int ordinal,
                       bool emitForeignReceiver = true)
    {
        if (receiver.target.equalsIgnoreCase("step")) {
            addUnsupported("V2_UNSUPPORTED_LEGACY_STEP_SPELLING",
                           "V2 step duration is step_size(...), not legacy step(...)");
            return false;
        }

        if (receiver.target.startsWithIgnoreCase("session.")) {
            addUnsupported("V2_UNSUPPORTED_SESSION_SCOPE",
                           "Session targets currently lower only as top-level statements");
            return false;
        }

        if (isStepProcessorCall(receiver.target)) {
            std::vector<ParseValue> args;
            juce::String error;
            if (! parsePostfixArgs(receiver.target, receiver.expr, args, error)) {
                addUnsupported("V2_UNSUPPORTED_SIGNAL_EXPR",
                               "Step processor expression '" + receiver.target + "(" + receiver.expr.source + ")"
                                   + "' parses but does not lower in the VM-first subset"
                                   + (error.isEmpty() ? juce::String() : ": " + error));
                return false;
            }
            if (! normalizeProcessorArgsForV2(receiver.target, args, error)) {
                addUnsupported(unsupportedProcessorArgsCode(receiver.target,
                                                            "V2_UNSUPPORTED_SIGNAL_EXPR",
                                                            error),
                               "Step processor expression '" + receiver.target + "(" + receiver.expr.source + ")"
                                   + "' parses but does not lower in the VM-first subset"
                                   + (error.isEmpty() ? juce::String() : ": " + error));
                return false;
            }
            auto proc = makeProcessor(receiver.target.toLowerCase(), std::move(args), ordinal);
            if (lastTrigger != nullptr && lastTrigger->module == routeTarget)
                lastTrigger->processors.push_back(std::move(proc));
            else
                astStep.processors.push_back(std::move(proc));
            return true;
        }

        ParseValue value;
        juce::String signalError;
        if (! parseReceiverValueForTarget(receiver.target, receiver.expr, value, signalError)) {
            addUnsupported("V2_UNSUPPORTED_SIGNAL_EXPR",
                           "Signal expression '" + receiver.expr.source
                               + "' parses but does not lower in the VM-first subset"
                               + (signalError.isEmpty() ? juce::String() : ": " + signalError));
            return false;
        }
        const bool isPolyMapped = containsPolyList(value);
        std::vector<ParseValue> polySlots;
        if (isPolyMapped) {
            polySlots = expandPolySlots(value);
            if (polySlots.empty()) {
                addUnsupported("V2_UNSUPPORTED_POLYPHONIC_EXPRESSION",
                               "Polyphonic expression '" + receiver.expr.source
                                   + "' contains no lowerable slots");
                return false;
            }
        }

        if (receiver.foreign) {
            if (! emitForeignReceiver)
                return false;
            const int dot = receiver.target.indexOfChar('.');
            if (dot <= 0 || dot >= receiver.target.length() - 1) {
                addUnsupported("V2_BAD_FOREIGN_TARGET", "Invalid foreign target '" + receiver.target + "'");
                return false;
            }
            const juce::String module = receiver.target.substring(0, dot);
            const juce::String param = receiver.target.substring(dot + 1);
            astStep.events.push_back(makeUpdateEvent(
                module,
                isPolyMapped ? makeVoiceStackProcessor(param, std::move(polySlots), ordinal)
                             : makeProcessor(param, value, ordinal)));
            return true;
        }

        auto proc = isPolyMapped ? makeVoiceStackProcessor(receiver.target, std::move(polySlots), ordinal)
                                 : makeProcessor(receiver.target, value, ordinal);
        if (lastTrigger != nullptr && lastTrigger->module == routeTarget) {
            lastTrigger->processors.push_back(std::move(proc));
        } else {
            astStep.events.push_back(makeUpdateEvent(routeTarget, std::move(proc)));
        }
        return true;
    }

    bool lowerParsedReceiverValue(const juce::String& receiverTarget,
                                  ParseValue value,
                                  const juce::String& routeTarget,
                                  AstNode* lastTrigger,
                                  AstNode& astStep,
                                  int ordinal,
                                  bool emitForeignReceiver = true)
    {
        const bool isPolyMapped = containsPolyList(value);
        std::vector<ParseValue> polySlots;
        if (isPolyMapped) {
            polySlots = expandPolySlots(value);
            if (polySlots.empty()) {
                addUnsupported("V2_UNSUPPORTED_POLYPHONIC_EXPRESSION",
                               "Polyphonic expression contains no lowerable slots");
                return false;
            }
        }

        if (receiverTarget.contains(".")) {
            if (! emitForeignReceiver)
                return false;
            const int dot = receiverTarget.indexOfChar('.');
            if (dot <= 0 || dot >= receiverTarget.length() - 1) {
                addUnsupported("V2_BAD_FOREIGN_TARGET",
                               "Invalid foreign target '" + receiverTarget + "'");
                return false;
            }
            const juce::String module = receiverTarget.substring(0, dot);
            const juce::String param = receiverTarget.substring(dot + 1);
            astStep.events.push_back(makeUpdateEvent(
                module,
                isPolyMapped ? makeVoiceStackProcessor(param, std::move(polySlots), ordinal)
                             : makeProcessor(param, std::move(value), ordinal)));
            return true;
        }

        auto proc = isPolyMapped
            ? makeVoiceStackProcessor(receiverTarget, std::move(polySlots), ordinal)
            : makeProcessor(receiverTarget, std::move(value), ordinal);
        if (lastTrigger != nullptr && lastTrigger->module == routeTarget)
            lastTrigger->processors.push_back(std::move(proc));
        else
            astStep.events.push_back(makeUpdateEvent(routeTarget, std::move(proc)));
        return true;
    }

    bool appendSessionTarget(const juce::String& target, const SignalExpr& expr, int ordinal)
    {
        if (! target.equalsIgnoreCase("session.bpm")) {
            addUnsupported("V2_UNSUPPORTED_SESSION_TARGET",
                           "Only session.bpm(...) lowers in the first session namespace slice");
            return false;
        }

        std::vector<ParseValue> args;
        juce::String error;
        if (! parsePostfixArgs("bpm", expr, args, error)) {
            addUnsupported("V2_UNSUPPORTED_SESSION_BPM",
                           "session.bpm(...) expects a lowerable tempo value"
                               + (error.isEmpty() ? juce::String() : ": " + error));
            return false;
        }
        if (args.empty()) {
            addUnsupported("V2_UNSUPPORTED_SESSION_BPM",
                           "session.bpm(...) expects a tempo value");
            return false;
        }
        if (args.size() != 1) {
            addUnsupported("V2_UNSUPPORTED_SESSION_BPM",
                           "session.bpm(...) expects exactly one tempo value");
            return false;
        }
        if (! validateTimeDomainValue(args.front(), "session.bpm", error)) {
            addUnsupported("V2_UNSUPPORTED_SESSION_BPM",
                           "session.bpm(...) expects a strict tempo value"
                               + (error.isEmpty() ? juce::String() : ": " + error));
            return false;
        }

        out.ast.statements.push_back(makePersistentProcessorSetter(
            makeProcessor("bpm", std::move(args), ordinal)));
        return true;
    }

    double bpm_ = 120.0;
};

static juce::String firstDiagnosticMessage(const std::vector<Diagnostic>& diagnostics)
{
    if (diagnostics.empty()) return {};
    return diagnostics.front().code + ": " + diagnostics.front().message;
}

} // namespace

Program parseProgramV2(const juce::String& source)
{
    Lexer lexer(source);
    Parser parser(source, lexer.run());
    return parser.parse();
}

ParseResult parseV2(const juce::String& source)
{
    auto program = parseProgramV2(source);
    auto report = programToVar(program);

    ParseResult out;
    out.typedProgram = program;
    out.program = report;
    out.diagnostics = program.diagnostics;
    out.reportJson = juce::JSON::toString(report, true);
    out.reportText = buildReportText(program);
    return out;
}

LowerResult lowerV2ToScriptAst(const Program& program, double bpm)
{
    LowerResult out;
    if (hasErrorDiagnostic(program.diagnostics)) {
        out.diagnostics = program.diagnostics;
        return out;
    }
    Lowerer lowerer(bpm);
    return lowerer.lower(program);
}

bool isChannelRouteSyntax(const Statement& statement)
{
    if (statement.kind != StatementKind::SocketRoute
        || statement.body.chain.empty())
        return false;
    return statement.body.chain.front().trimStart()
        .startsWithIgnoreCase("channels(");
}

bool parseChannelRoute(const Statement& statement, ChannelRoute& route,
                       juce::String& error)
{
    route = {};
    error = {};
    if (! isChannelRouteSyntax(statement))
        return false;
    if (statement.body.chain.size() != 1) {
        error = "channels(...) does not accept scalar channel transforms; "
                "route the selected channels through another Script module";
        return false;
    }

    const auto call = statement.body.chain.front().trim();
    if (! call.endsWithChar(')')) {
        error = "channels(...) channel selector is missing its closing ')'";
        return false;
    }
    const auto open = call.indexOfChar('(');
    if (open < 0 || ! call.substring(0, open).trim().equalsIgnoreCase("channels")) {
        error = "Invalid channels(...) channel selector";
        return false;
    }

    std::vector<juce::String> references;
    if (! splitSignalArgs(call.substring(open + 1, call.length() - 1),
                          references, error)) {
        error = "Invalid channels(...) channel arguments: " + error;
        return false;
    }
    if (references.empty()) {
        error = "channels(...) requires at least one addressed channel";
        return false;
    }

    route.output = statement.name;
    route.references.reserve(references.size());
    for (auto raw : references) {
        raw = raw.trim();
        ChannelReference reference;
        if (raw.startsWithChar('<')) {
            reference.sourceKind = curlop::vm::ChannelSourceKind::PacketInput;
            raw = raw.substring(1).trim();
        }

        const auto dot = raw.lastIndexOfChar('.');
        if (dot <= 0 || dot == raw.length() - 1) {
            error = "Each channels(...) argument must end in an explicit "
                    "unsigned channel index";
            return false;
        }
        reference.signal = raw.substring(0, dot).trim();
        const auto indexText = raw.substring(dot + 1).trim();
        if (reference.signal.isEmpty() || indexText.isEmpty()) {
            error = "Each channels(...) argument needs a signal and channel index";
            return false;
        }

        std::uint64_t value = 0;
        for (int i = 0; i < indexText.length(); ++i) {
            const auto c = indexText[i];
            if (c < '0' || c > '9') {
                error = "Channel indices in channels(...) must be unsigned integers";
                return false;
            }
            value = value * 10u + static_cast<unsigned>(c - '0');
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                error = "Channel index in channels(...) exceeds the U32 limit";
                return false;
            }
        }
        reference.channel = static_cast<std::uint32_t>(value);
        route.references.push_back(std::move(reference));
    }
    return true;
}

static void addInputSocketName(std::vector<juce::String>& out, std::set<juce::String>& seen,
                               const juce::String& name)
{
    if (name.isEmpty() || seen.count(name) != 0)
        return;
    seen.insert(name);
    out.push_back(name);
}

static void collectInputSocketNamesFromExpr(const SignalExpr& expr,
                                            std::vector<juce::String>& out,
                                            std::set<juce::String>& seen)
{
    for (const auto& name : expr.inputReads)
        addInputSocketName(out, seen, name);
}

static void collectInputSocketNamesFromMaterial(const Material& material,
                                                std::vector<juce::String>& out,
                                                std::set<juce::String>& seen)
{
    if (material.kind != MaterialKind::Sequence)
        return;
    for (const auto& step : material.steps) {
        for (const auto& item : step.items) {
            if (item.kind == StepItemKind::Receiver)
                collectInputSocketNamesFromExpr(item.receiver.expr, out, seen);
            else if (item.kind == StepItemKind::GroupedReceiver)
                collectInputSocketNamesFromExpr(item.grouped.expr, out, seen);
            else if (item.kind == StepItemKind::Markov
                     || item.kind == StepItemKind::Stack
                     || item.kind == StepItemKind::Preset
                     || item.kind == StepItemKind::Select)
                collectInputSocketNamesFromExpr(item.receiver.expr, out, seen);

            if ((item.kind == StepItemKind::Tuplet || item.kind == StepItemKind::Stack)
                && item.nested)
                collectInputSocketNamesFromMaterial(*item.nested, out, seen);
            for (const auto& option : item.blockOptions)
                if (option)
                    collectInputSocketNamesFromMaterial(*option, out, seen);
        }
    }
    for (const auto& op : material.postfix)
        collectInputSocketNamesFromExpr(op.expr, out, seen);
}

static std::vector<juce::String> collectInputSocketNamesFromProgram(const Program& program)
{
    std::vector<juce::String> out;
    std::set<juce::String> seen;
    for (const auto& stmt : program.statements) {
        collectInputSocketNamesFromExpr(stmt.body, out, seen);
        collectInputSocketNamesFromMaterial(stmt.material, out, seen);
        for (const auto& selector : stmt.selectors)
            collectInputSocketNamesFromExpr(selector.expr, out, seen);
    }
    return out;
}

static std::optional<double> staticTopLevelSessionBpm(const Program& program)
{
    for (const auto& stmt : program.statements) {
        if (stmt.kind != StatementKind::SessionTarget
            || ! stmt.name.equalsIgnoreCase("session.bpm"))
            continue;

        juce::String error;
        std::vector<juce::String> argSources;
        if (! splitSignalArgs(stmt.body.source, argSources, error) || argSources.empty())
            return std::nullopt;

        ParseValue value;
        if (! parseSignalArgValue(argSources.front(), value, error))
            return std::nullopt;

        if (value.kind == ParseValue::Kind::Number)
            return value.number;
        if (value.kind == ParseValue::Kind::UnitNumber) {
            if (value.unit == "ms") return value.number > 0.0
                                        ? std::optional<double>{ 60000.0 / value.number }
                                        : std::nullopt;
            if (value.unit == "s")  return value.number > 0.0
                                        ? std::optional<double>{ 60.0 / value.number }
                                        : std::nullopt;
            if (value.unit == "hz") return value.number * 60.0;
            return value.number;
        }
        if (value.kind == ParseValue::Kind::String) {
            const auto s = value.str.toLowerCase();
            if (s.endsWith("khz")) {
                double khz = 0.0;
                if (! parseStrictDoubleLiteral(s.dropLastCharacters(3), &khz))
                    return std::nullopt;
                const double hz = khz * 1000.0;
                return hz > 0.0 ? std::optional<double>{ hz * 60.0 } : std::nullopt;
            }
            if (s.endsWith("hz")) {
                double hz = 0.0;
                if (! parseStrictDoubleLiteral(s.dropLastCharacters(2), &hz))
                    return std::nullopt;
                return hz > 0.0 ? std::optional<double>{ hz * 60.0 } : std::nullopt;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

curlop::script::CompileResultV2 compileV2Syntax(const juce::String& source,
                                                const curlop::script::CompileOptions& opts)
{
    curlop::script::CompileResultV2 result;
    auto parsed = parseProgramV2(source);
    if (hasErrorDiagnostic(parsed.diagnostics)) {
        result.compileError = firstDiagnosticMessage(parsed.diagnostics);
        return result;
    }

    const double lowererBpm = staticTopLevelSessionBpm(parsed)
                                  .value_or(opts.bpmOverride > 0.0 ? opts.bpmOverride : 120.0);
    auto lowered = lowerV2ToScriptAst(parsed, lowererBpm);
    if (! lowered.diagnostics.empty()) {
        result.compileError = firstDiagnosticMessage(lowered.diagnostics);
        return result;
    }

    std::vector<ChannelRoute> channelRoutes;
    for (const auto& statement : parsed.statements) {
        if (! isChannelRouteSyntax(statement))
            continue;
        ChannelRoute route;
        juce::String error;
        if (! parseChannelRoute(statement, route, error)) {
            result.compileError = "V2_INVALID_CHANNEL_ROUTE: " + error;
            return result;
        }
        channelRoutes.push_back(std::move(route));
    }

    auto optsWithInputs = opts;
    optsWithInputs.inputSocketNames = collectInputSocketNamesFromProgram(parsed);
    result = curlop::script::compileV2(lowered.ast, optsWithInputs);
    if (result.compileError.isNotEmpty())
        return result;

    auto ownerIt = std::find_if(
        result.modulePrograms.begin(), result.modulePrograms.end(),
        [] (const auto& program) { return program.localOutputOwner; });
    if (ownerIt == result.modulePrograms.end() && ! channelRoutes.empty()) {
        if (opts.localOutputTarget.isEmpty()) {
            result.compileError =
                "V2_CHANNEL_PLAN: channels(...) requires a local Script output owner";
            return result;
        }
        curlop::script::ModuleProgramV2 owner;
        owner.dslName = opts.localOutputTarget;
        owner.localOutputOwner = true;
        const auto seeded = std::find_if(
            opts.moduleIndicesSeed.begin(), opts.moduleIndicesSeed.end(),
            [&opts] (const auto& entry) {
                return entry.first == opts.localOutputTarget;
            });
        owner.moduleIdx = seeded == opts.moduleIndicesSeed.end()
            ? 0 : seeded->second;
        curlop::vm::ProgramBuilder emptyProgram;
        emptyProgram.loop(1.0).halt();
        owner.program = emptyProgram.build();
        result.modulePrograms.push_back(std::move(owner));
        if (std::none_of(result.moduleIndices.begin(), result.moduleIndices.end(),
                         [&opts] (const auto& entry) {
                             return entry.first == opts.localOutputTarget;
                         }))
            result.moduleIndices.push_back({ opts.localOutputTarget,
                                             result.modulePrograms.back().moduleIdx });
        ownerIt = std::prev(result.modulePrograms.end());
    }
    if (ownerIt == result.modulePrograms.end())
        return result;

    auto& owner = *ownerIt;
    std::unordered_map<std::uint32_t, std::uint32_t> localEmissionWidths;
    {
        const auto& code = owner.program.code;
        std::size_t pc = 0;
        while (pc < code.size()) {
            const auto* spec = curlop::vm::findOp(code[pc++]);
            if (spec == nullptr)
                break;
            const auto operandStart = pc;
            const auto bytes = curlop::vm::operandBytes(
                spec->operands, code.data() + pc, code.size() - pc);
            if (bytes == SIZE_MAX)
                break;
            if ((spec->opcode == curlop::vm::opcode(curlop::vm::Op::LocalOutput)
                 || spec->opcode
                        == curlop::vm::opcode(curlop::vm::Op::LocalGateOnly))
                && bytes >= 8u) {
                std::uint32_t lane = 0;
                std::uint32_t channel = 0;
                std::memcpy(&lane, code.data() + operandStart, sizeof(lane));
                std::memcpy(&channel, code.data() + operandStart + 4u,
                            sizeof(channel));
                auto& width = localEmissionWidths[lane];
                width = std::max(width, channel + 1u);
            }
            pc += bytes;
        }
    }
    curlop::vm::ProgramBuilder builder(std::move(owner.program));
    const auto laneFor = [&owner] (const juce::String& name) {
        const auto found = std::find(owner.laneParams.begin(), owner.laneParams.end(), name);
        if (found != owner.laneParams.end())
            return static_cast<std::uint32_t>(
                std::distance(owner.laneParams.begin(), found));
        owner.laneParams.push_back(name);
        owner.laneInternal.push_back(0u);
        return static_cast<std::uint32_t>(owner.laneParams.size() - 1u);
    };
    const auto stableId = [] (const juce::String& output, std::size_t ordinal,
                              const ChannelReference& reference) {
        std::uint64_t hash = 14695981039346656037ull;
        const auto text = output.toStdString() + "#" + std::to_string(ordinal)
            + "#" + reference.signal.toStdString() + "#"
            + std::to_string(reference.channel) + "#"
            + std::to_string(static_cast<unsigned>(reference.sourceKind));
        for (const auto byte : text) {
            hash ^= static_cast<unsigned char>(byte);
            hash *= 1099511628211ull;
        }
        return hash == 0u ? 1u : hash;
    };
    const auto appendPlan = [&builder] (curlop::vm::ChannelPlan plan,
                                        juce::String& error) {
        if (std::any_of(
                builder.program().channelPlans.begin(),
                builder.program().channelPlans.end(),
                [&plan] (const auto& existing) {
                    return existing.outputSignal == plan.outputSignal;
                })) {
            error = "A channel output may have only one compiled channel plan";
            return false;
        }
        std::uint32_t index = 0;
        if (! builder.addChannelPlan(plan, index)
            || ! builder.channelApplyBeforeTrailingHalt(index)) {
            error = "Unable to install the compiled channel plan";
            return false;
        }
        return true;
    };

    // A local stack is a real addressed channel set, not merely N duplicate
    // voice rows. Only emitted local-output lanes receive this descriptor.
    for (const auto& [lane, width] : localEmissionWidths) {
        if (width <= 1u || lane >= owner.laneParams.size()
            || (lane < owner.laneInternal.size()
                && owner.laneInternal[lane] != 0u))
            continue;
        const auto output = owner.laneParams[lane];
        const auto declared = std::find_if(
            owner.localOutputs.begin(), owner.localOutputs.end(),
            [&output] (const auto& candidate) { return candidate.name == output; });
        if (declared == owner.localOutputs.end())
            continue;

        curlop::vm::ChannelPlan plan;
        plan.outputSignal = lane;
        plan.descriptorBacked = true;
        plan.entries.reserve(width);
        for (std::uint32_t channel = 0; channel < width; ++channel) {
            ChannelReference reference;
            reference.signal = output;
            reference.channel = channel;
            curlop::vm::ChannelPlanEntry entry;
            entry.sourceKind = curlop::vm::ChannelSourceKind::LocalEmission;
            entry.sourceSignal = lane;
            entry.sourceChannel = reference.channel;
            entry.outputStableChannelId =
                static_cast<std::uint64_t>(channel) + 1u;
            plan.entries.push_back(entry);
        }
        juce::String error;
        if (! appendPlan(std::move(plan), error)) {
            result.compileError = "V2_CHANNEL_PLAN: " + error;
            return result;
        }
    }

    for (const auto& route : channelRoutes) {
        const auto outputLane = laneFor(route.output);
        const auto declaration = std::find_if(
            owner.localOutputs.begin(), owner.localOutputs.end(),
            [&route] (const auto& candidate) { return candidate.name == route.output; });
        if (declaration == owner.localOutputs.end()) {
            curlop::script::ModuleProgramV2::LocalOutputDecl output;
            output.name = route.output;
            const auto leaf = route.output.fromLastOccurrenceOf(".", false, false)
                                  .toLowerCase();
            output.type = leaf == "gate"
                ? curlop::vm::SignalType::Gate
                : (leaf == "pitch"
                    ? curlop::vm::SignalType::Pitch
                    : ((leaf == "velocity" || leaf == "vel")
                        ? curlop::vm::SignalType::Velocity
                        : curlop::vm::SignalType::Value));
            output.def = output.type == curlop::vm::SignalType::Velocity ? 0.7f
                       : output.type == curlop::vm::SignalType::Gate ? 1.0f : 0.0f;
            output.max = output.type == curlop::vm::SignalType::Pitch ? 0.0f : 1.0f;
            owner.localOutputs.push_back(std::move(output));
        }

        curlop::vm::ChannelPlan plan;
        plan.outputSignal = outputLane;
        plan.descriptorBacked = true;
        plan.entries.reserve(route.references.size());
        const bool preservePacketIdentity =
            std::all_of(
                route.references.begin(), route.references.end(),
                [] (const auto& reference) {
                    return reference.sourceKind
                        == curlop::vm::ChannelSourceKind::PacketInput;
                })
            && std::all_of(
                route.references.begin(), route.references.end(),
                [&route] (const auto& reference) {
                    return reference.signal
                        == route.references.front().signal;
                })
            && std::none_of(
                route.references.begin(), route.references.end(),
                [&route] (const auto& reference) {
                    return std::count_if(
                               route.references.begin(),
                               route.references.end(),
                               [&reference] (const auto& candidate) {
                                   return candidate.signal == reference.signal
                                       && candidate.channel
                                           == reference.channel;
                               })
                        > 1;
                });
        std::vector<std::uint64_t> sourceStableIds;
        sourceStableIds.reserve(route.references.size());
        for (std::size_t ordinal = 0; ordinal < route.references.size(); ++ordinal) {
            const auto& reference = route.references[ordinal];
            std::uint32_t sourceLane = 0;
            if (reference.sourceKind == curlop::vm::ChannelSourceKind::LocalEmission) {
                const auto source = std::find(
                    owner.laneParams.begin(), owner.laneParams.end(),
                    reference.signal);
                if (source == owner.laneParams.end()) {
                    result.compileError =
                        "V2_INVALID_CHANNEL_ROUTE: Unknown local channel signal "
                        + reference.signal;
                    return result;
                }
                sourceLane = static_cast<std::uint32_t>(
                    std::distance(owner.laneParams.begin(), source));
                const auto sourcePlan = std::find_if(
                    builder.program().channelPlans.begin(),
                    builder.program().channelPlans.end(),
                    [sourceLane] (const auto& candidate) {
                        return candidate.outputSignal == sourceLane;
                    });
                const auto emitted = localEmissionWidths.find(sourceLane);
                if (sourcePlan != builder.program().channelPlans.end()
                    && emitted == localEmissionWidths.end()) {
                    result.compileError =
                        "V2_INVALID_CHANNEL_ROUTE: A channels(...) plan output "
                        "must cross a Script-module wire before another channel "
                        "transform can address it";
                    return result;
                }
                if (sourcePlan == builder.program().channelPlans.end()
                    && emitted == localEmissionWidths.end()) {
                    result.compileError =
                        "V2_INVALID_CHANNEL_ROUTE: Local channel signal "
                        + reference.signal + " has no compiled emission";
                    return result;
                }
                const auto available = sourcePlan
                                             == builder.program().channelPlans.end()
                    ? emitted->second
                    : static_cast<std::uint32_t>(sourcePlan->entries.size());
                if (reference.channel >= available) {
                    result.compileError =
                        "V2_INVALID_CHANNEL_ROUTE: Local channel reference "
                        + reference.signal + "." + juce::String(reference.channel)
                        + " is outside its compiled channel width";
                    return result;
                }
                sourceStableIds.push_back(
                    sourcePlan == builder.program().channelPlans.end()
                        ? static_cast<std::uint64_t>(reference.channel) + 1u
                        : sourcePlan->entries[reference.channel]
                              .outputStableChannelId);
            } else {
                sourceLane = laneFor(reference.signal);
                builder.packetInput(sourceLane, reference.channel);
                sourceStableIds.push_back(0u);
            }

            curlop::vm::ChannelPlanEntry entry;
            entry.sourceKind = reference.sourceKind;
            entry.sourceSignal = sourceLane;
            entry.sourceChannel = reference.channel;
            entry.outputStableChannelId =
                reference.sourceKind
                        == curlop::vm::ChannelSourceKind::LocalEmission
                    ? sourceStableIds.back()
                    : stableId(route.output, ordinal, reference);
            entry.preserveSourceStableChannelId =
                reference.sourceKind
                        == curlop::vm::ChannelSourceKind::PacketInput
                    && preservePacketIdentity;
            plan.entries.push_back(entry);
        }
        for (std::size_t ordinal = 0; ordinal < plan.entries.size(); ++ordinal) {
            if (plan.entries[ordinal].sourceKind
                != curlop::vm::ChannelSourceKind::LocalEmission)
                continue;
            const auto duplicateReference = std::count_if(
                plan.entries.begin(), plan.entries.end(),
                [&entry = plan.entries[ordinal]] (const auto& candidate) {
                    return candidate.sourceKind == entry.sourceKind
                        && candidate.sourceSignal == entry.sourceSignal
                        && candidate.sourceChannel == entry.sourceChannel;
                }) > 1;
            const auto collidingIdentity = std::count(
                sourceStableIds.begin(), sourceStableIds.end(),
                sourceStableIds[ordinal]) > 1;
            if (duplicateReference || collidingIdentity)
                plan.entries[ordinal].outputStableChannelId =
                    stableId(route.output, ordinal, route.references[ordinal]);
        }
        juce::String error;
        if (! appendPlan(std::move(plan), error)) {
            result.compileError = "V2_CHANNEL_PLAN: " + error;
            return result;
        }
    }
    owner.program = builder.build();
    return result;
}

juce::var referenceManifestV2()
{
    auto object = [] { return juce::var(new juce::DynamicObject()); };
    auto array = [] { return juce::var(juce::Array<juce::var>()); };
    auto put = [] (juce::var& target, const char* key, const juce::var& value) {
        target.getDynamicObject()->setProperty(key, value);
    };
    auto strings = [&array] (const juce::String& packedValues) {
        auto out = array();
        for (const auto& value : juce::StringArray::fromTokens(packedValues, "\x1f", ""))
            out.getArray()->add(value);
        return out;
    };

    auto root = object();
    put(root, "schema", "curlop.v2.native-language-reference");
    put(root, "schemaVersion", 1);
    put(root, "authority", "ScriptParserV2 parser and lowerer");
    put(root, "source", "native/control/surfaces/script/ScriptParserV2.cpp");
    auto entries = array();

    auto add = [&] (juce::String id, juce::String name, juce::String category,
                    juce::String syntax, juce::String contexts,
                    juce::String support, juce::String parserBinding,
                    juce::String compilerBinding, const char* completionLabel,
                    const char* completionInsert, const char* completionSummary,
                    const char* completionKind) {
        auto entry = object();
        put(entry, "id", id);
        put(entry, "name", name);
        put(entry, "category", category);
        put(entry, "syntax", strings(syntax));
        put(entry, "contexts", strings(contexts));
        put(entry, "support", support);
        put(entry, "parserSource", "native/control/surfaces/script/ScriptParserV2.cpp");
        put(entry, "compilerSource", "native/control/surfaces/script/ScriptCompiler.cpp");
        put(entry, "runtimeSource", "native/control/surfaces/script/ScriptNodeProcessor.h");
        put(entry, "parserBinding", parserBinding);
        put(entry, "compilerBinding", compilerBinding);
        if (completionLabel != nullptr) {
            auto completion = object();
            put(completion, "label", completionLabel);
            put(completion, "insert", completionInsert);
            put(completion, "summary", completionSummary);
            put(completion, "kind", completionKind);
            put(entry, "completion", completion);
        }
        entries.getArray()->add(entry);
    };

#define V2_CATALOGUE_ENTRY(id, name, category, syntax, contexts, support, parser, compiler, label, insert, summary, kind) \
    add(id, name, category, syntax, contexts, support, parser, compiler, label, insert, summary, kind);
#include "ScriptV2Catalogue.generated.inc"
#undef V2_CATALOGUE_ENTRY

    put(root, "entries", entries);
    return root;
}

} // namespace curlop::script::v2
