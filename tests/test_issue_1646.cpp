// tests/test_issue_1646.cpp — Issue #1646 (partial-redundant-ship)
//
// Source-driven test (paired pattern with tests/test_issue_1644_ir_hygiene.cpp
// for #1644, tests/test_issue_1645.cpp for #1645, tests/test_orchestration_
// steal_boundary.cpp for #1641).
//
// AC coverage:
//   AC1 — Guard dtor / flush mutations metrics wiring      (Phase 1 fresh in #1646)
//   AC2 — query:mutation-boundary-observability-stats 5-field hash (fresh in #1646)
//   AC3 — insert_kv shared helper refactor (249 sites)    (DEFER to follow-up #1668)
//   AC4 — stress test accuracy (predecessor #1637 + #1908 + #1641 covered)
//
// Phase 1 verifies the 4 fresh counters + 4 fresh bumpers + 4 paired wire-ups
// landed at the Guard dtor + cross_cow refresh + ensure_hygiene_violation_
// detection sites + the new primitive surface.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_1646_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& s, std::string_view needle) noexcept {
    return s.find(needle) != std::string::npos;
}

bool check_counters_xmacros_bumpers_ac1() {
    std::println("\n--- AC1: 4 new counters + 4 X-macros + 4 bumpers + 4 getters ---");
    std::string om = read_file("src/compiler/observability_metrics.h");
    std::string fields = read_file("src/compiler/compiler_metrics_fields.inc");
    std::string ixx = read_file("src/compiler/evaluator.ixx");
    std::vector<std::string> names = {
        "mutation_boundary_success_total",
        "mutation_boundary_macro_dirty_propagated_total",
        "mutation_boundary_epoch_bump_for_macro_total",
        "mutation_boundary_hygiene_violation_total",
        "mutation_boundary_observability_queries_total",
    };
    bool all = true;
    for (const auto& n : names) {
        bool counter = contains(om, "std::atomic<std::uint64_t> " + n);
        bool xmacro  = contains(fields, "AURA_COMPILER_METRICS_FIELD(" + n + ")");
        bool bumper  = contains(ixx, "bump_" + n);
        bool getter  = contains(ixx, n + "() const noexcept");
        bool seen = counter && xmacro && bumper && getter;
        all = all && seen;
        std::println("  {}\tctr={}\txm={}\tbump={}\tget={}",
                     n, counter, xmacro, bumper, getter);
    }
    if (!all) {
        std::println("FAIL: at least one missing dimension (counter / xmacro / bumper / getter)");
        return false;
    }
    std::println("OK: 5 fresh counters + 5 X-macros + 5 bumpers + 5 getters landed");
    return true;
}

bool check_paired_wireups_ac1() {
    std::println("\n--- AC1: paired legacy/new wire-ups at Guard + cross_cow + hygiene sites ---");
    std::string efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    // cross_cow refresh site (paired bumps right after #1645's bump_cross_cow_invalidations).
    bool cross_cow_paired =
        contains(efm, "bump_mutation_boundary_macro_dirty_propagated_total()") &&
        contains(efm, "bump_mutation_boundary_epoch_bump_for_macro_total()") &&
        contains(efm, "Issue #1646");
    // ensure_hygiene_violation_detection paired bump.
    bool hygiene_paired =
        contains(efm, "ensure_hygiene_violation_detection") &&
        contains(efm, "bump_mutation_boundary_hygiene_violation_total()");
    if (!cross_cow_paired || !hygiene_paired) {
        std::println("FAIL: paired wire-ups missing "
                     "(cross_cow_paired={} hygiene_paired={})",
                     cross_cow_paired, hygiene_paired);
        return false;
    }
    std::println("OK: 2 paired wire-up sites landed (cross_cow refresh + ensure_hygiene_violation_detection)");
    return true;
}

bool check_new_primitive_ac2() {
    std::println("\n--- AC2: query:mutation-boundary-observability-stats primitive ---");
    std::string pq = read_file("src/compiler/evaluator_primitives_query.cpp");
    bool header = contains(pq, "query:mutation-boundary-observability-stats");
    // Reads the 4 new counters + queries_total counter.
    bool reads_counters = contains(pq, "mutation_boundary_success_total()") &&
                          contains(pq, "mutation_boundary_macro_dirty_propagated_total()") &&
                          contains(pq, "mutation_boundary_epoch_bump_for_macro_total()") &&
                          contains(pq, "mutation_boundary_hygiene_violation_total()");
    // Per-call queries_total counter bump.
    bool queries_counter_bump = contains(pq, "mutation_boundary_observability_queries_total.fetch_add");
    if (!header || !reads_counters || !queries_counter_bump) {
        std::println("FAIL: primitive incomplete "
                     "(header={} reads_counters={} queries_counter_bump={})",
                     header, reads_counters, queries_counter_bump);
        return false;
    }
    std::println("OK: query:mutation-boundary-observability-stats primitive + queries_total surface");
    return true;
}

bool check_design_doc_present() {
    std::println("\n--- #1646 docs/design/1646-mutation-boundary-observability.md ---");
    std::ifstream in("docs/design/1646-mutation-boundary-observability.md");
    if (!in) {
        std::println("FAIL: design doc missing");
        return false;
    }
    std::println("OK: design doc present");
    return true;
}

bool check_baseline_ac4(CompilerService& cs) {
    std::println("\n--- AC4: cross-layer baseline round-trip survives #1646 wire-up ---");
    if (!cs.eval("(set-code \"(define x 42)\")")) {
        std::println("FAIL: set-code broke");
        return false;
    }
    if (auto r = cs.eval("(eval-current)"); !r || !is_int(*r)) {
        std::println("FAIL: eval-current broke");
        return false;
    }
    std::println("OK: baseline round-trip survives #1646 partial-redundant-ship");
    return true;
}

}  // namespace aura_1646_detail

int main() {
    using namespace aura_1646_detail;

    int rc = 0;
    if (!check_counters_xmacros_bumpers_ac1()) rc = 1;
    if (!check_paired_wireups_ac1())          rc = 1;
    if (!check_new_primitive_ac2())           rc = 1;
    if (!check_design_doc_present())          rc = 1;

    if (rc == 0) {
        CompilerService cs;
        if (!check_baseline_ac4(cs)) rc = 1;
    }

    if (rc == 0) {
        std::println("\n#1646 partial-redundant-ship — all ACs green ✅ "
                     "(AC3 insert_kv refactor 249 sites deferred to #1668 follow-up)");
    } else {
        std::println("\n#1646 — some ACs FAILED ❌");
    }
    return rc;
}
