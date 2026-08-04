// @category: unit
// @reason: Issue #2354 — debug lock-order audit for workspace / closures /
// module / wait_map / scheduler / fiber_registry.
//
//   AC1: Audit off → zero atomics on acquire (TLS depth still tracked —
//        required for OrderedUniqueLock::acquire_if_needed nest safety).
//   AC1b: Audit off + nested acquire_if_needed does NOT deadlock
//         (regression: #2354 zero-TLS-depth bug → EDEADLK).
//   AC2: Soft audit + correct order → passes (no inversion).
//   AC3: Soft audit + reverse order → detected (hard canary aborts; soft
//        bumps counters — tested via soft + deliberate reverse).
//   AC4: Rank table documented; source-cite instrumented locks.
//   AC5: Self-test (this file) + coverage linter.

#include "test_harness.hpp"

#include "compiler/lock_order_audit.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <print>
#include <shared_mutex>
#include <string>
#include <string_view>

import std;

namespace {

using aura::compiler::lock_order::AuditedMutexLock;
using aura::compiler::lock_order::AuditScope;
using aura::compiler::lock_order::force_audit_mode_for_test;
using aura::compiler::lock_order::g_lock_inversion_detected_total;
using aura::compiler::lock_order::g_lock_order_acquire_total;
using aura::compiler::lock_order::g_lock_order_violation_total;
using aura::compiler::lock_order::is_held;
using aura::compiler::lock_order::Level;
using aura::compiler::lock_order::level_name;
using aura::compiler::lock_order::lock_order_audit_enabled;
using aura::compiler::lock_order::lock_order_mode;
using aura::compiler::lock_order::on_acquire;
using aura::compiler::lock_order::on_release;
using aura::compiler::lock_order::OrderedUniqueLock;
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

// ── AC1: audit off → no acquire counters (TLS depth still tracked) ──
static void ac1_audit_off_zero_cost() {
    std::println("\n--- AC1: audit off → zero atomics (depth tracked) ---");
    force_audit_mode_for_test(1); // off
    reset_tls_for_test();
    CHECK(!lock_order_audit_enabled(), "AC1: audit disabled");
    const auto a0 = g_lock_order_acquire_total.load();
    CHECK(on_acquire(Level::Workspace), "AC1: on_acquire returns true when off");
    CHECK(is_held(Level::Workspace), "AC1: TLS depth set when audit off (nest safety)");
    CHECK(on_acquire(Level::Orphan), "AC1: second acquire true when off");
    CHECK(g_lock_order_acquire_total.load() == a0, "AC1: acquire_total unchanged when audit off");
    on_release(Level::Workspace);
    on_release(Level::Orphan);
    CHECK(!is_held(Level::Workspace), "AC1: depth cleared on release");
    CHECK(g_lock_order_acquire_total.load() == a0, "AC1: still unchanged after release");
}

// ── AC1b: audit off + nested acquire_if_needed must not EDEADLK ──
static void ac1b_nested_acquire_if_needed_no_deadlock() {
    std::println("\n--- AC1b: audit off + nested acquire_if_needed ---");
    force_audit_mode_for_test(1); // production default OFF
    reset_tls_for_test();
    std::shared_mutex mtx;
    {
        OrderedUniqueLock<std::shared_mutex> outer(mtx, Level::Mutate);
        CHECK(outer.owns_lock(), "AC1b: outer owns mutate lock");
        CHECK(is_held(Level::Mutate), "AC1b: is_held(Mutate) under outer");
        // Nested path used by CompilerService invalidate — must skip re-lock.
        auto inner = OrderedUniqueLock<std::shared_mutex>::acquire_if_needed(mtx, Level::Mutate);
        CHECK(!inner.owns_lock(), "AC1b: inner inactive (already held)");
        CHECK(outer.owns_lock(), "AC1b: outer still owns after nested skip");
    }
    CHECK(!is_held(Level::Mutate), "AC1b: depth cleared after outer release");
    // Fresh acquire still works after nest.
    {
        auto again = OrderedUniqueLock<std::shared_mutex>::acquire_if_needed(mtx, Level::Mutate);
        CHECK(again.owns_lock(), "AC1b: re-acquire after release owns lock");
    }
    reset_tls_for_test();
}

// ── AC2: soft audit + correct order ──
static void ac2_correct_order() {
    std::println("\n--- AC2: soft audit + correct order ---");
    force_audit_mode_for_test(2);
    reset_tls_for_test();
    CHECK(lock_order_audit_enabled(), "AC2: audit enabled");
    const auto inv0 = g_lock_inversion_detected_total.load();
    // Canonical scheduler order: Orphan → WaitMap → Joiner → OwnedFibers
    CHECK(on_acquire(Level::Orphan), "AC2: Orphan ok");
    CHECK(on_acquire(Level::WaitMap), "AC2: WaitMap after Orphan ok");
    CHECK(on_acquire(Level::Joiner), "AC2: Joiner ok");
    CHECK(on_acquire(Level::OwnedFibers), "AC2: OwnedFibers ok");
    CHECK(on_acquire(Level::FiberRegistry), "AC2: FiberRegistry ok");
    CHECK(g_lock_inversion_detected_total.load() == inv0, "AC2: no inversion");
    on_release(Level::FiberRegistry);
    on_release(Level::OwnedFibers);
    on_release(Level::Joiner);
    on_release(Level::WaitMap);
    on_release(Level::Orphan);
    // Workspace then Closures then Module
    CHECK(on_acquire(Level::Workspace), "AC2: Workspace ok");
    CHECK(on_acquire(Level::Closures), "AC2: Closures after Workspace ok");
    CHECK(on_acquire(Level::Module), "AC2: Module ok");
    CHECK(g_lock_inversion_detected_total.load() == inv0, "AC2: still no inversion");
    on_release(Level::Module);
    on_release(Level::Closures);
    on_release(Level::Workspace);
    reset_tls_for_test();
    force_audit_mode_for_test(1);
}

// ── AC3: reverse order detected ──
static void ac3_reverse_order_detected() {
    std::println("\n--- AC3: reverse order → inversion metric ---");
    force_audit_mode_for_test(2); // soft (no abort)
    reset_tls_for_test();
    const auto inv0 = g_lock_inversion_detected_total.load();
    const auto viol0 = g_lock_order_violation_total.load();
    // Reverse scheduler order: OwnedFibers then WaitMap (lower while higher held)
    CHECK(on_acquire(Level::OwnedFibers), "AC3: OwnedFibers first");
    const bool ok = on_acquire(Level::WaitMap); // WaitMap < OwnedFibers → inversion
    CHECK(!ok, "AC3: reverse WaitMap after OwnedFibers → !ok");
    CHECK(g_lock_inversion_detected_total.load() > inv0, "AC3: inversion counter bumped");
    CHECK(g_lock_order_violation_total.load() > viol0, "AC3: violation counter bumped");
    on_release(Level::WaitMap);
    on_release(Level::OwnedFibers);
    // Mailbox while Closures held
    CHECK(on_acquire(Level::Closures), "AC3: Closures held");
    CHECK(!on_acquire(Level::Mailbox), "AC3: Mailbox under Closures → inversion");
    on_release(Level::Mailbox);
    on_release(Level::Closures);
    reset_tls_for_test();
    force_audit_mode_for_test(1);
}

// ── AC3b: AuditedMutexLock reverse order ──
static void ac3b_audited_mutex_reverse() {
    std::println("\n--- AC3b: AuditedMutexLock reverse order ---");
    force_audit_mode_for_test(2);
    reset_tls_for_test();
    std::mutex m1, m2;
    const auto inv0 = g_lock_inversion_detected_total.load();
    {
        AuditedMutexLock a(m1, Level::OwnedFibers);
        AuditedMutexLock b(m2, Level::WaitMap); // reverse
        CHECK(g_lock_inversion_detected_total.load() > inv0,
              "AC3b: AuditedMutexLock reverse detected");
    }
    reset_tls_for_test();
    force_audit_mode_for_test(1);
}

// ── AC4: rank table + source-cite instrumented sites ──
static void ac4_rank_table_and_source_cite() {
    std::println("\n--- AC4: rank table + source-cite ---");
    const auto h = read_file("src/compiler/lock_order_audit.h");
    CHECK(h.find("Issue #2354") != std::string::npos, "AC4: header cites #2354");
    CHECK(h.find("Orphan = 7") != std::string::npos, "AC4: Orphan = 7");
    CHECK(h.find("WaitMap = 8") != std::string::npos, "AC4: WaitMap = 8");
    CHECK(h.find("Joiner = 9") != std::string::npos, "AC4: Joiner = 9");
    CHECK(h.find("OwnedFibers = 10") != std::string::npos, "AC4: OwnedFibers = 10");
    CHECK(h.find("FiberRegistry = 11") != std::string::npos, "AC4: FiberRegistry = 11");
    CHECK(h.find("Closures = 12") != std::string::npos, "AC4: Closures = 12");
    CHECK(h.find("Module = 13") != std::string::npos, "AC4: Module = 13");
    CHECK(h.find("kCount = 14") != std::string::npos, "AC4: kCount = 14");
    CHECK(h.find("AURA_LOCK_ORDER_AUDIT") != std::string::npos, "AC4: AUDIT env");
    CHECK(h.find("AuditedMutexLock") != std::string::npos, "AC4: AuditedMutexLock");
    CHECK(h.find("AuditScope") != std::string::npos, "AC4: AuditScope");
    CHECK(std::string_view(level_name(Level::Orphan)) == "Orphan", "AC4: level_name Orphan");
    CHECK(std::string_view(level_name(Level::WaitMap)) == "WaitMap", "AC4: level_name WaitMap");

    const auto sch = read_file("src/serve/scheduler.cpp");
    CHECK(sch.find("Level::Orphan") != std::string::npos, "AC4: scheduler Orphan wire");
    CHECK(sch.find("Level::WaitMap") != std::string::npos, "AC4: scheduler WaitMap wire");
    CHECK(sch.find("Level::Joiner") != std::string::npos, "AC4: scheduler Joiner wire");
    CHECK(sch.find("Level::OwnedFibers") != std::string::npos, "AC4: scheduler OwnedFibers wire");
    CHECK(sch.find("AuditedMutexLock") != std::string::npos, "AC4: scheduler AuditedMutexLock");
    CHECK(sch.find("lock_order_audit.h") != std::string::npos, "AC4: scheduler includes audit");

    const auto wk = read_file("src/serve/worker.cpp");
    CHECK(wk.find("Level::FiberRegistry") != std::string::npos, "AC4: worker FiberRegistry");
    CHECK(wk.find("lock_order_audit.h") != std::string::npos, "AC4: worker includes audit");

    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("Level::Workspace") != std::string::npos, "AC4: Phase5 Workspace wired");

    const auto gc = read_file("src/compiler/evaluator_gc.cpp");
    CHECK(gc.find("Level::Closures") != std::string::npos, "AC4: closures instrumented");

    const auto ml = read_file("src/compiler/evaluator_module_loader.cpp");
    CHECK(ml.find("Level::Module") != std::string::npos, "AC4: module instrumented");
}

// ── AC5: env gate docs ──
static void ac5_env_and_mode() {
    std::println("\n--- AC5: mode helpers ---");
    force_audit_mode_for_test(1);
    CHECK(lock_order_mode() == 1, "AC5: mode off == 1");
    force_audit_mode_for_test(2);
    CHECK(lock_order_mode() == 2, "AC5: mode soft == 2");
    force_audit_mode_for_test(3);
    CHECK(lock_order_mode() == 3, "AC5: mode hard == 3");
    force_audit_mode_for_test(1);
    {
        AuditScope scope(Level::Workspace); // compiles + zero-cost when off
        (void)scope;
    }
    CHECK(true, "AC5: AuditScope RAII compiles under audit off");
}

} // namespace

int run_test_lock_order_audit_hard() {
    std::println("=== Issue #2354: debug lock-order audit (scheduler/workspace/closures) ===");
    ac4_rank_table_and_source_cite();
    ac1_audit_off_zero_cost();
    ac1b_nested_acquire_if_needed_no_deadlock();
    ac2_correct_order();
    ac3_reverse_order_detected();
    ac3b_audited_mutex_reverse();
    ac5_env_and_mode();
    force_audit_mode_for_test(1);
    reset_tls_for_test();
    std::println("\n=== #2354: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_lock_order_audit_hard();
}
#endif
