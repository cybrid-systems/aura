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

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;
namespace mdh = aura::core::moving_densify_health;

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

} // namespace

int run_test_arena_moving_densify_health() {
    std::println("=== Issue #2619: Agent Moving densify health ===");
    ac1_query_exposes_window();
    ac2_incomplete_denies_mutate();
    ac3_soft_observe_only();
    ac4_no_densify_healthy();
    ac5_source_cite();
    std::println("\n=== #2619: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_arena_moving_densify_health();
}
#endif
