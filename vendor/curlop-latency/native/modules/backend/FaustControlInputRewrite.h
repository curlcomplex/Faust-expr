#pragma once

// FaustControlInputRewrite (SF-094, T-666) — pre-compile source transform that
// turns a CURLOP-convention Faust module's UI controls into `process` SIGNAL
// INPUTS, so each param is read at AUDIO RATE (a UI zone is control-rate by
// Faust's compilation model — `fSlow`; only process inputs are per-sample).
//
// CONVENTION (what the user authors): each control is a top-level binding
//     <ident> = hslider|vslider|nentry|button|checkbox("<label>"[meta], ...);
// and `process` is `process = <body>;` or `process(<audioIns>) = <body>;`.
// The body references the control idents.
//
// TARGET (what we compile): the control idents become leading `process` inputs,
//     process(<ctrl1>, <ctrl2>, ..., <audioIns>) = <body>;
// with the UI bindings removed. Control inputs come FIRST (before any audio
// tail), matching FaustNode's [control .. audio] channel order.
//
// The captured (ident, label, role, scaleMeta) metadata is returned so the host
// keeps names / ranges / roles / faceplate after the UI controls are gone — the
// rewrite SUBSUMES capture for these params. Role typing reuses the canonical
// `faustControlInputOf` name convention (gate->Gate, freq->Pitch, velocity->Velocity).
//
// Fail-loud: a source without a `process` definition returns an error; a source
// with no controls returns unchanged (controls empty). Multiple complete
// declarations may share a source line; multi-line/computed declarations simply
// are not matched — they stay a UI control. The convention is what CURLOP
// authors and what the catalog uses.

#include "modules/backend/FaustUiCapture.h"   // faustControlInputOf + vm::SignalType (via its includes)

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <vector>

namespace curlop {

struct RewrittenControl
{
    std::string     ident;       // original Faust binding name, retained as an alias
    std::string     processInputIdent; // generated raw process input for that alias
    std::string     label;       // bare UI label (metadata stripped), drives role + schema
    std::string     scaleMeta;   // "log" etc. from [scale:...], else empty
    std::string     minimumExpression; // original widget lower bound
    std::string     maximumExpression; // original widget upper bound
    std::string     postWidgetExpression; // source transform after the UI widget
    bool            buttonLike = false;
    vm::SignalType  role = vm::SignalType::Value;
};

struct RewrittenFaust
{
    std::string                   source;     // rewritten (process inputs); == input if no controls
    std::vector<RewrittenControl> controls;   // declaration order = process-input order
    std::string                   error;      // non-empty => fail-loud, source left unchanged
};

namespace detail {

// "tune[scale:log]" -> bare "tune" + meta "log". Faust treats [k:v] as metadata,
// not part of the label, so the role classifier must see the bare label.
inline void splitFaustLabel(const std::string& raw, std::string& bare, std::string& scaleMeta)
{
    const auto br = raw.find('[');
    bare = (br == std::string::npos) ? raw : raw.substr(0, br);
    // trim trailing space on bare
    while (! bare.empty() && (bare.back() == ' ' || bare.back() == '\t')) bare.pop_back();
    scaleMeta.clear();
    std::smatch m;
    static const std::regex scaleRe(R"(\[scale:([A-Za-z]+)\])");
    if (std::regex_search(raw, m, scaleRe)) scaleMeta = m[1].str();
}

inline std::string trimFaustSource(std::string text)
{
    while (! text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (! text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

inline std::size_t matchingFaustParen(const std::string& source, std::size_t open)
{
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = open; index < source.size(); ++index) {
        const char character = source[index];
        if (quoted) {
            if (character == '"' && ! escaped)
                quoted = false;
            escaped = character == '\\' && ! escaped;
            continue;
        }
        if (character == '"') {
            quoted = true;
            escaped = false;
            continue;
        }
        if (character == '(')
            ++depth;
        else if (character == ')' && --depth == 0)
            return index;
    }
    return std::string::npos;
}

inline std::vector<std::string> splitFaustArguments(const std::string& arguments)
{
    std::vector<std::string> values;
    std::string value;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const char character : arguments) {
        if (quoted) {
            value.push_back(character);
            if (character == '"' && ! escaped)
                quoted = false;
            escaped = character == '\\' && ! escaped;
            continue;
        }
        if (character == '"') {
            quoted = true;
            escaped = false;
            value.push_back(character);
        } else if (character == '(') {
            ++depth;
            value.push_back(character);
        } else if (character == ')') {
            --depth;
            value.push_back(character);
        } else if (character == ',' && depth == 0) {
            values.push_back(trimFaustSource(std::move(value)));
            value.clear();
        } else {
            value.push_back(character);
        }
    }
    values.push_back(trimFaustSource(std::move(value)));
    return values;
}

inline bool parseFaustControlStatement(const std::string& statement,
                                       const std::smatch& declaration,
                                       std::size_t processInputIndex,
                                       RewrittenControl& control,
                                       std::string& error)
{
    control.ident = declaration[1].str();
    const std::string primitive = declaration[2].str();
    control.buttonLike = primitive == "button" || primitive == "checkbox";
    splitFaustLabel(declaration[3].str(), control.label, control.scaleMeta);
    control.processInputIdent = "curlop_control_input_"
        + std::to_string(processInputIndex);

    const auto primitivePosition = static_cast<std::size_t>(declaration.position(2));
    const auto open = statement.find('(', primitivePosition + primitive.size());
    const auto close = open == std::string::npos
        ? std::string::npos : matchingFaustParen(statement, open);
    if (close == std::string::npos) {
        error = "unterminated " + primitive + " declaration for " + control.ident;
        return false;
    }
    const auto arguments = splitFaustArguments(
        statement.substr(open + 1, close - open - 1));
    if (control.buttonLike) {
        control.minimumExpression = "0.0";
        control.maximumExpression = "1.0";
    } else if (arguments.size() >= 4u
               && ! arguments[2].empty() && ! arguments[3].empty()) {
        control.minimumExpression = arguments[2];
        control.maximumExpression = arguments[3];
    } else {
        error = primitive + " declaration for " + control.ident
            + " has no recoverable range";
        return false;
    }
    control.postWidgetExpression = trimFaustSource(statement.substr(close + 1));
    if (! control.postWidgetExpression.empty()
        && control.postWidgetExpression.back() == ';')
        control.postWidgetExpression = trimFaustSource(
            control.postWidgetExpression.substr(
                0, control.postWidgetExpression.size() - 1));
    control.role = faustControlInputOf(control.label, control.buttonLike,
                                       0.0f, 0.0f, 1.0f, control.scaleMeta).type;
    return true;
}

} // namespace detail

inline RewrittenFaust rewriteControlsToProcessInputs(const std::string& source)
{
    RewrittenFaust out;

    // A top-level control binding: `<ident> = <primitive>("<label>" ...`.
    // Custom RX delimiter: the pattern contains `)"` (the [^"]* close), which
    // would terminate a plain R"(...)" raw string early.
    static const std::regex declRe(
        R"RX(^\s*([A-Za-z_]\w*)\s*=\s*(hslider|vslider|nentry|button|checkbox)\s*\(\s*"([^"]*)"[\s\S]*$)RX");
    // The process definition line: optional `(<inputs>)`, then `=`, then body tail.
    static const std::regex procRe(
        R"RX(^(\s*)process\s*(?:\(([^)]*)\))?\s*=([\s\S]*)$)RX");

    // Faust scoping: `process` pattern args are visible ONLY inside the process
    // expression — NOT to other top-level definitions. A module's helper defs
    // (e.g. `freq = tune * ...`) reference the controls, so once the controls
    // become process args those helpers must move into a `with{}` block on
    // process, where the args ARE in scope. We therefore split the source:
    //   topLines    — declare / import / comment / blank: stay at top level
    //   helperLines — every other definition: move into process's with{} block
    //   controls    — UI bindings: become leading process inputs
    //   process     — its rhs becomes the with{}-wrapped process body
    auto isTopKeep = [] (const std::string& l) -> bool {
        size_t i = 0;
        while (i < l.size()
               && (l[i] == ' ' || l[i] == '\t'
                   || l[i] == '\r' || l[i] == '\n'))
            ++i;
        if (i >= l.size()) return true;                       // blank
        if (l.compare(i, 2, "//") == 0) return true;          // comment
        if (l.compare(i, 8, "declare ") == 0) return true;
        if (l.compare(i, 7, "import(") == 0 || l.compare(i, 7, "import ") == 0) return true;
        return false;
    };

    // Faust permits several top-level declarations on one physical line. Work
    // on semicolon-terminated statements so `process = ...` after a helper on
    // the same line is still found, without splitting quoted metadata or
    // comments that happen to contain a semicolon.
    std::vector<std::string> statements;
    std::string statement;
    bool quoted = false;
    bool escaped = false;
    bool lineComment = false;
    int braceDepth = 0;
    for (std::size_t character = 0; character < source.size(); ++character)
    {
        const char c = source[character];
        if (! quoted && ! lineComment && c == '/'
            && character + 1 < source.size() && source[character + 1] == '/')
            lineComment = true;
        statement.push_back(c);
        if (lineComment) {
            if (c == '\n') {
                statements.push_back(std::move(statement));
                statement.clear();
                lineComment = false;
            }
            continue;
        }
        if (quoted) {
            if (c == '"' && ! escaped)
                quoted = false;
            escaped = c == '\\' && ! escaped;
            continue;
        }
        if (c == '"') {
            quoted = true;
            escaped = false;
            continue;
        }
        if (c == '{') {
            ++braceDepth;
            continue;
        }
        if (c == '}' && braceDepth > 0) {
            --braceDepth;
            continue;
        }
        if (c == ';' && braceDepth == 0) {
            statements.push_back(std::move(statement));
            statement.clear();
        }
    }
    if (! statement.empty())
        statements.push_back(std::move(statement));

    // Some released authored modules use a finite indexed helper such as
    // `gain(i)=hslider("gain_%i",...)` and then call gain(1)..gain(8).
    // Expand that static authoring shorthand before the ordinary top-level
    // control rewrite.  The result still has one real process input per
    // captured parameter; no control is left at a Faust UI default.
    static const std::regex indexedControlRe(
        R"RX(^\s*([A-Za-z_]\w*)\s*\(\s*i\s*\)\s*=\s*(hslider|vslider|nentry|button|checkbox)\s*\(\s*"([^"]*%i[^"]*)"[\s\S]*$)RX");
    for (std::size_t statementIndex = 0; statementIndex < statements.size(); ++statementIndex) {
        std::smatch indexed;
        if (!std::regex_match(statements[statementIndex], indexed, indexedControlRe))
            continue;
        const auto helper = indexed[1].str();
        // A released mixer-style module commonly calls a widget helper through
        // a chain of parameterised helpers: `g(i)` -> `preL(i, x)` ->
        // `postL(i, x)` -> `postL(1, a1)`.  Looking only for direct `g(1)`
        // calls leaves g/p/m/... as UI zones while the schema has expanded
        // them to eight parameters.  Follow first-index `i` helper
        // dependencies before finding the finite literal call sites.
        std::vector<std::string> reachable { helper };
        static const std::regex indexedDefinitionRe(
            R"RX(\b([A-Za-z_]\w*)\s*\(\s*i(?:\s*,[^)]*)?\)\s*=\s*([^;]*);)RX");
        bool expandedReachability = true;
        while (expandedReachability) {
            expandedReachability = false;
            for (std::sregex_iterator definition(source.begin(), source.end(), indexedDefinitionRe), end;
                 definition != end; ++definition) {
                const auto defined = (*definition)[1].str();
                const auto body = (*definition)[2].str();
                const bool dependsOnReachable = std::any_of(
                    reachable.begin(), reachable.end(), [&] (const auto& candidate) {
                        return std::regex_search(
                            body, std::regex("\\b" + candidate + R"(\s*\(\s*i\b)"));
                    });
                if (dependsOnReachable
                    && std::find(reachable.begin(), reachable.end(), defined) == reachable.end()) {
                    reachable.push_back(defined);
                    expandedReachability = true;
                }
            }
        }
        int maximumIndex = 0;
        for (const auto& candidate : reachable) {
            const std::regex literalCallRe(
                "\\b" + candidate + R"(\s*\(\s*([0-9]+)\s*(?:,|\)))");
            for (std::sregex_iterator it(source.begin(), source.end(), literalCallRe), end;
                 it != end; ++it)
                maximumIndex = std::max(maximumIndex, std::stoi((*it)[1].str()));
        }
        if (maximumIndex == 0)
            continue;

        std::vector<std::string> expanded;
        expanded.reserve(static_cast<std::size_t>(maximumIndex));
        for (int index = 1; index <= maximumIndex; ++index) {
            auto declaration = statements[statementIndex];
            const auto number = std::to_string(index);
            const std::regex definitionRe("^(\\s*)" + helper
                                          + R"(\s*\(\s*i\s*\))");
            declaration = std::regex_replace(
                declaration, definitionRe, "$1" + helper + "_" + number);
            std::size_t placeholder = 0;
            while ((placeholder = declaration.find("%i", placeholder)) != std::string::npos) {
                declaration.replace(placeholder, 2, number);
                placeholder += number.size();
            }
            expanded.push_back(std::move(declaration));
        }
        // Keep the authored helper callable from other parameterised Faust
        // helpers (for example `preL(i, x)` calling `g(i)`). Replacing only
        // direct `g(1)` sites leaves those downstream helpers dangling. The
        // dispatcher selects the matching finite lowered control while the
        // direct declarations above remain the real process inputs.
        std::string dispatcher = helper + "(i) = ";
        for (int index = 1; index <= maximumIndex; ++index) {
            if (index != 1)
                dispatcher += " + ";
            dispatcher += "(i == " + std::to_string(index) + ") * "
                + helper + "_" + std::to_string(index);
        }
        dispatcher += ";";
        expanded.push_back(std::move(dispatcher));
        statements.erase(statements.begin() + static_cast<std::ptrdiff_t>(statementIndex));
        statements.insert(statements.begin() + static_cast<std::ptrdiff_t>(statementIndex),
                          expanded.begin(), expanded.end());
        statementIndex += expanded.size() - 1;
    }

    std::vector<std::string> topLines, helperLines;
    bool sawProcess = false;
    std::string procIndent, procExisting, procBody;

    for (auto& line : statements)
    {
        std::smatch m;
        if (std::regex_match(line, m, declRe))
        {
            RewrittenControl c;
            if (! detail::parseFaustControlStatement(
                    line, m, out.controls.size(), c, out.error)) {
                out.source = source;
                return out;
            }
            out.controls.push_back(std::move(c));
            continue;
        }
        if (! sawProcess && std::regex_match(line, m, procRe))
        {
            sawProcess = true;
            procIndent   = m[1].str();
            procExisting = m[2].str();
            procBody     = m[3].str();
            continue;
        }
        if (isTopKeep(line)) topLines.push_back(line);
        else                 helperLines.push_back(line);
    }

    if (! sawProcess)
    {
        out.error = "no `process` definition found";
        out.source = source;
        return out;
    }

    // No controls => nothing to rewrite; hand the source back untouched.
    if (out.controls.empty())
    {
        out.source = source;
        return out;
    }

    // process args = control idents first, then any pre-existing audio inputs.
    std::string args;
    for (size_t i = 0; i < out.controls.size(); ++i)
    {
        if (i) args += ", ";
        args += out.controls[(size_t) i].processInputIdent;
    }
    const std::string existing = detail::trimFaustSource(procExisting);
    if (! existing.empty()) args += ", " + existing;

    // Strip the trailing `;` off the process rhs so we can wrap it.
    std::string body = detail::trimFaustSource(procBody);
    if (! body.empty() && body.back() == ';') {
        body.pop_back();
        body = detail::trimFaustSource(std::move(body));
    }

    std::string rebuilt;
    for (size_t i = 0; i < topLines.size(); ++i) rebuilt += topLines[(size_t) i] + "\n";
    rebuilt += procIndent + "process(" + args + ") = (" + body + ")";
    if (! helperLines.empty() || ! out.controls.empty())
    {
        rebuilt += " with {\n";
        for (const auto& control : out.controls) {
            // A Faust UI primitive gives the compiler a static interval. Once
            // it becomes a raw process input, retain that exact range before
            // applying the source-authored post-widget transform (smoothing,
            // unit conversion, etc.). This keeps delay/index safety proofs and
            // preserves the original signal shape rather than discarding it.
            rebuilt += control.ident + " = min(" + control.maximumExpression
                + ", max(" + control.minimumExpression + ", "
                + control.processInputIdent + "))"
                + control.postWidgetExpression + ";\n";
        }
        for (size_t i = 0; i < helperLines.size(); ++i) rebuilt += helperLines[(size_t) i] + "\n";
        rebuilt += "}";
    }
    rebuilt += ";\n";
    out.source = std::move(rebuilt);
    return out;
}

} // namespace curlop
