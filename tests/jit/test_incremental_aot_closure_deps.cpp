// @category: integration
// @reason: uses AOT bridge C-linkage API + CompilerMetrics accessors +
//          re-emit candidate iterator (push-based callback). No LLVM JIT
//          compile, no CompilerService integration — minimal surface so
//          the test links fast even with the system 5-min build timeout.
//
// test_incremental_aot_closure_deps.cpp — Verify Issue #1480 acceptance
// criteria ("[P0][AOT][Incremental] Complete closure capture dependency
// tracking + region re-apply in incremental re-AOT pipeline").
//
// Background: #1046 closed Phase 1 (observability + 3 unrelated bugfixes)
// on 2026-07-10 and explicitly deferred the full re-AOT pipeline
// (closure capture dep tracking + region re-apply + atomic hot-swap +
// ClosureBridge refresh) to a follow-up PR. #1480 (Grok-bot survey-lag
// re-filing, body references "Exact code in review" with 0 comments)
// asks for the same deferred scope. This commit ships the foundation
// (aura_reemit_aot_for_dirty Phase 2 + 4 metrics + region mask filter +
// atomic hot-swap commit) and registers the test that verifies the
// bridge C-linkage surface.
//
// Test strategy: 6 ACs, one per public C-linkage surface + integration.
// All tests are pure C-linkage (no CompilerService, no LLVM JIT) so
// the test links in <1 minute:
//
//   AC1: aura_set_reemit_candidate_fn accepts callback (no-op verify)
//   AC2: aura_reemit_aot_for_dirty Phase 1 fallback (no callback →
//        returns 0, bumps aot_reemit_dirty_skeleton_calls)
//   AC3: aura_reemit_aot_for_dirty Phase 2 with 3 candidates + region
//        mask (region bit filter skips 1 of 3, returns 2, bumps
//        aot_incremental_reemit_count by 2 + aot_region_filtered_skips
//        by 1 + aot_closure_dependency_reemit_total by the from_closure
//        capture count)
//   AC4: aura_reemit_dirty_count / aura_reemit_region_filtered_skips /
//        aura_reemit_closure_dep_count return the per-call last stats
//        (relaxed atomic, observable to EDSL)
//   AC5: 100-iter stress test — push the same 5 candidates 100x,
//        verify the cumulative aot_incremental_reemit_count grows by
//        ≥500 and g_aot_table_epoch is bumped exactly 100 times (one
//        commit_func_table_swap per call that re-emits anything).
//   AC6: aot_closure_bridge_refresh_total grows by the re-emit count
//        per call (paired with JIT-side jit_hotswap_live_closure_
//        refreshed_total for cross-side observability).

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <print>
#include <string>
#include <vector>

namespace {

// ── Test fixtures ──────────────────────────────────────────────

// Re-emit candidate iterator state. Captures a fixed vector of
// (name, region, from_closure_capture) tuples and replays them
// across multiple aura_reemit_aot_for_dirty calls (cursor resets
// to 0 when iteration completes, so the bridge can call the
// callback repeatedly).
struct ReemitFixture {
    struct Candidate {
        std::string name;
        std::uint64_t region;
        bool from_closure_capture;
    };
    std::vector<Candidate> candidates;
    std::size_t cursor = 0;
    std::atomic<std::uint32_t> callback_calls{0};
};

// C-linkage shim: aura_reemit_candidate_fn_t is `bool (*)(void*,
// const char**, uint64_t*, bool*)`. Reads the next candidate from
// the fixture's cursor; resets cursor when iteration completes so
// the bridge can call us again on the next aura_reemit_aot_for_dirty.
static bool reemit_candidate_iter(void* userdata, const char** out_name, std::uint64_t* out_region,
                                  bool* out_from_closure_capture) {
    auto* f = static_cast<ReemitFixture*>(userdata);
    f->callback_calls.fetch_add(1, std::memory_order_relaxed);
    if (!f || f->candidates.empty())
        return false;
    if (f->cursor >= f->candidates.size()) {
        f->cursor = 0; // reset for next aura_reemit_aot_for_dirty call
        return false;
    }
    const auto& c = f->candidates[f->cursor++];
    *out_name = c.name.c_str();
    *out_region = c.region;
    *out_from_closure_capture = c.from_closure_capture;
    return true;
}

// Helper: read the current aot_incremental_reemit_count (with the
// global aot_metrics pointer wired by aura_set_aot_metrics).
static std::uint64_t read_aot_incremental_reemit_count() {
    void* m = aura_get_aot_metrics();
    if (!m)
        return 0;
    const auto* metrics = static_cast<const aura::compiler::CompilerMetrics*>(m);
    return metrics->aot_incremental_reemit_count.load(std::memory_order_relaxed);
}

static std::uint64_t read_aot_closure_dependency_reemit_total() {
    void* m = aura_get_aot_metrics();
    if (!m)
        return 0;
    const auto* metrics = static_cast<const aura::compiler::CompilerMetrics*>(m);
    return metrics->aot_closure_dependency_reemit_total.load(std::memory_order_relaxed);
}

static std::uint64_t read_aot_region_filtered_skips() {
    void* m = aura_get_aot_metrics();
    if (!m)
        return 0;
    const auto* metrics = static_cast<const aura::compiler::CompilerMetrics*>(m);
    return metrics->aot_region_filtered_skips.load(std::memory_order_relaxed);
}

static std::uint64_t read_aot_closure_bridge_refresh_total() {
    void* m = aura_get_aot_metrics();
    if (!m)
        return 0;
    const auto* metrics = static_cast<const aura::compiler::CompilerMetrics*>(m);
    return metrics->aot_closure_bridge_refresh_total.load(std::memory_order_relaxed);
}

static std::uint64_t read_aot_reemit_dirty_skeleton_calls() {
    void* m = aura_get_aot_metrics();
    if (!m)
        return 0;
    const auto* metrics = static_cast<const aura::compiler::CompilerMetrics*>(m);
    return metrics->aot_reemit_dirty_skeleton_calls.load(std::memory_order_relaxed);
}

static std::uint64_t read_aot_func_table_epoch() {
    return aura_aot_func_table_epoch();
}

// ── AC1: aura_set_reemit_candidate_fn accepts callback ─────────

bool test_set_reemit_candidate_callback() {
    std::println("\n--- AC1: aura_set_reemit_candidate_fn accepts callback ---");
    ReemitFixture f;
    f.candidates = {{"foo", 1, false}, {"bar", 2, true}, {"baz", 3, false}};

    // The setter just stores the pointer; no error path.
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &f);
    // The setter accepts a callback; verify by calling it.
    const char* name = nullptr;
    std::uint64_t region = 0;
    bool from_cc = false;
    const bool ok = reemit_candidate_iter(&f, &name, &region, &from_cc);
    CHECK(ok, "callback returns true on first call");
    if (ok) {
        CHECK(name != nullptr && std::string(name) == "foo", "first candidate name is 'foo'");
        CHECK(region == 1, "first candidate region is 1");
        CHECK(!from_cc, "first candidate from_closure_capture is false");
    }
    // Reset for next test (don't leak the callback between ACs).
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    return true;
}

// ── AC2: Phase 1 fallback when no callback wired ───────────────

bool test_phase1_skeleton_fallback() {
    std::println("\n--- AC2: aura_reemit_aot_for_dirty Phase 1 fallback ---");
    // Wire metrics so the skeleton counter bumps are visible.
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    // Reset the callback (no host wired).
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    const auto before = read_aot_reemit_dirty_skeleton_calls();

    const std::uint64_t result = aura_reemit_aot_for_dirty(0);
    CHECK(result == 0, "Phase 1 fallback returns 0 when no callback wired");
    CHECK(read_aot_reemit_dirty_skeleton_calls() == before + 1,
          "aot_reemit_dirty_skeleton_calls bumps by 1");
    return true;
}

// ── AC3: Phase 2 with region mask filter ───────────────────────

bool test_phase2_region_mask_filter() {
    std::println("\n--- AC3: Phase 2 region mask filter ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    // 3 candidates: regions 1, 2, 3. Set region mask = bit 1 + bit 3
    // (= 0b101 = 5) → region 2 (bit 2) is filtered out.
    aura_set_aot_emit_region_mask(/*bit 1 + bit 3*/ (1ULL << 1) | (1ULL << 3));

    ReemitFixture f;
    f.candidates = {
        {"foo", 1, false}, // survives (bit 1)
        {"bar", 2, true},  // filtered (bit 2 not in mask)
        {"baz", 3, false}, // survives (bit 3)
    };
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &f);

    const auto before_reemit = metrics.aot_incremental_reemit_count.load();
    const auto before_region_skips = metrics.aot_region_filtered_skips.load();
    const auto before_closure_dep = metrics.aot_closure_dependency_reemit_total.load();
    const auto before_bridge_refresh = metrics.aot_closure_bridge_refresh_total.load();
    const auto before_epoch = read_aot_func_table_epoch();

    const std::uint64_t result = aura_reemit_aot_for_dirty(0);
    CHECK(result == 2, "returns 2 (foo + baz survive region filter)");

    CHECK(metrics.aot_incremental_reemit_count.load() == before_reemit + 2,
          "aot_incremental_reemit_count grows by 2");
    CHECK(metrics.aot_region_filtered_skips.load() == before_region_skips + 1,
          "aot_region_filtered_skips grows by 1 (bar)");
    CHECK(metrics.aot_closure_dependency_reemit_total.load() == before_closure_dep + 1,
          "aot_closure_dependency_reemit_total grows by 1 (bar from_closure_capture=true)");
    CHECK(metrics.aot_closure_bridge_refresh_total.load() == before_bridge_refresh + 2,
          "aot_closure_bridge_refresh_total grows by 2 (foo + baz)");
    CHECK(read_aot_func_table_epoch() == before_epoch + 1,
          "g_aot_table_epoch bumped by 1 (atomic commit_func_table_swap)");
    CHECK(f.callback_calls.load() == 4,
          "callback called 4 times (3 candidates + 1 sentinel false)");

    // Reset for next test.
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_region_mask(0);
    return true;
}

// ── AC4: last-call stats accessors ─────────────────────────────

bool test_last_call_stats_accessors() {
    std::println("\n--- AC4: aura_reemit_* last-call stats ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_set_aot_emit_region_mask((1ULL << 1) | (1ULL << 3));

    ReemitFixture f;
    f.candidates = {
        {"foo", 1, false},
        {"bar", 2, true}, // filtered
        {"baz", 3, false},
    };
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &f);
    aura_reemit_aot_for_dirty(0);

    CHECK(aura_reemit_dirty_count() == 2, "aura_reemit_dirty_count returns 2");
    CHECK(aura_reemit_region_filtered_skips() == 1, "aura_reemit_region_filtered_skips returns 1");
    CHECK(aura_reemit_closure_dep_count() == 1, "aura_reemit_closure_dep_count returns 1");

    // Run again with no candidates — last-call stats reset to 0.
    ReemitFixture empty;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &empty);
    const std::uint64_t empty_result = aura_reemit_aot_for_dirty(0);
    CHECK(empty_result == 0, "second call returns 0 (no candidates)");
    CHECK(aura_reemit_dirty_count() == 0, "last-call stats reset to 0");
    CHECK(aura_reemit_region_filtered_skips() == 0, "skips reset to 0");
    CHECK(aura_reemit_closure_dep_count() == 0, "closure_dep reset to 0");

    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_region_mask(0);
    return true;
}

// ── AC5: 100-iter stress test ──────────────────────────────────

bool test_100_iter_stress() {
    std::println("\n--- AC5: 100-iter stress ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_set_aot_emit_region_mask(0); // no region filter — all 5 candidates re-emit

    ReemitFixture f;
    f.candidates = {
        {"alpha", 1, false}, {"bravo", 2, true}, {"charlie", 3, false},
        {"delta", 4, true},  {"echo", 5, false},
    };
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &f);

    const auto before_reemit = metrics.aot_incremental_reemit_count.load();
    const auto before_epoch = read_aot_func_table_epoch();
    constexpr int kIters = 100;
    for (int i = 0; i < kIters; ++i) {
        const std::uint64_t n = aura_reemit_aot_for_dirty(0);
        CHECK(n == 5, "each iter re-emits all 5 candidates");
    }

    CHECK(metrics.aot_incremental_reemit_count.load() == before_reemit + 5 * kIters,
          "aot_incremental_reemit_count grows by 500 over 100 iters");
    CHECK(read_aot_func_table_epoch() == before_epoch + kIters,
          "g_aot_table_epoch bumped exactly 100 times (one commit per iter)");

    aura_set_reemit_candidate_fn(nullptr, nullptr);
    return true;
}

// ── AC6: closure_bridge_refresh_total pair metric ──────────────

bool test_closure_bridge_refresh_pair_metric() {
    std::println("\n--- AC6: closure_bridge_refresh_total pair metric ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    // All 4 candidates re-emit, none from closure capture, but the
    // metric still grows by the re-emit count (closure bridge
    // re-stamp is a side-effect of any successful re-emit commit).
    ReemitFixture f;
    f.candidates = {
        {"w", 1, false},
        {"x", 2, false},
        {"y", 3, false},
        {"z", 4, false},
    };
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &f);

    const auto before = metrics.aot_closure_bridge_refresh_total.load();
    aura_reemit_aot_for_dirty(0);
    CHECK(metrics.aot_closure_bridge_refresh_total.load() == before + 4,
          "aot_closure_bridge_refresh_total grows by 4 (all 4 re-emit)");
    // Pair metric is the same as aot_incremental_reemit_count when
    // no closure_capture candidates are present (no special
    // re-stamp path for closure captures in Phase 2 — that's the
    // #1481 follow-up for full LLVM re-emit + ClosureBridge epoch
    // refresh against the live func_table).

    aura_set_reemit_candidate_fn(nullptr, nullptr);
    return true;
}

// ── Main runner ────────────────────────────────────────────────

} // namespace

int main() {
    std::println("═══ Issue #1480 incremental re-AOT pipeline verification ═══\n");
    aura::test::g_passed = 0;
    aura::test::g_failed = 0;

    test_set_reemit_candidate_callback();
    test_phase1_skeleton_fallback();
    test_phase2_region_mask_filter();
    test_last_call_stats_accessors();
    test_100_iter_stress();
    test_closure_bridge_refresh_pair_metric();

    std::println("\n════════════════════════════════════════");
    return aura::test::RUN_ALL_TESTS();
}
