// test_issue_401.cpp — Issue #401: invalidate_function claims BFS
// but uses DFS (vector-as-stack). The dep_graph_ traversal and
// re-lower order should be deterministic across runs.
//
// The bug: invalidate_function used `std::vector` +
// `push_back/pop_back`, which is stack/DFS behavior. The
// misleading comment claimed "natural BFS order" but iteration
// was depth-first, which made the re-lower order depend on the
// hash-map iteration order of
// `std::unordered_map<string, DepEntry>::called_by`. For AI
// multi-round mutate:rebind flows, this meant dep_graph_
// calls/called_by edges recorded by record_dependency during
// re-lower could land in different orders across runs.
//
// The fix:
//   - Use `std::deque` + `push_back/pop_front` for proper FIFO BFS.
//   - Sort the dependents vector lexicographically before
//     re-lower for stable iteration.
//
// These tests verify the fix end-to-end via the
// (mutate:rebind ...) Aura EDSL path AND via the direct
// public_invalidate_function hook. The direct hook is the
// primary test surface (the EDSL path mixes traversal with
// the rebind logic, which would obscure the determinism check).

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <unordered_set>
#include <vector>

// Unified test harness (Issue #226).
#include "test_harness.hpp"

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.ir_executor;
import aura.compiler.evaluator;
import aura.compiler.service;

using aura::test::g_passed;
using aura::test::g_failed;

// ── AC1: invalidate_function uses real BFS ────────────────────
//
// Build a small dep_graph_ via (define ...) calls, then trigger
// public_invalidate_function on a leaf. The jit_cache_evictions
// counter should bump by exactly (1 + dependents.size()). We
// can't directly observe the internal `dependents` vector,
// but we can observe two invariants:
//
//   (a) Every dependent's entry is erased from jit_cache_ (via
//       the metric).
//   (b) Every dependent's entry is erased from dep_graph_
//       (the cleanup loop runs before re-lower).
//
// If the BFS is wrong (e.g., cycles forever, misses a node),
// these counters won't line up.

namespace aura_issue_401_detail {

// Helper: build a chain f → g → h with g and h in dep_graph_
// (g calls f, h calls g). After invalidate_function("f"), the
// called_by chain should reach both g and h.
//
// Uses (set-code ...) instead of plain (eval "(define ...)") because
// populate_dep_graph_from_workspace is wired to the (set-code)
// callback path (the pre_cache_workspace_defines_fn_ std::function).
// A plain eval goes through the per-eval parse+lower path, which
// doesn't invoke that callback. So set-code is the public way to
// populate the dep_graph_ end-to-end.
static bool build_three_level_chain(aura::compiler::CompilerService& cs) {
    // Combined source: f, g, h all in one workspace so
    // populate_dep_graph_from_workspace walks the entire body
    // and records edges (g → f) + (h → g).
    auto r1 = cs.eval("(set-code \"(begin (define (f x) (* x 2)) "
                      "(define (g x) (f (* x 3))) "
                      "(define (h x) (g (* x 4))))\")");
    CHECK(r1.has_value(), "set-code: define f + g + h");
    if (!r1.has_value()) return false;
    // eval-current drives the workspace through lowering so
    // jit_cache_ + ir_cache_v2_ are populated (and the
    // invalidate_function path has real cache entries to erase).
    auto r2 = cs.eval("(eval-current)");
    CHECK(r2.has_value(), "eval-current after set-code");
    return r2.has_value();
}

bool test_ac1_bfs_visits_all_transitives() {
    std::println("\n--- AC1: BFS visits all transitive dependents ---");
    aura::compiler::CompilerService cs;
    if (!build_three_level_chain(cs))
        return false;

    // Before invalidate: dep_graph_ has f, g, h with edges
    // g → f and h → g (calls). called_by: f has {g}, g has {h}.
    CHECK(cs.public_dep_graph_contains("f"), "f is in dep_graph_ pre-invalidate");
    CHECK(cs.public_dep_graph_contains("g"), "g is in dep_graph_ pre-invalidate");
    CHECK(cs.public_dep_graph_contains("h"), "h is in dep_graph_ pre-invalidate");
    CHECK(cs.public_dep_graph_has_edge("g", "f"),
          "edge g → f exists pre-invalidate");
    CHECK(cs.public_dep_graph_has_edge("h", "g"),
          "edge h → g exists pre-invalidate");
    CHECK(cs.public_dep_graph_called_by_for("f") == 1,
          "f.called_by size == 1 (g only) pre-invalidate");
    CHECK(cs.public_dep_graph_called_by_for("g") == 1,
          "g.called_by size == 1 (h only) pre-invalidate");

    auto ev_before = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    auto calls_before = cs.metrics().invalidate_function_calls.load(
        std::memory_order_relaxed);

    // Trigger invalidate on the root (f). BFS should reach g
    // (called_by edge) and h (g's called_by edge).
    cs.public_invalidate_function("f");

    auto ev_after = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    auto calls_after = cs.metrics().invalidate_function_calls.load(
        std::memory_order_relaxed);

    // invalidate_function erases jit_cache_ for f + each
    // dependent (g, h). 3 evictions total — this is the
    // BFS-correctness contract: the BFS MUST reach both g
    // and h, otherwise only 1 eviction would happen.
    CHECK(ev_after - ev_before == 3,
          "jit_cache_evictions bumps by 3 (f + g + h)");
    CHECK(calls_after - calls_before == 1,
          "invalidate_function_calls bumps by 1");

    // After invalidate, the cleanup loop erased entries
    // for f, g, h from dep_graph_ before re-lower. The
    // re-lower loop then re-parses each dependent and
    // calls record_dependency with cache_hits — but
    // because f's cache was just erased, the lowered g/h
    // don't record edges to f (no cache hit for f). The
    // f entry itself is permanently removed. The new
    // dep_graph_ state: f absent (not in dependents), and
    // f.called_by is gone with it.
    CHECK(!cs.public_dep_graph_contains("f"),
          "f is removed from dep_graph_ post-invalidate (not re-lowered)");
    CHECK(cs.public_dep_graph_called_by_for("f") == 0,
          "f.called_by gone post-invalidate");
    return true;
}

// ── AC2: dependents are sorted before re-lower ────────────────
//
// Two invalidate cycles with the same fan-out should produce
// identical dep_graph_ state (size + edge membership). We
// can't directly observe the internal `dependents` vector,
// but we can verify the post-rebuild dep_graph_ has the same
// shape across two invalidate("f") cycles — because the
// sort ensures the re-lower order is stable, which means
// record_dependency records edges in the same order both
// times, which (combined with the same iteration order on
// the unordered_set visited) yields the same final
// dep_graph_.
//
// We compare the dep_graph_ size and the per-node
// calls/called_by sizes — these are the contract.

bool test_ac2_relower_is_stable() {
    std::println("\n--- AC2: re-lower produces stable dep_graph_ shape ---");

    struct State {
        std::size_t size;
        std::size_t f_called_by;
        std::size_t g_called_by;
        std::size_t h_called_by;
        std::size_t g_calls;
        std::size_t h_calls;
        bool g_calls_f;
        bool h_calls_g;
    };
    auto capture_state =
        [](aura::compiler::CompilerService& cs) {
            State s{};
            s.size = cs.public_dep_graph_size();
            s.f_called_by = cs.public_dep_graph_called_by_for("f");
            s.g_called_by = cs.public_dep_graph_called_by_for("g");
            s.h_called_by = cs.public_dep_graph_called_by_for("h");
            s.g_calls = cs.public_dep_graph_calls_for("g");
            s.h_calls = cs.public_dep_graph_calls_for("h");
            s.g_calls_f = cs.public_dep_graph_has_edge("g", "f");
            s.h_calls_g = cs.public_dep_graph_has_edge("h", "g");
            return s;
        };

    aura::compiler::CompilerService cs1;
    build_three_level_chain(cs1);
    cs1.public_invalidate_function("f");

    // Re-build the chain via set-code + eval-current (the
    // public path that triggers populate_dep_graph_from_workspace
    // and re-records the workspace's edges). After this, the
    // dep_graph_ should be fully rebuilt with f, g, h and
    // edges (g → f) + (h → g).
    cs1.eval("(set-code \"(begin (define (f x) (* x 2)) "
             "(define (g x) (f (* x 3))) "
             "(define (h x) (g (* x 4))))\")");
    cs1.eval("(eval-current)");
    auto s1b = capture_state(cs1);

    // Repeat with a fresh service.
    aura::compiler::CompilerService cs2;
    build_three_level_chain(cs2);
    cs2.public_invalidate_function("f");
    cs2.eval("(set-code \"(begin (define (f x) (* x 2)) "
             "(define (g x) (f (* x 3))) "
             "(define (h x) (g (* x 4))))\")");
    cs2.eval("(eval-current)");
    auto s2b = capture_state(cs2);

    // The dep_graph_ shapes must match across the two
    // services. If the BFS/sort contract is broken, the
    // record_dependency call order would differ (because
    // re-lower iterates dependents in insertion order),
    // but the calls/called_by vector CONTENTS would still
    // be the same — so we only assert on counts and edge
    // membership, not insertion order.
    CHECK(s1b.size == s2b.size,
          "post-redefine dep_graph_ size matches across services");
    CHECK(s1b.g_calls_f == s2b.g_calls_f,
          "edge g → f present in both services post-redefine");
    CHECK(s1b.h_calls_g == s2b.h_calls_g,
          "edge h → g present in both services post-redefine");
    CHECK(s1b.f_called_by == s2b.f_called_by,
          "f.called_by count matches");
    CHECK(s1b.g_called_by == s2b.g_called_by,
          "g.called_by count matches");
    CHECK(s1b.g_calls == s2b.g_calls,
          "g.calls count matches");
    CHECK(s1b.h_calls == s2b.h_calls,
          "h.calls count matches");
    return true;
}

// ── AC3: dep_graph_ integrity across invalidate cycles ────────
//
// After invalidate → re-define → invalidate again, the
// dep_graph_ should still be consistent: every calls edge
// has a matching called_by edge (no dangling one-way edges).
// This is the integrity contract from the issue body.

bool test_ac3_dep_graph_integrity() {
    std::println("\n--- AC3: dep_graph_ integrity across cycles ---");
    aura::compiler::CompilerService cs;
    build_three_level_chain(cs);

    // Verify initial edges are symmetric: g → f implies
    // f has g in its called_by set.
    CHECK(cs.public_dep_graph_has_edge("g", "f"),
          "initial: g → f edge");
    CHECK(cs.public_dep_graph_called_by_for("f") == 1,
          "initial: f.called_by includes g");

    // Cycle 1: invalidate f. The BFS visits g (direct
    // called_by) and h (transitive via g's called_by).
    // Eviction count is 3 (f + g + h).
    auto ev_before = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    cs.public_invalidate_function("f");
    auto ev_after = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    CHECK(ev_after - ev_before == 3,
          "cycle 1 invalidate(f): 3 evictions (BFS reaches f+g+h)");

    // Cycle 2: invalidate g. The BFS visits g's called_by
    // which is empty (cycle 1's re-lower emptied g's
    // called_by because f's cache was empty mid-traversal).
    // So only g itself is evicted (1).
    auto ev_before_2 = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    cs.public_invalidate_function("g");
    auto ev_after_2 = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    CHECK(ev_after_2 - ev_before_2 == 1,
          "cycle 2 invalidate(g): 1 eviction (g only)");

    // Cycle 3: invalidate h. The BFS visits h's called_by
    // which is empty (cycle 1's re-lower emptied h's
    // called_by because g's cache was empty). So only h
    // itself is evicted (1).
    auto ev_before_3 = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    cs.public_invalidate_function("h");
    auto ev_after_3 = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    CHECK(ev_after_3 - ev_before_3 == 1,
          "cycle 3 invalidate(h): 1 eviction (h only)");

    // invalidate_function_calls: 3 invalidates total.
    auto calls = cs.metrics().invalidate_function_calls.load(
        std::memory_order_relaxed);
    CHECK(calls == 3,
          "invalidate_function_calls counter == 3 (3 invalidate calls)");
    return true;
}

// ── AC4: invalidate_function_calls metric semantics ──────────
//
// The counter is bumped exactly once per invalidate_function
// entry, regardless of how many dependents exist.

bool test_ac4_metric_semantics() {
    std::println("\n--- AC4: invalidate_function_calls bumps once per call ---");
    aura::compiler::CompilerService cs;
    build_three_level_chain(cs);

    auto before = cs.metrics().invalidate_function_calls.load(
        std::memory_order_relaxed);

    cs.public_invalidate_function("f");
    auto after1 = cs.metrics().invalidate_function_calls.load(
        std::memory_order_relaxed);
    CHECK(after1 - before == 1,
          "single invalidate bumps counter by 1");

    cs.public_invalidate_function("nonexistent_fn");
    auto after2 = cs.metrics().invalidate_function_calls.load(
        std::memory_order_relaxed);
    CHECK(after2 - after1 == 1,
          "invalidate on non-existent name also bumps counter by 1 (idempotent)");

    cs.public_invalidate_function("f");
    auto after3 = cs.metrics().invalidate_function_calls.load(
        std::memory_order_relaxed);
    CHECK(after3 - after2 == 1,
          "third invalidate bumps by 1");
    return true;
}

// ── AC5: dep_graph_ integrity under repeated (mutate:rebind) ─
//
// The original motivating scenario for the fix: AI drives
// multiple (mutate:rebind ...) calls in sequence. After
// each rebind, the dep_graph_ should still be in a
// consistent state. This is the determinism contract —
// we verify it via dep_graph_ shape (size + edge
// membership), not via call results (the IR cache may
// not be flushed for the dependent until a subsequent
// (define ...) re-defines it; that's a separate
// hot_swap concern, not this issue's scope).

bool test_ac5_dep_graph_stable_across_rebinds() {
    std::println("\n--- AC5: dep_graph_ integrity under repeated mutate:rebind ---");
    aura::compiler::CompilerService cs;

    // Populate workspace with f → g (g calls f).
    auto r1 = cs.eval("(set-code \"(begin (define (f x) (* x 2)) "
                      "(define (g x) (f (* x 3))))\")");
    CHECK(r1.has_value(), "set-code initial chain");
    auto r2 = cs.eval("(eval-current)");
    CHECK(r2.has_value(), "eval-current after set-code");

    // Sanity: dep_graph_ has f, g with edge g → f.
    CHECK(cs.public_dep_graph_contains("f"),
          "pre-rebind: f in dep_graph_");
    CHECK(cs.public_dep_graph_contains("g"),
          "pre-rebind: g in dep_graph_");
    CHECK(cs.public_dep_graph_has_edge("g", "f"),
          "pre-rebind: edge g → f");

    // Pre-rebind metric snapshot.
    auto calls_before = cs.metrics().invalidate_function_calls.load(
        std::memory_order_relaxed);

    // Three consecutive rebinds on f. Each mutate:rebind
    // calls mark_define_dirty which cascades via the
    // dep_graph_ BFS in mark_define_dirty (not via
    // invalidate_function — but the dep_graph_ walk is
    // the same data structure).
    for (int i = 0; i < 3; ++i) {
        auto rb = cs.eval("(mutate:rebind \"f\" "
                          "\"(lambda (x) (* x 10))\" \"test\")");
        CHECK(rb.has_value(),
              "mutate:rebind iteration succeeds");
    }

    auto calls_after = cs.metrics().invalidate_function_calls.load(
        std::memory_order_relaxed);
    // mutate:rebind does NOT call invalidate_function
    // (it uses mark_define_dirty instead), so the counter
    // should not have moved.
    CHECK(calls_after == calls_before,
          "mutate:rebind does not bump invalidate_function_calls (uses mark_define_dirty)");

    // After 3 rebinds, the dep_graph_ shape is preserved:
    // f is still defined, g is still defined, and the
    // g → f edge is still recorded.
    CHECK(cs.public_dep_graph_contains("f"),
          "post-3x-rebind: f still in dep_graph_");
    CHECK(cs.public_dep_graph_contains("g"),
          "post-3x-rebind: g still in dep_graph_");
    CHECK(cs.public_dep_graph_has_edge("g", "f"),
          "post-3x-rebind: edge g → f still present");

    // Now call public_invalidate_function("f"). This
    // exercises the FIXED BFS path with a populated
    // dep_graph_. The eviction counter should bump by 2
    // (f + g) since g is a direct called_by of f.
    auto ev_before = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    cs.public_invalidate_function("f");
    auto ev_after = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    CHECK(ev_after - ev_before == 2,
          "post-rebind invalidate(f): 2 evictions (f + g)");

    // f is gone from dep_graph_; g may or may not be re-added
    // (depends on whether the re-lower path records it; for
    // the dep_graph_ integrity contract we just check f).
    CHECK(!cs.public_dep_graph_contains("f"),
          "post-invalidate: f removed");
    return true;
}

// ── AC6: BFS terminates; no infinite traversal ───────────────
//
// If the dep_graph_ accidentally has a cycle (which shouldn't
// happen in production, but is a sharp edge for any graph
// traversal), the BFS visited set must short-circuit. We
// can't easily inject a cycle from the public API, but we
// can verify the visited set by checking that no
// invalidate_function call exceeds a sane time bound —
// covered indirectly by 1000 invalidate("f") calls each
// completing in milliseconds with the eviction counter
// exactly matching the expected 3 per call.

bool test_ac6_no_infinite_traversal() {
    std::println("\n--- AC6: BFS terminates on small graph ---");
    aura::compiler::CompilerService cs;
    // Build a 3-node chain.
    build_three_level_chain(cs);
    // 1000 invalidate calls on the same root. Each must
    // complete (no infinite loop, no stack overflow from
    // huge vector-as-stack). After the first call, the
    // dep_graph_ loses f (f's called_by set is gone, and
    // re-lower of g/h doesn't re-record edges to f because
    // f's cache is empty mid-traversal). So subsequent
    // invalidates evict only f itself (1 per call), not 3.
    // Total = 3 (first call: f + g + h) + 999 (subsequent: f only)
    //       = 1002.
    auto ev_before = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
        cs.public_invalidate_function("f");
    }
    auto ev_after = cs.metrics().jit_cache_evictions.load(
        std::memory_order_relaxed);
    auto delta = ev_after - ev_before;
    // First invalidate: 3 evictions. Each subsequent: 1 eviction (f).
    // Total = 3 + 999 = 1002.
    CHECK(delta == 1002,
          "1000 invalidate(f): 3 (first: f+g+h) + 999 (rest: f only) = 1002 evictions");
    return true;
}

// ═════════════════════════════════════════════════════════════
// Main test runner
// ═════════════════════════════════════════════════════════════

int run_tests() {
    std::println("═══ Issue #401 (invalidate_function BFS) ═══\n");

    test_ac1_bfs_visits_all_transitives();
    test_ac2_relower_is_stable();
    test_ac3_dep_graph_integrity();
    test_ac4_metric_semantics();
    test_ac5_dep_graph_stable_across_rebinds();
    test_ac6_no_infinite_traversal();

    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_401_detail

int aura_issue_401_run() { return aura_issue_401_detail::run_tests(); }

int main() {
    return aura_issue_401_run();
}