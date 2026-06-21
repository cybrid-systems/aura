// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_274.cpp — Issue #274: MutationVisitor concept +
// run_mutation_pipeline + StableNodeRef helpers.

#include <print>
#include <string>
#include <vector>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;

namespace aura_issue_274_detail {

bool test_mutation_visitor_concept() {
    std::println("\n--- AC1: MutationVisitor concept satisfaction ---");
    CHECK(static_cast<bool>(aura::ast::MutationVisitor<aura::ast::MutationCountVisitor>),
          "MutationCountVisitor satisfies MutationVisitor");
    CHECK(static_cast<bool>(aura::ast::MutationVisitor<aura::ast::MutationTargetValidityVisitor>),
          "MutationTargetValidityVisitor satisfies MutationVisitor");
    return true;
}

bool test_pure_fn_wrap() {
    std::println("\n--- AC2: MutationFnWrap adapts pure functions ---");
    std::size_t seen = 0;
    auto count_fn = [&](aura::ast::FlatAST&, const aura::ast::MutationRecord&) { ++seen; };
    aura::ast::MutationFnWrap wrap(count_fn);
    CHECK(static_cast<bool>(aura::ast::MutationVisitor<decltype(wrap)>),
          "MutationFnWrap satisfies MutationVisitor");
    aura::ast::FlatAST flat;
    auto id = flat.add_literal(1);
    flat.add_mutation_with_rollback(id, "replace-value", "", "", "lit=2",
                                    aura::ast::MutationStatus::Committed,
                                    static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal),
                                    1, 2, true);
    wrap.visit_mutation(flat, flat.all_mutations().back());
    CHECK(seen == 1, "pure fn wrap invoked once");
    return true;
}

bool test_run_mutation_pipeline() {
    std::println("\n--- AC3: run_mutation_pipeline folds over log ---");
    aura::ast::FlatAST flat;
    auto a = flat.add_literal(1);
    auto b = flat.add_literal(2);
    flat.add_mutation_with_rollback(a, "replace-value", "", "", "a=10",
                                    aura::ast::MutationStatus::Committed,
                                    static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal),
                                    1, 10, true);
    flat.add_mutation_with_rollback(b, "replace-value", "", "", "b=20",
                                    aura::ast::MutationStatus::Committed,
                                    static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal),
                                    2, 20, true);

    aura::ast::MutationCountVisitor counter;
    aura::ast::MutationTargetValidityVisitor validity;
    bool ok = aura::ast::run_mutation_pipeline(flat, counter, validity);
    CHECK(ok, "pipeline returned true");
    CHECK(counter.total_count() == 2, "counter saw both records");
    CHECK(counter.committed_count() == 2, "both records committed");
    CHECK(!validity.has_error(), "targets still valid after field mutations");
    return true;
}

bool test_stable_ref_helpers() {
    std::println("\n--- AC4: StableNodeRef helpers ---");
    aura::ast::FlatAST flat;
    auto id = flat.add_variable(0);
    aura::ast::MutationRecord rec;
    rec.target_node = id;
    rec.parent_id = aura::ast::NULL_NODE;
    rec.status = aura::ast::MutationStatus::Committed;

    auto target_ref = aura::ast::mutation_target_ref(flat, rec);
    CHECK(flat.is_valid(target_ref), "mutation_target_ref is valid");
    CHECK(aura::ast::is_mutation_target_valid(flat, rec), "is_mutation_target_valid true");

    flat.bump_generation();
    CHECK(!flat.is_valid(target_ref), "captured ref stale after bump");
    CHECK(!aura::ast::is_mutation_target_valid(flat, rec), "helper reports stale target");
    return true;
}

bool test_pipeline_short_circuit() {
    std::println("\n--- AC5: pipeline short-circuits on visitor error ---");
    aura::ast::FlatAST flat;
    auto id = flat.add_literal(3);
    flat.add_mutation_with_rollback(id, "replace-value", "", "", "lit=9",
                                    aura::ast::MutationStatus::Committed,
                                    static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal),
                                    3, 9, true);
    flat.bump_generation(); // target_node in log is now stale

    aura::ast::MutationTargetValidityVisitor validity;
    bool ok = aura::ast::run_mutation_pipeline(flat, validity);
    CHECK(!ok, "pipeline short-circuits when validity visitor errors");
    CHECK(validity.has_error(), "validity visitor flagged stale target");
    return true;
}

} // namespace aura_issue_274_detail

int main() {
    using namespace aura_issue_274_detail;
    std::println("Issue #274 — MutationVisitor concept + pipeline\n");
    bool ok = true;
    ok = test_mutation_visitor_concept() && ok;
    ok = test_pure_fn_wrap() && ok;
    ok = test_run_mutation_pipeline() && ok;
    ok = test_stable_ref_helpers() && ok;
    ok = test_pipeline_short_circuit() && ok;
    std::println("\n{} passed, {} failed", g_passed, g_failed);
    return ok ? 0 : 1;
}