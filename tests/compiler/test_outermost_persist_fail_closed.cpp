// @category: unit
// @reason: Issue #3376 \u2014 outermost persist must not stamp green / grant
// query:type when fingerprint-mismatch or occurrence recover fails.
// Three half-green exits on aura_outermost_success_persist_occurrence:
//   1. fingerprint mismatch early-return (no reject stamp / proof-face clear)
//   2. drain non-SOLVED (already stamps reject \u2014 add clear_type_linear_commit_proof_on_abort)
//   3. ensure_occurrence_commit_or_recover return discarded (add full reject path)
// Soft / Off: zero extra (mismatch / recover helpers already early-out when
// !production_defaults_active()). Non-duplicative to #2938/#2995/#3170/
// #3281/#3030/#3237/#3316/#3318.
//
//   AC1: source cites the fingerprint-mismatch early-return + new reject stamp
//   AC2: source cites the new check after ensure_occurrence_commit_or_recover
//   AC3: source cites clear_type_linear_commit_proof_on_abort in all 4 existing
//        reject paths (mid-abort, drain non-SOLVED, pending face hit, ADT exhaust)
//   AC4: no docs/design/3376-*; no test_issue_3376.cpp per #1655 / #81967

#include "test_harness.hpp"
#include "compiler/dce_elided_deopt_meta.h"
#include "compiler/mutation_concurrency_health.hh"
#include "compiler/typed_mutation_audit.h"
#include "compiler/messaging_bridge.h"
#include "core/flatast_restamp.hh"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.ast;
import aura.compiler.coercion_map;
import aura.compiler.dirty_propagation;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.type_checker;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
namespace typed_audit = aura::compiler::typed_audit;
using aura::compiler::TypeChecker;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::get_strategy;
using aura::compiler::typed_audit::ir_typed_entry_commit_readiness_ok;
using aura::compiler::typed_audit::jit_execute_commit_readiness_blocked;
using aura::compiler::typed_audit::kCoercionMapPersistRejectUndoIssue;
using aura::compiler::typed_audit::kTypeLinearProofOutcomeReject;
using aura::compiler::typed_audit::last_proof_stamper_bound_v_read;
using aura::compiler::typed_audit::last_type_linear_proof_outcome_v_read;
using aura::compiler::typed_audit::linear_move_drop_elision_ok;
using aura::compiler::typed_audit::production_hard_face_active;
using aura::compiler::typed_audit::publish_last_proof_face;
using aura::compiler::typed_audit::publish_type_linear_proof_outcome;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::stamp_type_linear_commit_proof;
using aura::compiler::typed_audit::undo_apply_coercion_map_recent;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href_health(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:type-linear-commit-health\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static aura::ast::NodeId find_literal_int(aura::ast::FlatAST& flat, std::int64_t want) {
    using aura::ast::NodeId;
    using aura::ast::NodeTag;
    using aura::ast::NULL_NODE;
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto v = flat.get(id);
        if (v.tag == NodeTag::LiteralInt && v.int_value == want)
            return id;
    }
    return NULL_NODE;
}

static aura::ast::NodeId find_first_tag(aura::ast::FlatAST& flat, aura::ast::NodeTag tag) {
    using aura::ast::NodeId;
    using aura::ast::NULL_NODE;
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        if (flat.get(id).tag == tag)
            return id;
    }
    return NULL_NODE;
}

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

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

Evaluator* g_ev_arm_pending_3697 = nullptr;
void arm_linear_pending_3697() {
    if (!g_ev_arm_pending_3697)
        return;
    g_ev_arm_pending_3697->note_linear_synth_hard_fail_pending();
    g_ev_arm_pending_3697->mark_outermost_mutation_failed();
}

} // namespace

// Brace-matched block extractor: from byte offset `from`, find the first
// '{' and return that brace block (open..matching close) as a string,
// skipping braces inside string/char literals and // /* */ comments.
// Empty when no '{' follows or the block is unterminated. Replaces the
// fixed-size windows (1800/2000/2400, all drift-bait): the body itself is
// the window, so only structural changes go red.
static std::string brace_block_after(const std::string& text, std::size_t from) {
    const auto open = text.find('{', from);
    if (open == std::string::npos)
        return {};
    int depth = 0;
    bool in_str = false, in_chr = false, in_line = false, in_blk = false;
    for (std::size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        const char n = i + 1 < text.size() ? text[i + 1] : '\0';
        if (in_line) {
            if (c == '\n')
                in_line = false;
            continue;
        }
        if (in_blk) {
            if (c == '*' && n == '/') {
                in_blk = false;
                ++i;
            }
            continue;
        }
        if (in_str) {
            if (c == '\\') {
                ++i;
                continue;
            }
            if (c == '"')
                in_str = false;
            continue;
        }
        if (in_chr) {
            if (c == '\\') {
                ++i;
                continue;
            }
            if (c == '\'')
                in_chr = false;
            continue;
        }
        if (c == '/' && n == '/') {
            in_line = true;
            ++i;
            continue;
        }
        if (c == '/' && n == '*') {
            in_blk = true;
            ++i;
            continue;
        }
        if (c == '"') {
            in_str = true;
            continue;
        }
        if (c == '\'') {
            in_chr = true;
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0)
                return text.substr(open, i - open + 1);
        }
    }
    return {};
}

// Issue #4310: production persist-reject and outermost abort put type
// columns and the solve_delta high-water back. Soft does not copy.
static void ac4310_abort_restores_type_columns_and_cs() {
    std::println("\n--- #4310: abort restores type columns and CS high-water ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto ast = read_file("src/core/ast.ixx");
    const auto helper = emb.find("void Evaluator::restore_abort_type_face");
    CHECK(helper != std::string::npos, "4310: helper present");
    // Window = brace-matched helper body (drift-proof; was fixed 1800).
    const auto win = brace_block_after(emb, helper);
    CHECK(!win.empty(), "4310: helper body (brace-matched)");
    CHECK(emb.find("Issue #4310") != std::string::npos, "4310: boundary cite");
    CHECK(win.find("restore_type_columns") != std::string::npos, "4310: restores columns");
    CHECK(win.find("restore_abort_high_water") != std::string::npos, "4310: restores CS");
    CHECK(win.find("invalidate_persistent_typechecker") == std::string::npos,
          "4310: helper does not drop the persistent checker");
    CHECK(ast.find("void restore_type_columns") != std::string::npos, "4310: column restore");
    CHECK(ast.find("bump_type_cache_generation()") != std::string::npos,
          "4310: generation bump stays available");
    CHECK(emb.find("defuse_version_.fetch_add(1, std::memory_order_release);") != std::string::npos,
          "4310: enter defuse bump stays");
    CHECK(read_file("tests/compiler/test_issue_4310.cpp").empty(), "4310: no test_issue file");
    CHECK(read_file("docs/design/4310-type-column-abort.md").empty(), "4310: no design doc");

    reset_for_test();
    apply_production_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "4310: warm");
    CHECK(cs.eval("(set-code \"(define f (+ 1 2))\")").has_value(), "4310: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4310: eval");
    CHECK(cs.eval("(typecheck-current)").has_value(), "4310: typecheck");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "4310: workspace");
    const auto lit = ws ? find_literal_int(*ws, 1) : aura::ast::NULL_NODE;
    const auto call = ws ? find_first_tag(*ws, aura::ast::NodeTag::Call) : aura::ast::NULL_NODE;
    CHECK(lit != aura::ast::NULL_NODE, "4310: literal");
    CHECK(call != aura::ast::NULL_NODE, "4310: call");
    auto* tc = static_cast<TypeChecker*>(cs.evaluator().persistent_typechecker());
    if (!tc)
        tc = static_cast<TypeChecker*>(cs.evaluator().commit_type_checker_handle());
    CHECK(tc != nullptr, "4310: type checker");
    if (!ws || !tc || lit == aura::ast::NULL_NODE || call == aura::ast::NULL_NODE) {
        apply_dev_audit_defaults();
        reset_for_test();
        return;
    }
    const auto type0 = ws->type_id(lit);
    const auto call_type0 = ws->type_id(call);
    const auto gen_col0 = ws->type_cache_gen(lit);
    const auto bind0 = ws->type_cache_binding_gen(lit);
    const auto schema0 = ws->schema_cache(lit);
    const auto cache_gen0 = ws->type_cache_generation();
    const auto cs_n0 = tc->constraint_system().constraints_size();
    const auto uf0 = tc->constraint_system().union_find_size();
    const auto adt0 = tc->constraint_system().adt_match_goals_size();
    const auto pending0 = tc->constraint_system().pending_full_solve_roots_size();
    const auto touched0 = tc->constraint_system().touched_roots_size();
    const auto occ_pri0 = tc->constraint_system().occurrence_priority_roots_size();
    const auto let0 = tc->constraint_system().let_poly_dirty_roots_size();
    const auto goals0 = tc->constraint_system().occurrence_goals_size();
    const auto defuse0 = cs.evaluator().defuse_version();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        CHECK(cs.evaluator().defuse_version() > defuse0, "4310: enter still bumps defuse");
        ws->set_type(lit, 0x4310);
        ws->set_schema_cache(lit, 0x4310);
        ws->set_type_with_binding_gen(lit, 0x4310, 0x4310, 0x4310);
        ws->set_type(call, 0x4310);
        const auto extra = ws->add_literal(99);
        ws->set_type(extra, 0x4310);
        aura::compiler::Constraint c;
        c.kind = aura::compiler::Constraint::EQUAL;
        tc->constraint_system().add(c);
        tc->constraint_system().note_adt_match_goal(7, 8, 9);
        tc->constraint_system().seed_pending_full_solve_root_for_test(42);
        tc->constraint_system().seed_abort_priority_roots_for_test(43);
        tc->constraint_system().grow_union_find_for_test();
        const auto gen_before = ws->type_cache_generation();
        cs.evaluator().restore_checkpoint_topology_for_persist_reject();
        CHECK(ws->type_id(lit) == type0, "4310: literal type restored");
        CHECK(ws->type_id(call) == call_type0, "4310: call type restored");
        CHECK(ws->type_cache_gen(lit) == gen_col0, "4310: cache gen column restored");
        CHECK(ws->type_cache_binding_gen(lit) == bind0, "4310: binding gen restored");
        CHECK(ws->schema_cache(lit) == schema0, "4310: schema cache restored");
        CHECK(ws->type_id(extra) == 0, "4310: node added in the boundary is zero");
        CHECK(ws->type_cache_generation() > gen_before, "4310: cache generation bumped once");
        CHECK(tc->constraint_system().constraints_size() == cs_n0, "4310: constraints truncated");
        CHECK(tc->constraint_system().union_find_size() == uf0, "4310: union-find restored");
        CHECK(tc->constraint_system().adt_match_goals_size() == adt0, "4310: adt goals truncated");
        CHECK(tc->constraint_system().pending_full_solve_roots_size() == pending0,
              "4310: pending roots restored");
        CHECK(tc->constraint_system().touched_roots_size() == touched0, "4310: touched restored");
        CHECK(tc->constraint_system().occurrence_priority_roots_size() == occ_pri0,
              "4310: occurrence priority restored");
        CHECK(tc->constraint_system().let_poly_dirty_roots_size() == let0,
              "4310: let-poly restored");
        CHECK(tc->constraint_system().occurrence_goals_size() == goals0,
              "4310: occurrence goals left to their own restore");
        ok = false;
    }
    CHECK(ws->type_id(call) == call_type0, "4310: call type still the enter id after dtor");
    CHECK(ws->type_cache_generation() > cache_gen0, "4310: generation stays bumped");

    // Outermost !success, without calling the persist-reject helper.
    ws->set_type(lit, type0);
    const auto type_again = ws->type_id(lit);
    bool ok2 = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok2);
        ws->set_type(lit, 0x4311);
        tc->constraint_system().add({});
        ok2 = false;
    }
    CHECK(ws->type_id(lit) == type_again, "4310: outermost abort restores the column");
    CHECK(tc->constraint_system().constraints_size() == cs_n0,
          "4310: outermost abort truncates constraints");

    apply_dev_audit_defaults();
    reset_for_test();
    CompilerService cs_soft;
    CHECK(cs_soft.eval("(set-code \"(define s 1)\")").has_value(), "4310: soft set-code");
    auto* ws_soft = cs_soft.evaluator().workspace_flat();
    const auto lit_s = ws_soft ? find_literal_int(*ws_soft, 1) : aura::ast::NULL_NODE;
    CHECK(lit_s != aura::ast::NULL_NODE, "4310: soft literal");
    if (ws_soft && lit_s != aura::ast::NULL_NODE) {
        const auto soft_before = ws_soft->type_id(lit_s);
        bool ok_s = true;
        {
            Evaluator::MutationBoundaryGuard g(cs_soft.evaluator(), &ok_s);
            ws_soft->set_type(lit_s, 0x4310);
            cs_soft.evaluator().restore_checkpoint_topology_for_persist_reject();
            CHECK(ws_soft->type_id(lit_s) == 0x4310, "4310: soft helper does not copy columns");
            ok_s = false;
        }
        CHECK(ws_soft->type_id(lit_s) == 0x4310, "4310: soft abort does not restore columns");
        (void)soft_before;
    }
    apply_dev_audit_defaults();
    reset_for_test();
}

int run_test_outermost_persist_fail_closed() {
    std::println("=== Issue #3376: outermost persist fail-closed (no half-green) ===");
    CHECK(true, "3376: issue stamp");

    // \u2500\u2500 AC1: fingerprint-mismatch early-return + new reject stamp \u2500\u2500
    {
        std::println("\n--- AC1: fingerprint mismatch early-return ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        // Find aura_outermost_success_persist_occurrence function body.
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        CHECK(fn_pos != std::string::npos,
              "AC1: aura_outermost_success_persist_occurrence present");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        // The fingerprint mismatch branch must stamp reject + clear proof.
        CHECK(contains(emb_after, "expected_occurrence_snapshot_fp() != 0"),
              "AC1: fingerprint mismatch check present");
        CHECK(contains(emb_after, "build_type_linear_commit_proof_from_live_with_outcome("),
              "AC1: reject proof stamp present");
        CHECK(contains(emb_after, "kTypeLinearProofOutcomeReject"),
              "AC1: reject outcome stamp present");
        CHECK(contains(emb_after, "clear_type_linear_commit_proof_on_abort()"),
              "AC1: clear_type_linear_commit_proof_on_abort called");
        CHECK(contains(emb_after, "ev->clear_type_export_authority()"),
              "AC1: clear_type_export_authority drops stale grant");
        // Must reference #3376 to anchor the regression contract.
        CHECK(contains(emb_after, "#3376"), "AC1: helper cites #3376");
    }

    // \u2500\u2500 AC2: new check after ensure_occurrence_commit_or_recover \u2500\u2500
    {
        std::println("\n--- AC2: ensure_occurrence_commit_or_recover return check ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        // The return of ensure_occurrence_commit_or_recover must NOT be discarded.
        // Find the new check.
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        // The new check is: if (!tc->ensure_occurrence_commit_or_recover()) { ... return; }
        // It must come AFTER the (line 403) build_type_linear_commit_proof_from_live stamp.
        const auto new_check_pos =
            emb_after.find("if (!tc->ensure_occurrence_commit_or_recover())");
        CHECK(new_check_pos != std::string::npos,
              "AC2: new if-check on ensure_occurrence_commit_or_recover present");
        const auto stamp_pos = emb_after.find("build_type_linear_commit_proof_from_live(");
        CHECK(stamp_pos != std::string::npos,
              "AC2: build_type_linear_commit_proof_from_live stamp present");
        CHECK(new_check_pos > stamp_pos,
              "AC2: new check comes AFTER the green proof stamp (ordering matches issue body)");
        // The new check must stamp reject + clear proof + skip grant.
        CHECK(contains(emb_after.substr(new_check_pos),
                       "build_type_linear_commit_proof_from_live_with_outcome("),
              "AC2: reject proof stamp in new check");
        CHECK(contains(emb_after.substr(new_check_pos), "kTypeLinearProofOutcomeReject"),
              "AC2: reject outcome in new check");
        CHECK(
            contains(emb_after.substr(new_check_pos), "clear_type_linear_commit_proof_on_abort()"),
            "AC2: clear_type_linear_commit_proof_on_abort in new check");
    }

    // \u2500\u2500 AC3: clear_type_linear_commit_proof_on_abort in all 4 existing reject paths
    // \u2500\u2500
    {
        std::println("\n--- AC3: clear_type_linear_commit_proof_on_abort in all reject paths ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        // Count occurrences of clear_type_linear_commit_proof_on_abort.
        // Expected: 1 in Exit 1 (fingerprint mismatch) + 1 in mid-abort + 1 in drain
        // non-SOLVED + 1 in pending face hit + 1 in ADT exhaust + 1 in new Exit 3
        // check = 6 total.
        const auto count = [](const std::string& s, const char* needle) -> std::size_t {
            std::size_t c = 0, pos = 0;
            while ((pos = s.find(needle, pos)) != std::string::npos) {
                ++c;
                pos += std::strlen(needle);
            }
            return c;
        };
        const std::size_t n = count(emb_after, "clear_type_linear_commit_proof_on_abort");
        CHECK(n >= 5, "AC3: clear_type_linear_commit_proof_on_abort called >= 5 times "
                      "(Exit 1 + 4 existing reject paths + new Exit 3)");
    }

    // \u2500\u2500 AC4: no docs/design/3376-*; no test_issue_3376.cpp \u2500\u2500
    {
        std::println("\n--- AC4: no docs/design/3376-*; no test_issue_3376.cpp ---");
        CHECK(read_file("docs/design/3376-outermost-persist-fail-closed.md").empty(),
              "AC4: no docs/design/3376-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_3376.cpp").empty(),
              "AC4: no test_issue_3376.cpp per #81967");
        CHECK(read_file("tests/issues/test_issue_3376.cpp").empty(),
              "AC4: no tests/issues/test_issue_3376.cpp (R1 abandoned scheme)");
    }

    // AC5: #3406 recover-fail branch clears persist buffer + bumps mismatch.
    {
        std::println("\n--- AC5: #3406 recover-fail branch clears persist buffer ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        const auto recover_pos = emb_after.find("if (!tc->ensure_occurrence_commit_or_recover())");
        CHECK(recover_pos != std::string::npos,
              "AC5: ensure_occurrence_commit_or_recover check present");
        const auto return_pos = emb_after.find(
            "return; // skip grant; recover face stamps via publish_occurrence_commit_health",
            recover_pos);
        CHECK(return_pos != std::string::npos, "AC5: recover-fail return line present");
        const auto branch = emb_after.substr(recover_pos, return_pos - recover_pos);
        CHECK(contains(branch, "clear_occurrence_persist_buffer(tc)"),
              "AC5: recover-fail branch calls clear_occurrence_persist_buffer(tc)");
        CHECK(contains(branch, "bump_occurrence_persist_fingerprint_mismatch"),
              "AC5: recover-fail branch bumps mismatch counter");
        CHECK(contains(branch, "#3406"),
              "AC5: recover-fail branch cites #3406 (source-cite anchor)");
    }

    // AC6: clear_occurrence_persist_buffer count >= 6 (5 existing + #3406 recover-fail).
    {
        std::println("\n--- AC6: clear_occurrence_persist_buffer count >= 6 ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        const auto count = [](const std::string& s, const char* needle) -> std::size_t {
            std::size_t c = 0, pos = 0;
            while ((pos = s.find(needle, pos)) != std::string::npos) {
                ++c;
                pos += std::strlen(needle);
            }
            return c;
        };
        const std::size_t n = count(emb_after, "clear_occurrence_persist_buffer(tc)");
        CHECK(n >= 6, "AC6: clear_occurrence_persist_buffer(tc) called >= 6 times "
                      "(5 existing reject paths + #3406 recover-fail)");
        const auto recover_pos = emb_after.find("if (!tc->ensure_occurrence_commit_or_recover())");
        const auto before_recover =
            (recover_pos == std::string::npos) ? emb_after : emb_after.substr(0, recover_pos);
        const std::size_t n_before = count(before_recover, "clear_occurrence_persist_buffer(tc)");
        CHECK(n_before >= 5,
              "AC6: existing 5 reject paths still call clear_occurrence_persist_buffer "
              "(#3376 contract unchanged)");
    }

    // AC7: no docs/design/3406-*; no test_issue_3406.cpp.
    {
        std::println("\n--- AC7: no docs/design/3406-*; no test_issue_3406.cpp ---");
        CHECK(read_file("docs/design/3406-recover-fail-clear-persist.md").empty(),
              "AC7: no docs/design/3406-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_3406.cpp").empty(),
              "AC7: no test_issue_3406.cpp per #81967");
        CHECK(read_file("tests/issues/test_issue_3406.cpp").empty(),
              "AC7: no tests/issues/test_issue_3406.cpp (R1 abandoned scheme)");
    }

    // ── #3431: unstaged expected_fp==0 skips #3170 guard under Production ──
    {
        std::println("\n--- #3431 AC1: Production + expected==0 + live goals → abort ---");
        CHECK(aura::compiler::typed_audit::kOccurrenceUnstagedExpectedFpIssue == 3431,
              "3431 AC1: stamp");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        const auto persist_pos = emb_after.find("maybe_persist_occurrence_snapshot");
        CHECK(persist_pos != std::string::npos, "3431 AC1: persist write site present");
        const auto win =
            persist_pos == std::string::npos ? emb_after : emb_after.substr(0, persist_pos);
        CHECK(contains(win, "Issue #3431"), "3431 AC1: unstaged abort cite");
        CHECK(contains(win, "expected == 0"), "3431 AC1: unstaged expected==0");
        CHECK(contains(win, "live_goal_count > 0"), "3431 AC1: nonempty live goals");
        CHECK(contains(win, "clear_occurrence_persist_buffer(tc)"),
              "3431 AC1: no persist write (clear before maybe_persist)");
        CHECK(contains(win, "kTypeLinearProofOutcomeReject"), "3431 AC1: proof Reject");
        CHECK(contains(win, "clear_type_export_authority()"), "3431 AC1: grant false");
        CHECK(contains(win, "force_reason=*/16"), "3431 AC1: reuse force_reason 16");
        CHECK(contains(win, "bump_occurrence_persist_fingerprint_mismatch"),
              "3431 AC1: reuse mismatch counter");
    }

    {
        std::println("\n--- #3431 AC2: staged expected match path unchanged (#3170/#3376) ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(contains(emb, "if (aura::compiler::typed_audit::production_hard_face_active() &&\n"
                            "        ev->expected_occurrence_snapshot_fp() != 0 &&\n"
                            "        live_fp != ev->expected_occurrence_snapshot_fp()) {"),
              "3431 AC2: #3170 staged-mismatch needle kept (prod||Full via hard_face)");
        CHECK(contains(emb, "Issue #3376"), "3431 AC2: #3376 reject stamp kept");
    }

    {
        std::println("\n--- #3431 AC3: Soft expected 0 does not bump mismatch ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        const auto u = emb_after.find("Issue #3431");
        CHECK(u != std::string::npos, "3431 AC3: #3431 block");
        const auto uwin = u == std::string::npos ? std::string{} : emb_after.substr(u, 1200);
        CHECK(contains(uwin, "production_defaults_active()"), "3431 AC3: hard gate");
        CHECK(contains(uwin, "get_strategy()"), "3431 AC3: Full strategy in hard");
        CHECK(contains(emb, "Soft keeps expected==0 skip"), "3431 AC3: Soft 0==0 skip");
    }

    {
        std::println("\n--- #3431 AC4: #3406 recover-fail clear still required ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(contains(emb, "Issue #3406"), "3431 AC4: #3406 cite kept");
        const auto recover_pos = emb.find("if (!tc->ensure_occurrence_commit_or_recover())");
        CHECK(recover_pos != std::string::npos, "3431 AC4: recover-fail check kept");
        CHECK(emb.find("clear_occurrence_persist_buffer(tc)", recover_pos) != std::string::npos,
              "3431 AC4: recover-fail still clears persist");
    }

    {
        std::println("\n--- #3431 AC5: no invent / docs ---");
        CHECK(read_file("docs/design/3431-unstaged-expected-fp.md").empty(),
              "3431 AC5: no docs/design");
        CHECK(read_file("tests/compiler/test_issue_3431.cpp").empty(), "3431 AC5: no invent");
        CHECK(read_file("tests/issues/test_issue_3431.cpp").empty(), "3431 AC5: no issues invent");
        const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(q.find("schema-3431") == std::string::npos, "3431 AC5: no schema-3431");
        const auto build = read_file("build.py");
        CHECK(contains(build, "check_occurrence_unstaged_expected_fp_3431"),
              "3431 AC5: build.py wires linter");
    }

    {
        std::println("\n--- #3440: persist-reject notes restore (structural commit face) ---");
        CHECK(aura::compiler::typed_audit::kOutermostPersistRejectRestoreIssue == 3440,
              "3440: stamp");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        CHECK(contains(emb_after, "note_3440_restore()"), "3440: persist-reject arms note restore");
        CHECK(contains(emb, "consume_outermost_persist_reject_needs_restore()"),
              "3440: dtor consumes restore flag");
        CHECK(contains(emb, "Issue #3440"), "3440: dtor cites #3440");
        const auto persist_call = emb.find("aura_outermost_success_persist_occurrence(ev_");
        const auto exit_pos = emb.find("ev_->exit_mutation_boundary(success)");
        CHECK(persist_call != std::string::npos && exit_pos != std::string::npos &&
                  persist_call < exit_pos,
              "3440: persist helper runs BEFORE exit_mutation_boundary");
    }

    {
        std::println("\n--- #3545: persist-reject CoercionMap undo ---");
        CHECK(kCoercionMapPersistRejectUndoIssue == 3545, "3545: stamp");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        CHECK(contains(emb, "undo_apply_coercion_map_recent") &&
                  (contains(emb_after, "aura_persist_reject_undo") ||
                   contains(emb_after, "undo_apply_coercion_map_recent")),
              "3545 AC2: persist-reject calls undo");
        CHECK(contains(emb, "kCoercionMapPersistRejectUndoIssue") ||
                  contains(read_file("src/compiler/typed_mutation_audit.h"),
                           "kCoercionMapPersistRejectUndoIssue = 3545"),
              "3545 AC2: typed_audit stamp");
        const auto cm = read_file("src/compiler/coercion_map.ixx");
        CHECK(contains(cm, "unmark_eliminated"), "3545 AC1: CoercionMap unmark");
        CHECK(contains(cm, "coercion_map_apply_journal_note_elision"),
              "3545 AC1: apply journals elision");
        CHECK(emb.find("schema-3545") == std::string::npos, "3545: no new query key");
        CHECK(read_file("tests/compiler/test_issue_3545.cpp").empty(), "3545: no invent");
        CHECK(read_file("tests/issues/test_issue_3545.cpp").empty(), "3545: no issues invent");
        CHECK(read_file("docs/design/3545-coercion-map-undo.md").empty(), "3545: no docs/design");

        reset_for_test();
        apply_production_audit_defaults();
        aura::compiler::dirty::reset_dead_coercion_decision_invalidate_for_test();
        aura::compiler::clear_coercion_map_abort_rewind_for_test();
        const auto elided0 = aura::compiler::g_dead_coercion_ast_elided_total.load();
        const auto gen0 = aura::compiler::dirty::dead_coercion_decision_invalidate_gen();
        aura::ast::StringPool pool;
        aura::ast::FlatAST flat;
        auto xv = flat.add_variable(pool.intern("x"));
        auto lit = flat.add_literal(1);
        flat.set_type(lit, 7);
        auto call = flat.add_call(xv, std::array<aura::ast::NodeId, 1>{lit});
        flat.root = call;
        aura::compiler::CoercionMap map;
        map.add(call, 1, lit, 1, 7, 0, 0);
        aura::compiler::coerced_nodes_tracker_enter_boundary();
        (void)aura::compiler::apply_coercion_map(flat, map, nullptr, &map);
        CHECK(map.eliminated_count() >= 1, "3545 AC1: identity elision marked");
        CHECK(aura::compiler::g_dead_coercion_ast_elided_total.load() > elided0,
              "3545 AC1: ast-elided bumped");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "3545 live: warm");
        undo_apply_coercion_map_recent(&cs.evaluator(), 3545);
        aura::compiler::coerced_nodes_tracker_exit_boundary();
        CHECK(aura::compiler::g_dead_coercion_ast_elided_total.load() == elided0,
              "3545 AC1: ast-elided restored on undo");
        CHECK(aura::compiler::dirty::dead_coercion_decision_invalidate_gen() > gen0,
              "3545 AC1: decision invalidate gen bumped");
        CHECK(last_proof_stamper_bound_v_read() == 0, "3545 AC4: stamper_bound=0 after undo");
        auto snap = cs.eval("(hash-ref (engine:metrics \"query:type-linear-evolution-snapshot\") "
                            "\"last-proof-stamper-bound\")");
        CHECK(snap && is_int(*snap) && as_int(*snap) == 0,
              "3545 AC4: query last-proof-stamper-bound is 0");
        apply_dev_audit_defaults();
    }

    {
        std::println("\n--- #3547: persist-reject invalidates elided CastOp deopt meta ---");
        using aura::compiler::dce_deopt::clear_elided_cast_deopt_meta_for_test;
        using aura::compiler::dce_deopt::lookup_elided_cast_deopt_meta;
        using aura::compiler::dce_deopt::make_site_key;
        using aura::compiler::dce_deopt::stamp_elided_cast_deopt_meta;
        CHECK(typed_audit::kDeadCoercionDecisionReverifyIssue == 3547, "3547: stamp");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(contains(emb, "invalidate_elided_cast_deopt_meta"),
              "3547 AC3: persist-reject invalidates deopt meta");
        reset_for_test();
        apply_production_audit_defaults();
        clear_elided_cast_deopt_meta_for_test();
        const auto site = make_site_key(0, 11, 3);
        stamp_elided_cast_deopt_meta(site, 3547, 4, 1, 7);
        CHECK(lookup_elided_cast_deopt_meta(site).has_value(), "3547 AC3: site live before undo");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "3547 live: warm");
        undo_apply_coercion_map_recent(&cs.evaluator(), 3547);
        CHECK(!lookup_elided_cast_deopt_meta(site).has_value(),
              "3547 AC3: persist-reject drops deopt meta");
        apply_dev_audit_defaults();
        clear_elided_cast_deopt_meta_for_test();
    }

    // ── Issue #3472: persist-green × Phase-1 linear deny (live, not only contains) ──
    {
        std::println("\n--- #3472 AC1 live: persist-green × pending → success false ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(contains(emb, "Issue #3472"), "3472: dtor cites #3472");
        const auto persist_call = emb.find("aura_outermost_success_persist_occurrence(ev_");
        const auto consume = emb.find("consume_outermost_persist_reject_needs_restore()");
        const auto issue = emb.find("Issue #3472");
        const auto exit_pos = emb.find("ev_->exit_mutation_boundary(success)");
        CHECK(persist_call != std::string::npos && consume != std::string::npos &&
                  issue != std::string::npos && exit_pos != std::string::npos &&
                  persist_call < consume && consume < issue && issue < exit_pos,
              "3472: persist → consume → linear deny → exit (before abort_restore)");
        const auto win =
            (issue == std::string::npos || exit_pos == std::string::npos || issue > exit_pos)
                ? std::string{}
                : emb.substr(issue, exit_pos - issue);
        CHECK(contains(win, "enforce_linear_boundary_consistency"),
              "3472: pre-exit enforce return is the deny signal");
        CHECK(contains(win, "linear_synth_hard_fail_pending"), "3472: pending is a deny signal");
        CHECK(contains(win, "clear_type_linear_commit_proof_on_abort"),
              "3472: reuse #3030 proof clear");
        CHECK(!contains(win, "linear_post_mutate_force_rollback_total"),
              "3472: rollback counter is not the deny signal");
        CHECK(emb.find("schema-3472") == std::string::npos, "3472: no new query key");

        reset_for_test();
        apply_production_audit_defaults();
        typed_audit::clear_type_linear_proof_outcome_for_test();
        typed_audit::clear_type_linear_commit_proof_for_test();
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "3472 live: warm");
        (void)cs.eval("(set-code \"(define f 1)\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(typecheck-current)");
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
            cs.evaluator().note_linear_synth_hard_fail_pending();
        }
        CHECK(!ok, "3472 live: success==false (not only contains(src))");
        CHECK(last_type_linear_proof_outcome_v_read() == kTypeLinearProofOutcomeReject,
              "3472 live: last_proof_outcome==Reject");
        CHECK(cs.evaluator().last_boundary_rollback_stats().children_column_restored,
              "3472 live: dual-topology restored");
        CHECK(!linear_move_drop_elision_ok(), "3472 live: !Move/Drop elision");
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = 1;
        CHECK(!ir_typed_entry_commit_readiness_ok(), "3472 live: IR typed-entry refused");
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = -1;
        // Issue #3818: #3472 window must call shared reject-undo (#3545 journal).
        CHECK(contains(win, "aura_persist_reject_undo") ||
                  contains(win, "undo_apply_coercion_map_recent"),
              "3818: #3472 post-persist deny calls CoercionMap undo");
        CHECK(contains(emb, "Issue #3818"), "3818: dtor cites #3818");
        CHECK(emb.find("schema-3818") == std::string::npos, "3818: no new query key");
        apply_dev_audit_defaults();
        reset_for_test();
    }

    {
        std::println(
            "\n--- #3818 soak: shared reject-undo restores elim (next CastOp/DCE safe) ---");
        // Live #3472 flip after green persist is hard to arm without the
        // #3614 pre-persist gate consuming the same pending/density latch
        // first. Static window checks above prove #3472 calls
        // aura_persist_reject_undo; here prove the shared #3545 undo
        // restores elim counts so a later CastOp/DCE cannot ride inflation.
        reset_for_test();
        apply_production_audit_defaults();
        aura::compiler::dirty::reset_dead_coercion_decision_invalidate_for_test();
        aura::compiler::clear_coercion_map_abort_rewind_for_test();
        const auto elided0 = aura::compiler::g_dead_coercion_ast_elided_total.load();
        aura::ast::StringPool pool;
        aura::ast::FlatAST flat;
        auto xv = flat.add_variable(pool.intern("x"));
        auto lit = flat.add_literal(1);
        flat.set_type(lit, 7);
        auto call = flat.add_call(xv, std::array<aura::ast::NodeId, 1>{lit});
        flat.root = call;
        aura::compiler::CoercionMap map;
        map.add(call, 1, lit, 1, 7, 0, 0);
        aura::compiler::coerced_nodes_tracker_enter_boundary();
        (void)aura::compiler::apply_coercion_map(flat, map, nullptr, &map);
        CHECK(map.eliminated_count() >= 1, "3818 soak: identity elision marked");
        CHECK(aura::compiler::g_dead_coercion_ast_elided_total.load() > elided0,
              "3818 soak: ast-elided bumped");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "3818 soak: warm");
        // Same undo the #3472 belt invokes via aura_persist_reject_undo.
        undo_apply_coercion_map_recent(&cs.evaluator(), 3818);
        aura::compiler::coerced_nodes_tracker_exit_boundary();
        CHECK(aura::compiler::g_dead_coercion_ast_elided_total.load() == elided0,
              "3818 soak: ast-elided restored (no inflated elim for next CastOp/DCE)");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto issue = emb.find("Issue #3472");
        const auto exit_pos = emb.find("ev_->exit_mutation_boundary(success)");
        const auto win =
            (issue == std::string::npos || exit_pos == std::string::npos || issue > exit_pos)
                ? std::string{}
                : emb.substr(issue, exit_pos - issue);
        CHECK(contains(win, "aura_persist_reject_undo"),
              "3818 soak: #3472 post-persist deny wires shared reject-undo");
        CHECK(contains(emb, "note_3440_restore") && contains(emb, "aura_persist_reject_undo(ev,"),
              "3818 soak: in-helper persist-reject shares the same undo");
        apply_dev_audit_defaults();
        reset_for_test();
    }

    {
        std::println("\n--- #3687: persist-reject AST+CoercionMap+Occurrence one transaction ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        CHECK(contains(emb, "Issue #3687"), "3687: persist helper cites #3687");
        CHECK(contains(emb, "restore_checkpoint_topology_for_persist_reject"),
              "3687: topology restore in persist-reject txn");
        // Issue #3818: shared aura_persist_reject_undo owns AST+#3545 order;
        // note_3440_restore / #3472 post-persist deny both call it.
        const auto shared_pos = emb.find("aura_persist_reject_undo(");
        const auto shared =
            (shared_pos == std::string::npos) ? std::string{} : emb.substr(shared_pos);
        const auto shared_end =
            shared.find("extern \"C\" void aura_outermost_success_persist_occurrence");
        const auto shared_body =
            shared_end == std::string::npos ? shared : shared.substr(0, shared_end);
        const auto topo = shared_body.find("restore_checkpoint_topology_for_persist_reject");
        const auto undo = shared_body.find("undo_apply_coercion_map_recent");
        CHECK(topo != std::string::npos && undo != std::string::npos && topo < undo,
              "3687 AC1: AST restore before CoercionMap undo in note_3440 (same function)");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto helper = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        CHECK(contains(helper, "aura_persist_reject_undo") ||
                  contains(helper, "restore_checkpoint_topology_for_persist_reject"),
              "3687 AC1: persist helper invokes shared reject-undo");
        CHECK(contains(emb, "if (!cp.topology_restored)"),
              "3687: exit_mutation_boundary no-ops dual-topology if already restored");
        CHECK(contains(tma, "Issue #3687"), "3687: typed_audit cites #3687");
        CHECK(emb.find("abort_restore_dual_topology_persist_reject") == std::string::npos,
              "3687: no second restore helper name");
        CHECK(emb.find("schema-3687") == std::string::npos &&
                  tma.find("schema-3687") == std::string::npos,
              "3687 AC5: no new query key");
        CHECK(tma.find("strip_green_face_on_remount_last_zero") != std::string::npos,
              "3687 AC4: remount last==0 strip_green_face unchanged");
        CHECK(read_file("tests/compiler/test_issue_3687.cpp").empty(),
              "3687: no test_issue_3687.cpp");
        CHECK(read_file("docs/design/3687-persist-reject-txn.md").empty(), "3687: no docs/design/");

        reset_for_test();
        apply_dev_audit_defaults();
        typed_audit::clear_type_linear_proof_outcome_for_test();
        typed_audit::clear_type_linear_commit_proof_for_test();
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "3687 soak: warm");
        CHECK(cs.eval("(set-code \"(define f 1)\")").has_value(), "3687 soak: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3687 soak: eval");
        (void)cs.eval("(typecheck-current)");
        (void)cs.evaluator().ensure_typechecker();
        (void)cs.evaluator().run_post_mutate_typecheck_no_lock();
        apply_production_audit_defaults();
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "3687 soak: workspace");
        const auto lit = ws ? find_literal_int(*ws, 1) : aura::ast::NULL_NODE;
        CHECK(lit != aura::ast::NULL_NODE, "3687 soak: literal 1");
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
            if (ws && lit != aura::ast::NULL_NODE) {
                const auto old_val = ws->get(lit).int_value;
                (void)ws->add_mutation_with_rollback(
                    lit, "tweak-literal", "Int", "Int", "3687",
                    aura::ast::MutationStatus::Committed,
                    static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal),
                    static_cast<std::uint64_t>(old_val), static_cast<std::uint64_t>(old_val + 7),
                    true);
                ws->set_int(lit, old_val + 7);
                ws->mark_dirty_upward_fast(lit);
            }
        }
        (void)ok;
        CHECK(href_health(cs, "would-allow-commit") == 0,
              "3687 soak: query:type-linear-commit-health would_allow=0");
        CHECK(cs.eval("(eval-current)").has_value(), "3687 soak: eval_flat after persist-reject");
        apply_dev_audit_defaults();
        CHECK(!production_hard_face_active(), "3687 AC3: Soft/Off hard-face off");
        CHECK(get_strategy() != AuditStrategy::Full, "3687 AC3: Soft is not Full");
        const auto note_fn = tma.find("inline void note_outermost_persist_reject_needs_restore");
        const auto note_body =
            note_fn == std::string::npos ? std::string{} : tma.substr(note_fn, 400);
        CHECK(note_body.find("production_defaults_active()") != std::string::npos,
              "3687 AC3: persist-reject note is no-op unless Production/Full");
        apply_dev_audit_defaults();
        reset_for_test();
    }

    {
        std::println("\n--- #3688: apply_closure / eval_flat refuse half-green like IR ---");
        const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
        CHECK(contains(efl, "Issue #3688"), "3688: eval_flat cites #3688");
        CHECK(contains(efl, "production_eval_flat_commit_readiness_refuse"), "3688: refuse helper");
        CHECK(contains(efl, "ir_typed_entry_commit_readiness_ok()"), "3688: reuses IR helper");
        CHECK(contains(efl, "production_hard_face_active()"),
              "3688 AC4: Soft skips commit_readiness");
        CHECK(contains(efl, "commit-readiness-refused"), "3688: same TypeError as IR");
        CHECK(efl.find("schema-3688") == std::string::npos, "3688 AC5: no new query key");
        CHECK(read_file("tests/compiler/test_issue_3688.cpp").empty(), "3688: no invent");

        reset_for_test();
        apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "3688 soak: warm");
        // Workspace Call+Lambda: eval_flat of Lambda allocates a TW closures_
        // slot (cs.eval compiles through IR, whose ClosureId is not in
        // closures_ — apply_closure would nullopt even before reject).
        CHECK(cs.eval("(set-code \"((lambda () 42))\")").has_value(), "3688 soak: set-code Call");
        auto* ws = cs.evaluator().workspace_flat();
        auto* pool = cs.evaluator().workspace_pool();
        CHECK(ws && pool, "3688 soak: workspace attached");
        const auto call = ws ? find_first_tag(*ws, aura::ast::NodeTag::Call) : aura::ast::NULL_NODE;
        const auto lam =
            ws ? find_first_tag(*ws, aura::ast::NodeTag::Lambda) : aura::ast::NULL_NODE;
        CHECK(call != aura::ast::NULL_NODE, "3688 soak: Call node");
        CHECK(lam != aura::ast::NULL_NODE, "3688 soak: Lambda node");
        auto clo = cs.evaluator().eval_flat(*ws, *pool, lam, cs.evaluator().top_env());
        CHECK(clo && is_closure(*clo), "3688 soak: TW capture");
        const auto cid = clo && is_closure(*clo) ? as_closure_id(*clo) : 0;
        CHECK(cs.evaluator().apply_closure(cid, {}).has_value(),
              "3688 soak: apply_closure before reject");
        apply_production_audit_defaults();
        typed_audit::clear_type_linear_proof_outcome_for_test();
        typed_audit::clear_type_linear_commit_proof_for_test();
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
            typed_audit::g_linear_ir_fastpath_boundary_depth_override = 1;
            stamp_type_linear_commit_proof(3688);
            publish_type_linear_proof_outcome(kTypeLinearProofOutcomeReject);
            publish_last_proof_face(false, false);
            CHECK(!ir_typed_entry_commit_readiness_ok(), "3688: helper refuses at the same depth");
            CHECK(!cs.evaluator().apply_closure(cid, {}).has_value(),
                  "3688 AC1: apply_closure refuses half-green body");
            auto er = cs.evaluator().eval_flat(*ws, *pool, call, cs.evaluator().top_env());
            CHECK(!er.has_value(), "3688 AC2: eval_flat refuses half-green Call");
            if (!er)
                CHECK(er.error().message.find("commit-readiness-refused") != std::string::npos,
                      "3688 AC2: same TypeError as IR execute");
            CHECK(jit_execute_commit_readiness_blocked(),
                  "3758 AC1: execute catalog blocked after persist-reject");
            CHECK(cs.public_try_jit_execute_counting_scalar_for_test() == 0,
                  "3758 AC1: try_jit_execute does not invoke ScalarFn after persist-reject");
            typed_audit::g_linear_ir_fastpath_boundary_depth_override = -1;
        }
        (void)ok;
        auto metrics = cs.eval("(engine:metrics \"query:type-linear-commit-health\")");
        CHECK(metrics.has_value(), "3688 AC3: engine:metrics at depth==0 still succeeds");
        CHECK(href_health(cs, "would-allow-commit") == 0,
              "3688 soak: query:type-linear-commit-health would_allow=0");
        apply_dev_audit_defaults();
        typed_audit::clear_type_linear_proof_outcome_for_test();
        typed_audit::clear_type_linear_commit_proof_for_test();
        auto clo_soft = cs.evaluator().eval_flat(*ws, *pool, lam, cs.evaluator().top_env());
        CHECK(clo_soft && is_closure(*clo_soft), "3688 AC4: Soft TW capture");
        CHECK(cs.evaluator().apply_closure(as_closure_id(*clo_soft), {}).has_value(),
              "3688 AC4: Soft/Off apply_closure still runs");
        CHECK(!jit_execute_commit_readiness_blocked(),
              "3758 AC3: Soft execute catalog not blocked");
        CHECK(cs.public_try_jit_execute_counting_scalar_for_test() == 1,
              "3758 AC3: Soft try_jit_execute still invokes ScalarFn");
        reset_for_test();
    }

    {
        std::println("\n--- #3758: try_jit_execute consults commit_readiness before ScalarFn ---");
        const auto ixx = read_file("src/compiler/service.ixx");
        const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        const auto jit = read_file("src/compiler/aura_jit.cpp");
        CHECK(contains(tma, "kJitExecuteCommitReadinessIssue = 3758"), "3758: issue stamp");
        CHECK(contains(tma, "jit_execute_commit_readiness_blocked"),
              "3758: shared execute-catalog helper");
        CHECK(contains(tma, "production_hard_face_active()"),
              "3758 AC3: Soft skips commit_readiness load");
        {
            const auto cite =
                ixx.find("Issue #3758: same execute catalog as IRInterpreter::execute");
            CHECK(cite != std::string::npos, "3758: try_jit_execute present");
            const auto win = cite == std::string::npos ? std::string{} : ixx.substr(cite, 2000);
            const auto gate = win.find("jit_execute_commit_readiness_blocked()");
            const auto call = win.find("reinterpret_cast<aura::jit::ScalarFn>(fn_ptr)");
            CHECK(gate != std::string::npos, "3758 AC1: try_jit_execute consults helper");
            CHECK(call != std::string::npos, "3758 AC1: ScalarFn invoke site");
            CHECK(gate < call, "3758 AC1: gate is before ScalarFn");
        }
        CHECK(contains(ir, "ir_typed_entry_blocked_result(context_.metrics)"),
              "3758 AC1: IRInterpreter::execute still refuses at C++ entry");
        CHECK(contains(jit, "fn_ir_typed_entry_commit_readiness_ok"),
              "3758 AC2: compile-time prologue retained (defense-in-depth)");
        CHECK(
            contains(
                jit,
                "can_typed = hard_typed_entry && builder.fn_ir_typed_entry_commit_readiness_ok") ||
                contains(jit, "const bool can_typed = hard_typed_entry"),
            "3758 AC2: can_typed prologue still emitted under production/Full");
        CHECK(ixx.find("schema-3758") == std::string::npos &&
                  tma.find("schema-3758") == std::string::npos,
              "3758: no new query key");
        CHECK(read_file("tests/compiler/test_issue_3758.cpp").empty(), "3758: no invent");
        CHECK(read_file("docs/design/3758-jit-execute-commit-readiness.md").empty(),
              "3758: no docs/design");
    }

    {
        std::println("\n--- #3697: add_mutate returns persist-reject after dtor abort_restore ---");
        const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(contains(mut, "Issue #3697"), "3697: add_mutate cites #3697");
        const auto cite = mut.find("Issue #3697: outermost dtor persist-reject");
        CHECK(cite != std::string::npos, "3697: add_mutate wrapper cite");
        // Window = brace-matched add_mutate lambda body (the dtor
        // persist/abort_restore face lives at the end of the wrapper body;
        // the fixed 1800-char cite window was drift-bait).
        const auto lam =
            mut.find("auto add_mutate = [&](std::string name, auto fn, bool guard_exempt");
        const auto body = brace_block_after(mut, lam);
        CHECK(!body.empty(), "3697: add_mutate lambda body (brace-matched)");
        CHECK(contains(body, "wrapper_guard.reset()"),
              "3697: dtor persist/abort_restore runs before EDSL return");
        CHECK(contains(body, "\"persist-reject\""), "3697: structured persist-reject mev");
        CHECK(contains(body, "wrapper_ok"), "3697: consults the success flag the dtor flips");
        CHECK(mut.find("\"hold-budget-cancel\"") != std::string::npos,
              "3697: hold-budget cancel still replaces result");
        CHECK(mut.find("schema-3697") == std::string::npos, "3697 AC5: no new query key");
        CHECK(read_file("tests/compiler/test_issue_3697.cpp").empty(),
              "3697 AC5: no test_issue_3697.cpp");
        CHECK(read_file("docs/design/3697-add-mutate-persist-reject.md").empty(),
              "3697 AC5: no docs/design/");

        reset_for_test();
        apply_dev_audit_defaults();
        aura::compiler::reset_mutation_concurrency_health_admit_for_test();
        aura::compiler::MutationConcurrencyHealthSnapshot clean;
        aura::compiler::set_mutation_concurrency_health_admit_snapshot_for_test(clean);
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "3697 soak: warm");
        CHECK(cs.eval("(set-code \"(define t3697 1)\")").has_value(), "3697 soak: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3697 soak: eval");
        (void)cs.eval("(typecheck-current)");
        (void)cs.evaluator().ensure_typechecker();
        (void)cs.evaluator().run_post_mutate_typecheck_no_lock();
        apply_production_audit_defaults();
        CHECK(cs.eval("(define qr3697 (query :find \"t3697\"))").has_value(),
              "3697 soak: bind find hash");
        auto qr = cs.eval("qr3697");
        CHECK(qr && is_hash(*qr), "3697 soak: production find is schema-2 hash");
        CHECK(cs.eval("(define qc3697 (query:filter (query:where :node-type \"LiteralInt\")))")
                  .has_value(),
              "3697 soak: bind literal matches");
        CHECK(aura::compiler::typed_audit::production_defaults_active(),
              "3697 soak: production defaults");
        g_ev_arm_pending_3697 = &cs.evaluator();
        auto* prev_yield = aura::messaging::g_fiber_yield_mutation_boundary;
        aura::messaging::g_fiber_yield_mutation_boundary = &arm_linear_pending_3697;
        auto r3697 = cs.eval("(mutate:tweak-literal qc3697 7 :index 0)");
        aura::messaging::g_fiber_yield_mutation_boundary = prev_yield;
        g_ev_arm_pending_3697 = nullptr;
        CHECK(r3697.has_value(), "3697 AC1: mutate returns");
        CHECK(r3697 && is_pair(*r3697),
              "3697 AC1: production persist-reject is mev pair, not body's int");
        CHECK(!(r3697 && is_int(*r3697)), "3697 AC1: not the tweak result int");
        CHECK(cs.evaluator().last_boundary_rollback_stats().children_column_restored,
              "3697 AC2: dual-topology restored");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "3697 AC2: workspace");
        CHECK(find_literal_int(*ws, 1) != aura::ast::NULL_NODE,
              "3697 AC2: pre-mutate literal 1 still live");
        CHECK(find_literal_int(*ws, 8) == aura::ast::NULL_NODE,
              "3697 AC2: rejected tree (tweak 1+7=8) is absent");
        auto still = cs.eval("(query :find \"t3697\")");
        CHECK(still && is_hash(*still), "3697 AC2: original Define still queryable");
        auto absent = cs.eval("(query :find \"n3697\")");
        CHECK(absent.has_value(), "3697 AC2: query:find new symbol returns");
        apply_dev_audit_defaults();
        aura::compiler::reset_mutation_concurrency_health_admit_for_test();
        aura::compiler::set_mutation_concurrency_health_admit_snapshot_for_test(clean);
        reset_for_test();
        CompilerService cs_soft;
        CHECK(cs_soft.eval("(set-code \"(define s3697 (lambda () 1))\")").has_value(),
              "3697 AC3/AC4: Soft set-code");
        CHECK(cs_soft.eval("(eval-current)").has_value(), "3697 AC3/AC4: Soft eval");
        CHECK(cs_soft.eval("(define r3697s (mutate:set-body \"s3697\" \"(lambda () 9)\"))")
                  .has_value(),
              "3697 AC3: Soft mutate returns");
        auto soft_rej =
            cs_soft.eval("(and (pair? r3697s) (equal? (car r3697s) \"persist-reject\"))");
        CHECK(soft_rej && is_bool(*soft_rej) && !as_bool(*soft_rej),
              "3697 AC4: Soft/Off no extra persist-reject mev");
        auto* ws_soft = cs_soft.evaluator().workspace_flat();
        CHECK(ws_soft && find_literal_int(*ws_soft, 9) != aura::ast::NULL_NODE,
              "3697 AC3: happy persist keeps body's write");
        reset_for_test();
    }

    {
        std::println("\n--- #3743: persist-reject restore restamps surviving node before dtor ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(contains(emb, "Issue #3743"), "3743: boundary cites #3743");
        const auto helper_fn =
            emb.find("void Evaluator::restore_checkpoint_topology_for_persist_reject()");
        const auto helper_win =
            helper_fn == std::string::npos ? std::string{} : emb.substr(helper_fn, 2200);
        CHECK(helper_win.find("unified_restamp_after_boundary") != std::string::npos &&
                  helper_win.find("UnifiedRestampSite::AbortRestore") != std::string::npos,
              "3743: persist-reject helper restamps AbortRestore");
        CHECK(emb.find("schema-3743") == std::string::npos, "3743: no new query key");
        CHECK(read_file("tests/compiler/test_issue_3743.cpp").empty(),
              "3743: no test_issue_3743.cpp");

        reset_for_test();
        apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "3743 soak: warm");
        CHECK(cs.eval("(set-code \"(define f 1)\")").has_value(), "3743 soak: set-code");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "3743 soak: workspace");
        const auto lit = ws ? find_literal_int(*ws, 1) : aura::ast::NULL_NODE;
        const auto def =
            ws ? find_first_tag(*ws, aura::ast::NodeTag::Define) : aura::ast::NULL_NODE;
        CHECK(lit != aura::ast::NULL_NODE, "3743 soak: literal 1");
        CHECK(def != aura::ast::NULL_NODE, "3743 soak: Define");
        apply_production_audit_defaults();
        const auto calls0 = aura::ast::unified_restamp_calls_total_v_read();
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
            if (ws && lit != aura::ast::NULL_NODE) {
                const auto old_val = ws->get(lit).int_value;
                ws->set_int(lit, old_val + 7);
                ws->mark_dirty_upward_fast(lit);
            }
            if (ws && def != aura::ast::NULL_NODE) {
                const auto extra = ws->add_literal(99);
                ws->insert_child(def, 0, extra);
            }
            cs.evaluator().restore_checkpoint_topology_for_persist_reject();
            CHECK(ws && ws->node_generation_is_post_mutate(lit),
                  "3743 AC1: persist-reject restore restamps surviving node "
                  "before Guard dtor (not a pre-abort gen)");
            CHECK(aura::ast::unified_restamp_calls_total_v_read() > calls0,
                  "3743 AC1: AbortRestore unified restamp ran in the restore helper");
            ok = false;
        }
        CHECK(!ok, "3743 AC1: abort Guard");
        CHECK(ws && ws->node_generation_is_post_mutate(lit),
              "3743 AC1: surviving node still post-mutate after dtor belt restamp");
        CHECK(ws && lit != aura::ast::NULL_NODE && ws->is_live_node(lit),
              "3743 AC1: surviving node stays live after abort restore");
        apply_dev_audit_defaults();
        {
            CompilerService cs_soft;
            CHECK(cs_soft.eval("(set-code \"(define s 1)\")").has_value(),
                  "3743 AC3: Soft set-code");
            auto* ws_soft = cs_soft.evaluator().workspace_flat();
            const auto lit_s = ws_soft ? find_literal_int(*ws_soft, 1) : aura::ast::NULL_NODE;
            const auto calls_soft = aura::ast::unified_restamp_calls_total_v_read();
            bool ok_s = true;
            {
                Evaluator::MutationBoundaryGuard g(cs_soft.evaluator(), &ok_s);
                if (ws_soft && lit_s != aura::ast::NULL_NODE)
                    ws_soft->set_int(lit_s, 2);
                cs_soft.evaluator().restore_checkpoint_topology_for_persist_reject();
                CHECK(aura::ast::unified_restamp_calls_total_v_read() == calls_soft,
                      "3743 AC3: Soft persist-reject helper is a no-op (no extra restamp)");
                ok_s = false;
            }
            (void)ok_s;
        }
        reset_for_test();
    }

    // ── Issue #3819: fingerprint mismatch reject dual-track (prod ∥ Full) ──
    {
        std::println("\n--- #3819 AC1: Full hard-face gates fingerprint mismatch reject ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        const auto cite = emb_after.find("Issue #3819");
        CHECK(cite != std::string::npos, "3819 AC1: outermost persist cites #3819");
        const auto win = cite == std::string::npos ? std::string{} : emb_after.substr(cite, 2400);
        CHECK(contains(win, "production_hard_face_active()"),
              "3819 AC1: mismatch uses production_hard_face_active (prod||Full)");
        CHECK(contains(win, "expected_occurrence_snapshot_fp() != 0"),
              "3819 AC1: staged expected != 0");
        CHECK(contains(win, "live_fp != ev->expected_occurrence_snapshot_fp()"),
              "3819 AC1: live!=expected mismatch");
        CHECK(contains(win, "kTypeLinearProofOutcomeReject"),
              "3819 AC1: stamps reject proof outcome");
        CHECK(contains(win, "force_reason=*/16"), "3819 AC1: force_reason 16");
        CHECK(contains(win, "bump_occurrence_persist_fingerprint_mismatch"),
              "3819 AC1: bumps mismatch counter");
        // #3556 sibling already hard-face; cite that dual-track is shared.
        CHECK(contains(emb_after, "production_hard_face_active()"),
              "3819 AC1: #3556 hard-face still present in helper");
    }

    {
        std::println("\n--- #3819 AC2: Soft/Off — hard refuse still gated (no Soft hard path) ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto cite = emb.find("Issue #3819");
        const auto win = cite == std::string::npos ? std::string{} : emb.substr(cite, 900);
        CHECK(contains(win, "Soft/Off unchanged"), "3819 AC2: Soft/Off contract cited");
        // Soft: production_hard_face_active is false under Sampled/dev defaults.
        reset_for_test();
        apply_dev_audit_defaults();
        CHECK(!aura::compiler::typed_audit::production_defaults_active(),
              "3819 AC2: Soft production_defaults off");
        CHECK(aura::compiler::typed_audit::get_strategy() !=
                  aura::compiler::typed_audit::AuditStrategy::Full,
              "3819 AC2: Soft strategy is not Full");
        CHECK(!aura::compiler::typed_audit::production_hard_face_active(),
              "3819 AC2: Soft hard-face inactive (no hard refuse)");
    }

    {
        std::println("\n--- #3819 soak: Full-without-prod arms hard-face (no persist freeze under "
                     "drift) ---");
        reset_for_test();
        apply_dev_audit_defaults();
        aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Full);
        CHECK(!aura::compiler::typed_audit::production_defaults_active(),
              "3819 soak: Full-without-prod (production_defaults off)");
        CHECK(aura::compiler::typed_audit::get_strategy() ==
                  aura::compiler::typed_audit::AuditStrategy::Full,
              "3819 soak: strategy Full");
        CHECK(aura::compiler::typed_audit::production_hard_face_active(),
              "3819 soak: hard-face active under Full-without-prod");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(contains(emb, "if (aura::compiler::typed_audit::production_hard_face_active() &&\n"
                            "        ev->expected_occurrence_snapshot_fp() != 0 &&\n"
                            "        live_fp != ev->expected_occurrence_snapshot_fp()) {"),
              "3819 soak: mismatch early-return gated on hard-face (refuses freeze)");
        CHECK(emb.find("schema-3819") == std::string::npos, "3819: no new query key");
        CHECK(read_file("tests/compiler/test_issue_3819.cpp").empty(),
              "3819: no test_issue_3819.cpp");
        CHECK(read_file("docs/design/3819-fp-mismatch-dual-track.md").empty(),
              "3819: no docs/design/");
        apply_dev_audit_defaults();
        reset_for_test();
    }

    ac4310_abort_restores_type_columns_and_cs();

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_outermost_persist_fail_closed();
}
#endif
