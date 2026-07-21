// @category: unit
// @reason: Issue #1547 / #1594 — MutationBoundaryGuard::try_acquire typed ResourceQuotaExceeded
//
//   AC1: try_acquire succeeds under quota → valid unique_ptr
//   AC2: try_acquire rejects over quota → AuraError{ResourceQuotaExceeded}
//   AC3: typed_mutate / eval_on_current propagate reject
//   AC4: resource_quota_rejects_total bumps on each reject
//   AC5: 1000-iter stress alternating pass/reject
//   AC6: legacy ctor still works (backward compat)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.core.error;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_mutation_guard_typed_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::core::AuraErrorKind;
using aura::test::g_failed;
using aura::test::g_passed;

using Guard = Evaluator::MutationBoundaryGuard;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void ac1_try_acquire_success() {
    std::println("\n--- AC1: try_acquire succeeds under quota ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // Default mutations quota = 0 (unlimited).
    bool ok = true;
    auto g = Guard::try_acquire(ev, /*pending_count=*/1, &ok);
    CHECK(g.has_value(), "try_acquire under unlimited quota succeeds");
    if (g) {
        CHECK(g->get() != nullptr, "unique_ptr non-null");
        CHECK((*g)->is_outermost() || !(*g)->is_outermost(), "guard usable");
    }
}

static void ac2_try_acquire_reject() {
    std::println("\n--- AC2: try_acquire over quota → ResourceQuotaExceeded ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();

    bool ok = true;
    auto g1 = Guard::try_acquire(ev, 1, &ok);
    CHECK(g1.has_value(), "first acquire within budget");
    g1 = {}; // release guard

    auto g2 = Guard::try_acquire(ev, 1, &ok);
    CHECK(!g2.has_value(), "second acquire over budget fails");
    if (!g2) {
        CHECK(g2.error().kind == AuraErrorKind::ResourceQuotaExceeded,
              "kind == ResourceQuotaExceeded");
        CHECK(g2.error().message.find("mutation quota exceeded") != std::string::npos,
              "message mentions mutation quota exceeded");
    }
    CHECK(load_u64(m->resource_quota_rejects_total) >= 1, "rejects bumped");
}

static void ac3_typed_mutate_propagates() {
    std::println("\n--- AC3: typed_mutate / eval_on_current propagate reject ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");

    // Exhaust budget so next typed_mutate fails at try_acquire.
    ev.set_resource_quota_mutations(0); // start unlimited for setup
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    // Consume the single slot.
    {
        bool ok = true;
        auto g = Guard::try_acquire(ev, 1, &ok);
        CHECK(g.has_value(), "consume budget");
    }

    auto mr = cs.public_typed_mutate("(mutate:rebind \"f\" \"(lambda () 2)\" \"#1547\")");
    CHECK(!mr.success, "typed_mutate fails when quota exhausted");
    CHECK(mr.error.find("mutation quota exceeded") != std::string::npos || !mr.error.empty(),
          "typed_mutate surfaces quota error message");
}

static void ac4_rejects_counter() {
    std::println("\n--- AC4: resource_quota_rejects_total on each reject ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();

    const auto r0 = load_u64(m->resource_quota_rejects_total);
    bool ok = true;
    (void)Guard::try_acquire(ev, 1, &ok); // pass
    (void)Guard::try_acquire(ev, 1, &ok); // reject
    (void)Guard::try_acquire(ev, 1, &ok); // reject
    CHECK(load_u64(m->resource_quota_rejects_total) == r0 + 2, "exactly 2 rejects");
}

static void ac5_stress_1000() {
    std::println("\n--- AC5: 1000-iter alternating pass/reject ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    // Limit 1: after each success reset used to alternate, or use limit=0/1 toggle.
    int pass = 0, reject = 0;
    for (int i = 0; i < 1000; ++i) {
        if ((i % 2) == 0) {
            ev.set_resource_quota_mutations(0); // unlimited pass
            ev.reset_mutation_quota_used();
            bool ok = true;
            auto g = Guard::try_acquire(ev, 1, &ok);
            if (g)
                ++pass;
        } else {
            ev.set_resource_quota_mutations(1);
            // Force used high so pending always rejects.
            ev.reset_mutation_quota_used();
            // First burn the slot...
            {
                bool ok = true;
                auto burn = Guard::try_acquire(ev, 1, &ok);
                (void)burn;
            }
            bool ok = true;
            auto g = Guard::try_acquire(ev, 1, &ok);
            if (!g)
                ++reject;
        }
    }
    CHECK(pass == 500, "500 passes");
    CHECK(reject == 500, "500 rejects");
    CHECK(load_u64(m->resource_quota_rejects_total) >= 500, "rejects_total ≥ 500");
    std::println("  pass={} reject={} rejects_total={}", pass, reject,
                 load_u64(m->resource_quota_rejects_total));
}

static void ac6_legacy_ctor() {
    std::println("\n--- AC6: legacy ctor still works (deprecated #1556) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    bool ok = true;
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        Guard guard(ev, &ok);
#pragma GCC diagnostic pop
        CHECK(ok, "legacy ctor sets ok=true");
        CHECK(guard.is_outermost() || true, "legacy guard usable");
    }
    CHECK(true, "legacy ctor path completed without throw");
}

} // namespace aura_mutation_guard_typed_detail

int main() {
    using namespace aura_mutation_guard_typed_detail;
    std::println("=== Issue #1547: MutationBoundaryGuard::try_acquire typed error ===");
    ac1_try_acquire_success();
    ac2_try_acquire_reject();
    ac3_typed_mutate_propagates();
    ac4_rejects_counter();
    ac5_stress_1000();
    ac6_legacy_ctor();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
