// @category: integration
// @reason: Issue #1442 — EDSL (typed-mutate-atomic) for #1408 typed_mutate_atomic
//
// AC1: happy path — 3 mutations all applied
// AC2: mid-failure — 0 applied (atomic abort)
// AC3: empty mutations list — returns #f

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.core.ast;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

namespace {

bool is_true(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::string(expr));
    return r && is_bool(*r) && as_bool(*r);
}

bool is_false(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::string(expr));
    return r && is_bool(*r) && !as_bool(*r);
}

std::int64_t eval_int(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::string(expr));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

bool setup_xyz(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2) (define z 3)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

// ── AC1 ──────────────────────────────────────────────────────────
void ac1_happy_path() {
    std::println("\n--- AC1: typed-mutate-atomic happy path (3 applied) ---");
    CompilerService cs;
    CHECK(setup_xyz(cs), "setup x,y,z");
    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "workspace flat");
    const auto committed_before = flat->committed_mutation_count();

    // EDSL: list of mutation sexpr strings (same form as C++ typed_mutate).
    const char* expr = R"aura(
(typed-mutate-atomic
  (list "(mutate:rebind \"x\" \"10\")"
        "(mutate:rebind \"y\" \"20\")"
        "(mutate:rebind \"z\" \"30\")"))
)aura";
    CHECK(is_true(cs, expr), "typed-mutate-atomic returns #t on success");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after atomic");
    CHECK(eval_int(cs, "x") == 10, "x == 10 after happy path");
    CHECK(eval_int(cs, "y") == 20, "y == 20 after happy path");
    CHECK(eval_int(cs, "z") == 30, "z == 30 after happy path");
    const auto committed_after = flat->committed_mutation_count();
    CHECK(committed_after > committed_before,
          std::format("committed mutations grew ({} → {})", committed_before, committed_after));
}

// ── AC2 ──────────────────────────────────────────────────────────
void ac2_mid_failure_abort() {
    std::println("\n--- AC2: typed-mutate-atomic mid-failure rolls back ---");
    CompilerService cs;
    CHECK(setup_xyz(cs), "setup x,y,z");
    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "workspace flat");
    const auto committed_before = flat->committed_mutation_count();

    // Third sexpr has unbalanced parens → typed_mutate fails → atomic abort.
    const char* expr = R"aura(
(typed-mutate-atomic
  (list "(mutate:rebind \"x\" \"100\")"
        "(mutate:rebind \"y\" \"200\")"
        "(mutate:rebind \"z\"   "))
)aura";
    CHECK(is_false(cs, expr), "typed-mutate-atomic returns #f on abort");
    const auto committed_after = flat->committed_mutation_count();
    CHECK(committed_after == committed_before,
          std::format("0 new committed after abort ({} → {})", committed_before, committed_after));
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after abort");
    CHECK(eval_int(cs, "x") == 1, "x unchanged after abort");
    CHECK(eval_int(cs, "y") == 2, "y unchanged after abort");
    CHECK(eval_int(cs, "z") == 3, "z unchanged after abort");
}

// ── AC3 ──────────────────────────────────────────────────────────
void ac3_empty_list() {
    std::println("\n--- AC3: empty mutations list returns #f ---");
    CompilerService cs;
    CHECK(setup_xyz(cs), "setup x,y,z");
    CHECK(is_false(cs, "(typed-mutate-atomic (list))"), "empty list → #f");
    CHECK(is_false(cs, "(typed-mutate-atomic)"), "no args → #f");
}

} // namespace

int main() {
    std::println("=== Issue #1442: EDSL (typed-mutate-atomic) ===");
    ac1_happy_path();
    ac2_mid_failure_abort();
    ac3_empty_list();
    if (::aura::test::g_failed) {
        std::println(std::cerr, "FAIL ({} failed, {} passed)", ::aura::test::g_failed,
                     ::aura::test::g_passed);
        return 1;
    }
    std::println("OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
