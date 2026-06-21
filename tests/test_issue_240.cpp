// @category: integration
// @reason: uses CompilerService to eval Aura source; tests #240 primitives + hook wiring

// test_issue_240.cpp — Issue #240: per-node occurrence-dirty bit
// (fine-grained scoping for find_occurrence_contexts).
//
// Pre-#240: post_mutation_invariant_check flagged every if-context
// in the dirty scope as "StaleOccurrenceRefinement" — too
// noisy under WarningsOnly mode (any mutation that touched a
// function's body fired notes for every nested if, even if the
// mutation didn't touch the predicate).
//
// #240: gives mutators a way to tag specific nodes with the
// `DirtyReason::kOccurrenceDirty` bit (#188 infrastructure). The
// post-mutation invariant check then:
//
//   - skips nodes that are NOT dirty at all (no false positives)
//   - emits a "conservative" note for nodes with only the general
//     dirty bit (backward-compatible with pre-#240 callers)
//   - emits a precise note for nodes explicitly tagged with
//     `kOccurrenceDirty` (the new scope-limited path)
//
// The bit is exposed via two new Aura primitives and one C++ hook:
//   - (compile:mark-narrowing-dirty! node-id [#t|#f]) — set/clear
//   - (compile:narrowing-dirty? node-id)                — peek
//   - Evaluator::set_set_occurrence_dirty_fn            — C++ hook
//     (wired by CompilerService to read/write the
//      kOccurrenceDirty bit on workspace_flat_.)
//
// MVP scope (this test, scope-limited close):
//   AC1: compile:mark-narrowing-dirty! sets the bit (peek → #t)
//   AC2: compile:mark-narrowing-dirty! with #f clears the bit
//   AC3: round-trip: clean → set → query → clear → query (clean)
//   AC4: out-of-range node-id returns #f (graceful)
//   AC5: empty args / non-int args return #f (graceful)
//   AC6: per-node scoping — when a node has kOccurrenceDirty set,
//        find_occurrence_contexts emits the precise note (not the
//        conservative fallback). Verified end-to-end via
//        post_mutation_invariant_check.
//   AC7: mark-narrowing-dirty! is idempotent (bit stays set on
//        repeated calls without affecting other dirty bits)
//
// Out of scope (deferred follow-ups):
//   - Blame tracking in structural mutation primitives
//   - Per-mutation exhaustiveness re-eval (#227 follow-up #3)
//   - Wire mark_narrowing_dirty into mutate:rebind / mutate:replace-value
//     automatically when the mutation target is an if-predicate.
//
// Implementation refs:
//   - src/core/ast.ixx:1720           — kOccurrenceDirty = 0x04
//   - src/core/ast.ixx:1728           — mark_dirty(id, uint8_t reasons)
//   - src/core/ast.ixx:1752           — is_dirty_for(id, uint8_t reason_mask)
//   - src/core/ast.ixx:1766           — clear_dirty_for(id, uint8_t reason_mask)
//   - src/compiler/evaluator.ixx:651  — SetOccurrenceDirtyFn typedef
//   - src/compiler/service.ixx:478    — hook wiring
//   - src/compiler/evaluator_primitives_compile.cpp — compile:mark-narrowing-dirty! primitive
//   - src/compiler/evaluator_primitives_compile.cpp — compile:narrowing-dirty? primitive
//   - src/compiler/type_checker_impl.cpp:3383 — find_occurrence_contexts
//     per-node kOccurrenceDirty filter

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <print>
#include <string>
#include <vector>

// Unified test harness (Issue #226).
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.ir_executor;
import aura.compiler.evaluator;
import aura.compiler.type_checker;
import aura.compiler.service;

// Helper: run a snippet and return the raw EvalValue (handles
// errors like test_issue_188's run_on). Use this for queries
// where we don't care about the typed return value.
namespace aura_issue_240_detail {
static aura::compiler::types::EvalValue run_on(
        aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error in: {}]", std::string(src).substr(0, 80));
        return aura::compiler::types::make_void();
    }
    return *r;
}

// Helper: load code into the workspace via (set-code ...) — the
// Aura primitive that ALSO sets workspace_flat_ on the evaluator.
// The C++ cs.set_code() helper parses into current_ast_/current_pool_
// but does NOT set workspace_flat_; only the Aura primitive does.
static bool load_workspace(aura::compiler::CompilerService& cs,
                            const std::string& code) {
    std::string cmd = std::string("(set-code \"") + code + "\")";
    auto r = cs.eval(cmd);
    return r.has_value();
}

// ── AC1+AC2: compile:mark-narrowing-dirty! sets / clears the bit ──
//
// The primitive is the public surface for tagging workspace nodes
// with kOccurrenceDirty. The Aura layer is the right place to
// call this from — a mutation primitive that mutates an
// if-predicate can mark the predicate's parent so the
// post-mutation invariant check scopes its diagnostic precisely.

bool test_mark_sets_bit() {
    std::println("\n--- AC1+AC2: compile:mark-narrowing-dirty! sets / clears the bit ---");
    aura::compiler::CompilerService cs;
    // (set-code ...) sets workspace_flat_ so the hook can find it.
    CHECK(load_workspace(cs,
        "(define (f x) "
        "  (if (number? x) (+ x 1) (* x 2)))"),
        "load_workspace succeeds");

    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr, "workspace_flat() non-null after set-code");
    if (!ws) return true;
    CHECK(ws->size() > 0, "workspace has nodes");
    if (ws->size() == 0) return true;
    aura::ast::NodeId target = 1;
    if (target >= ws->size()) target = static_cast<aura::ast::NodeId>(ws->size() - 1);

    // Capture prior state — should be 0 (workspace was just
    // type-checked and dirty bits cleared).
    auto prior_before = ws->dirty_reasons(target);
    CHECK(prior_before == 0, "target node starts clean");

    // Set the bit via the Aura primitive.
    // The primitive returns the PRIOR state (hook contract).
    // First mark on a clean node → prior was false → returns #f.
    auto set_r = cs.eval(
        std::string("(compile:mark-narrowing-dirty! ") +
        std::to_string(target) + ")");
    CHECK(set_r.has_value(), "mark-narrowing-dirty! returns a value");
    if (set_r) {
        CHECK(!aura::compiler::types::as_bool(*set_r),
              "mark-narrowing-dirty! returns #f (prior was clean)");
    }

    // Verify the bit is set on the FlatAST.
    auto reasons_after_set = ws->dirty_reasons(target);
    const auto kOccBit = static_cast<std::uint8_t>(
        aura::ast::FlatAST::DirtyReason::kOccurrenceDirty);
    CHECK((reasons_after_set & kOccBit) != 0,
          "kOccurrenceDirty bit is set after mark-narrowing-dirty!");

    // Peek via the query primitive — should be #t.
    auto peek_r = cs.eval(
        std::string("(compile:narrowing-dirty? ") +
        std::to_string(target) + ")");
    CHECK(peek_r.has_value(), "narrowing-dirty? returns a value");
    if (peek_r) {
        CHECK(aura::compiler::types::as_bool(*peek_r),
              "narrowing-dirty? returns #t after mark");
    }

    // Re-mark (bit already set). Returns #t (prior was true).
    auto set_again = cs.eval(
        std::string("(compile:mark-narrowing-dirty! ") +
        std::to_string(target) + ")");
    CHECK(set_again.has_value(), "re-mark returns");
    if (set_again) {
        CHECK(aura::compiler::types::as_bool(*set_again),
              "re-mark returns #t (prior was already set)");
    }

    // Clear the bit.
    // Clear on already-set bit → prior was true → returns #t.
    auto clear_r = cs.eval(
        std::string("(compile:mark-narrowing-dirty! ") +
        std::to_string(target) + " #f)");
    CHECK(clear_r.has_value(), "mark-narrowing-dirty! with #f returns");
    if (clear_r) {
        CHECK(aura::compiler::types::as_bool(*clear_r),
              "clear on set bit returns #t (prior was set)");
    }

    auto reasons_after_clear = ws->dirty_reasons(target);
    CHECK((reasons_after_clear & kOccBit) == 0,
          "kOccurrenceDirty bit cleared after mark with #f");

    // Peek again — should be #f.
    auto peek_r2 = cs.eval(
        std::string("(compile:narrowing-dirty? ") +
        std::to_string(target) + ")");
    CHECK(peek_r2.has_value(), "narrowing-dirty? post-clear returns");
    if (peek_r2) {
        CHECK(!aura::compiler::types::as_bool(*peek_r2),
              "narrowing-dirty? returns #f after clear");
    }

    // Clear again on clean bit → prior was false → returns #f.
    auto clear_again = cs.eval(
        std::string("(compile:mark-narrowing-dirty! ") +
        std::to_string(target) + " #f)");
    CHECK(clear_again.has_value(), "clear-on-clean returns");
    if (clear_again) {
        CHECK(!aura::compiler::types::as_bool(*clear_again),
              "clear on clean bit returns #f (prior was clean)");
    }

    return true;
}

// ── AC3: Round-trip on a multi-bit set/clear ────────────────
//
// Ensure the kOccurrenceDirty bit doesn't clobber other dirty
// bits (kGeneralDirty, etc.). Mark the node with multiple bits
// via FlatAST::mark_dirty(), then clear just the occurrence
// bit, and verify the others remain.

bool test_mark_preserves_other_bits() {
    std::println("\n--- AC3: kOccurrenceDirty bit doesn't clobber other bits ---");
    aura::compiler::CompilerService cs;
    CHECK(load_workspace(cs, "(define g 42)"),
          "load_workspace succeeds");

    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr, "workspace_flat() non-null");
    if (!ws) return true;
    aura::ast::NodeId target = 1;
    if (target >= ws->size()) target = static_cast<aura::ast::NodeId>(ws->size() - 1);

    // Mark with both general + occurrence bits via direct API.
    const auto kGeneral = static_cast<std::uint8_t>(
        aura::ast::FlatAST::DirtyReason::kGeneralDirty);
    const auto kOccBit = static_cast<std::uint8_t>(
        aura::ast::FlatAST::DirtyReason::kOccurrenceDirty);
    ws->mark_dirty(target, kGeneral | kOccBit);

    auto reasons_combined = ws->dirty_reasons(target);
    CHECK((reasons_combined & kGeneral) != 0, "kGeneralDirty set");
    CHECK((reasons_combined & kOccBit) != 0, "kOccurrenceDirty set");

    // Now clear just the occurrence bit via the Aura primitive.
    auto clear_r = cs.eval(
        std::string("(compile:mark-narrowing-dirty! ") +
        std::to_string(target) + " #f)");
    CHECK(clear_r.has_value(), "clear returns");

    auto reasons_after = ws->dirty_reasons(target);
    CHECK((reasons_after & kOccBit) == 0,
          "kOccurrenceDirty cleared via primitive");
    CHECK((reasons_after & kGeneral) != 0,
          "kGeneralDirty preserved (primitive doesn't clobber)");

    // Cleanup: clear all dirty bits for the target so we don't
    // leak state into other tests.
    ws->clear_dirty(target);

    return true;
}

// ── AC4: Out-of-range node-id returns #f ───────────────────
//
// The hook must gracefully handle a node-id past the workspace
// size — the eval path can't crash on user input.

bool test_out_of_range_returns_false() {
    std::println("\n--- AC4: out-of-range node-id returns #f ---");
    aura::compiler::CompilerService cs;
    CHECK(load_workspace(cs, "(define h 1)"),
          "load_workspace succeeds");

    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr, "workspace_flat() non-null");
    if (!ws) return true;

    // Pick a node-id past the workspace size.
    aura::ast::NodeId bad = static_cast<aura::ast::NodeId>(ws->size() + 100);
    auto mark_r = cs.eval(
        std::string("(compile:mark-narrowing-dirty! ") +
        std::to_string(bad) + ")");
    CHECK(mark_r.has_value(), "mark with bad id returns");
    if (mark_r) {
        CHECK(!aura::compiler::types::as_bool(*mark_r),
              "mark with bad id returns #f");
    }
    auto peek_r = cs.eval(
        std::string("(compile:narrowing-dirty? ") +
        std::to_string(bad) + ")");
    CHECK(peek_r.has_value(), "peek with bad id returns");
    if (peek_r) {
        CHECK(!aura::compiler::types::as_bool(*peek_r),
              "peek with bad id returns #f");
    }
    return true;
}

// ── AC5: Empty / non-int args return #f ─────────────────────
//
// The primitive must not crash on garbage args. The Aura-level
// type check (is_int) returns #f before the hook is called.

bool test_bad_args_return_false() {
    std::println("\n--- AC5: empty / non-int args return #f ---");
    aura::compiler::CompilerService cs;
    CHECK(load_workspace(cs, "(define k 1)"),
          "load_workspace succeeds");

    // No args.
    auto r1 = cs.eval("(compile:mark-narrowing-dirty!)");
    CHECK(r1.has_value(), "no-args returns");
    if (r1) {
        CHECK(!aura::compiler::types::as_bool(*r1),
              "no-args returns #f");
    }
    auto r2 = cs.eval("(compile:narrowing-dirty?)");
    CHECK(r2.has_value(), "no-args (peek) returns");
    if (r2) {
        CHECK(!aura::compiler::types::as_bool(*r2),
              "no-args (peek) returns #f");
    }

    // Non-int first arg.
    auto r3 = cs.eval("(compile:mark-narrowing-dirty! \"not-a-number\")");
    CHECK(r3.has_value(), "non-int arg returns");
    if (r3) {
        CHECK(!aura::compiler::types::as_bool(*r3),
              "non-int arg returns #f");
    }

    // Non-bool second arg (defaults to set=true via the
    // primitive's fallback path). After previously marking
    // node 1 with the occurrence bit (or even if not), the
    // hook returns the prior state. We test that:
    //   1. The call doesn't crash
    //   2. The return is a bool (success — either #t or #f
    //      is valid; both indicate the primitive ran)
    auto r4 = cs.eval("(compile:mark-narrowing-dirty! 1 \"ignored\")");
    CHECK(r4.has_value(), "non-bool 2nd arg returns");
    if (r4) {
        // The hook returns the prior state when called with
        // set=true. The test just verifies the primitive ran
        // without crashing — both #t and #f are valid outcomes.
        CHECK(aura::compiler::types::is_bool(*r4),
              "non-bool 2nd arg returns a bool (primitive ran)");
    }

    // Cleanup: clear the bit we may have set on node 1.
    auto* ws = cs.workspace_flat();
    if (ws) {
        for (std::uint32_t i = 0; i < ws->size(); ++i) {
            ws->clear_dirty_for(i,
                static_cast<std::uint8_t>(
                    aura::ast::FlatAST::DirtyReason::kOccurrenceDirty));
        }
    }

    return true;
}

// ── AC6: Per-node scoping — find_occurrence_contexts honors
//          kOccurrenceDirty (precise) vs general-only (conservative)
//          vs clean (skip)
// ─────────────────────────────────────────────────────────────
//
// This is the AC that motivated #240. We construct a workspace
// with an if-context, mark just the if-node with
// kOccurrenceDirty, and verify post_mutation_invariant_check
// emits the precise note (not the conservative fallback, not
// no note at all). Three sub-cases (A/B/C) cover all dirty-bit
// combinations.

bool test_per_node_scoping_in_post_mutation_check() {
    std::println("\n--- AC6: per-node scoping via post_mutation_invariant_check ---");
    aura::compiler::CompilerService cs;
    // (if (string? x) ...) uses a built-in predicate that
    // analyze_predicate_flat recognizes without TypeRegistry state.
    CHECK(load_workspace(cs,
        "(define (pred x) "
        "  (if (string? x) (+ 1 2) (+ 3 4)))"),
        "load_workspace succeeds");

    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr, "workspace_flat() non-null");
    if (!ws) return true;
    CHECK(cs.workspace_pool() != nullptr, "workspace_pool() non-null");
    if (!cs.workspace_pool()) return true;
    auto& pool_ref = *cs.workspace_pool();

    // Find the IfExpr node in the workspace. Walk all nodes,
    // pick the one with tag IfExpr.
    aura::ast::NodeId if_id = aura::ast::NULL_NODE;
    for (std::uint32_t i = 0; i < ws->size(); ++i) {
        if (static_cast<std::uint32_t>(ws->get(i).tag) ==
            static_cast<std::uint32_t>(aura::ast::NodeTag::IfExpr)) {
            if_id = i;
            break;
        }
    }
    CHECK(if_id != aura::ast::NULL_NODE,
          "found IfExpr node in workspace");
    if (if_id == aura::ast::NULL_NODE) return true;

    // Build a minimal MutationRecord so post_mutation_invariant_check
    // can walk the dirty scope (parent_id + target_node). Note: the
    // MutationRecord struct has no `snapshot_id` or `kind` field —
    // just target_node + parent_id are enough for the dirty-walk.
    aura::ast::MutationRecord rec;
    rec.target_node = if_id;
    rec.parent_id = ws->parent_of(if_id);

    // Build a TypeRegistry (needed by analyze_predicate_flat for
    // (type? x "Name") predicates; built-in predicates like
    // (string? x) don't need registry state). Default-constructed
    // TypeRegistry has the pre-defined types (DYNAMIC=0, INT=1,
    // BOOL=2, STRING=3, ...) so the built-in predicates resolve.
    aura::core::TypeRegistry reg;

    // ── Case A: kOccurrenceDirty set on the if-node ────
    // Mark the IfExpr node with kOccurrenceDirty and clear
    // other bits. Post-mutation check should emit a precise
    // note with kind == "StaleOccurrenceRefinement".
    const auto kOccBit = static_cast<std::uint8_t>(
        aura::ast::FlatAST::DirtyReason::kOccurrenceDirty);
    const auto kGeneral = static_cast<std::uint8_t>(
        aura::ast::FlatAST::DirtyReason::kGeneralDirty);
    ws->clear_dirty(if_id);
    ws->mark_dirty(if_id, kOccBit);

    std::vector<aura::compiler::OwnershipNote> notes;
    auto status = aura::compiler::post_mutation_invariant_check(
        *ws, pool_ref, reg, rec, notes);

    bool found_precise = false;
    bool found_conservative = false;
    for (auto& n : notes) {
        if (n.kind == "StaleOccurrenceRefinement") found_precise = true;
        if (n.kind == "StaleOccurrenceRefinement-conservative") found_conservative = true;
    }
    CHECK(found_precise,
          "Case A: precise note emitted for kOccurrenceDirty-tagged node");
    CHECK(!found_conservative,
          "Case A: conservative note NOT emitted when precise bit is set");

    // ── Case B: only kGeneralDirty set (no kOccurrenceDirty) ───
    // Pre-#240 behavior — emits the conservative note.
    ws->clear_dirty(if_id);
    ws->mark_dirty(if_id, kGeneral);

    notes.clear();
    status = aura::compiler::post_mutation_invariant_check(
        *ws, pool_ref, reg, rec, notes);

    found_precise = false;
    found_conservative = false;
    for (auto& n : notes) {
        if (n.kind == "StaleOccurrenceRefinement") found_precise = true;
        if (n.kind == "StaleOccurrenceRefinement-conservative") found_conservative = true;
    }
    CHECK(!found_precise,
          "Case B: precise note NOT emitted when only general bit is set");
    CHECK(found_conservative,
          "Case B: conservative note emitted (backward-compat path)");

    // ── Case C: no dirty bits ───
    // The pre-#240 behavior would still flag (false positive).
    // Post-#240 should skip the node entirely.
    ws->clear_dirty(if_id);

    notes.clear();
    status = aura::compiler::post_mutation_invariant_check(
        *ws, pool_ref, reg, rec, notes);

    found_precise = false;
    found_conservative = false;
    for (auto& n : notes) {
        if (n.kind == "StaleOccurrenceRefinement") found_precise = true;
        if (n.kind == "StaleOccurrenceRefinement-conservative") found_conservative = true;
    }
    CHECK(!found_precise,
          "Case C: precise note NOT emitted for clean node");
    CHECK(!found_conservative,
          "Case C: conservative note NOT emitted for clean node");

    return true;
}

// ── AC7: Mark! is idempotent on the bit level ──────────────
//
// Calling mark-narrowing-dirty! twice in a row on a clean node
// must keep the bit set without flipping other state.

bool test_mark_is_idempotent() {
    std::println("\n--- AC7: mark-narrowing-dirty! is idempotent ---");
    aura::compiler::CompilerService cs;
    CHECK(load_workspace(cs, "(define (m x) (* x 2))"),
          "load_workspace succeeds");

    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr, "workspace_flat() non-null");
    if (!ws) return true;
    aura::ast::NodeId target = 1;
    if (target >= ws->size()) target = static_cast<aura::ast::NodeId>(ws->size() - 1);

    const auto kOccBit = static_cast<std::uint8_t>(
        aura::ast::FlatAST::DirtyReason::kOccurrenceDirty);
    ws->clear_dirty(target);

    // First mark.
    auto r1 = cs.eval(
        std::string("(compile:mark-narrowing-dirty! ") +
        std::to_string(target) + ")");
    CHECK(r1.has_value(), "first mark returns");
    auto reasons1 = ws->dirty_reasons(target);
    CHECK((reasons1 & kOccBit) != 0, "bit set after first mark");

    // Second mark — bit stays set, no error.
    auto r2 = cs.eval(
        std::string("(compile:mark-narrowing-dirty! ") +
        std::to_string(target) + ")");
    CHECK(r2.has_value(), "second mark returns");
    auto reasons2 = ws->dirty_reasons(target);
    CHECK((reasons2 & kOccBit) != 0, "bit still set after second mark");

    // Cleanup.
    ws->clear_dirty(target);
    return true;
}

// ═════════════════════════════════════════════════════════════
// Main test runner
// ═════════════════════════════════════════════════════════════

int run_tests() {
    std::println("═══ Issue #240 verification tests ═══\n");
    std::println("AC #1+#2: compile:mark-narrowing-dirty! sets/clears the bit");
    test_mark_sets_bit();

    std::println("\nAC #3: kOccurrenceDirty bit doesn't clobber other bits");
    test_mark_preserves_other_bits();

    std::println("\nAC #4: out-of-range node-id returns #f");
    test_out_of_range_returns_false();

    std::println("\nAC #5: empty / non-int args return #f");
    test_bad_args_return_false();

    std::println("\nAC #6: per-node scoping via post_mutation_invariant_check");
    test_per_node_scoping_in_post_mutation_check();

    std::println("\nAC #7: mark-narrowing-dirty! is idempotent");
    test_mark_is_idempotent();

    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}
}  // namespace aura_issue_240_detail

int aura_issue_240_run() { return aura_issue_240_detail::run_tests(); }

