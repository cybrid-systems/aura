// form_split.hh — one top-level form splitter for every entry point.
//
// #5239: serve, eval and the CLI each carried their own paren/string scanner
// and printed different parse errors for the same source. This is the single
// implementation; callers that only need an "incomplete input" signal (REPL)
// may still keep their state, but an error must come from the parser.
#ifndef AURA_COMPILER_FORM_SPLIT_HH
#define AURA_COMPILER_FORM_SPLIT_HH

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aura::compiler::form_split {

inline std::size_t character_literal_length(std::string_view src, std::size_t i) {
    if (i + 1 >= src.size() || src[i] != '#' || src[i + 1] != '\\')
        return 0;
    std::size_t p = i + 2; // past the '#' and the '\\'
    if (p >= src.size())
        return 0; // bare #\ at EOF — malformed, the lexer reports it
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F')
            return 10 + (c - 'A');
        return -1;
    };
    const char c = src[p];
    if ((c == 'x' || c == 'X') && p + 1 < src.size() && hex_val(src[p + 1]) >= 0) {
        ++p; // the 'x'
        int digits = 0;
        while (p < src.size() && digits < 6 && hex_val(src[p]) >= 0) {
            ++p;
            ++digits;
        }
        return p - i;
    }
    if (!std::isalpha(static_cast<unsigned char>(c)))
        return p + 1 - i; // single-character form
    while (p < src.size() && (std::isalnum(static_cast<unsigned char>(src[p])) || src[p] == '-'))
        ++p;
    return p - i;
}

// #5259: byte range of one top-level form in the source it was split from.
struct FormSpan {
    std::size_t begin = 0;
    std::size_t end = 0; // one past the last character of the form
};

// Result of splitting an exec chunk into its top-level forms.
struct FormSplit {
    std::vector<std::string> forms;
    // Empty ⇔ the chunk is balanced and every form was recovered.
    // Otherwise it is the ready-to-report diagnostic (line:col prefix
    // included) and `forms` holds nothing that may be evaluated.
    std::string error;
    // #5259: byte range of each entry in `forms`, parallel to it. The LSP
    // symbol layer reads definitions from these spans instead of searching
    // the text for "(define ". Empty exactly when `forms` is empty.
    std::vector<FormSpan> spans;
    // #5259: every comment in the source. The LSP needs them for folding and
    // to refuse a definition inside one; they are never part of a form.
    std::vector<FormSpan> comments;
    bool ok() const noexcept { return error.empty(); }
};

// Split a chunk of Aura source into its top-level forms.
//
// The scan tracks delimiter depth ('(' / '['), skips string literals,
// character literals and ';' line comments, and keeps the leading text
// of every form so a diagnostic raised on a form keeps the line
// number it has inside the chunk. An unmatched closer or an unclosed
// opener is reported as an error instead of being recovered from.
inline FormSplit split_top_level_forms(std::string_view src) {
    FormSplit out;
    std::string current;
    int depth = 0;
    bool in_string = false;
    std::uint32_t line = 1;
    std::uint32_t col = 1;
    std::uint32_t open_line = 1;
    std::uint32_t open_col = 1;
    // #5259: byte range of the form being collected (leading whitespace and
    // the ' \` , prefixes are not part of it).
    std::size_t form_begin = std::string_view::npos;
    std::size_t form_end = std::string_view::npos;
    constexpr std::size_t kNone = std::string_view::npos;

    // Advance the line/column cursor over the "count" characters that
    // start at "from" in the source.
    auto advance = [&](std::size_t from, std::size_t count) {
        for (std::size_t k = 0; k < count; ++k) {
            if (src[from + k] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
    };
    // A rejected chunk yields NO form to evaluate: the forms recovered
    // before the error are dropped so no caller can run a truncated
    // chunk by accident.
    auto at = [&](std::uint32_t l, std::uint32_t c, const char* what) {
        out.forms.clear();
        out.spans.clear();
        out.comments.clear();
        out.error = std::to_string(l) + ":" + std::to_string(c) + ": parse error: " + what;
    };
    auto push_form = [&]() {
        // Keep the leading text (line numbers stay absolute inside the
        // chunk); trim the trailing whitespace only.
        std::size_t end = current.size();
        while (end > 0 && std::isspace(static_cast<unsigned char>(current[end - 1])))
            --end;
        current.resize(end);
        bool blank = true;
        for (char c : current) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                blank = false;
                break;
            }
        }
        if (!blank) {
            out.forms.push_back(current);
            if (form_begin != kNone)
                out.spans.push_back({form_begin, form_end});
        }
        current.clear();
        form_begin = form_end = kNone;
    };
    // Append src[from .. from+count) to the form text and track its span.
    auto take = [&](std::size_t from, std::size_t count) {
        for (std::size_t k = 0; k < count; ++k) {
            const char ch = src[from + k];
            current += ch;
            if (std::isspace(static_cast<unsigned char>(ch)))
                continue;
            if (form_begin == kNone)
                form_begin = from + k;
            form_end = from + k + 1;
        }
    };

    for (std::size_t i = 0; i < src.size();) {
        const char c = src[i];
        if (in_string) {
            if (c == '\\' && i + 1 < src.size()) {
                take(i, 2);
                advance(i, 2);
                i += 2;
                continue;
            }
            take(i, 1);
            if (c == '"')
                in_string = false;
            advance(i, 1);
            ++i;
            continue;
        }
        if (c == '#' && i + 1 < src.size() && src[i + 1] == '|') {
            // #5259: R7RS block comments hide their contents from the form
            // splitter too — a "(define ghost …)" inside one is not a form.
            // The newlines inside stay in the text so the line numbers of
            // the following forms do not move. An unclosed "#|" is left to
            // the parser (it reports the error) exactly as before.
            std::size_t j = i;
            int nest = 0;
            bool closed = false;
            while (j < src.size()) {
                if (src[j] == '#' && j + 1 < src.size() && src[j + 1] == '|') {
                    j += 2;
                    ++nest;
                    continue;
                }
                if (src[j] == '|' && j + 1 < src.size() && src[j + 1] == '#') {
                    j += 2;
                    if (--nest == 0) {
                        closed = true;
                        break;
                    }
                    continue;
                }
                ++j;
            }
            if (closed) {
                out.comments.push_back({i, j});
                for (; i < j; ++i) {
                    const char cc = src[i];
                    if (cc == '\n')
                        current += '\n';
                    advance(i, 1);
                }
                continue;
            }
        }
        if (c == '#') {
            const std::size_t lit = character_literal_length(src, i);
            if (lit > 0) {
                take(i, lit);
                advance(i, lit);
                i += lit;
                continue;
            }
        }
        if (c == ';') {
            // Line comment: the text is dropped, the newline stays so
            // the line numbers of the following forms are unchanged.
            if (depth > 0)
                current += ';';
            std::size_t j = i + 1;
            while (j < src.size() && src[j] != '\n')
                ++j;
            out.comments.push_back({i, j});
            advance(i, j - i);
            i = j;
            continue;
        }
        if (c == '"') {
            take(i, 1);
            in_string = true;
            advance(i, 1);
            ++i;
            continue;
        }
        if (c == '(' || c == '[') {
            if (depth == 0 && !current.empty()) {
                // A new form starts here. A leftover that is only a
                // prefix char (' ` ,) belongs to the new form.
                bool is_prefix = false;
                bool only_prefix = true;
                for (char pc : current) {
                    if (pc == '\'' || pc == '`' || pc == ',') {
                        is_prefix = true;
                        continue;
                    }
                    if (!std::isspace(static_cast<unsigned char>(pc))) {
                        only_prefix = false;
                        break;
                    }
                }
                // #5234: whitespace (or a lone ' quote prefix) before the
                // next form is that form's leading text. It carries the
                // absolute line number; dropping it reported 1:L for a
                // form on chunk line L.
                if (!only_prefix)
                    push_form();
            }
            if (depth == 0) {
                open_line = line;
                open_col = col;
            }
            take(i, 1);
            ++depth;
            advance(i, 1);
            ++i;
            continue;
        }
        if (c == ')' || c == ']') {
            if (depth == 0) {
                at(line, col, c == ')' ? "unexpected ')'" : "unexpected ']'");
                return out;
            }
            take(i, 1);
            --depth;
            advance(i, 1);
            ++i;
            if (depth == 0)
                push_form();
            continue;
        }
        take(i, 1);
        advance(i, 1);
        ++i;
    }
    if (in_string) {
        at(line, col, "unterminated string literal");
        return out;
    }
    if (depth > 0) {
        at(open_line, open_col, "unclosed '(' (missing ')')");
        return out;
    }
    // Trailing text that is not a parenthesised form (a bare atom).
    if (!current.empty())
        push_form();
    return out;
}

} // namespace aura::compiler::form_split

#endif // AURA_COMPILER_FORM_SPLIT_HH
