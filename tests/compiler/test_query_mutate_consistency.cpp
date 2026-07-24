// test_query_mutate_consistency.cpp — Issue #1374:
// query:pattern and mutate:replace-pattern share default Kleene mode.

#include "test_harness.hpp"

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

namespace {

bool set_source(CompilerService& cs, std::string_view src) {
    std::string cmd = "(set-code \"";
    for (char c : src) {
        if (c == '\\' || c == '"')
            cmd += '\\';
        cmd += c;
    }
    cmd += "\")";
    auto r = cs.eval(cmd);
    return r.has_value();
}

std::int64_t run_int(CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

bool run_bool(CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    return r && is_bool(*r) && as_bool(*r);
}

} // namespace

int main() {
    // ── default Kleene: query and mutate agree on (+ ...) ──
    {
        CompilerService cs;
        CHECK(set_source(cs, "(begin (+ 1) (+ 1 2) (+ 1 2 3) (- 9 8))"), "set-code");
        // Default query:pattern is Kleene — (+ ...) matches 2-, 3-, 4-child +
        auto q = run_int(cs, "(length (query:pattern \"(+ ...)\"))");
        CHECK(q >= 2, "default Kleene query '(+ ...)' finds multiple + calls");

        // Default mutate:replace-pattern is also Kleene (#1374)
        CHECK(run_bool(cs, "(mutate:replace-pattern \"(+ 1 2)\" \"(* 1 2)\")"),
              "default mutate exact replace");
        auto after_plus = run_int(cs, "(length (query:pattern \"(+ 1 2)\"))");
        auto after_star = run_int(cs, "(length (query:pattern \"(* 1 2)\"))");
        CHECK(after_plus == 0, "exact (+ 1 2) gone after mutate");
        CHECK(after_star == 1, "(* 1 2) present after mutate");
    }

    // ── same pattern string → same match count (default mode) ──
    {
        CompilerService cs;
        CHECK(set_source(cs, "(begin (foo a b) (foo x y) (bar p q) (foo))"), "set-code foo");
        auto q_def = run_int(cs, "(length (query:pattern \"(foo ... ...)\"))");
        auto q_strict = run_int(cs, "(length (query:pattern \"(foo ... ...)\" :strict-arity #t))");
        CHECK(q_def >= q_strict, "Kleene ≥ strict for same pattern");
        CHECK(q_strict == 2, "strict (foo ... ...) matches two 3-child foo");

        // Mutate with default (Kleene) — replace 3-child foos
        CHECK(run_bool(cs, "(mutate:replace-pattern \"(foo ... ...)\" \"(baz ... ...)\" "
                           ":strict-arity #t)"),
              "mutate :strict-arity #t");
        auto foo_left = run_int(cs, "(length (query:pattern \"(foo ... ...)\" :strict-arity #t))");
        auto baz_n = run_int(cs, "(length (query:pattern \"(baz ... ...)\" :strict-arity #t))");
        CHECK(foo_left == 0, "no strict (foo ... ...) left");
        CHECK(baz_n == 2, "two (baz ... ...) after strict mutate");
    }

    // ── :strict-arity on mutate restores pre-#1374 behavior ──
    {
        CompilerService cs;
        CHECK(set_source(cs, "(begin (+ 1) (+ 1 2) (+ 1 2 3))"), "set-code +");
        // Strict: (+ ...) is Call(+, one-wildcard) → only 2-child (+ 1)
        auto q = run_int(cs, "(length (query:pattern \"(+ ...)\" :strict-arity #t))");
        CHECK(q == 1, "strict query (+ ...) = 1");
        CHECK(run_bool(cs, "(mutate:replace-pattern \"(+ ...)\" \"(- ...)\" :strict-arity #t)"),
              "strict mutate");
        auto plus_left = run_int(cs, "(length (query:pattern \"(+ ...)\" :strict-arity #t))");
        auto minus_n = run_int(cs, "(length (query:pattern \"(- ...)\" :strict-arity #t))");
        CHECK(plus_left == 0, "strict (+ ...) cleared");
        CHECK(minus_n == 1, "one (- ...) from strict replace");
    }

    // ── :nested-arity #f equivalent to :strict-arity #t ──
    {
        CompilerService cs;
        CHECK(set_source(cs, "(begin (* 1) (* 1 2))"), "set-code *");
        CHECK(run_bool(cs, "(mutate:replace-pattern \"(* ...)\" \"(+ ...)\" :nested-arity #f)"),
              "mutate :nested-arity #f");
        auto star = run_int(cs, "(length (query:pattern \"(* ...)\" :strict-arity #t))");
        auto plus = run_int(cs, "(length (query:pattern \"(+ ...)\" :strict-arity #t))");
        CHECK(star == 0, "no strict (* ...) after nested-arity #f replace");
        CHECK(plus == 1, "one strict (+ ...) after replace of 2-child *");
    }

    // ── hygiene keywords accepted (no crash; default excludes MacroIntroduced) ──
    {
        CompilerService cs;
        CHECK(set_source(cs, "(begin (+ 1 2) (+ 3 4))"), "set-code hygiene");
        CHECK(run_bool(cs, "(mutate:replace-pattern \"(+ 1 2)\" \"(+ 9 9)\" "
                           ":exclude-macro-introduced #t)"),
              "exclude-macro-introduced accepted");
        CHECK(run_bool(cs, "(mutate:replace-pattern \"(+ 3 4)\" \"(+ 8 8)\" "
                           ":include-macro-introduced #f)"),
              "include-macro-introduced #f accepted");
        auto q = run_int(cs, "(length (query:pattern \"(+ 9 9)\"))");
        CHECK(q == 1, "replacement visible");
    }

    // ── default-mode query count == mutate would-replace set (exact literals) ──
    {
        CompilerService cs;
        CHECK(set_source(cs, "(begin 1 2 3 2)"), "set-code lits");
        auto n2 = run_int(cs, "(length (query:pattern \"2\"))");
        CHECK(n2 == 2, "two literal 2 nodes");
        CHECK(run_bool(cs, "(mutate:replace-pattern \"2\" \"99\")"), "replace both 2s");
        auto n2_after = run_int(cs, "(length (query:pattern \"2\"))");
        auto n99 = run_int(cs, "(length (query:pattern \"99\"))");
        CHECK(n2_after == 0, "no 2 left");
        CHECK(n99 == 2, "two 99s (same count as query found)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("query mutate consistency #1374: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
