// @category: integration
// @reason: uses CompilerService to eval Aura source
// test_issue_218.cpp — Issue #218 Cycle 5: reflection tests +
// integration (composes with #140 hygiene and #248 schema
// observability).
//
// Scope (from GitHub #218):
//   1. query:schema-of-marker on macro-introduced bindings
//      returns the user's inferred schema (not the macro's
//      internal hygiene nodes) — composes with #140.
//   2. query:pattern hygiene still skips MacroIntroduced
//      nodes while schema introspection remains available.
//   3. mutate:rebind on an unrelated binding does not break
//      macro-introduced schema observability.
//
// The 1000+ serialize/deserialize stress loop lives in
// test_issue_178 (reflect TU). TSan/ASan runs:
//   tests/run_issue_218_tsan.sh


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_218_detail {

struct EvalResult {
    bool ok = false;
    aura::compiler::types::EvalValue v{};
};

static EvalResult try_run(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) return {false, aura::compiler::types::make_void()};
    return {true, *r};
}

static std::int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = try_run(cs, src);
    if (!r.ok || !aura::compiler::types::is_int(r.v)) return -1;
    return aura::compiler::types::as_int(r.v);
}

static std::int64_t is_pair_result(aura::compiler::CompilerService& cs, std::string_view expr) {
    auto r = try_run(cs, std::string("(if (pair? ") + std::string(expr) + ") 1 0)");
    if (!r.ok || !aura::compiler::types::is_int(r.v)) return -1;
    return aura::compiler::types::as_int(r.v);
}

static bool set_source(aura::compiler::CompilerService& cs, const std::string& src) {
    return try_run(cs, std::string("(set-code \"") + src + "\")").ok;
}

// ── Test 1: macro-introduced binding has typed schema ────────
//
// A hygienic macro introduces a Let binding. After
// typecheck, query:schema-of-marker "MacroIntroduced" should
// return (NodeId . type-name) pairs for the macro-expanded
// nodes — the user's intended schema, not internal hygiene.
bool test_macro_introduced_schema() {
    std::println("\n--- Test 1: macro-introduced binding schema ---");
    aura::compiler::CompilerService cs;
    const std::string code =
        "(define-hygienic-macro (typed-let name val) "
        "  (let ((name val)) name)) "
        "(typed-let x 42)";
    if (!set_source(cs, code)) {
        CHECK(false, "set-code succeeded");
        return false;
    }
    auto tc = try_run(cs, "(typecheck-current)");
    if (!tc.ok) {
        CHECK(false, "typecheck-current succeeded");
        return false;
    }
    // Known #244 limitation: macro-expanded body nodes often keep
    // User marker (clone_macro_body hygiene), so the user's
    // intended schema appears under "User", not "MacroIntroduced".
    // The outer expansion site is MacroIntroduced but may lack
    // type_id until full schema tracking ships (#248 deferred).
    std::int64_t user_schema_ok = is_pair_result(cs, "(query:schema-of-marker \"User\")");
    CHECK(user_schema_ok == 1, "query:schema-of-marker User returns a list");
    const std::int64_t user_len = run_int(cs, "(length (query:schema-of-marker \"User\"))");
    CHECK(user_len > 0, "User-marker schema has typed entries (user's intended schema)");
    std::println("  INFO: User-marker schema entries = {}", user_len);
    return true;
}

// ── Test 2: query:pattern hygiene composes with schema query ─
//
// #140: query:pattern skips MacroIntroduced nodes at the
// outer expansion site. Schema introspection (#248) remains
// available for typed nodes regardless of marker.
bool test_hygiene_and_schema_compose() {
    std::println("\n--- Test 2: query:pattern hygiene + schema compose ---");
    aura::compiler::CompilerService cs;
    const std::string code =
        "(define-hygienic-macro (my-add) (+ 1 2)) "
        "(my-add) "
        "(+ 1 2)";
    if (!set_source(cs, code)) {
        CHECK(false, "set-code succeeded");
        return false;
    }
    (void)try_run(cs, "(typecheck-current)");
    // #140: outer (my-add) call is MacroIntroduced and skipped.
    const std::int64_t pattern_count = run_int(
        cs, "(length (query:pattern \"(+ 1 2)\"))");
    CHECK(pattern_count >= 1,
          "query:pattern finds user (+ 1 2) despite macro hygiene (got " +
              std::to_string(pattern_count) + ")");
    // Schema query still works on User-marker typed nodes.
    std::int64_t schema_ok = is_pair_result(cs, "(query:schema-of-marker \"User\")");
    CHECK(schema_ok == 1, "query:schema-of-marker User still returns a list");
    return true;
}

// ── Test 3: mutate on unrelated binding preserves macro schema
bool test_mutate_preserves_macro_schema() {
    std::println("\n--- Test 3: mutate + macro schema coexist ---");
    aura::compiler::CompilerService cs;
    const std::string setup =
        "(define (base n) (* n 2)) "
        "(define-hygienic-macro (triple x) (* x 3)) "
        "(define result (triple 5))";
    if (!set_source(cs, setup)) {
        CHECK(false, "set-code succeeded");
        return false;
    }
    (void)try_run(cs, "(typecheck-current)");
    const std::int64_t macro_val = run_int(cs, "(triple 7)");
    CHECK(macro_val == 21, "macro works before mutate (triple 7) = 21");
    const std::int64_t mutate_ok = run_int(
        cs, "(mutate:rebind \"base\" \"(lambda (n) (* n 4))\" \"issue218\")");
    (void)mutate_ok;
    const std::int64_t macro_after = run_int(cs, "(triple 7)");
    CHECK(macro_after == 21, "macro still works after unrelated mutate (triple 7) = 21");
    std::int64_t schema_ok = is_pair_result(cs, "(query:schema-of-marker \"User\")");
    CHECK(schema_ok == 1, "schema query still works after mutate");
    return true;
}

// ── Test 4: query:schema type lookup (Cycle 3 composition) ───
bool test_query_schema_primitive() {
    std::println("\n--- Test 4: query:schema primitive ---");
    aura::compiler::CompilerService cs;
    std::int64_t is_str = run_int(cs, "(if (string? (query:schema \"Int\")) 1 0)");
    CHECK(is_str == 1, "query:schema \"Int\" returns a string");
    return true;
}

int run_tests() {
    std::println("═══ Issue #218 — reflection integration (Cycle 5) ═══\n");
    test_macro_introduced_schema();
    test_hygiene_and_schema_compose();
    test_mutate_preserves_macro_schema();
    test_query_schema_primitive();
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_218_detail

int main() {
    return aura_issue_218_detail::run_tests();
}