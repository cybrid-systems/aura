// @category: integration
// @reason: Issue #478 — Unified primitive error handling framework.
//          Validates:
//            - make_primitive_error path increments primitive_error_count_
//            - query:primitive-error-stats returns (count . stored)
//            - hotspot primitives (modulo, list-ref, car) return errors
//            - happy-path arithmetic unchanged


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace aura_issue_478_detail {

bool test_primitive_error_stats_zero_on_fresh() {
    std::println("\n--- AC1: primitive error stats start at 0 ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(ev.get_primitive_error_count() == 0, "primitive_error_count == 0 on fresh service");
    CHECK(ev.get_primitive_error_values_size() == 0, "error_values_.size() == 0 on fresh service");
    return true;
}

bool test_query_primitive_error_stats() {
    std::println("\n--- AC2: query:primitive-error-stats returns a pair ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:primitive-error-stats)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_pair(*r), "query:primitive-error-stats returns a pair");
    return true;
}

bool test_modulo_division_by_zero_increments_counter() {
    std::println("\n--- AC3: modulo division-by-zero increments counter ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    auto before = ev.get_primitive_error_count();
    auto r = cs.eval("(modulo 1 0)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_error(*r), "modulo 1 0 returns an error value");
    auto after = ev.get_primitive_error_count();
    CHECK(after == before + 1, "primitive_error_count bumped: " + std::to_string(before) + " -> " +
                                   std::to_string(after));
    return true;
}

bool test_list_ref_error_increments_counter() {
    std::println("\n--- AC4: list-ref error increments counter ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    auto before = ev.get_primitive_error_count();
    auto r = cs.eval("(list-ref '(1 2) 99)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_error(*r), "list-ref out of bounds returns an error value");
    auto after = ev.get_primitive_error_count();
    CHECK(after == before + 1, "list-ref error bumped counter: " + std::to_string(before) + " -> " +
                                   std::to_string(after));
    return true;
}

bool test_car_error_increments_counter() {
    std::println("\n--- AC5: car on non-pair increments counter ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    auto before = ev.get_primitive_error_count();
    auto r = cs.eval("(car 42)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_error(*r), "car 42 returns an error value");
    auto after = ev.get_primitive_error_count();
    CHECK(after == before + 1,
          "car error bumped counter: " + std::to_string(before) + " -> " + std::to_string(after));
    return true;
}

bool test_happy_path_unchanged() {
    std::println("\n--- AC6: happy-path modulo unchanged ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(modulo 7 3)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 1,
          "modulo 7 3 == 1");
    return true;
}

} // namespace aura_issue_478_detail

int main() {
    using namespace aura_issue_478_detail;
    std::println("=== Issue #478: Unified Primitive Error Handling ===");

    test_primitive_error_stats_zero_on_fresh();
    test_query_primitive_error_stats();
    test_modulo_division_by_zero_increments_counter();
    test_list_ref_error_increments_counter();
    test_car_error_increments_counter();
    test_happy_path_unchanged();

    std::println("\n=== Results: {}/{} passed ===", g_passed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}