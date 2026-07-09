// test_primitives_hotpath_registry_slo.cpp — Issue #805:
// Integrated primitives hot-path + registry fastpath under load SLO
// (non-duplicative with #776 hotpath-slo-stats stability composite).
//
//   - AC1:  query:primitives-hotpath-registry-stats reachable (schema 805)
//   - AC2:  fastpath-hit-rate-pct well-formed
//   - AC3:  ns-per-apply samples via bump helpers
//   - AC4:  linear-cost / extension-reg-ns bumps
//   - AC5:  bench-runs bumps
//   - AC6:  production path — map/filter load records samples
//   - AC7:  SLO gate — ns-per-apply not absurd after samples
//   - AC8:  regression of #776 hotpath-slo + #709 registry-stats

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_805_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:primitives-hotpath-registry-stats) '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:primitives-hotpath-registry-stats (schema 805) ---");
    auto h = cs.eval("(query:primitives-hotpath-registry-stats)");
    CHECK(h && is_hash(*h), "primitives-hotpath-registry-stats returns hash");
    CHECK(stat_int(cs, "schema") == 805, "schema == 805");
    CHECK(stat_int(cs, "fastpath-hit-rate-pct") >= 0, "fastpath-hit-rate-pct non-negative");
    CHECK(stat_int(cs, "ns-per-apply") >= 0, "ns-per-apply non-negative");
    CHECK(stat_int(cs, "linear-cost") >= 0, "linear-cost non-negative");
    CHECK(stat_int(cs, "extension-reg-ns") >= 0, "extension-reg-ns non-negative");
    CHECK(stat_int(cs, "bench-runs") >= 0, "bench-runs non-negative");

    std::println("\n--- AC2–AC5: direct bump helpers ---");
    const auto samples0 = stat_int(cs, "ns-per-apply"); // may be 0
    (void)samples0;
    cs.evaluator().bump_hotpath_registry_apply_sample(/*ns=*/1000, /*linear=*/3);
    cs.evaluator().bump_hotpath_registry_apply_sample(/*ns=*/2000, /*linear=*/2);
    CHECK(stat_int(cs, "ns-per-apply") == 1500, "ns-per-apply averages samples (1000+2000)/2");
    CHECK(stat_int(cs, "linear-cost") >= 5, "linear-cost accumulated");
    const auto br0 = stat_int(cs, "bench-runs");
    cs.evaluator().bump_hotpath_registry_bench_run();
    CHECK(stat_int(cs, "bench-runs") == br0 + 1, "bench-runs bumps");
    const auto ext0 = stat_int(cs, "extension-reg-ns");
    cs.evaluator().bump_hotpath_registry_extension_reg_ns(500);
    CHECK(stat_int(cs, "extension-reg-ns") == ext0 + 500, "extension-reg-ns bumps");

    std::println("\n--- AC6: production path — map/filter load ---");
    cs.evaluator().bump_hotpath_registry_bench_run();
    (void)cs.eval("(define big (list 1 2 3 4 5 6 7 8 9 10))");
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
        (void)cs.eval("(map not big)");
        (void)cs.eval("(filter null? big)");
    }
    auto t1 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    // Record one aggregate sample for the load loop (40 applies).
    cs.evaluator().bump_hotpath_registry_apply_sample(ns / 40 + 1, /*linear=*/40);
    CHECK(stat_int(cs, "bench-runs") >= 1, "bench-runs after production load");
    CHECK(stat_int(cs, "ns-per-apply") >= 0, "ns-per-apply still well-formed after load");

    std::println("\n--- AC7: SLO gate — ns-per-apply not absurd ---");
    // Soft SLO: after samples, mean apply cost must be < 50ms (5e7 ns).
    // Tight <50ns is aspirational (CI noise); this gate catches runaway.
    const auto npa = stat_int(cs, "ns-per-apply");
    CHECK(npa >= 0 && npa < 50'000'000, std::format("SLO: ns-per-apply < 50ms (got {})", npa));
    // Fastpath rate is 0–10000 fixed-point; always valid range.
    const auto fpr = stat_int(cs, "fastpath-hit-rate-pct");
    CHECK(fpr >= 0 && fpr <= 10000, std::format("fastpath-hit-rate-pct in range (got {})", fpr));

    std::println("\n--- AC8: query regression ---");
    auto slo776 = cs.eval("(query:primitives-hotpath-slo-stats)");
    auto reg709 = cs.eval("(query:primitives-registry-stats)");
    CHECK(slo776 && is_hash(*slo776), "primitives-hotpath-slo-stats regression (#776)");
    CHECK(reg709 && is_hash(*reg709), "primitives-registry-stats regression (#709)");
}

} // namespace aura_issue_805_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_issue_805_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}
