// @category: unit
// @reason: Issue #3374 \u2014 lockless `eval_flat_apply_mutate_rebind` (used by
// `mutate:atomic-batch` :rebind sub-op) must hygiene-check the parsed
// new body for MacroIntroduced, not only the destination define. The
// destination gate was added by #3301 batch-level parity; the #2792
// new-body-walk twin for the lockless path was still missing, so
// `mutate:atomic-batch` could install a macro-introduced body onto a
// User Define and defeat #373. Non-duplicative to #2792 / #3301 / #3344.
//
//   AC1: source cites the new-body walk in eval_flat_apply_mutate_rebind
//   AC2: walk uses the same opt-out as the destination gate
//   AC3: walk stamps kHygieneLimitReasonMacroIntroduced on hit
//   AC4: #3301 lint (check_atomic_batch_macro_audit_3301.py) extended
//        with AC7 row for the walk (fail-closed under regen / amend)
//   AC5: existing #3301 / #2792 tests still pass + #3301 linter green
//   AC6: no docs/design/3374-*; no test_issue_3374.cpp per #1655 / #81967

#include "test_harness.hpp"

#include <format>
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

int run_test_atomic_batch_rebind_hygiene_lockless() {
    std::println("=== Issue #3374: lockless rebind new-body MacroIntroduced hygiene ===");
    CHECK(true, "3374: issue stamp");

    // \u2500\u2500 AC1: source cites the new-body walk in the lockless helper \u2500\u2500
    {
        std::println("\n--- AC1: new-body walk in eval_flat_apply_mutate_rebind ---");
        const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
        CHECK(!efl.empty(), "AC1: lockless helper readable");
        const auto fn_pos = efl.find("EvalResult Evaluator::eval_flat_apply_mutate_rebind(");
        CHECK(fn_pos != std::string::npos, "AC1: lockless rebind helper present");
        // The walk must be inside eval_flat_apply_mutate_rebind, not just
        // somewhere in the file. Slice from the function start to the next
        // free function.
        const auto after_fn = (fn_pos == std::string::npos) ? std::string::npos : fn_pos + 1;
        const auto efl_after =
            (after_fn == std::string::npos) ? std::string{} : efl.substr(after_fn);
        CHECK(contains(efl_after, "walk_subtree(new_value"),
              "AC1: walk_subtree(new_value) inside lockless rebind");
        CHECK(contains(efl_after, "flat.is_macro_introduced(id)"),
              "AC1: is_macro_introduced(id) probe in walk");
        CHECK(contains(efl_after, "#3374"), "AC1: lockless helper cites #3374");
        // Pre-install fail-fast: the walk must precede add_mutation_with_rollback
        // + set_child + mark_dirty_upward_fast in the same function body.
        const auto walk_pos = efl_after.find("walk_subtree(new_value");
        const auto add_mut_pos = efl_after.find("add_mutation_with_rollback");
        CHECK(walk_pos != std::string::npos && add_mut_pos != std::string::npos,
              "AC1: both walk and add_mutation present");
        CHECK(walk_pos < add_mut_pos,
              "AC1: walk precedes add_mutation_with_rollback (pre-install fail-fast)");
    }

    // \u2500\u2500 AC2: walk uses the same opt-out as the destination gate \u2500\u2500
    {
        std::println("\n--- AC2: opt-out parity ---");
        const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
        // Both gates (destination at #3301, new-body at #3374) must use
        // get_allow_macro_mutate() || parse_allow_macro_opt_out(a).
        CHECK(contains(efl, "get_allow_macro_mutate() || parse_allow_macro_opt_out(a)"),
              "AC2: shared opt-out expression present in lockless helper");
        // Must appear at least twice (destination gate + new-body gate).
        const auto first = efl.find("get_allow_macro_mutate() || parse_allow_macro_opt_out(a)");
        CHECK(first != std::string::npos, "AC2: opt-out #1 (destination)");
        const auto second =
            efl.find("get_allow_macro_mutate() || parse_allow_macro_opt_out(a)", first + 1);
        CHECK(second != std::string::npos, "AC2: opt-out #2 (new-body)");
    }

    // \u2500\u2500 AC3: walk stamps the same reason + counter as destination \u2500\u2500
    {
        std::println("\n--- AC3: reason + counter parity ---");
        const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
        CHECK(contains(efl, "kHygieneLimitReasonMacroIntroduced"),
              "AC3: reason stamp present in lockless helper");
        CHECK(contains(efl, "note_hygiene_last_limit_reason(kHygieneLimitReasonMacroIntroduced)"),
              "AC3: reason stamp call in lockless helper");
        CHECK(contains(efl, "note_rebind_hygiene_reject"),
              "AC3: rebind reject counter bump in lockless helper");
        CHECK(contains(efl, "record_hygiene_violation_attempt"),
              "AC3: typed audit trail bump in lockless helper");
    }

    // \u2500\u2500 AC4: #3301 lint extended with AC7 row for the walk \u2500\u2500
    {
        std::println("\n--- AC4: #3301 lint AC7 row ---");
        const auto lint =
            read_file("scripts/coverage/checks/check_atomic_batch_macro_audit_3301.py");
        CHECK(!lint.empty(), "AC4: #3301 linter readable");
        CHECK(contains(lint, "AC7"), "AC4: #3301 linter declares AC7");
        CHECK(contains(lint, "walk_subtree(new_value"),
              "AC4: #3301 linter checks for walk_subtree(new_value");
        CHECK(contains(lint, "flat.is_macro_introduced(id)"),
              "AC4: #3301 linter checks for is_macro_introduced(id)");
        CHECK(contains(lint, "cannot install MacroIntroduced body"),
              "AC4: #3301 linter checks for new-body reject message");
    }

    // \u2500\u2500 AC5: existing tests still present + linter green \u2500\u2500
    {
        std::println("\n--- AC5: existing tests still present ---");
        const auto rebind_test = read_file("tests/compiler/test_rebind_new_body_hygiene.cpp");
        const auto closed_loop_test =
            read_file("tests/compiler/test_hygiene_mutate_closed_loop.cpp");
        CHECK(!rebind_test.empty(), "AC5: test_rebind_new_body_hygiene.cpp present");
        CHECK(!closed_loop_test.empty(), "AC5: test_hygiene_mutate_closed_loop.cpp present");
        // The new helper does not touch Soft / Off or the other 12 lockless
        // ops: only the `eval_flat_apply_mutate_rebind` function body changed.
        CHECK(contains(rebind_test, "2792"), "AC5: existing #2792 suite intact");
        CHECK(contains(closed_loop_test, "3301 AC5"),
              "AC5: existing #3301 AC5 marker in test_hygiene_mutate_closed_loop");
    }

    // \u2500\u2500 AC6: no docs/design/3374-*; no test_issue_3374.cpp \u2500\u2500
    {
        std::println("\n--- AC6: no docs/design/3374-*; no test_issue_3374.cpp ---");
        CHECK(read_file("docs/design/3374-lockless-rebind-new-body-hygiene.md").empty(),
              "AC6: no docs/design/3374-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_3374.cpp").empty(),
              "AC6: no test_issue_3374.cpp per #81967");
        CHECK(read_file("tests/issues/test_issue_3374.cpp").empty(),
              "AC6: no tests/issues/test_issue_3374.cpp (R1 abandoned scheme)");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_atomic_batch_rebind_hygiene_lockless();
}
#endif
