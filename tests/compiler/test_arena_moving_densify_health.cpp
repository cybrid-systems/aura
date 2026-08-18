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
import aura.core.lifetime_pin;

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

// #2682 AC5: the moving-unified success-gate counters are exposed on
// query:soa-dirty-stats (the block that carries the process-wide
// Moving densify unified totals), not the #2619 health surface.
// Pre-existing surface split; keep the two hrefs distinct.
static std::int64_t href_soa(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"query:soa-dirty-stats\") \"{}\")",
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
    // #2682 unified counters live on query:soa-dirty-stats (process-wide
    // totals block). Light-link binaries (e.g. test_densify_pin_batch) do
    // not always register full query:soa-dirty-stats (obs_jit
    // register_jit_p5), so the runtime query is best-effort; the schema /
    // key wiring is source-cited in AC6 + the coverage linter.
    const auto schema_q = href_soa(cs, "schema-2682");
    if (schema_q >= 0) {
        CHECK(schema_q == 2682, "AC5: schema-2682 sentinel (when query wired)");
        CHECK(href_soa(cs, "issue-2682") == 2682, "AC5: issue-2682 sentinel (when query wired)");
        CHECK(href_soa(cs, "moving-unified-success-gate-wired") == 1,
              "AC5: gate-wired sentinel (when query wired)");
        const auto success_total = href_soa(cs, "moving-unified-success-total");
        const auto fail_total = href_soa(cs, "moving-unified-fail-total");
        CHECK(success_total >= 0, "AC5: success-total queryable (>= 0)");
        CHECK(fail_total >= 0, "AC5: fail-total queryable (>= 0)");
    } else {
        // Light-link: source-cite the keys in the obs_jit stats block.
        const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(obs.find("\"schema-2682\"") != std::string::npos,
              "AC5: schema-2682 key source-cited (light link)");
        CHECK(obs.find("\"issue-2682\"") != std::string::npos,
              "AC5: issue-2682 key source-cited (light link)");
        CHECK(obs.find("moving-unified-success-total") != std::string::npos,
              "AC5: success-total key source-cited (light link)");
        CHECK(obs.find("moving-unified-fail-total") != std::string::npos,
              "AC5: fail-total key source-cited (light link)");
        CHECK(obs.find("moving-unified-success-gate-wired") != std::string::npos,
              "AC5: gate-wired key source-cited (light link)");
    }

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

// ── Issue #3123: production auto-arm + sticky-clear discipline ──

struct AutoArmPrefGuard {
    int prev = -1;
    explicit AutoArmPrefGuard(int enable) {
        prev = aura::ast::g_production_auto_arm_moving_pref.load(std::memory_order_acquire);
        aura::ast::g_production_auto_arm_moving_pref.store(enable, std::memory_order_release);
    }
    ~AutoArmPrefGuard() {
        aura::ast::g_production_auto_arm_moving_pref.store(prev, std::memory_order_release);
    }
};

struct MovingFlagGuard3123 {
    int prev = -1;
    explicit MovingFlagGuard3123(int enable) {
        prev = aura::ast::moving_compact_enabled();
        aura::ast::set_moving_compact_enabled(enable);
    }
    ~MovingFlagGuard3123() { aura::ast::set_moving_compact_enabled(prev); }
};

struct Pod16_3123 {
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    std::int32_t d = 0;
    Pod16_3123() = default;
    Pod16_3123(std::int32_t a_, std::int32_t b_, std::int32_t c_, std::int32_t d_) noexcept
        : a(a_)
        , b(b_)
        , c(c_)
        , d(d_) {}
};

static void ac3123_1_production_auto_arm_soft_never() {
    std::println("\n--- #3123 AC1: production auto-arm; Soft/sandbox never ---");
    MovingFlagGuard3123 on(1);
    aura::ast::g_last_moving_compact_ms.store(0, std::memory_order_release);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    CHECK(aura::core::lifetime::live_pin_count() == 0, "AC1: zero pins");
    {
        AutoArmPrefGuard prod(1);
        CHECK(aura::ast::should_production_auto_arm_moving(0.50),
              "AC1: production + frag 0.50 + quiet → auto-arm");
        CHECK(aura::ast::should_production_auto_arm_moving(0.40),
              "AC1: frag at threshold auto-arms");
        CHECK(!aura::ast::should_production_auto_arm_moving(0.39),
              "AC1: below threshold does not auto-arm");
    }
    {
        AutoArmPrefGuard soft(0);
        CHECK(!aura::ast::should_production_auto_arm_moving(0.90),
              "AC1: Soft pref never auto-arms");
    }
    {
        AutoArmPrefGuard unset(-1);
        // Derive path: without production_defaults_active the probe is 0
        // in this unit (or unset). High frag must not arm.
        if (!aura::ast::production_auto_arm_pack_active()) {
            CHECK(!aura::ast::should_production_auto_arm_moving(0.90),
                  "AC1: unset pack does not auto-arm");
        }
    }
    {
        AutoArmPrefGuard prod(1);
        MovingFlagGuard3123 off(0);
        aura::ast::g_last_moving_compact_ms.store(0, std::memory_order_release);
        CHECK(!aura::ast::should_production_auto_arm_moving(0.90),
              "AC1: Moving flag off does not auto-arm");
    }
    {
        AutoArmPrefGuard prod(1);
        aura::ast::g_last_moving_compact_ms.store(0, std::memory_order_release);
        ASTArena arena(64 * 1024);
        auto* p = arena.create<Pod16_3123>(1, 2, 3, 4);
        aura::core::lifetime::LifetimePin pin;
        pin.pin(p, arena.generation(), arena.arena_id());
        CHECK(!aura::ast::should_production_auto_arm_moving(0.90), "AC1: live pin blocks auto-arm");
    }
    CHECK(aura::core::lifetime::live_pin_count() == 0, "AC1: pin released");
}

static void ac3123_2_untracked_fail_closed_sticky() {
    std::println("\n--- #3123 AC2: production untracked kept fail-closed + sticky ---");
    MovingFlagGuard3123 on(1);
    const auto prev_hard = aura::ast::g_moving_untracked_hard_abort_pref.load();
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    mdh::reset_moving_densify_health_for_test();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16_3123>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16_3123>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16_3123>(9, 10, 11, 12);
    void* ext = p0;
    arena.register_external_root_for_densify(ext); // value-only → untracked under hard
    const auto r = arena.live_compact(aura::ast::LiveCompactMode::Moving);
    CHECK(r.moving_incomplete_remap || r.moving_blocked_precondition || !r.pin_contract_held ||
              r.untracked_kept_count > 0,
          "AC2: production hard untracked is fail-closed");
    if (r.moving_incomplete_remap || r.untracked_kept_count > 0 || r.moving_blocked_precondition) {
        CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(),
              "AC2: production hard untracked arms sticky");
        CHECK(!aura::ast::should_production_auto_arm_moving(0.90),
              "AC2: sticky-off blocks auto-arm (Moving flag reads 0)");
    }
    (void)p1;
    (void)p2;
    aura::ast::g_moving_untracked_hard_abort_pref.store(prev_hard, std::memory_order_relaxed);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
}

static void ac3123_3_sticky_clears_only_on_healthy() {
    std::println("\n--- #3123 AC3: sticky clears only after healthy Moving window ---");
    MovingFlagGuard3123 on(1);
    const auto prev_hard = aura::ast::g_moving_untracked_hard_abort_pref.load();
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    mdh::reset_moving_densify_health_for_test();

    // Incomplete window must leave a force-armed sticky set.
    aura::ast::g_moving_incomplete_remap_sticky_densify_off.store(1, std::memory_order_release);
    {
        ASTArena arena(64 * 1024);
        auto* p0 = arena.create<Pod16_3123>(1, 2, 3, 4);
        auto* p1 = arena.create<Pod16_3123>(5, 6, 7, 8);
        auto* p2 = arena.create<Pod16_3123>(9, 10, 11, 12);
        void* ext = p0;
        arena.register_external_root_for_densify(ext);
        const auto r = arena.live_compact(aura::ast::LiveCompactMode::Moving);
        if (r.moving_incomplete_remap || r.untracked_kept_count > 0 ||
            r.moving_blocked_precondition || !r.pin_contract_held) {
            CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(),
                  "AC3: incomplete window leaves sticky set");
        }
        (void)p1;
        (void)p2;
    }

    // Healthy Moving window (no untracked) clears sticky + records reason.
    aura::ast::g_moving_incomplete_remap_sticky_densify_off.store(1, std::memory_order_release);
    {
        ASTArena arena(64 * 1024);
        auto* p0 = arena.create<Pod16_3123>(1, 2, 3, 4);
        auto* p1 = arena.create<Pod16_3123>(5, 6, 7, 8);
        auto* p2 = arena.create<Pod16_3123>(9, 10, 11, 12);
        CHECK(p0 && p1 && p2, "AC3: create");
        const auto r = arena.live_compact(aura::ast::LiveCompactMode::Moving);
        CHECK(!r.moving_blocked_precondition, "AC3: healthy window not blocked");
        CHECK(r.untracked_kept_count == 0, "AC3: zero untracked");
        CHECK(!r.moving_incomplete_remap, "AC3: complete remap");
        CHECK(r.pin_contract_held, "AC3: pin held");
        CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(),
              "AC3: healthy window clears sticky");
        const auto reason = mdh::g_sticky_last_clear_reason.load();
        CHECK(reason == aura::ast::kStickyClearHealthyWindow ||
                  reason == aura::ast::kStickyClearZeroMoveClean,
              "AC3: last clear reason is healthy/zero-move");
    }
    aura::ast::g_moving_untracked_hard_abort_pref.store(prev_hard, std::memory_order_relaxed);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
}

static void ac3123_4_soft_force_unchanged() {
    std::println("\n--- #3123 AC4: Soft/Force stay non-moving ---");
    MovingFlagGuard3123 off(0);
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16_3123>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16_3123>(5, 6, 7, 8);
    void* a0 = p0;
    void* a1 = p1;
    const auto rs = arena.live_compact(aura::ast::LiveCompactMode::Soft);
    CHECK(!rs.moved_live_objects, "AC4: Soft non-moving");
    CHECK(rs.objects_moved == 0, "AC4: Soft objects_moved 0");
    const auto rf = arena.live_compact(aura::ast::LiveCompactMode::Force);
    CHECK(!rf.moved_live_objects, "AC4: Force non-moving");
    CHECK(p0 == a0 && p1 == a1, "AC4: addresses stable");
    const auto arena_src = read_file("src/core/arena.ixx");
    CHECK(arena_src.find("should_production_auto_arm_moving") != std::string::npos,
          "AC4: auto-arm helper wired");
    CHECK(arena_src.find("live_compact(LiveCompactMode::Moving)") != std::string::npos,
          "AC4: auto path requests Moving");
    CHECK(arena_src.find("Issue #3123") != std::string::npos, "AC4: arena cites #3123");
}

static void ac3123_5_agent_surface_and_linter() {
    std::println("\n--- #3123 AC5: Agent surface + linter + no invent file ---");
    mdh::reset_moving_densify_health_for_test();
    mdh::note_production_auto_arm();
    mdh::note_sticky_last_clear_reason(aura::ast::kStickyClearHealthyWindow);
    auto s = mdh::snapshot();
    CHECK(s.last_auto_arm_fired, "AC5: snapshot last_auto_arm_fired");
    CHECK(s.production_auto_arm_total >= 1, "AC5: snapshot auto-arm total");
    CHECK(s.sticky_last_clear_reason == aura::ast::kStickyClearHealthyWindow,
          "AC5: snapshot clear reason");

    CompilerService cs;
    CHECK(href(cs, "production-auto-arm-wired") == 1, "AC5: query wired");
    CHECK(href(cs, "schema-3123") == 3123, "AC5: schema-3123");
    CHECK(href(cs, "issue-3123") == 3123, "AC5: issue-3123");
    CHECK(href(cs, "production-auto-arm-total") >= 1, "AC5: query auto-arm total");
    CHECK(href(cs, "last-auto-arm-fired") == 1, "AC5: query last-auto-arm-fired");
    CHECK(href(cs, "sticky-last-clear-reason") == aura::ast::kStickyClearHealthyWindow,
          "AC5: query sticky-last-clear-reason");
    CHECK(href(cs, "schema-2619") == 2619, "AC5: legacy schema preserved");

    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto hh = read_file("src/core/moving_densify_health.hh");
    const auto t = read_file("tests/compiler/test_arena_moving_densify_health.cpp");
    const auto build = read_file("build.py");
    CHECK(q.find("production-auto-arm-total") != std::string::npos, "AC5: query key");
    CHECK(q.find("sticky-last-clear-reason") != std::string::npos, "AC5: clear-reason key");
    CHECK(hh.find("kProductionAutoArmMovingIssue = 3123") != std::string::npos,
          "AC5: health stamp");
    CHECK(t.find("ac3123_1_production_auto_arm_soft_never") != std::string::npos, "AC5: AC1 test");
    CHECK(build.find("check_production_auto_arm_moving_3123") != std::string::npos,
          "AC5: build.py linter");
    CHECK(read_file("tests/compiler/test_issue_3123.cpp").empty(), "AC5: no test_issue_3123.cpp");
    std::ifstream design("docs/design/3123-production-auto-arm.md");
    if (!design)
        design.open("../docs/design/3123-production-auto-arm.md");
    CHECK(!design.good(), "AC5: no docs/design/3123-* per #1655");
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
    ac3123_1_production_auto_arm_soft_never();
    ac3123_2_untracked_fail_closed_sticky();
    ac3123_3_sticky_clears_only_on_healthy();
    ac3123_4_soft_force_unchanged();
    ac3123_5_agent_surface_and_linter();
    std::println("\n=== #2619/#2682/#2775/#3123: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_arena_moving_densify_health();
}
#endif
