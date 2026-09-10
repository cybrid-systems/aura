// @category: unit
// @reason: Issue #3627 — pack-binary binding fixture. Compiled WITH
// AURA_PRODUCTION_PACK=1 (the same define as the production `aura`
// target), so apply_pipeline_strict_defaults is exercised through the
// pack arm: env unset → Forbidden regardless of dev_sandbox_off, while
// AURA_PIPELINE_STRICT operator overrides still win. The non-pack Soft
// face lives in test_tree_walker_fallback_strict.cpp (sandbox=off →
// Allow, #2213 AC2).

#include "compiler/pipeline_policy.hh"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using aura::compiler::apply_pipeline_strict_defaults;
using aura::compiler::production_pipeline_strict;
using aura::compiler::reset_tree_walker_fallback_policy_for_test;
using aura::compiler::tree_walker_fallback_disposition;
using aura::compiler::tree_walker_fallback_policy;
using aura::compiler::TreeWalkerFallbackDisposition;
using aura::compiler::TreeWalkerFallbackPolicy;

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond, label)                                                                         \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::printf("  FAIL: %s\n", label);                                                    \
        }                                                                                          \
    } while (0)

void ac3627_default_and_reset() {
    std::printf("\n--- 3627 AC0: static default + reset_for_test stay Allow ---\n");
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Allow,
          "static atomic default Allow before any apply");
    reset_tree_walker_fallback_policy_for_test();
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Allow,
          "reset_for_test still Allow (unit Soft path unchanged)");
}

void ac3627_pack_env_unset() {
    std::printf("\n--- 3627 AC1: pack + env unset -> Forbidden, never TakeWalker ---\n");
    (void)unsetenv("AURA_PIPELINE_STRICT");
    (void)unsetenv("AURA_SANDBOX");
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Forbidden,
          "pack + sandbox=off + env unset -> Forbidden (#3627 AC1)");
    CHECK(production_pipeline_strict(), "pack binding implies pipeline strict");
    CHECK(tree_walker_fallback_disposition(true) == TreeWalkerFallbackDisposition::HardError,
          "needs -> HardError, never TakeWalker");
    CHECK(tree_walker_fallback_disposition(false) == TreeWalkerFallbackDisposition::ContinueIr,
          "happy path -> ContinueIr (policy load only, no extra branch)");
}

void ac3627_pack_ignores_dev_flag() {
    std::printf("\n--- 3627 AC2: pack ignores dev_sandbox_off entirely ---\n");
    (void)unsetenv("AURA_PIPELINE_STRICT");
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/false);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Forbidden,
          "pack + dev_off=false -> Forbidden (same strict binding)");
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Forbidden,
          "pack + dev_off=true -> Forbidden (sandbox knob does not unlock)");
}

void ac3627_operator_wins() {
    std::printf("\n--- 3627 AC3: AURA_PIPELINE_STRICT operator override wins in pack ---\n");
    (void)setenv("AURA_PIPELINE_STRICT", "allow", 1);
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Allow,
          "env allow -> operator Soft in pack (#3627 AC3)");
    CHECK(tree_walker_fallback_disposition(true) == TreeWalkerFallbackDisposition::TakeWalker,
          "operator allow -> TakeWalker (explicit, not silent)");

    (void)setenv("AURA_PIPELINE_STRICT", "force-soa", 1);
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::ForceSoa,
          "env force-soa -> ForceSoa in pack");
    CHECK(tree_walker_fallback_disposition(true) == TreeWalkerFallbackDisposition::ContinueIr,
          "ForceSoa -> ContinueIr (no silent walker)");

    (void)setenv("AURA_PIPELINE_STRICT", "1", 1);
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Forbidden,
          "env 1 -> Forbidden in pack");
    (void)unsetenv("AURA_PIPELINE_STRICT");
    reset_tree_walker_fallback_policy_for_test();
}

} // namespace

int main() {
    std::printf("=== Issue #3627: pack-binary binding (AURA_PRODUCTION_PACK fixture) ===\n");
    ac3627_default_and_reset();
    ac3627_pack_env_unset();
    ac3627_pack_ignores_dev_flag();
    ac3627_operator_wins();
    std::printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
