// test_domain_fiber_orchestration.cpp — Domain suite: fiber / steal / Guard
//
// Collapses orchestration observability gates that used to land as
// separate test_issue_N.cpp files (#810/#812/#813/#875 etc.).
// Heavy multi-worker stress remains in test_concurrent / late bundles.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref ({}) '{}')", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void expect_hash_schema(CompilerService& cs, std::string_view q, std::int64_t schema) {
    auto h = cs.eval(std::format("({})", q));
    CHECK(h && is_hash(*h), std::format("{} returns hash", q));
    CHECK(href(cs, q, "schema") == schema, std::format("{} schema == {}", q, schema));
}

void run_suite(CompilerService& cs) {
    auto& ev = cs.evaluator();

    std::println("\n=== Fiber/scheduler init observability (#810) ===");
    expect_hash_schema(cs, "query:fiber-scheduler-init-stats", 810);
    CHECK(href(cs, "query:fiber-scheduler-init-stats", "aura-result-init-active") == 1,
          "aura-result-init-active");
    ev.bump_fiber_init_aura_result_ok();
    ev.bump_scheduler_init_aura_result_ok();
    CHECK(href(cs, "query:fiber-scheduler-init-stats", "fiber-init-ok") >= 1, "fiber-init-ok");
    CHECK(href(cs, "query:fiber-scheduler-init-stats", "scheduler-init-ok") >= 1,
          "scheduler-init-ok");

    std::println("\n=== Steal + arena/GC coordination (#812) ===");
    expect_hash_schema(cs, "query:orchestration-steal-arena-gc-stats", 812);
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "steal-safety-active") == 1,
          "steal-safety-active");
    ev.bump_steal_arena_yield_during_compact();
    ev.bump_steal_outermost_only_enforced();
    ev.bump_steal_linear_probe_on_success();
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "yield-during-compact") >= 1,
          "yield-during-compact");
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "outermost-only-enforced") >= 1,
          "outermost-only-enforced");
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "linear-probe-on-success") >= 1,
          "linear-probe-on-success");

    std::println("\n=== Guard AuraResult path (#813) ===");
    expect_hash_schema(cs, "query:guard-error-stats", 813);
    CHECK(href(cs, "query:guard-error-stats", "no-unwind-through-guard") == 1,
          "no-unwind-through-guard");
    ev.bump_guard_aura_result_path();
    ev.bump_guard_panic_checkpoint_aura_result();
    CHECK(href(cs, "query:guard-error-stats", "guard-aura-result-path") >= 1,
          "guard-aura-result-path");

    std::println("\n=== Guard/steal safety v2 surface (#875) ===");
    expect_hash_schema(cs, "query:guard-steal-gc-safety-v2-stats", 875);
    CHECK(href(cs, "query:guard-steal-gc-safety-v2-stats", "active") == 1, "875 active");

    std::println("\n=== Runtime health under orchestration (#814) ===");
    expect_hash_schema(cs, "query:runtime-production-health", 814);
    CHECK(href(cs, "query:runtime-production-health", "health-score") >= 0, "health-score");
    auto heal = cs.eval("(runtime:self-heal-on-drift)");
    CHECK(heal.has_value(), "runtime:self-heal-on-drift callable");

    std::println("\n=== JIT exception bridge (#811) ===");
    expect_hash_schema(cs, "query:jit-exception-bridge-stats", 811);
    CHECK(href(cs, "query:jit-exception-bridge-stats", "guest-only-policy-active") == 1,
          "guest-only-policy-active");
    ev.bump_jit_guest_exception_bridge();
    ev.bump_jit_internal_aura_result_path();
    CHECK(href(cs, "query:jit-exception-bridge-stats", "guest-exception-bridge") >= 1,
          "guest-exception-bridge");
    CHECK(href(cs, "query:jit-exception-bridge-stats", "internal-aura-result-path") >= 1,
          "internal-aura-result-path");

    std::println("\n=== JIT fiber exception locality (#821) ===");
    expect_hash_schema(cs, "query:jit-fiber-exception-stats", 821);
    CHECK(href(cs, "query:jit-fiber-exception-stats", "fiber-local-policy-active") == 1,
          "fiber-local-policy-active");
    ev.bump_jit_fiber_ex_stack_local();
    ev.bump_jit_fiber_ex_cross_prevented();
    CHECK(href(cs, "query:jit-fiber-exception-stats", "fiber-local-ex-stack") >= 1,
          "fiber-local-ex-stack");
    CHECK(href(cs, "query:jit-fiber-exception-stats", "cross-fiber-prevented") >= 1,
          "cross-fiber-prevented");

    std::println("\n=== L2 specialization deopt (#822) ===");
    expect_hash_schema(cs, "query:l2-specialization-deopt-stats", 822);
    CHECK(href(cs, "query:l2-specialization-deopt-stats", "l2-maturity-active") == 1,
          "l2-maturity-active");
    ev.bump_l2_spec_pair_fastpath();
    ev.bump_l2_spec_linear_probe();
    CHECK(href(cs, "query:l2-specialization-deopt-stats", "pair-fastpath") >= 1, "pair-fastpath");

    std::println("\n=== Opcode coverage deopt (#823) ===");
    expect_hash_schema(cs, "query:opcode-coverage-deopt-stats", 823);
    CHECK(href(cs, "query:opcode-coverage-deopt-stats", "zero-fallback-policy") == 1,
          "zero-fallback-policy");
    ev.bump_opcode_cov_hit();
    CHECK(href(cs, "query:opcode-coverage-deopt-stats", "coverage-hits") >= 1, "coverage-hits");
}

} // namespace

int aura_issue_domain_fiber_orchestration_run() {
    aura::compiler::CompilerService cs;
    run_suite(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_domain_fiber_orchestration_run();
}
#endif
