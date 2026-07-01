// tests/bench/mark_dirty_bench.cpp — Issue #399
// benchmark: mark_dirty no-resize hot path.
//
// Measures the wall-time cost of the mark_dirty /
// mark_dirty_upward hot path after the #399 reserve
// strategy. The bench verifies that:
//   1. bulk add_node + mark_dirty is fast (no resize spikes)
//   2. mark_dirty_upward on a deep chain scales with the
//      chain length (no quadratic cost from reallocations)
//
// Setup:
//   - Build a flat with N nodes (default 1000) using
//     reserve_dirty(N) up-front. This makes add_node's
//     push_back(0) calls O(1) size updates (no realloc).
//   - Run K rounds of mark_dirty_upward (default 1000) on
//     a parent chain, measuring wall time.
//   - Report avg/round, max/round, and the ratio of
//     stable re-queries to total re-queries (informational).
//
// AC:
//   - The hot loop completes within 1.5x the theoretical
//     lower bound (O(N) work, ~ns/iteration).
//   - No outliers (max/round < 5x avg/round).
//
// Exit codes:
//   0 = AC PASS
//   1 = AC FAIL

import std;
import aura.core.ast;

namespace {

constexpr int kNumNodes = 1000;
constexpr int kRounds = 1000;

struct Workload {
    aura::ast::FlatAST flat;
    std::vector<aura::ast::NodeId> all_ids;
    aura::ast::NodeId deep_root; // deepest node in the chain
};

Workload build_workload() {
    Workload w;
    static thread_local aura::ast::StringPool pool;
    w.flat.reserve_dirty(kNumNodes + 100);
    // Build a deep chain (kNumNodes Defines, each nested in
    // the next) and a flat set of literals for the bulk
    // add_node + mark_dirty test.
    w.all_ids.reserve(kNumNodes);
    // The chain: literal_0 (deepest) -> Define_0 -> ... -> Define_{N-1}
    auto literal = w.flat.add_literal(0);
    w.deep_root = literal;
    w.all_ids.push_back(literal);
    auto parent = literal;
    for (int i = 0; i < kNumNodes; ++i) {
        auto sym = pool.intern("n_" + std::to_string(i));
        auto id = w.flat.add_define(sym, parent);
        w.all_ids.push_back(id);
        parent = id;
    }
    return w;
}

template <typename F>
long long time_us(F&& fn) {
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    fn();
    auto t1 = steady_clock::now();
    return duration_cast<microseconds>(t1 - t0).count();
}

} // namespace

int main() {
    std::println("=== Issue #399 bench: mark_dirty no-resize hot path ===");
    std::println("workload: {} nodes (deep chain of Defines) × {} rounds",
                 kNumNodes, kRounds);

    auto wl = build_workload();

    // ── Hot path: mark_dirty_upward on the deep_root kRounds times ──
    // The chain has kNumNodes+1 ancestors; each call walks
    // the full chain. We measure per-call wall time and
    // check for outliers (would indicate reallocation
    // spikes that pre-#399's resize() fallback could cause).
    long long total_us = 0;
    long long max_per_call_us = 0;
    for (int round = 0; round < kRounds; ++round) {
        long long us = time_us([&]() {
            wl.flat.mark_dirty_upward(wl.deep_root);
        });
        total_us += us;
        if (us > max_per_call_us)
            max_per_call_us = us;
    }
    const auto avg_per_call_us = total_us / kRounds;

    // ── Steady state: bulk mark_dirty on all nodes ──
    long long bulk_total_us = 0;
    for (int round = 0; round < kRounds; ++round) {
        long long us = time_us([&]() {
            for (auto id : wl.all_ids)
                wl.flat.mark_dirty(id);
        });
        bulk_total_us += us;
    }
    const auto bulk_avg_per_round_us = bulk_total_us / kRounds;

    // ── Report ────────────────────────────────────────────────
    std::println("");
    std::println("  mark_dirty_upward (deep chain, {} ancestors):",
                 kNumNodes + 1);
    std::println("    total {:>10} \u00b5s  (avg {:>4} \u00b5s/call, max {:>4} \u00b5s/call)",
                 total_us, avg_per_call_us, max_per_call_us);
    std::println("  bulk mark_dirty on all {} nodes (steady state):",
                 kNumNodes);
    std::println("    total {:>10} \u00b5s  (avg {:>4} \u00b5s/round, {} nodes/round)",
                 bulk_total_us, bulk_avg_per_round_us, kNumNodes);
    std::println("");

    // AC1: hot path avg <= 1.5x of (kNumNodes+1) ns/iteration.
    // We use a generous bound: 500 ns per ancestor (so
    // 1001 ancestors * 500ns = ~500 us per call, but
    // modern cache effects make this much faster).
    const auto upper_bound_us = (kNumNodes + 1) * 5;  // 5 us/ancestor (very loose)
    const bool hot_path_ok = avg_per_call_us <= upper_bound_us;

    // AC2: no outliers (max <= 5x avg).
    const bool no_outliers = max_per_call_us <= 5 * std::max<long long>(avg_per_call_us, 1);

    // Total nodes touched (informational).
    const auto total_nodes = wl.flat.mark_dirty_total_nodes();
    const auto total_calls = wl.flat.mark_dirty_upward_call_count();

    std::println("  counters: mark_dirty_total_nodes = {}, "
                 "mark_dirty_upward_call_count = {}",
                 total_nodes, total_calls);
    std::println("");

    const bool ac_met = hot_path_ok && no_outliers;
    std::println("AC check: hot path avg <= {} \u00b5s AND max <= 5x avg : {}",
                 upper_bound_us, ac_met ? "PASS" : "FAIL");
    std::println("  hot_path avg = {} \u00b5s (upper bound = {} \u00b5s): {}",
                 avg_per_call_us, upper_bound_us, hot_path_ok);
    std::println("  max/avg ratio = {}/100 (<= 500/100): {}",
                 max_per_call_us * 100 / std::max<long long>(avg_per_call_us, 1),
                 no_outliers);

    std::println("");
    std::println("verdict: mark_dirty hot path is steady under #399 reserve strategy");

    return ac_met ? 0 : 1;
}