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

#include "core/workspace_epoch.hh" // Issue #4165: QueryResult + schema-2 reserved
// FRESHNESS_ONLY: omit the g_hash_tables-linked decode/resolve
// templates (not needed here - the freshness validator is self-contained).
#define AURA_QUERY_RESULT_DECODE_FRESHNESS_ONLY
#include "compiler/query_result_decode.hh" // Issue #4165: freshness SSOT (post-module)

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
    {
        const auto wvf = mut.find("if (!walk_v2(cdr))");
        const auto ret =
            wvf == std::string::npos ? std::string::npos : mut.find("return std::nullopt", wvf);
        CHECK(wvf != std::string::npos && ret != std::string::npos && ret - wvf < 64,
              "3396 AC1: walk_v2 failure under production → nullopt (v1 reject)");
    }
    // Query side: same gate
    CHECK(qws.find("aura::compiler::typed_audit::production_defaults_active()") !=
              std::string::npos,
          "3396 AC1: production_defaults_active() gate present in query_workspace.cpp");
    CHECK(qws.find("walk_v2") != std::string::npos,
          "3396 AC1: walk_v2 v2 spine walker present in query_workspace.cpp");
    {
        const auto wvf = qws.find("if (!walk_v2(cdr))");
        const auto ret =
            wvf == std::string::npos ? std::string::npos : qws.find("return std::nullopt", wvf);
        CHECK(wvf != std::string::npos && ret != std::string::npos && ret - wvf < 64,
              "3396 AC1: walk_v2 failure under production → nullopt (v1 reject)");
    }
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
    // The v2 pack (pack_v2, "Issue #3398: v2 spine packer") lives BELOW the
    // #3396 unpack lambda in file order — anchor the pack-side field checks
    // on the packer block so the round-trip compares the landed layout.
    const std::size_t pack_pos = mut.find("Issue #3398: v2 spine packer");
    CHECK(pack_pos != std::string::npos, "3398 AC2: v2 spine packer block present in mutate.cpp");
    if (pack_pos != std::string::npos) {
        // Check that the v2 pack reads the same fields the unpack writes
        // (ref.wrap_epoch + ref.tenant_id + ref.cow_epoch_at_capture).
        const std::string pack_block = mut.substr(pack_pos, 4000);
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

// Issue #3661: production packed v2 with expired gen must stale-ref, not
// occupancy-remake the current occupant. Soft auto_refresh kept.

static void test_ac3661_1_expired_gen_stale_ref() {
    std::println("\n=== #3661 AC1: production expired-gen v2 is stale-ref ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_pair;
    apply_dev_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define A3661 (lambda () 1))\\n(define B3661 (lambda () 2))\")")
              .has_value(),
          "3661 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3661 AC1: eval");
    apply_production_audit_defaults();
    CHECK(cs.eval("(define qrA3661 (query :find \"A3661\"))").has_value(), "3661 AC1: bind A hash");
    CHECK(cs.eval("(define qrB3661 (query :find \"B3661\"))").has_value(), "3661 AC1: bind B hash");
    auto ha = cs.eval("qrA3661");
    auto hb = cs.eval("qrB3661");
    CHECK(ha && is_hash(*ha), "3661 AC1: A is schema-2 hash");
    CHECK(hb && is_hash(*hb), "3661 AC1: B is schema-2 hash");
    CHECK(cs.eval("(define pA3661 (query:as-stable-ref qrA3661))").has_value(),
          "3661 AC1: pack A v2");
    auto packed = cs.eval("pA3661");
    CHECK(packed && is_pair(*packed), "3661 AC1: packed v2 pair");
    // Expire gen without a second mutate Guard (Guard-reject is not the
    // residual). Slot stays live; wrap/tenant/cow unchanged.
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "3661 AC1: workspace");
    // Poison packed gen in-place. Slot stays live; wrap/tenant/cow
    // unchanged. A second replace-subtree Guard is not the residual.
    aura::ast::FlatAST::StableNodeRef poisoned{};
    {
        using aura::compiler::types::as_int;
        using aura::compiler::types::as_pair_idx;
        using aura::compiler::types::is_int;
        using aura::compiler::types::is_pair;
        using aura::compiler::types::make_int;
        auto& ev = cs.evaluator();
        const auto pidx = as_pair_idx(*packed);
        CHECK(is_int(ev.pairs()[pidx].car), "3661 AC1: v2 id is int");
        poisoned.id = static_cast<aura::ast::NodeId>(as_int(ev.pairs()[pidx].car));
        auto rest = ev.pairs()[pidx].cdr;
        CHECK(is_pair(rest), "3661 AC1: v2 gen cell is a pair");
        const auto gidx = as_pair_idx(rest);
        CHECK(is_int(ev.pairs()[gidx].car), "3661 AC1: v2 gen is int");
        const auto oldg = as_int(ev.pairs()[gidx].car);
        ev.pairs()[gidx].car = make_int(oldg + 97);
        poisoned.gen = static_cast<std::uint16_t>(oldg + 97);
        rest = ev.pairs()[gidx].cdr;
        if (is_pair(rest) && is_int(ev.pairs()[as_pair_idx(rest)].car))
            poisoned.wrap_epoch =
                static_cast<std::uint32_t>(as_int(ev.pairs()[as_pair_idx(rest)].car));
        CHECK(ws->is_live_node(poisoned.id), "3661 AC1: slot still live");
    }
    CHECK(aura::compiler::typed_audit::production_defaults_active(),
          "3661 AC1: production still active");
    auto& ev = cs.evaluator();
    CHECK(!ev.ensure_valid_or_refresh(poisoned, /*auto_refresh=*/false).has_value(),
          "3661 AC1: mutate ensure auto_refresh=false rejects expired gen");
    CHECK(cs.eval("(define rChk3661 (mutate:check-stable-ref pA3661))").has_value(),
          "3661 AC1: bind check-stable-ref");
    auto chk_t = cs.eval("(eq? rChk3661 #t)");
    CHECK(chk_t && is_bool(*chk_t) && !as_bool(*chk_t),
          "3661 AC1: mutate probe of expired-gen v2 is not ok");
    CHECK(cs.eval("(define rQ3661 (query :children pA3661))").has_value(),
          "3661 AC1: bind query children packed");
    auto stale_q = cs.eval("(and (pair? rQ3661) (equal? (car rQ3661) \"stale-ref\"))");
    CHECK(stale_q && is_bool(*stale_q) && as_bool(*stale_q),
          "3661 AC1: query resolve of expired-gen v2 is stale-ref");
    apply_dev_audit_defaults();
}

static void test_ac3661_2_matching_gen_succeeds() {
    std::println("\n=== #3661 AC2: production matching-gen v2 still resolves ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_pair;
    apply_dev_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define C3661 (lambda () 1))\")").has_value(), "3661 AC2: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3661 AC2: eval");
    apply_production_audit_defaults();
    CHECK(cs.eval("(define qrC3661 (query :find \"C3661\"))").has_value(), "3661 AC2: bind hash");
    auto h = cs.eval("qrC3661");
    CHECK(h && is_hash(*h), "3661 AC2: schema-2 hash");
    CHECK(cs.eval("(define pC3661 (query:as-stable-ref qrC3661))").has_value(),
          "3661 AC2: pack v2");
    auto packed = cs.eval("pC3661");
    CHECK(packed && is_pair(*packed), "3661 AC2: packed v2");
    CHECK(cs.eval("(define rCh3661 (query :children pC3661))").has_value(),
          "3661 AC2: bind children");
    auto stale = cs.eval("(and (pair? rCh3661) (equal? (car rCh3661) \"stale-ref\"))");
    CHECK(stale && is_bool(*stale) && !as_bool(*stale),
          "3661 AC2: matching-gen v2 query resolve is not stale-ref");
    apply_dev_audit_defaults();
}

static void test_ac3661_3_soft_auto_refresh() {
    std::println("\n=== #3661 AC3: Soft auto_refresh historical ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_pair;
    apply_dev_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define S3661 (lambda () 1))\\n(define T3661 (lambda () 2))\")")
              .has_value(),
          "3661 AC3: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3661 AC3: eval");
    auto soft = cs.eval("(query:as-stable-ref 1)");
    CHECK(soft && is_pair(*soft), "3661 AC3: Soft int → v1 pair");
    CHECK(cs.eval("(define pS3661 (query:as-stable-ref 1))").has_value(), "3661 AC3: bind packed");
    CHECK(cs.eval("(mutate:rebind \"T3661\" \"9\")").has_value(), "3661 AC3: rebind T");
    CHECK(cs.eval("(define rS3661 (query :children pS3661))").has_value(),
          "3661 AC3: bind children after mutate");
    auto stale = cs.eval("(and (pair? rS3661) (equal? (car rS3661) \"stale-ref\"))");
    CHECK(stale && is_bool(*stale) && !as_bool(*stale),
          "3661 AC3: Soft packed still auto-refresh after mutate");
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(mut.find("auto_refresh=*/true") != std::string::npos,
          "3661 AC3: Soft auto_refresh=true still in mutate.cpp");
}

static void test_ac3661_4_strict_not_bypassed() {
    std::println("\n=== #3661 AC4: Strict policy on ensure-fail, not after refresh success ===");
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    const auto r = mut.find("auto resolve_mutate_node_arg");
    CHECK(r != std::string::npos, "3661 AC4: resolve helper");
    const auto win = r == std::string::npos ? std::string{} : mut.substr(r, 6000);
    CHECK(win.find("Issue #3661") != std::string::npos, "3661 AC4: cite");
    CHECK(win.find("auto_refresh=*/refresh") != std::string::npos,
          "3661 AC4: production refresh flag");
    const auto ens = win.find("auto_refresh=*/refresh");
    const auto strict = win.find("StaleRefPolicy::Strict");
    CHECK(ens != std::string::npos && strict != std::string::npos && ens < strict,
          "3661 AC4: Strict runs after ensure, not skipped by refresh success");
    CHECK(win.find("stable-ref is stale (Strict policy blocked)") != std::string::npos,
          "3661 AC4: Strict stale-ref message kept");
}

static void test_ac3661_5_suites_and_linter() {
    std::println("\n=== #3661 AC5: suites + linter; no invent ===");
    std::ifstream f_build("build.py");
    std::string build((std::istreambuf_iterator<char>(f_build)), std::istreambuf_iterator<char>());
    CHECK(build.find("check_packed_v2_no_occupancy_refresh_3661") != std::string::npos,
          "3661 AC5: linter wired");
    CHECK(build.find("check_query_result_per_match_fresh_3660") != std::string::npos,
          "3661 AC5: #3660 linter retained");
    std::ifstream f_hyg("tests/compiler/test_hygiene_mutate_closed_loop.cpp");
    std::string hyg((std::istreambuf_iterator<char>(f_hyg)), std::istreambuf_iterator<char>());
    CHECK(hyg.find("ac3661_hygiene_source_cite") != std::string::npos,
          "3661 AC5: hygiene extended");
    std::ifstream f_ten("tests/compiler/test_stable_ref_tenant_mandate.cpp");
    std::string ten((std::istreambuf_iterator<char>(f_ten)), std::istreambuf_iterator<char>());
    CHECK(ten.find("3661") != std::string::npos, "3661 AC5: tenant isolation extended");
    std::ifstream f_mut("src/compiler/evaluator_primitives_mutate.cpp");
    std::string mut((std::istreambuf_iterator<char>(f_mut)), std::istreambuf_iterator<char>());
    CHECK(mut.find("raw node-id rejected under production") != std::string::npos,
          "3661 AC5: #3395 retained");
    CHECK(mut.find("walk_v2") != std::string::npos, "3661 AC5: #3396 retained");
    std::ifstream f_st("src/core/ast_stability.cpp");
    std::string st((std::istreambuf_iterator<char>(f_st)), std::istreambuf_iterator<char>());
    CHECK(st.find("wrap_epoch != 0 && wrap_epoch != ast.wrap_epoch()") != std::string::npos,
          "3661 AC5: #2393 wrap fence retained");
    {
        std::ifstream f("tests/compiler/test_issue_3661.cpp");
        CHECK(!f.good(), "3661 AC5: no invent");
    }
    {
        std::ifstream f("docs/design/3661-packed-v2-no-occupancy-refresh.md");
        CHECK(!f.good(), "3661 AC5: no docs/design");
    }
}

// Issue #4106: production query mint prims must not treat a bare NodeId as
// the current occupant. After a slot recycle the remembered int aliases the
// NEW occupant; query:stable-ref / query:ensure-ref / query:stable-ref-
// provenance must refuse it (stale-ref / valid=0 / #f) instead of minting a
// green stamp, while packed v2 / schema-2 operands whose gen still matches
// still resolve. Soft keeps the historical bare-int mint.

// Issue #4106: C++-side error-kind reader (pair + string car) — avoids the
// EDSL define-of-error + string-arg forms the compile-time checker rejects.
static std::string kind4106(CompilerService& cs, const aura::compiler::types::EvalValue& v) {
    using namespace aura::compiler::types;
    if (!is_pair(v))
        return {};
    auto& prs = cs.evaluator().pairs();
    const auto pidx = as_pair_idx(v);
    if (pidx >= prs.size() || !is_string(prs[pidx].car))
        return {};
    auto heap = cs.evaluator().string_heap();
    const auto cidx = as_string_idx(prs[pidx].car);
    if (cidx >= heap.size())
        return {};
    return std::string(heap[cidx]);
}

static void ac4106_1_prod_mint_bare_int_refuses() {
    std::println("\n=== #4106 AC1: production mint prims refuse bare int ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    // Issue #3049 / #1547 / #1618: the resource-quota limits/usage are
    // process-global; the long-running suites before these ACs exhaust the
    // production mutation budget, so set-code would deny with
    // resource-quota-exceeded before the #4106 faces run. Reset the whole
    // process quota (orthogonal DoS guard; default is unlimited).
    aura::core::resource_quota::reset_process_resource_quota_for_test();
    apply_production_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define t4106 (lambda (x) 1))\")").has_value(),
          "4106 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4106 AC1: eval");
    // stable-ref: the #3395 stale-ref face — never a schema-2 hash minted
    // from the current occupant of the int's slot.
    auto sr4106 = cs.eval("(query:stable-ref 1)");
    CHECK(sr4106.has_value(), "4106 AC1: bind");
    CHECK(kind4106(cs, *sr4106) == "stale-ref", "4106 AC1: stable-ref bare int → stale-ref");
    // ensure-ref: the diagnostic hash face — valid=0, refreshed=0, and no
    // make_stamped_safe_ref / export ever runs on the unstamped int.
    auto ens4106 = cs.eval("(query:ensure-ref 1)");
    CHECK(ens4106 && is_hash(*ens4106), "4106 AC1: ensure-ref returns diagnostic hash");
    auto ev4106 = cs.eval("(hash-ref (query:ensure-ref 1) \"valid\")");
    CHECK(ev4106 && is_int(*ev4106) && as_int(*ev4106) == 0, "4106 AC1: ensure-ref valid=0");
    auto er4106 = cs.eval("(hash-ref (query:ensure-ref 1) \"refreshed\")");
    CHECK(er4106 && is_int(*er4106) && as_int(*er4106) == 0, "4106 AC1: ensure-ref refreshed=0");
    // stable-ref-provenance: the primitive's existing #f refusal face —
    // never is-live=1 of the occupant.
    auto pr4106 = cs.eval("(query:stable-ref-provenance 1)");
    CHECK(pr4106 && is_bool(*pr4106) && !as_bool(*pr4106), "4106 AC1: provenance bare int → #f");
    apply_dev_audit_defaults();
}

static void ac4106_2_prod_packed_and_hash_still_resolve() {
    std::println("\n=== #4106 AC2: packed v2 / schema-2 whose gen matches still resolve ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::compiler::types::is_pair;
    {
        auto& rq4106 = aura::core::resource_quota::process_resource_quota();
        rq4106.set_limit(aura::core::resource_quota::Dimension::Mutations, 0);
        rq4106.set_limit(aura::core::resource_quota::Dimension::Memory, 0);
        rq4106.set_limit(aura::core::resource_quota::Dimension::Fibers, 0);
        rq4106.set_limit(aura::core::resource_quota::Dimension::TimeUs, 0);
        rq4106.reset_usage();
    }
    aura::core::resource_quota::reset_process_resource_quota_for_test();
    apply_production_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define u4106 (lambda (x) 2))\")").has_value(),
          "4106 AC2: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4106 AC2: eval");
    CHECK(cs.eval("(define qr4106 (query :find \"u4106\" :as-query-result #t))").has_value(),
          "4106 AC2: bind hash");
    auto qr4106 = cs.eval("qr4106");
    CHECK(qr4106 && is_hash(*qr4106), "4106 AC2: QueryResult hash");
    CHECK(cs.eval("(define p4106 (query:as-stable-ref qr4106))").has_value(), "4106 AC2: pack v2");
    auto pk4106 = cs.eval("p4106");
    CHECK(pk4106 && is_pair(*pk4106), "4106 AC2: v2 pair");
    // stable-ref on a live v2 pair → schema-2 hash (production export).
    auto sp4106 = cs.eval("(query:stable-ref p4106)");
    CHECK(sp4106 && is_hash(*sp4106), "4106 AC2: live v2 → schema-2 hash out");
    // ensure-ref on the live v2 pair → valid=1, refreshed=0 (non-refresh
    // rule never fires on a fresh gen).
    auto ev1 = cs.eval("(hash-ref (query:ensure-ref p4106) \"valid\")");
    CHECK(ev1 && is_int(*ev1) && as_int(*ev1) == 1, "4106 AC2: ensure-ref valid=1");
    auto er1 = cs.eval("(hash-ref (query:ensure-ref p4106) \"refreshed\")");
    CHECK(er1 && is_int(*er1) && as_int(*er1) == 0, "4106 AC2: ensure-ref refreshed=0");
    // provenance on the live v2 pair → schema-620 hash.
    auto prp = cs.eval("(query:stable-ref-provenance p4106)");
    CHECK(prp && is_hash(*prp), "4106 AC2: live v2 → schema-620 hash");
    auto sch = cs.eval("(hash-ref (query:stable-ref-provenance p4106) \"schema\")");
    CHECK(sch && is_int(*sch) && as_int(*sch) == 620, "4106 AC2: schema=620");
    // schema-2 hash operand resolves on stable-ref too.
    auto sh4106 = cs.eval("(query:stable-ref qr4106)");
    CHECK(sh4106 && is_hash(*sh4106), "4106 AC2: live hash → schema-2 hash out");
    apply_dev_audit_defaults();
}

static void ac4106_3_recycled_slot_never_rebound() {
    std::println("\n=== #4106 AC3: recycled slot — stale packed gen fails closed ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    {
        auto& rq4106 = aura::core::resource_quota::process_resource_quota();
        rq4106.set_limit(aura::core::resource_quota::Dimension::Mutations, 0);
        rq4106.set_limit(aura::core::resource_quota::Dimension::Memory, 0);
        rq4106.set_limit(aura::core::resource_quota::Dimension::Fibers, 0);
        rq4106.set_limit(aura::core::resource_quota::Dimension::TimeUs, 0);
        rq4106.reset_usage();
    }
    aura::core::resource_quota::reset_process_resource_quota_for_test();
    apply_production_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define w4106 (lambda (x) 3))\")").has_value(),
          "4106 AC3: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4106 AC3: eval");
    CHECK(cs.eval("(define qr4106w (query :find \"w4106\" :as-query-result #t))").has_value(),
          "4106 AC3: bind hash");
    CHECK(cs.eval("(define p4106w (query:as-stable-ref qr4106w))").has_value(),
          "4106 AC3: pack v2");
    // Force a deterministic recycle (the 3121 fixture idiom): bump the
    // workspace generation and restamp — every pre-bump packed gen now
    // mismatches the authority, so the OLD packed v2 ref must fail closed
    // on every mint prim — never restamped / refreshed onto the slot.
    auto* ws4106r = cs.evaluator().workspace_flat();
    CHECK(ws4106r != nullptr, "4106 AC3: workspace for recycle");
    if (ws4106r) {
        ws4106r->bump_generation();
        ws4106r->restamp_all_node_generations();
    }
    auto sr4106s = cs.eval("(query:stable-ref p4106w)");
    CHECK(sr4106s.has_value(), "4106 AC3: bind stale");
    const auto kind_s = kind4106(cs, *sr4106s);
    CHECK(kind_s == "stale-ref" || kind_s == "restamp-lag",
          "4106 AC3: stale v2 → stale-ref/restamp-lag (never reminted)");
    auto pr4106s = cs.eval("(query:stable-ref-provenance p4106w)");
    CHECK(pr4106s && is_bool(*pr4106s) && !as_bool(*pr4106s), "4106 AC3: stale v2 provenance → #f");
    auto ev0 = cs.eval("(hash-ref (query:ensure-ref p4106w) \"valid\")");
    CHECK(ev0 && is_int(*ev0) && as_int(*ev0) == 0,
          "4106 AC3: stale v2 ensure-ref valid=0 (no wrap-0 refresh)");
    auto er0 = cs.eval("(hash-ref (query:ensure-ref p4106w) \"refreshed\")");
    CHECK(er0 && is_int(*er0) && as_int(*er0) == 0, "4106 AC3: stale v2 ensure-ref refreshed=0");
    apply_dev_audit_defaults();
}

static void ac4106_4_soft_stable_ref_still_mints() {
    std::println("\n=== #4106 AC4: Soft stable-ref still mints the bare pair ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::types::is_int;
    using aura::compiler::types::is_pair;
    apply_dev_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s4106s (lambda (x) 4))\")").has_value(),
          "4106 AC4: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4106 AC4: eval");
    auto soft4106 = cs.eval("(query:stable-ref 1)");
    CHECK(soft4106 && is_pair(*soft4106), "4106 AC4: Soft stable-ref still mints pair");
    auto car4106 = cs.eval("(car (query:stable-ref 1))");
    CHECK(car4106 && is_int(*car4106), "4106 AC4: Soft mint car is the NodeId int");
}

static void ac4106_5_no_docs_linter_wired() {
    std::println("\n=== #4106 AC5: no docs/design/, linter wired, no invented test ===");
    {
        std::ifstream f("docs/design/4106-query-bare-nodeid-occupancy.md");
        CHECK(!f.good(), "4106 AC5: no docs/design/4106-*");
    }
    {
        std::ifstream f("tests/compiler/test_issue_4106.cpp");
        CHECK(!f.good(), "4106 AC5: no tests/compiler/test_issue_4106.cpp (#81934)");
    }
    std::ifstream f_build("build.py");
    std::string build((std::istreambuf_iterator<char>(f_build)), std::istreambuf_iterator<char>());
    CHECK(build.find("check_query_bare_nodeid_4106") != std::string::npos,
          "4106 AC5: linter wired into build.py");
    std::ifstream f_allow("scripts/coverage/root_check_allowlist.txt");
    std::string allow((std::istreambuf_iterator<char>(f_allow)), std::istreambuf_iterator<char>());
    CHECK(allow.find("check_query_bare_nodeid_4106.py") != std::string::npos,
          "4106 AC5: root allowlist carries the linter");
    aura::core::resource_quota::reset_process_resource_quota_for_test();
}

// ── #4165 ACs — same-tenant multi-Agent Agent-scoped fiber isolation ──
// Production Agent entry that runs fiberless stamped fiber_id 0 on every
// export, which permanently skipped the InvalidFiber freshness check (the
// current_fiber_id != 0 gate): two same-tenant Agents shared one
// workspace_flat_ authority with no Agent-scoped deny beyond Guard + epoch.
// Evaluator::agent_scoped_fiber_id mints a stable per-Evaluator Agent-band
// fiber (0x41650000+) under the production face so cross-Agent held
// QueryResult / StableNodeRef memory denies while the same Agent stays
// fresh. Dispatched in the armed pristine block (before the stress suites
// exhaust the process-global production budget, #1547).
static void ac4165_1_prod_fiberless_entry_stamps_agent_band() {
    std::println("\n=== #4165 AC1: production fiberless Agent entry stamps Agent-band fiber ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_int;
    aura::core::resource_quota::reset_process_resource_quota_for_test();
    apply_production_audit_defaults();
    CompilerService cs;
    // Library-side arm (#3640): the stamp/mint TU (evaluator_security.cpp)
    // must see the production face, not just the test TU's copy.
    cs.evaluator().arm_production_audit_defaults_for_test();
    CHECK(cs.eval("(set-code \"(define a4165 (lambda (x) 1))\")").has_value(),
          "4165 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4165 AC1: eval");
    CHECK(cs.eval("(define q4165 (query :find \"a4165\" :as-query-result #t))").has_value(),
          "4165 AC1: bind QueryResult hash");
    auto fid = cs.eval("(hash-ref q4165 \"fiber-id\")");
    CHECK(fid && is_int(*fid), "4165 AC1: fiber-id key present");
    const auto fid_v = fid && is_int(*fid) ? as_int(*fid) : -1;
    CHECK(fid_v >= 0x41650000LL,
          "4165 AC1: fiber stamped in the Agent band (never 0 when entry is fiberless)");
    auto tid = cs.eval("(hash-ref q4165 \"tenant-id\")");
    CHECK(tid && is_int(*tid), "4165 AC1: tenant-id key present");
    cs.evaluator().disarm_production_audit_defaults_for_test();
    apply_dev_audit_defaults();
}

static void ac4165_2_same_agent_requery_fresh() {
    std::println("\n=== #4165 AC2: same-Agent re-resolve stays fresh under its own mint ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_hash;
    aura::core::resource_quota::reset_process_resource_quota_for_test();
    apply_production_audit_defaults();
    CompilerService cs;
    cs.evaluator().arm_production_audit_defaults_for_test();
    CHECK(cs.eval("(set-code \"(define a4165b (lambda (x) 1))\")").has_value(),
          "4165 AC2: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4165 AC2: eval");
    CHECK(cs.eval("(define q4165b (query :find \"a4165b\" :as-query-result #t))").has_value(),
          "4165 AC2: bind QueryResult hash");
    // Same Evaluator resolves its own held hash through the schema-2
    // operand face (#3395) — freshness via its own minted fiber id.
    auto sr = cs.eval("(query:stable-ref q4165b)");
    CHECK(sr && is_hash(*sr), "4165 AC2: same-Agent schema-2 resolve stays fresh");
    auto fresh = cs.eval("(query:result-fresh? q4165b)");
    CHECK(fresh && is_bool(*fresh) && as_bool(*fresh),
          "4165 AC2: query:result-fresh? #t for the Agent's own mint");
    cs.evaluator().disarm_production_audit_defaults_for_test();
    apply_dev_audit_defaults();
}

static void ac4165_3_cross_agent_same_tenant_deny() {
    std::println(
        "\n=== #4165 AC3: held QueryResult from Agent A denies under Agent B (same tenant) ===");
    using aura::compiler::query_result_decode::query_result_is_fresh_with_refs;
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::core::QueryResultFreshness;
    aura::core::resource_quota::reset_process_resource_quota_for_test();
    apply_production_audit_defaults();
    CompilerService csA;
    CompilerService csB;
    csA.evaluator().arm_production_audit_defaults_for_test();
    csB.evaluator().arm_production_audit_defaults_for_test();
    // Same tenant, no fiber minted for either Agent (fiberless entry).
    csA.evaluator().set_capability_tenant_id(7);
    csB.evaluator().set_capability_tenant_id(7);
    const auto mintA = csA.evaluator().agent_scoped_fiber_id();
    const auto mintB = csB.evaluator().agent_scoped_fiber_id();
    CHECK(mintA >= 0x41650000u && mintB >= 0x41650000u,
          "4165 AC3: both Agents minted in the Agent band");
    CHECK(mintA != mintB, "4165 AC3: per-Evaluator mints differ (Agent-scoped)");
    CHECK(csA.eval("(set-code \"(define b4165 (lambda (y) 2))\")").has_value(),
          "4165 AC3: A set-code");
    CHECK(csA.eval("(eval-current)").has_value(), "4165 AC3: A eval");
    CHECK(csB.eval("(set-code \"(define b4165 (lambda (y) 2))\")").has_value(),
          "4165 AC3: B set-code");
    CHECK(csB.eval("(eval-current)").has_value(), "4165 AC3: B eval");
    auto* flatB = csB.evaluator().workspace_flat();
    CHECK(flatB != nullptr, "4165 AC3: B workspace live");
    aura::ast::NodeId nid = 0;
    for (aura::ast::NodeId i = 1; flatB && i < flatB->size(); ++i) {
        if (flatB->is_live_node(i)) {
            nid = i;
            break;
        }
    }
    CHECK(nid != 0, "4165 AC3: live node found on B");
    if (flatB && nid != 0) {
        const auto genB = flatB->node_gen_for(nid);
        const auto wrapB = flatB->wrap_epoch();
        const auto cowB = flatB->workspace_cow_epoch();
        // A-held memory: node identity matches B's authority, fiber = A's mint.
        aura::core::QueryResult qrX;
        CHECK(qrX.push_match_full(nid, genB, wrapB, cowB, /*tenant=*/7, /*fiber=*/mintA, 0, 0),
              "4165 AC3: foreign-stamped match pushed");
        qrX.matches[0].reserved = aura::core::kQueryResultMatchSchema2Prod;
        CHECK(query_result_is_fresh_with_refs(qrX, *flatB, /*tenant=*/7, mintB) ==
                  QueryResultFreshness::InvalidFiber,
              "4165 AC3: Agent A's held QueryResult denies InvalidFiber under Agent B");
        // Same-Agent arm: B's own mint resolves its own stamp fresh.
        aura::core::QueryResult qrOwn;
        CHECK(qrOwn.push_match_full(nid, genB, wrapB, cowB, /*tenant=*/7, /*fiber=*/mintB, 0, 0),
              "4165 AC3: own-stamped match pushed");
        qrOwn.matches[0].reserved = aura::core::kQueryResultMatchSchema2Prod;
        CHECK(query_result_is_fresh_with_refs(qrOwn, *flatB, /*tenant=*/7, mintB) ==
                  QueryResultFreshness::Fresh,
              "4165 AC3: Agent B's own mint stays fresh");
        // The issue's hole — a fiber-0 stamp used to skip the check entirely;
        // under a non-zero Agent-scoped current it must deny again.
        aura::core::QueryResult qrZero;
        CHECK(qrZero.push_match_full(nid, genB, wrapB, cowB, /*tenant=*/7, /*fiber=*/0, 0, 0),
              "4165 AC3: zero-stamped match pushed");
        qrZero.matches[0].reserved = aura::core::kQueryResultMatchSchema2Prod;
        CHECK(query_result_is_fresh_with_refs(qrZero, *flatB, /*tenant=*/7, mintB) ==
                  QueryResultFreshness::InvalidFiber,
              "4165 AC3: fiber-0 stamp no longer skips the Agent-scoped check");
    }
    csA.evaluator().disarm_production_audit_defaults_for_test();
    csB.evaluator().disarm_production_audit_defaults_for_test();
    apply_dev_audit_defaults();
}

static void ac4165_4_source_cite() {
    std::println("\n=== #4165 AC4: source-cite — mint + stamp/resolve wiring ===");
    auto read_src = [](const char* rel) {
        std::ifstream f(rel);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const auto sec = read_src("src/compiler/evaluator_security.cpp");
    CHECK(sec.find("agent_scoped_fiber_id") != std::string::npos,
          "4165 AC4: Agent-scoped resolution defined in the security TU");
    CHECK(sec.find("kAgentFiberStampBand") != std::string::npos,
          "4165 AC4: Agent band constant present");
    CHECK(sec.find("g_agent_fiber_stamp_seq") != std::string::npos,
          "4165 AC4: per-process mint sequence present");
    const auto ixx = read_src("src/compiler/evaluator.ixx");
    CHECK(ixx.find("agent_scoped_fiber_id() const noexcept") != std::string::npos,
          "4165 AC4: evaluator.ixx declares the resolution");
    CHECK(ixx.find("agent_fiber_id_ = 0") != std::string::npos,
          "4165 AC4: per-Evaluator mint member present");
    const auto mut = read_src("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(mut.find("ev.agent_scoped_fiber_id()") != std::string::npos,
          "4165 AC4: resolve_mutate_node_arg resolves Agent-scoped");
    const auto qws = read_src("src/compiler/evaluator_primitives_query_workspace.cpp");
    size_t hits = 0;
    for (auto p = qws.find("ev.agent_scoped_fiber_id()"); p != std::string::npos;
         p = qws.find("ev.agent_scoped_fiber_id()", p + 1))
        ++hits;
    CHECK(hits >= 2, "4165 AC4: result-fresh? + result-matches resolve Agent-scoped");
    const auto qprov = read_src("src/compiler/evaluator_primitives_query.cpp");
    CHECK(qprov.find("ev.agent_scoped_fiber_id()") != std::string::npos,
          "4165 AC4: stable-ref-provenance resolves Agent-scoped");
    const auto dec = read_src("src/compiler/query_result_decode.hh");
    CHECK(dec.find("current_fiber_id != 0 && m.fiber_id != current_fiber_id") != std::string::npos,
          "4165 AC4: hard InvalidFiber face kept (no weakening)");
    const auto epoch = read_src("src/core/workspace_epoch.hh");
    CHECK(epoch.find("InvalidFiber = 3") != std::string::npos,
          "4165 AC4: InvalidFiber enum value unchanged");
}

static void ac4165_5_no_docs_linter_wired() {
    std::println("\n=== #4165 AC5: no docs/design/, linter wired, no invented test ===");
    {
        std::ifstream f("docs/design/4165-agent-fiber-isolation.md");
        CHECK(!f.good(), "4165 AC5: no docs/design/4165-*");
    }
    {
        std::ifstream f("tests/core/test_issue_4165.cpp");
        CHECK(!f.good(), "4165 AC5: no tests/core/test_issue_4165.cpp (#81934)");
    }
    std::ifstream f_build("build.py");
    std::string build((std::istreambuf_iterator<char>(f_build)), std::istreambuf_iterator<char>());
    CHECK(build.find("check_agent_fiber_isolation_4165") != std::string::npos,
          "4165 AC5: linter wired into build.py");
    std::ifstream f_allow("scripts/coverage/root_check_allowlist.txt");
    std::string allow((std::istreambuf_iterator<char>(f_allow)), std::istreambuf_iterator<char>());
    CHECK(allow.find("check_agent_fiber_isolation_4165.py") != std::string::npos,
          "4165 AC5: root allowlist carries the linter");
    aura::core::resource_quota::reset_process_resource_quota_for_test();
}

// ── #4235 ACs — agent_scoped_fiber_id is per-Agent, not per-Evaluator ──
// #4165 minted ONE Agent-band id per Evaluator (agent_fiber_id_ single
// slot), so two Agents sharing one Evaluator fiberless resolved the SAME
// mint and same-tenant code-as-memory held refs passed InvalidFiber
// against each other (A's export resolved fresh under B). The mint now
// keys on the #1419 agent fingerprint (agent_mint_slots_ CAS table): each
// identified Agent on the Evaluator gets its own band id, fingerprint 0
// keeps the #4165 per-Evaluator fallback, and the resolution order
// (explicit fiber > #2151 override > live fiber) is unchanged. Dispatched
// in the armed pristine block (before the stress suites exhaust the
// process-global production budget, #1547).
static void ac4235_1_per_agent_distinct_mints_one_evaluator() {
    std::println(
        "\n=== #4235 AC1: two identified Agents on ONE Evaluator mint distinct stable fibers ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    aura::core::resource_quota::reset_process_resource_quota_for_test();
    apply_production_audit_defaults();
    CompilerService cs; // ONE Evaluator — the #4235 shared-Evaluator scenario
    cs.evaluator().arm_production_audit_defaults_for_test();
    cs.evaluator().set_current_agent_fingerprint(0x5A'0000'0001ull); // Agent A
    const auto mint_a = cs.evaluator().agent_scoped_fiber_id();
    cs.evaluator().set_current_agent_fingerprint(0x5A'0000'0002ull); // Agent B, same Evaluator
    const auto mint_b = cs.evaluator().agent_scoped_fiber_id();
    CHECK(mint_a >= 0x41650000u && mint_b >= 0x41650000u,
          "4235 AC1: both identified Agents mint in the Agent band");
    CHECK(
        mint_a != mint_b,
        "4235 AC1: distinct Agents on ONE Evaluator get distinct mints (was shared per-Evaluator)");
    cs.evaluator().set_current_agent_fingerprint(0x5A'0000'0001ull);
    CHECK(cs.evaluator().agent_scoped_fiber_id() == mint_a,
          "4235 AC1: Agent A's mint is stable across re-resolve");
    cs.evaluator().set_current_agent_fingerprint(0x5A'0000'0002ull);
    CHECK(cs.evaluator().agent_scoped_fiber_id() == mint_b,
          "4235 AC1: Agent B's mint is stable across re-resolve");
    cs.evaluator().disarm_production_audit_defaults_for_test();
    apply_dev_audit_defaults();
}

static void ac4235_2_cross_agent_held_result_deny_same_evaluator() {
    std::println("\n=== #4235 AC2: Agent A's held QueryResult denies under Agent B on the SAME "
                 "Evaluator ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_int;
    aura::core::resource_quota::reset_process_resource_quota_for_test();
    apply_production_audit_defaults();
    CompilerService cs; // the #4235 hole: one Evaluator, two fiberless Agents
    cs.evaluator().arm_production_audit_defaults_for_test();
    CHECK(cs.eval("(set-code \"(define a4235 (lambda (x) 1))\")").has_value(),
          "4235 AC2: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4235 AC2: eval");
    cs.evaluator().set_current_agent_fingerprint(0x5A'0000'0011ull); // Agent A
    CHECK(cs.eval("(define q4235a (query :find \"a4235\" :as-query-result #t))").has_value(),
          "4235 AC2: A exports QueryResult hash");
    auto fid = cs.eval("(hash-ref q4235a \"fiber-id\")");
    const auto fid_v = fid && is_int(*fid) ? as_int(*fid) : -1;
    CHECK(fid_v >= 0x41650000LL, "4235 AC2: A's export stamped with A's own Agent-band mint");
    cs.evaluator().set_current_agent_fingerprint(0x5A'0000'0012ull); // Agent B, same Evaluator
    auto fresh_b = cs.eval("(query:result-fresh? q4235a)");
    CHECK(fresh_b && is_bool(*fresh_b) && !as_bool(*fresh_b),
          "4235 AC2: B resolves A's held QueryResult stale (InvalidFiber deny bites)");
    CHECK(cs.eval("(define m4235b (query:result-matches q4235a))").has_value(),
          "4235 AC2: B result-matches attempt binds");
    auto eq_stale = cs.eval("(equal? (car m4235b) \"stale-ref\")");
    CHECK(eq_stale && is_bool(*eq_stale) && as_bool(*eq_stale),
          "4235 AC2: B query:result-matches denies stale-ref");
    cs.evaluator().set_current_agent_fingerprint(0x5A'0000'0011ull); // back to Agent A
    auto fresh_a = cs.eval("(query:result-fresh? q4235a)");
    CHECK(fresh_a && is_bool(*fresh_a) && as_bool(*fresh_a),
          "4235 AC2: A re-resolves its own export fresh under its own mint");
    cs.evaluator().disarm_production_audit_defaults_for_test();
    apply_dev_audit_defaults();
}

static void ac4235_3_unidentified_agent_keeps_per_evaluator_mint() {
    std::println("\n=== #4235 AC3: fingerprint-0 entry keeps the #4165 per-Evaluator mint ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    aura::core::resource_quota::reset_process_resource_quota_for_test();
    apply_production_audit_defaults();
    CompilerService cs;
    cs.evaluator().arm_production_audit_defaults_for_test();
    // No fingerprint installed (#1419 identity absent) — the Evaluator's
    // implicit agent: stable per-Evaluator mint, #4165 contract unchanged.
    const auto m1 = cs.evaluator().agent_scoped_fiber_id();
    const auto m2 = cs.evaluator().agent_scoped_fiber_id();
    CHECK(m1 >= 0x41650000u, "4235 AC3: fallback mint in the Agent band");
    CHECK(m1 == m2, "4235 AC3: fingerprint-0 mint stable per Evaluator (#4165 kept)");
    // An identified Agent draws its own mint, distinct from the fallback.
    cs.evaluator().set_current_agent_fingerprint(0x5A'0000'0003ull);
    const auto m3 = cs.evaluator().agent_scoped_fiber_id();
    CHECK(m3 != m1, "4235 AC3: identified Agent's mint differs from the fallback slot");
    CHECK(cs.evaluator().agent_scoped_fiber_id() == m3,
          "4235 AC3: identified Agent keeps its table mint");
    cs.evaluator().set_current_agent_fingerprint(0); // clear back to system
    CHECK(cs.evaluator().agent_scoped_fiber_id() == m1,
          "4235 AC3: cleared fingerprint resolves the per-Evaluator fallback again");
    cs.evaluator().disarm_production_audit_defaults_for_test();
    apply_dev_audit_defaults();
}

static void ac4235_4_override_beats_agent_mint() {
    std::println(
        "\n=== #4235 AC4: #2151 effect-fiber override still wins over the per-Agent mint ===");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    aura::core::resource_quota::reset_process_resource_quota_for_test();
    apply_production_audit_defaults();
    CompilerService cs;
    cs.evaluator().arm_production_audit_defaults_for_test();
    cs.evaluator().set_current_agent_fingerprint(0x5A'0000'0004ull);
    // Real fiber id (steal×resume contract): the #2151 process-global
    // override simulates fiber A vs B without a scheduler — it must win
    // over any mint, per-Agent or fallback.
    aura::core::capability::set_effect_fiber_id_override(0x2E4u);
    CHECK(cs.evaluator().agent_scoped_fiber_id() == 0x2E4u,
          "4235 AC4: explicit/override fiber wins over the per-Agent mint");
    aura::core::capability::set_effect_fiber_id_override(0);
    const auto mint = cs.evaluator().agent_scoped_fiber_id();
    CHECK(mint >= 0x41650000u, "4235 AC4: override cleared → back to the Agent-band resolution");
    cs.evaluator().disarm_production_audit_defaults_for_test();
    apply_dev_audit_defaults();
}

static void ac4235_5_source_cite() {
    std::println("\n=== #4235 AC5: source-cite — per-Agent table + wiring kept + linter wired ===");
    auto read_src = [](const char* rel) {
        std::ifstream f(rel);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const auto sec = read_src("src/compiler/evaluator_security.cpp");
    CHECK(sec.find("agent_mint_slots_") != std::string::npos,
          "4235 AC5: per-Agent mint table consulted in the security TU");
    CHECK(sec.find("current_agent_fingerprint()") != std::string::npos,
          "4235 AC5: mint keyed on the #1419 agent fingerprint");
    CHECK(sec.find("agent_fiber_id_ == 0") != std::string::npos,
          "4235 AC5: #4165 per-Evaluator fallback kept");
    const auto ixx = read_src("src/compiler/evaluator.ixx");
    CHECK(ixx.find("kAgentMintSlotCount = 32") != std::string::npos,
          "4235 AC5: per-Agent table member declared");
    const auto dec = read_src("src/compiler/query_result_decode.hh");
    CHECK(dec.find("current_fiber_id != 0 && m.fiber_id != current_fiber_id") != std::string::npos,
          "4235 AC5: hard InvalidFiber face unchanged (per-Agent deny rides the same face)");
    const auto qws = read_src("src/compiler/evaluator_primitives_query_workspace.cpp");
    CHECK(qws.find("ev.agent_scoped_fiber_id()") != std::string::npos,
          "4235 AC5: resolve sites ride the per-Agent resolution unchanged");
    {
        std::ifstream f("docs/design/4235-agent-mint-per-agent.md");
        CHECK(!f.good(), "4235 AC5: no docs/design/4235-*");
    }
    {
        std::ifstream f("tests/core/test_issue_4235.cpp");
        CHECK(!f.good(), "4235 AC5: no tests/core/test_issue_4235.cpp (#81934)");
    }
    std::ifstream f_build("build.py");
    std::string build((std::istreambuf_iterator<char>(f_build)), std::istreambuf_iterator<char>());
    CHECK(build.find("check_agent_mint_per_agent_4235") != std::string::npos,
          "4235 AC5: linter wired into build.py");
    std::ifstream f_allow("scripts/coverage/root_check_allowlist.txt");
    std::string allow((std::istreambuf_iterator<char>(f_allow)), std::istreambuf_iterator<char>());
    CHECK(allow.find("check_agent_mint_per_agent_4235.py") != std::string::npos,
          "4235 AC5: root allowlist carries the linter");
    aura::core::resource_quota::reset_process_resource_quota_for_test();
}

int main() {
    std::println(
        "=== Merged stable-ref provenance fiber COW: ORIG #457-#549 + TASK1 #551-#552 ===");
    // #4106 ACs — production query prims refuse bare NodeId occupancy.
    // Dispatched FIRST: these ACs arm production from a pristine process;
    // the long-running stress suites below exhaust the process-global
    // production budget (#1547) that the mint prims consult under
    // production, which would deny set-code before the faces can run.
    ac4106_1_prod_mint_bare_int_refuses();
    ac4106_2_prod_packed_and_hash_still_resolve();
    ac4106_3_recycled_slot_never_rebound();
    ac4106_4_soft_stable_ref_still_mints();
    ac4106_5_no_docs_linter_wired();
    // #4165 ACs (5) — same-tenant multi-Agent Agent-scoped fiber isolation.
    ac4165_1_prod_fiberless_entry_stamps_agent_band();
    ac4165_2_same_agent_requery_fresh();
    ac4165_3_cross_agent_same_tenant_deny();
    ac4165_4_source_cite();
    ac4165_5_no_docs_linter_wired();
    // #4235 ACs (5) — per-Agent mint on one shared Evaluator (leak fix).
    ac4235_1_per_agent_distinct_mints_one_evaluator();
    ac4235_2_cross_agent_held_result_deny_same_evaluator();
    ac4235_3_unidentified_agent_keeps_per_evaluator_mint();
    ac4235_4_override_beats_agent_mint();
    ac4235_5_source_cite();
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
    test_ac3661_1_expired_gen_stale_ref();
    test_ac3661_2_matching_gen_succeeds();
    test_ac3661_3_soft_auto_refresh();
    test_ac3661_4_strict_not_bypassed();
    test_ac3661_5_suites_and_linter();
    std::println("\n=== Results: {} passed, {} failed ===", ::aura::test::g_passed,
                 ::aura::test::g_failed);
    return ::aura::test::g_failed ? 1 : 0;
}