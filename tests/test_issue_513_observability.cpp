// @category: integration
// @reason: Issue #513 — aot-hot-reload-stats hash slice

#include <iostream>
#include <print>
#include <string>

#include "aura_jit_bridge.h"

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_513_detail {
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
        std::format("(hash-ref (engine:metrics \"query:aot-hot-reload-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_513_detail

int aura_issue_513_observability_run() {
    using namespace aura_issue_513_detail;

    std::println("=== Issue #513: aot-hot-reload-stats hash ===");

    aura::compiler::CompilerService cs;

    // AC1: query:aot-hot-reload-stats returns hash with consolidated fields
    {
        std::println("\n--- AC1: query:aot-hot-reload-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:aot-hot-reload-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:aot-hot-reload-stats returns hash");
        CHECK(hash_int(cs, "reload-attempts") >= 0, "reload-attempts present");
        CHECK(hash_int(cs, "reload-success") >= 0, "reload-success present");
        CHECK(hash_int(cs, "stale-rejected") >= 0, "stale-rejected present");
        CHECK(hash_int(cs, "refcount-swaps") >= 0, "refcount-swaps present");
        CHECK(hash_int(cs, "func-table-epoch") >= 1, "func-table-epoch present");
        CHECK(hash_int(cs, "defuse-version") >= 0, "defuse-version present");
        CHECK(hash_int(cs, "aot-hot-reload-total") >= 0, "aot-hot-reload-total present");
        CHECK(hash_int(cs, "aot-hot-reload-recommendation") >= 0,
              "aot-hot-reload-recommendation present");
    }

    const auto attempts_before = hash_int(cs, "reload-attempts");
    const auto drifts_before = hash_int(cs, "checkpoint-version-drifts");
    const auto total_before = hash_int(cs, "aot-hot-reload-total");

    // AC2: reload attempt bumps consolidated counters
    {
        std::println("\n--- AC2: reload attempt bumps counters ---");
        (void)aura_reload_aot_module(nullptr, 0);
        (void)aura_reload_aot_module("/tmp/aura_513_no_such_module.so", 99);
        const auto attempts_after = hash_int(cs, "reload-attempts");
        CHECK(attempts_after >= attempts_before + 2,
              std::format("reload-attempts grew ({} -> {})", attempts_before, attempts_after));
    }

    // AC3: checkpoint version drift probe updates consolidated hash
    {
        std::println("\n--- AC3: checkpoint version drift probe ---");
        aura_set_aot_defuse_version(513);
        const bool drift = aura_aot_probe_checkpoint_version(1, 0);
        CHECK(drift, "aura_aot_probe_checkpoint_version detects defuse drift");
        const auto drifts_after = hash_int(cs, "checkpoint-version-drifts");
        CHECK(drifts_after > drifts_before, std::format("checkpoint-version-drifts grew ({} -> {})",
                                                        drifts_before, drifts_after));
        CHECK(hash_int(cs, "defuse-version") == 513, "defuse-version reflects setter");
        aura_aot_record_deopt_on_steal();
        CHECK(hash_int(cs, "deopt-on-steal") >= 1, "deopt-on-steal bumped");
        aura_set_aot_defuse_version(0);
    }

    // AC4: legacy AOT stats primitives regression
    {
        std::println("\n--- AC4: regression ---");
        auto reload = cs.eval("(engine:metrics \"query:aot-reload-stats\")");
        CHECK(reload && aura::compiler::types::is_hash(*reload),
              "query:aot-reload-stats hash regression");
        auto checkpoint = cs.eval("(engine:metrics \"query:aot-checkpoint-version-stats\")");
        CHECK(checkpoint && aura::compiler::types::is_hash(*checkpoint),
              "query:aot-checkpoint-version-stats hash regression");
        const auto total_after = hash_int(cs, "aot-hot-reload-total");
        CHECK(total_after >= total_before,
              std::format("aot-hot-reload-total monotonic ({} -> {})", total_before, total_after));
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 110,
              "stats:count >= 110");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_513_observability_run();
}
#endif
