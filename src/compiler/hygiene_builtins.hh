// hygiene_builtins.hh — Issue #4936 B4 / E1.
// The macro hygiene reserved-name table used to be a function-local
// unordered_set built on first call. The names are a closed compile-time
// list, so lookup is a sorted constexpr array.
#ifndef AURA_COMPILER_HYGIENE_BUILTINS_HH
#define AURA_COMPILER_HYGIENE_BUILTINS_HH

#include <algorithm>
#include <array>
#include <string_view>

namespace aura::compiler::macro_exp::detail {

inline constexpr std::array<std::string_view, 108> kHygieneBuiltinNames = {
    "if",
    "cond",
    "let",
    "let*",
    "letrec",
    "lambda",
    "define",
    "begin",
    "set!",
    "quote",
    "unquote",
    "quasiquote",
    "case",
    "when",
    "unless",
    "do",
    "delay",
    "force",
    "import",
    "export",
    "module",
    "car",
    "cdr",
    "cons",
    "list",
    "list-sort",
    "pair?",
    "null?",
    "list?",
    "length",
    "append",
    "reverse",
    "member",
    "member?",
    "memv",
    "assoc",
    "assv",
    "eq?",
    "equal?",
    "eqv?",
    "+",
    "-",
    "*",
    "/",
    "quotient",
    "remainder",
    "modulo",
    "=",
    "<",
    ">",
    "<=",
    ">=",
    "not",
    "and",
    "or",
    "void",
    "max",
    "min",
    "abs",
    "display",
    "write",
    "newline",
    "read",
    "write-file",
    "read-file",
    "number?",
    "integer?",
    "float?",
    "boolean?",
    "string?",
    "symbol?",
    "char?",
    "vector?",
    "hash?",
    "procedure?",
    "error?",
    "string-append",
    "string-length",
    "string-ref",
    "substring",
    "number->string",
    "string->number",
    "string=?",
    "symbol->string",
    "string->symbol",
    "apply",
    "map",
    "filter",
    "foldl",
    "foldr",
    "for-each",
    "vector",
    "make-vector",
    "vector-ref",
    "vector-set!",
    "vector-length",
    "make-hash",
    "hash-ref",
    "hash-set!",
    "hash-count",
    "error",
    "assert",
    "gensym",
    "raise",
    "eval",
    "current-time",
    "current-time-ms",
    "monotonic-ms",
};

consteval std::array<std::string_view, kHygieneBuiltinNames.size()> hygiene_builtins_sorted() {
    auto names = kHygieneBuiltinNames;
    std::ranges::sort(names);
    return names;
}

inline constexpr auto kHygieneBuiltins = hygiene_builtins_sorted();

[[nodiscard]] inline bool hygiene_builtins_contains(std::string_view name) noexcept {
    return std::ranges::binary_search(kHygieneBuiltins, name);
}

} // namespace aura::compiler::macro_exp::detail

#endif
