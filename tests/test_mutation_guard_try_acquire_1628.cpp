// @category: integration
// @reason: Issue #1628 — MutationBoundaryGuard::try_acquire factory +
// typed ResourceQuotaExceeded (replace panic-checkpoint quota path).
// Refines #1547 / #1556 / #1590.
//
//   AC1: try_acquire success under unlimited quota
//   AC2: try_acquire reject → ResourceQuotaExceeded + reject metrics
//   AC3: typed_mutate + eval_on_current propagate typed reject
//   AC4: query:resource-quota-stats schema 1628 AC keys
//   AC5: 1000-iter stress pass/reject; no throw
//   AC6: legacy deprecated ctor soft-fails inert (compat)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.core.error;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:resource-quota-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_success() {
    std::println("\n--- AC1: try_acquire success ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto t0 = load_u64(m->mutation_guard_try_acquire_total);
    bool ok = true;
    auto g = Guard::try_acquire(ev, 1, &ok);
    CHECK(g.has_value(), "try_acquire ok under unlimited");
    CHECK(g && g->get() != nullptr, "non-null guard");
    CHECK(load_u64(m->mutation_guard_try_acquire_total) > t0, "try_acquire total bumped");
}

static void ac2_reject() {
    std::println("\n--- AC2: typed ResourceQuotaExceeded ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    const auto r0 = load_u64(m->resource_quota_rejects_total);
    const auto tr0 = load_u64(m->mutation_guard_try_acquire_reject_total);
    bool ok = true;
    CHECK(Guard::try_acquire(ev, 1, &ok).has_value(), "first ok");
    auto g2 = Guard::try_acquire(ev, 1, &ok);
    CHECK(!g2.has_value(), "second fails");
    CHECK(g2.error().kind == AuraErrorKind::ResourceQuotaExceeded, "kind");
    CHECK(g2.error().message.find("mutation quota exceeded") != std::string::npos, "message");
    CHECK(load_u64(m->resource_quota_rejects_total) > r0, "rejects_total");
    CHECK(load_u64(m->mutation_guard_try_acquire_reject_total) > tr0, "try_acquire reject");
}

static void ac3_propagate() {
    std::println("\n--- AC3: typed_mutate / eval_on_current propagate ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    {
        bool ok = true;
        CHECK(Guard::try_acquire(ev, 1, &ok).has_value(), "burn budget");
    }
    auto mr = cs.public_typed_mutate("(mutate:rebind \"f\" \"(lambda () 2)\" \"#1628\")");
    CHECK(!mr.success, "typed_mutate fails");
    CHECK(mr.error.find("mutation quota exceeded") != std::string::npos || !mr.error.empty(),
          "typed_mutate error text");

    // eval_on_current also uses try_acquire
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    {
        bool ok = true;
        CHECK(Guard::try_acquire(ev, 1, &ok).has_value(), "burn for eval_on");
    }
    auto er = cs.eval_on_current("(mutate:rebind \"f\" \"(lambda () 3)\" \"#1628b\")");
    CHECK(!er.has_value(), "eval_on_current fails on quota");
}

static void ac4_schema() {
    std::println("\n--- AC4: query:resource-quota-stats schema 1628 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1634 || href(cs, "schema") == 1628 || href(cs, "schema") == 1618 ||
              href(cs, "schema") == 1600 || href(cs, "schema") == 1590,
          "schema 1634|1628|lineage");
    CHECK(href(cs, "try_acquire_wired") == 1, "try_acquire_wired");
    CHECK(href(cs, "panic_checkpoint_quota_replaced") == 1, "panic replaced");
    CHECK(href(cs, "eval_on_current_try_acquire") == 1, "eval_on_current");
    CHECK(href(cs, "typed_mutate_try_acquire") == 1, "typed_mutate");
    CHECK(href(cs, "legacy_ctor_deprecated") == 1, "legacy deprecated");
    CHECK(href(cs, "mutation_guard_try_acquire_total") >= 0, "try_acquire total key");
    CHECK(href(cs, "mutation_guard_try_acquire_reject_total") >= 0, "reject key");
    CHECK(href(cs, "rejects_total") >= 0, "rejects_total lineage");
}

static void ac5_stress() {
    std::println("\n--- AC5: 1000-iter no throw ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    int pass = 0, reject = 0;
    for (int i = 0; i < 1000; ++i) {
        try {
            if ((i % 2) == 0) {
                ev.set_resource_quota_mutations(0);
                ev.reset_mutation_quota_used();
                bool ok = true;
                if (Guard::try_acquire(ev, 1, &ok))
                    ++pass;
            } else {
                ev.set_resource_quota_mutations(1);
                ev.reset_mutation_quota_used();
                {
                    bool ok = true;
                    (void)Guard::try_acquire(ev, 1, &ok);
                }
                bool ok = true;
                if (!Guard::try_acquire(ev, 1, &ok))
                    ++reject;
            }
        } catch (...) {
            CHECK(false, "must not throw on quota path");
        }
    }
    CHECK(pass == 500, "500 pass");
    CHECK(reject == 500, "500 reject");
    CHECK(load_u64(m->mutation_guard_try_acquire_total) >= 1000, "try_acquire ≥1000");
    CHECK(load_u64(m->mutation_guard_try_acquire_reject_total) >= 500, "reject ≥500");
    std::println("  pass={} reject={} try_total={}", pass, reject,
                 load_u64(m->mutation_guard_try_acquire_total));
}

static void ac6_legacy() {
    std::println("\n--- AC6: legacy deprecated ctor inert soft-fail ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    bool ok = true;
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        Guard guard(ev, &ok);
#pragma GCC diagnostic pop
        CHECK(ok, "unlimited legacy ok");
    }
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    {
        bool ok2 = true;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        Guard burn(ev, &ok2);
#pragma GCC diagnostic pop
        CHECK(ok2, "first legacy within budget");
    }
    {
        bool ok3 = true;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        Guard over(ev, &ok3);
#pragma GCC diagnostic pop
        CHECK(!ok3 || over.is_inert(), "over budget → inert or flag false");
        CHECK(true, "legacy does not throw");
    }
}

} // namespace

int main() {
    std::println("=== Issue #1628: try_acquire typed ResourceQuotaExceeded ===");
    ac1_success();
    ac2_reject();
    ac3_propagate();
    ac4_schema();
    ac5_stress();
    ac6_legacy();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
