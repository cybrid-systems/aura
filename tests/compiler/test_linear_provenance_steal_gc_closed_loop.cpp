// @category: unit
// @reason: Issue #2026 — post-steal / GC linear ownership + provenance
// consistency closed-loop (shared validate_linear_provenance).
//
//   AC1: source cites #2026; validate_linear_provenance in tracker +
//        evaluator_gc / ir_executor / boundary
//   AC2: Untracked always ok; Moved live → mismatch + force_deopt
//   AC3: bridge/version stale → mismatch
//   AC4: require_complete=true with zero provenance → mismatch
//   AC5: query:post-steal-closed-loop-stats schema-2026 + consistency-bp
//   AC6: probe_linear_ownership_on_fiber_steal / GC paths wired (source)
//   AC7: IR enforce_linear_ownership_state shares helper; service smoke

#include "test_harness.hpp"
#include "core/provenance_tracker.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::provenance::g_provenance_enforcement;
using aura::core::provenance::kLinearBorrowed;
using aura::core::provenance::kLinearMoved;
using aura::core::provenance::kLinearOwned;
using aura::core::provenance::kLinearUntracked;
using aura::core::provenance::linear_provenance_consistency_bp;
using aura::core::provenance::reset_provenance_enforcement_for_test;
using aura::core::provenance::validate_linear_provenance;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:post-steal-closed-loop-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_source() {
    std::println("\n--- AC1: source cites #2026 ---");
    auto pt = read_file("src/core/provenance_tracker.hh");
    auto gc = read_file("src/compiler/evaluator_gc.cpp");
    auto ir = read_file("src/compiler/ir_executor_impl.cpp");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(!pt.empty(), "provenance_tracker.hh readable");
    CHECK(pt.find("Issue #2026") != std::string::npos, "tracker cites #2026");
    CHECK(pt.find("validate_linear_provenance") != std::string::npos, "validate helper");
    CHECK(pt.find("linear_provenance_consistency_bp") != std::string::npos, "consistency bp");
    CHECK(!gc.empty() && gc.find("Issue #2026") != std::string::npos, "evaluator_gc cites #2026");
    CHECK(gc.find("validate_linear_provenance") != std::string::npos, "gc uses helper");
    CHECK(gc.find("linear_provenance_steal_checks") != std::string::npos ||
              gc.find("linear_provenance_steal_checks_total") != std::string::npos,
          "steal path stamps checks");
    CHECK(!ir.empty() && ir.find("Issue #2026") != std::string::npos, "ir_executor cites #2026");
    CHECK(!q.empty() && q.find("schema-2026") != std::string::npos, "query schema-2026");
    CHECK(q.find("linear-provenance-consistency-bp") != std::string::npos, "query consistency key");
}

static void ac2_untracked_and_moved() {
    std::println("\n--- AC2: Untracked ok; Moved live mismatch ---");
    reset_provenance_enforcement_for_test();
    auto u = validate_linear_provenance(kLinearUntracked);
    CHECK(u.ok && !u.force_deopt, "Untracked ok");
    auto m = validate_linear_provenance(kLinearMoved, /*node*/ 1);
    CHECK(!m.ok && m.force_deopt, "Moved force_deopt");
    CHECK(m.reason != nullptr, "reason set");
    CHECK(g_provenance_enforcement().linear_provenance_moved_live_total.load() >= 1,
          "moved-live counter");
    CHECK(g_provenance_enforcement().linear_provenance_deopt_total.load() >= 1, "deopt counter");
}

static void ac3_stale_version_bridge() {
    std::println("\n--- AC3: version/bridge stale → mismatch ---");
    reset_provenance_enforcement_for_test();
    auto v = validate_linear_provenance(kLinearOwned, 0, /*prov*/ 1, /*mut*/ 1,
                                        /*frame*/ 1, /*current*/ 5, 0, 0, false);
    CHECK(!v.ok && v.force_deopt, "stale frame version fails");
    auto b = validate_linear_provenance(kLinearOwned, 0, 1, 1, 10, 10,
                                        /*bridge*/ 2, /*current_bridge*/ 9, false);
    CHECK(!b.ok && b.force_deopt, "bridge mismatch fails");
}

static void ac4_require_complete() {
    std::println("\n--- AC4: require_complete without provenance ---");
    reset_provenance_enforcement_for_test();
    auto soft = validate_linear_provenance(kLinearOwned, 0, 0, 0, 0, 0, 0, 0,
                                           /*require_complete=*/false);
    CHECK(soft.ok, "soft incomplete still ok");
    CHECK(g_provenance_enforcement().linear_provenance_incomplete_total.load() >= 1,
          "incomplete counted");
    auto hard = validate_linear_provenance(kLinearBorrowed, 0, 0, 0, 0, 0, 0, 0,
                                           /*require_complete=*/true);
    CHECK(!hard.ok && hard.force_deopt, "require_complete fails without trail");
    // With provenance trail, ok
    auto ok =
        validate_linear_provenance(kLinearOwned, 7, /*prov*/ 42, /*mut*/ 99, 0, 0, 0, 0, true);
    CHECK(ok.ok, "complete trail ok under require_complete");
}

static void ac5_query_keys() {
    std::println("\n--- AC5: post-steal-closed-loop-stats schema-2026 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:post-steal-closed-loop-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2026") == 2026, "schema-2026");
    CHECK(href(cs, "issue-2026") == 2026, "issue-2026");
    CHECK(href(cs, "linear-provenance-wired") == 1, "wired");
    CHECK(href(cs, "linear-provenance-checks") >= 0, "checks");
    CHECK(href(cs, "linear-provenance-ok") >= 0, "ok");
    CHECK(href(cs, "linear-provenance-mismatch") >= 0, "mismatch");
    CHECK(href(cs, "linear-provenance-consistency-bp") >= 0, "consistency-bp");
    CHECK(href(cs, "linear-provenance-consistency-ratio-bp") >= 0, "ratio alias");
    CHECK(href(cs, "linear-provenance-steal-checks") >= 0, "steal checks");
    CHECK(href(cs, "linear-provenance-gc-checks") >= 0, "gc checks");
    // Primary post-steal schema lineage retained (#1631); #2026 is satellite
    CHECK(href(cs, "schema") == 1631 || href(cs, "schema") == 2026, "schema lineage");
}

static void ac6_steal_gc_source() {
    std::println("\n--- AC6: steal/GC probe paths wired ---");
    auto gc = read_file("src/compiler/evaluator_gc.cpp");
    CHECK(gc.find("probe_linear_ownership_on_fiber_steal") != std::string::npos, "steal probe");
    CHECK(gc.find("probe_linear_ownership_at_gc_safepoint") != std::string::npos, "gc probe");
    CHECK(gc.find("enforce_linear_boundary_consistency") != std::string::npos, "boundary");
    CHECK(gc.find("linear_provenance_steal_checks_total") != std::string::npos, "steal counter");
    CHECK(gc.find("linear_provenance_gc_checks_total") != std::string::npos, "gc counter");
    // Boundary failure consolidates through enforce_linear_boundary_consistency
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(bound.find("enforce_linear_post_failure") != std::string::npos ||
              bound.find("enforce_linear_boundary_consistency") != std::string::npos,
          "boundary uses linear enforce");
}

static void ac7_service_smoke() {
    std::println("\n--- AC7: service smoke + consistency bp ---");
    reset_provenance_enforcement_for_test();
    // Drive a few checks
    (void)validate_linear_provenance(kLinearUntracked);
    (void)validate_linear_provenance(kLinearOwned, 1, 1, 1);
    const auto bp = linear_provenance_consistency_bp();
    CHECK(bp <= 10000, "bp ≤ 10000");
    CHECK(bp >= 0, "bp ≥ 0");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval ok");
    CHECK(href(cs, "linear-provenance-wired") == 1, "wired after eval");
    auto h = cs.eval("(engine:metrics \"query:post-steal-closed-loop-stats\")");
    CHECK(h && is_hash(*h), "stats hash after eval");
}

} // namespace

int main() {
    ac1_source();
    ac2_untracked_and_moved();
    ac3_stale_version_bridge();
    ac4_require_complete();
    ac5_query_keys();
    ac6_steal_gc_source();
    ac7_service_smoke();
    if (g_failed)
        return 1;
    std::println("linear provenance steal/GC closed-loop (#2026): OK ({} passed)", g_passed);
    return 0;
}
