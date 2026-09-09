// ═══════════════════════════════════════════════════════════════════════════
// ScriptParser.cpp — Cut 0.D-2a.
// Hand-written recursive-descent port of src/sequencer/Parser.js.
// Tokenizer: complete (mirrors Parser.js:150-950).
// Parser:    minimal — Script/Assignment/Sequence/SeqBody-basic only.
//            Unported forms emit "NYI-0D2b: <feature>" warnings + resync.
// ═══════════════════════════════════════════════════════════════════════════

#include "control/surfaces/script/ScriptParser.h"
#include "modules/contract/ModuleTypes.h"   // curlop::escapeJsonString
#include "control/surfaces/script/ScriptLanguage.h" // canonicalize / kStepLevelProcs / kChordNames / kCondValues / kCondArgValues / kNoArgOps

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace curlop::script {

namespace {

// ─── Bracket pair helpers ─────────────────────────────────────────────────

static TokenType closerFor(TokenType open)
{
    switch (open) {
        case TokenType::LBRACKET: return TokenType::RBRACKET;
        case TokenType::LPAREN:   return TokenType::RPAREN;
        case TokenType::LBRACE:   return TokenType::RBRACE;
        default:                  return TokenType::UNKNOWN;
    }
}

static const char* bracketChar(TokenType t)
{
    switch (t) {
        case TokenType::LBRACKET: return "[";
        case TokenType::RBRACKET: return "]";
        case TokenType::LPAREN:   return "(";
        case TokenType::RPAREN:   return ")";
        case TokenType::LBRACE:   return "{";
        case TokenType::RBRACE:   return "}";
        default:                  return "?";
    }
}

// ─── Tokenizer ────────────────────────────────────────────────────────────

struct BracketFrame { TokenType type; int line; int col; };

class Tokenizer
{
public:
    Tokenizer(const juce::String& src) : src_(src), chars_(src.toRawUTF8()) {}

    struct Result {
        std::vector<Token>            tokens;
        std::vector<juce::String>     errors;
        std::vector<ScriptDiagnostic> diagnostics;
    };

    Result tokenize()
    {
        const int n = (int) src_.getNumBytesAsUTF8();
        while (pos_ < n) {
            skipWhitespaceAndComments();
            if (pos_ >= n) break;
            readNext();
        }

        for (auto const& open : bracketStack_) {
            const auto closer = closerFor(open.type);
            juce::String msg;
            msg << "Unclosed '" << bracketChar(open.type)
                << "' opened at line " << open.line << ", column " << open.col;
            errors_.push_back(msg);
            diagnostics_.push_back({ Severity::Error, msg, open.line, open.col,
                juce::String(bracketChar(open.type)),
                juce::String(bracketChar(closer)),
                juce::String("Add a closing '") + bracketChar(closer)
                    + "' to match the '" + bracketChar(open.type)
                    + "' at line " + juce::String(open.line)
                    + ", column " + juce::String(open.col) });
        }

        emit(TokenType::EOF_, "", line_, col_);
        return { std::move(tokens_), std::move(errors_), std::move(diagnostics_) };
    }

private:
    // ── State ─────────────────────────────────────────────────────────────
    const juce::String&           src_;
    const char*                   chars_ = nullptr;
    int                           pos_   = 0;
    int                           line_  = 1;
    int                           col_   = 1;
    std::vector<Token>            tokens_;
    std::vector<BracketFrame>     bracketStack_;
    std::vector<juce::String>     errors_;
    std::vector<ScriptDiagnostic> diagnostics_;

    // ── Byte-level helpers (ASCII-only — DSL uses ASCII) ──────────────────
    char peekAt(int off) const
    {
        const int i = pos_ + off;
        const int n = (int) src_.getNumBytesAsUTF8();
        return (i >= 0 && i < n) ? chars_[i] : '\0';
    }

    bool hasAt(int off) const
    {
        return (pos_ + off) < (int) src_.getNumBytesAsUTF8();
    }

    void advance(int nBytes)
    {
        for (int i = 0; i < nBytes && pos_ < (int) src_.getNumBytesAsUTF8(); ++i) {
            const char ch = chars_[pos_++];
            if (ch == '\n') { ++line_; col_ = 1; }
            else            { ++col_; }
        }
    }

    void emit(TokenType type, juce::String value, int line, int col)
    {
        tokens_.push_back({ type, std::move(value), line, col });
    }

    static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    static bool isDigit(char c) { return c >= '0' && c <= '9'; }
    static bool isAlnumUS(char c) { return isAlpha(c) || isDigit(c) || c == '_'; }
    static bool isIdentStart(char c) { return isAlpha(c) || c == '_'; }
    static char toLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

    // Match literal (case-insensitive). Returns bytes matched, 0 if no match.
    int matchLiteralCI(const char* lit, int offset = 0) const
    {
        int i = 0;
        while (lit[i] != '\0') {
            if (toLowerAscii(peekAt(offset + i)) != toLowerAscii(lit[i])) return 0;
            ++i;
        }
        return i;
    }

    // ── Whitespace + comments ─────────────────────────────────────────────
    void skipWhitespaceAndComments()
    {
        bool changed = true;
        while (changed) {
            changed = false;

            // Metadata directive: @name(...) / @color(...) / @tempo(...) — skip to newline or EOF
            if (peekAt(0) == '@') {
                const char* dirs[] = { "name", "color", "tempo" };
                int prefixLen = 0;
                for (const char* d : dirs) {
                    int L = (int) std::strlen(d);
                    if (matchLiteralCI(d, 1) == L) {
                        int p = 1 + L;
                        while (peekAt(p) == ' ' || peekAt(p) == '\t') ++p;
                        if (peekAt(p) == '(') {
                            // consume through ')' or newline
                            int q = p + 1;
                            while (hasAt(q) && peekAt(q) != ')' && peekAt(q) != '\n') ++q;
                            if (peekAt(q) == ')') ++q;
                            prefixLen = q;
                            break;
                        }
                    }
                }
                if (prefixLen > 0) { advance(prefixLen); changed = true; continue; }
            }

            // Inline mute **text** — must check before bare ** line comment
            if (peekAt(0) == '*' && peekAt(1) == '*') {
                // look for closing **
                int q = 2;
                while (hasAt(q) && peekAt(q) != '\n') {
                    if (peekAt(q) == '*' && peekAt(q + 1) == '*') { q += 2; advance(q); changed = true; goto cont; }
                    ++q;
                }
                // No closing ** → treat as line comment
                while (hasAt(q) && peekAt(q) != '\n') ++q;
                advance(q);
                changed = true;
                continue;
            }

            // Horizontal whitespace (tabs + spaces + carriage returns, NOT
            // newlines). B-285: CRLF-ending scripts (Windows pastes, some
            // editor paths) carried '\r' into the token stream — the lexer
            // rejected it as an unexpected character, the node program
            // failed to parse, zero machines installed, and play was
            // SILENT with no visible error. '\r' is whitespace; '\n'
            // remains the sole statement terminator.
            if (peekAt(0) == ' ' || peekAt(0) == '\t' || peekAt(0) == '\r') {
                int q = 0;
                while (peekAt(q) == ' ' || peekAt(q) == '\t' || peekAt(q) == '\r') ++q;
                advance(q);
                changed = true;
                continue;
            }

            cont: ;
        }
    }

    // ── Main token dispatch ───────────────────────────────────────────────
    void readNext()
    {
        const int line = line_;
        const int col  = col_;
        const char ch0 = peekAt(0);

        // Statement terminator: newline
        if (ch0 == '\n') { emit(TokenType::NEWLINE, "\n", line, col); advance(1); return; }

        // Semicolon: not a terminator per spec §1 — error + skip
        if (ch0 == ';') {
            juce::String msg;
            msg << "Semicolons are not valid in CURLOP DSL at line " << line << ", column " << col
                << " — use newline as statement terminator";
            errors_.push_back(msg);
            diagnostics_.push_back({ Severity::Error, msg, line, col, ";", "newline",
                "Replace ; with a newline to separate statements" });
            advance(1);
            return;
        }

        // FREQ: [0-9]+(\.[0-9]+)?(khz|hz)  — BEFORE NUMBER
        if (int L = tryMatchFreq()) {
            juce::String val = juce::String::fromUTF8(chars_ + pos_, L).toLowerCase();
            emit(TokenType::FREQ, val, line, col); advance(L); return;
        }

        // CENTS: [+-]?[0-9]+c(?![a-z0-9_])  — BEFORE HYPHEN/PLUS/NUMBER
        if (int L = tryMatchCents()) {
            juce::String val = juce::String::fromUTF8(chars_ + pos_, L).toLowerCase();
            emit(TokenType::CENTS, val, line, col); advance(L); return;
        }

        // NOTE: [a-g][#b]?[0-9](?![a-z0-9_#])  — BEFORE CHORD/IDENT
        if (int L = tryMatchNote()) {
            juce::String val = juce::String::fromUTF8(chars_ + pos_, L).toLowerCase();
            emit(TokenType::NOTE, val, line, col); advance(L); return;
        }

        // NUMBER: integer or decimal
        if (int L = tryMatchNumber()) {
            juce::String val = juce::String::fromUTF8(chars_ + pos_, L);
            emit(TokenType::NUMBER, val, line, col); advance(L); return;
        }

        // IDENT (and UNDERSCORE for standalone `_`)
        if (isIdentStart(ch0)) {
            int L = 1;
            while (isAlnumUS(peekAt(L))) ++L;
            juce::String val = juce::String::fromUTF8(chars_ + pos_, L).toLowerCase();
            const TokenType ty = (val == "_") ? TokenType::UNDERSCORE : TokenType::IDENT;
            emit(ty, val, line, col); advance(L); return;
        }

        // Single-character tokens
        switch (ch0) {
            case '@': emit(TokenType::AT,      "@", line, col); advance(1); return;
            case ':': emit(TokenType::COLON,   ":", line, col); advance(1); return;
            case '.': emit(TokenType::DOT,     ".", line, col); advance(1); return;
            case '/': emit(TokenType::SLASH,   "/", line, col); advance(1); return;
            case ',': emit(TokenType::COMMA,   ",", line, col); advance(1); return;
            case '-': emit(TokenType::HYPHEN,  "-", line, col); advance(1); return;
            case '+': emit(TokenType::PLUS,    "+", line, col); advance(1); return;
            case '%': emit(TokenType::PERCENT, "%", line, col); advance(1); return;
            case '$': emit(TokenType::DOLLAR,  "$", line, col); advance(1); return;
            case '=': emit(TokenType::ASSIGN,  "=", line, col); advance(1); return;
            case '!': emit(TokenType::BANG,    "!", line, col); advance(1); return;
            case '~': emit(TokenType::TILDE,   "~", line, col); advance(1); return;
            case '#': emit(TokenType::HASH,    "#", line, col); advance(1); return;
            case '>': emit(TokenType::GT,      ">", line, col); advance(1); return;
            case '[': openBracket(TokenType::LBRACKET, "[", line, col); advance(1); return;
            case ']': closeBracket(TokenType::RBRACKET, "]", line, col); advance(1); return;
            case '(': openBracket(TokenType::LPAREN,   "(", line, col); advance(1); return;
            case ')': closeBracket(TokenType::RPAREN,   ")", line, col); advance(1); return;
            case '{': openBracket(TokenType::LBRACE,   "{", line, col); advance(1); return;
            case '}': closeBracket(TokenType::RBRACE,   "}", line, col); advance(1); return;
            default: {
                juce::String msg;
                msg << "Unexpected character '" << juce::String::charToString((juce::juce_wchar) ch0)
                    << "' at line " << line << ", column " << col;
                errors_.push_back(msg);
                diagnostics_.push_back({ Severity::Error, msg, line, col,
                    juce::String::charToString((juce::juce_wchar) ch0), "",
                    juce::String("Remove or replace the unrecognized character '")
                        + juce::String::charToString((juce::juce_wchar) ch0) + "'" });
                advance(1);
            }
        }
    }

    // ── Pattern matchers — positional, mirror PATTERNS regexes ─────────────

    // [0-9]+(\.[0-9]+)?(khz|hz) — case-insensitive
    int tryMatchFreq() const
    {
        int p = 0;
        if (!isDigit(peekAt(p))) return 0;
        while (isDigit(peekAt(p))) ++p;
        if (peekAt(p) == '.' && isDigit(peekAt(p + 1))) {
            ++p;
            while (isDigit(peekAt(p))) ++p;
        }
        // khz (3 chars) or hz (2 chars), case-insensitive
        if (matchLiteralCI("khz", p) == 3) return p + 3;
        if (matchLiteralCI("hz",  p) == 2) return p + 2;
        return 0;
    }

    // [+-]?[0-9]+c(?![a-z0-9_]) — case-insensitive on 'c'
    int tryMatchCents() const
    {
        int p = 0;
        if (peekAt(p) == '+' || peekAt(p) == '-') ++p;
        int digStart = p;
        while (isDigit(peekAt(p))) ++p;
        if (p == digStart) return 0;
        const char c = peekAt(p);
        if (c != 'c' && c != 'C') return 0;
        // Negative lookahead: not followed by [a-z0-9_]
        const char la = toLowerAscii(peekAt(p + 1));
        if ((la >= 'a' && la <= 'z') || isDigit(la) || la == '_') return 0;
        return p + 1;
    }

    // [a-g][#b]?[0-9](?![a-z0-9_#]) — case-insensitive
    int tryMatchNote() const
    {
        const char c0 = toLowerAscii(peekAt(0));
        if (c0 < 'a' || c0 > 'g') return 0;
        int p = 1;
        if (peekAt(p) == '#' || peekAt(p) == 'b' || peekAt(p) == 'B') ++p;
        if (!isDigit(peekAt(p))) return 0;
        ++p;
        const char la = toLowerAscii(peekAt(p));
        if ((la >= 'a' && la <= 'z') || isDigit(la) || la == '_' || la == '#') return 0;
        return p;
    }

    // [0-9]+(\.[0-9]+)?
    int tryMatchNumber() const
    {
        int p = 0;
        if (!isDigit(peekAt(p))) return 0;
        while (isDigit(peekAt(p))) ++p;
        if (peekAt(p) == '.' && isDigit(peekAt(p + 1))) {
            ++p;
            while (isDigit(peekAt(p))) ++p;
        }
        return p;
    }

    // ── Bracket tracking ──────────────────────────────────────────────────
    void openBracket(TokenType ty, const char* val, int line, int col)
    {
        emit(ty, val, line, col);
        bracketStack_.push_back({ ty, line, col });
    }

    void closeBracket(TokenType closeTy, const char* val, int line, int col)
    {
        TokenType expectedOpen = TokenType::UNKNOWN;
        if (closeTy == TokenType::RBRACKET) expectedOpen = TokenType::LBRACKET;
        else if (closeTy == TokenType::RPAREN) expectedOpen = TokenType::LPAREN;
        else if (closeTy == TokenType::RBRACE) expectedOpen = TokenType::LBRACE;

        if (bracketStack_.empty()) {
            juce::String msg;
            msg << "Unexpected '" << val << "' at line " << line << ", column " << col
                << " — no matching '" << bracketChar(expectedOpen) << "'";
            errors_.push_back(msg);
            diagnostics_.push_back({ Severity::Error, msg, line, col, val, "",
                juce::String("Remove the unexpected '") + val
                    + "' — there is no matching opening bracket" });
            emit(closeTy, val, line, col);
            return;
        }

        const auto& top = bracketStack_.back();
        if (top.type != expectedOpen) {
            const auto topCloser = closerFor(top.type);
            juce::String msg;
            msg << "Mismatched bracket at line " << line << ", column " << col
                << " — expected '" << bracketChar(topCloser)
                << "' to close '" << bracketChar(top.type)
                << "' opened at line " << top.line << ", column " << top.col
                << ", but got '" << val << "'";
            errors_.push_back(msg);
            diagnostics_.push_back({ Severity::Error, msg, line, col, val,
                juce::String(bracketChar(topCloser)),
                juce::String("Replace '") + val + "' with '"
                    + bracketChar(topCloser) + "' to close the '"
                    + bracketChar(top.type) + "' opened at line "
                    + juce::String(top.line) + ", column " + juce::String(top.col) });
        }
        bracketStack_.pop_back();
        emit(closeTy, val, line, col);
    }
};

// ─── Shared utilities (port of Parser.js free fns — 0.D-2b) ──────────────
// Forward decls first (mutual recursion: parseValue ↔ parseArgList ↔ parseGeneratorCall).

struct ParseValueResult { ParseValue value; int newPos = 0; juce::String error; };
struct ArgListResult {
    std::vector<ParseValue>                          args;
    std::vector<std::pair<juce::String, ParseValue>> named;
    int                                              newPos = 0;
    juce::String                                     error;
};
struct ProcessorResult  { Processor node; int newPos = 0; juce::String error; bool ok = false; };

static ParseValueResult parseValue     (const std::vector<Token>& toks, int pos);
static ArgListResult    parseArgList   (const std::vector<Token>& toks, int pos);
static ProcessorResult  parseProcessor (const std::vector<Token>& toks, int pos);
static CondNode         buildCondNode  (const ParseValue& raw,
                                        const std::vector<ParseValue>& positional,
                                        const ParseValue* condArg);
static ParseValueResult parseGeneratorCall(const std::vector<Token>& toks, int pos);

// ── Small token helpers (anon ns, mirror Parser.js sentinel behavior) ────

// V2 syntax cut A (T-242, parallel grammar): persistent-override marker.
// Accepts both v1 `@` sigil token AND v2 `set` keyword (lexed as IDENT).
// Single decision point — when v1 `@` is eventually deleted, only this body changes.
static bool isPersistentMarker(const Token& tok)
{
    return tok.type == TokenType::AT
        || (tok.type == TokenType::IDENT && tok.value == "set");
}

// V2 syntax cut B (T-252, parallel grammar): bare control-op keyword set.
// Per CURLOP-SCRIPTING_SYNTAX_V2_CONCEPT.md §1.7. When an IDENT in this set is
// followed by `(`, it parses as a generator call without the v1 `~` prefix.
// `accum` and `glide` were already bare in v1; cut B extends to the full set.
// `midi` is intentionally omitted (not listed in V2 §1.7) — keeps v1 `~midi(...)`.
static bool isControlOpKeyword(const juce::String& name)
{
    return name == "lfo"      || name == "ad"      || name == "adsr"
        || name == "auto"     || name == "random"  || name == "deviate"
        || name == "keytrack" || name == "bernoulli" || name == "shepard"
        || name == "accum"    || name == "glide";
}

static TokenType tokenTypeAt(const std::vector<Token>& t, int pos)
{
    return (pos >= 0 && pos < (int) t.size()) ? t[pos].type : TokenType::EOF_;
}
static const Token& tokenAt(const std::vector<Token>& t, int pos)
{
    static const Token eof{ TokenType::EOF_, "", 0, 0 };
    return (pos >= 0 && pos < (int) t.size()) ? t[pos] : eof;
}

static bool rejectNonFiniteNumber(double value, const Token& tok, int newPos, ParseValueResult& r)
{
    if (std::isfinite(value))
        return false;

    r.newPos = newPos;
    r.error = "Non-finite numeric literal '" + tok.value
            + "' at line " + juce::String(tok.line)
            + ", column " + juce::String(tok.col);
    return true;
}

static bool rejectNonFiniteFrequencyToken(const Token& tok, int newPos, ParseValueResult& r)
{
    const auto lower = tok.value.toLowerCase();
    double hz = lower.getDoubleValue();
    if (lower.endsWith("khz"))
        hz *= 1000.0;
    return rejectNonFiniteNumber(hz, tok, newPos, r);
}

// Peek a unit suffix after a number — { 'ms' | 's' | 'steps' | 'step' |
// 'beats' | 'beat' | '%' }.
// Returns a filled ParseValueResult of Kind::UnitNumber when matched, else kind=Null.
// B-271 — 'steps'/'step' is the explicit form of Rule 6's bare-as-steps
// (spec ~lfo example `4steps`); the compiler already resolves the unit.
static ParseValueResult peekUnitSuffix(const std::vector<Token>& toks, int i,
                                       double val, bool offset)
{
    ParseValueResult out;
    const auto& next = tokenAt(toks, i);
    // `4 step:2` starts the next processor; `step` is a unit only when it
    // is not followed by the processor colon.
    if (next.type == TokenType::IDENT
        && (next.value == "ms" || next.value == "s"
            || next.value == "steps" || next.value == "step"
            || next.value == "beats" || next.value == "beat")
        && tokenTypeAt(toks, i + 1) != TokenType::COLON) {
        out.value.kind   = ParseValue::Kind::UnitNumber;
        out.value.number = val;
        out.value.unit   = next.value;
        out.value.offset = offset;
        out.newPos = i + 1;
        return out;
    }
    if (next.type == TokenType::PERCENT) {
        out.value.kind   = ParseValue::Kind::UnitNumber;
        out.value.number = val;
        out.value.unit   = "%";
        out.value.offset = offset;
        out.newPos = i + 1;
        return out;
    }
    // No unit — caller gets kind=Null back (sentinel).
    return out;
}

// ── parseValue — Parser.js:587-726 ───────────────────────────────────────
// Per-token-type helpers. Each consumes from `pos` (pointing at the head
// token of the value) and returns a ParseValueResult; the orchestrator
// dispatches via tok.type. Family discoverable by `grep parse.*Value`.

static ParseValueResult parseNumberValue(const std::vector<Token>& toks, int pos)
{
    ParseValueResult r;
    const auto& tok = tokenAt(toks, pos);
    double val = tok.value.getDoubleValue();
    if (rejectNonFiniteNumber(val, tok, pos + 1, r)) return r;
    int i = pos + 1;
    // Fraction: NUMBER SLASH NUMBER
    if (tokenTypeAt(toks, i) == TokenType::SLASH
        && tokenTypeAt(toks, i + 1) == TokenType::NUMBER) {
        const double denom = toks[i + 1].value.getDoubleValue();
        if (denom != 0.0) val = val / denom;
        if (rejectNonFiniteNumber(val, tok, i + 2, r)) return r;
        i += 2;
        r.value.kind = ParseValue::Kind::Number;
        r.value.number = val;
        r.newPos = i;
        return r;
    }
    auto unit = peekUnitSuffix(toks, i, val, false);
    if (unit.value.kind != ParseValue::Kind::Null) return unit;
    r.value.kind = ParseValue::Kind::Number;
    r.value.number = val;
    r.newPos = i;
    return r;
}

static ParseValueResult parseHyphenValue(const std::vector<Token>& toks, int pos)
{
    ParseValueResult r;
    r.newPos = pos;
    const auto& tok  = tokenAt(toks, pos);
    const auto& next = tokenAt(toks, pos + 1);
    if (next.type == TokenType::NUMBER) {
        double val = -next.value.getDoubleValue();
        if (rejectNonFiniteNumber(val, next, pos + 2, r)) return r;
        int i = pos + 2;
        if (tokenTypeAt(toks, i) == TokenType::SLASH
            && tokenTypeAt(toks, i + 1) == TokenType::NUMBER) {
            const double denom = toks[i + 1].value.getDoubleValue();
            if (denom != 0.0) val = val / denom;
            if (rejectNonFiniteNumber(val, next, i + 2, r)) return r;
            i += 2;
            r.value.kind = ParseValue::Kind::Number;
            r.value.number = val;
            r.newPos = i;
            return r;
        }
        auto unit = peekUnitSuffix(toks, i, val, false);
        if (unit.value.kind != ParseValue::Kind::Null) return unit;
        r.value.kind = ParseValue::Kind::Number;
        r.value.number = val;
        r.newPos = i;
        return r;
    }
    // Bare '-' followed by , or ) → rest marker
    if (next.type == TokenType::EOF_
        || next.type == TokenType::COMMA
        || next.type == TokenType::RPAREN) {
        r.value.kind = ParseValue::Kind::Rest;
        r.value.str  = "-";
        r.newPos = pos + 1;
        return r;
    }
    r.error = "Expected number after '-' at line " + juce::String(tok.line)
            + ", column " + juce::String(tok.col);
    return r;
}

static ParseValueResult parsePlusValue(const std::vector<Token>& toks, int pos)
{
    ParseValueResult r;
    r.newPos = pos;
    const auto& tok  = tokenAt(toks, pos);
    const auto& next = tokenAt(toks, pos + 1);
    if (next.type == TokenType::NUMBER) {
        const double val = next.value.getDoubleValue();
        if (rejectNonFiniteNumber(val, next, pos + 2, r)) return r;
        int i = pos + 2;
        auto unit = peekUnitSuffix(toks, i, val, true);
        if (unit.value.kind != ParseValue::Kind::Null) return unit;
        r.value.kind   = ParseValue::Kind::UnitNumber;
        r.value.number = val;
        r.value.offset = true;
        r.newPos = i;
        return r;
    }
    r.error = "Expected number after '+' at line " + juce::String(tok.line)
            + ", column " + juce::String(tok.col);
    return r;
}

static ParseValueResult parseTildeValue(const std::vector<Token>& toks, int pos)
{
    ParseValueResult r;
    r.newPos = pos;
    const auto& tok  = tokenAt(toks, pos);
    const auto& next = tokenAt(toks, pos + 1);
    if (next.type != TokenType::IDENT) {
        r.error = "Expected generator name after '~' at line "
                + juce::String(tok.line) + ", column " + juce::String(tok.col);
        return r;
    }
    return parseGeneratorCall(toks, pos + 1);
}

static ParseValueResult parsePercentValue(const std::vector<Token>& toks, int pos)
{
    ParseValueResult r;
    r.newPos = pos;
    const auto& tok  = tokenAt(toks, pos);
    const auto& next = tokenAt(toks, pos + 1);
    if (next.type == TokenType::TILDE) {
        const auto& gn = tokenAt(toks, pos + 2);
        if (gn.type != TokenType::IDENT) {
            r.error = "Expected generator name after '%~' at line "
                    + juce::String(tok.line) + ", column " + juce::String(tok.col);
            return r;
        }
        auto g = parseGeneratorCall(toks, pos + 2);
        if (g.value.gen) g.value.gen->isOffset = true;
        return g;
    }
    // V2 cut B: `%<control-op>(` form (no TILDE) — bare control-op keyword
    // followed by `(`. Sibling branch to `%~` for parallel grammar.
    if (next.type == TokenType::IDENT
        && isControlOpKeyword(next.value)
        && tokenTypeAt(toks, pos + 2) == TokenType::LPAREN) {
        auto g = parseGeneratorCall(toks, pos + 1);
        if (g.value.gen) g.value.gen->isOffset = true;
        return g;
    }
    // Not %~ or %<keyword>( — return Null sentinel, no error, no advance (caller handles)
    return r;
}

static ParseValueResult parseIdentValue(const std::vector<Token>& toks, int pos)
{
    ParseValueResult r;
    const auto& tok = tokenAt(toks, pos);
    // V2 cut B: any control-op keyword followed by `(` parses as generator.
    // V1 `~lfo(...)` form still works via parseTildeValue → parseGeneratorCall.
    // Allowlist (isControlOpKeyword): lfo, ad, adsr, auto, random, deviate,
    // keytrack, bernoulli, shepard, accum, glide.
    if (tokenTypeAt(toks, pos + 1) == TokenType::LPAREN
        && isControlOpKeyword(tok.value)) {
        return parseGeneratorCall(toks, pos);
    }
    r.value.kind = ParseValue::Kind::String;
    r.value.str  = tok.value;
    r.newPos = pos + 1;
    return r;
}

static ParseValueResult parseBangValue(const std::vector<Token>& toks, int pos)
{
    ParseValueResult r;
    r.newPos = pos;
    const auto& next = tokenAt(toks, pos + 1);
    if (next.type == TokenType::IDENT) {
        r.value.kind = ParseValue::Kind::String;
        r.value.str  = juce::String("!") + next.value;
        r.newPos = pos + 2;
        return r;
    }
    if (next.type == TokenType::LPAREN) {
        auto al = parseArgList(toks, pos + 2);
        if (al.error.isNotEmpty()) { r.error = al.error; r.newPos = al.newPos; return r; }
        r.value.kind = ParseValue::Kind::NegatedLoops;
        for (auto const& v : al.args)
            if (v.kind == ParseValue::Kind::Number) r.value.negLoops.push_back(v.number);
        r.newPos = al.newPos;
        return r;
    }
    r.value.kind = ParseValue::Kind::String;
    r.value.str  = "!";
    r.newPos = pos + 1;
    return r;
}

static ParseValueResult parseValue(const std::vector<Token>& toks, int pos)
{
    ParseValueResult r;
    r.newPos = pos;
    const auto& tok = tokenAt(toks, pos);
    if (tok.type == TokenType::EOF_) {
        r.error = "Unexpected end of input while reading value";
        return r;
    }

    switch (tok.type) {
        case TokenType::NUMBER:  return parseNumberValue (toks, pos);
        case TokenType::HYPHEN:  return parseHyphenValue (toks, pos);
        case TokenType::PLUS:    return parsePlusValue   (toks, pos);
        case TokenType::NOTE:
        case TokenType::CHORD:
        case TokenType::CENTS:
            r.value.kind = ParseValue::Kind::String;
            r.value.str  = tok.value;
            r.newPos = pos + 1;
            return r;
        case TokenType::FREQ:
            if (rejectNonFiniteFrequencyToken(tok, pos + 1, r)) return r;
            r.value.kind = ParseValue::Kind::String;
            r.value.str  = tok.value;
            r.newPos = pos + 1;
            return r;
        case TokenType::TILDE:   return parseTildeValue  (toks, pos);
        case TokenType::PERCENT: return parsePercentValue(toks, pos);
        case TokenType::IDENT:   return parseIdentValue  (toks, pos);
        case TokenType::BANG:    return parseBangValue   (toks, pos);
        default:
            r.error = "Expected value at line " + juce::String(tok.line)
                    + ", column " + juce::String(tok.col);
            return r;
    }
}

// ── parseArgList — Parser.js:497-538 ─────────────────────────────────────
static ArgListResult parseArgList(const std::vector<Token>& toks, int pos)
{
    ArgListResult out;
    int i = pos;

    if (tokenTypeAt(toks, i) == TokenType::RPAREN) {
        out.newPos = i + 1;
        return out;
    }

    const int N = (int) toks.size();
    while (i < N) {
        // Named arg: IDENT COLON value
        if (tokenTypeAt(toks, i) == TokenType::IDENT
            && tokenTypeAt(toks, i + 1) == TokenType::COLON) {
            const juce::String paramName = toks[i].value;
            i += 2;
            auto v = parseValue(toks, i);
            if (v.error.isNotEmpty()) { out.error = v.error; out.newPos = i; return out; }
            out.named.emplace_back(paramName, std::move(v.value));
            i = v.newPos;
        } else {
            auto v = parseValue(toks, i);
            if (v.error.isNotEmpty()) { out.error = v.error; out.newPos = i; return out; }
            out.args.push_back(std::move(v.value));
            i = v.newPos;
        }

        const auto ty = tokenTypeAt(toks, i);
        if (ty == TokenType::COMMA) { ++i; continue; }
        if (ty == TokenType::RPAREN) { ++i; break; }

        const auto& tok = tokenAt(toks, i);
        if (tok.type == TokenType::EOF_) {
            out.error = "Unexpected end of input inside argument list";
        } else {
            out.error = "Expected ',' or ')' at line " + juce::String(tok.line)
                      + ", column " + juce::String(tok.col);
        }
        out.newPos = i;
        return out;
    }

    out.newPos = i;
    return out;
}

// ── buildCondNode — Parser.js:776-825 ────────────────────────────────────
static bool isCondCycleKeyword(const juce::String& s)
{
    return s == "prime" || s == "fib" || s == "even" || s == "odd"
        || s == "first" || s == "!first";
}

static bool isCondNoArgKeyword(const juce::String& s)
{
    return isCondCycleKeyword(s)
        || s == "silence" || s == "held" || s == "changed"
        || s == "previous" || s == "!previous";
}

static bool isCondLoopArgKeyword(const juce::String& s)
{
    return s == "mod" || s == "every" || s == "once" || s == "after";
}

static juce::String validateCondArgs(const ParseValue& rawArg,
                                     const std::vector<ParseValue>& positional,
                                     const ParseValue* condArg)
{
    auto requireNumber = [] (const ParseValue& v, const char* label) -> juce::String {
        return v.kind == ParseValue::Kind::Number
            ? juce::String()
            : (juce::String("cond ") + label + " argument must be numeric");
    };

    if (! positional.empty()) {
        const auto& first = positional[0];
        if (first.kind == ParseValue::Kind::String) {
            const auto& s = first.str;
            if (isCondCycleKeyword(s)) {
                if (positional.size() > 2)
                    return "cond cycle form accepts at most one numeric cycle argument";
                if (positional.size() == 2)
                    return requireNumber(positional[1], "cycle");
                return {};
            }
            if (isCondLoopArgKeyword(s)) {
                if (positional.size() != 2)
                    return "cond " + s + " form requires exactly one numeric loop argument";
                return requireNumber(positional[1], "loop");
            }
            if (s == "silence" || s == "held" || s == "changed"
                || s == "previous" || s == "!previous") {
                return positional.size() == 1
                    ? juce::String()
                    : (juce::String("cond ") + s + " form does not accept arguments");
            }
            return "Unknown cond value '" + s + "'";
        }

        for (const auto& v : positional) {
            if (v.kind != ParseValue::Kind::Number)
                return "cond loop set arguments must all be numeric";
        }
        return {};
    }

    if (rawArg.kind == ParseValue::Kind::String) {
        const auto& s = rawArg.str;
        if (isCondNoArgKeyword(s))
            return {};
        if (isCondLoopArgKeyword(s)) {
            if (condArg == nullptr)
                return "cond:" + s + " requires a numeric loop argument";
            return requireNumber(*condArg, "loop");
        }
        return "Unknown cond value '" + s + "'";
    }

    return {};
}

static CondNode buildCondNode(const ParseValue& rawArg,
                              const std::vector<ParseValue>& positional,
                              const ParseValue* condArg)
{
    CondNode c;
    // Canonical cond(first, cycle) or cond(1,3,5)
    if (!positional.empty()) {
        const auto& first = positional[0];
        if (first.kind == ParseValue::Kind::String) {
            const juce::String& s = first.str;
            if (isCondCycleKeyword(s)) {
                c.condType = s;
                c.cycle = (positional.size() > 1 && positional[1].kind == ParseValue::Kind::Number)
                            ? (int) positional[1].number : 0;
                return c;
            }
        }
        c.condType = "loops";
        for (auto const& v : positional)
            if (v.kind == ParseValue::Kind::Number) c.loops.push_back(v.number);
        return c;
    }

    if (rawArg.kind == ParseValue::Kind::NegatedLoops) {
        c.condType = "!loops";
        c.loops = rawArg.negLoops;
        return c;
    }

    if (rawArg.kind == ParseValue::Kind::String) {
        const juce::String& s = rawArg.str;
        if (s == "first" || s == "!first" || s == "even" || s == "odd"
            || s == "prime" || s == "fib" || s == "silence" || s == "held"
            || s == "changed" || s == "previous" || s == "!previous") {
            c.condType = s;
            return c;
        }
        if (s == "mod" || s == "every") {
            c.condType = "mod";
            if (condArg && condArg->kind == ParseValue::Kind::Number)
                c.loops.push_back(condArg->number);
            return c;
        }
        if (s == "once" || s == "after") {
            c.condType = s;
            if (condArg && condArg->kind == ParseValue::Kind::Number)
                c.loops.push_back(condArg->number);
            return c;
        }
        // Fallback: loops:[Number(s)]
        c.condType = "loops";
        c.loops.push_back(s.getDoubleValue());
        return c;
    }

    if (rawArg.kind == ParseValue::Kind::Number) {
        c.condType = "loops";
        c.loops.push_back(rawArg.number);
        return c;
    }

    c.condType = "loops";
    return c;
}

// ── parseGeneratorCall — Parser.js:840-865 ───────────────────────────────
// pos points at the IDENT (caller advances past ~ or %~ prefix).
static ParseValueResult parseGeneratorCall(const std::vector<Token>& toks, int pos)
{
    ParseValueResult r;
    const auto& nameTok = tokenAt(toks, pos);
    int i = pos + 1;

    auto g = std::make_shared<GeneratorNode>();
    g->generatorType = nameTok.value;
    g->line = nameTok.line;
    g->col  = nameTok.col;

    if (tokenTypeAt(toks, i) != TokenType::LPAREN) {
        // Bare generator without args
        r.value.kind = ParseValue::Kind::Generator;
        r.value.gen  = g;
        r.newPos = i;
        return r;
    }
    ++i; // consume '('

    auto al = parseArgList(toks, i);
    if (al.error.isNotEmpty()) {
        r.error = al.error;
        r.newPos = al.newPos;
        return r;
    }
    g->args  = std::move(al.args);
    g->named = std::move(al.named);

    r.value.kind = ParseValue::Kind::Generator;
    r.value.gen  = g;
    r.newPos = al.newPos;
    return r;
}

// ── parseProcessor — Parser.js:389-481 ───────────────────────────────────
static ProcessorResult parseProcessor(const std::vector<Token>& toks, int pos)
{
    ProcessorResult out;
    int i = pos;

    bool persistent = false;
    // V2 cut A: accept both `@` sigil and `set` keyword as persistent markers.
    if (isPersistentMarker(tokenAt(toks, i))) { persistent = true; ++i; }

    const auto& nameTok = tokenAt(toks, i);
    if (nameTok.type != TokenType::IDENT) {
        const auto& ref = tokenAt(toks, pos);
        out.error = "Expected processor name at line " + juce::String(ref.line)
                  + ", column " + juce::String(ref.col);
        out.newPos = pos;
        return out;
    }

    const juce::String rawName = nameTok.value;
    const auto canonSv = ::curlop::script::canonicalize(
        std::string_view(rawName.toRawUTF8(), (size_t) rawName.getNumBytesAsUTF8()));
    const juce::String canonicalName = juce::String::fromUTF8(canonSv.data(), (int) canonSv.size());
    ++i;

    const auto nextTy = tokenTypeAt(toks, i);

    // Sugar form: name:value
    if (nextTy == TokenType::COLON) {
        ++i; // consume ':'
        auto v = parseValue(toks, i);
        if (v.error.isNotEmpty()) { out.error = v.error; out.newPos = pos; return out; }

        // Special: cond: — produce CondNode in args[0]
        if (canonicalName == "cond") {
            ParseValue condVal = v.value;
            const ParseValue* condArg = nullptr;
            ParseValue argStorage;
            int finalPos = v.newPos;
            if (condVal.kind == ParseValue::Kind::String) {
                bool isArgTaking = false;
                for (auto const& c : ::curlop::script::kCondArgValues)
                    if (condVal.str == juce::String::fromUTF8(c.data(), (int) c.size())) { isArgTaking = true; break; }
                if (isArgTaking && tokenTypeAt(toks, v.newPos) == TokenType::COLON) {
                    auto av = parseValue(toks, v.newPos + 1);
                    if (av.error.isEmpty() && av.value.kind == ParseValue::Kind::Number) {
                        argStorage = av.value;
                        condArg    = &argStorage;
                        finalPos   = av.newPos;
                    }
                }
            }
            const auto condError = validateCondArgs(condVal, {}, condArg);
            if (condError.isNotEmpty()) {
                out.error = condError;
                out.newPos = pos;
                return out;
            }
            auto cnode = std::make_shared<CondNode>(buildCondNode(condVal, {}, condArg));
            Processor p;
            p.name = rawName;
        p.sourceOrdinal = pos;
            p.canonicalName = canonicalName;
            p.persistent = persistent;
            ParseValue wrap;
            wrap.kind = ParseValue::Kind::Cond;
            wrap.cond = cnode;
            p.args.push_back(std::move(wrap));
            out.node = std::move(p);
            out.ok = true;
            out.newPos = finalPos;
            return out;
        }

        Processor p;
        p.name = rawName;
        p.sourceOrdinal = pos;
        p.canonicalName = canonicalName;
        p.persistent = persistent;
        p.args.push_back(std::move(v.value));
        out.node = std::move(p);
        out.ok = true;
        out.newPos = v.newPos;
        return out;
    }

    // Canonical form: name(args...)
    if (nextTy == TokenType::LPAREN) {
        ++i; // consume '('
        auto al = parseArgList(toks, i);
        if (al.error.isNotEmpty()) { out.error = al.error; out.newPos = pos; return out; }

        if (canonicalName == "cond") {
            const auto condError = validateCondArgs(ParseValue{}, al.args, nullptr);
            if (condError.isNotEmpty()) {
                out.error = condError;
                out.newPos = pos;
                return out;
            }
            auto cnode = std::make_shared<CondNode>(buildCondNode(ParseValue{}, al.args, nullptr));
            Processor p;
            p.name = rawName;
        p.sourceOrdinal = pos;
            p.canonicalName = canonicalName;
            p.persistent = persistent;
            ParseValue wrap;
            wrap.kind = ParseValue::Kind::Cond;
            wrap.cond = cnode;
            p.args.push_back(std::move(wrap));
            out.node = std::move(p);
            out.ok = true;
            out.newPos = al.newPos;
            return out;
        }

        Processor p;
        p.name = rawName;
        p.sourceOrdinal = pos;
        p.canonicalName = canonicalName;
        p.persistent = persistent;
        p.args  = std::move(al.args);
        p.named = std::move(al.named);
        out.node = std::move(p);
        out.ok = true;
        out.newPos = al.newPos;
        return out;
    }

    // Bare name
    Processor p;
    p.name = rawName;
        p.sourceOrdinal = pos;
    p.canonicalName = canonicalName;
    p.persistent = persistent;
    out.node = std::move(p);
    out.ok = true;
    out.newPos = i;
    return out;
}

// ─── Parser — Cut 0.D-2b complete body ───────────────────────────────────
// Builds statements (Assignment / Sequence / PersistentSetter) + tracks
// variable names + emits matching errors. Sub-statement AST construction is
// delegated to parseSeqBody which consumes all grammar forms without NYI.

class Parser
{
public:
    Parser(std::vector<Token> toks, const ParseOptions& opts)
        : toks_(std::move(toks)), opts_(opts) {}

    ScriptAst parseScript()
    {
        ScriptAst ast;
        while (peekType() == TokenType::NEWLINE) ++pos_;

        while (peekType() != TokenType::EOF_) {
            auto stmt = parseStatement(ast);
            if (stmt) ast.statements.push_back(std::move(stmt));
            while (peekType() == TokenType::NEWLINE) ++pos_;
        }

        std::sort(ast.variableNames.begin(), ast.variableNames.end(),
                  [](const juce::String& a, const juce::String& b) { return a.compare(b) < 0; });

        ast.shape = computeShapeFingerprint(ast);
        return ast;
    }

    std::vector<juce::String>&     errors()      { return errors_; }
    std::vector<ScriptDiagnostic>& diagnostics() { return diagnostics_; }

private:
    std::vector<Token>               toks_;
    const ParseOptions&              opts_;
    int                              pos_ = 0;
    std::vector<juce::String>        errors_;
    std::vector<ScriptDiagnostic>    diagnostics_;

    // State (0.D-2b Spike S4/S9)
    juce::String                     contextModule_;
    std::unordered_map<juce::String, AstNode*> variables_; // non-owning ptr into Assignment's value subtree

    // ── Peek / consume / error helpers ────────────────────────────────────
    const Token& peek(int off = 0) const
    {
        static const Token eof{ TokenType::EOF_, "", 0, 0 };
        const int i = pos_ + off;
        return (i >= 0 && i < (int) toks_.size()) ? toks_[i] : eof;
    }
    TokenType peekType(int off = 0) const { return peek(off).type; }
    const Token& consume() { return toks_[pos_++]; }
    bool atEnd() const { return peekType() == TokenType::EOF_; }

    void emitError(const juce::String& msg, const Token& at,
                   const juce::String& expected = {},
                   const juce::String& suggestion = {})
    {
        errors_.push_back(msg);
        diagnostics_.push_back({ Severity::Error, msg, at.line, at.col,
                                 at.value, expected, suggestion });
    }
    void emitWarning(const juce::String& msg, const Token& at,
                     const juce::String& suggestion = {})
    {
        diagnostics_.push_back({ Severity::Warning, msg, at.line, at.col,
                                 at.value, {}, suggestion });
    }

    // ── Pattern tests ─────────────────────────────────────────────────────
    bool isChordName(const juce::String& s) const
    {
        if (opts_.chordRegistry.count(s) > 0) return true;
        for (auto const& c : ::curlop::script::kChordNames)
            if (s == juce::String::fromUTF8(c.data(), (int) c.size())) return true;
        return false;
    }
    bool isStandardProcessor(const juce::String& s) const
    {
        for (auto const& p : ::curlop::script::kStandardProcessors)
            if (s == juce::String::fromUTF8(p.data(), (int) p.size())) return true;
        return false;
    }
    bool isNoArgOp(const juce::String& s) const
    {
        for (auto const& p : ::curlop::script::kNoArgOps)
            if (s == juce::String::fromUTF8(p.data(), (int) p.size())) return true;
        return false;
    }

    enum class DotRole { PitchChain, ParamAccess, HoldAccess, Unknown };
    DotRole classifyDot(const Token& next) const
    {
        if (next.type == TokenType::NOTE || next.type == TokenType::FREQ
            || next.type == TokenType::CENTS || next.type == TokenType::CHORD)
            return DotRole::PitchChain;
        if (next.type == TokenType::IDENT) {
            if (isChordName(next.value)) return DotRole::PitchChain;
            return DotRole::ParamAccess;
        }
        if (next.type == TokenType::UNDERSCORE) return DotRole::HoldAccess;
        return DotRole::Unknown;
    }
    bool isPitchToken(TokenType t) const
    {
        return t == TokenType::NOTE || t == TokenType::FREQ
            || t == TokenType::CENTS || t == TokenType::CHORD;
    }

    // Step 5b — pitch-chain component dispatch. Mirrors JS Parser.js:1751-1758.
    void applyPitchComponent(PitchChain& pitch, const Token& tok) const
    {
        if      (tok.type == TokenType::NOTE)  pitch.note  = tok.value;
        else if (tok.type == TokenType::FREQ)  pitch.freq  = tok.value;
        else if (tok.type == TokenType::CENTS) pitch.cents = tok.value;
        else if (tok.type == TokenType::CHORD) pitch.chord = tok.value;
        else if (tok.type == TokenType::IDENT && isChordName(tok.value))
            pitch.chord = tok.value;
    }

    // ── Statement dispatch ────────────────────────────────────────────────
    AstNodePtr parseStatement(ScriptAst& ast)
    {
        const auto& tok = peek();
        if (tok.type == TokenType::EOF_) return nullptr;

        // @ persistent (v1) or `set` keyword (v2 cut A — parallel grammar)
        if (isPersistentMarker(tok)) {
            const auto& afterAt = peek(1);
            const bool isModuleOverride =
                afterAt.type == TokenType::HASH
                || (afterAt.type == TokenType::IDENT && peekType(2) == TokenType::DOT);
            if (isModuleOverride) {
                const int startLine = tok.line, startCol = tok.col;
                ++pos_; // consume marker (@ or set)
                if (peekType() == TokenType::HASH) ++pos_;
                auto ev = parseModuleExpr(0, juce::String());
                if (ev) {
                    // Mark all processors persistent (mirrors JS Parser.js:975)
                    for (auto& p : ev->processors) p.persistent = true;
                }
                auto n = std::make_unique<AstNode>();
                n->type        = "PersistentSetter";
                n->setterEvent = std::move(ev); // T-049 consumer reads this
                n->line        = startLine;
                n->col         = startCol;
                return n;
            }
            auto pr = parseProcessor(toks_, pos_);
            if (pr.error.isNotEmpty()) {
                errors_.push_back(pr.error);
                ++pos_;
                return nullptr;
            }
            pos_ = pr.newPos;
            auto n = std::make_unique<AstNode>();
            n->type               = "PersistentSetter";
            n->isSetterProcessor  = true;
            n->setterProcessor    = std::move(pr.node);
            n->setterProcessor.persistent = true; // mirror JS Parser.js:975
            n->line               = tok.line;
            n->col                = tok.col;
            return n;
        }

        // $var = fragment
        if (tok.type == TokenType::DOLLAR
            && peekType(1) == TokenType::IDENT
            && peekType(2) == TokenType::ASSIGN) {
            return parseAssignment(ast);
        }

        // bare IDENT = ... — missing $ error
        if (tok.type == TokenType::IDENT && peekType(1) == TokenType::ASSIGN) {
            juce::String msg = "Variable assignments require '$' prefix at line "
                             + juce::String(tok.line) + ", column " + juce::String(tok.col)
                             + " — use $" + tok.value + " = ...";
            emitError(msg, tok, juce::String("$") + tok.value,
                      juce::String("Add '$' before '") + tok.value + "': $" + tok.value + " = ...");
            ++pos_; // IDENT
            ++pos_; // ASSIGN
            parseFragment(juce::String()); // consume + discard
            return nullptr;
        }

        // Sequence/expr statement
        return parseSequenceOrExpr();
    }

    AstNodePtr parseAssignment(ScriptAst& ast)
    {
        const auto dollarTok = consume();  // $
        const auto nameTok   = consume();  // IDENT
        consume();                          // =

        const juce::String name = nameTok.value;
        ast.variableNames.push_back(name);
        variables_[name] = nullptr; // pre-register with nullptr so circular-ref detection in
                                    // parseSeqBody sees the name as known during RHS parse.

        auto rhs = parseFragment(name);

        // LOAD-BEARING: capture raw ptr BEFORE std::move transfers ownership.
        AstNode* rhsPtr = rhs.get();
        variables_[name] = rhsPtr; // VariableRef.resolved points here; ScriptAst owns lifetime.

        auto n = std::make_unique<AstNode>();
        n->type  = "Assignment";
        n->name  = name;
        n->line  = dollarTok.line;
        n->col   = dollarTok.col;
        n->value = std::move(rhs);
        return n;
    }

    // ── Sequence / Fragment dispatch ──────────────────────────────────────
    AstNodePtr parseSequenceOrExpr()
    {
        if (peekType() == TokenType::LBRACKET) return parseSequence(juce::String());
        if (peekType() == TokenType::LPAREN)   return parseTuplet(juce::String());
        return parseImplicitSequence(juce::String());
    }

    AstNodePtr parseFragment(const juce::String& assigningName)
    {
        const auto ty = peekType();
        if (ty == TokenType::LBRACKET) {
            const int savedPos = pos_;
            auto seq = parseSequence(assigningName);
            const auto afterTy = peekType();
            const bool hasMoreContent =
                afterTy != TokenType::EOF_ && afterTy != TokenType::NEWLINE;
            if (hasMoreContent) {
                pos_ = savedPos;
                return parseImplicitSequence(assigningName);
            }
            return seq;
        }
        if (ty == TokenType::LPAREN) {
            const int savedPos = pos_;
            auto tup = parseTuplet(assigningName);
            const auto afterTy = peekType();
            const bool hasMoreContent =
                afterTy != TokenType::EOF_ && afterTy != TokenType::NEWLINE;
            if (hasMoreContent) {
                pos_ = savedPos;
                return parseImplicitSequence(assigningName);
            }
            return tup;
        }
        return parseImplicitSequence(assigningName);
    }

    AstNodePtr parseSequence(const juce::String& assigningName)
    {
        const auto openTok = consume(); // LBRACKET
        auto steps = parseSeqBody(TokenType::RBRACKET, assigningName);
        if (peekType() == TokenType::RBRACKET) ++pos_;
        auto n = std::make_unique<AstNode>();
        n->type      = "Sequence";
        n->scopeType = "parallel";
        n->line      = openTok.line;
        n->col       = openTok.col;
        n->steps     = std::move(steps);
        return n;
    }

    AstNodePtr parseTuplet(const juce::String& assigningName)
    {
        const auto openTok = consume(); // LPAREN
        auto steps = parseSeqBody(TokenType::RPAREN, assigningName);
        if (peekType() == TokenType::RPAREN) ++pos_;
        auto n = std::make_unique<AstNode>();
        n->type      = "Sequence";
        n->scopeType = "tuplet";
        n->line      = openTok.line;
        n->col       = openTok.col;
        n->steps     = std::move(steps);
        return n;
    }

    AstNodePtr parseImplicitSequence(const juce::String& assigningName)
    {
        const int startLine = peek().line, startCol = peek().col;
        // Sentinel closeType = EOF_ means "stop at NEWLINE or EOF"
        auto steps = parseSeqBody(TokenType::EOF_, assigningName);
        auto n = std::make_unique<AstNode>();
        n->type      = "Sequence";
        n->scopeType = "parallel";
        n->line      = startLine;
        n->col       = startCol;
        n->steps     = std::move(steps);
        return n;
    }

    // ── parseSeqBody local state + flush/push helpers (T-222) ─────────────
    struct SeqBodyState {
        std::vector<AstNodePtr> steps;
        std::vector<AstNodePtr> events;
        std::vector<Processor>  processors;     // step-level (bracket scope)
        std::vector<Processor>  pendingProcs;   // implicit-scope pending distribution
        int                     commaGroup = 0;
        bool                    isImplicit = false;
        TokenType               closeType  = TokenType::EOF_;
        juce::String            assigningName;
    };

    bool isAtCloseToken(const SeqBodyState& st) const
    {
        if (atEnd()) return true;
        const auto t = peekType();
        if (!st.isImplicit) return t == st.closeType;
        return t == TokenType::NEWLINE;
    }

    void drainPendingProcsToLastEvent(SeqBodyState& st)
    {
        // JS Parser.js:1133-1138 — implicit-scope only.
        if (!st.isImplicit || st.pendingProcs.empty() || st.events.empty()) return;
        auto& lastEvent = st.events.back();
        for (auto& p : st.pendingProcs)
            lastEvent->processors.push_back(std::move(p));
        st.pendingProcs.clear();
    }

    void applyPendingByEventShape(SeqBodyState& st)
    {
        // JS Parser.js:1148-1186 — 3-way distribution on current state.
        if (st.pendingProcs.empty()) return;

        auto isStepLevel = [](const juce::String& s) {
            return isStepLevelProc(std::string_view(s.toRawUTF8(),
                                                    (size_t) s.getNumBytesAsUTF8()));
        };

        if (st.events.size() == 1 && st.events.front()->type == "Sequence") {
            // Sequence-as-event: all pendingProcs → step.processors
            for (auto& p : st.pendingProcs) st.processors.push_back(std::move(p));
        } else if (st.events.size() == 1) {
            // Single Event: split by kStepLevelProcs
            auto& evProcs = st.events.front()->processors;
            for (auto& p : st.pendingProcs) {
                const bool stepLevel = isStepLevel(p.name) || isStepLevel(p.canonicalName);
                if (stepLevel) st.processors.push_back(std::move(p));
                else           evProcs.push_back(std::move(p));
            }
        } else {
            // Multiple events: tag each with afterEventCount = events.size()
            const int eventCountSnap = (int) st.events.size();
            for (auto& p : st.pendingProcs) {
                p.afterEventCount = eventCountSnap;
                st.processors.push_back(std::move(p));
            }
        }
        st.pendingProcs.clear();
    }

    void pushAccumulatedStep(SeqBodyState& st)
    {
        applyPendingByEventShape(st);
        auto step = std::make_unique<AstNode>();
        step->type       = "Step";
        step->events     = std::move(st.events);
        step->processors = std::move(st.processors);
        st.steps.push_back(std::move(step));
        st.events.clear();
        st.processors.clear();
        st.commaGroup = 0;
    }

    // ── parseSeqBody per-token-kind handlers (T-222 H5–H18) ───────────────

    void emitRestOrHoldEvent(SeqBodyState& st, const char* eventType, const Token& tok)
    {
        drainPendingProcsToLastEvent(st);
        auto ev = std::make_unique<AstNode>();
        ev->type       = "Event";
        ev->eventType  = juce::String(eventType);
        ev->commaGroup = st.commaGroup;
        ev->line       = tok.line;
        ev->col        = tok.col;
        st.events.push_back(std::move(ev));
    }

    void handleBracketScope(SeqBodyState& st)
    {
        drainPendingProcsToLastEvent(st);
        auto seq = parseSequence(st.assigningName);
        if (seq) {
            seq->commaGroup = st.commaGroup;
            st.events.push_back(std::move(seq));
        }
    }

    void handleParenScope(SeqBodyState& st)
    {
        drainPendingProcsToLastEvent(st);
        auto tup = parseTuplet(st.assigningName);
        if (tup) {
            tup->commaGroup = st.commaGroup;
            st.events.push_back(std::move(tup));
        }
    }

    void handleAtToken(SeqBodyState& st)
    {
        // V2 cut A: pos_ is at either `@` (v1) or `set` IDENT (v2). Both consume one token.
        const auto& afterAt = peek(1);
        const bool isModuleOverride =
            afterAt.type == TokenType::HASH
            || (afterAt.type == TokenType::IDENT && peekType(2) == TokenType::DOT);
        if (isModuleOverride) {
            ++pos_; // consume marker (@ or set)
            if (peekType() == TokenType::HASH) ++pos_;
            drainPendingProcsToLastEvent(st);
            auto ev = parseModuleExpr(st.commaGroup, st.assigningName);
            if (ev) {
                for (auto& p : ev->processors) p.persistent = true;
                st.events.push_back(std::move(ev));
            }
            return;
        }
        auto pr = parseProcessor(toks_, pos_);
        if (pr.error.isNotEmpty()) {
            errors_.push_back(pr.error);
            ++pos_;
            return;
        }
        pos_ = pr.newPos;
        Processor p  = pr.node;
        p.commaGroup = st.commaGroup;
        p.persistent = true;
        st.processors.push_back(std::move(p));
    }

    void handleHashToken(SeqBodyState& st, const Token& tok)
    {
        const auto& nextTok = peek(1);
        if (nextTok.type != TokenType::IDENT) {
            juce::String msg = "Expected module name after '#' at line "
                             + juce::String(tok.line) + ", column " + juce::String(tok.col);
            emitError(msg, tok, "module name (e.g. #kick, #ref)",
                      "Add a module name after '#'");
            ++pos_;
            return;
        }
        drainPendingProcsToLastEvent(st);
        ++pos_; // consume #
        auto ev = parseModuleExpr(st.commaGroup, st.assigningName);
        if (ev) {
            consumeSpaceParamLocks(
                ev.get(), &st.processors, &st.pendingProcs,
                (int) st.events.size(), st.commaGroup, st.closeType);
            contextModule_ = ev->module;
            st.events.push_back(std::move(ev));
        }
    }

    void handleDollarToken(SeqBodyState& st, const Token& tok)
    {
        const auto& nameTok = peek(1);
        if (nameTok.type != TokenType::IDENT) {
            ++pos_; // $ alone — skip
            return;
        }
        if (st.assigningName.isNotEmpty() && nameTok.value == st.assigningName) {
            juce::String msg = juce::String("Circular variable reference: '$") + nameTok.value
                             + "' references itself at line " + juce::String(tok.line)
                             + ", column " + juce::String(tok.col);
            emitError(msg, tok, juce::String(),
                      juce::String("A variable cannot reference itself — remove '$")
                        + nameTok.value + "' from its own definition");
            pos_ += 2;
            return;
        }
        auto it = variables_.find(nameTok.value);
        if (it != variables_.end()) {
            drainPendingProcsToLastEvent(st);
            auto ref = std::make_unique<AstNode>();
            ref->type       = "VariableRef";
            ref->name       = nameTok.value;
            ref->resolved   = it->second;
            ref->commaGroup = st.commaGroup;
            ref->line       = tok.line;
            ref->col        = tok.col;
            pos_ += 2;
            st.events.push_back(std::move(ref));
            return;
        }
        juce::String msg = juce::String("Undefined variable '$") + nameTok.value
                         + "' at line " + juce::String(tok.line)
                         + ", column " + juce::String(tok.col);
        emitError(msg, tok, juce::String(),
                  juce::String("Declare '$") + nameTok.value + "' before using it");
        pos_ += 2;
    }

    void emitLocalOutputEvent(SeqBodyState& st, const Token& tok,
                              const juce::String& socketName,
                              int valuePos,
                              bool preferProcessor)
    {
        drainPendingProcsToLastEvent(st);

        auto ev = std::make_unique<AstNode>();
        ev->type         = "Event";
        ev->eventType    = "local_output";
        ev->outputSocket = socketName;
        ev->commaGroup   = st.commaGroup;
        ev->line         = tok.line;
        ev->col          = tok.col;

        if (preferProcessor)
        {
            auto pr = parseProcessor(toks_, valuePos);
            if (pr.error.isNotEmpty()) {
                emitError(pr.error, tok);
                ++pos_;
                return;
            }
            pos_ = pr.newPos;
            Processor p = std::move(pr.node);
            p.commaGroup = st.commaGroup;
            ev->processors.push_back(std::move(p));
        }
        else
        {
            pos_ = valuePos;
            auto v = consumeValue();
            if (v.kind == ParseValue::Kind::Null) return;
            Processor p;
            p.name          = socketName;
            p.canonicalName = socketName;
            p.sourceOrdinal = valuePos;
            p.commaGroup    = st.commaGroup;
            p.args.push_back(std::move(v));
            ev->processors.push_back(std::move(p));
        }

        st.events.push_back(std::move(ev));
    }

    void handleLocalOutputToken(SeqBodyState& st, const Token& tok)
    {
        const auto& nameTok = peek(1);
        if (nameTok.type != TokenType::IDENT) {
            emitError("Expected local output socket name after '>' at line "
                        + juce::String(tok.line) + ", column " + juce::String(tok.col),
                      tok, "socket name (e.g. >mod)", "Add a socket name after '>'");
            ++pos_;
            return;
        }
        if (peekType(2) != TokenType::COLON) {
            emitError("Expected ':' after local output socket '>" + nameTok.value + "' at line "
                        + juce::String(nameTok.line) + ", column " + juce::String(nameTok.col),
                      nameTok, ":", "Use >" + nameTok.value + ":value");
            pos_ += 2;
            return;
        }

        const int valuePos = pos_ + 3;
        const bool processorValue =
            tokenTypeAt(toks_, valuePos) == TokenType::IDENT
            && (tokenTypeAt(toks_, valuePos + 1) == TokenType::LPAREN
                || tokenTypeAt(toks_, valuePos + 1) == TokenType::COLON);
        emitLocalOutputEvent(st, tok, nameTok.value, valuePos, processorValue);
    }

    bool tryEmitLocalBuiltInOutput(SeqBodyState& st, const Token& tok, TokenType nextTy)
    {
        if (contextModule_.isNotEmpty()) return false;
        if (tok.value != "pitch" && tok.value != "gate" && tok.value != "velocity")
            return false;
        if (nextTy != TokenType::COLON && nextTy != TokenType::LPAREN)
            return false;

        const bool processorValue = nextTy == TokenType::LPAREN;
        const int valuePos = processorValue ? pos_ : pos_ + 2;
        emitLocalOutputEvent(st, tok, tok.value, valuePos, processorValue);
        return true;
    }

    // Pitch chain + dot-param locks from the current token. Shared by the
    // step-body pitch trigger and bernoulli option parsing (T-500 — options
    // are full step content).
    void parsePitchChainAndDotParams(PitchChain& pitch,
                                     std::vector<Processor>& dotProcs,
                                     int commaGroup)
    {
        applyPitchComponent(pitch, peek());
        ++pos_; // first pitch token

        while (peekType() == TokenType::DOT) {
            const auto& next = peek(1);
            const DotRole role = classifyDot(next);
            if (role == DotRole::PitchChain) {
                ++pos_; // .
                const auto pitchTok = peek();
                applyPitchComponent(pitch, pitchTok);
                ++pos_;
            } else if (role == DotRole::ParamAccess) {
                ++pos_; // .
                const juce::String paramName = peek().value;
                const int paramOrd = (int) pos_;
                ++pos_; // IDENT
                if (peekType() == TokenType::COLON) {
                    ++pos_; // :
                    auto v = consumeValue();
                    if (v.kind == ParseValue::Kind::Null) break;
                    const auto sv = ::curlop::script::canonicalize(
                        std::string_view(paramName.toRawUTF8(),
                                         (size_t) paramName.getNumBytesAsUTF8()));
                    Processor p;
                    p.name          = paramName;
                    p.sourceOrdinal = paramOrd;
                    p.canonicalName = juce::String::fromUTF8(sv.data(), (int) sv.size());
                    p.args.push_back(std::move(v));
                    p.isDotParam    = true;
                    p.commaGroup    = commaGroup;
                    dotProcs.push_back(std::move(p));
                }
            } else {
                break;
            }
        }
    }

    void handlePitchTrigger(SeqBodyState& st, const Token& firstTok)
    {
        drainPendingProcsToLastEvent(st);
        PitchChain             pitch;
        std::vector<Processor> dotProcs;
        const int line = firstTok.line, col = firstTok.col;
        parsePitchChainAndDotParams(pitch, dotProcs, st.commaGroup);

        auto ev = std::make_unique<AstNode>();
        ev->type       = "Event";
        ev->eventType  = "trigger";
        ev->module     = contextModule_;
        ev->pitch      = pitch;
        ev->processors = std::move(dotProcs);
        ev->commaGroup = st.commaGroup;
        ev->line       = line;
        ev->col        = col;
        consumeSpaceParamLocks(
            ev.get(), &st.processors, &st.pendingProcs,
            (int) st.events.size(), st.commaGroup, st.closeType);
        st.events.push_back(std::move(ev));
    }

    bool tryEmitRestHoldKeyword(SeqBodyState& st, const Token& tok)
    {
        if (tok.value == "rest") { emitRestOrHoldEvent(st, "rest", tok); ++pos_; return true; }
        if (tok.value == "hold") { emitRestOrHoldEvent(st, "hold", tok); ++pos_; return true; }
        return false;
    }

    bool tryEmitBernoulliSelector(SeqBodyState& st, const Token& tok, TokenType nextTy)
    {
        if (tok.value != "bernoulli" || nextTy != TokenType::LPAREN) return false;
        drainPendingProcsToLastEvent(st);
        auto bern = parseBernoulliSelector(st.commaGroup, st.assigningName);
        if (bern) st.events.push_back(std::move(bern));
        return true;
    }

    bool tryEmitStandardProcessor(SeqBodyState& st, const Token& tok, TokenType nextTy)
    {
        const bool sugar = (nextTy == TokenType::COLON || nextTy == TokenType::LPAREN)
                        && isStandardProcessor(tok.value);
        const bool bareKnown = !sugar && isStandardProcessor(tok.value) && !st.events.empty();
        if (!sugar && !bareKnown) return false;

        if (bareKnown && !isNoArgOp(tok.value)) {
            juce::String msg = juce::String("Op '") + tok.value
                             + "' requires an argument at line " + juce::String(tok.line)
                             + ", column " + juce::String(tok.col)
                             + " — use '" + tok.value + ":value' (Rule 7)";
            emitError(msg, tok, tok.value + ":value",
                      juce::String("Add an argument: '") + tok.value
                        + ":1' to enable or '" + tok.value + ":0' to disable");
            ++pos_;
            return true;
        }

        auto pr = parseProcessor(toks_, pos_);
        if (pr.error.isNotEmpty()) {
            errors_.push_back(pr.error);
            ++pos_;
            return true;
        }
        pos_ = pr.newPos;
        Processor p = pr.node;
        p.commaGroup = st.commaGroup;
        if (!st.isImplicit) {
            p.afterEventCount = (int) st.events.size();
            st.processors.push_back(std::move(p));
        } else {
            st.pendingProcs.push_back(std::move(p));
        }
        return true;
    }

    bool tryReportCircularBareRef(const SeqBodyState& st, const Token& tok, TokenType nextTy)
    {
        if (st.assigningName.isEmpty() || tok.value != st.assigningName || nextTy == TokenType::DOT)
            return false;
        juce::String msg = juce::String("Circular variable reference: '") + tok.value
                         + "' references itself at line " + juce::String(tok.line)
                         + ", column " + juce::String(tok.col);
        emitError(msg, tok, juce::String(),
                  juce::String("A variable cannot reference itself — remove '")
                    + tok.value + "' from its own definition");
        ++pos_;
        return true;
    }

    bool tryEmitKnownVariableRef(SeqBodyState& st, const Token& tok, TokenType nextTy)
    {
        auto it = variables_.find(tok.value);
        if (it == variables_.end() || nextTy == TokenType::DOT) return false;
        drainPendingProcsToLastEvent(st);
        auto ref = std::make_unique<AstNode>();
        ref->type       = "VariableRef";
        ref->name       = tok.value;
        ref->resolved   = it->second;
        ref->commaGroup = st.commaGroup;
        ref->line       = tok.line;
        ref->col        = tok.col;
        ++pos_;
        st.events.push_back(std::move(ref));
        return true;
    }

    bool tryReportMissingSigil(SeqBodyState& st, const Token& tok, TokenType nextTy)
    {
        if (nextTy != TokenType::DOT || variables_.count(tok.value) != 0) return false;
        juce::String msg = juce::String("Missing '#' sigil for module '") + tok.value
                         + "' at line " + juce::String(tok.line)
                         + ", column " + juce::String(tok.col)
                         + " — use #" + tok.value;
        emitError(msg, tok, juce::String("#") + tok.value,
                  juce::String("Module references require '#' prefix: #") + tok.value);
        drainPendingProcsToLastEvent(st);
        auto ev = parseModuleExpr(st.commaGroup, st.assigningName);
        if (ev) st.events.push_back(std::move(ev));
        return true;
    }

    bool tryEmitContextParamUpdate(SeqBodyState& st, const Token& tok, TokenType nextTy)
    {
        if (nextTy != TokenType::COLON || contextModule_.isEmpty()) return false;
        drainPendingProcsToLastEvent(st);
        auto ev = std::make_unique<AstNode>();
        ev->type       = "Event";
        ev->eventType  = "update";
        ev->module     = contextModule_;
        ev->commaGroup = st.commaGroup;
        ev->line       = tok.line;
        ev->col        = tok.col;
        const juce::String paramName = tok.value;
        const int paramOrd = (int) pos_;
        pos_ += 2; // IDENT + ':'
        auto v = consumeValue();
        if (v.kind == ParseValue::Kind::Null) return true;
        const auto sv = ::curlop::script::canonicalize(
            std::string_view(paramName.toRawUTF8(),
                             (size_t) paramName.getNumBytesAsUTF8()));
        Processor p;
        p.name          = paramName;
                    p.sourceOrdinal = paramOrd;
        p.canonicalName = juce::String::fromUTF8(sv.data(), (int) sv.size());
        p.args.push_back(std::move(v));
        p.isDotParam    = true;
        p.commaGroup    = st.commaGroup;
        ev->processors.push_back(std::move(p));
        consumeSpaceParamLocks(
            ev.get(), &st.processors, &st.pendingProcs,
            (int) st.events.size(), st.commaGroup, st.closeType);
        st.events.push_back(std::move(ev));
        return true;
    }

    void emitUndefinedVariableError(const Token& tok)
    {
        juce::String msg = juce::String("Undefined variable '") + tok.value
                         + "' at line " + juce::String(tok.line)
                         + ", column " + juce::String(tok.col);
        emitError(msg, tok, juce::String(),
                  juce::String("Assign '") + tok.value + "' before use: "
                    + tok.value + " = [your_pattern]. Or did you mean #" + tok.value + "?");
        ++pos_;
    }

    // T-469 — bare `gate` (no colon): a full-step gate at the module's
    // current/last pitch. Parses as an update event carrying an argless
    // `gate` processor; the compiler emits GATE_ONLY (spec §5.1 / §6).
    bool tryEmitBareGate(SeqBodyState& st, const Token& tok, TokenType nextTy)
    {
        if (tok.value != "gate" || nextTy == TokenType::COLON
            || contextModule_.isEmpty())
            return false;
        drainPendingProcsToLastEvent(st);
        auto ev = std::make_unique<AstNode>();
        ev->type       = "Event";
        ev->eventType  = "update";
        ev->module     = contextModule_;
        ev->commaGroup = st.commaGroup;
        ev->line       = tok.line;
        ev->col        = tok.col;
        Processor p;
        p.name          = "gate";
        p.sourceOrdinal = (int) pos_;
        p.canonicalName = "gate";
        p.commaGroup    = st.commaGroup;
        ev->processors.push_back(std::move(p));
        ++pos_;
        consumeSpaceParamLocks(
            ev.get(), &st.processors, &st.pendingProcs,
            (int) st.events.size(), st.commaGroup, st.closeType);
        st.events.push_back(std::move(ev));
        return true;
    }

    void handleIdentToken(SeqBodyState& st, const Token& tok)
    {
        const auto nextTy = peekType(1);
        if (tryEmitRestHoldKeyword(st, tok))                return;
        if (tryEmitBernoulliSelector(st, tok, nextTy))      return;
        if (tryEmitLocalBuiltInOutput(st, tok, nextTy))     return;
        if (tryEmitStandardProcessor(st, tok, nextTy))      return;
        if (tryReportCircularBareRef(st, tok, nextTy))      return;
        if (tryEmitKnownVariableRef(st, tok, nextTy))       return;
        if (tryReportMissingSigil(st, tok, nextTy))         return;
        if (tryEmitBareGate(st, tok, nextTy))               return;
        if (tryEmitContextParamUpdate(st, tok, nextTy))     return;
        emitUndefinedVariableError(tok);
    }

    void emitUnexpectedTokenError(const Token& tok)
    {
        juce::String msg = juce::String("Unexpected token '") + tok.value
                         + "' at line " + juce::String(tok.line)
                         + ", column " + juce::String(tok.col);
        emitError(msg, tok, "module name, variable, bracket, or processor",
                  juce::String("Remove or fix the unexpected token '") + tok.value + "'");
        ++pos_;
    }

    // ── parseSeqBody — Parser.js:1111-1555 ────────────────────────────────
    // closeType == EOF_ means implicit (stop at NEWLINE).
    // Cut 0.D-2c: returns accumulated Step[] with full Event/VariableRef/
    // BernoulliSelector node construction. T-222: state + flush/push moved to
    // SeqBodyState + isAtCloseToken/drainPendingProcsToLastEvent/
    // applyPendingByEventShape/pushAccumulatedStep helpers.
    std::vector<AstNodePtr> parseSeqBody(TokenType closeType, const juce::String& assigningName)
    {
        SeqBodyState st;
        st.isImplicit    = (closeType == TokenType::EOF_);
        st.closeType     = closeType;
        st.assigningName = assigningName;

        while (!isAtCloseToken(st)) {
            const auto& tok = peek();
            if (tok.type == TokenType::EOF_) break;

            if (tok.type == TokenType::NEWLINE) {
                if (!st.isImplicit) { ++pos_; continue; }
                break;
            }
            if (tok.type == TokenType::SLASH)      { ++pos_; pushAccumulatedStep(st); continue; }
            if (tok.type == TokenType::COMMA)      { ++pos_; ++st.commaGroup; continue; }
            if (tok.type == TokenType::LBRACKET)   { handleBracketScope(st); continue; }
            if (tok.type == TokenType::LPAREN)     { handleParenScope(st); continue; }
            if (tok.type == TokenType::HYPHEN)     { emitRestOrHoldEvent(st, "rest", tok); ++pos_; continue; }
            if (tok.type == TokenType::UNDERSCORE) { emitRestOrHoldEvent(st, "hold", tok); ++pos_; continue; }
            if (isPersistentMarker(tok))           { handleAtToken(st); continue; }  // v1 @ or v2 set
            if (tok.type == TokenType::HASH)       { handleHashToken(st, tok); continue; }
            if (tok.type == TokenType::GT)         { handleLocalOutputToken(st, tok); continue; }
            if (tok.type == TokenType::DOLLAR)     { handleDollarToken(st, tok); continue; }
            if (tok.type == TokenType::IDENT)      { handleIdentToken(st, tok); continue; }
            if (isPitchToken(tok.type) && contextModule_.isNotEmpty()) {
                handlePitchTrigger(st, tok);
                continue;
            }
            emitUnexpectedTokenError(tok);
        }
        pushAccumulatedStep(st);  // always push final accumulated step (JS L1553)
        return std::move(st.steps);
    }

    // ── Module expression — Parser.js:1563-1636 ───────────────────────────
    // Consumes HASH-already-eaten IDENT + dot chain. commaGroup + assigningName
    // threaded through but not stored (sub-AST shallow).
    AstNodePtr parseModuleExpr(int commaGroup, const juce::String& assigningName)
    {
        juce::ignoreUnused(assigningName);
        const auto nameTok = peek();
        if (nameTok.type != TokenType::IDENT) return nullptr;
        const juce::String moduleName = nameTok.value;
        const int line = nameTok.line, col = nameTok.col;
        ++pos_; // consume IDENT

        auto canon = [](const juce::String& s) -> juce::String {
            const auto sv = ::curlop::script::canonicalize(
                std::string_view(s.toRawUTF8(), (size_t) s.getNumBytesAsUTF8()));
            return juce::String::fromUTF8(sv.data(), (int) sv.size());
        };

        PitchChain             pitch;
        std::vector<Processor> dotProcs;
        bool                   hasPitch = false;

        while (peekType() == TokenType::DOT) {
            const auto dotTok = peek();
            const auto& next  = peek(1);
            const DotRole role = classifyDot(next);

            if (role == DotRole::PitchChain) {
                hasPitch = true;
                ++pos_; // .
                const auto pitchTok = peek();
                applyPitchComponent(pitch, pitchTok);
                ++pos_; // component
                continue;
            }
            if (role == DotRole::ParamAccess) {
                ++pos_; // .
                const juce::String paramName = peek().value;
                const int paramOrd = (int) pos_;
                ++pos_; // IDENT (param name)
                if (peekType() == TokenType::COLON) {
                    ++pos_; // :
                    auto v = consumeValue();
                    if (v.kind == ParseValue::Kind::Null) break;
                    Processor p;
                    p.name          = paramName;
                    p.sourceOrdinal = paramOrd;
                    p.canonicalName = canon(paramName);
                    p.args.push_back(std::move(v));
                    p.isDotParam    = true;
                    p.commaGroup    = commaGroup;
                    dotProcs.push_back(std::move(p));
                } else if (peekType() == TokenType::LBRACE) {
                    auto values = parseVoiceStack();
                    Processor p;
                    p.name          = paramName;
                    p.sourceOrdinal = paramOrd;
                    p.canonicalName = canon(paramName);
                    p.args          = std::move(values);
                    p.isVoiceStack  = true;
                    p.commaGroup    = commaGroup;
                    dotProcs.push_back(std::move(p));
                }
                continue;
            }
            if (role == DotRole::HoldAccess) {
                ++pos_; // .
                ++pos_; // _
                // Skip the unknown-module warning when the registry is empty
                // — call sites don't populate opts_.moduleRegistry yet
                // (Spike S6 stub), so warning on every #ref is noise. When
                // the registry is wired through, this guard self-disables.
                if (! opts_.moduleRegistry.empty()
                    && opts_.moduleRegistry.count(moduleName) == 0) {
                    emitWarning(juce::String("Unknown module '") + moduleName
                                  + "' at line " + juce::String(line)
                                  + ", column " + juce::String(col)
                                  + " — not in module registry",
                                nameTok,
                                juce::String("Check the module name spelling, or register '")
                                  + moduleName + "' in the module registry");
                }
                auto ev = std::make_unique<AstNode>();
                ev->type       = "Event";
                ev->eventType  = "hold";
                ev->module     = moduleName;
                ev->processors = std::move(dotProcs);
                ev->commaGroup = commaGroup;
                ev->line       = line;
                ev->col        = col;
                return ev;
            }
            juce::String msg = juce::String("Invalid token after '.' at line ")
                             + juce::String(dotTok.line) + ", column " + juce::String(dotTok.col)
                             + " — expected note, frequency, chord, cents offset, or parameter name";
            emitError(msg, dotTok,
                      "note (c3), frequency (440hz), chord (min7), cents (+10c), or parameter name",
                      juce::String("After '.' specify a pitch component or a parameter name"));
            break;
        }

        if (! opts_.moduleRegistry.empty()
            && opts_.moduleRegistry.count(moduleName) == 0) {
            emitWarning(juce::String("Unknown module '") + moduleName
                          + "' at line " + juce::String(line)
                          + ", column " + juce::String(col)
                          + " — not in module registry",
                        nameTok,
                        juce::String("Check the module name spelling, or register '")
                          + moduleName + "' in the module registry");
        }

        auto ev = std::make_unique<AstNode>();
        ev->type       = "Event";
        ev->eventType  = hasPitch ? "trigger" : "update";
        ev->module     = moduleName;
        if (hasPitch) ev->pitch = pitch;
        ev->processors = std::move(dotProcs);
        ev->commaGroup = commaGroup;
        ev->line       = line;
        ev->col        = col;
        return ev;
    }

    // T-500 — postfix ops written on a bernoulli option (`c3 ratchet:4`)
    // attach to THAT option's event, up to the next comma / closing paren.
    // Options are full step content (§5.12 infinitely-nested-modulation law).
    void attachBernoulliOptionOps(AstNode& ev, int commaGroup)
    {
        while (pos_ < (int) toks_.size()) {
            if (peekType() != TokenType::IDENT) break;
            const auto nx = peekType(1);
            if (nx != TokenType::COLON && nx != TokenType::LPAREN) break;
            auto pr = parseProcessor(toks_, pos_);
            if (pr.error.isNotEmpty()) {
                errors_.push_back(pr.error);
                ++pos_;
                break;
            }
            pos_ = pr.newPos;
            Processor p = pr.node;
            p.commaGroup = commaGroup;
            ev.processors.push_back(std::move(p));
        }
    }

    // ── parseBernoulliSelector — Parser.js:1642-1698 ──────────────────────
    AstNodePtr parseBernoulliSelector(int commaGroup, const juce::String& assigningName)
    {
        const auto startTok = peek();
        ++pos_; // 'bernoulli' IDENT
        ++pos_; // '('

        auto weight = consumeValue();
        if (weight.kind == ParseValue::Kind::Null) return nullptr;

        std::vector<AstNodePtr> options;
        while (pos_ < (int) toks_.size()) {
            const auto ty = peekType();
            if (ty == TokenType::RPAREN) { ++pos_; break; }
            if (ty == TokenType::COMMA) { ++pos_; } else break;

            const auto optTy = peekType();
            if (optTy == TokenType::COMMA || optTy == TokenType::RPAREN) {
                // §5.8 — empty option = keep original (the written event the
                // selector postfixes). Separator/paren stays for the loop.
                auto ev = std::make_unique<AstNode>();
                ev->type = "Event"; ev->eventType = "keep";
                options.push_back(std::move(ev));
                continue;
            }
            if (optTy == TokenType::HASH) {
                ++pos_;
                auto ev = parseModuleExpr(0, assigningName);
                if (ev) {
                    attachBernoulliOptionOps(*ev, commaGroup);   // T-500
                    options.push_back(std::move(ev));
                }
            } else if (optTy == TokenType::HYPHEN) {
                ++pos_;
                auto ev = std::make_unique<AstNode>();
                ev->type = "Event"; ev->eventType = "rest";
                options.push_back(std::move(ev));
            } else if (optTy == TokenType::UNDERSCORE) {
                // §5.8 lists '_' (hold) as a V1 option, but a fire-time hold
                // needs gate extension of the PREVIOUS note after its gate-off
                // already fired — machine support doesn't exist yet. Error
                // loudly rather than degrade to a rest silently (T-492).
                const auto& t = peek();
                errors_.push_back(juce::String("bernoulli '_' (hold) option is not supported yet at line ")
                                  + juce::String(t.line) + ", column " + juce::String(t.col)
                                  + " — use '' (keep original) or '-' (rest)");
                ++pos_;
            } else if (optTy == TokenType::NOTE || optTy == TokenType::FREQ) {
                // T-500 — full step content: pitch CHAIN (chords included),
                // dot-param locks, postfix ops, all bound to this option.
                auto ev = std::make_unique<AstNode>();
                ev->type = "Event";
                ev->line = peek().line;
                ev->col  = peek().col;
                if (contextModule_.isNotEmpty()) {
                    ev->eventType = "trigger";
                    ev->module    = contextModule_;
                    PitchChain pc;
                    std::vector<Processor> dotProcs;
                    parsePitchChainAndDotParams(pc, dotProcs, commaGroup);
                    ev->pitch      = pc;
                    ev->processors = std::move(dotProcs);
                    attachBernoulliOptionOps(*ev, commaGroup);
                } else {
                    ++pos_;
                    ev->eventType = "rest";
                }
                options.push_back(std::move(ev));
            } else {
                const auto& t = peek();
                if (t.type == TokenType::EOF_ || t.type == TokenType::RPAREN) break;
                errors_.push_back(juce::String("Unexpected token in bernoulli options at line ")
                                  + juce::String(t.line) + ", column " + juce::String(t.col));
                break;
            }
        }

        if (options.size() < 2) {
            // Never swallow the selector silently — a vanished event is the
            // worst failure mode (T-492; the step would just not play).
            errors_.push_back(juce::String("bernoulli needs a weight and at least 2 options at line ")
                              + juce::String(startTok.line) + ", column "
                              + juce::String(startTok.col));
            return nullptr;
        }

        const double weightVal =
            (weight.kind == ParseValue::Kind::Number
          || weight.kind == ParseValue::Kind::UnitNumber)
            ? weight.number : 0.5;

        auto node = std::make_unique<AstNode>();
        node->type       = "BernoulliSelector";
        node->weight     = weightVal;
        node->options    = std::move(options);
        node->commaGroup = commaGroup;
        node->line       = startTok.line;
        node->col        = startTok.col;
        return node;
    }

    // ── consumeSpaceParamLocks — Parser.js:1708-1749 ──────────────────────
    void consumeSpaceParamLocks(
        AstNode*                targetEvent,
        std::vector<Processor>* parentProcessors,
        std::vector<Processor>* parentPendingProcs,
        int                     parentEventsSize,
        int                     commaGroup,
        TokenType               closeType)
    {
        const bool isImplicit = (closeType == TokenType::EOF_);
        auto canon = [](const juce::String& s) -> juce::String {
            const auto sv = ::curlop::script::canonicalize(
                std::string_view(s.toRawUTF8(), (size_t) s.getNumBytesAsUTF8()));
            return juce::String::fromUTF8(sv.data(), (int) sv.size());
        };

        while (pos_ < (int) toks_.size()) {
            const auto& sp    = peek();
            const auto nextTy = peekType(1);
            if (sp.type != TokenType::IDENT) break;
            if (nextTy != TokenType::COLON && nextTy != TokenType::LBRACE) break;

            // SF-052 — space-form voice stack: `<module> param{v1,v2,...}`
            // (e.g. `#synth.c3 cutoff{200,2000,8000}`, Neo's `gain{}` syntax).
            // Mirrors the dot-form voice stack (parsePitchChainAndDotParams)
            // — a per-voice value lock attached to the target trigger event;
            // module params are never step ops, so it always binds the event.
            if (nextTy == TokenType::LBRACE) {
                jassert(targetEvent != nullptr);
                const juce::String paramName = sp.value;
                const int paramOrd = (int) pos_;
                ++pos_; // consume the param IDENT; parseVoiceStack reads the `{`
                auto values = parseVoiceStack();
                Processor p;
                p.name          = paramName;
                p.sourceOrdinal = paramOrd;
                p.canonicalName = canon(paramName);
                p.args          = std::move(values);
                p.isVoiceStack  = true;
                p.commaGroup    = commaGroup;
                targetEvent->processors.push_back(std::move(p));
                continue;
            }

            if (isStandardProcessor(sp.value)) {
                auto pr = parseProcessor(toks_, pos_);
                if (pr.error.isNotEmpty()) { errors_.push_back(pr.error); break; }
                pos_ = pr.newPos;

                jassert(targetEvent != nullptr);
                const bool argIsGenerator =
                    !pr.node.args.empty()
                    && pr.node.args.front().kind == ParseValue::Kind::Generator;
                if (argIsGenerator) {
                    Processor p = pr.node;
                    p.isDotParam = true;
                    p.commaGroup = commaGroup;
                    targetEvent->processors.push_back(std::move(p));
                    continue;
                }
                if (!isImplicit) {
                    if (parentProcessors) {
                        Processor p = pr.node;
                        p.commaGroup      = commaGroup;
                        p.afterEventCount = parentEventsSize + 1;
                        parentProcessors->push_back(std::move(p));
                    }
                } else {
                    if (parentPendingProcs) {
                        Processor p = pr.node;
                        p.commaGroup = commaGroup;
                        parentPendingProcs->push_back(std::move(p));
                    }
                }
                continue;
            }

            // Non-standard IDENT:value → attach to targetEvent
            jassert(targetEvent != nullptr);
            const juce::String paramName = sp.value;
            const int paramOrd = (int) pos_;
            pos_ += 2; // IDENT + COLON
            auto v = consumeValue();
            if (v.kind == ParseValue::Kind::Null) break;
            Processor p;
            p.name          = paramName;
                    p.sourceOrdinal = paramOrd;
            p.canonicalName = canon(paramName);
            p.args.push_back(std::move(v));
            p.isDotParam    = true;
            p.commaGroup    = commaGroup;
            targetEvent->processors.push_back(std::move(p));
        }
    }

    // ── parseVoiceStack — Parser.js:1760-1771 ─────────────────────────────
    std::vector<ParseValue> parseVoiceStack()
    {
        std::vector<ParseValue> values;
        if (peekType() != TokenType::LBRACE) return values;
        ++pos_; // {
        while (peekType() != TokenType::RBRACE && !atEnd()) {
            auto v = consumeValue();
            if (v.kind == ParseValue::Kind::Null) break;
            values.push_back(std::move(v));
            if (peekType() == TokenType::COMMA) ++pos_;
        }
        if (peekType() == TokenType::RBRACE) ++pos_;
        return values;
    }

    // ── consumeValue — Parser.js:1782-1787 ────────────────────────────────
    ParseValue consumeValue()
    {
        auto r = parseValue(toks_, pos_);
        if (r.error.isNotEmpty()) {
            errors_.push_back(r.error);
            return ParseValue{};
        }
        pos_ = r.newPos;
        return std::move(r.value);
    }
};

// ─── Cut 0.D-3e₁: per-step timing resolution (Parser.js:1859-2098 port) ────
// File-local helpers. Entry point `resolveTimings(ScriptAst&)` is defined in
// namespace scope below and forwards into these helpers.

static void resolveSeqTimingsImpl(AstNode& node, double availableBeats);
static void resolveParallelTimings(std::vector<AstNodePtr>& steps);
static void resolveTupletTimings(std::vector<AstNodePtr>& steps, double availableBeats);
static double getNaturalBeatsFor(const AstNode& step);
static double getStepMultiplierFor(AstNode& step);
static int    getRepeatCountFor(const AstNode& step);

// Mirror Parser.js:2043-2080 `_getStepMultiplier`. Handles Number / UnitNumber /
// String (note-name/khz/hz) processor args. Populates step.stepUnitArg for
// non-numeric cases so 3e₂ can resolve at compile time with real BPM.
static double getStepMultiplierFor(AstNode& step)
{
    constexpr double REF_BPM = 120.0;
    for (const auto& proc : step.processors) {
        if (proc.name != "step" && proc.canonicalName != "step") continue;
        if (proc.args.empty()) continue;
        const ParseValue& arg = proc.args[0];
        if (arg.kind == ParseValue::Kind::Number) {
            return arg.number;
        }
        if (arg.kind == ParseValue::Kind::UnitNumber) {
            step.stepUnitArg = arg;
            const juce::String& u = arg.unit;
            if (u == "ms") return arg.number * (REF_BPM / 60000.0);
            if (u == "s")  return arg.number * REF_BPM / 60.0;
            if (u == "hz") return (1000.0 / arg.number) * (REF_BPM / 60000.0);
            if (u == "%")  return arg.number / 100.0;
            return arg.number;
        }
        if (arg.kind == ParseValue::Kind::String) {
            step.stepUnitArg = arg;
            // Note/freq string branch — estimate only; full resolve in 3e₂.
            return 1.0;
        }
    }
    return 1.0;
}

// Mirror Parser.js:2086-2108 `_getRepeatCount` (step + event fallback).
static int getRepeatCountFor(const AstNode& step)
{
    for (const auto& proc : step.processors) {
        if ((proc.name == "repeat" || proc.canonicalName == "repeat")
            && !proc.args.empty()
            && proc.args[0].kind == ParseValue::Kind::Number) {
            return std::max(1, static_cast<int>(std::floor(proc.args[0].number)));
        }
    }
    for (const auto& evPtr : step.events) {
        if (!evPtr) continue;
        for (const auto& proc : evPtr->processors) {
            if ((proc.name == "repeat" || proc.canonicalName == "repeat")
                && !proc.args.empty()
                && proc.args[0].kind == ParseValue::Kind::Number) {
                return std::max(1, static_cast<int>(std::floor(proc.args[0].number)));
            }
        }
    }
    return 1;
}

// Mirror Parser.js:1904-1919 `_getNaturalBeats`. Tuplet events excluded.
static double getNaturalBeatsFor(const AstNode& step)
{
    double naturalBeats = 1.0;
    for (const auto& evPtr : step.events) {
        if (!evPtr) continue;
        const AstNode& ev = *evPtr;
        if (ev.type == "VariableRef" && ev.resolved
            && ev.resolved->type == "Sequence") {
            double varBeats = 0.0;
            for (const auto& sPtr : ev.resolved->steps) {
                varBeats += (sPtr && sPtr->duration > 0) ? sPtr->duration : 1.0;
            }
            if (varBeats > naturalBeats) naturalBeats = varBeats;
        }
        if (ev.type == "Sequence" && ev.scopeType != "tuplet") {
            double nestedBeats = 0.0;
            for (const auto& sPtr : ev.steps) {
                nestedBeats += (sPtr && sPtr->duration > 0) ? sPtr->duration : 1.0;
            }
            if (nestedBeats > naturalBeats) naturalBeats = nestedBeats;
        }
    }
    return naturalBeats;
}

// Mirror Parser.js:1929-1981 `_resolveParallelTimings`.
static void resolveParallelTimings(std::vector<AstNodePtr>& steps)
{
    double cursor = 0.0;
    for (auto& stepPtr : steps) {
        if (!stepPtr) continue;
        AstNode& step = *stepPtr;
        const double stepMult = getStepMultiplierFor(step);

        // Bottom-up: resolve nested parallel (non-tuplet) sequences first so
        // their step durations feed into naturalBeats below. Tuplets compress
        // into parent — resolved later with perCopyDuration.
        for (auto& evPtr : step.events) {
            if (!evPtr) continue;
            AstNode& ev = *evPtr;
            if (ev.type == "Sequence" && ev.scopeType != "tuplet") {
                resolveSeqTimingsImpl(ev, 1.0);
            } else if (ev.type == "VariableRef" && ev.resolved
                       && ev.resolved->type == "Sequence"
                       && ev.resolved->scopeType != "tuplet") {
                resolveSeqTimingsImpl(*ev.resolved, 1.0);
            }
        }

        const double naturalBeats = getNaturalBeatsFor(step);
        const int    repeatMult   = getRepeatCountFor(step);
        const double duration     = naturalBeats * stepMult * static_cast<double>(repeatMult);

        step.beat_offset = cursor;
        step.duration    = duration;
        cursor += duration;

        const double perCopyDuration = (repeatMult > 0)
            ? duration / static_cast<double>(repeatMult)
            : duration;

        for (auto& evPtr : step.events) {
            if (!evPtr) continue;
            AstNode& ev = *evPtr;
            if (ev.type == "Sequence" && ev.scopeType == "tuplet") {
                resolveSeqTimingsImpl(ev, perCopyDuration);
            } else if (ev.type == "VariableRef" && ev.resolved
                       && ev.resolved->type == "Sequence"
                       && ev.resolved->scopeType == "tuplet") {
                resolveSeqTimingsImpl(*ev.resolved, perCopyDuration);
            }
        }
    }
}

// Mirror Parser.js:1999-2034 `_resolveTupletTimings`. Weights = stepMult ×
// naturalBeats; distribute availableBeats proportionally; recurse inner
// Sequences with allocated dur (VariableRef forces tuplet-style compression).
static void resolveTupletTimings(std::vector<AstNodePtr>& steps, double availableBeats)
{
    if (steps.empty()) return;

    std::vector<double> weights;
    weights.reserve(steps.size());
    double totalWeight = 0.0;
    for (auto& stepPtr : steps) {
        if (!stepPtr) { weights.push_back(1.0); totalWeight += 1.0; continue; }
        const double w = getStepMultiplierFor(*stepPtr) * getNaturalBeatsFor(*stepPtr);
        weights.push_back(w);
        totalWeight += w;
    }
    if (totalWeight <= 0.0) totalWeight = static_cast<double>(steps.size());

    double cursor = 0.0;
    for (size_t i = 0; i < steps.size(); ++i) {
        auto& stepPtr = steps[i];
        if (!stepPtr) continue;
        AstNode& step = *stepPtr;
        const double dur = (weights[i] / totalWeight) * availableBeats;
        step.beat_offset = cursor;
        step.duration    = dur;
        cursor += dur;

        for (auto& evPtr : step.events) {
            if (!evPtr) continue;
            AstNode& ev = *evPtr;
            if (ev.type == "Sequence") {
                resolveSeqTimingsImpl(ev, dur);
            } else if (ev.type == "VariableRef" && ev.resolved
                       && ev.resolved->type == "Sequence") {
                // Force tuplet-style compression into dur beats.
                if (!ev.resolved->steps.empty()) {
                    resolveTupletTimings(ev.resolved->steps, dur);
                }
            }
        }
    }
}

// Mirror Parser.js:1872-1894 `_resolveSeqTimings`. Dispatch by node type.
static void resolveSeqTimingsImpl(AstNode& node, double availableBeats)
{
    if (node.type == "Sequence") {
        if (node.scopeType == "tuplet") {
            resolveTupletTimings(node.steps, availableBeats);
        } else {
            resolveParallelTimings(node.steps);
        }
        return;
    }
    if (node.type == "Assignment" && node.value) {
        resolveSeqTimingsImpl(*node.value, availableBeats);
        return;
    }
    // PersistentSetter / Event / Step / BernoulliSelector — no timing at this
    // level. Parser.js matches by not recursing further.
}

} // namespace (anonymous)

// ─── Public API ───────────────────────────────────────────────────────────

ScriptAst parse(const juce::String& source, const ParseOptions& opts)
{
    Tokenizer tk(source);
    auto tkResult = tk.tokenize();

    Parser p(std::move(tkResult.tokens), opts);
    auto ast = p.parseScript();

    // Merge tokenizer + parser diagnostics + errors into ast (tokenizer first)
    ast.errors.reserve(tkResult.errors.size() + p.errors().size());
    for (auto& e : tkResult.errors)      ast.errors.push_back(std::move(e));
    for (auto& e : p.errors())           ast.errors.push_back(std::move(e));

    ast.diagnostics.reserve(tkResult.diagnostics.size() + p.diagnostics().size());
    for (auto& d : tkResult.diagnostics) ast.diagnostics.push_back(std::move(d));
    for (auto& d : p.diagnostics())      ast.diagnostics.push_back(std::move(d));

    resolveTimings(ast);                 // Cut 0.D-3e₁ — populate step.duration + beat_offset + stepUnitArg

    ast.shape = computeShapeFingerprint(ast);
    return ast;
}

// Cut 0.D-3e₁: per-step timing resolver entry. Mirror Parser.js:1859-1863.
void resolveTimings(ScriptAst& ast)
{
    for (auto& stmt : ast.statements) {
        if (stmt) resolveSeqTimingsImpl(*stmt, 1.0);
    }
}

// V2 syntax cut B follow-on (T-253, stub): reserved-name registry impl.
// See ScriptParser.h for band breakdown and cut-C wiring plan. Single source
// of truth for "names a user cannot bind as a variable / module / context".
bool isReservedName(const juce::String& name)
{
    // Band 1: load-bearing keywords
    if (name == "set") return true;

    // Band 2: control-op names (V2 §1.7 + `midi`)
    if (name == "lfo"      || name == "ad"        || name == "adsr"
     || name == "auto"     || name == "random"    || name == "deviate"
     || name == "keytrack" || name == "bernoulli" || name == "shepard"
     || name == "accum"    || name == "glide"     || name == "midi") return true;

    // Band 3: sequencer op names
    if (name == "reverse"   || name == "shuffle"  || name == "transpose"
     || name == "invert"    || name == "rotate"   || name == "prob"
     || name == "len"       || name == "onset"    || name == "step"
     || name == "groove"    || name == "timescale"|| name == "sort"
     || name == "grid"      || name == "octave"   || name == "cond"
     || name == "repeat"    || name == "ratchet"  || name == "flam"
     || name == "bounce"    || name == "buzz"     || name == "geiger"
     || name == "strum") return true;

    // Band 4: V2 future-reserved (gate triggers + output convention)
    if (name == "x"   || name == "X"   || name == "out"
     || name == "rest"|| name == "hold") return true;

    return false;
}

juce::String computeShapeFingerprint(const ScriptAst& ast)
{
    juce::StringArray stmtTypes;
    for (auto const& s : ast.statements)
        stmtTypes.add(s ? s->type : juce::String("UNKNOWN"));

    juce::StringArray sortedVars;
    for (auto const& v : ast.variableNames) sortedVars.add(v);

    const juce::String prefix =
        juce::String((int) ast.statements.size()) + ":"
        + stmtTypes.joinIntoString(",") + ":"
        + juce::String((int) ast.variableNames.size()) + ":"
        + sortedVars.joinIntoString(",");

    // Cut 0.D-2c Step 19 — nested accumulator. Depth-first walk; Step counts
    // totalSteps, Event/VariableRef/BernoulliSelector counts totalEvents.
    int totalSteps  = 0;
    int totalEvents = 0;

    std::function<void(const AstNode*)> walk = [&](const AstNode* n) {
        if (!n) return;
        if (n->type == "Step") {
            ++totalSteps;
            for (const auto& e : n->events) walk(e.get());
            return;
        }
        if (n->type == "Event" || n->type == "VariableRef"
            || n->type == "BernoulliSelector") {
            ++totalEvents;
            for (const auto& opt : n->options) walk(opt.get());
            return;
        }
        // Sequence / Assignment / PersistentSetter — walk children
        for (const auto& s : n->steps)  walk(s.get());
        for (const auto& e : n->events) walk(e.get());
        if (n->value)       walk(n->value.get());
        if (n->setterEvent) walk(n->setterEvent.get());
    };
    for (const auto& stmt : ast.statements) walk(stmt.get());

    return prefix + ":" + juce::String(totalSteps) + ":" + juce::String(totalEvents);
}

// ─── Deep-clone helpers (Cut 0.D-3a) ──────────────────────────────────────
// ScriptAst owns std::vector<AstNodePtr> which is move-only. copyAst walks
// the tree recursively, cloning every AstNode. GeneratorNode/CondNode are
// held via shared_ptr inside ParseValue — copy is shallow (cheap share, no
// ownership contention because observation reads never mutate those nodes).

static AstNodePtr cloneNode(const AstNode& src);

static AstNodePtr cloneNodeOrNull(const AstNodePtr& src)
{
    return src ? cloneNode(*src) : AstNodePtr{};
}

static std::vector<AstNodePtr> cloneVec(const std::vector<AstNodePtr>& src)
{
    std::vector<AstNodePtr> out;
    out.reserve(src.size());
    for (const auto& n : src) out.push_back(cloneNodeOrNull(n));
    return out;
}

static AstNodePtr cloneNode(const AstNode& src)
{
    auto out = std::make_unique<AstNode>();
    out->type       = src.type;
    out->name       = src.name;
    out->scopeType  = src.scopeType;
    out->eventType  = src.eventType;
    out->module     = src.module;
    out->outputSocket = src.outputSocket;
    out->pitch      = src.pitch;
    out->processors = src.processors;       // Processor values copy-safe; shared_ptrs shared.
    out->steps      = cloneVec(src.steps);
    out->events     = cloneVec(src.events);
    out->options    = cloneVec(src.options);
    out->weight     = src.weight;
    out->value      = cloneNodeOrNull(src.value);
    out->setterEvent       = cloneNodeOrNull(src.setterEvent);
    out->setterProcessor   = src.setterProcessor;
    out->isSetterProcessor = src.isSetterProcessor;
    out->commaGroup        = src.commaGroup;
    out->line              = src.line;
    out->col               = src.col;
    // Cut 0.D-3e₁ — resolved timing fields.
    out->duration          = src.duration;
    out->beat_offset       = src.beat_offset;
    out->stepUnitArg       = src.stepUnitArg;
    return out;
}

// ─── VariableRef.resolved re-wire after clone (Cut 0.D-3d Step 2a) ────────
// cloneNode can't re-resolve VariableRef.resolved by itself — the ptr points
// cross-node (into another Assignment's value subtree). After cloneVec on
// statements, walk the cloned tree and re-wire every VariableRef.resolved
// by name lookup in the new tree. Without this, live oracle path
// (ClipStateContainer::getClipScriptAst → copyAst → compile) sees all
// VariableRef.resolved = nullptr and T-047 branch silently skips every
// variable reference.
static void bindResolvedRefs(std::vector<AstNodePtr>& nodes,
                                const std::unordered_map<juce::String, AstNode*>& nameToValue);

static void bindResolvedOne(AstNode& n,
                               const std::unordered_map<juce::String, AstNode*>& nameToValue)
{
    if (n.type == "VariableRef") {
        auto it = nameToValue.find(n.name);
        n.resolved = (it != nameToValue.end()) ? it->second : nullptr;
    }
    bindResolvedRefs(n.steps,   nameToValue);
    bindResolvedRefs(n.events,  nameToValue);
    bindResolvedRefs(n.options, nameToValue);
    if (n.value)       bindResolvedOne(*n.value,       nameToValue);
    if (n.setterEvent) bindResolvedOne(*n.setterEvent, nameToValue);
}

static void bindResolvedRefs(std::vector<AstNodePtr>& nodes,
                                const std::unordered_map<juce::String, AstNode*>& nameToValue)
{
    for (auto& n : nodes) if (n) bindResolvedOne(*n, nameToValue);
}

ScriptAst copyAst(const ScriptAst& src)
{
    ScriptAst out;
    out.statements    = cloneVec(src.statements);
    out.variableNames = src.variableNames;
    out.errors        = src.errors;
    out.diagnostics   = src.diagnostics;
    out.shape         = src.shape;

    // Re-wire VariableRef.resolved ptrs into the cloned tree.
    std::unordered_map<juce::String, AstNode*> nameToValue;
    for (auto& s : out.statements) {
        if (s && s->type == "Assignment" && s->value)
            nameToValue[s->name] = s->value.get();
    }
    bindResolvedRefs(out.statements, nameToValue);

    return out;
}

juce::String buildScriptDiagnosticsJson(int                                  clipId,
                                        const std::vector<ScriptDiagnostic>& diags,
                                        const juce::String&                  astShape)
{
    auto toStd = [](const juce::String& s) { return std::string(s.toRawUTF8()); };

    std::string out;
    out += "{\"type\":\"SCRIPT_DIAGNOSTICS\",\"clipId\":";
    out += std::to_string(clipId);
    out += ",\"astShape\":\"";
    out += ::curlop::escapeJsonString(toStd(astShape));
    out += "\",\"diagnostics\":[";

    for (size_t i = 0; i < diags.size(); ++i) {
        const auto& d = diags[i];
        if (i > 0) out += ",";
        out += "{\"severity\":\"";
        out += (d.severity == Severity::Error ? "error" : "warning");
        out += "\",\"message\":\"";
        out += ::curlop::escapeJsonString(toStd(d.message));
        out += "\",\"line\":";
        out += std::to_string(d.line);
        out += ",\"col\":";
        out += std::to_string(d.col);
        out += ",\"token\":\"";
        out += ::curlop::escapeJsonString(toStd(d.token));
        out += "\",\"expected\":\"";
        out += ::curlop::escapeJsonString(toStd(d.expected));
        out += "\",\"suggestion\":\"";
        out += ::curlop::escapeJsonString(toStd(d.suggestion));
        out += "\"}";
    }

    out += "]}";
    return juce::String::fromUTF8(out.c_str(), (int) out.size());
}

} // namespace curlop::script
