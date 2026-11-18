// Issue #5263: aura --fmt and comments.
//
// The reader keeps no trivia: Lexer::skip_ws drops ';' line comments and
// '#| |#' block comments, and the flat AST records no source spans. So the
// formatter has to work on the raw source text. This file is the one lexical
// scanner both halves of #5263 use:
//
// reindent() is the conservative formatter: line-leading whitespace only,
// every other byte copied verbatim. The first wave of the fix (refusing to
// overwrite a comment-bearing file) used the scanner's comment test; now
// that comments survive, that refusal is gone and only the skip helpers
// remain.
//
// It is a copy of the reader's literal rules, not a second parser: string
// escapes, |symbol| quoting, character literals (#\ ) and nested block
// comments all pass through untouched.
namespace cli_fmt_5263 {

// Byte-level type tests. Source is UTF-8; every byte >= 0x80 is an ordinary
// token byte, so no decoding happens here.
[[nodiscard]] inline bool fmt_is_hspace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
}
[[nodiscard]] inline bool fmt_is_atom_char(char c) noexcept {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        return true;
    switch (c) {
        case '_':
        case '+':
        case '*':
        case '-':
        case '/':
        case '=':
        case '<':
        case '>':
        case '!':
        case '?':
        case ':':
        case '&':
        case '%':
        case '.':
        case '^':
        case '~':
        case '$':
        case '@':
        case '\'':
        case ',':
            return true;
        default:
            return false;
    }
}

// i points at the opening quote. Returns the index just past the literal
// (src.size() when unterminated, so callers stop at EOF instead of looping).
[[nodiscard]] inline std::size_t fmt_skip_string(std::string_view s, std::size_t i) noexcept {
    const std::size_t n = s.size();
    ++i; // opening "
    while (i < n) {
        const char c = s[i];
        if (c == '\\' && i + 1 < n) {
            i += 2; // covers \" \\ \xHH (the hex digits are plain bytes here)
            continue;
        }
        if (c == '"')
            return i + 1;
        ++i;
    }
    return n;
}

// i points at '|'. R7RS |...| symbols may hold spaces, ';' and newlines, with
// a backslash escape. A '|#' with no open block comment is the reader's #4875
// error token: step over one byte instead of swallowing text.
[[nodiscard]] inline std::size_t fmt_skip_pipe_symbol(std::string_view s, std::size_t i) noexcept {
    const std::size_t n = s.size();
    if (i + 1 < n && s[i + 1] == '#')
        return i + 1;
    ++i; // opening |
    while (i < n) {
        const char c = s[i];
        if (c == '\\' && i + 1 < n) {
            i += 2;
            continue;
        }
        if (c == '|')
            return i + 1;
        ++i;
    }
    return n;
}

// i points at '#', s[i + 1] is a backslash. Consume #\name or #\x; the byte
// after the backslash may itself be '"', ';' or '|', which must not open a
// literal or a comment.
[[nodiscard]] inline std::size_t fmt_skip_character(std::string_view s, std::size_t i) noexcept {
    const std::size_t n = s.size();
    i += 2; // '#', backslash
    if (i >= n)
        return n;
    if (fmt_is_atom_char(s[i])) {
        while (i < n && fmt_is_atom_char(s[i]))
            ++i;
        return i;
    }
    return i + 1;
}

// ── Conservative reindent (second wave) ─────────────────────────────────────
//
// Only the leading whitespace of a line may change. Every other byte —
// comment text, string and character literals, |symbol| quoting, the
// (define (f x) ...) sugar, the number of top-level forms — is copied
// through unchanged. Indentation follows the lisp-indent-function shape:
// a body form (define, lambda, let, when, ...) indents its subforms two
// columns past the open paren; any other call aligns its arguments under
// the first argument; a line that starts with ')' aligns with its opener.
namespace detail {

    struct Frame {
        std::size_t open_col = 0;  // column of the '(' on the output line
        std::size_t child_col = 0; // column a new subform line uses
        std::string head;          // head atom, while it is still being read
        bool head_open = true;
    };

    [[nodiscard]] inline bool body_form(std::string_view h) noexcept {
        // Emacs scheme-indent-function 0 or 1: all subforms two columns in.
        static constexpr std::string_view kBody[] = {
            "define",
            "define-values",
            "define-syntax",
            "define-macro",
            "define-hygienic-macro",
            "define-record-type",
            "lambda",
            "let",
            "let*",
            "letrec",
            "letrec*",
            "let-values",
            "let*-values",
            "let-syntax",
            "letrec-syntax",
            "when",
            "unless",
            "begin",
            "cond",
            "case",
            "do",
            "and",
            "or",
            "if",
            "parameterize",
            "guard",
            "dynamic-wind",
            "match",
            "syntax-rules",
            "delay",
            "delay-force",
        };
        for (const auto& form : kBody) {
            if (h == form)
                return true;
        }
        return false;
    }

} // namespace detail

// Reindent src. The result is a pure function of the input text, so
// reindent(reindent(x)) == reindent(x).
[[nodiscard]] inline std::string reindent(std::string_view src) {
    const std::size_t n = src.size();
    std::string out;
    out.reserve(n + n / 8 + 16);
    std::vector<detail::Frame> stack;
    std::size_t col = 0; // columns emitted on the current output line
    bool line_start = true;
    std::size_t i = 0;

    const auto raw = [&](char c) {
        out.push_back(c);
        if (c == '\n') {
            col = 0;
            line_start = false; // a copied newline is not a line to re-indent
        } else if (c != '\r') {
            ++col;
        }
    };
    const auto finish_head = [&](char delim) {
        if (stack.empty())
            return;
        auto& f = stack.back();
        if (!f.head_open)
            return;
        f.head_open = false;
        if (f.head.empty()) { // head was '(' or a literal: align under it
            f.child_col = f.open_col + 1;
            return;
        }
        f.child_col = detail::body_form(f.head)
                          ? f.open_col + 2
                          : col + ((delim == '\n' || fmt_is_hspace(delim)) ? 1 : 0);
    };

    while (i < n) {
        const char c = src[i];
        if (line_start) {
            line_start = false;
            std::size_t j = i;
            while (j < n && (src[j] == ' ' || src[j] == '\t'))
                ++j;
            if (j >= n) { // trailing blank at EOF
                i = n;
                break;
            }
            if (src[j] == '\n' || src[j] == '\r') { // blank line: no indent
                i = j;
                continue;
            }
            // A block comment's column is part of its text: leave that line
            // (and everything inside the comment) exactly as written.
            const bool verbatim = src[j] == '#' && j + 1 < n && src[j + 1] == '|';
            if (verbatim) {
                for (std::size_t k = i; k < j; ++k)
                    raw(src[k]);
            } else {
                std::size_t ind = 0;
                if (!stack.empty())
                    ind = (src[j] == ')') ? stack.back().open_col : stack.back().child_col;
                out.append(ind, ' ');
                col = ind;
            }
            i = j;
            continue;
        }

        if (c == '\n') {
            out.push_back('\n');
            col = 0;
            line_start = true;
            ++i;
            continue;
        }
        if (c == '"') { // string literal: byte-for-byte
            finish_head(c);
            const std::size_t end = fmt_skip_string(src, i);
            for (; i < end; ++i)
                raw(src[i]);
            continue;
        }
        if (c == '|') { // |symbol|
            finish_head(c);
            const std::size_t end = fmt_skip_pipe_symbol(src, i);
            for (; i < end; ++i)
                raw(src[i]);
            continue;
        }
        if (c == ';') { // to end of line
            while (i < n && src[i] != '\n')
                raw(src[i++]);
            continue;
        }
        if (c == '#' && i + 1 < n && src[i + 1] == '|') { // block comment, nests
            int nest = 1;
            raw(src[i++]);
            raw(src[i++]);
            while (i < n && nest > 0) {
                if (src[i] == '#' && i + 1 < n && src[i + 1] == '|') {
                    raw(src[i++]);
                    raw(src[i++]);
                    ++nest;
                    continue;
                }
                if (src[i] == '|' && i + 1 < n && src[i + 1] == '#') {
                    raw(src[i++]);
                    raw(src[i++]);
                    --nest;
                    continue;
                }
                raw(src[i++]);
            }
            continue;
        }
        if (c == '#' && i + 1 < n && src[i + 1] == '\\') { // #\x character
            finish_head(c);
            const std::size_t end = fmt_skip_character(src, i);
            for (; i < end; ++i)
                raw(src[i]);
            continue;
        }
        if (c == '#' && i + 1 < n && src[i + 1] == '(') { // #( vector
            finish_head(c);
            detail::Frame f;
            f.open_col = col;
            f.child_col = col + 1;
            stack.push_back(f);
            raw(src[i++]);
            raw(src[i++]);
            continue;
        }
        if (c == '(') {
            finish_head(c);
            detail::Frame f;
            f.open_col = col;
            f.child_col = col + 1;
            stack.push_back(f);
            raw(src[i++]);
            continue;
        }
        if (c == ')') {
            finish_head(c);
            if (!stack.empty())
                stack.pop_back();
            raw(src[i++]);
            continue;
        }
        if (fmt_is_hspace(c)) {
            finish_head(c);
            raw(src[i++]);
            continue;
        }
        // atom byte: part of the frame's head while the head is still open
        if (!stack.empty() && stack.back().head_open)
            stack.back().head.push_back(c);
        raw(src[i++]);
    }
    return out;
}

} // namespace cli_fmt_5263
