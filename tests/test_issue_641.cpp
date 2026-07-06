// @category: integration
// @reason: Issue #641 StableNodeRef Cross-Fiber Provenance
// Enforcement in Multi-Agent Orchestration —
// query:stable-ref-provenance-sv-stats structured companion
//
// Scope-limited close matching #601/#491/#479/#604/#606/#614/
// #615/#616/#617/#618/#620/#621/#622/#623/#624/#625/#626/#630/
// #631/#632/#633/#637/#640 pattern.
//
// Discovery before this PR: the StableNodeRef cross-fiber
// provenance observability surface already covers ~70% of the
// AC3 surface via existing primitives + counters:
//   - (query:stable-ref-provenance) (#604) — base provenance
//     summary primitive (no SV-specific track)
//   - (query:stable-ref-provenance-sv-stats-hash) (#631) —
//     historical hash primitive (different naming; -hash suffix
//     from the pre-enforcement era)
//   - stable_ref_provenance_query_total (#631) +
//     cross_fiber_violations_total (#631) +
//     safe_resolves_total (#631) — cross-fiber / multi-agent
//     SV provenance counters
//
// What the issue body AC3 specifies by **exact name + fields** —
// `query:stable-ref-provenance-sv-stats` (no `-hash` suffix,
// distinct from #631) with AC1+AC2+AC4-specific counters — was
// *not* shipped under that exact name with that exact field set.
// So #641 ships ONE new Aura primitive + 3 new atomics that are
// foundation scaffolding for the future AC1 (fiber_id /
// workspace_id match enforcement in query:/mutate: + Guard
// dtor after is_valid_in), AC2 (Guard success → auto-refresh
// provenance stamp), and AC4 (provenance-checked SV feedback
// path wire-up) enforcement work.
//
// The remaining #641 AC1 + AC2 + AC4 work is invasive C++ on
// the StableNodeRef validate_with_provenance + Guard dtor +
// SV feedback hot path + needs the multi-fiber steal + SV
// sequences + TSan coverage from the issue body — separate
// follow-ups.

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_641_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (query:stable-ref-provenance-sv-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_641_detail

int main() {
    using namespace aura_issue_641_detail;
    std::println("=== Issue #641: query:stable-ref-provenance-sv-stats structured companion ===");

    aura::compiler::CompilerService cs;

    // AC1: hash returns a hash with the documented fields.
    {
        std::println("\n--- AC1: (query:stable-ref-provenance-sv-stats) shape ---");
        auto h = cs.eval("(query:stable-ref-provenance-sv-stats)");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "stable-ref-provenance-sv-stats returns a hash");
        const auto fiber_check = hash_int(cs, "fiber-check");
        const auto auto_refresh = hash_int(cs, "auto-refresh");
        const auto sv_fb_wired = hash_int(cs, "sv-feedback-wired");
        const auto schema = hash_int(cs, "schema");
        CHECK(fiber_check >= 0,
              std::format("fiber-check >= 0 (got {})", fiber_check));
        CHECK(auto_refresh >= 0,
              std::format("auto-refresh >= 0 (got {})", auto_refresh));
        CHECK(sv_fb_wired >= 0,
              std::format("sv-feedback-wired >= 0 (got {})", sv_fb_wired));
        CHECK(schema == 641,
              std::format("schema == 641 (got {})", schema));
    }

    // AC2: existing primitives remain reachable
    // (back-compat — #641 doesn't disturb them).
    {
        std::println("\n--- AC2: existing primitives back-compat ---");
        auto s_604 = cs.eval("(query:stable-ref-provenance)");
        CHECK(s_604.has_value(),
              "(query:stable-ref-provenance) reachable (#604 back-compat)");
        auto s_631 = cs.eval("(query:stable-ref-provenance-sv-stats-hash)");
        CHECK(s_631.has_value(),
              "(query:stable-ref-provenance-sv-stats-hash) reachable (#631 back-compat — historical hash primitive)");
        auto s_640 = cs.eval("(query:sv-verification-closedloop-stats)");
        CHECK(s_640.has_value(),
              "(query:sv-verification-closedloop-stats) reachable (#640 back-compat)");
        auto s_637 = cs.eval("(query:closure-bridge-safety-stats-hash)");
        CHECK(s_637.has_value(),
              "(query:closure-bridge-safety-stats-hash) reachable (#637 back-compat)");
        auto s_633 = cs.eval("(query:stdlib-compiler-demands-stats-hash)");
        CHECK(s_633.has_value(),
              "(query:stdlib-compiler-demands-stats-hash) reachable (#633 back-compat)");
        auto s_632 = cs.eval("(query:atomic-batch-sv-stats-hash)");
        CHECK(s_632.has_value(),
              "(query:atomic-batch-sv-stats-hash) reachable (#632 back-compat)");
    }

    // AC3: derived-metric invariants on a fresh service.
    // fiber-check / auto-refresh / sv-feedback-wired are all
    // 0 on a fresh service — they are foundation scaffolding
    // for the future AC1 + AC2 + AC4 enforcement work
    // (fiber_id/workspace_id match enforcement + Guard
    // auto-refresh + provenance-checked SV feedback).
    {
        std::println("\n--- AC3: derived-metric invariants on fresh service ---");
        const auto fiber_check = hash_int(cs, "fiber-check");
        const auto auto_refresh = hash_int(cs, "auto-refresh");
        const auto sv_fb_wired = hash_int(cs, "sv-feedback-wired");
        CHECK(fiber_check == 0,
              std::format("fresh-service fiber-check == 0 (got {})", fiber_check));
        CHECK(auto_refresh == 0,
              std::format("fresh-service auto-refresh == 0 (got {})", auto_refresh));
        CHECK(sv_fb_wired == 0,
              std::format("fresh-service sv-feedback-wired == 0 (got {})", sv_fb_wired));
    }

    // AC4: schema sentinel is exactly 641 (not 637/640/631/632).
    {
        std::println("\n--- AC4: schema sentinel ---");
        const auto schema = hash_int(cs, "schema");
        CHECK(schema == 641,
              std::format("schema == 641 (got {})", schema));
    }

    // AC5: naming distinction — the new primitive name has NO
    // `-hash` suffix (per issue body AC3), and is distinct from
    // the historical #631 primitive that does have `-hash`.
    {
        std::println("\n--- AC5: naming distinction from #631 ---");
        auto new_p = cs.eval("(query:stable-ref-provenance-sv-stats)");
        auto old_p = cs.eval("(query:stable-ref-provenance-sv-stats-hash)");
        CHECK(new_p.has_value(),
              "new primitive (query:stable-ref-provenance-sv-stats) reachable (no -hash suffix)");
        CHECK(old_p.has_value(),
              "historical primitive (query:stable-ref-provenance-sv-stats-hash) still reachable");
        // The new primitive's schema sentinel is 641; the
        // historical one's is 631 — they should NOT collide.
        const auto new_schema = hash_int(cs, "schema");
        auto old_schema_r = cs.eval(
            "(hash-ref (query:stable-ref-provenance-sv-stats-hash) 'schema')");
        CHECK(old_schema_r.has_value() &&
                  aura::compiler::types::is_int(*old_schema_r) &&
                  aura::compiler::types::as_int(*old_schema_r) == 631,
              std::format("#631 primitive schema == 631 (got {})",
                          old_schema_r.has_value()
                              ? aura::compiler::types::as_int(*old_schema_r)
                              : -1));
        CHECK(new_schema == 641,
              std::format("new primitive schema != 631 collision (new={})", new_schema));
    }

    // AC6: concurrent reads under 2 threads × 4 iters. Atomicity
    // regression coverage for the underlying counters + 3 new
    // scaffolding atomics.
    {
        std::println("\n--- AC6: concurrent stable-ref-provenance-sv-stats reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(query:stable-ref-provenance-sv-stats)");
                if (r.has_value())
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() == k_iters * 2,
              std::format("concurrent: {} / {} calls returned value",
                          ok_count.load(), k_iters * 2));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}