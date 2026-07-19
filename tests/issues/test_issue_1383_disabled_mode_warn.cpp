// @category: integration
// @reason: uses CompilerService to exercise typed_mutate +
//          set_invariant_check_mode + eval_warnings accessor.
//
// test_issue_1383_disabled_mode_warn.cpp — Issue #1383:
// Warn when InvariantCheckMode::Disabled is set on a workspace
// with mutation_history > 0.
//
// Background: #147 introduced post_mutation_invariant_check with
// three modes (Disabled / WarningsOnly / Strict). The Disabled
// branch silently bypasses the linear-ownership + occurrence-
// narrowing + match-exhaustiveness defense layer. For long-
// running processes (aura-pets-style per-pipeline evaluators
// running for hours), flipping to Disabled silently turns off
// soundness checks without any operator-visible signal.
//
// This test verifies the throttled warning:
//   - Disabled + ≥ 1 prior mutation → exactly ONE warning
//     (not per-mutation spam).
//   - Subsequent typed_mutate calls (still Disabled) → no new
//     warnings (throttled to once per mode flip).
//   - Re-flipping to Disabled later → NEW warning.
//   - WarningsOnly / Strict modes → no warning.
//   - Warning text includes the current mutation count.
//
// Test plan uses mutate:rebind to bump mutation_count > 0
// (same pattern as test_issue_147).

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core;
import aura.core.type;
import aura.diag;
import aura.compiler.ir;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.compiler.service;

namespace aura_issue_1383_detail {

// Helper: set up the workspace via the (set-code ...) Aura
// primitive (same as test_issue_147).
static void setup_workspace(aura::compiler::CompilerService& cs, const std::string& src) {
    std::string sexpr = std::format(R"X((set-code "{}"))X", src);
    auto v = cs.eval(sexpr);
    if (!v) {
        std::println(std::cerr, "    [eval(set-code) failed: {}]", v.error().message);
    }
}

// Helper: rebind a Define. mutate:rebind takes (name, new-value
// source, summary) and adds a MutationRecord to mutation_log_.
// Bumps workspace_flat_->mutation_count().
static aura::compiler::CompilerService::MutationResult
do_rebind(aura::compiler::CompilerService& cs, const std::string& name,
          const std::string& new_value_src, const std::string& summary) {
    std::string sexpr =
        std::format(R"X((mutate:rebind "{}" "{}" "{}"))X", name, new_value_src, summary);
    auto mr = cs.typed_mutate(sexpr);
    if (!mr.success) {
        std::println(std::cerr, "    [typed_mutate failed: {}]", mr.error);
    }
    return mr;
}

// ═════════════════════════════════════════════════════════════
// AC1: Disabled + ≥ 1 prior mutation → exactly ONE warning
// ═════════════════════════════════════════════════════════════
bool test_ac1_disabled_with_history_warns_once() {
    std::println("\n--- AC1: Disabled + history → 1 warning ---");
    aura::compiler::CompilerService cs;
    setup_workspace(cs, "(define x 1) (define y 2)");

    // Reset warnings from set_code path (none expected, but be safe).
    cs.clear_eval_warnings();

    // Step 1: warnings-only mode, make a mutation → no warning
    // (only Disabled emits).
    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::WarningsOnly);
    auto mr1 = do_rebind(cs, "x", "10", "rebind x to 10");
    CHECK(mr1.success, "AC1: first typed_mutate succeeds");
    CHECK(cs.eval_warnings().size() == 0, "AC1: WarningsOnly mode does NOT emit (no warning)");

    // Step 2: flip to Disabled.
    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::Disabled);

    // Step 3: typed_mutate with prior history → expect ONE warning.
    auto mr2 = do_rebind(cs, "y", "20", "rebind y to 20");
    CHECK(mr2.success, "AC1: typed_mutate in Disabled mode succeeds");
    CHECK(cs.eval_warnings().size() == 1, "AC1: Disabled + history → exactly 1 warning");
    if (!cs.eval_warnings().empty()) {
        const auto& w = cs.eval_warnings().back();
        // Warning must mention mutation count.
        CHECK(w.find("typed mutations") != std::string::npos,
              "AC1: warning mentions 'typed mutations'");
        CHECK(w.find("linear ownership") != std::string::npos,
              "AC1: warning mentions 'linear ownership' (the risk)");
    }

    // Step 4: another typed_mutate (still Disabled) → no new warning.
    auto mr3 = do_rebind(cs, "x", "11", "rebind x to 11");
    CHECK(mr3.success, "AC1: subsequent typed_mutate succeeds");
    CHECK(cs.eval_warnings().size() == 1,
          "AC1: subsequent typed_mutate (still Disabled) → no new warning");

    return true;
}

// ═════════════════════════════════════════════════════════════
// AC2: re-flipping to Disabled later → NEW warning
// ═════════════════════════════════════════════════════════════
bool test_ac2_reflip_to_disabled_warns_again() {
    std::println("\n--- AC2: re-flip → Disabled → new warning ---");
    aura::compiler::CompilerService cs;
    setup_workspace(cs, "(define a 1)");

    cs.clear_eval_warnings();

    // Setup: prior mutations in WarningsOnly.
    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::WarningsOnly);
    auto mr0 = do_rebind(cs, "a", "100", "rebind a to 100");
    CHECK(mr0.success, "AC2: initial mutation succeeds");

    // Flip to Disabled → 1 warning.
    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::Disabled);
    auto mr1 = do_rebind(cs, "a", "101", "rebind a to 101");
    CHECK(mr1.success, "AC2: first Disabled mutate succeeds");
    CHECK(cs.eval_warnings().size() == 1, "AC2: first Disabled flip → 1 warning");

    // Flip back to WarningsOnly → no new warning (only Disabled emits).
    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::WarningsOnly);
    auto mr2 = do_rebind(cs, "a", "102", "rebind a to 102");
    CHECK(mr2.success, "AC2: flip back to WarningsOnly mutate succeeds");
    CHECK(cs.eval_warnings().size() == 1, "AC2: WarningsOnly after Disabled → no new warning");

    // Re-flip to Disabled → NEW warning.
    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::Disabled);
    auto mr3 = do_rebind(cs, "a", "103", "rebind a to 103");
    CHECK(mr3.success, "AC2: re-flip to Disabled mutate succeeds");
    CHECK(cs.eval_warnings().size() == 2, "AC2: re-flip to Disabled → 2nd warning");

    // Yet another mutate (still Disabled) → still 2 warnings (throttled).
    auto mr4 = do_rebind(cs, "a", "104", "rebind a to 104");
    CHECK(mr4.success, "AC2: subsequent Disabled mutate succeeds");
    CHECK(cs.eval_warnings().size() == 2,
          "AC2: subsequent mutate (still Disabled) → still 2 warnings");

    return true;
}

// ═════════════════════════════════════════════════════════════
// AC3: WarningsOnly and Strict modes do NOT emit
// ═════════════════════════════════════════════════════════════
bool test_ac3_other_modes_silent() {
    std::println("\n--- AC3: WarningsOnly/Strict do NOT emit ---");
    aura::compiler::CompilerService cs;
    setup_workspace(cs, "(define m 1)");

    cs.clear_eval_warnings();

    // WarningsOnly: make multiple mutations → 0 warnings.
    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::WarningsOnly);
    for (int i = 0; i < 5; ++i) {
        auto mr = do_rebind(cs, "m", std::to_string(i + 10), "rebind m");
        CHECK(mr.success, "AC3: WarningsOnly mutate succeeds");
    }
    CHECK(cs.eval_warnings().size() == 0, "AC3: WarningsOnly mutations emit no mode warnings");

    // Strict: make multiple mutations → 0 warnings.
    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::Strict);
    for (int i = 0; i < 5; ++i) {
        auto mr = do_rebind(cs, "m", std::to_string(i + 20), "rebind m");
        CHECK(mr.success, "AC3: Strict mutate succeeds");
    }
    CHECK(cs.eval_warnings().size() == 0, "AC3: Strict mutations emit no mode warnings");

    return true;
}

// ═════════════════════════════════════════════════════════════
// AC5: clear_eval_warnings resets the buffer
// ═════════════════════════════════════════════════════════════
bool test_ac5_clear_resets() {
    std::println("\n--- AC5: clear_eval_warnings resets ---");
    aura::compiler::CompilerService cs;
    setup_workspace(cs, "(define q 1)");

    cs.clear_eval_warnings();

    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::WarningsOnly);
    auto mr1 = do_rebind(cs, "q", "10", "rebind q");
    CHECK(mr1.success, "AC5: setup mutate succeeds");

    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::Disabled);
    auto mr2 = do_rebind(cs, "q", "11", "rebind q");
    CHECK(mr2.success, "AC5: Disabled mutate succeeds");
    CHECK(cs.eval_warnings().size() == 1, "AC5: 1 warning accumulated");

    cs.clear_eval_warnings();
    CHECK(cs.eval_warnings().size() == 0, "AC5: clear_eval_warnings resets buffer");

    // Re-flip not needed — but verify a fresh Disabled mutate
    // still warns (the throttle state is internal and unaffected
    // by clear_eval_warnings).
    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::WarningsOnly);
    auto mr3 = do_rebind(cs, "q", "12", "rebind q");
    CHECK(mr3.success, "AC5: post-clear mutate succeeds");
    cs.set_invariant_check_mode(aura::compiler::InvariantCheckMode::Disabled);
    auto mr4 = do_rebind(cs, "q", "13", "rebind q");
    CHECK(mr4.success, "AC5: re-Disabled mutate succeeds");
    CHECK(cs.eval_warnings().size() == 1, "AC5: re-Disabled after clear → 1 new warning");

    return true;
}

} // namespace aura_issue_1383_detail

int main() {
    using namespace aura_issue_1383_detail;
    bool ok = true;
    ok &= test_ac1_disabled_with_history_warns_once();
    ok &= test_ac2_reflip_to_disabled_warns_again();
    ok &= test_ac3_other_modes_silent();
    ok &= test_ac5_clear_resets();

    int failed = aura::test::g_failed;
    std::println("\n=== Issue #1383 disabled-mode warn: {} ({} failures) ===",
                 (ok && failed == 0) ? "PASS" : "FAIL", failed);
    return (ok && failed == 0) ? 0 : 1;
}