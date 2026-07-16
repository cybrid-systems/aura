// test_mutation_hold_time.cpp — Issue #1375:
// MutationBoundaryGuard hold-time metrics + 9-bucket histogram.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <chrono>
#include <cstdint>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-boundary-hold-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void spin_us(std::int64_t min_us) {
    auto t0 = std::chrono::steady_clock::now();
    while (
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count() < min_us) {
    }
}

} // namespace

int main() {
    // ── enter_ts_ drives hold counters ──
    {
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        const auto h0 = metrics.mutation_boundary_holds_total.load();
        const auto t0 = metrics.mutation_boundary_hold_time_total_us.load();
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
            spin_us(200); // land in 100–500us or 500us–1ms bucket
        }
        CHECK(metrics.mutation_boundary_holds_total.load() == h0 + 1, "holds_total +1");
        CHECK(metrics.mutation_boundary_hold_time_total_us.load() > t0, "hold_time_us increased");
        CHECK(metrics.mutation_hold_duration_us_total.load() > 0, "#1253 total also bumped");
        // Histogram: at least one bucket non-zero; sum == holds
        std::uint64_t hist_sum = 0;
        for (std::size_t i = 0; i < CompilerMetrics::kMutationBoundaryHoldHistBuckets; ++i)
            hist_sum += metrics.mutation_boundary_hold_histogram[i].load();
        CHECK(hist_sum == metrics.mutation_boundary_holds_total.load(), "hist sum == holds_total");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── many short holds populate low buckets ──
    {
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        constexpr int kN = 50;
        for (int i = 0; i < kN; ++i) {
            bool ok = true;
            Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
            // minimal work
        }
        CHECK(metrics.mutation_boundary_holds_total.load() == static_cast<std::uint64_t>(kN),
              "50 holds");
        std::uint64_t hist_sum = 0;
        for (std::size_t i = 0; i < CompilerMetrics::kMutationBoundaryHoldHistBuckets; ++i)
            hist_sum += metrics.mutation_boundary_hold_histogram[i].load();
        CHECK(hist_sum == static_cast<std::uint64_t>(kN), "hist covers all 50");
        // Short holds should mostly be in buckets 0–2 (<1ms)
        const auto b0 = metrics.mutation_boundary_hold_histogram[0].load();
        const auto b1 = metrics.mutation_boundary_hold_histogram[1].load();
        const auto b2 = metrics.mutation_boundary_hold_histogram[2].load();
        CHECK(b0 + b1 + b2 >= static_cast<std::uint64_t>(kN / 2), "majority of holds under 1ms");
        const auto avg = metrics.mutation_boundary_hold_time_total_us.load() / kN;
        CHECK(avg >= 0, "avg_us computable");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── nested Guard: only outermost samples ──
    {
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard outer(cs.evaluator(), &ok);
            {
                bool ok2 = true;
                Evaluator::MutationBoundaryGuard inner(cs.evaluator(), &ok2);
                spin_us(50);
            }
        }
        CHECK(metrics.mutation_boundary_holds_total.load() == 1, "one sample for nested pair");
        std::uint64_t hist_sum = 0;
        for (std::size_t i = 0; i < CompilerMetrics::kMutationBoundaryHoldHistBuckets; ++i)
            hist_sum += metrics.mutation_boundary_hold_histogram[i].load();
        CHECK(hist_sum == 1, "one hist entry for nested pair");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── query:mutation-boundary-hold-stats exposes histogram ──
    {
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        for (int i = 0; i < 10; ++i) {
            bool ok = true;
            Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        }
        auto r = cs.eval("(engine:metrics \"query:mutation-boundary-hold-stats\")");
        CHECK(r && is_hash(*r), "hold-stats is hash");
        CHECK(href(cs, "holds-total") == 10, "holds-total via query");
        CHECK(href(cs, "total-us") >= 0, "total-us alias");
        CHECK(href(cs, "avg-us") >= 0, "avg-us alias");
        CHECK(href(cs, "hist-sum") == 10, "hist-sum via query");
        CHECK(href(cs, "hist-buckets") == 9, "9 histogram buckets");
        CHECK(href(cs, "hist-0-100us") >= 0, "hist-0-100us key");
        CHECK(href(cs, "hist-100-500us") >= 0, "hist-100-500us key");
        CHECK(href(cs, "hist-500us-1ms") >= 0, "hist-500us-1ms key");
        CHECK(href(cs, "hist-1-5ms") >= 0, "hist-1-5ms key");
        CHECK(href(cs, "hist-5-10ms") >= 0, "hist-5-10ms key");
        CHECK(href(cs, "hist-10-50ms") >= 0, "hist-10-50ms key");
        CHECK(href(cs, "hist-50-100ms") >= 0, "hist-50-100ms key");
        CHECK(href(cs, "hist-100ms-1s") >= 0, "hist-100ms-1s key");
        CHECK(href(cs, "hist-gt-1s") >= 0, "hist-gt-1s key");
        CHECK(href(cs, "schema") == 1375, "schema 1375");
        // avg matches total/holds
        const auto total = href(cs, "hold-time-us-total");
        const auto avg = href(cs, "avg-hold-us");
        CHECK(avg == total / 10, "avg_us == total_us / holds");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── over-1ms counter on longer hold ──
    {
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        const auto o0 = metrics.mutation_boundary_holds_over_1ms_total.load();
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
            spin_us(1500);
        }
        CHECK(metrics.mutation_boundary_holds_over_1ms_total.load() >= o0 + 1,
              "over_1ms bumped after 1.5ms hold");
        // Bucket 3 is 1–5ms
        CHECK(metrics.mutation_boundary_hold_histogram[3].load() >= 1 ||
                  metrics.mutation_boundary_hold_histogram[2].load() >= 1,
              "landed in 500us–5ms buckets");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("mutation hold time #1375: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
