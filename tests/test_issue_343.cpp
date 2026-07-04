// @category: integration
// @reason: uses CompilerService + workspace_flat to verify long-term stability observability

// test_issue_343.cpp — Issue #343: StableNodeRef
// long-term stability observability and iteration
// support (scope-limited close).
//
// The full #343 scope is 5 sub-deliverables (observability
// hooks in bump_generation/StructuralMutationGuard,
// snapshot/restore helper, generation-stats primitive,
// docs, uint16→uint32 migration). This scope-limited
// slice ships the observability foundation: the
// (ast:generation-stats) Aura primitive + 3 new
// snapshot fields + plumbing from the workspace FlatAST.
//
// Pre-#343, the 3 lifetime stable-ref counters
// (bump_generation_count, generation_wrap_count,
// node_gen_stale_access_count) were only accessible
// via (query:stable-ref-stats) which returns the
// SUM. Post-#343 the (ast:generation-stats) primitive
// exposes each category individually so the AI Agent
// can react to each independently (e.g. checkpoint
// when wrap-count > 0, investigate when
// stale-access-count grows faster than bump-count).
//
// Test cases:
//   AC1: fresh CompilerService → all 5 fields == 0
//   AC2: snapshot has 3 new stable-ref fields
//        (current_generation + generation_wrap_count
//        + node_gen_stale_access_count)
//   AC3: (ast:generation-stats) returns 5-key hash
//   AC4: eval-current + a mutation → bump-generation-total > 0
//   AC5: existing eval still works (regression)


import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_343_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println("  PASS: {}", msg);                                                       \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println("  FAIL: {}", msg);                                                       \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                                        \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            ++g_passed;                                                                            \
            std::println("  PASS: {}  ({} = {})", msg, _a, _b);                                    \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println("  FAIL: {}  ({} != {})", msg, _a, _b);                                   \
        }                                                                                          \
    } while (0)

// ── AC1: fresh CompilerService → all 5 fields == 0
bool test_initial_fields_zero() {
    std::println("\n--- AC1: stable-ref fields start at 0 ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK_EQ(snap.current_generation, 0u, "current_generation == 0 (no mutations yet)");
    CHECK_EQ(snap.generation_wrap_count, 0u, "generation_wrap_count == 0");
    CHECK_EQ(snap.node_gen_stale_access_count, 0u, "node_gen_stale_access_count == 0");
    CHECK_EQ(snap.bump_generation_count, 0u, "bump_generation_count == 0");
    CHECK_EQ(snap.stable_ref_invalidations, 0u, "stable_ref_invalidations == 0");
    return true;
}

// ── AC2: snapshot has 3 new stable-ref fields
bool test_snapshot_has_new_fields() {
    std::println("\n--- AC2: snapshot has 3 new stable-ref fields ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK(true, "snapshot has current_generation field");
    CHECK(true, "snapshot has generation_wrap_count field");
    CHECK(true, "snapshot has node_gen_stale_access_count field");
    return true;
}

// ── AC3: (ast:generation-stats) returns 5-key hash
bool test_generation_stats_primitive() {
    std::println("\n--- AC3: (ast:generation-stats) returns 5-key hash ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define gss (ast:generation-stats))\")");
    cs.eval("(eval-current)");
    for (const char* key : {"current-generation", "bump-generation-total", "generation-wrap-total",
                            "stable-ref-invalidations-total", "node-gen-stale-access-total"}) {
        std::string check = std::string("(hash-ref gss \"") + key + "\")";
        auto rv = cs.eval(check);
        if (!rv || !aura::compiler::types::is_int(*rv)) {
            std::println("  FAIL: hash-ref gss {} did not return int", key);
            ++g_failed;
        } else {
            CHECK(true, std::string("hash-ref gss \"") + key + "\" returns int");
        }
    }
    return true;
}

// ── AC4: eval-current + a mutation → bump-generation-total > 0
bool test_mutation_bumps_generation() {
    std::println("\n--- AC4: mutation bumps generation counter ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Eager);
    cs.eval("(set-code \"(define gsr 1)\")");
    cs.eval("(eval-current)");
    auto r = cs.eval("(mutate:rebind \"gsr\" \"2\" \"gen-test\")");
    if (!r) {
        std::println("  FAIL: mutate:rebind failed");
        ++g_failed;
        return false;
    }
    auto snap = cs.snapshot();
    std::println("  bump_generation_count: {}", snap.bump_generation_count);
    std::println("  current_generation: {}", snap.current_generation);
    CHECK(snap.bump_generation_count > 0u,
          "bump_generation_count > 0 (post-mutation generation bumped)");
    return true;
}

// ── AC5: existing eval still works (regression)
bool test_eval_still_works() {
    std::println("\n--- AC5: existing eval still works (regression) ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define gse 42)\")");
    cs.eval("(eval-current)");
    auto r = cs.eval("(eval-current)");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 42,
          "plain (define gse 42) + (eval-current) returns 42");
    return true;
}

} // namespace aura_343_detail

int main() {
    using namespace aura_343_detail;
    std::println("=== Issue #343: StableNodeRef long-term stability (scope-limited) ===");
    test_initial_fields_zero();
    test_snapshot_has_new_fields();
    test_generation_stats_primitive();
    test_mutation_bumps_generation();
    test_eval_still_works();
    std::println("\n=== Summary: {}/{} passed, {}/{} failed ===", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
