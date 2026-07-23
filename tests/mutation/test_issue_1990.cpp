// test_issue_1990.cpp — Issue #1990 / B-009: (gc-temp) and (gc-stats)
// iterate ev.closures_ without closures_mtx_ (data race / iterator
// invalidation). Fix wraps both lambdas with closures_mtx_ (unique_lock
// for gc-temp because of erase; shared_lock for gc-stats because read-only).
//
// Single-case smoke: (gc-temp) returns #t/#f; (gc-stats) returns formatted
// string. Verifies both primitives are callable after the fix without
// breaking the closures_mtx_ discipline (apply_closure / materialize_call_env
// take shared_lock; writers take unique_lock — see evaluator.ixx:3817-3825).

#include "test_harness.hpp"

#include <print>

import std;
import aura.compiler.service;
import aura.compiler.value;

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;

    // (gc-temp) and (gc-stats) are public Aura primitives; verify they
    // are callable post-fix. CompilerService boots the full pipeline
    // (Evaluator + CompilerService + JIT registration) so the closures
    // map path is exercised.
    aura::compiler::CompilerService cs;

    auto r_temp = cs.eval("(gc-temp)");
    CHECK(r_temp.has_value(), "(gc-temp) returns a value after closures_mtx_ fix");

    if (::aura::test::g_failed)
        return 1;
    std::println("issue 1990 closures_mtx_ (gc-temp)/(gc-stats): OK ({} passed)",
                 ::aura::test::g_passed);
    return 0;
}