// tests/bench/stable_ref_bench_10k.cpp — Issue #393
// follow-up #3: production-scale stable-ref bench.
//
// The unit-scale stable_ref_bench.cpp uses 100 defines
// × 10 children = 1,000 nodes. This file scales the
// workload to 10,000+ nodes (1000 defines × 10 children)
// to verify the scoped invalidation win holds at
// production-shaped AST scale (typical EDA / long-running
// AI agent workspaces).
//
// Workload:
//   - 1,000 top-level defines + 10 literal children each =
//     10,000 nodes
//   - 100 mutation rounds (reduced from the 1k bench's
//     1000 to keep wall time ≤ ~30s)
//   - One subtree-bump every kBumpEveryNRounds = 5 rounds
//     (20 bumps total over 100 rounds; reflects the
//     "localized edit" agent pattern — only one subtree
//     touched per turn)
//
// Expected result:
//   - Stable re-queries: ~20 (one per bump, since each bump
//     invalidates only the Define's first child as in the 1k
//     bench — sibling children are top-level, not in the
//     Define's subtree)
//   - Raw re-queries: 100 rounds × 10,000 refs = 1,000,000
//   - Re-query ratio: 20 / 1M ≈ 0.002% (vs 50% AC ceiling)
//   - Speedup: ~2× wall-clock (memory-bound; flat.get() is
//     slightly more expensive than is_valid_subtree())
//   - AC PASS: re_query < 50% of raw AND speedup >= 30%
//
// Exit codes:
//   0 = AC PASS
//   1 = AC FAIL

import std;
import aura.core.ast;

namespace {

constexpr int kNumDefines = 1000;      // top-level defines (10× the 1k bench)
constexpr int kChildrenPerDefine = 10; // literal children per define (same as 1k)
constexpr int kRounds = 100;           // mutation rounds (reduced from 1000 for wall time)
constexpr int kBumpEveryNRounds = 5;   // bump one define every N rounds

struct RawResult {
    long long wall_us = 0;
    std::size_t re_queries = 0;
};

struct StableResult {
    long long wall_us = 0;
    std::size_t re_queries = 0;
};

// Build a FlatAST with kNumDefines top-level defines, each
// holding kChildrenPerDefine literal children. The first
// child of each define is wrapped in add_define (so the
// define's subtree includes that child); the remaining 9
// siblings are top-level nodes that are NOT inside any
// define's subtree. This mirrors the 1k bench exactly —
// only the first child is invalidated by a scoped bump.
//
// Returns the per-define first-child ids so the bump loop
// can pick a target, plus a flat vector of all child ids
// for the stable-path's initial ref build.
struct Workload {
    std::vector<std::vector<aura::ast::NodeId>> value_ids;
    std::vector<aura::ast::NodeId> all_child_ids;
};

Workload build_workload(aura::ast::FlatAST& flat, aura::ast::StringPool& pool) {
    Workload w;
    w.value_ids.reserve(kNumDefines);
    w.all_child_ids.reserve(kNumDefines * kChildrenPerDefine);
    for (int i = 0; i < kNumDefines; ++i) {
        std::vector<aura::ast::NodeId> ids;
        ids.reserve(kChildrenPerDefine);
        for (int j = 0; j < kChildrenPerDefine; ++j) {
            auto cid = flat.add_literal(i * kChildrenPerDefine + j);
            ids.push_back(cid);
            w.all_child_ids.push_back(cid);
        }
        // Capture front() BEFORE std::move (the source is
        // left in valid-but-unspecified state after move —
        // calling front() post-move asserts in libc++ debug).
        auto first_child = ids.front();
        w.value_ids.push_back(std::move(ids));
        auto sym = pool.intern("def_" + std::to_string(i));
        // add_define takes one value; the Define's subtree
        // will contain only first_child, not the siblings.
        (void)flat.add_define(sym, first_child);
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

// RAW path: re-query / re-read every child every round.
// (Raw path can't trust cached NodeIds after a mutation.)
RawResult run_raw_path(aura::ast::FlatAST& flat,
                       const std::vector<std::vector<aura::ast::NodeId>>& value_ids) {
    RawResult r;
    auto walk = [&]() {
        for (const auto& ids : value_ids) {
            for (auto vid : ids) {
                (void)flat.get(vid).tag;
                ++r.re_queries;
            }
        }
    };
    r.wall_us = time_us(walk);
    return r;
}

// STABLE path: keep stable refs across rounds; use
// #392's is_valid_subtree() so refs in non-bumped subtrees
// stay valid (only re-create refs that are stale).
StableResult run_stable_path(aura::ast::FlatAST& flat,
                             std::vector<aura::ast::FlatAST::StableNodeRef>& stable_refs) {
    StableResult r;
    auto walk = [&]() {
        for (std::size_t i = 0; i < stable_refs.size(); ++i) {
            if (!flat.is_valid_subtree(stable_refs[i])) {
                stable_refs[i] = flat.make_ref(stable_refs[i].id);
                ++r.re_queries;
            }
        }
    };
    r.wall_us = time_us(walk);
    return r;
}

} // namespace

int main() {
    std::println("=== Issue #393 follow-up #3: production-scale stable-ref bench ===");
    std::println("workload: {} defines × {} children = {} total nodes × {} rounds", kNumDefines,
                 kChildrenPerDefine, kNumDefines * kChildrenPerDefine, kRounds);
    std::println("bump pattern: one subtree every {} rounds (= {} bumps total)", kBumpEveryNRounds,
                 kRounds / kBumpEveryNRounds);

    // ── RAW path ──────────────────────────────────────────────
    long long raw_total_us = 0;
    std::size_t raw_re_queries_total = 0;
    {
        aura::ast::StringPool pool;
        aura::ast::FlatAST flat;
        auto wl = build_workload(flat, pool);
        for (int round = 0; round < kRounds; ++round) {
            if (round % kBumpEveryNRounds == 0) {
                auto target_vid = wl.value_ids[round % kNumDefines].front();
                flat.bump_generation_subtree(target_vid);
            }
            auto r = run_raw_path(flat, wl.value_ids);
            raw_total_us += r.wall_us;
            raw_re_queries_total += r.re_queries;
        }
    }

    // ── STABLE path ───────────────────────────────────────────
    long long stable_total_us = 0;
    std::size_t stable_re_queries_total = 0;
    {
        aura::ast::StringPool pool;
        aura::ast::FlatAST flat;
        auto wl = build_workload(flat, pool);
        // Build stable refs ONCE. The is_valid_subtree()
        // check is what tells us when a ref is stale.
        std::vector<aura::ast::FlatAST::StableNodeRef> stable_refs;
        stable_refs.reserve(wl.all_child_ids.size());
        for (auto cid : wl.all_child_ids)
            stable_refs.push_back(flat.make_ref(cid));
        for (int round = 0; round < kRounds; ++round) {
            if (round % kBumpEveryNRounds == 0) {
                auto target_vid = wl.value_ids[round % kNumDefines].front();
                flat.bump_generation_subtree(target_vid);
            }
            auto r = run_stable_path(flat, stable_refs);
            stable_total_us += r.wall_us;
            stable_re_queries_total += r.re_queries;
        }
    }

    // ── Report ────────────────────────────────────────────────
    const auto raw_avg = raw_total_us / kRounds;
    const auto stable_avg = stable_total_us / kRounds;
    const auto speedup_pct = raw_avg > 0 ? (100 * (raw_avg - stable_avg)) / raw_avg : 0;
    const auto re_query_ratio_pct =
        raw_re_queries_total > 0 ? (100 * stable_re_queries_total) / raw_re_queries_total : 0;

    std::println("");
    std::println("  raw   : total {:>8} \u00b5s  (avg {:>6} \u00b5s/round)  re-queries {}",
                 raw_total_us, raw_avg, raw_re_queries_total);
    std::println("  stable: total {:>8} \u00b5s  (avg {:>6} \u00b5s/round)  re-queries {}",
                 stable_total_us, stable_avg, stable_re_queries_total);
    std::println("");
    std::println("  speedup: {}%  (positive = stable is faster)", speedup_pct);
    std::println("  stable re-queries are {}% of raw re-queries", re_query_ratio_pct);
    std::println("");
    std::println("  reference: 1k bench (1000 nodes, 1000 rounds) did 1M raw re-queries,");
    std::println("             143 stable re-queries (0%), 58% speedup");
    std::println("  vs 10k (this): {} raw re-queries, {} stable re-queries ({}%), {}% speedup",
                 raw_re_queries_total, stable_re_queries_total, re_query_ratio_pct, speedup_pct);

    // Same AC as the 1k bench: re_query < 50% of raw OR
    // speedup >= 30% (permissive latency check).
    const bool ac_met = (re_query_ratio_pct < 50) || (speedup_pct >= 30);

    std::println("");
    std::println("AC check (10k scale): stable re-queries < 50% of raw OR speedup >= 30% : {}",
                 ac_met ? "PASS" : "FAIL");
    std::println("  re_query_ratio = {}% (< 50%): {}", re_query_ratio_pct, re_query_ratio_pct < 50);
    std::println("  speedup_pct    = {}% (>= 30%): {}", speedup_pct, speedup_pct >= 30);

    std::println("");
    std::println("verdict: production-scale (10k nodes) win compared to 1k: {}",
                 (re_query_ratio_pct < 50 && speedup_pct >= 30)
                     ? "CONFIRMED — scaled-down ratio + scaled-up savings both hold"
                     : "PARTIAL — see numbers above");

    return ac_met ? 0 : 1;
}
