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
        // #5348: lisp-indent-function's distinguished-argument count. With a
        // head at end of line, those arguments start one body indent further
        // in (open_col + 4), and the remaining subforms use open_col + 2 -
        // what indent-region does for let/when/lambda/define.
        int body_args = 0;
        bool first_arg = false;         // the next subform is a distinguished arg
        bool first_arg_started = false; // that subform has consumed input
    };

    // lisp-indent-function for the form: >= 0 is a body form whose first
    // N subforms are distinguished; -1 means "align under arg 1".
    [[nodiscard]] inline int body_form_args(std::string_view h) noexcept {
        static constexpr std::string_view kZero[] = {
            "cond",         "do",    "and",         "or",           "begin",
            "parameterize", "delay", "delay-force", "dynamic-wind", "syntax-rules",
        };
        for (const auto& form : kZero) {
            if (h == form)
                return 0;
        }
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
                return 1;
        }
        return -1;
    }

    [[nodiscard]] inline bool body_form(std::string_view h) noexcept {
        return body_form_args(h) >= 0;
    }

} // namespace detail

// Reindent src. The result is a pure function of the input text, so
// reindent(reindent(x)) == reindent(x).
//
// tab_size and insert_spaces are the editor's FormattingOptions (#5263):
// tab_size is the column a tab advances to (and the unit insert_spaces =
// false writes), NOT the body-form offset — that stays the lisp-indent
// width of 2 columns (#5348). The defaults (2, spaces) are what the CLI
// uses and what lisp-indent-function means.
[[nodiscard]] inline std::string reindent(std::string_view src, int tab_size = 2,
                                          bool insert_spaces = true) {
    // #5348: FormattingOptions.tabSize is "size of a tab in spaces", not the
    // Lisp body indent. The body offset stays the lisp-indent width (2, what
    // the CLI uses); tab_size only sets the column a tab advances to, and an
    // insertSpaces = false line still lands on the same columns.
    const std::size_t step = tab_size > 0 ? static_cast<std::size_t>(tab_size) : 2;
    constexpr std::size_t kBodyIndent = 2;
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
        } else if (c == '\t') {
            col = (col / step + 1) * step; // a copied tab still moves the column
        } else if (c != '\r') {
            ++col;
        }
    };
    const auto put_indent = [&](std::size_t target) {
        if (insert_spaces) {
            out.append(target, ' ');
        } else {
            out.append(target / step, '\t');
            out.append(target % step, ' ');
        }
        col = target;
    };
    const auto finish_head = [&](char delim) {
        if (stack.empty())
            return;
        auto& f = stack.back();
        if (!f.head_open)
            return;
        f.head_open = false;
        if (f.head.empty()) { // head was '(' or a literal: align under it
            f.body_args = -1;
            f.child_col = f.open_col + 1;
            return;
        }
        const int args = detail::body_form_args(f.head);
        f.body_args = args;
        f.child_col = args >= 0 ? f.open_col + kBodyIndent
                                : col + ((delim == '\n' || fmt_is_hspace(delim)) ? 1 : 0);
    };
    // #5348: a head that ends the line has no first argument to align under.
    // A body form still indents its subforms two columns past the paren; any
    // other call keeps the lisp-indent column for that shape, open_col + 1.
    // Finishing the head here is what makes the first clause line agree with
    // the rest of a cond/let/when whose head sits at end of line.
    const auto finish_head_eol = [&]() {
        if (stack.empty())
            return;
        auto& f = stack.back();
        if (!f.head_open)
            return;
        f.head_open = false;
        const int args = detail::body_form_args(f.head);
        f.body_args = args;
        f.first_arg = args > 0;
        f.first_arg_started = false;
        f.child_col = args > 0    ? f.open_col + 2 * kBodyIndent
                      : args == 0 ? f.open_col + kBodyIndent
                                  : f.open_col + 1;
    };
    // The distinguished first argument ended: the rest of the form is body.
    const auto end_first_arg = [&]() {
        if (stack.empty())
            return;
        auto& f = stack.back();
        if (!f.first_arg)
            return;
        f.first_arg = false;
        f.first_arg_started = false;
        f.child_col = f.open_col + kBodyIndent;
    };
    const auto mark_first_arg_started = [&]() {
        if (!stack.empty() && stack.back().first_arg && !stack.back().first_arg_started)
            stack.back().first_arg_started = true;
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
                put_indent(ind);
            }
            i = j;
            continue;
        }

        if (c == '\n') {
            finish_head_eol();
            if (!stack.empty() && stack.back().first_arg && stack.back().first_arg_started)
                end_first_arg();
            out.push_back('\n');
            col = 0;
            line_start = true;
            ++i;
            continue;
        }
        if (c == '"') { // string literal: byte-for-byte
            finish_head(c);
            mark_first_arg_started();
            const std::size_t end = fmt_skip_string(src, i);
            for (; i < end; ++i)
                raw(src[i]);
            continue;
        }
        if (c == '|') { // |symbol|
            finish_head(c);
            mark_first_arg_started();
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
            mark_first_arg_started();
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
            mark_first_arg_started();
            const std::size_t end = fmt_skip_character(src, i);
            for (; i < end; ++i)
                raw(src[i]);
            continue;
        }
        if (c == '#' && i + 1 < n && src[i + 1] == '(') { // #( vector
            finish_head(c);
            mark_first_arg_started();
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
            mark_first_arg_started();
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
            // #5348: the closed form was the frame's distinguished first
            // argument; the rest of that form is body-indented.
            if (!stack.empty() && stack.back().first_arg)
                end_first_arg();
            raw(src[i++]);
            continue;
        }
        if (fmt_is_hspace(c)) {
            // A CR before the LF ends the line like the LF does; finishing the
            // head twice is a no-op, so CRLF files take the EOL rule too.
            if (c == '\r' && i + 1 < n && src[i + 1] == '\n')
                finish_head_eol();
            else
                finish_head(c);
            if (!stack.empty() && stack.back().first_arg && stack.back().first_arg_started)
                end_first_arg();
            raw(src[i++]);
            continue;
        }
        // atom byte: part of the frame's head while the head is still open
        if (!stack.empty() && stack.back().head_open)
            stack.back().head.push_back(c);
        else
            mark_first_arg_started();
        raw(src[i++]);
    }
    return out;
}

// Syntax gate shared by the CLI and the language server: true when the reader
// accepts src. Callers that are about to rewrite a buffer call this first —
// a file the reader rejects is never reformatted (--fmt -i exits nonzero, the
// LSP formatting request answers with no edits).
[[nodiscard]] inline bool parses(std::string_view src, std::string* error = nullptr) {
    aura::compiler::CompilerService cs;
    auto alloc = cs.arena().allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto pr = aura::parser::parse_to_flat(src, flat, pool);
    if (pr.success && pr.root != aura::ast::NULL_NODE)
        return true;
    if (error != nullptr)
        *error = pr.error;
    return false;
}

} // namespace cli_fmt_5263
