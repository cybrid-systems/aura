// Issue #5263: aura --fmt and comments.
//
// The reader keeps no trivia: Lexer::skip_ws drops ';' line comments and
// '#| |#' block comments, and the flat AST records no source spans. So the
// formatter has to work on the raw source text. This file is the one lexical
// scanner both halves of #5263 use:
//
//   - has_comment():  wave one refuses to overwrite a file whose comments the
//                     current unparser would silently delete;
//   - reindent():     wave two's conservative formatter -- line-leading
//                     whitespace only, every other byte copied verbatim.
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
    case '_': case '+': case '*': case '-': case '/': case '=': case '<': case '>':
    case '!': case '?': case ':': case '&': case '%': case '.': case '^': case '~':
    case '$': case '@': case '\'': case ',':
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

// True when the source contains a ';' line comment or a '#| ... |#' block
// comment outside every string, |symbol| and character literal.
[[nodiscard]] inline bool has_comment(std::string_view s) noexcept {
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (c == ';')
            return true;
        if (c == '"') {
            i = fmt_skip_string(s, i);
            continue;
        }
        if (c == '|') {
            i = fmt_skip_pipe_symbol(s, i);
            continue;
        }
        if (c == '#' && i + 1 < n) {
            if (s[i + 1] == '|')
                return true;
            if (s[i + 1] == '\\') {
                i = fmt_skip_character(s, i);
                continue;
            }
        }
        ++i;
    }
    return false;
}

} // namespace cli_fmt_5263
