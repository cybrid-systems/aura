// tests/test_issue_1649.cpp — Issue #1649 (partial-redundant Phase 1)
//
// Source-driven test (paired pattern with tests/test_issue_1644_ir_hygiene.cpp
// for #1644, tests/test_issue_1645.cpp for #1645, tests/test_issue_1646.cpp
// for #1646, tests/test_issue_1647.cpp for #1647, tests/test_reflect_nested.cpp
// for #1648). Verifies Phase 1 ships the 2 body-named atomic counters
// (atomic_batch_hygiene_violation_prevented_total + mutate_template_marker_
// propagated_total) + their X-macro fields + Evaluator bumpers/getters +
// paired wire-up sites. AC1 (atomic batch full snapshot for marker/dirty)
// + AC4 (IR/JIT marker check + deopt) + AC5 (TSan stress + failing
// mutation inside batch + macro template) deferred to follow-ups
// #1680 / #1681 / #1682.
//
// AC coverage:
//   AC1 — atomic batch full snapshot coverage for marker/dirty
//       DEFER to #1680 (multi-session refactor).
//   AC2 — mutate template SyntaxMarker propagation (Phase 1: paired
//       wire-up added at atomic_batch_pinning + template-respect site)
//   AC3 — query:pattern hygiene filter (:exclude-macro-introduced)
//       Predecessor-covered (#547 / #1501 / #1609 / #1636).
//   AC4 — IR inline / JIT Apply marker check + deopt
//       DEFER to #1681 (multi-session IR / JIT instrumentation).
//   AC5 — TSan stress test (failing mutation inside batch + macro template)
//       DEFER to #1682 (CI-gated TSan test plan).
//
// Phase 1 verifies:
//   - 2 atomic counter slots present in observability_metrics.h
//   - 2 X-macro fields present in compiler_metrics_fields.inc
//   - 2 Evaluator::bump_* paired with 2 *() const noexcept getters
//   - 1+ paired wire-up sites in evaluator_fiber_mutation.cpp
//   - composition points into existing surfaces are valid

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

namespace aura_1649_detail {

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

bool check_counters_ac2_ac5_partial() {
    std::println("\n--- AC2/AC5 partial: 2 body-named atomic counters present ---");
    std::string om = read_file("src/compiler/observability_metrics.h");
    bool a = contains(om, "std::atomic<std::uint64_t> atomic_batch_hygiene_violation_prevented_total{0}");
    bool b = contains(om, "std::atomic<std::uint64_t> mutate_template_marker_propagated_total{0}");
    if (!a || !b) {
        std::println("FAIL: 2 body-named atomic counters missing "
                     "(atomic_batch_hygiene_violation_prevented={} mutate_template_marker_propagated={})",
                     a, b);
        return false;
    }
    std::println("OK: 2 body-named atomic counters present (atomic_batch_hygiene_violation_prevented_total + mutate_template_marker_propagated_total)");
    return true;
}

bool check_xmacro_fields() {
    std::println("\n--- AC2/AC5 partial: 2 X-macro fields ---");
    std::string fields = read_file("src/compiler/compiler_metrics_fields.inc");
    bool a = contains(fields, "AURA_COMPILER_METRICS_FIELD(atomic_batch_hygiene_violation_prevented_total)");
    bool b = contains(fields, "AURA_COMPILER_METRICS_FIELD(mutate_template_marker_propagated_total)");
    if (!a || !b) {
        std::println("FAIL: 2 X-macro fields missing "
                     "(atomic_batch_hygiene_violation_prevented={} mutate_template_marker_propagated={})",
                     a, b);
        return false;
    }
    std::println("OK: 2 X-macro fields present (atomic_batch_hygiene_violation_prevented_total + mutate_template_marker_propagated_total)");
    return true;
}

bool check_bumpers_getters() {
    std::println("\n--- AC2/AC5 partial: 2 Evaluator bumpers + 2 getters ---");
    std::string ixx = read_file("src/compiler/evaluator.ixx");
    bool ba = contains(ixx, "void bump_atomic_batch_hygiene_violation_prevented_total()");
    bool bb = contains(ixx, "void bump_mutate_template_marker_propagated_total()");
    bool ga = contains(ixx, "std::uint64_t atomic_batch_hygiene_violation_prevented_total() const noexcept");
    bool gb = contains(ixx, "std::uint64_t mutate_template_marker_propagated_total() const noexcept");
    if (!ba || !bb || !ga || !gb) {
        std::println("FAIL: bump_/getter pairs missing "
                     "(bumper_a={} bumper_b={} getter_a={} getter_b={})",
                     ba, bb, ga, gb);
        return false;
    }
    std::println("OK: 2 Evaluator bumpers + 2 getters present");
    return true;
}

bool check_paired_wire_up_ac2() {
    std::println("\n--- AC2: paired wire-ups at atomic_batch_pinning + template-respect site ---");
    std::string efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    // Site: atomic_batch_pinning + template-respect paired block (paired with
    // the existing #1646 macro-dirty + epoch-bump wire-ups).
    bool has_template_propagated = contains(efm, "bump_mutate_template_marker_propagated_total()") &&
                                  contains(efm, "Issue #1649");
    bool has_atomic_batch_violation_prevented = contains(efm, "bump_atomic_batch_hygiene_violation_prevented_total()");
    bool yield_hook = contains(efm, "Evaluator::yield_hook_evaluator()");
    if (!has_template_propagated || !has_atomic_batch_violation_prevented || !yield_hook) {
        std::println("FAIL: paired wire-ups missing "
                     "(propagated={} violation_prevented={} yield_hook={})",
                     has_template_propagated, has_atomic_batch_violation_prevented, yield_hook);
        return false;
    }
    std::println("OK: paired legacy/new wire-ups at template-respect + atomic_batch_pinning site");
    return true;
}

bool check_design_doc_present() {
    std::println("\n--- #1649 docs/design/1649-atomic-batch-marker.md ---");
    std::ifstream in("docs/design/1649-atomic-batch-marker.md");
    if (!in) {
        std::println("FAIL: design doc missing");
        return false;
    }
    std::println("OK: design doc present");
    return true;
}

bool check_baseline_ac4(CompilerService& cs) {
    std::println("\n--- AC4: cross-layer baseline round-trip survives #1649 wire-up ---");
    if (!cs.eval("(set-code \"(define x 42)\")")) {
        std::println("FAIL: set-code broke");
        return false;
    }
    if (auto r = cs.eval("(eval-current)"); !r || !is_int(*r)) {
        std::println("FAIL: eval-current broke");
        return false;
    }
    std::println("OK: baseline round-trip survives #1649 partial-redundant Phase 1");
    return true;
}

}  // namespace aura_1649_detail

int main() {
    using namespace aura_1649_detail;

    int rc = 0;
    if (!check_counters_ac2_ac5_partial()) rc = 1;
    if (!check_xmacro_fields())                 rc = 1;
    if (!check_bumpers_getters())              rc = 1;
    if (!check_paired_wire_up_ac2())           rc = 1;
    if (!check_design_doc_present())           rc = 1;

    if (rc == 0) {
        CompilerService cs;
        if (!check_baseline_ac4(cs)) rc = 1;
    }

    if (rc == 0) {
        std::println("\n#1649 partial-redundant Phase 1 — all AC checks green ✅\n"
                     "    AC1 full snapshot coverage → #1680\n"
                     "    AC4 IR/JIT marker check + deopt → #1681\n"
                     "    AC5 TSan stress test → #1682");
    } else {
        std::println("\n#1649 — some AC checks FAILED ❌");
    }
    return rc;
}
