// tests/bench/ast_hotpath_bench.cpp — Consolidated FlatAST hot-path bench (#393/#398/#399). (R18
// moved from tests/core/.)
//
// Merges four near-duplicate drivers that all `import aura.core.ast` and
// share the same chrono/AC-report shape:
//
//   stable-ref      Issue #393  — StableNodeRef vs raw re-query (1k + 10k tiers)
//   children-stable Issue #398  — children_stable vs for_each_stable_child
//   mark-dirty      Issue #399  — mark_dirty / mark_dirty_upward no-resize path
//
// Usage:
//   ./build/ast_hotpath_bench                  # run all scenarios
//   ./build/ast_hotpath_bench stable-ref       # both 1k and 10k tiers
//   ./build/ast_hotpath_bench stable-ref 1k
//   ./build/ast_hotpath_bench stable-ref 10k
//   ./build/ast_hotpath_bench children-stable
//   ./build/ast_hotpath_bench mark-dirty
//
// Exit 0 iff every selected scenario's AC passes.

import std;
import aura.core.ast;

namespace {

template <typename F> long long time_us(F&& fn) {
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    fn();
    auto t1 = steady_clock::now();
    return duration_cast<microseconds>(t1 - t0).count();
}

// ── #393 stable-ref ──────────────────────────────────────────────────────

struct StableRefScale {
    const char* name;
    int num_defines;
    int children_per_define;
    int rounds;
    int bump_every_n_rounds;
};

struct StableRefWorkload {
    aura::ast::FlatAST flat;
    std::vector<std::vector<aura::ast::NodeId>> value_ids;
    std::vector<aura::ast::NodeId> all_child_ids;
};

StableRefWorkload build_stable_ref_workload(aura::ast::StringPool& pool, const StableRefScale& s) {
    StableRefWorkload w;
    w.value_ids.reserve(static_cast<std::size_t>(s.num_defines));
    w.all_child_ids.reserve(static_cast<std::size_t>(s.num_defines * s.children_per_define));
    for (int i = 0; i < s.num_defines; ++i) {
        std::vector<aura::ast::NodeId> ids;
        ids.reserve(static_cast<std::size_t>(s.children_per_define));
        for (int j = 0; j < s.children_per_define; ++j) {
            auto cid = w.flat.add_literal(i * s.children_per_define + j);
            ids.push_back(cid);
            w.all_child_ids.push_back(cid);
        }
        auto first_child = ids.front();
        w.value_ids.push_back(std::move(ids));
        auto sym = pool.intern("def_" + std::to_string(i));
        (void)w.flat.add_define(sym, first_child);
    }
    return w;
}

struct PathResult {
    long long wall_us = 0;
    std::size_t re_queries = 0;
};

PathResult run_raw_path(aura::ast::FlatAST& flat,
                        const std::vector<std::vector<aura::ast::NodeId>>& value_ids) {
    PathResult r;
    r.wall_us = time_us([&] {
        for (const auto& ids : value_ids) {
            for (auto vid : ids) {
                (void)flat.get(vid).tag;
                ++r.re_queries;
            }
        }
    });
    return r;
}

PathResult run_stable_path(aura::ast::FlatAST& flat,
                           std::vector<aura::ast::FlatAST::StableNodeRef>& stable_refs) {
    PathResult r;
    r.wall_us = time_us([&] {
        for (std::size_t i = 0; i < stable_refs.size(); ++i) {
            if (!flat.is_valid_subtree(stable_refs[i])) {
                stable_refs[i] = flat.make_ref(stable_refs[i].id);
                ++r.re_queries;
            }
        }
    });
    return r;
}

bool run_stable_ref(const StableRefScale& s) {
    std::println("\n=== Issue #393: stable-ref vs raw re-query ({}) ===", s.name);
    std::println("workload: {} defines × {} children = {} nodes × {} rounds (bump every {})",
                 s.num_defines, s.children_per_define, s.num_defines * s.children_per_define,
                 s.rounds, s.bump_every_n_rounds);

    long long raw_total_us = 0;
    std::size_t raw_re_queries_total = 0;
    {
        aura::ast::StringPool pool;
        auto wl = build_stable_ref_workload(pool, s);
        for (int round = 0; round < s.rounds; ++round) {
            if (round % s.bump_every_n_rounds == 0) {
                auto target = wl.value_ids[static_cast<std::size_t>(round % s.num_defines)].front();
                wl.flat.bump_generation_subtree(target);
            }
            auto r = run_raw_path(wl.flat, wl.value_ids);
            raw_total_us += r.wall_us;
            raw_re_queries_total += r.re_queries;
        }
    }

    long long stable_total_us = 0;
    std::size_t stable_re_queries_total = 0;
    {
        aura::ast::StringPool pool;
        auto wl = build_stable_ref_workload(pool, s);
        std::vector<aura::ast::FlatAST::StableNodeRef> stable_refs;
        stable_refs.reserve(wl.all_child_ids.size());
        for (auto cid : wl.all_child_ids)
            stable_refs.push_back(wl.flat.make_ref(cid));
        for (int round = 0; round < s.rounds; ++round) {
            if (round % s.bump_every_n_rounds == 0) {
                auto target = wl.value_ids[static_cast<std::size_t>(round % s.num_defines)].front();
                wl.flat.bump_generation_subtree(target);
            }
            auto r = run_stable_path(wl.flat, stable_refs);
            stable_total_us += r.wall_us;
            stable_re_queries_total += r.re_queries;
        }
    }

    const auto raw_avg = raw_total_us / s.rounds;
    const auto stable_avg = stable_total_us / s.rounds;
    const auto speedup_pct = raw_avg > 0 ? (100 * (raw_avg - stable_avg)) / raw_avg : 0;
    const auto re_query_ratio_pct =
        raw_re_queries_total > 0 ? (100 * stable_re_queries_total) / raw_re_queries_total : 0;

    std::println("  raw   : total {:>8} µs  (avg {:>6} µs/round)  re-queries {}", raw_total_us,
                 raw_avg, raw_re_queries_total);
    std::println("  stable: total {:>8} µs  (avg {:>6} µs/round)  re-queries {}", stable_total_us,
                 stable_avg, stable_re_queries_total);
    std::println("  speedup: {}%   stable re-queries are {}% of raw", speedup_pct,
                 re_query_ratio_pct);

    const bool ac_met = (re_query_ratio_pct < 50) || (speedup_pct >= 30);
    std::println("AC (re_query < 50% OR speedup >= 30%): {}", ac_met ? "PASS" : "FAIL");
    return ac_met;
}

// ── #398 children-stable ─────────────────────────────────────────────────

bool run_children_stable() {
    constexpr int kNumDefines = 1000;
    constexpr int kChildrenPerDefine = 10;
    constexpr int kRounds = 1000;

    std::println("\n=== Issue #398: children_stable vs for_each_stable_child ===");
    std::println("workload: {} defines × {} children × {} rounds", kNumDefines, kChildrenPerDefine,
                 kRounds);

    struct Workload {
        aura::ast::FlatAST flat;
        std::vector<aura::ast::NodeId> parent_ids;
    };
    auto build = []() {
        Workload w;
        static thread_local aura::ast::StringPool pool;
        w.parent_ids.reserve(kNumDefines);
        for (int i = 0; i < kNumDefines; ++i) {
            auto c0 = w.flat.add_literal(0);
            auto parent = w.flat.add_define(pool.intern("d"), c0);
            for (int j = 1; j < kChildrenPerDefine; ++j) {
                auto c = w.flat.add_literal(j);
                w.flat.insert_child(parent, static_cast<std::uint32_t>(j), c);
            }
            w.parent_ids.push_back(parent);
        }
        return w;
    };

    long long old_total_us = 0;
    {
        auto wl = build();
        old_total_us = time_us([&] {
            for (int round = 0; round < kRounds; ++round) {
                for (auto parent : wl.parent_ids) {
                    auto children = wl.flat.children_stable(parent);
                    (void)children.size();
                }
            }
        });
    }

    long long new_total_us = 0;
    std::size_t new_visit_count = 0;
    {
        auto wl = build();
        new_total_us = time_us([&] {
            for (int round = 0; round < kRounds; ++round) {
                for (auto parent : wl.parent_ids) {
                    wl.flat.for_each_stable_child(parent, [&](auto ref) {
                        (void)ref.id;
                        ++new_visit_count;
                    });
                }
            }
        });
    }

    const auto old_avg = old_total_us / kRounds;
    const auto new_avg = new_total_us / kRounds;
    const auto speedup_pct = old_avg > 0 ? (100 * (old_avg - new_avg)) / old_avg : 0;
    const auto expected = static_cast<std::size_t>(kRounds) * kNumDefines * kChildrenPerDefine;

    std::println("  OLD (allocating children_stable): {:>8} µs  (avg {:>5} µs/round)", old_total_us,
                 old_avg);
    std::println("  NEW (for_each_stable_child):      {:>8} µs  (avg {:>5} µs/round, {} visits)",
                 new_total_us, new_avg, new_visit_count);
    std::println("  speedup: {}%", speedup_pct);

    if (new_visit_count != expected) {
        std::println("AC FAIL: visited {} expected {}", new_visit_count, expected);
        return false;
    }
    const bool ac_met = (speedup_pct >= 20) || (new_avg * 2 < old_avg);
    std::println("AC (speedup >= 20% OR new < 50% old): {}", ac_met ? "PASS" : "FAIL");
    return ac_met;
}

// ── #399 mark-dirty ──────────────────────────────────────────────────────

bool run_mark_dirty() {
    constexpr int kNumNodes = 1000;
    constexpr int kRounds = 1000;

    std::println("\n=== Issue #399: mark_dirty no-resize hot path ===");
    std::println("workload: {} nodes (deep Define chain) × {} rounds", kNumNodes, kRounds);

    aura::ast::FlatAST flat;
    std::vector<aura::ast::NodeId> all_ids;
    aura::ast::NodeId deep_root;
    {
        static thread_local aura::ast::StringPool pool;
        flat.reserve_dirty(kNumNodes + 100);
        all_ids.reserve(static_cast<std::size_t>(kNumNodes));
        auto literal = flat.add_literal(0);
        deep_root = literal;
        all_ids.push_back(literal);
        auto parent = literal;
        for (int i = 0; i < kNumNodes; ++i) {
            auto sym = pool.intern("n_" + std::to_string(i));
            auto id = flat.add_define(sym, parent);
            all_ids.push_back(id);
            parent = id;
        }
    }

    long long total_us = 0;
    long long max_per_call_us = 0;
    // Discard the first call for max/outlier (cold i-cache / TLB); still
    // count it in total/avg so the workload matches historical #399.
    for (int round = 0; round < kRounds; ++round) {
        long long us = time_us([&] { flat.mark_dirty_upward(deep_root); });
        total_us += us;
        if (round > 0 && us > max_per_call_us)
            max_per_call_us = us;
    }
    const auto avg_per_call_us = total_us / kRounds;

    long long bulk_total_us = 0;
    for (int round = 0; round < kRounds; ++round) {
        bulk_total_us += time_us([&] {
            for (auto id : all_ids)
                flat.mark_dirty(id);
        });
    }
    const auto bulk_avg = bulk_total_us / kRounds;

    std::println("  mark_dirty_upward: total {:>10} µs  (avg {:>4} µs/call, max {:>4} µs/call)",
                 total_us, avg_per_call_us, max_per_call_us);
    std::println("  bulk mark_dirty:   total {:>10} µs  (avg {:>4} µs/round)", bulk_total_us,
                 bulk_avg);
    std::println("  counters: total_nodes={}, upward_calls={}", flat.mark_dirty_total_nodes(),
                 flat.mark_dirty_upward_call_count());

    const auto upper_bound_us = (kNumNodes + 1) * 5; // 5 µs/ancestor (loose)
    const bool hot_path_ok = avg_per_call_us <= upper_bound_us;
    const bool no_outliers = max_per_call_us <= 5 * std::max<long long>(avg_per_call_us, 1);
    const bool ac_met = hot_path_ok && no_outliers;
    std::println("AC (avg <= {} µs AND max <= 5×avg): {}", upper_bound_us,
                 ac_met ? "PASS" : "FAIL");
    return ac_met;
}

void print_usage(const char* argv0) {
    std::println("Usage: {} [scenario ...]", argv0);
    std::println("  scenarios: all (default) | stable-ref [1k|10k] | children-stable | mark-dirty");
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);

    if (!args.empty() && (args[0] == "-h" || args[0] == "--help")) {
        print_usage(argv[0]);
        return 0;
    }

    const bool run_all = args.empty() || args[0] == "all";
    bool ok = true;
    int ran = 0;

    auto want = [&](std::string_view name) {
        if (run_all)
            return true;
        for (auto a : args)
            if (a == name)
                return true;
        return false;
    };

    if (want("stable-ref")) {
        // Optional scale filter: stable-ref 1k | stable-ref 10k | (both)
        bool only_1k = false;
        bool only_10k = false;
        for (auto a : args) {
            if (a == "1k")
                only_1k = true;
            if (a == "10k")
                only_10k = true;
        }
        const bool both = !only_1k && !only_10k;
        if (both || only_1k) {
            // Historical 1k-node workload (100 defines × 10 children × 1000 rounds)
            ok &= run_stable_ref({"1k", 100, 10, 1000, 7});
            ++ran;
        }
        if (both || only_10k) {
            // Production-scale (1000 defines × 10 children × 100 rounds)
            ok &= run_stable_ref({"10k", 1000, 10, 100, 5});
            ++ran;
        }
    }
    if (want("children-stable")) {
        ok &= run_children_stable();
        ++ran;
    }
    if (want("mark-dirty")) {
        ok &= run_mark_dirty();
        ++ran;
    }

    if (ran == 0) {
        std::println("unknown scenario(s); nothing ran");
        print_usage(argv[0]);
        return 2;
    }

    std::println("\n═══ ast_hotpath_bench: {} scenario(s) — {} ═══", ran, ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
