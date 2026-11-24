// Issue #5438: text helpers for publishing a define.
//
// Three publishers wrap a bare rebind value as `(define name body)`: the
// rebind's function_sources_ row, the lockless batch path, and the set-code
// text patch. They share canonical_define_source so the three texts cannot
// drift, and so a body that ends in a line comment still reads back. The
// set-code patch finds the define form by name with the same reader the
// wrapper uses for comments, strings and character literals.
#ifndef AURA_COMPILER_DEFINE_SOURCE_HH
#define AURA_COMPILER_DEFINE_SOURCE_HH

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aura::compiler {

[[nodiscard]] inline bool source_is_space(char character) noexcept {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

// A character that ends a token: whitespace, a bracket, a string or a
// comment start.
[[nodiscard]] inline bool source_ends_token(char character) noexcept {
    return source_is_space(character) || character == '(' || character == ')' || character == '[' ||
           character == ']' || character == '{' || character == '}' || character == '"' ||
           character == ';';
}

// Skips the literal that starts at `pos`: a `;` comment, a `#| |#` block
// comment (nested), a `#\x` character literal or a string. Returns `pos`
// unchanged when the character there starts none of them.
[[nodiscard]] inline std::size_t source_skip_literal(std::string_view text,
                                                     std::size_t pos) noexcept {
    if (pos >= text.size())
        return pos;
    const char character = text[pos];
    if (character == ';') {
        while (pos < text.size() && text[pos] != '\n')
            ++pos;
        return pos;
    }
    if (character == '#' && pos + 1 < text.size() && text[pos + 1] == '|') {
        std::size_t depth = 1;
        pos += 2;
        while (pos < text.size() && depth != 0) {
            if (text[pos] == '#' && pos + 1 < text.size() && text[pos + 1] == '|') {
                ++depth;
                pos += 2;
            } else if (text[pos] == '|' && pos + 1 < text.size() && text[pos + 1] == '#') {
                --depth;
                pos += 2;
            } else {
                ++pos;
            }
        }
        return pos;
    }
    if (character == '#' && pos + 1 < text.size() && text[pos + 1] == '\\') {
        pos += 2;
        while (pos < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[pos])) != 0 || text[pos] == '-'))
            ++pos;
        return pos;
    }
    if (character == '"') {
        ++pos;
        while (pos < text.size()) {
            if (text[pos] == '\\') {
                pos += 2;
                continue;
            }
            if (text[pos] == '"') {
                ++pos;
                break;
            }
            ++pos;
        }
        return pos;
    }
    return pos;
}

// True when the text ends inside a `;` comment, so a paren appended after
// it would be part of that comment.
[[nodiscard]] inline bool source_ends_in_line_comment(std::string_view text) noexcept {
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] == ';')
            return source_skip_literal(text, pos) >= text.size();
        const std::size_t next = source_skip_literal(text, pos);
        pos = next > pos ? next : pos + 1;
    }
    return false;
}

// True when the body already is a `(define ...)` form and is published as
// it is: `(`, `define`, then whitespace or `(`, then the name.
[[nodiscard]] inline bool source_is_define_form(std::string_view body) noexcept {
    std::size_t pos = 0;
    while (pos < body.size() && source_is_space(body[pos]))
        ++pos;
    if (pos >= body.size() || body[pos] != '(')
        return false;
    ++pos;
    while (pos < body.size() && source_is_space(body[pos]))
        ++pos;
    constexpr std::string_view kDefine = "define";
    if (body.size() - pos < kDefine.size() || body.compare(pos, kDefine.size(), kDefine) != 0)
        return false;
    pos += kDefine.size();
    if (pos >= body.size())
        return true;
    return source_is_space(body[pos]) || body[pos] == '(';
}

// `(define name body)`, or the body itself when it already is a define form.
// The closing paren goes on its own line when the body ends in a line
// comment: appended directly it would be part of the comment, and the
// published source would not read back.
[[nodiscard]] inline std::string canonical_define_source(std::string_view name,
                                                         std::string_view body) {
    if (source_is_define_form(body))
        return std::string(body);
    std::string out;
    out.reserve(body.size() + name.size() + 10);
    out += "(define ";
    out += name;
    out += ' ';
    out += body;
    if (source_ends_in_line_comment(body))
        out += '\n';
    out += ')';
    return out;
}

// Consumes `token` at `pos` when the text holds it there as a whole token.
[[nodiscard]] inline bool source_take_token(std::string_view text, std::size_t& pos,
                                            std::string_view token) noexcept {
    if (text.size() - pos < token.size() || text.compare(pos, token.size(), token) != 0)
        return false;
    const std::size_t end = pos + token.size();
    if (end < text.size() && !source_ends_token(text[end]))
        return false;
    pos = end;
    return true;
}

// True when the form that opens at `start` defines `name`: either
// `(define name ...)` or the shorthand `(define (name ...) ...)`.
[[nodiscard]] inline bool source_form_defines(std::string_view text, std::size_t start,
                                              std::string_view name) noexcept {
    std::size_t pos = start + 1;
    const auto skip_space = [&] {
        while (pos < text.size() && source_is_space(text[pos]))
            ++pos;
    };
    skip_space();
    if (!source_take_token(text, pos, "define"))
        return false;
    skip_space();
    if (pos < text.size() && text[pos] == '(') {
        ++pos;
        skip_space();
    }
    return source_take_token(text, pos, name);
}

// Every `(define name ...)` form in `text`, in text order: [start, end).
// Comments, strings, block comments and character literals are skipped, so
// a paren or a `define` inside one of them does not open or close a form.
[[nodiscard]] inline std::vector<std::pair<std::size_t, std::size_t>>
source_define_forms(std::string_view text, std::string_view name) {
    std::vector<std::pair<std::size_t, std::size_t>> forms;
    std::vector<std::pair<std::size_t, bool>> open; // start, defines `name`
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char character = text[pos];
        const std::size_t next = source_skip_literal(text, pos);
        if (next > pos) {
            pos = next;
            continue;
        }
        if (character == '(' || character == '[' || character == '{') {
            open.emplace_back(pos, character == '(' && source_form_defines(text, pos, name));
        } else if (character == ')' || character == ']' || character == '}') {
            if (!open.empty()) {
                const auto [form_start, wanted] = open.back();
                open.pop_back();
                if (wanted)
                    forms.emplace_back(form_start, pos + 1);
            }
        }
        ++pos;
    }
    return forms;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_DEFINE_SOURCE_HH
