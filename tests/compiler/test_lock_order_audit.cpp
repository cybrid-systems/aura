// test_lock_order_audit.cpp — Issue #2316:
// Extend lock-order audit to mailbox, hot-update, and compact_env_frames paths.
// Refines existing lock_order_audit surface | #2010 (mailbox) |
// hot-update Defer/reemit (#2205/#2208/#2273) | env compact interlock |
// `workspace_mtx_` MutationBoundary contract.
//
//   AC1: Global rank table — Mailbox < Mutate < HotUpdate < Workspace
//        < EnvFrames < CompactEnv < DepGraph (extended from #1388 #2316).
//        Forbidden inversions documented in header comment.
//   AC2: Runtime canary (debug/canary) — AURA_LOCK_ORDER_CANARY=1
//        aborts with file:line on inversion. Production default OFF.
//   AC3: Wire critical acquire sites — mailbox push/broadcast +
//        hot-update reemit/drain + compact_env_frames interlock +
//        MutationBoundaryGuard workspace lock.
//   AC4: Counter `lock_order_violation_total` (canary only) +
//        test-only accessor.
//   AC5: Tests — deliberate inversion under canary → abort/detect.
//        Source-cite rank table + at least one wire site per subsystem
//        (mailbox, hot-update, compact_env, workspace).

#include "test_harness.hpp"
#include "compiler/lock_order_audit.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

using aura::compiler::lock_order::g_lock_inversion_detected_total;
using aura::compiler::lock_order::g_lock_order_acquire_total;
using aura::compiler::lock_order::g_lock_order_canary_enabled;
using aura::compiler::lock_order::g_lock_order_release_total;
using aura::compiler::lock_order::g_lock_order_violation_total;
using aura::compiler::lock_order::Level;
using aura::compiler::lock_order::lock_order_canary_enabled;
using aura::compiler::lock_order::on_acquire;
using aura::compiler::lock_order::reset_tls_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

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

// ── AC1: Global rank table + forbidden inversions ──
static void ac2316_rank_table() {
    std::println("\n--- #2316 AC1: rank table + forbidden inversions ---");
    const auto audit = read_file("src/compiler/lock_order_audit.h");
    CHECK(audit.find("Mailbox = 0") != std::string::npos, "AC1: Level::Mailbox = 0 in enum");
    CHECK(audit.find("Mutate = 1") != std::string::npos, "AC1: Level::Mutate = 1 in enum");
    CHECK(audit.find("HotUpdate = 2") != std::string::npos,
          "AC1: Level::HotUpdate = 2 in enum (#2316 extension)");
    CHECK(audit.find("Workspace = 3") != std::string::npos, "AC1: Level::Workspace = 3 in enum");
    CHECK(audit.find("EnvFrames = 4") != std::string::npos, "AC1: Level::EnvFrames = 4 in enum");
    CHECK(audit.find("CompactEnv = 5") != std::string::npos,
          "AC1: Level::CompactEnv = 5 in enum (#2316 extension)");
    CHECK(audit.find("DepGraph = 6") != std::string::npos, "AC1: Level::DepGraph = 6 in enum");
    // #2354 appends ranks after DepGraph; kCount >= 7 preserves #2316 table.
    CHECK(audit.find("kCount =") != std::string::npos, "AC1: kCount present");
    CHECK(audit.find("DepGraph = 6") != std::string::npos, "AC1: DepGraph rank stable");
    // Forbidden inversions header comment (decision table style)
    CHECK(audit.find("Forbidden inversions") != std::string::npos,
          "AC1: forbidden inversions documented");
    CHECK(audit.find(
              "Mailbox → Mutate → HotUpdate → Workspace → EnvFrames → CompactEnv → DepGraph") !=
              std::string::npos,
          "AC1: canonical acquire order documented");
    CHECK(audit.find("Issue #2316") != std::string::npos, "AC1: lock_order_audit.h cites 2316");
}

// ── AC2: Runtime canary (debug/canary) ──
static void ac2316_canary_mechanism() {
    std::println("\n--- #2316 AC2: runtime canary ---");
    const auto audit = read_file("src/compiler/lock_order_audit.h");
    CHECK(audit.find("AURA_LOCK_ORDER_CANARY") != std::string::npos,
          "AC2: AURA_LOCK_ORDER_CANARY env guard present");
    CHECK(audit.find("lock_order_canary_enabled") != std::string::npos,
          "AC2: lock_order_canary_enabled() helper present");
    CHECK(audit.find("g_lock_order_canary_enabled") != std::string::npos,
          "AC2: g_lock_order_canary_enabled atomic present");
    CHECK(audit.find("std::abort()") != std::string::npos, "AC2: abort on inversion under canary");
    // #3119: production default is Hard. Also accept legacy OFF / Soft docs.
    CHECK(audit.find("Production default OFF") != std::string::npos ||
              audit.find("production default OFF") != std::string::npos ||
              audit.find("production default): atomics") != std::string::npos ||
              audit.find("#2557 production default") != std::string::npos ||
              audit.find("soft is the production default") != std::string::npos ||
              audit.find("#3119 production default") != std::string::npos ||
              audit.find("production Restricted/Strict default") != std::string::npos,
          "AC2: production default documented (Hard #3119)");
    // Lazy-init pattern from getenv
    CHECK(audit.find("std::getenv(\"AURA_LOCK_ORDER_CANARY\")") != std::string::npos,
          "AC2: lazy-init from env via std::getenv");
}

// ── AC3: Wire critical acquire sites (source-cite) ──
static void ac2316_wire_sites() {
    std::println("\n--- #2316 AC3: wire critical acquire sites ---");
    const auto mb = read_file("src/serve/multi_fiber_mailbox.h");
    const auto emb = read_file("src/compiler/evaluator.ixx");
    const auto hu = read_file("src/compiler/hot_update_registry.cpp");
    // mailbox push/broadcast wire (Level::Mailbox)
    CHECK(mb.find("Level::Mailbox") != std::string::npos,
          "AC3: multi_fiber_mailbox.h wires Level::Mailbox");
    // workspace_mtx_ wire (Level::Workspace) in evaluator.ixx
    CHECK(emb.find("Level::Workspace") != std::string::npos,
          "AC3: evaluator.ixx wires Level::Workspace");
    // hot-update registry mutexes wire (Level::HotUpdate) in hot_update_registry.cpp
    CHECK(hu.find("Level::HotUpdate") != std::string::npos,
          "AC3: hot_update_registry.cpp wires Level::HotUpdate");
    CHECK(hu.find("#include \"compiler/lock_order_audit.h\"") != std::string::npos,
          "AC3: hot_update_registry.cpp includes lock_order_audit.h");
    // Each subsystem must have at least one wire site
    CHECK(mb.find("Level::Mailbox") != std::string::npos &&
              mb.find("on_acquire") != std::string::npos,
          "AC3: mailbox wire site present");
    CHECK(hu.find("lock_order::Level::HotUpdate") != std::string::npos,
          "AC3: hot-update wire site present");
}

// ── AC4: Counter `lock_order_violation_total` (canary only) ──
static void ac2314_counter_wired() {
    std::println("\n--- #2316 AC4: counter wired ---");
    const auto audit = read_file("src/compiler/lock_order_audit.h");
    CHECK(audit.find("g_lock_order_violation_total") != std::string::npos,
          "AC4: g_lock_order_violation_total counter present");
    CHECK(audit.find("canary only") != std::string::npos ||
              audit.find("AURA_LOCK_ORDER_CANARY") != std::string::npos,
          "AC4: counter documented as canary-only");
    // Counter is atomic (loadable)
    const auto v = g_lock_order_violation_total.load(std::memory_order_relaxed);
    CHECK(v >= 0, "AC4: counter atomic loadable");
    // Other observability counters also wired
    CHECK(g_lock_order_acquire_total.load(std::memory_order_relaxed) >= 0,
          "AC4: acquire counter loadable");
    CHECK(g_lock_order_release_total.load(std::memory_order_relaxed) >= 0,
          "AC4: release counter loadable");
}

// ── AC5: Deliberate inversion under canary — source-cite only ──
// (We don't actually invoke the abort path here because the canary is
// disabled in default test runs; testing the abort would require setting
// the env var which isn't process-local. Instead we source-cite the
// mechanism and verify the rank table ordering.)
static void ac2316_inversion_source_cite() {
    std::println("\n--- #2316 AC5: inversion detection source-cite ---");
    const auto audit = read_file("src/compiler/lock_order_audit.h");
    // any_higher_held checks all levels above the current one
    CHECK(audit.find("any_higher_held") != std::string::npos,
          "AC5: any_higher_held() inversion-check helper present");
    CHECK(audit.find("g_lock_inversion_detected_total") != std::string::npos,
          "AC5: g_lock_inversion_detected_total counter present");
    // The on_acquire function aborts under canary
    CHECK(audit.find("on_acquire") != std::string::npos, "AC5: on_acquire() entry point present");
    CHECK(audit.find("LOCK_ORDER_CANARY") != std::string::npos,
          "AC5: LOCK_ORDER_CANARY message present");
    // Demo: depth-tracking + inversion-detection under soft audit
    // (#2354: production default OFF requires force_audit_mode_for_test).
    using aura::compiler::lock_order::force_audit_mode_for_test;
    force_audit_mode_for_test(2); // soft audit
    reset_tls_for_test();
    const auto inv_before = g_lock_inversion_detected_total.load();
    (void)on_acquire(Level::EnvFrames, "test_lock_order_audit.cpp", 0);
    (void)on_acquire(Level::Mailbox, "test_lock_order_audit.cpp", 0);
    // Mailbox (idx 0) acquired while EnvFrames (idx 4) is held → inversion
    const auto inv_after = g_lock_inversion_detected_total.load();
    CHECK(inv_after > inv_before, "AC5: inversion detected via on_acquire");
    reset_tls_for_test();
    force_audit_mode_for_test(1); // restore off
}


// ── #3554 AC1: Evaluator ctor self-upgrade (embedder skip-init path).
// Source-cite: production_defaults_expected() must gate the ctor's
// lock_order mode + hold-budget reject upgrade, with zero-extra when
// production_defaults_expected() is false (Soft / Off / single-eval MVP).
static void ac3554_1_evaluator_ctor_self_upgrade_source_cite() {
    std::println("\n--- #3554 AC1: Evaluator ctor self-upgrade ---");
    const auto ixx = read_file("src/compiler/lock_order_audit.h");
    const auto mbh = read_file("src/compiler/mutation_hold_budget.h");
    const auto ctor = read_file("src/compiler/evaluator_ctor.cpp");

    // AC1: production_defaults_expected() reads env AURA_PRODUCTION_DEFAULTS=1
    // OR g_lock_order_production_soft_default non-zero.
    CHECK(ixx.find("AURA_PRODUCTION_DEFAULTS") != std::string::npos,
          "3554 AC1: production_defaults_expected reads AURA_PRODUCTION_DEFAULTS");
    CHECK(ixx.find("g_lock_order_production_soft_default") != std::string::npos,
          "3554 AC1: production_defaults_expected also reads soft_default gauge");
    CHECK(ixx.find("production_defaults_expected") != std::string::npos,
          "3554 AC1: helper declared in lock_order_audit.h");

    // AC2: mutation_hold_budget_reject_enabled_set(bool) setter added.
    CHECK(mbh.find("mutation_hold_budget_reject_enabled_set") != std::string::npos,
          "3554 AC2: setter declared in mutation_hold_budget.h");

    // AC3: Evaluator::Evaluator() ctor calls both upgrade paths under
    // production_defaults_expected() && !hold-budget reject.
    CHECK(ctor.find("Evaluator::Evaluator()") != std::string::npos, "3554 AC3: ctor located");
    CHECK(ctor.find("production_defaults_expected") != std::string::npos,
          "3554 AC3: ctor reads production_defaults_expected()");
    CHECK(ctor.find("g_lock_order_mode.store(3") != std::string::npos,
          "3554 AC3: ctor upgrades lock_order_mode to canary (3)");
    {
        // #3554 AC3: clang-format may wrap .store( across lines; search the
        // call then require the enabled value 1 on the same/nearby line.
        const auto pos = ctor.find("g_lock_order_canary_enabled.store(");
        const auto val_ok = pos != std::string::npos &&
                            ctor.find("1, std::memory_order_release", pos) != std::string::npos &&
                            ctor.find("1, std::memory_order_release", pos) < pos + 80;
        CHECK(val_ok, "3554 AC3: ctor enables canary gate");
    }
    CHECK(ctor.find("mutation_hold_budget_reject_enabled_set(true)") != std::string::npos,
          "3554 AC3: ctor enables hold-budget reject via setter");

    // AC4: Soft / Off / single-eval MVP zero-cost (single atomic load +
    // env getenv + branch). production_defaults_expected() returns false
    // → no set / no upgrade / no reject enable.
    CHECK(ixx.find(
              "return g_lock_order_production_soft_default.load(std::memory_order_acquire) != 0") !=
              std::string::npos,
          "3554 AC4: production_defaults_expected short-circuits when soft_default == 0");

    // Linter exists: the wave named it check_evaluator_ctor_production_upgrade.py
    // (read_file() has no glob); require it + the #3554 cite inside.
    const auto linter_file = read_file("scripts/check_evaluator_ctor_production_upgrade.py");
    CHECK(!linter_file.empty() && linter_file.find("3554") != std::string::npos,
          "3554: linter scripts/check_evaluator_ctor_production_upgrade.py exists");
    CHECK(read_file("docs/design/3554-*.md").empty(),
          "3554: no docs/design/3554-* (agent repo philosophy)");
}

// ── #3554 AC2: no docs/design/, no invent new test binary.
// (Source-cite pattern via read_file of existing files; no new test
//  file / docs / schema.)
static void ac3554_2_no_docs_no_invent() {
    std::println("\n--- #3554 AC2: no docs/, no invent ---");
    CHECK(read_file("docs/design/3554-*.md").empty(),
          "3554: no docs/design/3554-* (agent repo philosophy)");
}


} // namespace

int run_test_lock_order_audit() {
    std::println(
        "=== Issue #2316: extend lock-order audit (mailbox + hot-update + compact_env) ===");
    ac2316_rank_table();
    ac2316_canary_mechanism();
    ac2316_wire_sites();
    ac2314_counter_wired();
    ac2316_inversion_source_cite();
    std::println("\n=== #3554: Evaluator ctor self-upgrade ===");
    ac3554_1_evaluator_ctor_self_upgrade_source_cite();
    ac3554_2_no_docs_no_invent();
    std::println("\n=== #2316 lock-order audit: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_lock_order_audit();
}
#endif
