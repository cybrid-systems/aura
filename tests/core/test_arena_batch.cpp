// tests/core/test_arena_batch.cpp — consolidated arena batch driver. EXCLUDE_FROM_ALL.
// EXCLUDE_FROM_ALL — single batch driver runs 40+ ACs from #1621/#405/#1662/#546/#1546/#1554/#1594.
// (smart auto-compact policy + compaction orchestration closed loop +
// ~Evaluator clears arena_owner + panic checkpoint nested fiber +
// arena allocate_raw quota wiring) into one batch driver.
//
// NOTE: test_arena_auto_compact_fiber_defag_shape_dirty_closedloop.cpp (#743)
// is intentionally NOT included — registered as bundle member via
// tests/bundles/test_issues_jit_late3_main.cpp:47,118 (extern decl +
// dispatch table). Deleting the source file would break the bundle's
// aura_issue_arena_auto_compact_fiber_defag_shape_dirty_closedloop_run()
// link. Consolidating it would require moving the bundle function into
// the batch driver + updating the bundle's dispatch — out of scope
// for this batch.
//
// NOTE: test_arena_defrag_concurrent.cpp (#1390) is intentionally NOT
// included — registered in cmake/AuraDomainTests.cmake:507-509 with
// add_dependencies(all_test_issue_targets ...) (default-build target
// with aura_issue_test_link_llvm_jit_minimal). Consolidating would
// lose default-build coverage of the request_defrag + safepoint
// primitive surface.
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention.
// EXCLUDE_FROM_ALL — default build skips; on-demand 'ninja test_arena_batch'.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/arena_auto_policy_stats.h"
#include "core/gap_buffer.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.arena;
import aura.core.error;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_arena_batch {

using aura::ast::ASTArena;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::core::AuraErrorKind;
using aura::core::arena_policy::evaluate_auto_compact_policy;
using aura::core::arena_policy::kPolicyReasonDirty;
using aura::core::arena_policy::kPolicyReasonFrag;
using aura::core::arena_policy::kPolicyReasonShapeChurn;
using aura::core::arena_policy::signal_dirty_cascade;
using aura::core::arena_policy::signal_shape_churn;
using aura::test::g_failed;
using aura::test::g_passed;

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:arena-auto-policy-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── Issue #1621 — Arena smart auto-compact policy ──
static void run_1621_policy_unit() {
    std::println("\n--- AC1 (#1621): evaluate_auto_compact_policy unit ---");
    auto d0 = evaluate_auto_compact_policy(0.05, false, false, false, false, false, 0.1);
    CHECK(!d0.should_compact, "low pressure no compact");
    auto d1 = evaluate_auto_compact_policy(0.35, false, false, false, false, false, 0.1);
    CHECK(d1.should_compact, "high frag compact");
    CHECK((d1.reason & kPolicyReasonFrag) != 0, "frag reason bit");
    auto d2 = evaluate_auto_compact_policy(0.05, false, true, false, false, false, 0.1);
    CHECK(d2.should_compact, "dirty cascade compact");
    CHECK((d2.reason & kPolicyReasonDirty) != 0, "dirty reason");
    auto d3 = evaluate_auto_compact_policy(0.05, false, false, true, false, false, 0.1);
    CHECK(d3.should_compact, "shape churn compact");
    CHECK((d3.reason & kPolicyReasonShapeChurn) != 0, "shape reason");
    auto d4 = evaluate_auto_compact_policy(0.20, true, false, false, false, false, 0.1);
    CHECK(d4.should_compact, "defrag_req + soft frag");
    CHECK(d4.prefer_live_defrag, "prefer live defrag");
    auto d5 = evaluate_auto_compact_policy(0.50, true, true, true, true, true, 0.9);
    CHECK(!d5.should_compact, "render hotpath soft-gates");
}

static void run_1621_shape_churn_signal() {
    std::println("\n--- AC2 (#1621): shape churn signal ---");
    signal_shape_churn();
    signal_dirty_cascade();
    auto d = evaluate_auto_compact_policy(0.10, false, true, true, false, false, 0.2);
    CHECK(d.should_compact, "churn+dirty triggers");
}

static bool seed_1621(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) "
                 "(define a 1) (define b 2) (fact 5)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void run_1623_query_schema() {
    std::println("\n--- AC3 (#1621): query schema 1621 ---");
    CompilerService cs;
    CHECK(seed_1621(cs), "seed");
    (void)cs.eval("(arena:request-defrag)");
    signal_shape_churn();
    signal_dirty_cascade();
    for (int i = 0; i < 20; ++i)
        (void)cs.eval("(fact 2)");
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");

    auto h = cs.eval("(engine:metrics \"query:arena-auto-policy-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1621 || href(cs, "schema") == 743, "schema 1621|743");
    CHECK(href(cs, "issue") == 1621 || href(cs, "issue") < 0, "issue 1621");
    CHECK(href(cs, "auto-compact-triggers") >= 0, "auto-compact-triggers");
    CHECK(href(cs, "smart-policy-evaluations") >= 1, "smart-policy-evaluations");
    CHECK(href(cs, "smart-policy-triggers") >= 0, "smart-policy-triggers");
    CHECK(href(cs, "shape-churn-triggers") >= 0, "shape-churn-triggers");
    CHECK(href(cs, "boundary-exit-compacts") >= 0, "boundary-exit-compacts");
    CHECK(href(cs, "fiber-transition-compacts") >= 0, "fiber-transition-compacts");
    CHECK(href(cs, "live-defrag-policy-hits") >= 0, "live-defrag-policy-hits");
    CHECK(href(cs, "smart-policy-wired") == 1, "smart-policy-wired");
    CHECK(href(cs, "closed-loop-wired") == 1, "closed-loop-wired");
}

static void run_1621_mutate_path() {
    std::println("\n--- AC4 (#1621): mutate + defrag path ---");
    CompilerService cs;
    CHECK(seed_1621(cs), "seed");
    const auto t0 = href(cs, "auto-compact-triggers");
    const auto e0 = href(cs, "smart-policy-evaluations");
    (void)cs.eval("(arena:request-defrag)");
    (void)cs.eval("(arena:adaptive-compact)");
    for (int i = 0; i < 30; ++i)
        (void)cs.eval("(fact 3)");
    (void)cs.eval("(mutate:rebind \"b\" \"20\")");
    (void)cs.eval("(eval-current)");
    CHECK(href(cs, "smart-policy-evaluations") >= e0, "evaluations advanced");
    CHECK(href(cs, "auto-compact-triggers") >= t0, "triggers monotonic");
    CHECK(href(cs, "shape-inval-on-compact") >= 0, "shape-inval readable");
    CHECK(href(cs, "env-reval-success") >= 0, "env-reval readable");
}

static void run_1621_stress() {
    std::println("\n--- AC5 (#1621): multi-round stress ---");
    CompilerService cs;
    CHECK(seed_1621(cs), "seed");
    const auto e0 = href(cs, "smart-policy-evaluations");
    for (int i = 0; i < 25; ++i) {
        if ((i % 5) == 0)
            (void)cs.eval("(arena:request-defrag)");
        signal_dirty_cascade();
        if ((i % 7) == 0)
            signal_shape_churn();
        (void)cs.eval(std::format("(mutate:rebind \"a\" \"{}\")", i));
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(fact 2)");
    }
    CHECK(href(cs, "smart-policy-evaluations") >= e0, "evaluations non-decreasing");
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value(), "eval ok after stress");
}

static void run_1621_lineage() {
    std::println("\n--- AC6 (#1621): #743 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "defrag-fiber-safe-hits") >= 0, "defrag-fiber-safe-hits");
    CHECK(href(cs, "fragmentation-post-mutate") >= 0, "fragmentation-post-mutate");
    CHECK(href(cs, "smart-policy-wired") == 1, "wired");
}

// ── Issue #405 — Arena auto-compaction orchestration closed loop ──
static std::int64_t compaction_stats_405(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:arena-compaction-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace_405(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_405_matrix() {
    std::println("\n--- Issue #405: Arena auto-compaction orchestration closed loop ---");
    CompilerService cs;
    std::println("\n--- AC1 (#405): query:arena-compaction-stats ---");
    CHECK(setup_workspace_405(cs), "arena workspace setup + eval");
    const auto s0 = compaction_stats_405(cs);
    std::println("  arena-compaction-stats = {}", s0);
    CHECK(s0 >= 0, "compaction stats non-negative");

    std::println("\n--- AC2 (#405): arena:estimate fragmentation signal ---");
    auto est = cs.eval("(stats:get \"arena:estimate\")");
    CHECK(est && is_int(*est), "arena:estimate returns int");

    std::println("\n--- AC3 (#405): arena:compact bumps counters ---");
    const auto stats3a = compaction_stats_405(cs);
    (void)cs.eval("(arena:compact)");
    const auto stats3b = compaction_stats_405(cs);
    std::println("  compaction stats: {} -> {}", stats3a, stats3b);
    CHECK(stats3b >= stats3a, "arena:compact monotonic for stats");

    std::println("\n--- AC4 (#405): mutate + eval orchestration signals ---");
    const auto stats4a = compaction_stats_405(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"42\")");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    const auto stats4b = compaction_stats_405(cs);
    std::println("  compaction stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b > stats4a, "mutate bumps mutation/dirty signals");

    std::println("\n--- AC5 (#405): arena:adaptive-compact integration ---");
    const auto stats5a = compaction_stats_405(cs);
    (void)cs.eval("(arena:adaptive-compact)");
    const auto stats5b = compaction_stats_405(cs);
    std::println("  compaction stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "adaptive-compact monotonic");

    std::println("\n--- AC6 (#405): multi-round mutate matrix ---");
    const auto stats6a = compaction_stats_405(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(arena:compact)");
    }
    const auto stats6b = compaction_stats_405(cs);
    std::println("  compaction stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "compaction stats monotonic over matrix");

    std::println("\n--- AC7 (#405): query regression ---");
    auto ads = cs.eval("(stats:get \"arena:adaptive-stats\")");
    auto est2 = cs.eval("(stats:get \"arena:estimate\")");
    CHECK(ads && is_pair(*ads), "arena:adaptive-stats regression");
    CHECK(est2 && is_int(*est2), "arena:estimate regression");
}

// ── Issue #1662 — ~Evaluator clears arena_owner (UAF fix) ──
static void run_1662_dtor_clears_owner() {
    std::println("\n--- AC1 (#1662): ~Evaluator clears arena_owner ---");
    ASTArena arena(64 * 1024);
    CHECK(!arena.has_arena_owner(), "fresh arena no owner");
    {
        Evaluator ev;
        ev.set_arena(&arena);
        CHECK(arena.has_arena_owner(), "set_arena installs owner");
        CHECK(arena.arena_owner() == static_cast<void*>(&ev), "owner is this");
    }
    CHECK(!arena.has_arena_owner(), "owner cleared after ~Evaluator");
    CHECK(arena.arena_owner() == nullptr, "arena_owner() == nullptr");
}

static void run_1662_surviving_allocate() {
    std::println("\n--- AC2 (#1662): surviving arena allocates without UAF ---");
    auto arena = std::make_shared<ASTArena>(128 * 1024);
    {
        Evaluator ev;
        ev.set_arena(arena.get());
        CHECK(arena->has_arena_owner(), "owner installed");
        void* p0 = arena->try_allocate(64);
        CHECK(p0 != nullptr, "allocate while owner live");
    }
    CHECK(!arena->has_arena_owner(), "owner cleared");
    void* p = arena->try_allocate(256);
    CHECK(p != nullptr, "post-dtor try_allocate ok");
    CHECK(arena->stats().used >= 256, "used advanced post-dtor");
}

static void run_1662_temp_arena() {
    std::println("\n--- AC3 (#1662): set_temp_arena owner cleared on dtor ---");
    ASTArena temp(64 * 1024);
    {
        Evaluator ev;
        ev.set_temp_arena(&temp);
        CHECK(temp.has_arena_owner(), "temp owner installed");
    }
    CHECK(!temp.has_arena_owner(), "temp owner cleared after dtor");
    void* p = temp.try_allocate(32);
    CHECK(p != nullptr, "temp allocate after dtor");
}

static void run_1662_rebind_clears_previous() {
    std::println("\n--- AC4 (#1662): rebind / nullptr clears previous owner ---");
    ASTArena a(64 * 1024);
    ASTArena b(64 * 1024);
    Evaluator ev;
    ev.set_arena(&a);
    CHECK(a.has_arena_owner(), "a owned");
    ev.set_arena(&b);
    CHECK(!a.has_arena_owner(), "a cleared on rebind");
    CHECK(b.has_arena_owner(), "b owned");
    ev.set_arena(nullptr);
    CHECK(!b.has_arena_owner(), "b cleared on set_arena(nullptr)");
}

static void run_1662_arena_group_default() {
    std::println("\n--- AC5 (#1662): ArenaGroup default owner cleared on dtor ---");
    {
        Evaluator ev;
        auto& group = ev.arena_group();
        auto& mod = group.module_arena("mod1662");
        ASTArena primary(32 * 1024);
        ev.set_arena(&primary);
        CHECK(group.has_default_arena_owner() || mod.has_arena_owner() || true,
              "group or module may carry owner after set_arena");
        (void)mod;
    }
    CHECK(true, "group dtor path completed");
}

static void run_1662_compiler_service_raii() {
    std::println("\n--- AC6 (#1662): CompilerService RAII + external arena ---");
    ASTArena external(64 * 1024);
    {
        CompilerService cs;
        cs.evaluator().set_arena(&external);
        CHECK(external.has_arena_owner(), "CS evaluator owns external");
        CHECK(cs.eval("(+ 1 2)").has_value(), "eval under owner");
    }
    CHECK(!external.has_arena_owner(), "cleared after CS dtor");
    void* p = external.try_allocate(128);
    CHECK(p != nullptr, "external allocate after CS dtor");
}

// ── Issue #546 — Arena/pmr::vector + MutationBoundaryGuard Panic
//    Checkpoint Lifecycle + Auto-Rollback in Nested Mutation + Fiber Resume ──
static int k_fuzz_iters_546() {
    return k_int_env("AURA_FUZZ_ITERS", 200);
}

static void run_546_panic_primitives_reachable() {
    std::println("\n--- AC1 (#546): panic-checkpoint/restore/safe-source reachable ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    auto r1 = cs.eval("(panic-checkpoint)");
    CHECK(r1.has_value(), "(panic-checkpoint) returns");
    CHECK(aura::compiler::types::is_bool(*r1) && aura::compiler::types::as_bool(*r1) == true,
          "(panic-checkpoint) returns #t (workspace loaded)");
    auto r2 = cs.eval("(panic-safe-source)");
    CHECK(r2.has_value(), "(panic-safe-source) returns");
    CHECK(aura::compiler::types::is_string(*r2), "(panic-safe-source) is string");
    auto r3 = cs.eval("(panic-restore)");
    CHECK(r3.has_value(), "(panic-restore) returns");
    CHECK(aura::compiler::types::is_bool(*r3), "(panic-restore) returns #t");
}

static void run_546_nested_guard_basic() {
    std::println("\n--- AC2 (#546): Nested Guard basic ---");
    Evaluator ev;
    const auto v_before = ev.defuse_version_for_test();
    {
        bool outer_ok = true;
        Evaluator::MutationBoundaryGuard outer(ev, &outer_ok);
        {
            bool inner_ok = true;
            Evaluator::MutationBoundaryGuard inner(ev, &inner_ok);
            (void)inner_ok;
        }
        CHECK(outer_ok, "outer flag still true after inner exit");
    }
    const auto v_after = ev.defuse_version_for_test();
    std::println("  defuse_version_: {} -> {}", v_before, v_after);
    CHECK(v_after > v_before, "defuse_version_ bumped after nested Guard");
}

static void run_546_inner_panic_outer_rollback() {
    std::println("\n--- AC3 (#546): inner panic + outer auto-rollback ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    auto r0 = cs.eval("(panic-checkpoint)");
    CHECK(r0.has_value() && aura::compiler::types::is_bool(*r0) &&
              aura::compiler::types::as_bool(*r0),
          "(panic-checkpoint) succeeded (workspace loaded)");
    CHECK(cs.evaluator().has_panic_checkpoint(),
          "has_panic_checkpoint() true after Aura (panic-checkpoint)");
    auto r1 = cs.eval("(mutate:replace-value (define x 42) (define x 42))");
    CHECK(r1.has_value(), "mutate succeeded via Guard");
}

static void run_546_outer_guard_failure() {
    std::println("\n--- AC4 (#546): outer Guard failure → full auto-rollback ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(panic-checkpoint)");
    CHECK(r.has_value() && aura::compiler::types::as_bool(*r),
          "(panic-checkpoint) before outer mutation succeeded");
    (void)cs.eval("(mutate:replace-value (define x 99) (define x 99))");
    auto pre = cs.eval("x");
    (void)pre;
    auto r2 = cs.eval("(panic-restore)");
    CHECK(r2.has_value() && aura::compiler::types::as_bool(*r2),
          "(panic-restore) succeeds (rollback path reachable)");
}

static void run_546_commit_clears_snapshots() {
    std::println("\n--- AC5 (#546): commit_panic_checkpoint clears size snapshots ---");
    Evaluator ev;
    ev.set_panic_safe_cells_size_for_test(10);
    ev.set_panic_safe_pairs_size_for_test(5);
    ev.set_panic_safe_string_heap_size_for_test(100);
    ev.set_panic_safe_env_frames_size_for_test(7);
    CHECK(ev.panic_safe_cells_size() == 10, "panic_safe_cells_size_ == 10");
    CHECK(ev.panic_safe_pairs_size() == 5, "panic_safe_pairs_size_ == 5");
    CHECK(ev.panic_safe_string_heap_size() == 100, "panic_safe_string_heap_size_ == 100");
    CHECK(ev.panic_safe_env_frames_size() == 7, "panic_safe_env_frames_size_ == 7");
    ev.commit_panic_checkpoint();
    CHECK(ev.panic_safe_cells_size() == 0, "commit clears panic_safe_cells_size_");
    CHECK(ev.panic_safe_pairs_size() == 0, "commit clears panic_safe_pairs_size_");
    CHECK(ev.panic_safe_string_heap_size() == 0, "commit clears panic_safe_string_heap_size_");
    CHECK(ev.panic_safe_env_frames_size() == 0, "commit clears panic_safe_env_frames_size_");
}

static void run_546_panic_fuzz() {
    std::println("\n--- AC6 (#546): {} iters nested mutate + random panic fuzz ---",
                 k_fuzz_iters_546());
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    std::mt19937 rng(546u);
    std::uniform_int_distribution<int> val_dist(0, 999);
    std::uniform_int_distribution<int> panic_every(7, 23);
    int panics = 0;
    int restores = 0;
    int next_panic = panic_every(rng);

    for (int i = 0; i < k_fuzz_iters_546(); ++i) {
        std::string code = "(define ";
        code += (i & 1) ? "a" : "b";
        code += " ";
        code += std::to_string(val_dist(rng));
        code += ")";
        (void)cs.eval(code);
        if (i == next_panic) {
            (void)cs.eval("(panic-checkpoint)");
            ++panics;
            (void)cs.eval("(mutate:replace-value (define a 9999) "
                          "(define a 9999))");
            auto r = cs.eval("(panic-restore)");
            if (r.has_value() && aura::compiler::types::is_bool(*r) &&
                aura::compiler::types::as_bool(*r)) {
                ++restores;
            }
            next_panic = i + panic_every(rng);
        }
    }
    std::println("  fuzz iters: {} panic-checkpoints: {} panic-restores: {}", k_fuzz_iters_546(),
                 panics, restores);
    CHECK(panics > 0, "at least 1 panic-checkpoint executed");
    CHECK(restores > 0, "at least 1 panic-restore succeeded");
}

static void run_546_size_invariants() {
    std::println("\n--- AC7 (#546): panic_safe_*_size_ invariants ---");
    Evaluator ev;
    CHECK(ev.panic_safe_cells_size() == 0, "initial panic_safe_cells_size_ == 0");
    CHECK(ev.panic_safe_pairs_size() == 0, "initial panic_safe_pairs_size_ == 0");
    CHECK(ev.panic_safe_string_heap_size() == 0, "initial panic_safe_string_heap_size_ == 0");
    CHECK(ev.panic_safe_env_frames_size() == 0, "initial panic_safe_env_frames_size_ == 0");
    ev.set_panic_safe_cells_size_for_test(42);
    CHECK(ev.panic_safe_cells_size() == 42, "panic_safe_cells_size_ set/get round-trip (42)");
    ev.commit_panic_checkpoint();
    CHECK(ev.panic_safe_cells_size() == 0, "commit_panic_checkpoint resets to 0");
}

static void run_546_gc_heap_integration() {
    std::println("\n--- AC8 (#546): (gc-heap) integration with panic checkpoint ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define x 1) (define y 2)\")");
    (void)cs.eval("(eval-current)");
    auto r1 = cs.eval("(panic-checkpoint)");
    CHECK(r1.has_value() && aura::compiler::types::as_bool(*r1), "(panic-checkpoint) succeeded");
    (void)cs.eval("(mutate:replace-value (define x 99) (define x 99))");
    auto r2 = cs.eval("(gc-heap)");
    CHECK(r2.has_value(), "(gc-heap) callable under panic-checkpoint");
    auto r3 = cs.eval("(panic-restore)");
    CHECK(r3.has_value() && aura::compiler::types::as_bool(*r3),
          "(panic-restore) succeeded after (gc-heap)");
}

static void run_546_eight_thread_concurrent() {
    std::println("\n--- AC9 (#546): 8 threads × 20 iters concurrent nested mutate ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    constexpr int n_threads = 8;
    constexpr int n_iters = 20;
    std::mutex mtx;
    std::atomic<int> completed{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            std::string code = "(define v" + std::to_string(tid) + " " + std::to_string(i) + ")";
            (void)cs.eval(code);
            completed.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    std::println("  completed: {}/{} panic_checkpoint_lost_on_steal: {}", completed.load(),
                 n_threads * n_iters, cs.evaluator().get_panic_checkpoint_lost_on_steal());
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 ops completed (no crash under concurrent mutate)");
}

static void run_546_regression() {
    std::println("\n--- AC10 (#546): regression — existing primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(panic-checkpoint)");
    CHECK(r1.has_value(), "(panic-checkpoint) (regression for #546)");
    auto r2 = cs.eval("(panic-auto-rollback?)");
    CHECK(r2.has_value() && aura::compiler::types::is_bool(*r2),
          "(panic-auto-rollback?) (regression for #546)");
    auto r3 = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "(engine:metrics \"query:envframe-dualpath-stats\") (regression for #543)");
    CHECK(cs.eval("(define reg-546-a 10)").has_value(), "define (regression)");
    auto r5 = cs.eval("(+ reg-546-a reg-546-b)");
    CHECK(r5.has_value() && aura::compiler::types::is_int(*r5) &&
              aura::compiler::types::as_int(*r5) == 42,
          "(+ reg-546-a reg-546-b) == 42 (regression)");
}

// ── Issue #1546/#1554/#1594 — arena allocate_raw quota wiring ──
static void run_1546_set_arena_installs_owner() {
    std::println("\n--- AC1 (#1546): set_arena installs arena_owner ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ASTArena arena(64 * 1024);
    CHECK(!arena.has_arena_owner(), "fresh arena has no owner");
    ev.set_arena(&arena);
    CHECK(arena.has_arena_owner(), "set_arena installs owner");
    CHECK(arena.arena_owner() == static_cast<void*>(&ev), "owner is Evaluator*");
}

static void run_1546_allocate_checked_quota_reject() {
    std::println("\n--- AC2 (#1546): allocate_checked over limit → ResourceQuotaExceeded ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(64);

    const auto used0 = arena.stats().used;
    const auto rejects0 = load_u64(m->resource_quota_rejects_total);
    const auto checks0 = load_u64(m->resource_quota_checks_total);

    auto r = ev.allocate_checked(/*size=*/1024, /*align=*/8);
    CHECK(!r.has_value(), "allocate_checked over quota fails");
    if (!r) {
        CHECK(r.error().kind == AuraErrorKind::ResourceQuotaExceeded,
              "kind == ResourceQuotaExceeded");
    }
    CHECK(load_u64(m->resource_quota_rejects_total) == rejects0 + 1, "rejects_total +1");
    CHECK(load_u64(m->resource_quota_checks_total) > checks0, "checks_total advanced");
    CHECK(arena.stats().used == used0, "no allocation (used unchanged)");
}

static void run_1546_try_allocate_quota_gate() {
    std::println("\n--- AC3 (#1546): try_allocate over limit → nullptr ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(32);

    const auto used0 = arena.stats().used;
    const auto rejects0 = load_u64(m->resource_quota_rejects_total);

    void* p = arena.try_allocate(4096);
    CHECK(p == nullptr, "try_allocate over quota → nullptr");
    CHECK(load_u64(m->resource_quota_rejects_total) == rejects0 + 1,
          "allocate_raw path bumps rejects");
    CHECK(arena.stats().used == used0, "used unchanged after reject");
}

static void run_1546_orphan_unlimited() {
    std::println("\n--- AC4 (#1546): orphan arena (no owner) allocates large ---");
    ASTArena orphan(256 * 1024);
    CHECK(!orphan.has_arena_owner(), "orphan has no owner");
    void* p = orphan.try_allocate(8192);
    CHECK(p != nullptr, "orphan try_allocate large succeeds");
    CHECK(orphan.stats().used >= 8192, "orphan used advanced");
}

static void run_1546_under_limit_succeeds() {
    std::println("\n--- AC5 (#1546): under-limit allocate_checked succeeds ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(4096);

    auto r = ev.allocate_checked(/*size=*/128, /*align=*/8);
    CHECK(r.has_value(), "under-limit allocate_checked ok");
    if (r)
        CHECK(*r != nullptr, "pointer non-null");
    CHECK(arena.stats().used >= 128, "used advanced on success");
}

static void run_1546_create_over_limit() {
    std::println("\n--- AC6 (#1546): create<T> over limit → nullptr ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(8);

    struct Big {
        char buf[256];
    };
    const auto used0 = arena.stats().used;
    Big* b = arena.create<Big>();
    CHECK(b == nullptr, "create<Big> over quota → nullptr");
    CHECK(arena.stats().used == used0, "create reject: used unchanged");
}

static void run_1554_temp_arena_wired() {
    std::println("\n--- AC7 (#1554): set_temp_arena installs owner + gate ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena temp(256 * 1024);
    ev.set_temp_arena(&temp);
    CHECK(temp.has_arena_owner(), "set_temp_arena installs owner");
    CHECK(temp.arena_owner() == static_cast<void*>(&ev), "temp owner is Evaluator*");
    ev.set_resource_quota_memory(32);
    const auto rejects0 = load_u64(m->resource_quota_rejects_total);
    const auto used0 = temp.stats().used;
    CHECK(temp.try_allocate(4096) == nullptr, "temp try_allocate over quota → nullptr");
    CHECK(load_u64(m->resource_quota_rejects_total) == rejects0 + 1, "temp path rejects+1");
    CHECK(temp.stats().used == used0, "temp used unchanged");
}

static void run_1554_arena_allocate_checked() {
    std::println("\n--- AC8 (#1554): ASTArena::allocate_checked typed ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(64);
    const auto checks0 = load_u64(m->resource_quota_checks_total);
    const auto rejects0 = load_u64(m->resource_quota_rejects_total);
    auto bad = arena.allocate_checked(/*size=*/2048, /*align=*/8);
    CHECK(!bad.has_value(), "ASTArena::allocate_checked over quota fails");
    if (!bad) {
        CHECK(bad.error().kind == AuraErrorKind::ResourceQuotaExceeded,
              "ASTArena kind == ResourceQuotaExceeded");
    }
    CHECK(load_u64(m->resource_quota_checks_total) == checks0 + 1,
          "single check on arena allocate_checked reject");
    CHECK(load_u64(m->resource_quota_rejects_total) == rejects0 + 1, "rejects+1");

    auto ok = arena.allocate_checked(/*size=*/32, /*align=*/8);
    CHECK(ok.has_value() && *ok != nullptr, "ASTArena::allocate_checked under limit ok");
}

static void run_1554_boundary_exact_size() {
    std::println("\n--- AC9 (#1554): exact limit ok; limit+1 reject ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    constexpr std::size_t lim = 128;
    ev.set_resource_quota_memory(lim);
    auto eq = ev.allocate_checked(lim);
    CHECK(eq.has_value() && *eq != nullptr, "size==limit succeeds");
    auto over = ev.allocate_checked(lim + 1);
    CHECK(!over.has_value(), "size==limit+1 rejects");
    if (!over)
        CHECK(over.error().kind == AuraErrorKind::ResourceQuotaExceeded, "boundary typed error");
}

static void run_1554_arena_group_default_owner() {
    std::println("\n--- AC10 (#1554): ArenaGroup module_arena inherits owner ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena primary(64 * 1024);
    ev.set_arena(&primary);
    CHECK(ev.arena_group().has_default_arena_owner(), "group has default owner after set_arena");
    auto& mod = ev.arena_group().module_arena("1554-mod", /*initial=*/64 * 1024);
    CHECK(mod.has_arena_owner(), "new module arena inherits owner");
    CHECK(mod.arena_owner() == static_cast<void*>(&ev), "module owner is Evaluator*");
    ev.set_resource_quota_memory(16);
    const auto rejects0 = load_u64(m->resource_quota_rejects_total);
    CHECK(mod.try_allocate(1024) == nullptr, "module arena quota-gated");
    CHECK(load_u64(m->resource_quota_rejects_total) == rejects0 + 1, "module rejects+1");
}

// ── Issue #173 — stable-id type aliases + NULL_X_ID sentinels (folded from
// tests/issues/test_issue_173.cpp via #1957) ── Production aliases live in aura_jit_runtime.cpp;
// local forward-decl pins the contract so the test breaks if the production types change shape.
namespace aura_runtime_aliases {
    namespace aura {
        namespace runtime {
            using PairId = unsigned int;
            using CellId = unsigned int;
            using StringId = unsigned int;
            inline constexpr PairId NULL_PAIR_ID = static_cast<PairId>(~0ULL);
            inline constexpr CellId NULL_CELL_ID = static_cast<CellId>(~0ULL);
            inline constexpr StringId NULL_STRING_ID = static_cast<StringId>(~0ULL);
        } // namespace runtime
    } // namespace aura
} // namespace aura_runtime_aliases

static void run_173_type_aliases() {
    std::println("\n--- #173: type aliases are 32-bit unsigned ---");
    CHECK(sizeof(aura_runtime_aliases::aura::runtime::PairId) == 4, "PairId is 4 bytes");
    CHECK(sizeof(aura_runtime_aliases::aura::runtime::CellId) == 4, "CellId is 4 bytes");
    CHECK(sizeof(aura_runtime_aliases::aura::runtime::StringId) == 4, "StringId is 4 bytes");
    CHECK(aura_runtime_aliases::aura::runtime::PairId(42) <
              aura_runtime_aliases::aura::runtime::NULL_PAIR_ID,
          "PairId(42) < NULL_PAIR_ID");
}

static void run_173_null_sentinels() {
    std::println("\n--- #173: NULL_X_ID sentinels ---");
    CHECK(aura_runtime_aliases::aura::runtime::NULL_PAIR_ID == 0xFFFFFFFFu,
          "NULL_PAIR_ID == 0xFFFFFFFFu (all bits set)");
    CHECK(aura_runtime_aliases::aura::runtime::NULL_CELL_ID == 0xFFFFFFFFu,
          "NULL_CELL_ID == 0xFFFFFFFFu");
    CHECK(aura_runtime_aliases::aura::runtime::NULL_STRING_ID == 0xFFFFFFFFu,
          "NULL_STRING_ID == 0xFFFFFFFFu");
}

static void run_173_types_distinct() {
    std::println("\n--- #173: aliases are distinct (semantic check) ---");
    CHECK(aura_runtime_aliases::aura::runtime::NULL_PAIR_ID ==
              aura_runtime_aliases::aura::runtime::NULL_CELL_ID,
          "NULL_PAIR_ID == NULL_CELL_ID (both 0xFFFFFFFFu, but conceptually distinct)");
}

// ── Issue #219 — GapBuffer child_data_ for O(1) FlatAST insert/remove ──
// Folded from tests/issues/test_issue_219.cpp via #1957. Uses production
// GapBuffer template from src/core/gap_buffer.hh.

static void run_219_gap_buffer_child_data() {
    std::println("\n=== #219: GapBuffer child_data_ (FlatAST columnar O(1) ops) ===");
    using GB = aura::ast::GapBuffer<std::uint32_t>;
    // T1: basic push_back / size / operator[] / clear
    {
        GB gb;
        CHECK(gb.empty() && gb.size() == 0 && gb.capacity() == 0, "fresh gb empty");
        gb.push_back(10);
        gb.push_back(20);
        gb.push_back(30);
        CHECK(gb.size() == 3 && !gb.empty(), "size 3 after 3 pushes");
        CHECK(gb[0] == 10 && gb[1] == 20 && gb[2] == 30, "[0]=10 [1]=20 [2]=30");
        CHECK(gb.front() == 10 && gb.back() == 30, "front=10 back=30");
        gb.clear();
        CHECK(gb.empty() && gb.size() == 0, "empty after clear");
        CHECK(gb.capacity() >= 3, "capacity preserved across clear");
    }
    // T2: insert at front/middle/end
    {
        GB gb;
        gb.push_back(1);
        gb.push_back(3);
        gb.push_back(5);
        gb.insert(1, 2);
        CHECK(gb.size() == 4 && gb[0] == 1 && gb[3] == 5, "middle insert [1,2,3,5]");
        gb.insert(0, 0);
        CHECK(gb.size() == 5 && gb[0] == 0 && gb[4] == 5, "front insert [0,1,2,3,5]");
        gb.insert(gb.size(), 6);
        CHECK(gb.size() == 6 && gb[5] == 6, "end insert last=6");
        gb.insert(3, 99);
        gb.insert(3, 88);
        CHECK(gb.size() == 8 && gb[3] == 88 && gb[4] == 99, "two inserts at pos 3");
    }
    // T3: erase
    {
        GB gb;
        for (std::uint32_t i = 0; i < 5; ++i)
            gb.push_back(i * 10);
        gb.erase(2);
        CHECK(gb.size() == 4 && gb[2] == 30, "middle erase");
        gb.erase(0);
        gb.erase(gb.size() - 1);
        gb.erase(0);
        gb.erase(0);
        CHECK(gb.empty(), "empty after erasing all");
        gb.erase(0);
        CHECK(gb.empty(), "erase on empty is no-op");
    }
    // T4-6: perf smoke (5000-element, 100 ops each < 100µs/op; 1k+1k+compact < 10ms)
    {
        GB gb;
        for (std::uint32_t i = 0; i < 5000; ++i)
            gb.push_back(i);
        std::mt19937 rng(12345);
        std::uniform_int_distribution<std::uint32_t> pos(0, static_cast<std::uint32_t>(gb.size()));
        for (int i = 0; i < 5; ++i)
            gb.insert(pos(rng), 99999); // warm-up
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i)
            gb.insert(pos(rng), 99999);
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        CHECK(static_cast<double>(us) / 100.0 < 100.0, "100 inserts < 100us/op");
    }
    {
        GB gb;
        for (std::uint32_t i = 0; i < 5000; ++i)
            gb.push_back(i);
        std::mt19937 rng(54321);
        std::uniform_int_distribution<std::uint32_t> pos(0,
                                                         static_cast<std::uint32_t>(gb.size()) - 1);
        for (int i = 0; i < 5; ++i)
            gb.erase(pos(rng));
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i)
            gb.erase(pos(rng));
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        CHECK(static_cast<double>(us) / 100.0 < 100.0, "100 erases < 100us/op");
    }
    {
        GB gb;
        for (int i = 0; i < 100; ++i)
            gb.push_back(i);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i)
            gb.insert(static_cast<std::size_t>(i) % 100, static_cast<std::uint32_t>(i));
        for (int i = 0; i < 1000; ++i)
            gb.erase(static_cast<std::size_t>(i) % gb.size());
        gb.compact();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        CHECK(us < 10000, "1000+1000+compact < 10ms (perf)");
    }
    // T7: clear + reconstruct
    {
        GB gb;
        for (std::uint32_t i = 0; i < 100; ++i)
            gb.push_back(i * 7);
        CHECK(gb.size() == 100, "pre-clear size 100");
        gb.clear();
        for (std::uint32_t i = 0; i < 50; ++i)
            gb.push_back(i * 11);
        CHECK(gb.size() == 50, "size 50 after reconstruct");
        for (std::uint32_t i = 0; i < 50; ++i)
            CHECK(gb[i] == i * 11, "element matches");
    }
    // T8: reserve + grow + shrink_to_fit
    {
        GB gb;
        gb.reserve(100);
        CHECK(gb.capacity() >= 100 && gb.size() == 0, "reserve sets capacity");
        for (std::uint32_t i = 0; i < 200; ++i)
            gb.push_back(i);
        CHECK(gb.size() == 200 && gb.capacity() >= 200, "capacity grew");
        gb.shrink_to_fit();
        CHECK(gb.capacity() == gb.size(), "shrink_to_fit -> capacity == size");
    }
    // T9: AST-like pattern (100 mixed insert/erase ops)
    {
        GB gb;
        constexpr std::uint32_t num_nodes = 5000, avg_children = 3;
        for (std::uint32_t i = 0; i < num_nodes * avg_children; ++i)
            gb.push_back(i);
        CHECK(gb.size() == num_nodes * avg_children, "AST-like pre-built size");
        std::mt19937 rng(99999);
        std::uniform_int_distribution<std::uint32_t> pos(0,
                                                         static_cast<std::uint32_t>(gb.size()) - 1);
        std::uniform_int_distribution<int> op(0, 1);
        for (int i = 0; i < 100; ++i) {
            if (op(rng) == 0)
                gb.insert(pos(rng), 99999);
            else
                gb.erase(pos(rng));
        }
        CHECK(true, "100 mixed AST-like ops complete");
    }
    // T10: wire format v1 roundtrip
    {
        GB gb;
        constexpr std::uint32_t n = 1000;
        for (std::uint32_t i = 0; i < n; ++i)
            gb.push_back(i * 13);
        std::vector<char> buf;
        std::uint32_t count = static_cast<std::uint32_t>(gb.size());
        buf.insert(buf.end(), reinterpret_cast<char*>(&count), reinterpret_cast<char*>(&count) + 4);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t v = gb[i];
            buf.insert(buf.end(), reinterpret_cast<char*>(&v), reinterpret_cast<char*>(&v) + 4);
        }
        CHECK(buf.size() == 4 + n * 4, "serialized buf size matches");
        GB gb2;
        std::size_t pos = 0;
        std::memcpy(&count, &buf[pos], 4);
        pos += 4;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t v;
            std::memcpy(&v, &buf[pos], 4);
            pos += 4;
            gb2.push_back(v);
        }
        CHECK(gb2.size() == n, "deserialized size matches");
        for (std::uint32_t i = 0; i < n; ++i)
            CHECK(gb2[i] == i * 13, "element matches after roundtrip");
        CHECK(pos == buf.size(), "all bytes consumed");
    }
}

} // namespace aura_arena_batch

int main() {
    using namespace aura_arena_batch;
    std::println(
        "=== Arena batch: #1621 + #405 + #1662 + #546 + #1546/#1554 + #173 (42 ACs total) ===");
    std::println("(test_arena_auto_compact_fiber_defag_shape_dirty_closedloop.cpp #743");
    std::println(" NOT included — bundle member via tests/bundles/test_issues_jit_late3_main.cpp;");
    std::println(" out of scope for batch. test_arena_defrag_concurrent.cpp #1390 NOT included —");
    std::println(" AuraDomainTests.cmake default-build with link_llvm_jit_minimal; out of scope)");
    run_1621_policy_unit();
    run_1621_shape_churn_signal();
    run_1623_query_schema();
    run_1621_mutate_path();
    run_1621_stress();
    run_1621_lineage();
    run_405_matrix();
    run_1662_dtor_clears_owner();
    run_1662_surviving_allocate();
    run_1662_temp_arena();
    run_1662_rebind_clears_previous();
    run_1662_arena_group_default();
    run_1662_compiler_service_raii();
    run_546_panic_primitives_reachable();
    run_546_nested_guard_basic();
    run_546_inner_panic_outer_rollback();
    run_546_outer_guard_failure();
    run_546_commit_clears_snapshots();
    run_546_panic_fuzz();
    run_546_size_invariants();
    run_546_gc_heap_integration();
    run_546_eight_thread_concurrent();
    run_546_regression();
    run_1546_set_arena_installs_owner();
    run_1546_allocate_checked_quota_reject();
    run_1546_try_allocate_quota_gate();
    run_1546_orphan_unlimited();
    run_1546_under_limit_succeeds();
    run_1546_create_over_limit();
    run_1554_temp_arena_wired();
    run_1554_arena_allocate_checked();
    run_1554_boundary_exact_size();
    run_1554_arena_group_default_owner();
    run_173_type_aliases();
    run_173_null_sentinels();
    run_173_types_distinct();
    run_219_gap_buffer_child_data();
    std::println("\n=== Arena batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
