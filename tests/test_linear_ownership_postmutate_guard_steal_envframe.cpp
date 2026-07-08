// test_linear_ownership_postmutate_guard_steal_envframe.cpp — Issue #800:
// Linear ownership post-mutate fidelity observability (Guard rollback,
// steal resume, EnvFrame version sync; refines #793/#792/#784/#791).
//
//   - AC1:  query:linear-postmutate-fidelity-stats reachable (schema 800)
//   - AC2:  post-rollback-revalidate-hits bumps on direct path
//   - AC3:  escape-violations-prevented bumps on direct path
//   - AC4:  guard-boundary-linear-safe bumps on direct path
//   - AC5:  env-version-sync bumps on direct path
//   - AC6:  restore_panic_checkpoint success bumps post-rollback-revalidate
//   - AC7:  resume_fiber_migration bumps post-rollback-revalidate
//   - AC8:  query regression (#763 gc-compiler, #638 safety-stats)

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" void aura_evaluator_resume_fiber_migration();

namespace aura_issue_800_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:linear-postmutate-fidelity-stats) '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t post_rollback_revalidate(CompilerService& cs) {
    return stat_int(cs, "post-rollback-revalidate-hits");
}
static std::int64_t escape_prevented(CompilerService& cs) {
    return stat_int(cs, "escape-violations-prevented");
}
static std::int64_t guard_linear_safe(CompilerService& cs) {
    return stat_int(cs, "guard-boundary-linear-safe");
}
static std::int64_t env_version_sync(CompilerService& cs) {
    return stat_int(cs, "env-version-sync");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:linear-postmutate-fidelity-stats (schema 800) ---");
    auto h = cs.eval("(query:linear-postmutate-fidelity-stats)");
    CHECK(h && is_hash(*h), "linear-postmutate-fidelity-stats returns hash");
    CHECK(stat_int(cs, "schema") == 800, "schema == 800");
    CHECK(post_rollback_revalidate(cs) >= 0, "post-rollback-revalidate-hits non-negative");
    CHECK(escape_prevented(cs) >= 0, "escape-violations-prevented non-negative");
    CHECK(guard_linear_safe(cs) >= 0, "guard-boundary-linear-safe non-negative");
    CHECK(env_version_sync(cs) >= 0, "env-version-sync non-negative");

    std::println("\n--- AC2: post-rollback-revalidate-hits bumps on direct path ---");
    const auto p0 = post_rollback_revalidate(cs);
    cs.evaluator().bump_linear_postmutate_post_rollback_revalidate(2);
    CHECK(post_rollback_revalidate(cs) == p0 + 2, "post-rollback-revalidate-hits bumps by 2");

    std::println("\n--- AC3: escape-violations-prevented bumps on direct path ---");
    const auto e0 = escape_prevented(cs);
    cs.evaluator().bump_linear_postmutate_escape_violations_prevented();
    CHECK(escape_prevented(cs) == e0 + 1, "escape-violations-prevented bumps by 1");

    std::println("\n--- AC4: guard-boundary-linear-safe bumps on direct path ---");
    const auto g0 = guard_linear_safe(cs);
    cs.evaluator().bump_linear_postmutate_guard_boundary_linear_safe(2);
    CHECK(guard_linear_safe(cs) == g0 + 2, "guard-boundary-linear-safe bumps by 2");

    std::println("\n--- AC5: env-version-sync bumps on direct path ---");
    const auto v0 = env_version_sync(cs);
    cs.evaluator().bump_linear_postmutate_env_version_sync();
    CHECK(env_version_sync(cs) == v0 + 1, "env-version-sync bumps by 1");

    std::println("\n--- AC6: panic-restore bumps post-rollback-revalidate ---");
    const auto p6a = post_rollback_revalidate(cs);
    cs.eval("(set-code \"(define base 1) base\")");
    cs.eval("(eval-current)");
    auto saved = cs.eval("(panic-checkpoint)");
    CHECK(saved && is_bool(*saved) && as_bool(*saved), "panic-checkpoint succeeds");
    cs.eval("(set-code \"(define base 999) base\")");
    cs.eval("(eval-current)");
    auto restored = cs.eval("(panic-restore)");
    CHECK(restored && is_bool(*restored) && as_bool(*restored), "panic-restore succeeds");
    CHECK(post_rollback_revalidate(cs) > p6a,
          "post-rollback-revalidate-hits grew after panic-restore");

    std::println("\n--- AC7: resume_fiber_migration bumps post-rollback-revalidate ---");
    const auto p7a = post_rollback_revalidate(cs);
    aura_evaluator_resume_fiber_migration();
    CHECK(post_rollback_revalidate(cs) > p7a,
          "post-rollback-revalidate-hits grew after resume_fiber_migration");

    std::println("\n--- AC8: query regression ---");
    auto gc763 = cs.eval("(query:linear-ownership-gc-compiler-stats)");
    auto safe638 = cs.eval("(query:linear-ownership-safety-stats)");
    CHECK(gc763 && is_hash(*gc763), "linear-ownership-gc-compiler-stats regression (#763)");
    CHECK(safe638 && is_int(*safe638), "linear-ownership-safety-stats regression (#638)");
}

} // namespace aura_issue_800_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_issue_800_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}