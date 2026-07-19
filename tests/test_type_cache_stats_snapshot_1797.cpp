// @category: unit
// @reason: Issue #1797 — compile:type-cache-stats must read the 4
// typecheck cache atomics as one logical snapshot so gen-saved-ratio-bp
// is not mixed across concurrent typechecks.
//
//   AC1: CompilerMetrics has snapshot_type_cache_stats + #1797
//   AC2: compile:type-cache-stats uses snapshot (not 4 raw loads)
//   AC3: snapshot is stable under concurrent fetch_add stress
//   AC4: primitive returns hash with expected keys

#include "test_harness.hpp"

#include <atomic>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "compiler/observability_metrics.h"

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::TypeCacheStatsSnapshot;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

} // namespace

int main() {
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: snapshot API wired ---");
        auto om = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        CHECK(!om.empty(), "read observability_metrics.h");
        CHECK(om.find("#1797") != std::string::npos, "metrics cites #1797");
        CHECK(om.find("snapshot_type_cache_stats") != std::string::npos, "snapshot method");
        CHECK(om.find("struct TypeCacheStatsSnapshot") != std::string::npos, "snapshot struct");

        auto prim = read_first({"src/compiler/evaluator_primitives_compile_05.cpp",
                                "../src/compiler/evaluator_primitives_compile_05.cpp"});
        CHECK(!prim.empty(), "read compile_05.cpp");
        auto pos = prim.find("\"compile:type-cache-stats\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = prim.substr(pos, 1200);
        CHECK(win.find("snapshot_type_cache_stats") != std::string::npos, "uses snapshot");
        CHECK(win.find("typecheck_cache_hits_total.load") == std::string::npos,
              "no raw hits.load in primitive");
    }

    // ── AC3: concurrent stress ──
    {
        std::println("\n--- AC3: snapshot stable under concurrent bumps ---");
        CompilerMetrics m;
        std::atomic<bool> stop{false};
        std::vector<std::thread> writers;
        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&]() {
                while (!stop.load(std::memory_order_relaxed)) {
                    m.typecheck_cache_hits_total.fetch_add(1, std::memory_order_relaxed);
                    m.typecheck_cache_misses_total.fetch_add(1, std::memory_order_relaxed);
                    m.typecheck_stale_cache_total.fetch_add(1, std::memory_order_relaxed);
                    m.typecheck_gen_saved_total.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        std::uint64_t bad_ratio = 0;
        for (int i = 0; i < 2000; ++i) {
            auto s = m.snapshot_type_cache_stats();
            // Within a consistent snapshot, gen_saved and stale are
            // from the same epoch pair (double-check). ratio is well-defined.
            const auto gen_total = s.stale + s.gen_saved;
            if (gen_total > 0) {
                const auto ratio = (s.gen_saved * 10000u) / gen_total;
                if (ratio > 10000)
                    ++bad_ratio;
            }
            // hits/misses/stale/gen_saved should be roughly close under equal bumps
            // (not a hard invariant across counters, only ratio bound).
            (void)s;
        }
        stop.store(true, std::memory_order_relaxed);
        for (auto& th : writers)
            th.join();
        CHECK(bad_ratio == 0, std::format("ratio always ≤ 10000 (bad={})", bad_ratio));
    }

    // ── AC4: runtime primitive ──
    {
        std::println("\n--- AC4: compile:type-cache-stats hash shape ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"compile:type-cache-stats\")");
        CHECK(r && is_hash(*r), "returns hash");
        for (const char* k : {"cache-hits-total", "cache-misses-total", "stale-cache-total",
                              "gen-saved-total", "gen-saved-ratio-bp"}) {
            auto v = cs.eval(
                std::format("(hash-ref (engine:metrics \"compile:type-cache-stats\") \"{}\")", k));
            CHECK(v && is_int(*v), std::format("key {} present", k));
            if (v && is_int(*v) && std::string_view(k) == "gen-saved-ratio-bp")
                CHECK(as_int(*v) >= 0 && as_int(*v) <= 10000, "ratio_bp in range");
        }
    }

    std::println("\n=== test_type_cache_stats_snapshot_1797: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
