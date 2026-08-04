// @category: unit
// @reason: Issue #2557 — production default soft lock-order audit (metrics-only).
//
//   AC1: apply_production_lock_order_default(false) → soft; inversion bumps metrics
//   AC2: apply_production_lock_order_default(true) / sandbox=off → OFF (zero atomics)
//   AC3: Hard canary precedence (mode=3) still available via force
//   AC4: TLS depth still tracked under soft (nest safety; near-zero extra cost)
//   AC5: Source-cite + query:lock-order-audit-stats schema-2557 + intentional inversion

#include "test_harness.hpp"

#include "compiler/lock_order_audit.h"
#include "compiler/security_defaults.hh"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::lock_order::apply_production_lock_order_default;
using aura::compiler::lock_order::force_audit_mode_for_test;
using aura::compiler::lock_order::g_lock_inversion_detected_total;
using aura::compiler::lock_order::g_lock_order_acquire_total;
using aura::compiler::lock_order::g_lock_order_production_soft_default;
using aura::compiler::lock_order::g_lock_order_violation_total;
using aura::compiler::lock_order::is_held;
using aura::compiler::lock_order::Level;
using aura::compiler::lock_order::lock_order_audit_enabled;
using aura::compiler::lock_order::lock_order_canary_enabled;
using aura::compiler::lock_order::lock_order_mode;
using aura::compiler::lock_order::lock_order_production_soft_active;
using aura::compiler::lock_order::on_acquire;
using aura::compiler::lock_order::on_release;
using aura::compiler::lock_order::reset_tls_for_test;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:lock-order-audit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: production soft + intentional inversion metrics ──
static void ac1_production_soft_inversion() {
    std::println("\n--- #2557 AC1: production soft + inversion metrics ---");
    apply_production_lock_order_default(/*sandbox_off=*/false);
    reset_tls_for_test();
    CHECK(lock_order_mode() == 2, "AC1: mode soft (2)");
    CHECK(lock_order_audit_enabled(), "AC1: audit enabled");
    CHECK(!lock_order_canary_enabled(), "AC1: not canary");
    CHECK(lock_order_production_soft_active(), "AC1: production soft flag");
    CHECK(g_lock_order_production_soft_default.load() == 1, "AC1: production soft atomic");

    const auto inv0 = g_lock_inversion_detected_total.load();
    const auto vio0 = g_lock_order_violation_total.load();
    const auto acq0 = g_lock_order_acquire_total.load();

    // Intentional inversion: hold Workspace (rank 3), acquire Mutate (rank 1).
    CHECK(on_acquire(Level::Workspace), "AC1: Workspace acquire ok");
    const bool ok = on_acquire(Level::Mutate); // inversion
    CHECK(!ok, "AC1: inversion returns false");
    CHECK(g_lock_inversion_detected_total.load() > inv0, "AC1: inversion_detected bumped");
    CHECK(g_lock_order_violation_total.load() > vio0, "AC1: violation_total bumped");
    CHECK(g_lock_order_acquire_total.load() > acq0, "AC1: acquire_total bumped under soft");
    on_release(Level::Mutate);
    on_release(Level::Workspace);
    reset_tls_for_test();
}

// ── AC2: sandbox=off → OFF ──
static void ac2_sandbox_off() {
    std::println("\n--- #2557 AC2: sandbox=off → lock-order OFF ---");
    apply_production_lock_order_default(/*sandbox_off=*/true);
    reset_tls_for_test();
    CHECK(lock_order_mode() == 1, "AC2: mode off (1)");
    CHECK(!lock_order_audit_enabled(), "AC2: audit disabled");
    CHECK(!lock_order_production_soft_active(), "AC2: production soft inactive");

    const auto acq0 = g_lock_order_acquire_total.load();
    const auto inv0 = g_lock_inversion_detected_total.load();
    CHECK(on_acquire(Level::Workspace), "AC2: acquire ok when off");
    CHECK(is_held(Level::Workspace), "AC2: TLS depth still tracked");
    // Inversion while off: still updates depth, no metrics.
    (void)on_acquire(Level::Mutate);
    CHECK(g_lock_order_acquire_total.load() == acq0, "AC2: zero atomics (acquire_total)");
    CHECK(g_lock_inversion_detected_total.load() == inv0, "AC2: zero atomics (inversion)");
    on_release(Level::Mutate);
    on_release(Level::Workspace);
    reset_tls_for_test();
}

// ── AC3: hard canary precedence ──
static void ac3_canary_precedence() {
    std::println("\n--- #2557 AC3: hard canary still available ---");
    // force_audit_mode_for_test(3) simulates CANARY without aborting process
    // via environment (abort would kill the test). Soft path remains default
    // after re-apply production.
    force_audit_mode_for_test(3);
    CHECK(lock_order_mode() == 3, "AC3: mode hard (3)");
    CHECK(lock_order_canary_enabled(), "AC3: canary enabled");
    CHECK(lock_order_audit_enabled(), "AC3: audit enabled under canary");
    // Soft production re-apply restores soft (not canary) when env unset.
    apply_production_lock_order_default(/*sandbox_off=*/false);
    CHECK(lock_order_mode() == 2, "AC3: re-apply production → soft");
    CHECK(!lock_order_canary_enabled(), "AC3: canary cleared by production soft");
}

// ── AC4: nest depth under soft ──
static void ac4_depth_under_soft() {
    std::println("\n--- #2557 AC4: TLS depth under soft (nest safety) ---");
    apply_production_lock_order_default(/*sandbox_off=*/false);
    reset_tls_for_test();
    CHECK(on_acquire(Level::Mutate), "AC4: Mutate acquire");
    CHECK(is_held(Level::Mutate), "AC4: is_held after acquire");
    CHECK(on_acquire(Level::Workspace), "AC4: Workspace after Mutate ok");
    CHECK(is_held(Level::Workspace), "AC4: Workspace held");
    on_release(Level::Workspace);
    on_release(Level::Mutate);
    CHECK(!is_held(Level::Mutate), "AC4: depth cleared");
    reset_tls_for_test();
}

// ── AC5: source + query ──
static void ac5_source_schema() {
    std::println("\n--- #2557 AC5: source-cite + query schema ---");
    const auto lo = read_file("src/compiler/lock_order_audit.h");
    CHECK(lo.find("#2557") != std::string::npos, "AC5: lock_order_audit cites #2557");
    CHECK(lo.find("apply_production_lock_order_default") != std::string::npos,
          "AC5: apply_production_lock_order_default");
    CHECK(lo.find("g_lock_order_production_soft_default") != std::string::npos,
          "AC5: production soft flag");
    CHECK(lo.find("Mode  value") != std::string::npos || lo.find("soft  2") != std::string::npos ||
              lo.find("production Restricted") != std::string::npos,
          "AC5: mode table documented");

    const auto sec = read_file("src/compiler/security_defaults.hh");
    CHECK(sec.find("apply_production_lock_order_default") != std::string::npos,
          "AC5: security_defaults wires lock-order");
    CHECK(sec.find("#2557") != std::string::npos, "AC5: security_defaults cites #2557");

    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:lock-order-audit-stats") != std::string::npos, "AC5: query prim");
    CHECK(q.find("schema-2557") != std::string::npos, "AC5: schema-2557");
    CHECK(q.find("production-soft-active") != std::string::npos, "AC5: production-soft-active key");

    // Live query under production soft.
    apply_production_lock_order_default(/*sandbox_off=*/false);
    CompilerService cs;
    CHECK(href(cs, "schema-2557") == 2557, "AC5: live schema-2557");
    CHECK(href(cs, "mode") == 2, "AC5: live mode soft");
    CHECK(href(cs, "soft-active") == 1, "AC5: soft-active");
    CHECK(href(cs, "production-soft-active") == 1, "AC5: production-soft-active");
    CHECK(href(cs, "canary-active") == 0, "AC5: canary inactive");
    CHECK(href(cs, "inversion-detected-total") >= 0, "AC5: inversion total queryable");
}

} // namespace

int run_test_lock_order_production_soft() {
    std::println("=== Issue #2557: production soft lock-order audit ===");
    ac1_production_soft_inversion();
    ac2_sandbox_off();
    ac3_canary_precedence();
    ac4_depth_under_soft();
    ac5_source_schema();
    // Leave OFF for other tests sharing process if any.
    force_audit_mode_for_test(1);
    reset_tls_for_test();
    std::println("\n=== #2557: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_lock_order_production_soft();
}
#endif
