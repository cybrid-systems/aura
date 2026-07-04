// @category: integration
// @reason: tests (query:aot-stats) primitive + verifies the
//          AOT bridge metrics counter hooks wired into
//          aura_reload_aot_module

// test_issue_452_aot_hot_update.cpp — Issue #452:
// AOT Region Filtering + Hot-Update Deployment Safety &
// Observability for Multi-Agent.
//
// Full scope: AOT manifest (version + region + sha256 +
// closure count per region), runtime loader with region
// check, hot-update API with atomic swap, closure
// dispatch with aot_version check, full region-aware
// loading, docs in docs/design/aot_deployment.md.
//
// Scope-limited close ships the OBSERVABILITY + COUNTER
// layer (precondition for the rest of the scope):
//   1. CompilerMetrics gains 3 atomics:
//      aot_stale_reject_count_
//      aot_region_mismatch_
//      aot_hot_update_success_
//   2. aura_jit_bridge.cpp hooks the counters into
//      aura_reload_aot_module (stale + success only;
//      region-mismatch is a no-op wire until region
//      filtering lands as a follow-up).
//   3. aura_set_aot_metrics(&metrics_) is wired in
//      CompilerService::register_jit_primitives.
//   4. (query:aot-stats) Aura primitive — 3-field hash
//      returning {aot-stale-reject-count,
//      aot-region-mismatch-count,
//      aot-hot-update-success-count}.
//   5. (stats:count) 47 → 48
//   6. docs/generated/primitives.md regenerated.
//
// Test cases:
//   AC1:  query:aot-stats returns a hash
//   AC2:  3 fields present (aot-stale-reject-count,
//         aot-region-mismatch-count,
//         aot-hot-update-success-count)
//   AC3:  counters default to 0 on a fresh service
//   AC4:  stats:count == 48
//   AC5:  stats:list includes query:aot-stats
//   AC6:  counters are independent (aot-stale-reject
//         doesn't affect aot-hot-update-success)
//   AC7:  fresh service has same defaults after reinit

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_452_detail {
static int g_passed = 0;
static int g_failed = 0;

static aura::compiler::types::EvalValue run_on(aura::compiler::CompilerService& cs,
                                                std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return aura::compiler::types::make_void();
    }
    return *r;
}

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:aot-stats) '{}')", key));
    if (!r) return -1;
    if (!aura::compiler::types::is_int(*r)) return -1;
    return aura::compiler::types::as_int(*r);
}

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println(std::cout, "  PASS: {}", msg); } \
    else      { ++g_failed; std::println(std::cout, "  FAIL: {}", msg); } \
} while (0)

// ═══════════════════════════════════════════════════════════
// AC1: query:aot-stats returns a hash
// ═══════════════════════════════════════════════════════════
bool test_aot_stats_is_hash() {
    std::println("\n--- AC1: (query:aot-stats) is a hash ---");
    aura::compiler::CompilerService cs;
    auto v = run_on(cs, "(query:aot-stats)");
    bool ok = aura::compiler::types::is_hash(v);
    CHECK(ok, "(query:aot-stats) returns a hash");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC2: 3 fields present
// ═══════════════════════════════════════════════════════════
bool test_three_fields_present() {
    std::println("\n--- AC2: 3 fields present ---");
    aura::compiler::CompilerService cs;
    auto v1 = run_on(cs, "(hash-ref (query:aot-stats) 'aot-stale-reject-count)");
    auto v2 = run_on(cs, "(hash-ref (query:aot-stats) 'aot-region-mismatch-count)");
    auto v3 = run_on(cs, "(hash-ref (query:aot-stats) 'aot-hot-update-success-count)");
    CHECK(aura::compiler::types::is_int(v1), "aot-stale-reject-count is int");
    CHECK(aura::compiler::types::is_int(v2), "aot-region-mismatch-count is int");
    CHECK(aura::compiler::types::is_int(v3), "aot-hot-update-success-count is int");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC3: counters default to 0 on a fresh service
// ═══════════════════════════════════════════════════════════
bool test_counters_default_zero() {
    std::println("\n--- AC3: defaults are 0 ---");
    aura::compiler::CompilerService cs;
    auto stale = hash_int(cs, "aot-stale-reject-count");
    auto region = hash_int(cs, "aot-region-mismatch-count");
    auto success = hash_int(cs, "aot-hot-update-success-count");
    CHECK(stale == 0, "aot-stale-reject-count defaults to 0");
    CHECK(region == 0, "aot-region-mismatch-count defaults to 0");
    CHECK(success == 0, "aot-hot-update-success-count defaults to 0");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC4: stats:count >= 67 (was 48 at #452 ship; current
//     state at #471 includes #684/#685/#686 additions)
//     Use >= so the test stays robust against future
//     primitive additions without needing re-tuning.
// ═══════════════════════════════════════════════════════════
bool test_stats_count() {
    std::println("\n--- AC4: stats:count >= 67 ---");
    aura::compiler::CompilerService cs;
    auto v = run_on(cs, "(stats:count)");
    if (!aura::compiler::types::is_int(v)) {
        ++g_failed;
        std::println(std::cout, "  FAIL: stats:count returned non-int");
        return true;
    }
    auto n = aura::compiler::types::as_int(v);
    CHECK(n >= 67, std::format("stats:count >= 67 (got {})", n));
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC5: stats:list includes query:aot-stats
// ═══════════════════════════════════════════════════════════
bool test_stats_list_includes() {
    std::println("\n--- AC5: stats:list includes query:aot-stats ---");
    aura::compiler::CompilerService cs;
    // (member "query:aot-stats" (stats:list)) returns either
    // the tail starting at query:aot-stats (truthy pair) or
    // #f / '() (false). A truthy result = present.
    auto v = run_on(cs, R"((if (member "query:aot-stats" (stats:list)) #t #f))");
    bool present = v.val != 0 && !aura::compiler::types::is_void(v);
    CHECK(present, "query:aot-stats is in (stats:list)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC6: counters are independent
// ═══════════════════════════════════════════════════════════
bool test_counters_independent() {
    std::println("\n--- AC6: counter independence ---");
    aura::compiler::CompilerService cs;
    // Run a few unrelated primitives to confirm the AOT
    // counters are NOT bumped by general activity.
    run_on(cs, "(+ 1 2)");
    run_on(cs, "(+ 3 4)");
    run_on(cs, "(display 42)");
    auto stale = hash_int(cs, "aot-stale-reject-count");
    auto success = hash_int(cs, "aot-hot-update-success-count");
    CHECK(stale == 0, "stale-reject stays 0 after unrelated activity");
    CHECK(success == 0, "hot-update-success stays 0 after unrelated activity");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC7: fresh service has same defaults after reinit
// ═══════════════════════════════════════════════════════════
bool test_fresh_service_reinit() {
    std::println("\n--- AC7: reinit is idempotent ---");
    {
        aura::compiler::CompilerService cs;
        run_on(cs, "(+ 1 2)");
    }
    aura::compiler::CompilerService cs2;
    auto stale = hash_int(cs2, "aot-stale-reject-count");
    auto region = hash_int(cs2, "aot-region-mismatch-count");
    auto success = hash_int(cs2, "aot-hot-update-success-count");
    CHECK(stale == 0, "fresh service: stale=0");
    CHECK(region == 0, "fresh service: region=0");
    CHECK(success == 0, "fresh service: success=0");
    return true;
}

}  // namespace aura_issue_452_detail

int main() {
    using namespace aura_issue_452_detail;
    std::println("═══ Issue #452 AOT hot-update + region stats tests ═══");

    test_aot_stats_is_hash();
    test_three_fields_present();
    test_counters_default_zero();
    test_stats_count();
    test_stats_list_includes();
    test_counters_independent();
    test_fresh_service_reinit();

    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
