// tests/bench/children_stable_bench.cpp — Issue #398
// benchmark: zero-allocation children_stable.
//
// Compares wall-clock cost of 1000 calls of:
//   - old path: flat.children_stable(id) — allocates a
//     std::vector<StableNodeRef> per call, returns by value
//   - new path: flat.for_each_stable_child(id, fn) — zero
//     heap allocation per call, callback receives each
//     ref inline
//
// Workload: 1000 defines (10,000 nodes total), each with
// 10 children. We measure the cost of 1000 rounds × 1000
// children-stable lookups (= 1M lookups).
//
// Expected: new path is measurably faster (no vector alloc).
// AC: new-path avg/round < 0.5x old-path avg/round OR
//     new-path savings >= 20%.

import std;
import aura.core.ast;

namespace {

constexpr int kNumDefines = 1000;
constexpr int kChildrenPerDefine = 10;
constexpr int kRounds = 1000;

struct Workload {
    aura::ast::FlatAST flat;
    std::vector<aura::ast::NodeId> parent_ids;
};

Workload build_workload() {
    Workload w;
    static thread_local aura::ast::StringPool pool;
    w.parent_ids.reserve(kNumDefines);
    for (int i = 0; i < kNumDefines; ++i) {
        // Create a parent with kChildrenPerDefine children.
        auto c0 = w.flat.add_literal(0);
        auto parent = w.flat.add_define(pool.intern("d"), c0);
        for (int j = 1; j < kChildrenPerDefine; ++j) {
            auto c = w.flat.add_literal(j);
            w.flat.insert_child(parent, static_cast<std::uint32_t>(j), c);
        }
        w.parent_ids.push_back(parent);
    }
    return w;
}

template <typename F> long long time_us(F&& fn) {
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    fn();
    auto t1 = steady_clock::now();
    return duration_cast<microseconds>(t1 - t0).count();
}

} // namespace

int main() {
    std::println("=== Issue #398 bench: children_stable (allocating) vs "
                 "for_each_stable_child (zero-alloc) ===");
    std::println("workload: {} defines × {} children = {} total nodes × {} rounds", kNumDefines,
                 kChildrenPerDefine, kNumDefines * kChildrenPerDefine, kRounds);

    // ── OLD path: children_stable (allocating) ───────────────
    long long old_total_us = 0;
    {
        auto wl = build_workload();
        auto walk = [&]() {
            for (int round = 0; round < kRounds; ++round) {
                for (auto parent : wl.parent_ids) {
                    auto children = wl.flat.children_stable(parent);
                    (void)children.size();
                }
            }
        };
        old_total_us = time_us(walk);
    }

    // ── NEW path: for_each_stable_child (zero-alloc) ─────────
    long long new_total_us = 0;
    std::size_t new_visit_count = 0;
    {
        auto wl = build_workload();
        auto walk = [&]() {
            for (int round = 0; round < kRounds; ++round) {
                for (auto parent : wl.parent_ids) {
                    wl.flat.for_each_stable_child(parent, [&](auto ref) {
                        (void)ref.id;
                        ++new_visit_count;
                    });
                }
            }
        };
        new_total_us = time_us(walk);
    }

    // ── Report ────────────────────────────────────────────────
    const auto old_avg = old_total_us / kRounds;
    const auto new_avg = new_total_us / kRounds;
    const auto speedup_pct = old_avg > 0 ? (100 * (old_avg - new_avg)) / old_avg : 0;
    const auto total_lookups = static_cast<std::size_t>(kRounds) * kNumDefines;
    const auto expected_lookups =
        static_cast<std::size_t>(kRounds) * kNumDefines * kChildrenPerDefine;

    std::println("");
    std::println("  OLD (children_stable, allocating): total {:>8} \u00b5s  "
                 "(avg {:>5} \u00b5s/round, {} lookups)",
                 old_total_us, old_avg, total_lookups);
    std::println("  NEW (for_each_stable_child, zero-alloc): total {:>8} \u00b5s  "
                 "(avg {:>5} \u00b5s/round, {} visits)",
                 new_total_us, new_avg, new_visit_count);
    std::println("");
    std::println("  speedup: {}%  (positive = new is faster)", speedup_pct);
    std::println("  expected visits: {} ({} rounds \u00d7 {} parents \u00d7 {} children)",
                 expected_lookups, kRounds, kNumDefines, kChildrenPerDefine);

    // Verify the new path actually visited all the children.
    if (new_visit_count != expected_lookups) {
        std::println("  FAIL: new path visited {} nodes, expected {}", new_visit_count,
                     expected_lookups);
        return 1;
    }
    std::println("  PASS: new path visited all {} children", new_visit_count);

    // AC: new is at least 20% faster, OR uses < 50% the time.
    // We use the speedup threshold since alloc cost on small
    // vectors is real but small (~ns/call); a 20% speedup is
    // a clear win for the zero-alloc path.
    const bool ac_met = (speedup_pct >= 20) || (new_avg * 2 < old_avg);

    std::println("");
    std::println("AC check: new path speedup >= 20% OR new time < 50% of old : {}",
                 ac_met ? "PASS" : "FAIL");
    std::println("  speedup_pct = {}% (>= 20%): {}", speedup_pct, speedup_pct >= 20);
    std::println("  new/old ratio = {}/100 (< 50/100): {}", new_avg * 100 / (old_avg + 1),
                 new_avg * 2 < old_avg);

    return ac_met ? 0 : 1;
}
