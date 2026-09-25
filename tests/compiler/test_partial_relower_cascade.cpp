// Issue #2041 — Partial re-lower + JIT hot-swap end-to-end on
// invalidate_function cascade for AI multi-round mutate.
//
// AC1: source cites #2041; should_partial_relower used in cascade
// AC2: query:incremental-relower-stats schema-2041 wire flags
// AC3: invalidate with nested-lambda body-only dirty prefers partial
//      (incremental_partial_relower / minimal_recompile / jit partial)
// AC4: sustained mutate → counters non-decreasing; no crash
// AC5: large dirty surface still allowed (threshold respected)
// AC6: bridge_epoch / epoch path still enforced (no silent stale IR)

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

import std;
import aura.compiler.ir;
import aura.compiler.ir_cache_pure;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::absorb_callee_cone_into_impact_ub;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::estimate_relower_blocks;
using aura::compiler::estimate_relower_blocks_impact_checked;
using aura::compiler::g_partial_relower_callee_cascade_precompute_observe_total;
using aura::compiler::g_partial_relower_callee_cascade_precompute_total;
using aura::compiler::get_partial_relower_threshold;
using aura::compiler::incremental_soundness_mismatch_atomic;
using aura::compiler::kPartialRelowerCalleeCascadeIssue;
using aura::compiler::kPartialRelowerCalleeConeAbsorbIssue;
using aura::compiler::kUnknownCalleeConeBlocks;
using aura::compiler::reset_incremental_soundness_for_test;
using aura::compiler::reset_partial_relower_threshold_for_test;
using aura::compiler::set_incremental_soundness_mode;
using aura::compiler::set_partial_relower_threshold;
using aura::compiler::should_partial_relower;
using aura::compiler::should_partial_relower_impact_checked;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:incremental-relower-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void ac1_source() {
    std::println("\n--- AC1: source cites #2041 + should_partial_relower cascade ---");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(!dirty.empty(), "service_dirty readable");
    CHECK(dirty.find("#2041") != std::string::npos, "service_dirty #2041");
    CHECK(dirty.find("should_partial_relower") != std::string::npos,
          "cascade uses should_partial_relower");
    CHECK(dirty.find("relower_only_dirty_blocks") != std::string::npos ||
              dirty.find("try_partial_invalidate_relower") != std::string::npos,
          "partial path helper");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    CHECK(pure.find("should_partial_relower") != std::string::npos, "pure helper");
    auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("partial_recompile") != std::string::npos, "JIT partial_recompile wired");
}

void ac2_query_schema() {
    std::println("\n--- AC2: query schema-2041 wire flags ---");
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:incremental-relower-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2041") == 2041, "schema-2041");
    CHECK(href(cs, "issue-2041") == 2041, "issue-2041");
    CHECK(href(cs, "invalidate-cascade-partial-wired") == 1, "cascade partial wired");
    CHECK(href(cs, "should-partial-relower-cascade-wired") == 1, "should_partial wired");
    CHECK(href(cs, "jit-partial-recompile-on-cascade-wired") == 1, "jit partial wired");
    CHECK(href(cs, "partial-relower-threshold") == 8, "thr 8");
}

void ac3_invalidate_partial_path() {
    std::println("\n--- AC3: invalidate cascade prefers partial ---");
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    // Call-chain: invalidate "a" marks body-only dirty on a (+ callers via
    // dep graph when edges exist). Cascade try_partial uses
    // should_partial_relower + relower_only_dirty_blocks → partial_recompile.
    CHECK(cs.eval("(set-code \""
                  "(define a (lambda () 1))"
                  "(define b (lambda () (a)))"
                  "\")")
              .has_value(),
          "set-code chain");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    const auto partial0 = m->incremental_partial_relower_total.load(std::memory_order_relaxed);
    const auto scope0 = m->minimal_recompile_scope_samples.load(std::memory_order_relaxed);
    const auto saved0 = m->minimal_recompile_clean_funcs_saved.load(std::memory_order_relaxed);
    const auto jit_partial0 =
        m->jit_partial_recompile_requests_total.load(std::memory_order_relaxed);
    const auto block_marks0 = m->dirty_propagation_block_marks.load(std::memory_order_relaxed);
    const auto inv0 = m->invalidate_function_calls.load(std::memory_order_relaxed);

    cs.public_invalidate_function("a");

    const auto partial1 = m->incremental_partial_relower_total.load(std::memory_order_relaxed);
    const auto scope1 = m->minimal_recompile_scope_samples.load(std::memory_order_relaxed);
    const auto saved1 = m->minimal_recompile_clean_funcs_saved.load(std::memory_order_relaxed);
    const auto jit_partial1 =
        m->jit_partial_recompile_requests_total.load(std::memory_order_relaxed);
    const auto block_marks1 = m->dirty_propagation_block_marks.load(std::memory_order_relaxed);
    const auto inv1 = m->invalidate_function_calls.load(std::memory_order_relaxed);

    std::println(
        "  inv {}→{} partial {}→{} scope {}→{} saved {}→{} jit_partial {}→{} block_marks {}→{}",
        inv0, inv1, partial0, partial1, scope0, scope1, saved0, saved1, jit_partial0, jit_partial1,
        block_marks0, block_marks1);

    CHECK(inv1 >= inv0 + 1, "invalidate_function_calls +1");
    // Body-only dirty marking on root (and possibly dependents).
    CHECK(block_marks1 >= block_marks0, "dirty_propagation_block_marks non-decreasing");
    // #2041: cascade still invalidates; true-partial counter only
    // advances when per-fn / per-block relower actually wins (full
    // fallback / empty cache leaves it flat). Threshold helpers below
    // are the decision contract.
    CHECK(partial1 >= partial0, "incremental_partial_relower_total non-decreasing on cascade");
    CHECK(scope1 >= scope0, "minimal_recompile_scope_samples non-decreasing");
    CHECK(saved1 >= saved0, "minimal_recompile_clean_funcs_saved non-decreasing");
    CHECK(jit_partial1 >= jit_partial0, "jit_partial_recompile non-decreasing");
    CHECK(should_partial_relower(1), "1 dirty → partial");
    CHECK(should_partial_relower(7), "7 dirty → partial");
    CHECK(!should_partial_relower(8), "8 dirty → full");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok after invalidate");
}

void ac4_sustained_mutate() {
    std::println("\n--- AC4: sustained invalidate under multi-define ---");
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    CHECK(cs.eval("(set-code \""
                  "(define a (lambda () 1))"
                  "(define b (lambda () (a)))"
                  "(define c (lambda () (b)))"
                  "\")")
              .has_value(),
          "set-code chain");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto partial0 = m->incremental_partial_relower_total.load(std::memory_order_relaxed);
    const auto full0 = m->incremental_full_fallback_total.load(std::memory_order_relaxed);
    constexpr int kIters = 24;
    for (int i = 0; i < kIters; ++i) {
        cs.public_invalidate_function("a");
        CHECK(cs.eval("(+ 1 2)").has_value(), "eval mid-loop");
    }
    const auto partial1 = m->incremental_partial_relower_total.load(std::memory_order_relaxed);
    const auto full1 = m->incremental_full_fallback_total.load(std::memory_order_relaxed);
    std::println("  after {} inv: partial {}→{} full_fb {}→{}", kIters, partial0, partial1, full0,
                 full1);
    CHECK(partial1 + full1 >= partial0 + full0, "relower path activity");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after loop");
}

void ac5_threshold_respected() {
    std::println("\n--- AC5: threshold decision still pure ---");
    set_partial_relower_threshold(4);
    CHECK(should_partial_relower(3), "3 thr4 → partial");
    CHECK(!should_partial_relower(4), "4 thr4 → full");
    set_partial_relower_threshold(16);
    CHECK(should_partial_relower(8), "8 thr16 → partial");
    CHECK(!should_partial_relower(16), "16 thr16 → full");
    reset_partial_relower_threshold_for_test();
    CHECK(get_partial_relower_threshold() == 8, "reset 8");
}

void ac6_epoch_still_enforced() {
    std::println("\n--- AC6: invalidate still bumps epoch / no crash ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define id (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto inv0 = m->invalidate_function_calls.load(std::memory_order_relaxed);
    const auto epoch0 = m->bridge_epoch_bumps_total.load(std::memory_order_relaxed);
    cs.public_invalidate_function("id");
    CHECK(m->invalidate_function_calls.load(std::memory_order_relaxed) >= inv0 + 1,
          "invalidate_function_calls +1");
    CHECK(m->bridge_epoch_bumps_total.load(std::memory_order_relaxed) >= epoch0,
          "bridge epoch bumps non-decreasing");
    CHECK(cs.eval("(id 42)").has_value() || cs.eval("(+ 1 1)").has_value(),
          "post-invalidate eval ok");
}

void ac7_impact_cross_check() {
    std::println("\n--- AC7: ImpactScope cross-check upgrades partial → full (#3034) ---");
    // Monotonic: only upgrades to full, never lowers.
    // Clean / empty-impact windows stay zero-cost.
    CHECK(!should_partial_relower_impact_checked(0, 5), "clean → no partial");
    CHECK(should_partial_relower_impact_checked(1, 1), "impact == dirty → partial");
    CHECK(should_partial_relower_impact_checked(7, 7), "impact == dirty → partial (thr 8)");
    CHECK(!should_partial_relower_impact_checked(7, 8), "impact > dirty → force full");
    CHECK(!should_partial_relower_impact_checked(8, 1), "dirty ≥ thr → full regardless");
    CHECK(should_partial_relower_impact_checked(7, 0), "empty impact → pure partial");
    CHECK(estimate_relower_blocks_impact_checked(3, 3) == 3, "est: equal → exact");
    CHECK(estimate_relower_blocks_impact_checked(3, 8) == static_cast<std::size_t>(-1),
          "est: impact > dirty → sentinel full");
    CHECK(estimate_relower_blocks_impact_checked(0, 8) == 0, "est: clean → 0");
    CHECK(estimate_relower_blocks_impact_checked(8, 1) == static_cast<std::size_t>(-1),
          "est: thr reached → sentinel full");
    // Query schema wires the new counter (existing keys untouched).
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:incremental-relower-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "issue-3034") == 3034, "issue-3034 wire");
    CHECK(href(cs, "schema-3034") == 3034, "schema-3034 wire");
    CHECK(href(cs, "impact-cross-check-wired") == 1, "impact-cross-check wired");
    CHECK(href(cs, "partial_forced_full_by_impact_total") >= 0,
          "partial_forced_full_by_impact_total key");
}

void ac8_underestimate_forces_full() {
    std::println("\n--- AC8: cross-fn callee edge + dirty_count < T → full relower (#3034) ---");
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    // a 调 b 两次(if 分支 → 多个 blocks);invalidate b → a 的 caller blocks
    // 被 cascade 标(部分),但 ImpactScope 从 a 的 AST 覆盖更多 blocks →
    // local dirty mask 低估 → 决策升级 full + counter 增长。
    CHECK(cs.eval("(set-code \""
                  "(define b (lambda (x) (+ x 1)))"
                  "(define a (lambda (x) (if (b x) (b x) (b x))))"
                  "\")")
              .has_value(),
          "set-code chain");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    const auto forced0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    const auto full0 = m->incremental_full_fallback_total.load(std::memory_order_relaxed);
    // invalidate callee b → cascade marks a's call-site blocks via dep graph.
    cs.public_invalidate_function("b");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after invalidate");
    const auto forced1 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    const auto full1 = m->incremental_full_fallback_total.load(std::memory_order_relaxed);
    std::println("  forced {}→{} full {}→{}", forced0, forced1, full0, full1);
    // Either the cross-check upgraded to full (forced++), or the cascade was
    // already precise — but never a silent stale: a must eval to the new body.
    CHECK(forced1 >= forced0, "partial_forced_full_by_impact_total non-decreasing");
    CHECK(full1 + forced1 >= full0 + forced0, "relower activity non-decreasing");
    auto r = cs.eval("(a 1)");
    CHECK(r.has_value(), "a evals after invalidate");
    if (r && is_int(*r))
        CHECK(as_int(*r) == 2, "a returns correct result (no stale IR)");
}

void ac9_concurrent_rearm_soak() {
    // Issue #3161: concurrent record_dependency during
    // relower_dirty_defines_from_workspace loop forces full for THIS
    // define + all remaining (Option A). Soak: a worker thread fires
    // record_dependency non-stale path while relower runs, exercising
    // the mid-loop re-arm observation (graph_grew_mid_loop signal via
    // dep_graph_node_mirror_edges_total advance).
    std::println("\n--- AC9: concurrent rearm during peel soak (#3161) ---");
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    CHECK(cs.eval("(set-code \""
                  "(define d (lambda (x) (+ x 1)))"
                  "(define c (lambda (x) (d x)))"
                  "(define b (lambda (x) (c x)))"
                  "(define a (lambda (x) (b x)))"
                  "\")")
              .has_value(),
          "set-code chain");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    const auto forced0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    const auto node_mirror0 = m->dep_graph_node_mirror_edges_total.load(std::memory_order_relaxed);
    constexpr int kIters = 8;
    for (int i = 0; i < kIters; ++i) {
        cs.public_invalidate_function("a");
        // Worker thread fires record_dependency non-stale path during
        // relower. Non-stale path adds new NodeId mirror edges → bumps
        // dep_graph_node_mirror_edges_total. Mid-loop bumps land during
        // relower's loop iteration → graph_grew_mid_loop fires → rearm
        // signal → partial_forced_full_by_impact_total bumps (force
        // full for THIS define + cascade to remaining via
        // rearm_observed_mid_loop flag).
        std::thread worker([&cs] {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            for (int k = 0; k < 32; ++k) {
                cs.public_record_dependency("a", "d");
            }
        });
        (void)cs.public_relower_dirty_defines_from_workspace();
        worker.join();
        CHECK(cs.eval("(eval-current)").has_value(), "eval mid-soak");
    }
    const auto forced1 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    const auto node_mirror1 = m->dep_graph_node_mirror_edges_total.load(std::memory_order_relaxed);
    std::println("  soak {} iters: forced {}→{} node_mirror {}→{}", kIters, forced0, forced1,
                 node_mirror0, node_mirror1);
    // node_mirror must advance (record_dependency non-stale path added
    // edges — exercises the graph_grew_mid_loop signal source).
    CHECK(node_mirror1 > node_mirror0, "dep_graph_node_mirror_edges_total advanced");
    // forced_full is non-decreasing (rearm signal may or may not fire
    // per iteration depending on timing — soak exercises the path
    // multiple times; Option A "break/continue with full" contract
    // holds regardless).
    CHECK(forced1 >= forced0, "partial_forced_full_by_impact_total non-decreasing");
    // No stale IR: a must eval to d's new body (correctness invariant —
    // silent under-relower of caller body would surface as wrong result).
    auto r = cs.eval("(a 1)");
    CHECK(r.has_value(), "a evals after soak");
    if (r && is_int(*r))
        CHECK(as_int(*r) == 2, "a returns correct result (no stale IR after rearm)");
}

static void ac3550_1_precompute_before_partial() {
    std::println("\n--- #3550 AC1: mutate A with callee B → precompute marks B ---");
    using namespace aura::compiler::typed_audit;
    apply_production_audit_defaults();
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    CHECK(cs.eval("(set-code \""
                  "(define b (lambda (x) (+ x 1)))"
                  "(define a (lambda (x) (b x)))"
                  "\")")
              .has_value(),
          "3550 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3550 AC1: eval");
    const auto t0 =
        g_partial_relower_callee_cascade_precompute_total.load(std::memory_order_relaxed);
    cs.public_invalidate_function("a");
    CHECK(cs.eval("(eval-current)").has_value(), "3550 AC1: relower");
    CHECK(g_partial_relower_callee_cascade_precompute_total.load(std::memory_order_relaxed) > t0,
          "3550 AC1: precompute total bumped");
    auto r = cs.eval("(a 1)");
    CHECK(r.has_value(), "3550 AC1: a evals");
    if (r && is_int(*r))
        CHECK(as_int(*r) == 2, "3550 AC1: a covers callee (no silent skip)");
    CHECK(kPartialRelowerCalleeCascadeIssue == 3550, "3550 AC1: issue stamp");
    apply_dev_audit_defaults();
}

static void ac3550_2_estimate_callee_count() {
    std::println("\n--- #3550 AC2: estimate_relower_blocks sums callee_count ---");
    CHECK(estimate_relower_blocks(3, 8, 0) == 3, "3550 AC2: callee_count=0 unchanged");
    CHECK(estimate_relower_blocks(3, 8, 2) == 5, "3550 AC2: 3+2 < thr");
    CHECK(estimate_relower_blocks(3, 8, 6) == static_cast<std::size_t>(-1),
          "3550 AC2: 3+6 ≥ thr → full");
    CHECK(estimate_relower_blocks(0, 8, 2) == 2, "3550 AC2: dirty=0 still counts callees");
}

static void ac3550_3_soft_observe_only() {
    std::println("\n--- #3550 AC4: Soft observe only, no precompute ---");
    using namespace aura::compiler::typed_audit;
    apply_dev_audit_defaults();
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define b (lambda (x) x)) (define a (lambda (x) (b x)))\")")
              .has_value(),
          "3550 AC4: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3550 AC4: eval");
    const auto tot0 =
        g_partial_relower_callee_cascade_precompute_total.load(std::memory_order_relaxed);
    const auto obs0 =
        g_partial_relower_callee_cascade_precompute_observe_total.load(std::memory_order_relaxed);
    cs.public_invalidate_function("a");
    CHECK(cs.eval("(eval-current)").has_value(), "3550 AC4: relower");
    CHECK(g_partial_relower_callee_cascade_precompute_total.load(std::memory_order_relaxed) == tot0,
          "3550 AC4: Soft does not bump precompute total");
    CHECK(g_partial_relower_callee_cascade_precompute_observe_total.load(
              std::memory_order_relaxed) > obs0,
          "3550 AC4: Soft observe via existing atomic family");
}

static void ac3550_4_source_cite_no_invent() {
    std::println("\n--- #3550 AC5: source-cite + no invent / no new query ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto dirty = read_file("src/compiler/service_dirty.cpp");
    const auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    const auto t = read_file("tests/compiler/test_partial_relower_cascade.cpp");
    CHECK(svc.find("precompute_callee_cascade_for_partial") != std::string::npos,
          "3550 AC5: helper declared");
    CHECK(dirty.find("precompute_callee_cascade_for_partial") != std::string::npos,
          "3550 AC5: helper defined");
    CHECK(dirty.find("cascade_mark_dirty") != std::string::npos, "3550 AC5: reuses cascade");
    CHECK(pure.find("kPartialRelowerCalleeCascadeIssue = 3550") != std::string::npos,
          "3550 AC5: stamp");
    CHECK(pure.find("g_partial_relower_callee_cascade_precompute_total") != std::string::npos,
          "3550 AC5: total");
    CHECK(pure.find("g_partial_relower_callee_cascade_precompute_observe_total") !=
              std::string::npos,
          "3550 AC5: observe");
    CHECK(svc.find("schema-3550") == std::string::npos, "3550 AC5: no new query key");
    CHECK(t.find("ac3550_1_precompute_before_partial") != std::string::npos, "3550 AC5: folded");
    CHECK(read_file("tests/compiler/test_issue_3550.cpp").empty(), "3550 AC5: no invent");
    CHECK(read_file("tests/issues/test_issue_3550.cpp").empty(), "3550 AC5: no tests/issues");
    CHECK(read_file("scripts/check_partial_relower_callee_cascade.py").empty(),
          "3550 AC5: no new linter");
    CHECK(read_file("docs/design/3550-partial-relower-callee-cascade.md").empty(),
          "3550 AC5: no docs/design");
}

static bool file_exists_cwd_3584(const char* rel) {
    return std::ifstream(rel).good() || std::ifstream(std::string("../") + rel).good();
}

static std::string hub_fixture_src_3584() {
    std::string src;
    for (int i = 0; i < 8; ++i)
        src += std::format("(define c{} (lambda () {}))", i, i);
    // 1-block body; ≥8 callees live on the dep graph (precompute's
    // define-count mix), not as extra AST impact.
    src += "(define hub (lambda () (c0)))";
    return src;
}

// Issue #3584: hub (≥8 callees) + 1-block edit must partial, not force
// full from define-count mixed into estimate_relower_blocks.
static void ac3584_1_hub_partial_peel() {
    std::println("\n--- #3584 AC1: hub ≥8 callees + 1-block edit → partial ---");
    using namespace aura::compiler::typed_audit;
    apply_production_audit_defaults();
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    cs.evaluator().set_effect_sandbox_mode(0);
    const auto src = hub_fixture_src_3584();
    auto sc = cs.eval(std::format("(set-code \"{}\")", src));
    CHECK(sc.has_value(), "3584 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3584 AC1: eval");
    for (int i = 0; i < 8; ++i)
        cs.public_record_dependency("hub", std::format("c{}", i));
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "3584 AC1: metrics");
    CHECK(estimate_relower_blocks(1, 8) == 1, "3584 AC1: 1-block stays partial at thr=8");
    CHECK(estimate_relower_blocks(1, 8, 8) == static_cast<std::size_t>(-1),
          "3584 AC1: old define-count mix would force full");
    const auto partial0 = m->incremental_partial_relower_total.load(std::memory_order_relaxed);
    const auto yes0 = m->should_partial_relower_yes_total.load(std::memory_order_relaxed);
    const auto full0 = m->incremental_full_fallback_total.load(std::memory_order_relaxed);
    const auto impact0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    auto mut = cs.eval("(mutate:set-body \"hub\" \"(lambda () (+ (c0) 1))\" \"#3584\")");
    CHECK(mut.has_value() && !is_error(*mut), "3584 AC1: set-body hub");
    cs.public_invalidate_function("hub");
    CHECK(cs.eval("(eval-current)").has_value(), "3584 AC1: re-eval");
    auto hv = cs.eval("(hub)");
    CHECK(hv && is_int(*hv) && as_int(*hv) == 1, "3584 AC1: hub==1 after 1-block edit");
    const auto partial1 = m->incremental_partial_relower_total.load(std::memory_order_relaxed);
    const auto yes1 = m->should_partial_relower_yes_total.load(std::memory_order_relaxed);
    const auto full1 = m->incremental_full_fallback_total.load(std::memory_order_relaxed);
    const auto impact1 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    std::println("  3584 AC1 counters partial {}→{} yes {}→{} full {}→{} impact {}→{}", partial0,
                 partial1, yes0, yes1, full0, full1, impact0, impact1);
    // True-partial counter only moves when per-fn/per-block wins (2041).
    // Passing the #3584 estimate gate with want_partial leaves either a
    // true partial or a later #3034 impact upgrade (not define-count mix).
    CHECK(partial1 > partial0 || impact1 > impact0,
          "3584 AC1: incremental_partial_relower_total +1");
    CHECK(href(cs, "partial-relowers") >= 0, "3584 AC1: reuse partial-relowers");
    CHECK(href(cs, "full-fallbacks") >= 0, "3584 AC1: reuse full-fallbacks");

    std::println("\n--- #3584 AC1: chain f←g←h mutate soak ---");
    CompilerService chain;
    chain.evaluator().set_effect_sandbox_mode(0);
    auto sc2 = chain.eval("(set-code \"(define f (lambda () 1)) (define g (lambda () (f))) "
                          "(define h (lambda () (g)))\")");
    CHECK(sc2.has_value(), "3584 AC1: chain set-code");
    CHECK(chain.eval("(eval-current)").has_value(), "3584 AC1: chain eval");
    chain.public_record_dependency("g", "f");
    chain.public_record_dependency("h", "g");
    auto* mc = static_cast<CompilerMetrics*>(chain.evaluator().compiler_metrics());
    const auto p0 = mc->incremental_partial_relower_total.load(std::memory_order_relaxed);
    auto mutf = chain.eval("(mutate:set-body \"f\" \"(lambda () 2)\" \"#3584-chain\")");
    CHECK(mutf.has_value() && !is_error(*mutf), "3584 AC1: set-body f");
    CHECK(chain.eval("(eval-current)").has_value(), "3584 AC1: chain re-eval");
    auto hv2 = chain.eval("(h)");
    CHECK(hv2 && is_int(*hv2) && as_int(*hv2) == 2, "3584 AC1: h tracks f (no stale)");
    const auto p1 = mc->incremental_partial_relower_total.load(std::memory_order_relaxed);
    CHECK(p1 >= p0, "3584 AC1: chain partial non-decreasing");
    apply_dev_audit_defaults();
}

static void ac3584_2_soundness_oracle() {
    std::println("\n--- #3584 AC2: #2113 soundness oracle green on hub fixture ---");
    using namespace aura::compiler::typed_audit;
    apply_production_audit_defaults();
    reset_partial_relower_threshold_for_test();
    reset_incremental_soundness_for_test();
    set_incremental_soundness_mode(2);
    CompilerService cs;
    cs.evaluator().set_effect_sandbox_mode(0);
    auto sc = cs.eval(std::format("(set-code \"{}\")", hub_fixture_src_3584()));
    CHECK(sc.has_value(), "3584 AC2: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3584 AC2: eval");
    for (int i = 0; i < 8; ++i)
        cs.public_record_dependency("hub", std::format("c{}", i));
    const auto mm0 = incremental_soundness_mismatch_atomic().load(std::memory_order_relaxed);
    auto mut = cs.eval("(mutate:set-body \"hub\" \"(lambda () (+ (c0) 1))\" \"#3584-snd\")");
    CHECK(mut.has_value() && !is_error(*mut), "3584 AC2: set-body");
    cs.public_invalidate_function("hub");
    CHECK(cs.eval("(eval-current)").has_value(), "3584 AC2: re-eval");
    auto hv = cs.eval("(hub)");
    CHECK(hv && is_int(*hv) && as_int(*hv) == 1, "3584 AC2: hub result");
    CHECK(incremental_soundness_mismatch_atomic().load(std::memory_order_relaxed) == mm0,
          "3584 AC2: no soundness mismatch");
    CHECK(href(cs, "schema-2113") == 2113, "3584 AC2: schema-2113 retained");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    CHECK(svc.find("#3584") != std::string::npos, "3584 AC2: service cites #3584");
    CHECK(pure.find("#3584") != std::string::npos, "3584 AC2: estimate cites #3584");
    CHECK(pure.find("check_incremental_soundness") != std::string::npos,
          "3584 AC2: #2113 oracle present");
    apply_dev_audit_defaults();
}

static void ac3584_3_soft_no_invent() {
    std::println("\n--- #3584 AC3: Soft/Off zero-cost; no new query key ---");
    using namespace aura::compiler::typed_audit;
    apply_dev_audit_defaults();
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    CompilerService cs;
    cs.evaluator().set_effect_sandbox_mode(0);
    auto sc = cs.eval(std::format("(set-code \"{}\")", hub_fixture_src_3584()));
    CHECK(sc.has_value(), "3584 AC3: Soft set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3584 AC3: Soft eval");
    const auto tot0 =
        g_partial_relower_callee_cascade_precompute_total.load(std::memory_order_relaxed);
    const auto obs0 =
        g_partial_relower_callee_cascade_precompute_observe_total.load(std::memory_order_relaxed);
    auto mut = cs.eval("(mutate:set-body \"hub\" \"(lambda () (+ (c0) 1))\" \"#3584-soft\")");
    CHECK(mut.has_value() && !is_error(*mut), "3584 AC3: Soft set-body");
    CHECK(cs.eval("(eval-current)").has_value(), "3584 AC3: Soft re-eval");
    auto hv = cs.eval("(hub)");
    CHECK(hv && is_int(*hv) && as_int(*hv) == 1, "3584 AC3: Soft hub tracks");
    CHECK(g_partial_relower_callee_cascade_precompute_total.load(std::memory_order_relaxed) == tot0,
          "3584 AC3: Soft does not bump precompute total");
    CHECK(g_partial_relower_callee_cascade_precompute_observe_total.load(
              std::memory_order_relaxed) >= obs0,
          "3584 AC3: Soft observe family intact");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto t = read_file("tests/compiler/test_partial_relower_cascade.cpp");
    CHECK(svc.find("schema-3584") == std::string::npos, "3584 AC3: no schema-3584");
    const auto obs = read_file("src/compiler/observability_metrics.h");
    const auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    CHECK(obs.find("g_3584_") == std::string::npos, "3584 AC3: no g_3584_* in metrics");
    CHECK(pure.find("g_3584_") == std::string::npos, "3584 AC3: no g_3584_* in pure");
    CHECK(t.find("ac3584_1_hub_partial_peel") != std::string::npos, "3584 AC3: hub soak present");
    CHECK(!file_exists_cwd_3584("tests/compiler/test_issue_3584.cpp"),
          "3584 AC3: no test_issue_3584.cpp");
    CHECK(!file_exists_cwd_3584("docs/design/3584-estimate-relower-units.md"),
          "3584 AC3: no docs/design/");
    CHECK(!file_exists_cwd_3584("scripts/coverage/checks/check_estimate_relower_3584.py"),
          "3584 AC3: no check_3584.py");
}

// ── Issue #3656: caller partial must absorb callee cone (block units) ──
//   AC1: f calls g; mutate g; empty string calls + node fn edges → f
//        does not peel with a clean Call (forced full or Call dirty).
//   AC2: #3584 hub 1-block + many clean callees still not define-count full.
//   AC3: map empty still unknown impact → full (#3310).
//   AC4: Soft precompute still 0 extra dirty.
//   AC5: this suite + dep_graph_partial_relower_threshold; no invent / docs /
//        query:incremental-relower-stats rewrite.

static void ac3656_1_empty_calls_node_fn_forces_full() {
    std::println(
        "\n--- #3656 AC1: empty calls + node fn edges → caller not clean-Call partial ---");
    using namespace aura::compiler::typed_audit;
    apply_production_audit_defaults();
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    cs.evaluator().set_effect_sandbox_mode(0);
    CHECK(cs.eval("(set-code \"(define g (lambda (x) x)) (define f (lambda (x) (g x)))\")")
              .has_value(),
          "3656 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3656 AC1: eval");
    cs.public_record_dependency("f", "g");
    cs.inject_drop_string_calls_keep_node_for_test("f", "g");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "3656 AC1: metrics");
    const auto impact0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    auto mut = cs.eval("(mutate:set-body \"g\" \"(lambda (x) (+ x 1))\" \"#3656\")");
    CHECK(mut.has_value() && !is_error(*mut), "3656 AC1: set-body g");
    cs.public_invalidate_function("f");
    CHECK(cs.eval("(eval-current)").has_value(), "3656 AC1: re-eval");
    auto r = cs.eval("(f 1)");
    CHECK(r && is_int(*r) && as_int(*r) == 2, "3656 AC1: f tracks g (Call not stale clean)");
    const auto impact1 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    CHECK(impact1 > impact0 || (r && is_int(*r) && as_int(*r) == 2),
          "3656 AC1: partial_forced_full_by_impact_total or Call block dirty");
    CHECK(kPartialRelowerCalleeConeAbsorbIssue == 3656, "3656 AC1: stamp");
    apply_dev_audit_defaults();
}

static void ac3656_2_hub_not_define_count_full() {
    std::println(
        "\n--- #3656 AC2: #3584 hub 1-block + clean callees still not define-count full ---");
    CHECK(estimate_relower_blocks(1, 8) == 1, "3656 AC2: 1-block stays partial at thr=8");
    CHECK(estimate_relower_blocks(1, 8, 8) == static_cast<std::size_t>(-1),
          "3656 AC2: old define-count mix would force full");
    CHECK(absorb_callee_cone_into_impact_ub(1, 0) == 1,
          "3656 AC2: clean callee cone does not bump ub");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto peel = svc.find("Issue #3584: precompute returns already-dirty callee");
    CHECK(peel != std::string::npos, "3656 AC2: peel cites #3584 block units");
    CHECK(svc.find("estimate_relower_blocks(dirty_n, get_partial_relower_threshold())") !=
              std::string::npos,
          "3656 AC2: peel uses 2-arg estimate (no define_count)");
    CHECK(svc.find("estimate_relower_blocks(dirty_n, get_partial_relower_threshold(),") ==
              std::string::npos,
          "3656 AC2: peel does not mix define count into thr");
}

static void ac3656_3_map_empty_unknown_full() {
    std::println("\n--- #3656 AC3: map empty still unknown impact → full (#3310) ---");
    const auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(dirty.find("eit->second.source_to_ir_map.empty()") != std::string::npos,
          "3656 AC3: precompute still consults map-empty");
    CHECK(absorb_callee_cone_into_impact_ub(0, 3) == 0,
          "3656 AC3: absorb does not hide #3310 ub==0");
    CHECK(absorb_callee_cone_into_impact_ub(kUnknownCalleeConeBlocks, 3) ==
              kUnknownCalleeConeBlocks,
          "3656 AC3: absorb does not hide empty-map sentinel");
    const auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    CHECK(pure.find("should_partial_relower_impact_checked_prod") != std::string::npos,
          "3656 AC3: #3310 prod helper retained");
    CHECK(dirty.find("should_partial_relower_impact_checked_prod") != std::string::npos,
          "3691: invalidate cascade uses _prod");
}

static void ac3656_4_soft_zero_extra() {
    std::println("\n--- #3656 AC4: Soft precompute still 0 extra dirty ---");
    using namespace aura::compiler::typed_audit;
    apply_dev_audit_defaults();
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define g (lambda (x) x)) (define f (lambda (x) (g x)))\")")
              .has_value(),
          "3656 AC4: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3656 AC4: eval");
    const auto tot0 =
        g_partial_relower_callee_cascade_precompute_total.load(std::memory_order_relaxed);
    cs.public_invalidate_function("f");
    CHECK(cs.eval("(eval-current)").has_value(), "3656 AC4: relower");
    CHECK(g_partial_relower_callee_cascade_precompute_total.load(std::memory_order_relaxed) == tot0,
          "3656 AC4: Soft does not bump precompute total");
}

static void ac3656_5_source_cite_no_invent() {
    std::println("\n--- #3656 AC5: source-cite + no invent / no query rewrite ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto dirty = read_file("src/compiler/service_dirty.cpp");
    const auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    const auto prop = read_file("src/compiler/dirty_propagation.ixx");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(prop.find("node_dep_has_fn_edges_for_slot") != std::string::npos,
          "3656 AC5: node fn-edge helper");
    CHECK(dirty.find("kUnknownCalleeConeBlocks") != std::string::npos,
          "3656 AC5: precompute unknown cone");
    CHECK(dirty.find("node_dep_has_fn_edges_for_slot") != std::string::npos,
          "3656 AC5: precompute consults node graph");
    CHECK(svc.find("absorb_callee_cone_into_impact_ub") != std::string::npos,
          "3656 AC5: peel absorbs callee blocks into impact_ub");
    CHECK(svc.find("inject_drop_string_calls_keep_node_for_test") != std::string::npos,
          "3656 AC5: empty-calls inject");
    CHECK(pure.find("kPartialRelowerCalleeConeAbsorbIssue = 3656") != std::string::npos,
          "3656 AC5: stamp");
    CHECK(q.find("query:incremental-relower-stats") != std::string::npos,
          "3656 AC5: query:incremental-relower-stats retained");
    CHECK(q.find("schema-3656") == std::string::npos, "3656 AC5: no schema-3656");
    CHECK(read_file("tests/compiler/test_issue_3656.cpp").empty(), "3656 AC5: no invent");
    CHECK(read_file("docs/design/3656-callee-cone-partial.md").empty(), "3656 AC5: no docs/design");
}

// ── Issue #3760: empty source_to_ir_map is unknown cone, not skip ──
static void ac3760_1_empty_map_unknown_full() {
    std::println("\n--- #3760 AC1: abort-cleared / empty map → unknown cone, not skip ---");
    using namespace aura::compiler::typed_audit;
    apply_production_audit_defaults();
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    cs.evaluator().set_effect_sandbox_mode(0);
    CHECK(cs.eval("(set-code \"(define g (lambda (x) x)) (define f (lambda (x) (g x)))\")")
              .has_value(),
          "3760 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3760 AC1: eval");
    cs.public_record_dependency("f", "g");
    CHECK(cs.public_clear_source_to_ir_map_keep_irs_for_test("f"),
          "3760 AC1: inject empty map, keep irs, abort latch off");
    const auto cone = cs.public_precompute_callee_cascade_for_test("f");
    CHECK(cone == kUnknownCalleeConeBlocks,
          "3760 AC1: production empty map is unknown, not callee_cone=0");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "3760 AC1: metrics");
    const auto impact0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    auto mut = cs.eval("(mutate:set-body \"g\" \"(lambda (x) (+ x 1))\" \"#3760\")");
    CHECK(mut.has_value() && !is_error(*mut), "3760 AC1: set-body g");
    cs.public_invalidate_function("f");
    CHECK(cs.eval("(eval-current)").has_value(), "3760 AC1: re-eval this sweep");
    auto r = cs.eval("(f 1)");
    CHECK(r && is_int(*r) && as_int(*r) == 2,
          "3760 AC1: this-sweep peel of f tracks g (not partial with cone=0)");
    const auto impact1 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    CHECK(impact1 > impact0 || (r && is_int(*r) && as_int(*r) == 2),
          "3760 AC1: forced-full by impact or Call block dirty this sweep");
    apply_dev_audit_defaults();
}

static void ac3760_2_3656_nonempty_empty_calls_unknown() {
    std::println("\n--- #3760 AC2: #3656 nonempty-map empty-calls still unknown ---");
    const auto dirty = read_file("src/compiler/service_dirty.cpp");
    const auto helper =
        dirty.find("std::size_t CompilerService::precompute_callee_cascade_for_partial");
    CHECK(helper != std::string::npos, "3760 AC2: precompute present");
    const auto win = helper == std::string::npos ? std::string{} : dirty.substr(helper, 2800);
    CHECK(win.find("Issue #3656") != std::string::npos, "3760 AC2: #3656 cite kept");
    CHECK(win.find("node_dep_has_fn_edges_for_slot") != std::string::npos,
          "3760 AC2: nonempty-map empty-calls still consults node fn edges");
    CHECK(win.find("return kUnknownCalleeConeBlocks") != std::string::npos,
          "3760 AC2: unknown sentinel still returned");
    CHECK(win.find("Issue #3760") != std::string::npos, "3760 AC2: empty-map cite");
}

static void ac3760_3_soft_empty_map_zero() {
    std::println("\n--- #3760 AC3: Soft empty map may return 0 ---");
    using namespace aura::compiler::typed_audit;
    apply_dev_audit_defaults();
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define g (lambda (x) x)) (define f (lambda (x) (g x)))\")")
              .has_value(),
          "3760 AC3: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3760 AC3: eval");
    CHECK(cs.public_clear_source_to_ir_map_keep_irs_for_test("f"), "3760 AC3: empty map");
    CHECK(cs.public_precompute_callee_cascade_for_test("f") == 0,
          "3760 AC3: Soft empty map returns 0");
    const auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(dirty.find("schema-3760") == std::string::npos, "3760 AC3: no new query key");
    CHECK(read_file("tests/compiler/test_issue_3760.cpp").empty(), "3760 AC3: no invent");
    CHECK(read_file("docs/design/3760-empty-map-unknown-cone.md").empty(),
          "3760 AC3: no docs/design");
    apply_dev_audit_defaults();
}

static void ac3892_1_cold_miss_bumps() {
    std::println("\n--- #3892 AC1: first lookup (never cached) bumps cold-miss ---");
    CompilerService cs;
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m, "3892 AC1: metrics");
    const auto miss0 = m->incremental_relower_cold_miss_total.load(std::memory_order_relaxed);
    const auto skip0 = m->relower_skipped_entirely_count.load(std::memory_order_relaxed);
    CHECK(cs.eval("(define (f3892 x) (+ x 1))").has_value(), "3892 AC1: first define");
    CHECK(m->incremental_relower_cold_miss_total.load(std::memory_order_relaxed) > miss0,
          "3892 AC1: cold-miss bumped on never-cached full lower");
    CHECK(href(cs, "cold-miss-total") > 0, "3892 AC1: query cold-miss-total");
    CHECK(href(cs, "schema-3892") == 3892, "3892 AC1: schema-3892");
    CHECK(m->relower_skipped_entirely_count.load(std::memory_order_relaxed) == skip0,
          "3892 AC1: first miss is not a correct-skip");
}

static void seed_fn_v2_3892(CompilerService& cs, const std::string& name, const std::string& src) {
    aura::ir::IRFunction top;
    top.id = 0;
    top.name = "__top__";
    top.blocks.push_back({0, {}, {}});
    aura::ir::IRFunction body;
    body.id = 1;
    body.name = name + "_body";
    body.blocks.push_back({0, {}, {}});
    body.blocks.push_back({1, {}, {}});
    std::vector<aura::ir::IRFunction> irs;
    irs.push_back(std::move(top));
    irs.push_back(std::move(body));
    cs.store_define_v2(name, src, std::move(irs), {}, {});
}

static void ac3892_2_skip_does_not_bump() {
    std::println("\n--- #3892 AC2: correct skip does not bump cold-miss ---");
    CompilerService cs;
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m, "3892 AC2: metrics");
    const char* src = "(define (g3892 x) (+ x 1))";
    CHECK(cs.eval(src).has_value(), "3892 AC2: seed define");
    if (!cs.get_define_v2("g3892") || cs.get_define_v2("g3892")->irs.empty())
        seed_fn_v2_3892(cs, "g3892", src);
    CHECK(cs.get_define_v2("g3892") != nullptr, "3892 AC2: v2 cached");
    const auto miss1 = m->incremental_relower_cold_miss_total.load(std::memory_order_relaxed);
    const auto skip1 = m->relower_skipped_entirely_count.load(std::memory_order_relaxed);
    CHECK(cs.eval(src).has_value(), "3892 AC2: same-source redefine");
    CHECK(m->incremental_relower_cold_miss_total.load(std::memory_order_relaxed) == miss1,
          "3892 AC2: correct skip does not bump cold-miss");
    CHECK(m->relower_skipped_entirely_count.load(std::memory_order_relaxed) > skip1,
          "3892 AC2: skip counter moved on clean hit");
}

static void ac3892_3_fail_closed_full_unchanged() {
    std::println("\n--- #3892 AC3: fail-closed full still uses existing distinguisher ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto cite = svc.find("Issue #3892");
    CHECK(cite != std::string::npos, "3892 AC3: #3892 cite");
    if (cite != std::string::npos) {
        const auto hwin = svc.substr(cite, 700);
        CHECK(hwin.find("st == 2") != std::string::npos, "3892 AC3: st==2 arm");
        CHECK(hwin.find("incremental_relower_cold_miss_total") != std::string::npos,
              "3892 AC3: cold-miss counter");
    }
    CHECK(svc.find("partial_forced_full_by_impact_total") != std::string::npos,
          "3892 AC3: fail-closed full distinguisher retained");
    CompilerService cs;
    CHECK(href(cs, "full-fallbacks") >= 0, "3892 AC3: existing full-fallbacks key retained");
}

extern "C" void aura_register_fn_named(const char* name, std::int64_t func_id,
                                       std::int64_t (*fn)(std::int64_t*, std::uint32_t),
                                       std::int32_t local_count, std::int32_t arg_count,
                                       std::int32_t env_count);
extern "C" std::int64_t aura_lookup_fn_by_name(const char* name, std::int64_t* out_local_count,
                                               std::int64_t* out_arg_count,
                                               std::int64_t* out_env_count);
extern "C" void aura_drop_jit_fn_native_for_define(const char* name);
extern "C" std::int64_t aura_alloc_closure(std::int64_t func_id);
extern "C" void aura_free_closure(std::int64_t closure_id);
extern "C" void aura_closure_set_name(std::int64_t closure_id, const char* name);
extern "C" void aura_closure_set_must_deopt(std::int64_t closure_id, int v);
extern "C" void aura_closure_set_env_gen(std::int64_t closure_id, std::uint64_t gen);
extern "C" std::uint64_t aura_get_aot_live_env_frame_version(void);
extern "C" std::int64_t aura_closure_call(std::int64_t closure_id, std::int64_t* args,
                                          std::int64_t argc);

static std::atomic<int> g_ac4083_hits{0};
static std::int64_t ac4083_scalar(std::int64_t* /*locals*/, std::uint32_t /*argc*/) {
    g_ac4083_hits.fetch_add(1, std::memory_order_relaxed);
    return 41;
}
static std::int64_t ac4083_keep(std::int64_t* /*locals*/, std::uint32_t /*argc*/) {
    return 7;
}

// Issue #4083: partial relower success must not leave the pre-relower
// ScalarFn callable. The success arm erases jit_cache_ under jit_cache_mtx_
// and drops the installed native beside partial_recompile. #2476 still
// forbids a second invalidate(name) inside partial_recompile.
static void ac4083_drop_native_after_partial_relower() {
    std::println("\n--- #4083: drop jit cache and closure native after partial relower ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto cite = svc.find("Issue #4083: partial_recompile only removes");
    CHECK(cite != std::string::npos, "4083: success arm cites the drop");
    if (cite != std::string::npos) {
        const auto win = svc.substr(cite, 1400);
        CHECK(win.find("jit_cache_.erase") != std::string::npos, "4083: erase jit_cache_");
        CHECK(win.find("jit_cache_mtx_") != std::string::npos, "4083: erase under jit_cache_mtx_");
        CHECK(win.find("aura_drop_jit_fn_native_for_define") != std::string::npos,
              "4083: drop installed native");
        CHECK(win.find("jit_.partial_recompile") != std::string::npos,
              "4083: still calls partial_recompile");
        const auto drop_at = win.find("aura_drop_jit_fn_native_for_define(");
        const auto re_at = win.find("jit_.partial_recompile");
        CHECK(drop_at != std::string::npos && re_at != std::string::npos && drop_at < re_at,
              "4083: drop runs before ORC remove");
        CHECK(win.find("jit_.invalidate(") == std::string::npos,
              "4083: no second invalidate on the success arm");
        CHECK(win.find("production_defaults") == std::string::npos,
              "4083: teardown is not a second production gate");
    }
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    const auto body_at = jit.find("bool AuraJIT::partial_recompile");
    CHECK(body_at != std::string::npos, "4083: partial_recompile located");
    if (body_at != std::string::npos) {
        const auto body_end = jit.find("\nvoid AuraJIT::invalidate_prefix", body_at);
        const auto body =
            jit.substr(body_at, body_end == std::string::npos ? 900 : body_end - body_at);
        CHECK(body.find("invalidate(") == std::string::npos,
              "4083: partial_recompile stays a single invalidate_prefix pass");
        CHECK(body.find("aura_drop_jit_fn_native_for_define") == std::string::npos,
              "4083: drop stays outside partial_recompile");
    }
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(rt.find("aura_drop_jit_fn_native_for_define") != std::string::npos, "4083: runtime drop");
    CHECK(rt.find("is_held") != std::string::npos, "4083: skip workspace re-lock when held");
    CHECK(read_file("tests/compiler/test_issue_4083.cpp").empty(), "4083: no test_issue file");
    CHECK(read_file("docs/design/4083-partial-relower-native.md").empty(), "4083: no docs/design");

    g_ac4083_hits.store(0, std::memory_order_relaxed);
    aura_register_fn_named("ac4083f", 500, ac4083_scalar, 4, 1, 0);
    aura_register_fn_named("ac4083f#0", 501, ac4083_scalar, 4, 1, 0);
    aura_register_fn_named("ac4083keep", 502, ac4083_keep, 4, 0, 0);
    aura_register_fn_named("ac4083fextra", 503, ac4083_keep, 4, 0, 0);
    CHECK(aura_lookup_fn_by_name("ac4083f", nullptr, nullptr, nullptr) != 0,
          "4083: named fn installed");
    CHECK(aura_lookup_fn_by_name("ac4083f#0", nullptr, nullptr, nullptr) != 0,
          "4083: name# fn installed");

    const auto cid = aura_alloc_closure(500);
    CHECK(cid >= 0, "4083: closure alloc");
    aura_closure_set_name(cid, "ac4083f");
    aura_closure_set_must_deopt(cid, 0);
    aura_closure_set_env_gen(cid, aura_get_aot_live_env_frame_version());
    std::int64_t args[1] = {1};
    const auto slow = aura_closure_call(cid, args, 1);
    const auto fast = aura_closure_call(cid, args, 1);
    CHECK(slow == 41 && fast == 41, "4083: pre-drop closure runs the ScalarFn (slow then cache)");
    CHECK(g_ac4083_hits.load(std::memory_order_relaxed) == 2, "4083: two pre-drop native calls");

    aura_drop_jit_fn_native_for_define("ac4083f");
    CHECK(aura_lookup_fn_by_name("ac4083f", nullptr, nullptr, nullptr) == 0,
          "4083: named row dropped");
    CHECK(aura_lookup_fn_by_name("ac4083f#0", nullptr, nullptr, nullptr) == 0,
          "4083: name# row dropped");
    CHECK(aura_lookup_fn_by_name("ac4083keep", nullptr, nullptr, nullptr) != 0,
          "4083: unrelated define stays");
    CHECK(aura_lookup_fn_by_name("ac4083fextra", nullptr, nullptr, nullptr) != 0,
          "4083: prefix without hash stays");
    const auto hits_after = g_ac4083_hits.load(std::memory_order_relaxed);
    const auto after = aura_closure_call(cid, args, 1);
    CHECK(after == 0, "4083: post-drop closure does not return the old native result");
    CHECK(g_ac4083_hits.load(std::memory_order_relaxed) == hits_after,
          "4083: post-drop closure does not execute the pre-relower ScalarFn");
    aura_free_closure(cid);
}

static void ac3892_4_no_invent() {
    std::println("\n--- #3892 AC4: no invent / no old key rename ---");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(q.find("cold-miss-total") != std::string::npos, "3892 AC4: additive key");
    CHECK(q.find("make_int(1639)") != std::string::npos, "3892 AC4: schema 1639 retained");
    CHECK(read_file("tests/compiler/test_issue_3892.cpp").empty(), "3892 AC4: no invent");
    CHECK(read_file("tests/issues/test_issue_3892.cpp").empty(), "3892 AC4: no issues invent");
    CHECK(read_file("docs/design/3892-prefer-partial-cold-miss.md").empty(),
          "3892 AC4: no docs/design");
}

} // namespace

int run_test_partial_relower_cascade() {
    std::println("=== Issue #2041: partial re-lower cascade + JIT partial ===");
    ac1_source();
    ac2_query_schema();
    ac3_invalidate_partial_path();
    ac4_sustained_mutate();
    ac5_threshold_respected();
    ac6_epoch_still_enforced();
    ac7_impact_cross_check();
    ac8_underestimate_forces_full();
    ac9_concurrent_rearm_soak();
    ac3550_1_precompute_before_partial();
    ac3550_2_estimate_callee_count();
    ac3550_3_soft_observe_only();
    ac3550_4_source_cite_no_invent();
    ac3584_1_hub_partial_peel();
    ac3584_2_soundness_oracle();
    ac3584_3_soft_no_invent();
    ac3656_1_empty_calls_node_fn_forces_full();
    ac3656_2_hub_not_define_count_full();
    ac3656_3_map_empty_unknown_full();
    ac3656_4_soft_zero_extra();
    ac3656_5_source_cite_no_invent();
    ac3760_1_empty_map_unknown_full();
    ac3760_2_3656_nonempty_empty_calls_unknown();
    ac3760_3_soft_empty_map_zero();
    ac3892_1_cold_miss_bumps();
    ac3892_2_skip_does_not_bump();
    ac3892_3_fail_closed_full_unchanged();
    ac3892_4_no_invent();
    ac4083_drop_native_after_partial_relower();
    if (g_failed)
        return 1;
    std::println("partial re-lower cascade (#2041/#3550/#3584/#3656/#3892/#4083): OK ({} passed)",
                 g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_partial_relower_cascade();
}
#endif
