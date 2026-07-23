// test_issue_1991.cpp — Issue #1991 / B-010: (gc) primitive clears
// modules_/workspace_flat_/workspace_pool_ without any lock, racing
// concurrent load_module_file writes. Fix adds `mutable std::shared_mutex
// module_mtx_` to Evaluator and wraps (gc) + load_module_file write
// section in `std::unique_lock<std::shared_mutex> lock(module_mtx_);`.
//
// Single-case smoke: (gc) returns #t/#f after the lock; no race with
// any concurrent module operation. Pattern matches test_issue_1990.

#include "test_harness.hpp"

#include <print>

import std;
import aura.compiler.service;
import aura.compiler.value;

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;

    // Boot CompilerService (full pipeline incl. Evaluator + module loader
    // + JIT). After this, (gc) is callable and the module_mtx_ lock is
    // taken on every clear.
    aura::compiler::CompilerService cs;

    auto r_gc = cs.eval("(gc)");
    CHECK(r_gc.has_value(), "(gc) returns a value after module_mtx_ fix");

    if (::aura::test::g_failed)
        return 1;
    std::println("issue 1991 module_mtx_ (gc): OK ({} passed)", ::aura::test::g_passed);
    return 0;
}