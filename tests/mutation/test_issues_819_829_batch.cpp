// test_issues_819_829_batch.cpp — Phase 1 close for Issues #819–#829.
//
// Metrics + query surfaces + light production wiring:
//   #819 pattern-hygiene-provenance-stats
//   #820 mutate-atomic-batch-e2e-stats (+ bumps on mutate:atomic-batch)
//   #821 jit-fiber-exception-stats
//   #822 l2-specialization-deopt-stats
//   #823 opcode-coverage-deopt-stats
//   #824 terminal-render-production-stats + terminal:* primitives
//   #825 render-ffi-buffer-stats
//   #826 render-hotpath-stats
//   #827 shape-value-hotpath-contracts-stats
//   #828 ir-soa-full-enforcement-stats
//   #829 arena-live-defrag-stats

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issues_819_829 {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \'{}\')", aura::test::aura_call_expr(q), key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void check_schema(CompilerService& cs, std::string_view q, std::int64_t schema) {
    auto h = cs.eval(aura::test::aura_call_expr(q));
    CHECK(h && is_hash(*h), std::format("{} returns hash", q));
    CHECK(href(cs, q, "schema") == schema, std::format("{} schema == {}", q, schema));
}

static void run_matrix(CompilerService& cs) {
    auto& ev = cs.evaluator();

    std::println("\n--- #819 pattern-hygiene-provenance-stats ---");
    check_schema(cs, "query:pattern-hygiene-provenance-stats", 819);
    CHECK(href(cs, "query:pattern-hygiene-provenance-stats", "enforcement-active") == 1,
          "enforcement-active");
    ev.bump_pattern_hygiene_provenance_predicate_hit();
    ev.bump_pattern_hygiene_index_enforced_hit();
    ev.bump_pattern_hygiene_yield_enforced();
    ev.bump_pattern_hygiene_safe_span_enforced();
    CHECK(href(cs, "query:pattern-hygiene-provenance-stats", "predicate-hits") >= 1,
          "predicate-hits");
    CHECK(href(cs, "query:pattern-hygiene-provenance-stats", "safe-span-enforced") >= 1,
          "safe-span-enforced");
    CHECK(href(cs, "query:pattern-hygiene-provenance-stats", "yield-points-hit") >= 1,
          "yield-points-hit");

    std::println("\n--- #820 mutate-atomic-batch-e2e-stats ---");
    check_schema(cs, "query:mutate-atomic-batch-e2e-stats", 820);
    CHECK(href(cs, "query:mutate-atomic-batch-e2e-stats", "e2e-active") == 1, "e2e-active");
    ev.bump_mutate_batch_e2e_started();
    ev.bump_mutate_batch_e2e_suppressed_bumps(2);
    ev.bump_mutate_batch_e2e_hygiene_in_batch();
    ev.bump_mutate_batch_e2e_cross_fiber_steals();
    ev.bump_mutate_batch_e2e_pinned_snapshot();
    ev.bump_mutate_batch_e2e_panic_recoveries();
    CHECK(href(cs, "query:mutate-atomic-batch-e2e-stats", "batches-started") >= 1,
          "batches-started");
    CHECK(href(cs, "query:mutate-atomic-batch-e2e-stats", "suppressed-bumps") >= 2,
          "suppressed-bumps");
    CHECK(href(cs, "query:mutate-atomic-batch-e2e-stats", "pinned-snapshots") >= 1,
          "pinned-snapshots");

    std::println("\n--- #821 jit-fiber-exception-stats ---");
    check_schema(cs, "query:jit-fiber-exception-stats", 821);
    CHECK(href(cs, "query:jit-fiber-exception-stats", "fiber-local-policy-active") == 1,
          "fiber-local-policy-active");
    ev.bump_jit_fiber_ex_stack_local();
    ev.bump_jit_fiber_ex_cross_prevented();
    ev.bump_jit_fiber_ex_deopt_interpreter();
    CHECK(href(cs, "query:jit-fiber-exception-stats", "fiber-local-ex-stack") >= 1,
          "fiber-local-ex-stack");
    CHECK(href(cs, "query:jit-fiber-exception-stats", "cross-fiber-prevented") >= 1,
          "cross-fiber-prevented");

    std::println("\n--- #822 l2-specialization-deopt-stats ---");
    check_schema(cs, "query:l2-specialization-deopt-stats", 822);
    CHECK(href(cs, "query:l2-specialization-deopt-stats", "l2-maturity-active") == 1,
          "l2-maturity-active");
    ev.bump_l2_spec_pair_fastpath();
    ev.bump_l2_spec_deopt_version();
    ev.bump_l2_spec_guardshape_narrow();
    ev.bump_l2_spec_linear_probe();
    CHECK(href(cs, "query:l2-specialization-deopt-stats", "pair-fastpath") >= 1, "pair-fastpath");
    CHECK(href(cs, "query:l2-specialization-deopt-stats", "linear-probe") >= 1, "linear-probe");

    std::println("\n--- #823 opcode-coverage-deopt-stats ---");
    check_schema(cs, "query:opcode-coverage-deopt-stats", 823);
    CHECK(href(cs, "query:opcode-coverage-deopt-stats", "zero-fallback-policy") == 1,
          "zero-fallback-policy");
    ev.bump_opcode_cov_hit();
    ev.bump_opcode_cov_unhandled_hot();
    ev.bump_opcode_cov_per_fn_deopt();
    CHECK(href(cs, "query:opcode-coverage-deopt-stats", "coverage-hits") >= 1, "coverage-hits");
    CHECK(href(cs, "query:opcode-coverage-deopt-stats", "per-fn-deopt") >= 1, "per-fn-deopt");

    std::println("\n--- #824 terminal-render-production-stats + terminal:* ---");
    check_schema(cs, "query:terminal-render-production-stats", 824);
    CHECK(href(cs, "query:terminal-render-production-stats", "module-active") == 1,
          "module-active");
    auto clr = cs.eval("(terminal:clear)");
    CHECK(clr && is_bool(*clr), "terminal:clear");
    auto draw = cs.eval("(terminal:draw-batch 100)");
    CHECK(draw && is_bool(*draw), "terminal:draw-batch");
    auto pres = cs.eval("(terminal:present)");
    CHECK(pres && is_bool(*pres), "terminal:present");
    auto dirty = cs.eval("(terminal:mark-dirty-region)");
    CHECK(dirty && is_bool(*dirty), "terminal:mark-dirty-region");
    auto delta = cs.eval("(terminal:present-delta)");
    CHECK(delta && is_bool(*delta), "terminal:present-delta");
    CHECK(href(cs, "query:terminal-render-production-stats", "clear-total") >= 1, "clear-total");
    CHECK(href(cs, "query:terminal-render-production-stats", "present-total") >= 1,
          "present-total");
    CHECK(href(cs, "query:terminal-render-production-stats", "dirty-region-total") >= 1,
          "dirty-region-total");

    std::println("\n--- #825 render-ffi-buffer-stats ---");
    check_schema(cs, "query:render-ffi-buffer-stats", 825);
    CHECK(href(cs, "query:render-ffi-buffer-stats", "buffer-path-active") == 1,
          "buffer-path-active");
    ev.bump_render_ffi_batch_call();
    ev.bump_render_ffi_zerocopy_view();
    ev.bump_render_ffi_crossing_ns(50);
    ev.bump_render_ffi_allocs_frame(3);
    CHECK(href(cs, "query:render-ffi-buffer-stats", "batch-ffi-calls") >= 1, "batch-ffi-calls");
    CHECK(href(cs, "query:render-ffi-buffer-stats", "zerocopy-views") >= 1, "zerocopy-views");

    std::println("\n--- #826 render-hotpath-stats ---");
    check_schema(cs, "query:render-hotpath-stats", 826);
    CHECK(href(cs, "query:render-hotpath-stats", "hotpath-active") == 1, "hotpath-active");
    // dirty/delta already bumped by terminal:mark-dirty-region / present-delta
    CHECK(href(cs, "query:render-hotpath-stats", "dirty-hits") >= 1, "dirty-hits from terminal");
    CHECK(href(cs, "query:render-hotpath-stats", "present-delta") >= 1,
          "present-delta from terminal");
    ev.bump_render_hp_jit_coverage();
    ev.bump_render_hp_mutation_impact();
    CHECK(href(cs, "query:render-hotpath-stats", "jit-coverage") >= 1, "jit-coverage");

    std::println("\n--- #827 shape-value-hotpath-contracts-stats ---");
    check_schema(cs, "query:shape-value-hotpath-contracts-stats", 827);
    CHECK(href(cs, "query:shape-value-hotpath-contracts-stats", "contracts-active") == 1,
          "contracts-active");
    ev.bump_sv_contract_hotpath_check();
    ev.bump_sv_consteval_dispatch_hit();
    ev.bump_sv_stability_transition();
    CHECK(href(cs, "query:shape-value-hotpath-contracts-stats", "contract-checks-hotpath") >= 1,
          "contract-checks-hotpath");
    CHECK(href(cs, "query:shape-value-hotpath-contracts-stats", "consteval-dispatch-hits") >= 1,
          "consteval-dispatch-hits");

    std::println("\n--- #828 ir-soa-full-enforcement-stats ---");
    check_schema(cs, "query:ir-soa-full-enforcement-stats", 828);
    CHECK(href(cs, "query:ir-soa-full-enforcement-stats", "enforcement-active") == 1,
          "enforcement-active");
    ev.bump_irsoa_enforce_dirty_skip();
    ev.bump_irsoa_enforce_impact_hybrid();
    ev.bump_irsoa_enforce_pmr_util_pct(75);
    ev.bump_irsoa_enforce_relower_savings(10);
    CHECK(href(cs, "query:ir-soa-full-enforcement-stats", "dirty-skips") >= 1, "dirty-skips");
    CHECK(href(cs, "query:ir-soa-full-enforcement-stats", "pmr-util-pct") >= 75, "pmr-util-pct");

    std::println("\n--- #829 arena-live-defrag-stats ---");
    check_schema(cs, "query:arena-live-defrag-stats", 829);
    CHECK(href(cs, "query:arena-live-defrag-stats", "live-defrag-active") == 1,
          "live-defrag-active");
    ev.bump_arena_ldefrag_auto_trigger();
    ev.bump_arena_ldefrag_savings(4096);
    ev.bump_arena_ldefrag_fiber_yield();
    ev.bump_arena_ldefrag_shape_inval();
    ev.bump_arena_ldefrag_pointer_fixup();
    CHECK(href(cs, "query:arena-live-defrag-stats", "auto-triggers") >= 1, "auto-triggers");
    CHECK(href(cs, "query:arena-live-defrag-stats", "fiber-yield-during") >= 1,
          "fiber-yield-during");
    CHECK(href(cs, "query:arena-live-defrag-stats", "pointer-fixup-hits") >= 1,
          "pointer-fixup-hits");

    std::println("\n--- regression classic eval ---");
    auto c = cs.eval("(+ 20 22)");
    CHECK(c && is_int(*c) && as_int(*c) == 42, "classic eval");
}

} // namespace aura_issues_819_829

int main() {
    aura::compiler::CompilerService cs;
    aura_issues_819_829::run_matrix(cs);
    return RUN_ALL_TESTS();
}
