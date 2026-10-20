#pragma once
// Structural source-cite windows (Issue: anchor-based windows, no magic byte
// widths).
//
// Source-cite checks read a production file and assert on a marker that lives
// inside a specific function or switch-case body. Slicing with a fixed byte
// width silently loses the marker when the body grows by a few lines. These
// helpers slice by structure instead:
//
//   slice_between(text, begin, end)  -> [begin, end)
//   brace_body(text, sig)            -> sig's opening brace .. matching close
//   case_body(text, label)           -> case label .. next sibling case/default
//
// Every helper returns {view, ok}. ok == false when the anchors are missing or
// unmatched -- callers must CHECK(slice.ok) and must never fall back to a byte
// width or to the whole tail of the file.
//
// The primitives are constexpr and self-tested with static_assert below: the
// same proven code path runs at compile time on literals and at run time on the
// file contents.

#include <cstddef>
#include <string_view>

namespace aura::test::source_contract {

struct Slice {
    std::string_view view{};
    bool ok{false};
};

namespace detail {

    constexpr bool ident(char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    }

    // True when i starts a token (not in the middle of an identifier).
    constexpr bool token_start(std::string_view text, std::size_t i) noexcept {
        return i == 0 || !ident(text[i - 1]);
    }

    // True when everything between the previous newline and i is indentation.
    constexpr bool line_indent_only(std::string_view text, std::size_t i) noexcept {
        std::size_t p = i;
        while (p > 0) {
            const char c = text[p - 1];
            if (c == '\n')
                break;
            if (c != ' ' && c != '\t' && c != '\r')
                return false;
            --p;
        }
        return true;
    }

    // If a literal or comment starts at i, return the index just past it.
    // Otherwise return i unchanged. Comments are checked before strings because
    // their introducers are unambiguous; the caller scans left to right, so a
    // comment introducer inside a string is only reached after the string has
    // already been skipped.
    constexpr std::size_t skip_literal(std::string_view text, std::size_t i) noexcept {
        if (i >= text.size())
            return i;
        if (i + 1 < text.size()) {
            if (text[i] == '/' && text[i + 1] == '/') {
                const auto e = text.find('\n', i + 2);
                return e == std::string_view::npos ? text.size() : e + 1;
            }
            if (text[i] == '/' && text[i + 1] == '*') {
                const auto e = text.find("*/", i + 2);
                return e == std::string_view::npos ? text.size() : e + 2;
            }
            if (text[i] == 'R' && text[i + 1] == '"') {
                const auto open = text.find('(', i + 2);
                if (open == std::string_view::npos)
                    return text.size();
                const std::string_view delim = text.substr(i + 2, open - (i + 2));
                // Find ")" + delim + '"'.
                std::size_t p = open + 1;
                while (p < text.size()) {
                    const auto close = text.find(')', p);
                    if (close == std::string_view::npos)
                        return text.size();
                    if (close + 1 + delim.size() < text.size() &&
                        text.substr(close + 1, delim.size()) == delim &&
                        text[close + 1 + delim.size()] == '"') {
                        return close + 2 + delim.size();
                    }
                    p = close + 1;
                }
                return text.size();
            }
        }
        if (text[i] == '"' || text[i] == '\'') {
            // A single quote preceded by an identifier/digit is a digit separator
            // (1'000), not a character literal.
            if (text[i] == '\'' && i > 0 && ident(text[i - 1]))
                return i;
            const char quote = text[i];
            std::size_t p = i + 1;
            while (p < text.size()) {
                if (text[p] == '\\') {
                    p += 2;
                    continue;
                }
                if (text[p] == quote)
                    return p + 1;
                ++p;
            }
            return text.size();
        }
        return i;
    }

    // Brace depth just before position pos, ignoring literals and comments.
    constexpr int depth_at(std::string_view text, std::size_t pos) noexcept {
        int depth = 0;
        std::size_t i = 0;
        while (i < pos) {
            const auto skip = skip_literal(text, i);
            if (skip != i) {
                i = skip;
                continue;
            }
            if (text[i] == '{') {
                ++depth;
            } else if (text[i] == '}') {
                --depth;
            }
            ++i;
        }
        return depth;
    }

} // namespace detail

// Slice from the begin anchor (inclusive) to the end anchor (exclusive).
// Missing or out-of-order anchors yield {empty, false}: never the whole tail.
constexpr Slice slice_between(std::string_view text, std::string_view begin,
                              std::string_view end) noexcept {
    const auto b = text.find(begin);
    if (b == std::string_view::npos)
        return {};
    const auto e = text.find(end, b + begin.size());
    if (e == std::string_view::npos || e < b)
        return {};
    return {text.substr(b, e - b), true};
}

// From the signature anchor to the matching close brace of its body,
// inclusive. Fails loudly when the signature is absent, the declaration has no
// body (next ';' before '{'), or braces are unbalanced.
constexpr Slice brace_body(std::string_view text, std::string_view sig) noexcept {
    const auto p = text.find(sig);
    if (p == std::string_view::npos)
        return {};
    std::size_t i = p + sig.size();
    // A signature ending in '{' already names the opening brace.
    std::size_t tail = i;
    while (tail > p && (text[tail - 1] == ' ' || text[tail - 1] == '\t'))
        --tail;
    if (tail > p && text[tail - 1] == '{') {
        i = tail - 1;
    } else {
        while (i < text.size()) {
            const auto skip = detail::skip_literal(text, i);
            if (skip != i) {
                i = skip;
                continue;
            }
            if (text[i] == ';')
                return {};
            if (text[i] == '{')
                break;
            ++i;
        }
    }
    if (i >= text.size())
        return {};
    const std::size_t open = i;
    int depth = 0;
    while (i < text.size()) {
        const auto skip = detail::skip_literal(text, i);
        if (skip != i) {
            i = skip;
            continue;
        }
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0)
                return {text.substr(p, i - p + 1), true};
        }
        ++i;
    }
    (void)open;
    return {};
}

// From a case/default label to the next sibling label at the same brace depth,
// or to the switch's closing brace. The slice excludes the next sibling label.
constexpr Slice case_body(std::string_view text, std::string_view label) noexcept {
    std::size_t p = text.find(label);
    while (p != std::string_view::npos) {
        if (detail::line_indent_only(text, p) && detail::token_start(text, p))
            break;
        p = text.find(label, p + 1);
    }
    if (p == std::string_view::npos)
        return {};
    const int base = detail::depth_at(text, p);
    std::size_t i = p + label.size();
    int depth = base;
    std::size_t group_tail = p + label.size();
    while (i < text.size()) {
        const auto skip = detail::skip_literal(text, i);
        if (skip != i) {
            i = skip;
            continue;
        }
        const char c = text[i];
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (depth == base)
                return {text.substr(p, i - p), true};
            --depth;
        } else if (depth == base && detail::token_start(text, i)) {
            if (text.compare(i, 5, "case ") == 0 || text.compare(i, 4, "case") == 0 ||
                text.compare(i, 7, "default") == 0) {
                // Only a sibling label: "case"/"default" followed by a
                // non-identifier character (space, '(', ':').
                const std::size_t after = i + (text.compare(i, 7, "default") == 0 ? 7 : 4);
                if (after >= text.size() || !detail::ident(text[after])) {
                    // Grouped labels (case A: case B: body) share one body.
                    // A label with only whitespace after it is part of the
                    // same group, so keep scanning from the next label.
                    bool only_space = true;
                    for (std::size_t k = group_tail; k < i; ++k) {
                        const char s = text[k];
                        if (s != ' ' && s != '\t' && s != '\r' && s != '\n') {
                            only_space = false;
                            break;
                        }
                    }
                    if (only_space) {
                        const auto colon = text.find(':', i);
                        const auto nl = text.find('\n', i);
                        if (colon != std::string_view::npos &&
                            (nl == std::string_view::npos || colon < nl)) {
                            group_tail = colon + 1;
                            i = group_tail;
                        } else {
                            i = after;
                        }
                        continue;
                    }
                    return {text.substr(p, i - p), true};
                }
            }
        }
        ++i;
    }
    return {};
}

// ── Compile-time self-tests: the slicer is proven before any file is read ──
namespace selftest {

    inline constexpr std::string_view kFunctions = R"cpp(
void f() {
    int x = 1;
    if (x) { x = 2; }
}
void g() { int y = 3; }
struct S { void h() { } };
)cpp";

    inline constexpr std::string_view kSwitch = R"cpp(
switch (op) {
case OpApply: {
    apply();
    break;
}
case OpDrop: {
    drop();
    break;
}
default:
    other();
    break;
}
)cpp";

    static_assert(slice_between(kFunctions, "void f()", "void g()").ok);
    static_assert(slice_between(kFunctions, "void f()", "void g()").view.find("x = 2") !=
                  std::string_view::npos);
    static_assert(slice_between(kFunctions, "void f()", "void g()").view.find("int y") ==
                  std::string_view::npos);
    static_assert(!slice_between(kFunctions, "void missing()", "void g()").ok);
    static_assert(!slice_between(kFunctions, "void f()", "void missing()").ok);

    static_assert(brace_body(kFunctions, "void f()").ok);
    static_assert(brace_body(kFunctions, "void f()").view.find("x = 2") != std::string_view::npos);
    static_assert(brace_body(kFunctions, "void f()").view.find("void g") == std::string_view::npos);
    static_assert(brace_body(kFunctions, "void g()").view.find("int y = 3") !=
                  std::string_view::npos);
    static_assert(brace_body(kFunctions, "struct S").ok);
    static_assert(brace_body(kFunctions, "void f() {").ok);
    static_assert(brace_body(kFunctions, "void f() {").view.find("x = 2") !=
                  std::string_view::npos);
    static_assert(!brace_body(kFunctions, "void missing()").ok);

    static_assert(case_body(kSwitch, "case OpApply:").ok);
    static_assert(case_body(kSwitch, "case OpApply:").view.find("apply()") !=
                  std::string_view::npos);
    static_assert(case_body(kSwitch, "case OpApply:").view.find("drop()") ==
                  std::string_view::npos);
    static_assert(case_body(kSwitch, "case OpDrop:").view.find("drop()") != std::string_view::npos);
    static_assert(case_body(kSwitch, "case OpDrop:").view.find("other()") ==
                  std::string_view::npos);
    static_assert(case_body(kSwitch, "default:").ok);
    static_assert(case_body(kSwitch, "default:").view.find("other()") != std::string_view::npos);
    static_assert(!case_body(kSwitch, "case Missing:").ok);

    // Grouped labels share the following body.
    inline constexpr std::string_view kGrouped = R"cpp(
switch (op) {
case A:
case B: {
    shared();
    break;
}
default:
    other();
}
)cpp";
    static_assert(case_body(kGrouped, "case A:").ok);
    static_assert(case_body(kGrouped, "case A:").view.find("shared()") != std::string_view::npos);
    static_assert(case_body(kGrouped, "case A:").view.find("other()") == std::string_view::npos);
    static_assert(case_body(kGrouped, "case B:").view.find("shared()") != std::string_view::npos);

    // Braces inside literals/comments must not move the depth.
    inline constexpr std::string_view kLiterals = R"cpp(
void lit() {
    const char* s = "} not a brace";
    // } comment brace
    int x = 1;
}
)cpp";
    static_assert(brace_body(kLiterals, "void lit()").ok);
    static_assert(brace_body(kLiterals, "void lit()").view.find("int x = 1") !=
                  std::string_view::npos);

} // namespace selftest

} // namespace aura::test::source_contract
