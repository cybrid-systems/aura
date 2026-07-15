// @category: integration
// @reason: CompilerService facade + residual stats demotion (#1450 Phase 2)
//
// test_issue_1450.cpp — Epic #1449 Phase 1 / Issue #1450:
// Observability Facade reinforcement + residual public *-stats removal.
//
// ACs:
//   AC1: (stats:get name) routes catalog names without requiring std/stats
//   AC2: residual 10 names are NOT public Primitives (facade-only)
//   AC3: deprecated alias dispatch still bumps counter (query:children)
//   AC4: (engine:surface) public-stats-remaining == 0
//   AC5: (stats:prefix "query:") returns a non-empty name list
//   AC6: (stats:count) catalog still ≤ 420 target

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1450_detail {

#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::println("  FAIL: {} (line {})", msg, __LINE__);                                   \
            ++g_failed;                                                                            \
        } else {                                                                                   \
            std::println("  PASS: {}", msg);                                                       \
            ++g_passed;                                                                            \
        }                                                                                          \
    } while (0)

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_void;

std::int64_t hash_int(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void ac1_stats_get() {
    std::println("\n--- AC1: (stats:get) engine facade ---");
    CompilerService cs;
    auto r = cs.eval("(stats:get \"gc-stats\")");
    CHECK(r.has_value() && !is_void(*r), "(stats:get \"gc-stats\") returns non-void");
    auto u = cs.eval("(stats:get \"definitely-not-a-stats-name-zzz\")");
    CHECK(u.has_value() && is_void(*u), "unknown stats:get → void");
    // Bidirectional / residual also via facade
    auto b = cs.eval("(stats:get \"compile:bidirectional-stats\")");
    CHECK(b.has_value(), "(stats:get \"compile:bidirectional-stats\") callable");
}

void ac2_residual_not_public() {
    std::println("\n--- AC2: residual *-stats not on public Primitives ---");
    CompilerService cs;
    auto& prims = cs.evaluator().primitives();
    static constexpr const char* kResidual[] = {
        "arena:adaptive-stats",        "arena:defrag-stats",     "ast:generation-stats",
        "ast:node-lifecycle-stats",    "ast:post-restore-stats", "closure:free-stats",
        "compile:bidirectional-stats", "gc-arena-stats",         "gc-stats",
        "type-registry-stats",
    };
    std::size_t still_public = 0;
    for (const char* name : kResidual) {
        if (prims.slot_for_name(name) < prims.slot_count())
            ++still_public;
    }
    CHECK(still_public == 0,
          std::format("0 residual public *-stats (got {} still public)", still_public));
    // Bare name is no longer a public primitive — eval may be nullopt/error/void.
    // Must not crash the process.
    try {
        auto bare = cs.eval("(gc-stats)");
        CHECK(true, "bare (gc-stats) does not crash process");
        if (bare.has_value() && !is_void(*bare) && !is_error(*bare)) {
            // If it returns a normal value, something re-registered it.
            CHECK(false, "bare (gc-stats) must not resolve as a live stats prim");
        } else {
            CHECK(true, "bare (gc-stats) unbound/void/error (expected)");
        }
    } catch (...) {
        CHECK(false, "bare (gc-stats) must not throw");
    }
}

void ac3_dispatch_telemetry() {
    std::println("\n--- AC3: deprecated alias dispatch bumps counter ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    (void)cs.eval("(set-code \"(define (f x) (+ x 1))\")");
    const auto before = ev.deprecated_prim_dispatch_total();
    auto r = cs.eval("(query:children (query:root))");
    CHECK(r.has_value(), "(query:children …) still executes (compat)");
    const auto after = ev.deprecated_prim_dispatch_total();
    CHECK(after > before, std::format("deprecated_prim_dispatch_total {} → {}", before, after));
}

void ac4_engine_surface() {
    std::println("\n--- AC4: (engine:surface) inventory ---");
    CompilerService cs;
    auto r = cs.eval("(engine:surface)");
    CHECK(r.has_value() && is_hash(*r), "(engine:surface) returns hash");
    CHECK(hash_int(cs, "(engine:surface)", "schema") == 1, "schema == 1");
    CHECK(hash_int(cs, "(engine:surface)", "target-budget") == 420, "target-budget == 420");
    CHECK(hash_int(cs, "(engine:surface)", "interim-ceiling") == 700, "interim-ceiling == 700");
    const auto cat = hash_int(cs, "(engine:surface)", "stats-catalog-count");
    CHECK(cat > 0 && cat <= 420, std::format("stats-catalog-count={} in (0,420]", cat));
    const auto pub_stats = hash_int(cs, "(engine:surface)", "public-stats-remaining");
    CHECK(pub_stats == 0, std::format("public-stats-remaining={} (all facade-only)", pub_stats));
}

void ac5_stats_prefix() {
    std::println("\n--- AC5: (stats:prefix \"query:\") ---");
    CompilerService cs;
    auto r = cs.eval("(stats:prefix \"query:\")");
    CHECK(r.has_value() && !is_void(*r), "(stats:prefix \"query:\") non-void");
    CHECK(is_pair(*r), "(stats:prefix) returns a list (pair spine)");
}

void ac6_stats_count() {
    std::println("\n--- AC6: (stats:count) ≤ 420 ---");
    CompilerService cs;
    auto r = cs.eval("(stats:count)");
    CHECK(r.has_value() && is_int(*r), "(stats:count) returns int");
    if (r && is_int(*r)) {
        const auto n = as_int(*r);
        CHECK(n <= 420, std::format("stats:count={} ≤ 420", n));
        std::println("  info: stats:count = {}", n);
    }
}

} // namespace aura_issue_1450_detail

int main() {
    using namespace aura_issue_1450_detail;
    std::println("=== Issue #1450 Phase 2: residual public *-stats → facade ===");
    ac1_stats_get();
    ac2_residual_not_public();
    ac3_dispatch_telemetry();
    ac4_engine_surface();
    ac5_stats_prefix();
    ac6_stats_count();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
