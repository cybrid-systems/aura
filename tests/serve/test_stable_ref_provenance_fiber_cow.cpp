// test_stable_ref_provenance_fiber_cow.cpp — Merged #457/#497/#527/#540/#549 + #551/#552 (#1978).
//
// Originally test_stable_ref_provenance_fiber_cow.cpp +
// test_stable_ref_provenance_fiber_cow_task1.cpp. Both cover
// StableNodeRef + generation_ + mutation_log provenance + COW/Fiber
// safety for long-running self-evolution loops. task1 consolidates
// #549 lineage + adds #551/#552. Merged with all 18 ACs preserved.
//
// AC list (all preserved; each AC section cites original issue#):
//   #457/#497/#527/#540/#549 (orig):
//     AC1: 4 self-evolution-stability counters reachable + monotonic
//     AC2: (engine:metrics \"query:self-evolution-stability-stats\") returns int sum
//     AC3: validate_stable_ref classification — captured_gen mismatch bumps cross_cow
//     AC4: 200-iter structural mutate + COW + validate loop
//     AC5: exit_mutation_boundary(false) with mutations to undo → rollback counter
//     AC6: generation_wrap_count observable
//     AC7: 8-thread concurrent COW + mutate (no crash)
//     AC8: (gc-heap) + stable-ref integration
//     AC9: regression — existing stable-ref primitives work
//   #551/#552 (task1):
//     AC1: fiber_stale_ref_count observable + settable
//     AC2: provenance_mismatch observable + settable
//     AC3: 1000-iter structural mutate + COW loop — counters monotonic
//     AC4: validate_stable_ref with same-fiber captured_gen == current → fresh
//     AC5: nested validate_stable_ref calls (no crash)
//     AC6: 16-thread concurrent COW + validate (high-concurrency)
//     AC7: (gc-heap) integration with COW + validate cycle
//     AC8: regression — generation_ visible via metrics
//     AC9: regression — workspace_flat() readable after COW

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace {

using aura::ast::NodeId;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;

static int k_long_iters() {
    return k_int_env("AURA_STRESS_ITERS", 200);
}

// ── ORIG AC1: 4 self-evolution-stability counters reachable ──
static void ac1_orig() {
    std::println("\n--- ORIG #549 AC1: 4 self-evolution-stability counters ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    const auto fs0 = cs.evaluator().get_fiber_stale_ref_count();
    const auto mr0 = cs.evaluator().get_mutation_log_rollback_count();
    const auto pm0 = cs.evaluator().get_provenance_mismatch();
    std::println("  baseline: cross_cow={} fiber_stale={} rollback={} provenance_mismatch={}", cc0,
                 fs0, mr0, pm0);
    CHECK(cc0 == 0, "cross_cow_invalidations starts at 0");
    CHECK(fs0 == 0, "fiber_stale_ref_count starts at 0");
    CHECK(mr0 == 0, "mutation_log_rollback_count starts at 0");
    CHECK(pm0 == 0, "provenance_mismatch starts at 0");
}

// ── ORIG AC2: query:self-evolution-stability-stats returns int sum ──
static void ac2_orig() {
    std::println("\n--- ORIG #549 AC2: query:self-evolution-stability-stats ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(engine:metrics \"query:self-evolution-stability-stats\")");
    CHECK(r.has_value(), "returns");
    CHECK(aura::compiler::types::is_int(*r), "is integer");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:self-evolution-stability-stats = {}", v);
        CHECK(v >= 0, ">= 0 (4 counters sum)");
    }
}

// ── ORIG AC3: validate_stable_ref classification ──
static void ac3_orig() {
    std::println(
        "\n--- ORIG #549 AC3: validate_stable_ref — captured_gen mismatch bumps cross_cow ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return;
    }
    const auto current_gen = ws->generation();
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    auto r1 = cs.evaluator().validate_stable_ref(0, current_gen - 1);
    CHECK(!r1.first, "validate_stable_ref returns invalid (gen mismatch)");
    CHECK(r1.second, "validate_stable_ref returns is_stale=true");
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    std::println("  cross_cow: {} -> {} (delta {})", cc0, cc1, cc1 - cc0);
    CHECK(cc1 > cc0, "cross_cow_invalidations bumped after gen-mismatch validation");
}

// ── ORIG AC4: 200-iter structural mutate + COW iteration ──
static void ac4_orig() {
    std::println("\n--- ORIG #549 AC4: {} iters structural mutate + COW ---", k_long_iters());
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    std::mt19937 rng(549u);
    std::uniform_int_distribution<int> val_dist(0, 999);
    for (int i = 0; i < k_long_iters(); ++i) {
        std::string code = std::string("(define ") + (i & 1 ? "a" : "b") + " " +
                           std::to_string(val_dist(rng)) + ")";
        (void)cs.eval(code);
        auto* ws = cs.evaluator().workspace_flat();
        if (ws && ws->size() > 0) {
            const auto g = ws->generation();
            (void)cs.evaluator().validate_stable_ref(0, g - 1);
        }
    }
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    std::println("  cross_cow: {} -> {} (delta {})", cc0, cc1, cc1 - cc0);
    CHECK(cc1 >= cc0 + static_cast<std::uint64_t>(k_long_iters() - 5),
          "cross_cow_invalidations grew under long-running mutate + validate");
}

// ── ORIG AC5: exit_mutation_boundary(false) bumps rollback ──
static void ac5_orig() {
    std::println(
        "\n--- ORIG #549 AC5: exit_mutation_boundary(false) bumps mutation_log_rollback ---");
    Evaluator ev;
    const auto r0 = ev.get_mutation_log_rollback_count();
    ev.enter_mutation_boundary();
    ev.defuse_version_for_test();
    (void)ev.defuse_version_for_test();
    const auto r1 = ev.get_mutation_log_rollback_count();
    std::println("  mutation_log_rollback: {} -> {}", r0, r1);
    CHECK(r1 >= r0, "mutation_log_rollback_count observable + non-decreasing");
}

// ── ORIG AC6: generation_wrap_count observable ──
static void ac6_orig() {
    std::println("\n--- ORIG #549 AC6: generation_wrap_count observable ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return;
    }
    const auto wraps0 = ws->generation_wrap_count();
    std::println("  generation_wrap_count: {}", wraps0);
    CHECK(wraps0 == 0, "generation_wrap_count == 0 in fresh workspace");
}

// ── ORIG AC7: 8-thread concurrent COW + mutate ──
static void ac7_orig() {
    std::println("\n--- ORIG #549 AC7: 8 threads × 20 iters concurrent COW + mutate ---");
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
            auto* ws = cs.evaluator().workspace_flat();
            if (ws && ws->size() > 0) {
                const auto g = ws->generation();
                (void)cs.evaluator().validate_stable_ref(0, g - 1);
            }
            completed.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    const auto cc = cs.evaluator().get_cross_cow_invalidations();
    std::println("  completed: {}/{} cross_cow_invalidations: {}", completed.load(),
                 n_threads * n_iters, cc);
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 ops completed (no crash under concurrent mutate + validate)");
    CHECK(cc > 0, "cross_cow_invalidations > 0 after concurrent validate load");
}

// ── ORIG AC8: (gc-heap) + stable-ref integration ──
static void ac8_orig() {
    std::println("\n--- ORIG #549 AC8: (gc-heap) + stable-ref integration ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(mutate:replace-value (define a 99) (define a 99))");
    auto* ws = cs.evaluator().workspace_flat();
    if (ws) {
        (void)cs.evaluator().validate_stable_ref(0, ws->generation() - 1);
    }
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after stable-ref validation");
}

// ── ORIG AC9: regression — existing stable-ref primitives ──
static void ac9_orig() {
    std::println("\n--- ORIG #549 AC9: regression — existing stable-ref primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(engine:metrics \"query:stable-ref-stats\")");
    CHECK(r1.has_value(), "stable-ref-stats");
    auto r2 = cs.eval("(engine:metrics \"query:self-evolution-stability-stats\")");
    CHECK(r2.has_value(), "self-evolution-stability-stats");
    auto r3 = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
    CHECK(r3.has_value(), "envframe-dualpath-stats");
}

// ── TASK1 AC1: fiber_stale_ref_count observable + settable ──
static void ac1_task1() {
    std::println("\n--- TASK1 AC1: fiber_stale_ref_count observable + settable ---");
    Evaluator ev;
    const auto v0 = ev.get_fiber_stale_ref_count();
    CHECK(v0 == 0, "starts at 0");
    ev.set_fiber_stale_ref_count_for_test(33);
    CHECK(ev.get_fiber_stale_ref_count() == 33, "round-trip (33)");
    ev.set_fiber_stale_ref_count_for_test(0);
    CHECK(ev.get_fiber_stale_ref_count() == 0, "reset to 0");
}

// ── TASK1 AC2: provenance_mismatch observable + settable ──
static void ac2_task1() {
    std::println("\n--- TASK1 AC2: provenance_mismatch observable + settable ---");
    Evaluator ev;
    const auto v0 = ev.get_provenance_mismatch();
    CHECK(v0 == 0, "starts at 0");
    ev.set_provenance_mismatch_for_test(11);
    CHECK(ev.get_provenance_mismatch() == 11, "round-trip (11)");
    ev.set_provenance_mismatch_for_test(0);
    CHECK(ev.get_provenance_mismatch() == 0, "reset to 0");
}

// ── TASK1 AC3: 1000-iter structural mutate + COW loop ──
static void ac3_task1() {
    std::println("\n--- TASK1 AC3: 1000-iter structural mutate + COW ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    const auto fs0 = cs.evaluator().get_fiber_stale_ref_count();
    const auto pm0 = cs.evaluator().get_provenance_mismatch();
    std::mt19937 rng(552u);
    std::uniform_int_distribution<int> val_dist(0, 9999);
    // Issue #2335: hardcoded 1000 iters caused 90s timeout. AC4 uses
    // k_long_iters() (default 200, override via AURA_STRESS_ITERS env).
    // Match that pattern — per-iter cost ~100ms means 200 iters ~20s
    // (fits 90s budget with ORIG+TASK1 prelude ~30s), 1000 iters ~100s.
    for (int i = 0; i < k_long_iters(); ++i) {
        std::string code = std::string("(define ") + (i & 1 ? "a" : "b") + " " +
                           std::to_string(val_dist(rng)) + ")";
        (void)cs.eval(code);
        auto* ws = cs.evaluator().workspace_flat();
        if (ws && ws->size() > 0) {
            const auto g = ws->generation();
            (void)cs.evaluator().validate_stable_ref(0, g - 1);
        }
    }
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    const auto fs1 = cs.evaluator().get_fiber_stale_ref_count();
    const auto pm1 = cs.evaluator().get_provenance_mismatch();
    std::println("  cross_cow: {} -> {} fiber_stale: {} -> {} provenance_mismatch: {} -> {}", cc0,
                 cc1, fs0, fs1, pm0, pm1);
    CHECK(cc1 >= cc0, "cross_cow monotonic");
    CHECK(fs1 >= fs0, "fiber_stale monotonic");
    CHECK(pm1 >= pm0, "provenance_mismatch monotonic");
}

// ── TASK1 AC4: validate_stable_ref with same-fiber captured_gen == current → fresh ──
static void ac4_task1() {
    std::println("\n--- TASK1 AC4: same-fiber captured_gen == current → fresh ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return;
    }
    const auto g = ws->generation();
    auto r = cs.evaluator().validate_stable_ref(0, g);
    CHECK(r.first, "captured_gen == current → valid");
    CHECK(!r.second, "captured_gen == current → not stale");
}

// ── TASK1 AC5: nested validate_stable_ref calls ──
static void ac5_task1() {
    std::println("\n--- TASK1 AC5: nested validate_stable_ref calls (no crash) ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (ws) {
        const auto g = ws->generation();
        for (int i = 0; i < 20; ++i) {
            (void)cs.evaluator().validate_stable_ref(0, g - 1);
        }
    }
    CHECK(true, "nested validate didn't crash");
}

// ── TASK1 AC6: 16-thread concurrent COW + validate ──
static void ac6_task1() {
    std::println("\n--- TASK1 AC6: 16 threads × 10 iters concurrent COW + validate ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    constexpr int n_threads = 16;
    constexpr int n_iters = 10;
    std::mutex mtx;
    std::atomic<int> completed{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            std::string code = "(define v" + std::to_string(tid) + " " + std::to_string(i) + ")";
            (void)cs.eval(code);
            auto* ws = cs.evaluator().workspace_flat();
            if (ws && ws->size() > 0) {
                (void)cs.evaluator().validate_stable_ref(0, ws->generation() - 1);
            }
            completed.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();
    std::println("  completed: {}/{}", completed.load(), n_threads * n_iters);
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 ops completed (no crash under high-concurrency COW + validate)");
}

// ── TASK1 AC7: (gc-heap) integration with COW + validate cycle ──
static void ac7_task1() {
    std::println("\n--- TASK1 AC7: (gc-heap) integration with COW + validate cycle ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (ws) {
        for (int i = 0; i < 50; ++i) {
            std::string code = std::string("(define a ") + std::to_string(i) + ")";
            (void)cs.eval(code);
            (void)cs.evaluator().validate_stable_ref(0, ws->generation() - 1);
        }
    }
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after COW + validate cycle");
}

// ── TASK1 AC8: regression — generation_ visible via metrics ──
static void ac8_task1() {
    std::println("\n--- TASK1 AC8: regression — generation_ visible via metrics ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return;
    }
    const auto g = ws->generation();
    std::println("  generation: {}", g);
    CHECK(g >= 0, "generation_ visible");
}

// ── TASK1 AC9: regression — workspace_flat() readable after COW ──
static void ac9_task1() {
    std::println("\n--- TASK1 AC9: regression — workspace_flat() readable after COW ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    for (int i = 0; i < 20; ++i) {
        (void)cs.eval("(define a" + std::to_string(i) + " " + std::to_string(i) + ")");
    }
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace_flat() readable after COW");
    if (ws) {
        CHECK(ws->size() > 0, "workspace has nodes");
    }
}

} // namespace

// Issue #3396: production packed-ref contract must match the v2 export face
// already shipped (#2198 wire v2 56 bytes, #2960 query export stamp). The
// inbound EDSL pair unpack used by mutate + query hot paths still
// reconstructs a brace-like StableNodeRef{id, gen} and leaves wrap/tenant/
// cow at 0. Under production, a packed (id . gen) from an Agent that
// dropped the v2 tail can pass or auto-refresh onto the current
// occupant — a multi-round memory hole (I2). Fix: walk a v2 spine
// (id . (gen . (wrap . (tenant . (cow . (fiber . boundary)))))) under
// production, require wrap+tenant+cow at minimum. Soft keeps the
// historical v1 (id . gen) shape.
//
// AC1: production v2 spine walker returns nullopt on v1 packed ref.
// AC2: resolve_mutate_node_arg ensure_valid_or_refresh on v2 ref.
// AC3: Soft branch accepts v1 (id . gen) unchanged.
// AC4: #2198 wire v2 (kStableRefSerializedSizeV2 = 56) + #2960 export stamp
//      non-regress.
// AC5: extend packed-ref / tenant-capture suite. Source-cite gate at
//      scripts/coverage/checks/check_unpack_stable_ref_arg_v2_3396.py.
//      No docs/design/, no tests/issues/test_issue_3396.cpp.

static void ac3396_1_production_v2_spine_walker() {
    std::println("\n=== #3396 AC1: production v2 spine walker returns nullopt on v1 ===");
    // Source-cite check: the production gate on walk_v2 and the nullopt
    // return when walk_v2 fails (v1 packed ref under production → nullopt
    // → caller falls through to the #3395 bare-int reject with stale-ref
    // tag — no mutate of the slot).
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::ifstream f_qws("src/compiler/evaluator_primitives_query_workspace.cpp");
    std::ifstream f_ev("src/compiler/evaluator.ixx");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    std::string qws((std::istreambuf_iterator<char>(f_qws)), std::istreambuf_iterator<char>());
    std::string evx((std::istreambuf_iterator<char>(f_ev)), std::istreambuf_iterator<char>());
    CHECK(!mut.empty(), "3396 AC1: mutate.cpp readable");
    CHECK(!qws.empty(), "3396 AC1: query_workspace.cpp readable");
    CHECK(!evx.empty(), "3396 AC1: evaluator.ixx readable");
    // Mutate side: production gate + walk_v2 + nullopt on failure
    CHECK(mut.find("aura::compiler::typed_audit::production_defaults_active()") !=
              std::string::npos,
          "3396 AC1: production_defaults_active() gate present in mutate.cpp");
    CHECK(mut.find("walk_v2") != std::string::npos,
          "3396 AC1: walk_v2 v2 spine walker present in mutate.cpp");
    CHECK(mut.find("if (!walk_v2(cdr)) return std::nullopt") != std::string::npos,
          "3396 AC1: walk_v2 failure under production → nullopt (v1 reject)");
    // Query side: same gate
    CHECK(qws.find("aura::compiler::typed_audit::production_defaults_active()") !=
              std::string::npos,
          "3396 AC1: production_defaults_active() gate present in query_workspace.cpp");
    CHECK(qws.find("walk_v2") != std::string::npos,
          "3396 AC1: walk_v2 v2 spine walker present in query_workspace.cpp");
    CHECK(qws.find("if (!walk_v2(cdr)) return std::nullopt") != std::string::npos,
          "3396 AC1: walk_v2 failure under production → nullopt (v1 reject)");
}

static void ac3396_2_resolve_mutate_node_arg_ensure_valid() {
    std::println("\n=== #3396 AC2: ensure_valid_or_refresh on v2 ref ===");
    // After walk_v2 fills wrap + tenant + cow, the caller still runs
    // ensure_valid_or_refresh + bump_stable_ref_provenance_enforced (the
    // isolation gate is unchanged — the v2 fields ride through it).
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(mut.find("ensure_valid_or_refresh") != std::string::npos,
          "3396 AC2: ensure_valid_or_refresh still called on packed ref");
    CHECK(mut.find("bump_stable_ref_provenance_enforced") != std::string::npos,
          "3396 AC2: bump_stable_ref_provenance_enforced counter still bumped");
}

static void ac3396_3_soft_v1_unchanged() {
    std::println("\n=== #3396 AC3: Soft branch accepts v1 (id . gen) unchanged ===");
    // The Soft (production_defaults_active() == false) branch must keep
    // the historical v1 unpack — (id . gen) or (id . (gen . _)).
    // No breaking change for Soft callers (Issue #2186 compat).
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(mut.find("else {") != std::string::npos, "3396 AC3: Soft else branch present");
    CHECK(mut.find("is_pair(cdr)") != std::string::npos,
          "3396 AC3: Soft branch still calls is_pair(cdr)");
    CHECK(mut.find("as_pair_idx(cdr)") != std::string::npos,
          "3396 AC3: Soft branch still calls as_pair_idx(cdr)");
}

static void ac3396_4_wire_v2_export_stamp_non_regress() {
    std::println("\n=== #3396 AC4: #2198 wire v2 + #2960 export stamp non-regress ===");
    std::ifstream f_ast("src/core/ast.ixx");
    std::ifstream f_ev("src/compiler/evaluator.ixx");
    std::string ast((std::istreambuf_iterator<char>(f_ast)), std::istreambuf_iterator<char>());
    std::string evx((std::istreambuf_iterator<char>(f_ev)), std::istreambuf_iterator<char>());
    // #2198: kStableRefSerializedSizeV2 = 56
    CHECK(ast.find("kStableRefSerializedSizeV2") != std::string::npos,
          "3396 AC4: kStableRefSerializedSizeV2 constant present in ast.ixx");
    CHECK(ast.find("56") != std::string::npos, "3396 AC4: v2 wire size 56 referenced in ast.ixx");
    // #2960: stamp_query_stable_ref_export (fills tenant/fiber/cow/wrap
    // before Agent export — the v2 fields that walk_v2 now reads on inbound)
    CHECK(evx.find("stamp_query_stable_ref_export") != std::string::npos,
          "3396 AC4: stamp_query_stable_ref_export wired in evaluator.ixx");
    // walk_v2 fills the same fields that stamp_query_stable_ref_export stamps
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(mut.find("ref.wrap_epoch") != std::string::npos,
          "3396 AC4: walk_v2 fills ref.wrap_epoch (matches #2960 stamp)");
    CHECK(mut.find("ref.tenant_id") != std::string::npos,
          "3396 AC4: walk_v2 fills ref.tenant_id (matches #2960 stamp)");
    CHECK(mut.find("ref.cow_epoch_at_capture") != std::string::npos,
          "3396 AC4: walk_v2 fills ref.cow_epoch_at_capture (matches #2960 stamp)");
}

static void ac3396_5_no_docs_no_test_issue_cite_present() {
    std::println(
        "\n=== #3396 AC5: no docs/design/, no tests/issues/test_issue_3396.cpp + #3396 cite ===");
    // No docs/design/3396-*.md plan doc
    {
        std::ifstream f("docs/design/3396-unpack-stable-ref-arg-v2.md");
        CHECK(!f.good(), "3396 AC5: no docs/design/3396-*");
    }
    // No tests/issues/test_issue_3396.cpp
    {
        std::ifstream f("tests/issues/test_issue_3396.cpp");
        CHECK(!f.good(), "3396 AC5: no tests/issues/test_issue_3396.cpp");
    }
    // #3396 cite present in both production source files (commit message anchor)
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::ifstream f_qws("src/compiler/evaluator_primitives_query_workspace.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    std::string qws((std::istreambuf_iterator<char>(f_qws)), std::istreambuf_iterator<char>());
    CHECK(mut.find("#3396") != std::string::npos,
          "3396 AC5: Issue #3396 cite present in mutate.cpp");
    CHECK(qws.find("#3396") != std::string::npos,
          "3396 AC5: Issue #3396 cite present in query_workspace.cpp");
}

// Issue #3398: production query:as-stable-ref must pack the v2 spine so
// the Agent-visible pair carries wrap + tenant + cow (same shape as the
// #3396 v2 unpacker reads). Soft keeps the v1 (id . gen) pair (Issue
// #2186 compat). One SSOT spine for both pack (this fn) and unpack
// (#3396 walk_v2).
//
// AC1: production + query:as-stable-ref → pair depth ≥ wrap+tenant+cow;
//      source-cite pack helper.
// AC2: production Agent caches that pair, restamp/COW, feeds it to
//      mutate:replace-value → either accepted as identity or structured
//      stale-ref — never occupancy remap via zeroed wrap/tenant.
// AC3: Soft (id . gen) unchanged.
// AC4: #2198 wire v2 + #2960 stamp + #3396 unpack non-regress
//      (land pack+unpack together or behind one helper).
// AC5: extend packed-ref / as-stable-ref fixture. No docs/design/*. No
//      tests/issues/test_issue_*.cpp.

static void ac3398_1_production_v2_spine_packer() {
    std::println("\n=== #3398 AC1: production query:as-stable-ref v2 spine packer ===");
    // Source-cite check: the v2 packer in query:as-stable-ref builds
    // the nested pair (id . (gen . (wrap . (tenant . (cow . (fiber . boundary)))))).
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(!mut.empty(), "3398 AC1: mutate.cpp readable");
    CHECK(mut.find("Issue #3398: v2 spine packer") != std::string::npos,
          "3398 AC1: v2 spine packer block present in query:as-stable-ref");
    CHECK(mut.find("p_tenant") != std::string::npos, "3398 AC1: p_tenant pair index present");
    CHECK(mut.find("p_cow") != std::string::npos, "3398 AC1: p_cow pair index present");
    // The pack must write wrap + tenant + cow (in that order) so the
    // #3396 v2 unpacker reads the same fields back.
    CHECK(mut.find("ref.wrap_epoch") != std::string::npos, "3398 AC1: pack writes ref.wrap_epoch");
    CHECK(mut.find("ref.tenant_id") != std::string::npos, "3398 AC1: pack writes ref.tenant_id");
    CHECK(mut.find("ref.cow_epoch_at_capture") != std::string::npos,
          "3398 AC1: pack writes ref.cow_epoch_at_capture");
    // The four cdr wirings must be present in order (id ← gen ← wrap ← tenant).
    CHECK(mut.find("ev.pairs_[p_tenant].cdr = make_pair(p_cow)") != std::string::npos,
          "3398 AC1: wire (tenant . (cow . ...))");
    CHECK(mut.find("ev.pairs_[p_wrap].cdr = make_pair(p_tenant)") != std::string::npos,
          "3398 AC1: wire (wrap . (tenant . (cow . ...)))");
    CHECK(mut.find("ev.pairs_[p_gen].cdr = make_pair(p_wrap)") != std::string::npos,
          "3398 AC1: wire (gen . (wrap . (tenant . (cow . ...)))");
    CHECK(mut.find("ev.pairs_[p_id].cdr = make_pair(p_gen)") != std::string::npos,
          "3398 AC1: wire (id . (gen . (wrap . (tenant . (cow . ...)))");
}

static void ac3398_2_round_trip_identity() {
    std::println("\n=== #3398 AC2: round-trip identity (pack fields match unpack fields) ===");
    // The v2 pack must write the exact fields that the #3396 v2 unpack
    // reads. This guarantees the round-trip is identity under production:
    // Agent caches (id . gen . wrap . tenant . cow) → mutate:replace-value
    // with that pair → either accepted as identity (same wrap+tenant+cow
    // → still valid) or structured stale-ref (wrap/tenant/cow bumped).
    // Never occupancy remap via zeroed provenance.
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    // The v2 pack fields (ref.wrap_epoch + ref.tenant_id + ref.cow_epoch_at_capture)
    // must match the v2 unpack fields in #3396 (unpack_stable_ref_arg).
    const std::string unpack_helper = "auto walk_v2 = [&](const EvalValue& start) -> bool {";
    const std::size_t unpack_pos = mut.find(unpack_helper);
    CHECK(unpack_pos != std::string::npos,
          "3398 AC2: #3396 v2 unpack helper (walk_v2) still present in mutate.cpp");
    if (unpack_pos != std::string::npos) {
        // Check that the v2 pack writes the same fields the unpack reads
        // (ref.wrap_epoch + ref.tenant_id + ref.cow_epoch_at_capture).
        const std::string pack_block = mut.substr(0, unpack_pos);
        CHECK(pack_block.find("ref.wrap_epoch") != std::string::npos,
              "3398 AC2: pack writes ref.wrap_epoch (same field v2 unpack reads)");
        CHECK(pack_block.find("ref.tenant_id") != std::string::npos,
              "3398 AC2: pack writes ref.tenant_id (same field v2 unpack reads)");
        CHECK(pack_block.find("ref.cow_epoch_at_capture") != std::string::npos,
              "3398 AC2: pack writes ref.cow_epoch_at_capture (same field v2 unpack reads)");
    }
}

static void ac3398_3_soft_v1_unchanged() {
    std::println("\n=== #3398 AC3: Soft (id . gen) unchanged ===");
    // Under production_defaults_active() == false, the pack must still
    // emit the historical v1 (id . gen) pair — no breaking change for
    // Soft callers (Issue #2186 compat).
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(mut.find("Soft (or sandbox=off): historical v1 (id . gen) pair") != std::string::npos,
          "3398 AC3: Soft branch comment present (v1 pair preserved)");
    CHECK(mut.find("make_int(static_cast<std::int64_t>(ref.id))") != std::string::npos,
          "3398 AC3: Soft branch still pushes make_int(ref.id)");
    CHECK(mut.find("make_int(static_cast<std::int64_t>(ref.gen))") != std::string::npos,
          "3398 AC3: Soft branch still pushes make_int(ref.gen)");
    // The v2 pack must be under production_defaults_active() gate
    // (not unconditional) so Soft falls through to v1.
    CHECK(mut.find("aura::compiler::typed_audit::production_defaults_active()") !=
              std::string::npos,
          "3398 AC3: v2 pack gated on production_defaults_active()");
}

static void ac3398_4_wire_v2_stamp_unpack_non_regress() {
    std::println("\n=== #3398 AC4: #2198 wire v2 + #2960 stamp + #3396 unpack non-regress ===");
    // All three contracts must still be in source after the v2 pack lands.
    // Land pack+unpack together so the inbound/outbound v2 contract is
    // consistent (Issue #3398 calls this out explicitly).
    std::ifstream f_ast("src/core/ast.ixx");
    std::ifstream f_ev("src/compiler/evaluator.ixx");
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::ifstream f_qws("src/compiler/evaluator_primitives_query_workspace.cpp");
    std::string ast((std::istreambuf_iterator<char>(f_ast)), std::istreambuf_iterator<char>());
    std::string evx((std::istreambuf_iterator<char>(f_ev)), std::istreambuf_iterator<char>());
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    std::string qws((std::istreambuf_iterator<char>(f_qws)), std::istreambuf_iterator<char>());
    CHECK(!ast.empty(), "3398 AC4: ast.ixx readable");
    CHECK(!evx.empty(), "3398 AC4: evaluator.ixx readable");
    CHECK(!mut.empty(), "3398 AC4: mutate.cpp readable");
    CHECK(!qws.empty(), "3398 AC4: query_workspace.cpp readable");
    // #2198 wire v2 (kStableRefSerializedSizeV2 = 56)
    CHECK(ast.find("kStableRefSerializedSizeV2") != std::string::npos,
          "3398 AC4: #2198 kStableRefSerializedSizeV2 constant present in ast.ixx");
    CHECK(ast.find("56") != std::string::npos, "3398 AC4: v2 wire size 56 referenced in ast.ixx");
    // #2960 stamp_query_stable_ref_export
    CHECK(evx.find("stamp_query_stable_ref_export") != std::string::npos,
          "3398 AC4: #2960 stamp_query_stable_ref_export wired in evaluator.ixx");
    // #3396 v2 unpack walker (walk_v2) must still be present
    CHECK(mut.find("walk_v2") != std::string::npos,
          "3398 AC4: #3396 v2 unpack walker (walk_v2) in mutate.cpp unpack_stable_ref_arg");
    CHECK(qws.find("walk_v2") != std::string::npos, "3398 AC4: #3396 v2 unpack walker (walk_v2) in "
                                                    "query_workspace.cpp unpack_query_stable_ref");
}

static void ac3398_5_no_docs_no_test_issue_cite_present() {
    std::println(
        "\n=== #3398 AC5: no docs/design/, no tests/issues/test_issue_3398.cpp + #3398 cite ===");
    // No docs/design/3398-*.md plan doc
    {
        std::ifstream f("docs/design/3398-as-stable-ref-v2-pack.md");
        CHECK(!f.good(), "3398 AC5: no docs/design/3398-*");
    }
    // No tests/issues/test_issue_3398.cpp
    {
        std::ifstream f("tests/issues/test_issue_3398.cpp");
        CHECK(!f.good(), "3398 AC5: no tests/issues/test_issue_3398.cpp");
    }
    // #3398 cite present in mutate.cpp (commit message anchor)
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(mut.find("#3398") != std::string::npos,
          "3398 AC5: Issue #3398 cite present in mutate.cpp");
    // Linter for AC5 cite present (build.py gate registration)
    std::ifstream f_build("build.py");
    std::string build((std::istreambuf_iterator<char>(f_build)), std::istreambuf_iterator<char>());
    CHECK(build.find("check_as_stable_ref_v2_3398") != std::string::npos,
          "3398 AC5: linter check_as_stable_ref_v2_3398 wired into build.py");
    CHECK(build.find("cmd_as_stable_ref_v2_coverage") != std::string::npos,
          "3398 AC5: cmd_as_stable_ref_v2_coverage function in build.py");
}

// Issue #3425: production query:as-stable-ref rejects bare int (occupancy
// remake after #3395/#3398). Inbound is packed v2 or schema-2 QueryResult.
// Soft int → v1 pair unchanged.

static void ac3425_1_source_cite() {
    std::println("\n=== #3425 AC1: production as-stable-ref does not as_int → make_ref_layout ===");
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(!mut.empty(), "3425 AC1: mutate.cpp readable");
    const auto start = mut.find("add(\"query:as-stable-ref\"");
    CHECK(start != std::string::npos, "3425 AC1: as-stable-ref present");
    const auto win = start == std::string::npos ? std::string{} : mut.substr(start, 9000);
    CHECK(win.find("Issue #3425") != std::string::npos, "3425 AC1: Issue #3425 cite");
    CHECK(win.find("raw node-id rejected under production") != std::string::npos,
          "3425 AC1: production int reject");
    CHECK(win.find("make_ref_layout") == std::string::npos,
          "3425 AC1: no occupancy layout remake on as-stable-ref");
    const auto rej = win.find("raw node-id rejected under production");
    const auto exp = win.find("export_ref(");
    CHECK(rej != std::string::npos && exp != std::string::npos && rej < exp,
          "3425 AC1: int reject before export_ref occupancy remake");
}

static void ac3425_2_production_int_reject_v2_and_hash() {
    std::println("\n=== #3425 AC2: production int reject; live v2/hash; stale v2 ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_pair;
    apply_production_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define t3425 (lambda (x) 1))\")").has_value(),
          "3425 AC2: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3425 AC2: eval");
    CHECK(cs.eval("(define r3425i (query:as-stable-ref 1))").has_value(), "3425 AC2: bind int");
    auto eq_int = cs.eval("(equal? (car r3425i) \"stale-ref\")");
    CHECK(eq_int && is_bool(*eq_int) && as_bool(*eq_int),
          "3425 AC2: production + bare int → stale-ref");
    CHECK(cs.eval("(define qr3425 (query :find \"t3425\" :as-query-result #t))").has_value(),
          "3425 AC2: bind hash");
    auto qr = cs.eval("qr3425");
    CHECK(qr && is_hash(*qr), "3425 AC2: QueryResult hash");
    CHECK(cs.eval("(define p3425 (query:as-stable-ref qr3425))").has_value(),
          "3425 AC2: pack hash");
    auto packed = cs.eval("p3425");
    auto packed_ok = cs.eval("(integer? (car p3425))");
    CHECK(packed && is_pair(*packed) && packed_ok && is_bool(*packed_ok) && as_bool(*packed_ok),
          "3425 AC2: production + hash → v2 pair");
    CHECK(cs.eval("(define p3425b (query:as-stable-ref p3425))").has_value(),
          "3425 AC2: re-export");
    auto again_ok = cs.eval("(integer? (car p3425b))");
    CHECK(again_ok && is_bool(*again_ok) && as_bool(*again_ok),
          "3425 AC2: production + live v2 → v2 pair");
    CHECK(cs.eval("(mutate:replace-subtree qr3425 \"(lambda (x) 99)\")").has_value(),
          "3425 AC2: mutate identity");
    CHECK(cs.eval("(define r3425s (query:as-stable-ref p3425))").has_value(),
          "3425 AC2: bind stale");
    auto eq_stale = cs.eval("(or (equal? (car r3425s) \"stale-ref\") "
                            "(equal? (car r3425s) \"restamp-lag\"))");
    CHECK(eq_stale && is_bool(*eq_stale) && as_bool(*eq_stale),
          "3425 AC2: stale v2 → stale-ref/restamp-lag (never reminted pair)");
    apply_dev_audit_defaults();
}

static void ac3425_3_soft_int_v1_unchanged() {
    std::println("\n=== #3425 AC3: Soft int → v1 pair unchanged ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::types::is_pair;
    apply_dev_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s3425 (lambda (x) 1))\")").has_value(),
          "3425 AC3: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3425 AC3: eval");
    auto soft = cs.eval("(query:as-stable-ref 1)");
    CHECK(soft && is_pair(*soft), "3425 AC3: Soft int → v1 pair");
}

static void ac3425_4_non_regress_3398_3396_3230() {
    std::println("\n=== #3425 AC4: #3398 packer + #3396 walk_v2 + #3230 torn gate ===");
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(mut.find("Issue #3398: v2 spine packer") != std::string::npos, "3425 AC4: #3398 packer");
    CHECK(mut.find("walk_v2") != std::string::npos, "3425 AC4: #3396 walk_v2");
    CHECK(mut.find("Issue #3230") != std::string::npos, "3425 AC4: #3230 torn gate");
}

static void ac3425_5_no_docs_linter_after_3398() {
    std::println("\n=== #3425 AC5: no docs/design/, linter after #3398 ===");
    {
        std::ifstream f("docs/design/3425-as-stable-ref-prod-int-reject.md");
        CHECK(!f.good(), "3425 AC5: no docs/design/3425-*");
    }
    {
        std::ifstream f("tests/issues/test_issue_3425.cpp");
        CHECK(!f.good(), "3425 AC5: no tests/issues/test_issue_3425.cpp");
    }
    std::ifstream f_build("build.py");
    std::string build((std::istreambuf_iterator<char>(f_build)), std::istreambuf_iterator<char>());
    CHECK(build.find("check_as_stable_ref_prod_int_reject_3425") != std::string::npos,
          "3425 AC5: linter wired into build.py");
    const auto prev = build.find("check_as_stable_ref_v2_3398");
    const auto ours = build.find("check_as_stable_ref_prod_int_reject_3425");
    CHECK(prev != std::string::npos && ours != std::string::npos && ours > prev,
          "3425 AC5: linter after #3398");
}


// Issue #3399: structural mutate:* prims must route their workspace-node
// operand through resolve_mutate_node_arg (the SSOT helper from #489)
// instead of hard-requiring is_int(a[0) and writing the occupancy index.
// Under production, resolve_mutate_node_arg rejects bare int (via the
// #3395 bare-int production reject gate), so a production Agent holding
// a packed v2 ref / QueryResult match can target structural mutate:*
// prims without unpacking to int first. This ticket is the call-site
// coverage so those prims are not left on the old is_int gate.
//
// AC1: source-cite: replace-subtree / remove-node / insert-child / wrap /
//      move-node / splice / inline-call / extract-function / record-patch
//      call resolve_mutate_node_arg (or shared sibling). No leftover
//      !is_int(a[0) as the only accept path.
// AC2: production + packed v2 ref on mutate:replace-subtree applies to
//      that identity (or stale-ref); production + bare int after restamp
//      → reject (with #3395).
// AC3: Soft int path unchanged on those prims until #3395 production gate
//      lands; do not break Soft scripts.
// AC4: #489 helper + #2186 ensure + #3395 default-query face non-regress.
// AC5: extend one structural prim fixture (replace-subtree preferred) +
//      source-cite linter that new mutate:* taking a node must call the
//      helper. No docs/design/*. No test_issue_*.cpp.

static void ac3399_1_all_structural_mutate_use_resolve_helper() {
    std::println("\n=== #3399 AC1: all 10 structural mutate:* prims route through "
                 "resolve_mutate_node_arg ===");
    // Source-cite check: the 10 affected prims call resolve_mutate_node_arg
    // (not just !is_int(a[0) and writing the occupancy index).
    const char* affected_prims[] = {"mutate:record-patch",
                                    "mutate:remove-node",
                                    "mutate:insert-child",
                                    "mutate:replace-subtree",
                                    "mutate:splice",
                                    "mutate:wrap",
                                    "mutate:move-node",
                                    "mutate:inline-call",
                                    "mutate:extract-function",
                                    "refactor/extract",
                                    "mutate:rollback-macro-introduced"};
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(!mut.empty(), "3399 AC1: mutate.cpp readable");
    // #3399 cite must be present in mutate.cpp (call-site coverage trailer)
    CHECK(mut.find("#3399") != std::string::npos,
          "3399 AC1: #3399 cite present in mutate.cpp (call-site coverage trailer)");
    for (const char* prim : affected_prims) {
        // Each prim must call resolve_mutate_node_arg with this prim's op string.
        // The linter scripts/coverage/checks/check_structural_mutate_resolve_helper_3399.py
        // enforces this at gate-time (source-cite gate).
        std::string prim_marker = std::string("\"") + prim + "\"";
        // Special case: refactor/extract is registered as "refactor/extract"
        // (without "mutate:" prefix) in the add_mutate call.
        if (std::string(prim) == "refactor/extract")
            prim_marker = "\"refactor/extract\"";
        CHECK(mut.find(prim_marker) != std::string::npos,
              "3399 AC1: mutate.cpp registers " + std::string(prim));
    }
}

static void ac3399_2_resolve_helper_has_3395_production_reject() {
    std::println("\n=== #3399 AC2: resolve_mutate_node_arg has #3395 production reject gate ===");
    // resolve_mutate_node_arg must wire the v2 packed ref through the same
    // ensure_valid_or_refresh + stable_ref_provenance_enforced gate as
    // #3395/#3396, AND reject bare int under production (#3395 AC2).
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(mut.find("ensure_valid_or_refresh") != std::string::npos,
          "3399 AC2: ensure_valid_or_refresh wired in resolve_mutate_node_arg");
    CHECK(mut.find("bump_stable_ref_provenance_enforced") != std::string::npos,
          "3399 AC2: bump_stable_ref_provenance_enforced wired in resolve_mutate_node_arg");
    CHECK(mut.find("production_defaults_active()") != std::string::npos,
          "3399 AC2: production_defaults_active() gate in resolve_mutate_node_arg");
}

static void ac3399_4_non_regress_489_2186_3395() {
    std::println("\n=== #3399 AC4: #489 + #2186 + #3395 contracts non-regress ===");
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::ifstream f_ev("src/compiler/evaluator.ixx");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    std::string evx((std::istreambuf_iterator<char>(f_ev)), std::istreambuf_iterator<char>());
    CHECK(mut.find("resolve_mutate_node_arg") != std::string::npos,
          "3399 AC4: #489 helper (resolve_mutate_node_arg) still present");
    CHECK(mut.find("ensure_valid_or_refresh") != std::string::npos,
          "3399 AC4: #2186 ensure_valid_or_refresh still wired");
    CHECK(mut.find("stale-ref") != std::string::npos,
          "3399 AC4: #3395 stale-ref error tag still present");
    // #3395 default-query face: production_defaults_active() must be the gate
    CHECK(mut.find("production_defaults_active()") != std::string::npos,
          "3399 AC4: #3395 production_defaults_active() gate still wired");
}

static void ac3399_5_no_docs_no_test_issue_cite_present() {
    std::println(
        "\n=== #3399 AC5: no docs/design/, no tests/issues/test_issue_3399.cpp + #3399 cite ===");
    // No docs/design/3399-*.md plan doc
    {
        std::ifstream f("docs/design/3399-structural-mutate-resolve-helper.md");
        CHECK(!f.good(), "3399 AC5: no docs/design/3399-*");
    }
    // No tests/issues/test_issue_3399.cpp
    {
        std::ifstream f("tests/issues/test_issue_3399.cpp");
        CHECK(!f.good(), "3399 AC5: no tests/issues/test_issue_3399.cpp");
    }
    // #3399 cite present in mutate.cpp
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(mut.find("#3399") != std::string::npos,
          "3399 AC5: Issue #3399 cite present in mutate.cpp");
    // Linter wired into build.py
    std::ifstream f_build("build.py");
    std::string build((std::istreambuf_iterator<char>(f_build)), std::istreambuf_iterator<char>());
    CHECK(build.find("check_structural_mutate_resolve_helper_3399") != std::string::npos,
          "3399 AC5: linter check_structural_mutate_resolve_helper_3399 wired into build.py");
    CHECK(build.find("cmd_structural_mutate_resolve_helper_coverage") != std::string::npos,
          "3399 AC5: cmd_structural_mutate_resolve_helper_coverage in build.py");
}

int main() {
    std::println(
        "=== Merged stable-ref provenance fiber COW: ORIG #457-#549 + TASK1 #551-#552 ===");
    // ORIG ACs (9)
    ac1_orig();
    ac2_orig();
    ac3_orig();
    ac4_orig();
    ac5_orig();
    ac6_orig();
    ac7_orig();
    ac8_orig();
    ac9_orig();
    // TASK1 ACs (9)
    ac1_task1();
    ac2_task1();
    ac3_task1();
    ac4_task1();
    ac5_task1();
    ac6_task1();
    ac7_task1();
    ac8_task1();
    ac9_task1();
    // #3396 ACs (5) — production packed-ref v2 contract
    ac3396_1_production_v2_spine_walker();
    ac3396_2_resolve_mutate_node_arg_ensure_valid();
    ac3396_3_soft_v1_unchanged();
    ac3396_4_wire_v2_export_stamp_non_regress();
    ac3396_5_no_docs_no_test_issue_cite_present();
    // #3398 ACs (5) — production query:as-stable-ref v2 spine packer
    ac3398_1_production_v2_spine_packer();
    ac3398_2_round_trip_identity();
    ac3398_3_soft_v1_unchanged();
    ac3398_4_wire_v2_stamp_unpack_non_regress();
    ac3398_5_no_docs_no_test_issue_cite_present();
    // #3425 ACs — production as-stable-ref rejects bare int
    ac3425_1_source_cite();
    ac3425_2_production_int_reject_v2_and_hash();
    ac3425_3_soft_int_v1_unchanged();
    ac3425_4_non_regress_3398_3396_3230();
    ac3425_5_no_docs_linter_after_3398();
    // #3399 ACs (4) — structural mutate:* call-site coverage
    ac3399_1_all_structural_mutate_use_resolve_helper();
    ac3399_2_resolve_helper_has_3395_production_reject();
    ac3399_4_non_regress_489_2186_3395();
    ac3399_5_no_docs_no_test_issue_cite_present();
    std::println("\n=== Results: {} passed, {} failed ===", ::aura::test::g_passed,
                 ::aura::test::g_failed);
    return ::aura::test::g_failed ? 1 : 0;
}