// @category: integration
// @reason: Issue #522 — aot-production-reload-stats hash slice

#include <iostream>
#include <print>
#include <string>

#include "aura_jit_bridge.h"

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_522_detail {
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
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:aot-production-reload-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_522_detail

int aura_issue_522_observability_run() {
    using namespace aura_issue_522_detail;

    std::println("=== Issue #522: aot-production-reload-stats hash ===");

    aura::compiler::CompilerService cs;

    // AC1: query:aot-production-reload-stats returns hash
    {
        std::println("\n--- AC1: query:aot-production-reload-stats ---");
        auto stats = cs.eval("(query:aot-production-reload-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:aot-production-reload-stats returns hash");
        CHECK(hash_int(cs, "reload-attempts") >= 0, "reload-attempts present");
        CHECK(hash_int(cs, "func-table-epoch") >= 1, "func-table-epoch present");
        CHECK(hash_int(cs, "module-version") >= 0, "module-version present");
        CHECK(hash_int(cs, "host-region-mask") >= 0, "host-region-mask present");
        CHECK(hash_int(cs, "swap-success-rate-pct") >= 0, "swap-success-rate-pct present");
        CHECK(hash_int(cs, "aot-production-reload-total") >= 0,
              "aot-production-reload-total present");
        CHECK(hash_int(cs, "aot-production-reload-recommendation") >= 0,
              "aot-production-reload-recommendation present");
    }

    const auto attempts_before = hash_int(cs, "reload-attempts");
    const auto total_before = hash_int(cs, "aot-production-reload-total");

    // AC2: reload attempt bumps production counters
    {
        std::println("\n--- AC2: reload attempt bumps counters ---");
        (void)aura_reload_aot_module(nullptr, 0);
        (void)aura_reload_aot_module("/tmp/aura_522_no_such_module.so", 99);
        const auto attempts_after = hash_int(cs, "reload-attempts");
        CHECK(attempts_after >= attempts_before + 2,
              std::format("reload-attempts grew ({} -> {})", attempts_before, attempts_after));
    }

    // AC3: module-version + region mask reflected in hash
    {
        std::println("\n--- AC3: module-version + region namespace ---");
        aura_set_module_version(522);
        aura_set_aot_defuse_version(522);
        aura_set_aot_region_mask(0x522);
        CHECK(hash_int(cs, "module-version") == 522, "module-version reflects setter");
        CHECK(hash_int(cs, "defuse-version") == 522, "defuse-version reflects setter");
        CHECK(hash_int(cs, "host-region-mask") == 0x522, "host-region-mask reflects setter");
        aura_set_aot_region_mask(0xB);
        (void)aura_reload_aot_module("/tmp/aura_522_region_mismatch.so", 522);
        CHECK(hash_int(cs, "region-violations") >= 0, "region-violations readable after mask set");
        aura_set_aot_region_mask(0);
        aura_set_aot_defuse_version(0);
        aura_set_module_version(0);
    }

    // AC4: checkpoint drift + legacy AOT stats regression
    {
        std::println("\n--- AC4: regression ---");
        aura_set_aot_defuse_version(100);
        (void)aura_aot_probe_checkpoint_version(1, 0);
        const auto total_after = hash_int(cs, "aot-production-reload-total");
        CHECK(total_after >= total_before,
              std::format("aot-production-reload-total monotonic ({} -> {})", total_before,
                          total_after));
        auto hot = cs.eval("(query:aot-hot-reload-stats)");
        CHECK(hot && aura::compiler::types::is_hash(*hot),
              "query:aot-hot-reload-stats hash regression");
        auto reload = cs.eval("(query:aot-reload-stats)");
        CHECK(reload && aura::compiler::types::is_hash(*reload),
              "query:aot-reload-stats hash regression");
        aura_set_aot_defuse_version(0);
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 118,
              "stats:count >= 118");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_522_observability_run();
}
#endif
