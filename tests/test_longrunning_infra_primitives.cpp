// test_longrunning_infra_primitives.cpp — Issue #753:
// long-running deployment infra primitives + observability
// (refines #729; non-duplicative with #548 panic-checkpoint-lifecycle,
// #677 deployment-stats, #674 chaos-stats).
//
//   - AC1:  query:longrunning-infra-stats reachable (schema 753)
//   - AC2:  quota-violations bumps on direct path
//   - AC3:  checkpoint-success bumps on direct path
//   - AC4:  heal-triggers bumps on direct path
//   - AC5:  resource-trend + deployment-slo-hits bump paths
//   - AC6:  infra-events-total == sum of 5 per-counter fields
//   - AC7:  real infra exercise (resource:quota-* + panic-checkpoint/
//           panic-restore) — stats monotonic
//   - AC8:  query regression (deployment-stats, panic-checkpoint-lifecycle,
//           self-evolution-chaos-stats)

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_753_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:longrunning-infra-stats) '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t quota_violations(CompilerService& cs) {
    return stat_int(cs, "quota-violations");
}
static std::int64_t checkpoint_success(CompilerService& cs) {
    return stat_int(cs, "checkpoint-success");
}
static std::int64_t heal_triggers(CompilerService& cs) {
    return stat_int(cs, "heal-triggers");
}
static std::int64_t resource_trend(CompilerService& cs) {
    return stat_int(cs, "resource-trend");
}
static std::int64_t deployment_slo_hits(CompilerService& cs) {
    return stat_int(cs, "deployment-slo-hits");
}
static std::int64_t events_total(CompilerService& cs) {
    return stat_int(cs, "infra-events-total");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:longrunning-infra-stats (schema 753) ---");
    auto h = cs.eval("(query:longrunning-infra-stats)");
    CHECK(h && is_hash(*h), "longrunning-infra-stats returns hash");
    CHECK(stat_int(cs, "schema") == 753, "schema == 753");
    CHECK(quota_violations(cs) >= 0, "quota-violations non-negative");
    CHECK(checkpoint_success(cs) >= 0, "checkpoint-success non-negative");
    CHECK(heal_triggers(cs) >= 0, "heal-triggers non-negative");
    CHECK(resource_trend(cs) >= 0, "resource-trend non-negative");
    CHECK(deployment_slo_hits(cs) >= 0, "deployment-slo-hits non-negative");

    std::println("\n--- AC2: quota-violations bumps on direct path ---");
    const auto q0 = quota_violations(cs);
    cs.evaluator().bump_longrunning_quota_violations();
    cs.evaluator().bump_longrunning_quota_violations();
    const auto q1 = quota_violations(cs);
    CHECK(q1 == q0 + 2, "quota-violations bumps by exactly 2");

    std::println("\n--- AC3: checkpoint-success bumps on direct path ---");
    const auto c0 = checkpoint_success(cs);
    cs.evaluator().bump_longrunning_checkpoint_success();
    cs.evaluator().bump_longrunning_checkpoint_success();
    cs.evaluator().bump_longrunning_checkpoint_success();
    const auto c1 = checkpoint_success(cs);
    CHECK(c1 == c0 + 3, "checkpoint-success bumps by exactly 3");

    std::println("\n--- AC4: heal-triggers bumps on direct path ---");
    const auto h0 = heal_triggers(cs);
    cs.evaluator().bump_longrunning_heal_triggers();
    cs.evaluator().bump_longrunning_heal_triggers();
    const auto h1 = heal_triggers(cs);
    CHECK(h1 == h0 + 2, "heal-triggers bumps by exactly 2");

    std::println("\n--- AC5: resource-trend + deployment-slo-hits ---");
    const auto t0 = resource_trend(cs);
    const auto s0 = deployment_slo_hits(cs);
    cs.evaluator().bump_longrunning_resource_trend();
    cs.evaluator().bump_longrunning_deployment_slo_hits();
    CHECK(resource_trend(cs) == t0 + 1, "resource-trend bumps by 1");
    CHECK(deployment_slo_hits(cs) == s0 + 1, "deployment-slo-hits bumps by 1");

    std::println("\n--- AC6: infra-events-total == sum ---");
    const auto q = quota_violations(cs);
    const auto c = checkpoint_success(cs);
    const auto ht = heal_triggers(cs);
    const auto t = resource_trend(cs);
    const auto s = deployment_slo_hits(cs);
    const auto tot = events_total(cs);
    CHECK(tot == q + c + ht + t + s, "infra-events-total == sum of 5 counters");

    std::println("\n--- AC7: real infra exercise (quota + checkpoint/heal) ---");
    const auto ev7a = events_total(cs);
    const auto q7a = quota_violations(cs);
    const auto slo7a = deployment_slo_hits(cs);
    const auto trend7a = resource_trend(cs);

    cs.eval("(resource:quota-set \"memory\" 100)");
    auto within = cs.eval("(resource:quota-check \"memory\" 50)");
    auto over = cs.eval("(resource:quota-check \"memory\" 150)");
    CHECK(within && is_bool(*within) && as_bool(*within), "quota-check within limit returns #t");
    CHECK(over && is_bool(*over) && !as_bool(*over), "quota-check over limit returns #f");
    CHECK(quota_violations(cs) > q7a, "quota-violations grew after over-limit check");
    CHECK(resource_trend(cs) >= trend7a + 2, "resource-trend grew after quota checks");
    CHECK(deployment_slo_hits(cs) > slo7a, "deployment-slo-hits grew after within-limit check");

    cs.eval("(set-code \"(define base 1) base\")");
    cs.eval("(eval-current)");
    const auto ck7a = checkpoint_success(cs);
    auto saved = cs.eval("(panic-checkpoint)");
    CHECK(saved && is_bool(*saved) && as_bool(*saved), "panic-checkpoint succeeds with workspace");
    CHECK(checkpoint_success(cs) > ck7a, "checkpoint-success grew after panic-checkpoint");

    cs.eval("(set-code \"(define base 999) base\")");
    cs.eval("(eval-current)");
    const auto heal7a = heal_triggers(cs);
    auto restored = cs.eval("(panic-restore)");
    CHECK(restored && is_bool(*restored) && as_bool(*restored), "panic-restore succeeds");
    CHECK(heal_triggers(cs) > heal7a, "heal-triggers grew after panic-restore");

    CHECK(events_total(cs) > ev7a, "infra-events-total monotonic over real infra matrix");

    std::println("\n--- AC8: query regression ---");
    auto dep = cs.eval("(query:deployment-stats)");
    auto panic = cs.eval("(query:panic-checkpoint-lifecycle-stats)");
    auto chaos = cs.eval("(query:self-evolution-chaos-stats)");
    CHECK(dep && is_hash(*dep), "deployment-stats regression");
    CHECK(panic && is_int(*panic), "panic-checkpoint-lifecycle-stats regression");
    CHECK(chaos && is_hash(*chaos), "self-evolution-chaos-stats regression");
}

} // namespace aura_issue_753_detail

int aura_issue_longrunning_infra_primitives_run() {
    aura::compiler::CompilerService cs;
    aura_issue_753_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_longrunning_infra_primitives_run();
}
#endif
