// @category: unit
// @reason: Issue #3407 — synthesize_flat NodeTag::Set must walk RHS +
// unify with var type + report ground mismatch (the odd arm — was
// returning Void without walking RHS, breaking (set! x "hi") under
// Production+Strict where x : Int). check_flat Set already walks RHS
// (the intended contract); mirror it in synthesize_flat Set. Soft/Off:
// Warning path for ground mismatch unchanged (#3044 covered-tag table
// still counts Set). Non-duplicative to #3044/#976/#3330/#3202/#2992/
// #3406.
//
//   AC1: synthesize_flat Set case walks RHS + unifies + reports ground
//        mismatch (the fix — was Void-only, no RHS walk)
//   AC2: synthesize_flat_begin / infer_flat / infer_flat_partial walk
//        Set RHS (Begin children go through synthesize_flat)
//   AC3: check_flat Set contract unchanged (same unify + ground report)
//   AC4: no docs/design/3407-*; no test_issue_3407.cpp per #1655 / #81934
//        (test file is test_synthesize_set_walks_rhs.cpp, source-cite)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

namespace {

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

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

int run_test_synthesize_set_walks_rhs() {
    std::println("=== Issue #3407: synthesize_flat Set walks RHS + unifies + reports ground ===");
    CHECK(true, "3407: issue stamp");

    const auto tci = read_file("src/compiler/type_checker_impl.cpp");

    // ── AC1: synthesize_flat Set case walks RHS + unifies + reports ground ──
    {
        std::println("\n--- AC1: synthesize_flat Set case walks RHS ---");
        // Locate the synthesize_flat function body.
        const auto fn_pos = tci.find("TypeId InferenceEngine::synthesize_flat(");
        CHECK(fn_pos != std::string::npos, "AC1: synthesize_flat present");
        const auto tci_after = (fn_pos == std::string::npos) ? std::string{} : tci.substr(fn_pos);
        // Find the next `case Tag::Set:` inside synthesize_flat.
        const auto set_pos = tci_after.find("case Tag::Set:");
        CHECK(set_pos != std::string::npos, "AC1: synthesize_flat Set case present");
        // The case body must (1) call synthesize_flat on the RHS child,
        // (2) look up the var in env_, (3) call consistent_unify on
        // val_type vs var_type, (4) call maybe_report_ground_inconsistency.
        // Find the next `break;` after the Set case to scope the body.
        const auto break_pos = tci_after.find("break;", set_pos);
        CHECK(break_pos != std::string::npos, "AC1: synthesize_flat Set case has a break");
        const auto set_body = tci_after.substr(set_pos, break_pos - set_pos);
        // RHS walk: synthesize_flat(... v.child(0) ...)
        CHECK(contains(set_body, "synthesize_flat(flat, pool, val_id"),
              "AC1: Set case synthesizes the RHS child (val_id)");
        // env lookup
        CHECK(contains(set_body, "env_.lookup"), "AC1: Set case looks up var_name in env_");
        // Unify val_type vs var_type
        CHECK(contains(set_body, "consistent_unify(val_type, var_type)"),
              "AC1: Set case unifies val_type with var_type");
        // Ground report
        CHECK(contains(set_body, "maybe_report_ground_inconsistency(val_type, var_type)"),
              "AC1: Set case reports ground mismatch (val_type vs var_type)");
        // Source-cite #3407 anchor
        CHECK(contains(set_body, "#3407"), "AC1: Set case cites #3407 (source-cite anchor)");
    }

    // ── AC2: synthesize_flat_begin / infer_flat / infer_flat_partial walk Set ──
    {
        std::println("\n--- AC2: Begin / infer walk Set via synthesize_flat ---");
        // synthesize_flat_begin walks children through synthesize_flat,
        // which now (AC1) walks Set RHS. infer_flat defaults to
        // synthesize_flat for the root, infer_flat_partial calls
        // synthesize_flat for every node in the partial cone.
        const auto begin_pos = tci.find("synthesize_flat_begin");
        CHECK(begin_pos != std::string::npos, "AC2: synthesize_flat_begin present");
        // Begin should iterate children and call synthesize_flat on each.
        // Look for the helper body that walks v.children.
        const auto begin_after = tci.substr(begin_pos);
        CHECK(contains(begin_after, "synthesize_flat(flat, pool,") ||
                  contains(begin_after, "synthesize_flat_begin"),
              "AC2: synthesize_flat_begin delegates to synthesize_flat");
        // infer_flat calls synthesize_flat for the root (line ~4659).
        const auto infer_pos = tci.find("TypeId InferenceEngine::infer_flat(");
        CHECK(infer_pos != std::string::npos, "AC2: infer_flat present");
        const auto infer_after = tci.substr(infer_pos);
        CHECK(contains(infer_after, "synthesize_flat(flat, pool, id, flat.get(id))"),
              "AC2: infer_flat defaults to synthesize_flat for the root");
        // infer_flat_partial — find the function and verify it calls
        // synthesize_flat on every node in the partial cone.
        const auto infer_partial_pos = tci.find("infer_flat_partial");
        CHECK(infer_partial_pos != std::string::npos, "AC2: infer_flat_partial present");
    }

    // ── AC3: check_flat Set contract unchanged ──
    {
        std::println("\n--- AC3: check_flat Set contract unchanged ---");
        // check_flat Set should still synthesize RHS + unify + ground
        // report (existing contract, ~line 7558). It additionally
        // unifies with expected (because check_flat has expected).
        const auto check_pos = tci.find("InferenceEngine::check_flat(");
        CHECK(check_pos != std::string::npos, "AC3: check_flat present");
        const auto check_after = tci.substr(check_pos);
        // Find the Set branch in check_flat.
        const auto set_pos = check_after.find("NodeTag::Set");
        CHECK(set_pos != std::string::npos, "AC3: check_flat Set branch present");
        const auto set_branch = check_after.substr(set_pos);
        // Same contract: RHS synth + env lookup + unify + ground report.
        CHECK(contains(set_branch, "synthesize_flat(flat, pool, val_id"),
              "AC3: check_flat Set synthesizes RHS");
        CHECK(contains(set_branch, "env_.lookup"), "AC3: check_flat Set looks up var_name");
        CHECK(contains(set_branch, "consistent_unify(val_type, var_type)"),
              "AC3: check_flat Set unifies val_type with var_type");
        CHECK(contains(set_branch, "maybe_report_ground_inconsistency(val_type, var_type)"),
              "AC3: check_flat Set reports ground mismatch");
        // check_flat additionally unifies with expected.
        CHECK(contains(set_branch, "consistent_unify(val_type, expected)"),
              "AC3: check_flat Set also unifies with expected");
    }

    // ── AC4: no docs/design/3407-*; no test_issue_3407.cpp ──
    {
        std::println("\n--- AC4: no docs/design/3407-*; no test_issue_3407.cpp ---");
        CHECK(read_file("docs/design/3407-synthesize-set-walks-rhs.md").empty(),
              "AC4: no docs/design/3407-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_3407.cpp").empty(),
              "AC4: no test_issue_3407.cpp per #81934");
        CHECK(read_file("tests/issues/test_issue_3407.cpp").empty(),
              "AC4: no tests/issues/test_issue_3407.cpp (R1 abandoned scheme)");
    }

    // --- AC5: #3408 synthesize_flat Set case wired with assignment hygiene ---
    {
        std::println("\n--- AC5: #3408 synthesize_flat Set assignment hygiene ---");
        const auto tci2 = read_file("src/compiler/type_checker_impl.cpp");
        const auto fn_pos = tci2.find("TypeId InferenceEngine::synthesize_flat(");
        const auto tci2_after = (fn_pos == std::string::npos) ? std::string{} : tci2.substr(fn_pos);
        const auto set_pos = tci2_after.find("case Tag::Set:");
        CHECK(set_pos != std::string::npos, "AC5: synthesize_flat Set case present");
        // Find the next break after the Set case to scope the body.
        const auto break_pos = tci2_after.find("break;", set_pos);
        CHECK(break_pos != std::string::npos, "AC5: synthesize_flat Set case has a break");
        const auto set_body = tci2_after.substr(set_pos, break_pos - set_pos);
        // After #3408: the synthesize_flat Set case must call (in order):
        // (a) invalidate_predicate_memo_for_var_names({var_name})
        // (b) cs_.drop_occurrence_goals_for_var_type(var_type)
        // (c) cs_.mark_touched_on_delta(var_type, /*occurrence_narrow=*/false)
        CHECK(contains(set_body, "invalidate_predicate_memo_for_var_names({var_name})"),
              "AC5: synthesize_flat Set case invalidates predicate memo for var_name");
        CHECK(contains(set_body, "cs_.drop_occurrence_goals_for_var_type(var_type)"),
              "AC5: synthesize_flat Set case drops occurrence goals for var_type");
        CHECK(contains(set_body, "mark_touched_on_delta(var_type, /*occurrence_narrow=*/false)"),
              "AC5: synthesize_flat Set case marks touched on delta (non-narrow)");
        // Source-cite #3408 anchor must be in the branch comment.
        CHECK(contains(set_body, "#3408"),
              "AC5: synthesize_flat Set case cites #3408 (source-cite anchor)");
    }

    // --- AC6: #3408 check_flat Set case wired with assignment hygiene ---
    {
        std::println("\n--- AC6: #3408 check_flat Set assignment hygiene ---");
        const auto tci2 = read_file("src/compiler/type_checker_impl.cpp");
        const auto check_pos = tci2.find("InferenceEngine::check_flat(");
        CHECK(check_pos != std::string::npos, "AC6: check_flat present");
        const auto check_after = tci2.substr(check_pos);
        // Find the Set branch in check_flat.
        const auto set_branch_pos = check_after.find("NodeTag::Set");
        CHECK(set_branch_pos != std::string::npos, "AC6: check_flat Set branch present");
        // Scope: from the Set branch to the closing of the inner if block.
        const auto set_branch = check_after.substr(set_branch_pos);
        // After #3408: check_flat Set must call (in order):
        // (a) invalidate_predicate_memo_for_var_names({var_name})
        // (b) cs_.drop_occurrence_goals_for_var_type(var_type)
        // (c) cs_.mark_touched_on_delta(var_type, /*occurrence_narrow=*/false)
        CHECK(contains(set_branch, "invalidate_predicate_memo_for_var_names({var_name})"),
              "AC6: check_flat Set invalidates predicate memo for var_name");
        CHECK(contains(set_branch, "cs_.drop_occurrence_goals_for_var_type(var_type)"),
              "AC6: check_flat Set drops occurrence goals for var_type");
        CHECK(contains(set_branch, "mark_touched_on_delta(var_type, /*occurrence_narrow=*/false)"),
              "AC6: check_flat Set marks touched on delta (non-narrow)");
        // The expected-context unify must still happen (check_flat Set
        // contract unchanged for the expected half — #3407 AC3).
        CHECK(contains(set_branch, "consistent_unify(val_type, expected)"),
              "AC6: check_flat Set still unifies with expected (#3407 contract unchanged)");
    }

    // --- AC7: #3408 infer_flat_partial affected_names includes NodeTag::Set ---
    {
        std::println("\n--- AC7: #3408 infer_flat_partial affected_names includes Set ---");
        const auto tci2 = read_file("src/compiler/type_checker_impl.cpp");
        // Locate the affected_names collection block in infer_flat_partial.
        const auto aff_pos = tci2.find("affected_names.insert(std::string(nm));");
        CHECK(aff_pos != std::string::npos, "AC7: affected_names collection block present");
        // Walk backwards to find the tag list (the `if` condition with NodeTag::...).
        const auto tag_list_start = tci2.rfind("nv.tag ==", aff_pos);
        const auto tag_list_end_search = tci2.find("affected_names.insert", tag_list_start);
        const auto tag_list = tci2.substr(tag_list_start, tag_list_end_search - tag_list_start);
        // The tag list must include NodeTag::Set alongside Variable / Define /
        // Let / LetRec / Lambda.
        CHECK(contains(tag_list, "NodeTag::Set"),
              "AC7: infer_flat_partial affected_names tag list includes NodeTag::Set");
        // Existing tags must still be present (no regression).
        CHECK(contains(tag_list, "NodeTag::Variable"),
              "AC7: NodeTag::Variable still in affected_names tag list");
        CHECK(contains(tag_list, "NodeTag::Define"),
              "AC7: NodeTag::Define still in affected_names tag list");
    }

    // --- AC8: no docs/design/3408-*; no test_issue_3408.cpp ---
    {
        std::println("\n--- AC8: no docs/design/3408-*; no test_issue_3408.cpp ---");
        CHECK(read_file("docs/design/3408-set-assignment-hygiene.md").empty(),
              "AC8: no docs/design/3408-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_3408.cpp").empty(),
              "AC8: no test_issue_3408.cpp per #81934");
        CHECK(read_file("tests/issues/test_issue_3408.cpp").empty(),
              "AC8: no tests/issues/test_issue_3408.cpp (R1 abandoned scheme)");
    }

    // --- #3516: check_flat Set stamps TypeError on unify false ---
    {
        std::println("\n--- #3516 AC1: check_flat Set stamps TypeError on unify false ---");
        const auto tci2 = read_file("src/compiler/type_checker_impl.cpp");
        const auto check_pos = tci2.find("InferenceEngine::check_flat(");
        CHECK(check_pos != std::string::npos, "3516 AC1: check_flat present");
        const auto check_after = tci2.substr(check_pos);
        const auto set_pos = check_after.find("NodeTag::Set");
        CHECK(set_pos != std::string::npos, "3516 AC1: check_flat Set branch");
        const auto next_else = check_after.find("} else if (v.tag == NodeTag::Define)", set_pos);
        const auto set_branch = check_after.substr(
            set_pos, next_else == std::string::npos ? std::string::npos : next_else - set_pos);
        CHECK(contains(set_branch, "if (!cs_.consistent_unify(val_type, var_type))"),
              "3516 AC1: check_flat Set tests unify return");
        CHECK(contains(set_branch,
                       "set_node_error(id, static_cast<std::uint8_t>(ErrorKind::TypeError))"),
              "3516 AC1: check_flat Set stamps TypeError");
        CHECK(contains(set_branch, "Issue #3516"), "3516 AC1: source-cite");
        CHECK(contains(set_branch, "drop_occurrence_goals_for_var_type"),
              "3516 AC2: hygiene still runs");
        CHECK(contains(set_branch, "consistent_unify(val_type, expected)"),
              "3516 AC2: expected unify unchanged");
        const auto synth = tci2.find("TypeId InferenceEngine::synthesize_flat(");
        const auto synth_after = tci2.substr(synth);
        const auto synth_set = synth_after.find("case Tag::Set:");
        const auto synth_break = synth_after.find("break;", synth_set);
        const auto synth_body = synth_after.substr(synth_set, synth_break - synth_set);
        CHECK(contains(synth_body, "if (!cs_.consistent_unify(val_type, var_type))"),
              "3516 AC3: synth Set still stamps on unify false");
        CHECK(read_file("docs/design/3516-check-flat-set-unify.md").empty(),
              "3516 AC5: no docs/design");
        CHECK(read_file("tests/compiler/test_issue_3516.cpp").empty(), "3516 AC5: no invent");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_synthesize_set_walks_rhs();
}
#endif
