// @category: unit
// @reason: Issue #2619 — Agent-visible Moving densify health + untracked hard
//          surface (pairs #2596 production hard default).
//
//   AC1: Query exposes pin_contract / untracked / production-hard after window
//   AC2: Production + incomplete remap → would-allow-mutate = false
//   AC3: Soft/sandbox observe-only unless #2596 hard active
//   AC4: No densify → vacuous healthy
//   AC5: Schema additive; source-cite #2596 / #2495 / #2619

#include "core/arena_auto_policy_stats.h"
#include "core/moving_densify_health.hh"
#include "test_harness.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.arena; // Issue #2775: ASTArena::register_external_root_for_densify direct test

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;
namespace mdh = aura::core::moving_densify_health;
using aura::ast::ASTArena; // Issue #2775: prep API direct test surface

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:arena-moving-densify-health\") \"{}\")",
                    std::string(key)));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: query exposes window fields ──
static void ac1_query_exposes_window() {
    std::println("\n--- #2619 AC1: query exposes densify window state ---");
    CHECK(mdh::kMovingDensifyHealthIssue == 2619, "AC1: issue stamp");
    mdh::reset_moving_densify_health_for_test();
    // Synthetic densify window (incomplete untracked).
    mdh::publish_last_moving_densify_window(/*had*/ true, /*pin*/ true, /*incomplete*/ true,
                                            /*objects_moved*/ 4, /*untracked_kept*/ 2,
                                            /*root_fail*/ 0);

    CompilerService cs;
    CHECK(href(cs, "objects-moved") == 4, "AC1: objects-moved");
    CHECK(href(cs, "untracked-kept") == 2, "AC1: untracked-kept");
    CHECK(href(cs, "pin-contract-held") == 1, "AC1: pin-contract-held");
    CHECK(href(cs, "moving-incomplete-remap") == 1, "AC1: moving-incomplete-remap");
    CHECK(href(cs, "had-moving-densify") == 1, "AC1: had-moving-densify");
    CHECK(href(cs, "window-seq") >= 1, "AC1: window-seq advanced");
    CHECK(href(cs, "moving-densify-health-wired") == 1, "AC1: wired");
    // production-hard depends on pref; just ensure key present (0 or 1)
    CHECK(href(cs, "production-hard-active") == 0 || href(cs, "production-hard-active") == 1,
          "AC1: production-hard-active present");
}

// ── AC2: incomplete → would-allow-mutate false ──
static void ac2_incomplete_denies_mutate() {
    std::println("\n--- #2619 AC2: incomplete remap denies mutate ---");
    mdh::reset_moving_densify_health_for_test();
    // Force production-hard so throttle also engages.
    const auto prev = aura::ast::g_moving_untracked_hard_abort_pref.load();
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);

    mdh::publish_last_moving_densify_window(true, true, true, 3, 1, 0);
    auto s = mdh::snapshot();
    CHECK(!s.would_allow_mutate, "AC2: would_allow_mutate false");
    CHECK(s.force_reason_code == mdh::kForceUntrackedIncomplete, "AC2: force untracked");
    CHECK(s.agent_throttle, "AC2: agent_throttle under production-hard");

    CompilerService cs;
    CHECK(href(cs, "would-allow-mutate") == 0, "AC2: query would-allow-mutate=0");
    CHECK(href(cs, "force-reason-code") == mdh::kForceUntrackedIncomplete,
          "AC2: query force-reason");
    CHECK(href(cs, "agent-throttle") == 1, "AC2: query agent-throttle");

    // Healthy densify clears throttle under hard.
    mdh::publish_last_moving_densify_window(true, true, false, 2, 0, 0);
    s = mdh::snapshot();
    CHECK(s.would_allow_mutate, "AC2: healthy densify allows");
    CHECK(!s.agent_throttle, "AC2: throttle cleared on healthy");

    aura::ast::g_moving_untracked_hard_abort_pref.store(prev, std::memory_order_relaxed);
}

// ── AC3: soft observe-only ──
static void ac3_soft_observe_only() {
    std::println("\n--- #2619 AC3: soft path observe-only (no throttle) ---");
    mdh::reset_moving_densify_health_for_test();
    const auto prev = aura::ast::g_moving_untracked_hard_abort_pref.load();
    // Soft / observe-only (pref -1 or 0)
    aura::ast::g_moving_untracked_hard_abort_pref.store(-1, std::memory_order_relaxed);
    CHECK(!mdh::production_hard_active(), "AC3: production hard inactive");

    mdh::publish_last_moving_densify_window(true, false, true, 1, 5, 0);
    auto s = mdh::snapshot();
    CHECK(!s.would_allow_mutate, "AC3: window still reports deny (observe)");
    CHECK(!s.agent_throttle, "AC3: no agent_throttle under soft");
    CHECK(s.untracked_kept == 5, "AC3: untracked still published");

    CompilerService cs;
    CHECK(href(cs, "would-allow-mutate") == 0, "AC3: query deny");
    CHECK(href(cs, "agent-throttle") == 0, "AC3: query throttle off");
    CHECK(href(cs, "production-hard-active") == 0, "AC3: production-hard=0");

    aura::ast::g_moving_untracked_hard_abort_pref.store(prev, std::memory_order_relaxed);
}

// ── AC4: no densify vacuous healthy ──
static void ac4_no_densify_healthy() {
    std::println("\n--- #2619 AC4: no densify → vacuous healthy ---");
    mdh::reset_moving_densify_health_for_test();
    auto s = mdh::snapshot();
    CHECK(!s.had_moving_densify, "AC4: no densify");
    CHECK(s.would_allow_mutate, "AC4: would_allow true");
    CHECK(s.pin_contract_held, "AC4: pin held default");
    CHECK(s.force_reason_code == mdh::kForceNone, "AC4: force none");
    CHECK(!s.agent_throttle, "AC4: no throttle");

    // Explicit Soft publish (had=false) stays healthy
    mdh::publish_last_moving_densify_window(false, true, false, 0, 0, 0);
    s = mdh::snapshot();
    CHECK(s.would_allow_mutate, "AC4: soft publish still allows");

    CompilerService cs;
    CHECK(href(cs, "would-allow-mutate") == 1, "AC4: query healthy");
    CHECK(href(cs, "had-moving-densify") == 0, "AC4: had-moving-densify=0");
}

// ── AC5: source-cite ──
static void ac5_source_cite() {
    std::println("\n--- #2619 AC5: source-cite + schema ---");
    const auto hh = read_file("src/core/moving_densify_health.hh");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto b = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(hh.find("#2619") != std::string::npos, "AC5: header #2619");
    CHECK(hh.find("#2596") != std::string::npos, "AC5: lineage #2596");
    CHECK(hh.find("#2495") != std::string::npos, "AC5: lineage #2495");
    CHECK(q.find("query:arena-moving-densify-health") != std::string::npos,
          "AC5: query registered");
    CHECK(b.find("moving_densify_health") != std::string::npos, "AC5: Phase 5 publish");

    CompilerService cs;
    CHECK(href(cs, "schema-2619") == 2619, "AC5: schema-2619");
    CHECK(href(cs, "issue-2619") == 2619, "AC5: issue-2619");
    CHECK(href(cs, "schema-2596") == 2596, "AC5: schema-2596");
    CHECK(href(cs, "schema-2495") == 2495, "AC5: schema-2495");
    CHECK(mdh::moving_densify_health_wired() == 1, "AC5: wired live");
}

// ── #2682 AC1/AC2/AC4: single unified predicate (5-condition AND) ──
static void ac2682_unified_predicate_all_conditions() {
    std::println("\n--- #2682 AC1/AC2/AC4: unified predicate covers all 5 conditions ---");
    // True path: all conditions hold → success.
    CHECK(mdh::compute_moving_unified_success(
              /*moving_blocked_precondition=*/false,
              /*pin_contract_held=*/true,
              /*root_remap_stable_ref_fail_total=*/0,
              /*root_remap_closure_capture_fail_total=*/0,
              /*objects_moved=*/5,
              /*untracked_kept_count=*/0) == true,
          "AC2: clean registered roots + objects_moved=5 → unified success");

    // AC4: root_remap_stable_ref_fail_total > 0 alone → fail.
    CHECK(mdh::compute_moving_unified_success(false, true, 1, 0, 0, 0) == false,
          "AC4: root_remap_stable_ref_fail_total > 0 → unified fail");

    // AC4: root_remap_closure_capture_fail_total > 0 alone → fail.
    CHECK(mdh::compute_moving_unified_success(false, true, 0, 1, 0, 0) == false,
          "AC4: root_remap_closure_capture_fail_total > 0 → unified fail");

    // AC1: objects_moved > 0 && untracked_kept_count > 0 → fail.
    CHECK(mdh::compute_moving_unified_success(false, true, 0, 0, 1, 1) == false,
          "AC1: objects_moved > 0 && untracked_kept_count > 0 → unified fail");

    // AC2: objects_moved > 0 && untracked_kept_count == 0 → success.
    CHECK(mdh::compute_moving_unified_success(false, true, 0, 0, 5, 0) == true,
          "AC2: objects_moved > 0 && untracked_kept_count == 0 → success");

    // moving_blocked_precondition alone → fail.
    CHECK(mdh::compute_moving_unified_success(true, true, 0, 0, 0, 0) == false,
          "AC1: moving_blocked_precondition → unified fail");

    // pin_contract_held=false alone → fail.
    CHECK(mdh::compute_moving_unified_success(false, false, 0, 0, 0, 0) == false,
          "AC2: pin_contract_held=false → unified fail");

    // Vacuous healthy: no objects_moved AND untracked_kept_count > 0 → success
    // (AC4 condition only applies when objects_moved > 0).
    CHECK(mdh::compute_moving_unified_success(false, true, 0, 0, 0, 3) == true,
          "AC1: objects_moved=0 ignores untracked_kept_count → success");
}

// ── #2682 AC5: process-wide counters + query surface ──
static void ac2682_counters_and_query_wired() {
    std::println("\n--- #2682 AC5: counters + query surface ---");
    CompilerService cs;
    // Both new query keys wired alongside existing #2619 schema.
    CHECK(href(cs, "schema-2682") == 2682, "AC5: schema-2682 sentinel");
    CHECK(href(cs, "issue-2682") == 2682, "AC5: issue-2682 sentinel");
    CHECK(href(cs, "moving-unified-success-gate-wired") == 1, "AC5: gate-wired sentinel");
    // Counters queryable (must be >= 0; monotonic, no schema break).
    const auto success_total = href(cs, "moving-unified-success-total");
    const auto fail_total = href(cs, "moving-unified-fail-total");
    CHECK(success_total >= 0, "AC5: success-total queryable (>= 0)");
    CHECK(fail_total >= 0, "AC5: fail-total queryable (>= 0)");

    // Schema-2619 still works (additive — no regression).
    CHECK(href(cs, "schema-2619") == 2619, "AC5: legacy schema-2619 still wired");
}

// ── #2682 AC6: source-cite + no regression ──
static void ac2682_source_cite() {
    std::println("\n--- #2682 AC6: source-cite + no regression ---");
    const auto hh = read_file("src/core/moving_densify_health.hh");
    const auto arena = read_file("src/core/arena.ixx");
    const auto phase5 = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");

    // Issue #2682 sentinel in all 4 prod-side files (use "#2682" for combined
    // citations like "Issue #2682 / #2341 / #2619").
    CHECK(hh.find("#2682") != std::string::npos, "AC6: moving_densify_health.hh cites #2682");
    CHECK(arena.find("#2682") != std::string::npos, "AC6: arena.ixx cites #2682");
    CHECK(phase5.find("#2682") != std::string::npos,
          "AC6: evaluator_mutation_boundary.cpp cites #2682");
    CHECK(q.find("#2682") != std::string::npos,
          "AC6: evaluator_primitives_obs_jit.cpp cites #2682");

    // Predicate function declared in header.
    CHECK(hh.find("compute_moving_unified_success") != std::string::npos,
          "AC6: unified predicate declared in header");

    // Counters declared + called from Phase 5.
    CHECK(arena.find("g_moving_unified_success_total") != std::string::npos,
          "AC6: success counter declared");
    CHECK(arena.find("g_moving_unified_fail_total") != std::string::npos,
          "AC6: fail counter declared");
    CHECK(phase5.find("compute_moving_unified_success") != std::string::npos,
          "AC6: Phase 5 calls unified predicate");

    // No design doc regression (per #1655).
    for (const auto& p :
         {"docs/design/moving_unified_success_2682.md", "docs/moving_unified_success_2682.md"}) {
        std::ifstream f(p);
        CHECK(!f.good(), "AC6: no design doc at " + std::string(p));
    }
}

// ── #2775 AC1: prep API single register bumps counter + set state ──
static void ac2775_prep_register_single() {
    std::println("\n--- #2775 AC1: prep API single register ---");
    ASTArena arena;
    int dummy1 = 0;
    const auto before = aura::ast::g_moving_external_root_prep_register_total.load();
    arena.register_external_root_for_densify(&dummy1);
    CHECK(aura::ast::g_moving_external_root_prep_register_total.load() == before + 1,
          "AC1: counter +1 after single register");
    CHECK(arena.external_roots_for_densify_count() == 1, "AC1: count() == 1");
}

// ── #2775 AC2: batch span register bumps counter by N ──
static void ac2775_prep_register_batch() {
    std::println("\n--- #2775 AC2: prep API batch span ---");
    ASTArena arena;
    int dummies[5] = {};
    void* ptrs[5] = {&dummies[0], &dummies[1], &dummies[2], &dummies[3], &dummies[4]};
    std::span<void* const> span_ptrs(ptrs, 5);
    const auto before = aura::ast::g_moving_external_root_prep_register_total.load();
    arena.register_external_root_for_densify(span_ptrs);
    CHECK(aura::ast::g_moving_external_root_prep_register_total.load() == before + 5,
          "AC2: counter +5 after batch span");
    CHECK(arena.external_roots_for_densify_count() == 5, "AC2: count() == 5");
}

// ── #2775 AC3: duplicate register no-op (set dedup) ──
static void ac2775_prep_register_dup() {
    std::println("\n--- #2775 AC3: duplicate register no-op ---");
    ASTArena arena;
    int dummy = 0;
    void* p = &dummy;
    const auto before = aura::ast::g_moving_external_root_prep_register_total.load();
    arena.register_external_root_for_densify(p);
    CHECK(aura::ast::g_moving_external_root_prep_register_total.load() == before + 1,
          "AC3: first register +1");
    arena.register_external_root_for_densify(p);
    CHECK(aura::ast::g_moving_external_root_prep_register_total.load() == before + 1,
          "AC3: duplicate register no-op (set dedup)");
    CHECK(arena.external_roots_for_densify_count() == 1, "AC3: count() == 1");
}

// ── #2775 AC4: nullptr no-op (counter + set unchanged) ──
static void ac2775_prep_register_null() {
    std::println("\n--- #2775 AC4: nullptr no-op ---");
    ASTArena arena;
    const auto before = aura::ast::g_moving_external_root_prep_register_total.load();
    arena.register_external_root_for_densify(nullptr);
    CHECK(aura::ast::g_moving_external_root_prep_register_total.load() == before,
          "AC4: nullptr no-op (counter unchanged)");
    CHECK(arena.external_roots_for_densify_count() == 0, "AC4: count() == 0");
}

// ── #2775 AC5: explicit clear_external_roots_for_densify() ──
static void ac2775_prep_clear_explicit() {
    std::println("\n--- #2775 AC5: explicit clear ---");
    ASTArena arena;
    int dummies[3] = {};
    arena.register_external_root_for_densify(&dummies[0]);
    arena.register_external_root_for_densify(&dummies[1]);
    arena.register_external_root_for_densify(&dummies[2]);
    CHECK(arena.external_roots_for_densify_count() == 3, "AC5: 3 registered");
    arena.clear_external_roots_for_densify();
    CHECK(arena.external_roots_for_densify_count() == 0, "AC5: explicit clear");
    // Note: counter is process-wide cumulative and does NOT roll back on
    // clear (only set membership does). Counter semantics are
    // "register events", not "currently registered".
}

// ── #2775 AC6: publish_last_moving_densify_window prep_count → snapshot ──
static void ac2775_prep_publish_snapshot() {
    std::println("\n--- #2775 AC6: publish prep_count → snapshot ---");
    mdh::reset_moving_densify_health_for_test();
    mdh::publish_last_moving_densify_window(
        /*had*/ true, /*pin*/ true, /*incomplete*/ false,
        /*objects_moved*/ 5, /*untracked_kept*/ 0, /*root_fail*/ 0,
        /*external_roots_prep_registered_cleared*/ 42);
    const auto s = mdh::snapshot();
    CHECK(s.external_roots_prep_registered_last == 42,
          "AC6: snapshot.external_roots_prep_registered_last == 42");
}

// ── #2775 AC7: default arg backward-compat (existing 6-arg calls work) ──
static void ac2775_prep_default_arg_compat() {
    std::println("\n--- #2775 AC7: 6-arg call backward-compat ---");
    mdh::reset_moving_densify_health_for_test();
    mdh::publish_last_moving_densify_window(true, true, false, 5, 0, 0);
    const auto s = mdh::snapshot();
    CHECK(s.external_roots_prep_registered_last == 0,
          "AC7: 6-arg call uses default 0 for prep_count");
}

// ── #2775 AC8: source-cite + no regression ──
static void ac2775_source_cite() {
    std::println("\n--- #2775 AC8: source-cite + no design doc ---");
    const auto arena_src = read_file("src/core/arena.ixx");
    const auto hh = read_file("src/core/moving_densify_health.hh");
    const auto phase5 = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto test_self = read_file("tests/compiler/test_arena_moving_densify_health.cpp");
    // #2775 citation in all 4 touched files.
    CHECK(arena_src.find("#2775") != std::string::npos, "AC8: arena.ixx cites #2775");
    CHECK(hh.find("#2775") != std::string::npos, "AC8: moving_densify_health.hh cites #2775");
    CHECK(phase5.find("#2775") != std::string::npos,
          "AC8: evaluator_mutation_boundary.cpp cites #2775");
    CHECK(test_self.find("#2775") != std::string::npos,
          "AC8: test_arena_moving_densify_health.cpp cites #2775");
    // Prep API + counter + snapshot + aggregation all wired.
    CHECK(arena_src.find("register_external_root_for_densify") != std::string::npos,
          "AC8: prep API registered in arena.ixx");
    CHECK(hh.find("external_roots_prep_registered_last") != std::string::npos,
          "AC8: snapshot field in moving_densify_health.hh");
    CHECK(arena_src.find("g_moving_external_root_prep_register_total") != std::string::npos,
          "AC8: process-wide counter declared");
    CHECK(arena_src.find("external_roots_prep_registered_total") != std::string::npos,
          "AC8: AdaptiveCompactResult aggregation field declared");
    CHECK(phase5.find("external_roots_prep_registered_total") != std::string::npos,
          "AC8: Phase 5 reads aggregation field");
    CHECK(phase5.find("external_roots_prep_registered_cleared") != std::string::npos,
          "AC8: Phase 5 forwards to publish");
    // No design doc regression (per #1655).
    for (const auto& p :
         {"docs/design/2775-external-root-prep.md", "docs/design/prep_register_2775.md",
          "docs/design/external-root-prep-2775.md"}) {
        std::ifstream f(p);
        CHECK(!f.good(), "AC8: no design doc at " + std::string(p));
    }
}

} // namespace

int run_test_arena_moving_densify_health() {
    std::println("=== Issue #2619 + #2682 + #2775: Agent Moving densify health ===");
    ac1_query_exposes_window();
    ac2_incomplete_denies_mutate();
    ac3_soft_observe_only();
    ac4_no_densify_healthy();
    ac5_source_cite();
    ac2682_unified_predicate_all_conditions();
    ac2682_counters_and_query_wired();
    ac2682_source_cite();
    ac2775_prep_register_single();
    ac2775_prep_register_batch();
    ac2775_prep_register_dup();
    ac2775_prep_register_null();
    ac2775_prep_clear_explicit();
    ac2775_prep_publish_snapshot();
    ac2775_prep_default_arg_compat();
    ac2775_source_cite();
    std::println("\n=== #2619/#2682/#2775: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_arena_moving_densify_health();
}
#endif
