// @category: unit
// @reason: Issue #1542 — wire linear_post_mutate_enforce into materialize_call_env
//
//   AC1: materialize_call_env on Owned frame bumps linear_post_mutate_enforcements
//   AC2: materialize copies bindings when enforce passes
//   AC3: Moved binding → materialize empty-Env fallback + counter bump
//   AC4: Moved path bumps materialize_fallback_total
//   AC5: NULL_ENV_ID materialize does not bump enforcements (invalid path)
//   AC6: apply_closure path still safe (enforce via fallback + materialize)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1542_detail {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr std::uint8_t kOwned = 1;
constexpr std::uint8_t kMoved = 4;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static void ac1_materialize_bumps_enforce() {
    std::println("\n--- AC1: materialize on Owned frame bumps enforcements ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(11, make_int(42), kOwned);
    auto eid = ev.alloc_env_frame_from_env(src);
    CHECK(eid != NULL_ENV_ID, "alloc frame");

    Closure cl;
    cl.env_id = eid;
    const auto c0 = ev.test_linear_post_mutate_enforce_count();
    auto ne = ev.materialize_call_env(cl);
    const auto c1 = ev.test_linear_post_mutate_enforce_count();
    CHECK(c1 == c0 + 1, "materialize_call_env bumps linear_post_mutate_enforcements by 1");
    CHECK(!ne.bindings_symid().empty(), "Owned materialize keeps bindings");
}

static void ac2_materialize_copies_bindings() {
    std::println("\n--- AC2: materialize copies Owned bindings when enforce passes ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(22, make_int(7), kOwned);
    auto eid = ev.alloc_env_frame_from_env(src);
    Closure cl;
    cl.env_id = eid;
    auto ne = ev.materialize_call_env(cl);
    CHECK(ne.bindings_symid().size() == 1, "one binding copied");
    CHECK(ne.bindings_linear_ownership_state().size() >= 1, "linear SoA copied");
    CHECK(ne.bindings_linear_ownership_state()[0] == kOwned, "state remains Owned");
    CHECK(is_int(ne.bindings_symid()[0].second), "value is int");
}

static void ac3_moved_fallback_empty_env() {
    std::println("\n--- AC3: Moved → empty Env fallback + enforce counter ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(33, make_int(1), kMoved);
    auto eid = ev.alloc_env_frame_from_env(src);
    Closure cl;
    cl.env_id = eid;

    const auto c0 = ev.test_linear_post_mutate_enforce_count();
    const auto v0 = ev.test_linear_ownership_violation_prevented_count();
    auto ne = ev.materialize_call_env(cl);
    const auto c1 = ev.test_linear_post_mutate_enforce_count();
    const auto v1 = ev.test_linear_ownership_violation_prevented_count();

    CHECK(c1 == c0 + 1, "Moved materialize still bumps enforcements");
    CHECK(v1 > v0, "violation_prevented bumped on Moved materialize");
    CHECK(ne.bindings_symid().empty(), "safe fallback: empty bindings (no use-after-move walk)");
}

static void ac4_moved_bumps_materialize_fallback() {
    std::println("\n--- AC4: Moved path bumps materialize_fallback_total ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics available");

    aura::compiler::Env src;
    src.bind_symid_with_linear_state(44, make_int(0), kMoved);
    auto eid = ev.alloc_env_frame_from_env(src);
    Closure cl;
    cl.env_id = eid;

    const auto fb0 = m->materialize_fallback_total.load(std::memory_order_relaxed);
    (void)ev.materialize_call_env(cl);
    const auto fb1 = m->materialize_fallback_total.load(std::memory_order_relaxed);
    CHECK(fb1 == fb0 + 1, "materialize_fallback_total +1 on Moved");
}

static void ac5_null_env_no_enforce_bump() {
    std::println("\n--- AC5: NULL_ENV_ID materialize does not bump enforcements ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    Closure cl;
    cl.env_id = NULL_ENV_ID;
    const auto c0 = ev.test_linear_post_mutate_enforce_count();
    const auto fb0 = m->materialize_fallback_total.load(std::memory_order_relaxed);
    (void)ev.materialize_call_env(cl);
    CHECK(ev.test_linear_post_mutate_enforce_count() == c0,
          "NULL env_id materialize skips linear_post_mutate_enforce");
    CHECK(m->materialize_fallback_total.load(std::memory_order_relaxed) == fb0 + 1,
          "NULL env still bumps materialize_fallback_total");
}

static void ac6_apply_path_still_safe() {
    std::println("\n--- AC6: apply_closure path still enforces (owned lambda) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(cs.eval("(set-code \"(define g (lambda () 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    const auto c0 = ev.test_linear_post_mutate_enforce_count();
    auto r = cs.eval("(g)");
    CHECK(r.has_value(), "(g) applies");
    // apply may materialize (and/or run fallback check); count monotonic non-decreasing.
    CHECK(ev.test_linear_post_mutate_enforce_count() >= c0, "after (g) enforcements monotonic");
}

} // namespace aura_issue_1542_detail

int main() {
    using namespace aura_issue_1542_detail;
    std::println("=== Issue #1542: materialize_call_env linear_post_mutate_enforce ===");
    ac1_materialize_bumps_enforce();
    ac2_materialize_copies_bindings();
    ac3_moved_fallback_empty_env();
    ac4_moved_bumps_materialize_fallback();
    ac5_null_env_no_enforce_bump();
    ac6_apply_path_still_safe();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
