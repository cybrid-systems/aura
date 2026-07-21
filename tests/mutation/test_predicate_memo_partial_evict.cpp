// @category: unit
// @reason: Issue #1872 — predicate_memo_ partial LRU eviction under
// Issue #1872 (#1978 renamed): issue# moved from filename to header.
// high mutation (replace wholesale clear when size>4096) + strengthen
// per-binding gen exact compare; metrics partial_eviction + hit rate.
//
//   AC1: source cites #1872; partial eviction helper + last_used stamp
//   AC2: overflow path uses evict_until / partial (not clear-only)
//   AC3: metrics partial_evictions_total + per_binding_gen_hit_rate
//   AC4: Variable stamp uses set_type_with_binding_gen; hit path exact-compares

#include "compiler/bounded_lru.h"
#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>

import std;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::util::evict_until;
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

struct MemoStub {
    std::uint64_t last_used = 0;
    int id = 0;
};

} // namespace

int main() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: #1872 partial eviction + binding_gen strengthen ---");
        auto impl = read_first(
            {"src/compiler/type_checker_impl.cpp", "../src/compiler/type_checker_impl.cpp"});
        auto ixx =
            read_first({"src/compiler/type_checker.ixx", "../src/compiler/type_checker.ixx"});
        auto hdr = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        CHECK(!impl.empty(), "read type_checker_impl.cpp");
        CHECK(impl.find("#1872") != std::string::npos, "impl cites #1872");
        CHECK(impl.find("evict_predicate_memo_if_over_capacity") != std::string::npos,
              "partial eviction helper");
        CHECK(impl.find("evict_until") != std::string::npos, "uses bounded_lru evict_until");
        CHECK(impl.find("last_used") != std::string::npos, "LRU last_used stamps");
        CHECK(impl.find("set_type_with_binding_gen") != std::string::npos,
              "Variable stamp via set_type_with_binding_gen");
        CHECK(impl.find("binding_gen(nv.sym_id)") != std::string::npos ||
                  impl.find("binding_gen(v.sym_id)") != std::string::npos,
              "exact binding_gen compare/stamp");
        CHECK(!ixx.empty() && ixx.find("#1872") != std::string::npos, "ixx cites #1872");
        CHECK(ixx.find("predicate_memo_partial_evictions_") != std::string::npos,
              "partial eviction counter on engine");
        CHECK(ixx.find("last_used") != std::string::npos, "PredicateMemoEntry has last_used");
        CHECK(!hdr.empty() &&
                  hdr.find("predicate_memo_partial_evictions_total") != std::string::npos,
              "metrics partial_evictions_total");
        CHECK(hdr.find("per_binding_gen_hit_rate") != std::string::npos,
              "metrics per_binding_gen_hit_rate");
    }

    // ── AC2: overflow is partial, not wholesale clear-only ──
    {
        std::println("\n--- AC2: overflow path is partial (not clear-only) ---");
        auto impl = read_first(
            {"src/compiler/type_checker_impl.cpp", "../src/compiler/type_checker_impl.cpp"});
        // The two overflow sites must call the partial helper, not clear.
        auto p1 = impl.find("evict_predicate_memo_if_over_capacity");
        CHECK(p1 != std::string::npos, "helper defined/called");
        // Count calls (definition + 2 insert sites ≈ ≥3).
        std::size_t calls = 0;
        for (std::size_t pos = 0;
             (pos = impl.find("evict_predicate_memo_if_over_capacity", pos)) != std::string::npos;
             pos += 1)
            ++calls;
        CHECK(calls >= 3, "helper used at both overflow insert sites + def");

        // Helper body targets half capacity, not full clear.
        auto def = impl.find("void InferenceEngine::evict_predicate_memo_if_over_capacity");
        CHECK(def != std::string::npos, "helper definition present");
        auto body = impl.substr(def, 600);
        CHECK(body.find("PREDICATE_MEMO_MAX_ENTRIES / 2") != std::string::npos ||
                  body.find("MAX_ENTRIES / 2") != std::string::npos,
              "evicts down to half capacity");
        CHECK(body.find("partial_evictions_") != std::string::npos, "bumps partial counter");
        // Must not clear() inside the overflow helper.
        CHECK(body.find(".clear()") == std::string::npos, "helper does not wholesale clear");
    }

    // ── AC3: runtime metrics fields + LRU helper ──
    {
        std::println("\n--- AC3: metrics + LRU evict_until unit ---");
        CompilerMetrics m;
        CHECK(m.predicate_memo_partial_evictions_total.load() == 0, "partial starts 0");
        CHECK(m.per_binding_gen_hit_rate.load() == 0, "hit_rate starts 0");
        m.predicate_memo_partial_evictions_total.fetch_add(1, std::memory_order_relaxed);
        m.per_binding_gen_hit_rate.store(42, std::memory_order_relaxed);
        CHECK(m.predicate_memo_partial_evictions_total.load() == 1, "partial bump");
        CHECK(m.per_binding_gen_hit_rate.load() == 42, "hit_rate store");

        // Direct unit of the same eviction helper used by the engine.
        std::unordered_map<int, MemoStub> map;
        for (int i = 0; i < 10; ++i)
            map[i] = MemoStub{static_cast<std::uint64_t>(i), i};
        std::size_t n = 0;
        evict_until(map, 5, &n);
        CHECK(map.size() == 5, "evict_until leaves max_entries");
        CHECK(n == 5, "evicted 5");
        // Oldest last_used (0..4) should be gone; 5..9 remain.
        CHECK(map.find(0) == map.end() && map.find(9) != map.end(), "evicts oldest stamps");
    }

    // ── AC4: binding_gen exact compare in source ──
    {
        std::println("\n--- AC4: binding_gen exact compare (no optimistic non-zero hit) ---");
        auto impl = read_first(
            {"src/compiler/type_checker_impl.cpp", "../src/compiler/type_checker_impl.cpp"});
        auto pos = impl.find("type_cache_binding_gen(id)");
        CHECK(pos != std::string::npos, "reads type_cache_binding_gen");
        // Window around the hit path should compare cur_stamp == cached.
        auto win = impl.substr(pos, 900);
        CHECK(win.find("cur_stamp") != std::string::npos ||
                  win.find("== cached_binding_gen") != std::string::npos,
              "exact compare against cached stamp");
        CHECK(win.find("NodeTag::Variable") != std::string::npos, "Variable-scoped rescue");
        // Must not still treat any non-zero as unconditional hit without compare.
        // The old comment pattern is gone / replaced.
        CHECK(win.find("per_binding_gen_hits") != std::string::npos, "still counts rescues");
    }

    std::println("\n=== test_predicate_memo_partial_evict_1872: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
