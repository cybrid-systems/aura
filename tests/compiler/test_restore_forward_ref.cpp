// @category: unit
// @reason: Issue #5148 — mutate:restore of a forward-reference workspace
//          after a rebind must not abort on the lock-order canary, and
//          AURA_LOCK_ORDER_AUDIT=soft must stay soft across Evaluator ctor.
//
//   AC1: mode 2 + production-soft gauge survives Evaluator construction,
//        and an intentional inversion is counted without aborting
//   AC2: under the production hard canary, forward-ref → rebind → restore
//        returns #t, the restored source is the pre-rebind body, and
//        lock_inversion_detected_total does not grow
//   AC3: std/hot-strategy heal! survives the same shape

#include "test_harness.hpp"

#include "compiler/lock_order_audit.h"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
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
using aura::compiler::lock_order::g_lock_order_canary_enabled;
using aura::compiler::lock_order::g_lock_order_mode;
using aura::compiler::lock_order::g_lock_order_production_soft_default;
using aura::compiler::lock_order::Level;
using aura::compiler::lock_order::lock_order_canary_enabled;
using aura::compiler::lock_order::lock_order_mode;
using aura::compiler::lock_order::on_acquire;
using aura::compiler::lock_order::on_release;
using aura::compiler::lock_order::reset_tls_for_test;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;

bool eval_ok(CompilerService& cs, std::string_view expr, const char* label) {
    auto r = cs.eval(expr);
    if (!r.has_value()) {
        CHECK(false, std::format("{}: {}", label, r.error().message));
        return false;
    }
    CHECK(true, label);
    return true;
}

std::string workspace_source(CompilerService& cs) {
    auto r = cs.eval("(current-source :workspace :define-fn-sugar)");
    if (!r || !is_string(*r))
        return {};
    const auto idx = as_string_idx(*r);
    auto& heap = cs.evaluator().string_heap_mut();
    if (idx >= heap.size())
        return {};
    return heap[idx];
}

void ac1_soft_audit_survives_ctor() {
    std::println("\n--- #5148 AC1: explicit soft stays soft across Evaluator ---");
    // apply_production stores mode 2 and raises the production-soft gauge
    // when AUDIT=soft under a restricted profile. The ctor used to see that
    // gauge and rewrite mode to 3. Reproduce the post-apply atomics without
    // an env var (the test process may already have parsed a different one).
    g_lock_order_mode.store(2, std::memory_order_release);
    g_lock_order_canary_enabled.store(0, std::memory_order_release);
    g_lock_order_production_soft_default.store(1, std::memory_order_release);
    {
        CompilerService cs;
        CHECK(lock_order_mode() == 2, "AC1: ctor left mode soft (2)");
        CHECK(!lock_order_canary_enabled(), "AC1: ctor left the canary off");
        (void)cs;
    }
    if (lock_order_mode() != 2)
        return;
    reset_tls_for_test();
    const auto inv0 = g_lock_inversion_detected_total.load(std::memory_order_relaxed);
    CHECK(on_acquire(Level::Workspace), "AC1: hold Workspace");
    const bool nested = on_acquire(Level::Mutate);
    CHECK(!nested, "AC1: Mutate under Workspace is an inversion");
    CHECK(g_lock_inversion_detected_total.load(std::memory_order_relaxed) > inv0,
          "AC1: soft records the inversion");
    on_release(Level::Mutate);
    on_release(Level::Workspace);
    reset_tls_for_test();
    // The service ctor saw the production-soft gauge and armed the
    // process-wide audit latch. Later cases need a clean workspace.
    aura::compiler::typed_audit::reset_production_audit_defaults_for_test();
    g_lock_order_production_soft_default.store(0, std::memory_order_release);
    force_audit_mode_for_test(1);
}

void arm_canary() {
    apply_production_lock_order_default(/*sandbox_off=*/false);
    CHECK(lock_order_mode() == 3, "production canary is hard");
}

void arm_audit() {
    // The full IR restore path (the one that used to take Mutate under
    // Workspace) is selected only while production audit is active.
    aura::compiler::typed_audit::apply_production_audit_defaults();
    CHECK(aura::compiler::typed_audit::production_defaults_active(),
          "production audit latch is on (restore takes the full IR path)");
}

void ac2_forward_ref_restore() {
    std::println("\n--- #5148 AC2: forward ref rebind restore under hard canary ---");
    // --preset dev runs this restore with the hard canary and the production
    // audit latch. Arm both immediately before restore. Setup stays off so
    // set-code can install the workspace.
    force_audit_mode_for_test(1);
    aura::compiler::typed_audit::reset_production_audit_defaults_for_test();
    CompilerService cs;
    const char* src = "(define (f x) (g x))(define (g y) y)";
    if (!eval_ok(cs, std::format("(set-code \"{}\")", src), "AC2: set-code"))
        return;
    if (!eval_ok(cs, "(eval-current)", "AC2: eval-current"))
        return;
    auto snap = cs.eval("(mutate :snapshot \"before-candidate\")");
    const auto snap_id = snap && is_int(*snap) ? as_int(*snap) : -1;
    auto reason = cs.eval("(hash-ref (engine:metrics \"query:workspace-snapshot-stats\") "
                          "\"last-ast-snapshot-fail-reason\")");
    const auto reason_code = reason && is_int(*reason) ? as_int(*reason) : -99;
    const auto installed = workspace_source(cs);
    CHECK(snap_id >= 0, std::format("AC2: snapshot id {} reason {} source [{}]", snap_id,
                                    reason_code, installed));
    if (snap_id < 0)
        return;
    auto rebound = cs.eval("(mutate :rebind \"f\" \"(define (f x) (+ x 1))\" \"candidate\")");
    CHECK(rebound && is_bool(*rebound) && as_bool(*rebound), "AC2: rebind f");
    if (!rebound || !is_bool(*rebound) || !as_bool(*rebound))
        return;
    arm_canary();
    arm_audit();
    const auto inv0 = g_lock_inversion_detected_total.load(std::memory_order_relaxed);
    auto restored = cs.eval(std::format("(mutate :restore {})", snap_id));
    CHECK(restored && is_bool(*restored) && as_bool(*restored), "AC2: restore returns #t");
    CHECK(g_lock_inversion_detected_total.load(std::memory_order_relaxed) == inv0,
          "AC2: lock_inversion_detected_total did not grow");
    const auto source = workspace_source(cs);
    CHECK(source.find("(g x)") != std::string::npos,
          std::format("AC2: restored source calls g, got [{}]", source));
    CHECK(source.find("+ x 1") == std::string::npos,
          std::format("AC2: restored source dropped the candidate, got [{}]", source));
    CHECK(source.find("(g y)") != std::string::npos,
          std::format("AC2: restored source still has g, got [{}]", source));
}

void ac3_hot_strategy_heal() {
    std::println("\n--- #5148 AC3: hot-strategy heal! on a forward reference ---");
    force_audit_mode_for_test(1);
    aura::compiler::typed_audit::reset_production_audit_defaults_for_test();
    CompilerService cs;
    if (!eval_ok(cs, "(require \"std/hot-strategy\" all:)", "AC3: require hot-strategy"))
        return;
    const char* src = "(define (score x) (weight x))\\n(define (weight x) (* x 2))";
    if (!eval_ok(cs, std::format("(set-code \"{}\")", src), "AC3: set-code"))
        return;
    if (!eval_ok(cs, "(eval-current)", "AC3: eval-current"))
        return;
    if (!eval_ok(cs, "(hot-strategy:register! \"score\" \"(define (score x) (+ x 1))\")",
                 "AC3: register"))
        return;
    auto swapped =
        cs.eval("(hot-strategy:swap! \"score\" \"(define (score x) (- x 1))\" \"probe\")");
    CHECK(swapped.has_value(), "AC3: swap");
    if (!swapped)
        return;
    arm_canary();
    arm_audit();
    const auto inv0 = g_lock_inversion_detected_total.load(std::memory_order_relaxed);
    auto healed = cs.eval("(hot-strategy:heal!)");
    CHECK(healed && is_bool(*healed) && as_bool(*healed), "AC3: heal! returns #t");
    CHECK(g_lock_inversion_detected_total.load(std::memory_order_relaxed) == inv0,
          "AC3: heal! did not record an inversion");
    const auto source = workspace_source(cs);
    CHECK(source.find("(weight x)") != std::string::npos,
          std::format("AC3: heal restored the weight call, got [{}]", source));
    CHECK(source.find("- x 1") == std::string::npos,
          std::format("AC3: heal dropped the probe body, got [{}]", source));
}

} // namespace

int main() {
    // AC1 constructs an Evaluator while the production-soft gauge is set,
    // which arms process-wide audit state. Run the restore cases first.
    ac2_forward_ref_restore();
    ac3_hot_strategy_heal();
    ac1_soft_audit_survives_ctor();
    return RUN_ALL_TESTS();
}
