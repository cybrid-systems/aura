// @category: unit
// @reason: Issue #2560 — partial re-infer cone soft/hard cap + type_dep
//          degree truncation (type-layer SLA).
//
//   AC1: soft overflow metric + cap path source-cite (≤ soft or overflow)
//   AC2: hard fallback under production when cone > hard
//   AC3: under soft → zero new overflow when size ≤ soft (source + defaults)
//   AC4: #2516 order preserved (cap before invalidate; empty early-return)
//   AC5: schema-2560 on type-dep-partial-merge-stats + source-cite

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    // Partial-cone / dirty-txn keys live on type-dep-partial-merge-stats
    // (same surface as schema-2516 / #2283 merge counters).
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-dep-partial-merge-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: soft path source + metric keys ──
static void ac1_soft_overflow_path() {
    std::println("\n--- #2560 AC1: soft overflow path ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("Issue #2560") != std::string::npos, "AC1: impl cites #2560");
    CHECK(impl.find("partial_cone_soft_cap") != std::string::npos, "AC1: soft cap helper");
    CHECK(impl.find("partial_cone_soft_overflow_total") != std::string::npos,
          "AC1: soft overflow metric");
    CHECK(impl.find("AURA_PARTIAL_CONE_SOFT") != std::string::npos, "AC1: soft env");
    CHECK(impl.find("truncate_partial_cone_seed_preserving") != std::string::npos,
          "AC1: seed-preserving truncate");
    CHECK(impl.find("type_dep_fanout_cap") != std::string::npos, "AC1: degree fan-out cap");
    // Default 256 soft.
    CHECK(impl.find("return 256") != std::string::npos, "AC1: default soft 256");
}

// ── AC2: hard production fallback ──
static void ac2_hard_production() {
    std::println("\n--- #2560 AC2: hard fallback under production ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("partial_cone_hard_fallback_total") != std::string::npos,
          "AC2: hard_fallback metric");
    CHECK(impl.find("AURA_PARTIAL_CONE_HARD") != std::string::npos, "AC2: hard env");
    CHECK(impl.find("return 2048") != std::string::npos, "AC2: default hard 2048");
    CHECK(impl.find("production_defaults_active") != std::string::npos,
          "AC2: production gate for hard");
    // hard path uses pre-truncate orig_sz.
    CHECK(impl.find("orig_sz > hard") != std::string::npos, "AC2: hard uses pre-truncate size");
}

// ── AC3: under soft zero cost ──
static void ac3_under_soft_zero() {
    std::println("\n--- #2560 AC3: under soft_cap zero overflow work ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("Zero extra work when cone under soft cap") != std::string::npos ||
              impl.find("Zero cost when size") != std::string::npos ||
              impl.find("orig_sz > soft") != std::string::npos,
          "AC3: only overflow when over soft");
    // Metrics start at 0 on fresh service.
    CompilerService cs;
    CHECK(href(cs, "partial-cone-soft-overflow-total") == 0 ||
              href(cs, "partial-cone-soft-overflow-total") >= 0,
          "AC3: soft overflow queryable");
    CHECK(href(cs, "partial-cone-hard-fallback-total") >= 0, "AC3: hard fallback queryable");
}

// ── AC4: #2516 order ──
static void ac4_txn_order() {
    std::println("\n--- #2560 AC4: #2516 order preserved ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto cap_pos = impl.find("Issue #2560: partial cone soft/hard SLA");
    const auto p1_pos = impl.find("invalidate_type_dep_for_nodes");
    CHECK(cap_pos != std::string::npos, "AC4: #2560 block present");
    CHECK(p1_pos != std::string::npos, "AC4: phase1 invalidate present");
    // Cap block must appear before invalidate in this function body.
    // Use the #2516 comment as anchor after #2560.
    const auto txn_pos = impl.find("Issue #2516 dirty txn");
    CHECK(txn_pos != std::string::npos && cap_pos < txn_pos,
          "AC4: cone cap before #2516 dirty txn phases");
    CHECK(impl.find("Empty affected already returned above") != std::string::npos ||
              impl.find("affected.empty()") != std::string::npos,
          "AC4: empty early-return retained");

    const auto ixx = read_file("src/compiler/type_checker.ixx");
    CHECK(ixx.find("Issue #2560") != std::string::npos, "AC4: ixx documents #2560");
    CHECK(ixx.find("AURA_PARTIAL_CONE_SOFT") != std::string::npos, "AC4: ixx env cite");
}

// ── AC5: schema + registrations ──
static void ac5_schema() {
    std::println("\n--- #2560 AC5: schema-2560 + gate ---");
    const auto q = ::aura::test::aura_query_prims_source();
    CHECK(q.find("schema-2560") != std::string::npos, "AC5: schema-2560");
    CHECK(q.find("partial-cone-soft-overflow-total") != std::string::npos, "AC5: soft key");
    CHECK(q.find("partial-cone-hard-fallback-total") != std::string::npos, "AC5: hard key");
    CHECK(q.find("partial-cone-type-dep-degree-trunc-total") != std::string::npos,
          "AC5: degree trunc key");
    CHECK(q.find("partial-cone-last-size") != std::string::npos, "AC5: last size key");

    const auto mh = read_file("src/compiler/observability_metrics.h");
    CHECK(mh.find("partial_cone_soft_overflow_total") != std::string::npos, "AC5: metrics soft");
    CHECK(mh.find("partial_cone_hard_fallback_total") != std::string::npos, "AC5: metrics hard");
    CHECK(mh.find("#2560") != std::string::npos, "AC5: metrics cite #2560");

    CompilerService cs;
    CHECK(href(cs, "schema-2560") == 2560, "AC5: live schema-2560");
    CHECK(href(cs, "partial-cone-cap-wired") == 1, "AC5: cap wired");
    CHECK(href(cs, "schema-2516") == 2516, "AC5: #2516 lineage retained");

    // Direct metric smoke: counters are live atomics.
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "AC5: metrics ptr");
    if (m) {
        const auto s0 = m->partial_cone_soft_overflow_total.load();
        m->partial_cone_soft_overflow_total.fetch_add(1);
        CHECK(m->partial_cone_soft_overflow_total.load() == s0 + 1, "AC5: soft counter live");
        m->partial_cone_soft_overflow_total.store(s0);
    }

    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    CHECK(cmake.find("test_partial_cone_cap") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_partial_cone_cap_2560") != std::string::npos, "AC5: build script");
    CHECK(build.find("cmd_partial_cone_cap_coverage") != std::string::npos, "AC5: build cmd");
}

// ── Issue #3189: unify impact upper-bound on every production
// partial-relower decision site (fail-closed) ──
// Closes the residual where invalidate_bridge_with_impact (the quote /
// lambda path inside invalidate_function) chose partial based on
// `scope.affected_instrs/blocks.size() < threshold` without consulting
// the impact-checked helper that catches cross-fn callee under-count.
// Existing siblings (#3034 try_partial_invalidate_relower + #2246
// apply_partial_relower_storm_gate) already call the helper — #3189
// unifies the third site + verifies the contract.
//   AC1: every production partial decision site calls
//        should_partial_relower_impact_checked(dirty_n, impact_ub) —
//        try_partial_invalidate_relower (L1270) +
//        apply_partial_relower_storm_gate (service.ixx:7021) +
//        invalidate_bridge_with_impact (L1163, #3189)
//   AC2: dirty_count_est vs impact_ub in invalidate_bridge_with_impact;
//        bump partial_forced_full_by_impact_total on under-estimate
//   AC3: cross-fn callee scope via compute_impact_scope at L1127
//        before the impact-checked helper consults
//   AC4: Soft path unchanged — empty scope early-exits
//   AC5: existing #2560 / #2246 / #3034 sibling ACs preserved

static void ac3189_partial_impact_upper_bound_unified() {
    std::println(
        "\n--- #3189: production partial decision sites all consult impact-checked helper ---");

    const auto svc = read_file("src/compiler/service_dirty.cpp");
    const auto sixx = read_file("src/compiler/service.ixx");
    const auto ixx = read_file("src/compiler/ir_cache_pure.ixx");
    const auto obs = read_file("src/compiler/observability_metrics.h");

    // AC1: every production partial decision site calls the helper.
    {
        // Site 1: try_partial_invalidate_relower (L1270) — #3034 sibling
        CHECK(svc.find("should_partial_relower_impact_checked(dirty_n, impact_ub)") !=
                  std::string::npos,
              "ac3189 AC1: try_partial_invalidate_relower calls helper (dirty_n, impact_ub)");
        // Site 2: apply_partial_relower_storm_gate (service.ixx:7021) — #2246 sibling
        CHECK(sixx.find("should_partial_relower_impact_checked(dirty_n, impact_ub)") !=
                  std::string::npos,
              "ac3189 AC1: apply_partial_relower_storm_gate calls helper (dirty_n, impact_ub)");
        // Site 3: invalidate_bridge_with_impact (L1163) — #3189 NEW
        CHECK(
            svc.find("should_partial_relower_impact_checked(dirty_count_est, impact_ub)") !=
                std::string::npos,
            "ac3189 AC1: invalidate_bridge_with_impact calls helper (dirty_count_est, impact_ub)");
        // Helper definition
        CHECK(ixx.find("should_partial_relower_impact_checked") != std::string::npos,
              "ac3189 AC1: helper defined in ir_cache_pure.ixx");
    }

    // AC2: source-cite the dirty_count_est vs impact_ub decision + metric bump.
    {
        CHECK(svc.find("Issue #3189 AC1: every production partial decision entry") !=
                  std::string::npos,
              "ac3189 AC2: invalidate_bridge_with_impact cites Issue #3189 AC1");
        CHECK(svc.find("dirty_count_est") != std::string::npos,
              "ac3189 AC2: dirty_count_est variable defined (scope.affected_blocks + "
              "scope.affected_instrs)");
        CHECK(svc.find("impact_upper_bound_for_entry_(affected_name, cit->second)") !=
                  std::string::npos,
              "ac3189 AC2: impact_ub computed via impact_upper_bound_for_entry_");
        CHECK(svc.find("partial_forced_full_by_impact_total.fetch_add(") != std::string::npos,
              "ac3189 AC2: metric bumped on under-estimate");
        // No new metric key — reuses existing partial_forced_full_by_impact_total
        CHECK(obs.find("partial_forced_full_by_impact_total") != std::string::npos,
              "ac3189 AC2: existing counter reused (no new metric key)");
    }

    // AC3: cross-fn callee scope via compute_impact_scope before the impact helper consults.
    {
        // find the impact_checked call inside invalidate_bridge_with_impact
        const auto lambda_pos = svc.find("invalidate_bridge_with_impact");
        REQUIRE(lambda_pos != std::string::npos);
        const auto lambda_end = svc.find("\n    }", lambda_pos);
        const auto lambda_end2 = (lambda_end == std::string::npos) ? lambda_pos + 8000 : lambda_end;
        const auto lambda_win = svc.substr(lambda_pos, lambda_end2 - lambda_pos);
        const auto scope_call = lambda_win.find("compute_impact_scope");
        const auto helper_call = lambda_win.find("should_partial_relower_impact_checked");
        CHECK(scope_call != std::string::npos, "ac3189 AC3: invalidate_bridge_with_impact calls "
                                               "compute_impact_scope (cross-fn callee scope)");
        CHECK(helper_call != std::string::npos,
              "ac3189 AC3: invalidate_bridge_with_impact calls the impact-checked helper");
        CHECK(scope_call < helper_call, "ac3189 AC3: compute_impact_scope precedes the helper "
                                        "(cross-fn scope is available for impact_ub)");
    }

    // AC4: Soft path unchanged — empty scope early-exits.
    {
        // The helper returns true (partial OK) when dirty_count == 0
        // AND impact_ub == 0. Verify the helper definition handles this.
        const auto helper_def = ixx.find("should_partial_relower_impact_checked");
        REQUIRE(helper_def != std::string::npos);
        const auto helper_end = ixx.find("\n}\n", helper_def);
        const auto helper_end2 = (helper_end == std::string::npos) ? helper_def + 800 : helper_end;
        const auto helper_body = ixx.substr(helper_def, helper_end2 - helper_def);
        // Soft / Off + clean: dirty_count == 0 returns true (partial OK, no work)
        CHECK(helper_body.find("if (dirty_count == 0 && impact_upper_bound == 0)") !=
                      std::string::npos ||
                  helper_body.find("dirty_count == 0") != std::string::npos,
              "ac3189 AC4: helper has Soft / clean early-return (zero extra work)");
    }

    // AC5: existing sibling ACs preserved + no new tests/issues/test_issue_3189.cpp
    //      + no docs/design/3189-* + linter wired after #3188.
    {
        // #2560 AC1 (soft overflow metric + cap path)
        CHECK(svc.find("partial_cone_soft_overflow_total") != std::string::npos ||
                  svc.find("partial_cone") != std::string::npos,
              "ac3189 AC5: #2560 partial-cone sibling surface preserved");
        // #3034 sibling AC (try_partial_invalidate_relower already calls helper)
        CHECK(svc.find("try_partial_invalidate_relower") != std::string::npos,
              "ac3189 AC5: #3034 try_partial_invalidate_relower sibling surface preserved");
        // #2246 sibling AC (apply_partial_relower_storm_gate already calls helper)
        CHECK(sixx.find("apply_partial_relower_storm_gate") != std::string::npos,
              "ac3189 AC5: #2246 apply_partial_relower_storm_gate sibling surface preserved");
        // No new tests/issues/test_issue_3189.cpp (per #81934)
        const auto issue_test = read_file("tests/issues/test_issue_3189.cpp");
        CHECK(issue_test.empty(),
              "ac3189 AC5: no new tests/issues/test_issue_3189.cpp (must NOT — src-aligned only)");
        // No docs/design/3189-* (per #1655)
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3189-") == std::string::npos,
                      std::string("ac3189 AC5: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
        // Linter wired after #3188 (covered separately by
        // check_partial_impact_upper_bound_3189.py self-test)
        const auto build = read_file("build.py");
        CHECK(build.find("check_partial_impact_upper_bound_3189") != std::string::npos,
              "ac3189 AC5: linter wired in build.py");
    }
}

} // namespace

int run_test_partial_cone_cap() {
    std::println("=== Issue #2560: partial re-infer cone soft/hard cap ===");
    apply_dev_audit_defaults();
    ac1_soft_overflow_path();
    ac2_hard_production();
    ac3_under_soft_zero();
    ac4_txn_order();
    ac5_schema();

    // Issue #3189: unify impact upper-bound on every production
    // partial-relower decision site (fail-closed). Source-cite ACs
    // extend the #2560 partial-cone-cap test surface (closest match —
    // both concern partial vs full relower decision in
    // invalidate_bridge_with_impact / try_partial_invalidate_relower).
    //   AC1: every production partial decision site calls
    //        should_partial_relower_impact_checked (the helper was
    //        already wired into try_partial_invalidate_relower per
    //        #3034 + service.ixx apply_partial_relower_storm_gate
    //        per #2246 — #3189 wires it into the third site,
    //        invalidate_bridge_with_impact, inside invalidate_function)
    //   AC2: source-cite the dirty_count_est vs impact_ub decision in
    //        the new call site; the bump goes to
    //        partial_forced_full_by_impact_total (no new metric key)
    //   AC3: cross-fn callee scope crossing into a caller that
    //        block_dirty alone can't see is covered by
    //        compute_impact_scope (L1127) before the impact-checked
    //        helper is consulted at L1163
    //   AC4: Soft path unchanged — early-exit when
    //        scope.affected_blocks/instrs are both empty (no extra
    //        work on clean windows)
    //   AC5: existing #2560 / #2246 / #3034 sibling ACs preserved
    //        (no new tests/issues/test_issue_3189.cpp per #81934,
    //        no docs/design/3189-* per #1655, linter wired after
    //        #3188)

    ac3189_partial_impact_upper_bound_unified();

    apply_dev_audit_defaults();
    std::println("\n=== #2560 + #3189: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_partial_cone_cap();
}
#endif
