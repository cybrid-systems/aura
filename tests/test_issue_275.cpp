// @category: unit
// @reason: tests pure mutation helpers without CompilerService
// test_issue_275.cpp — Issue #275: pure mutation / rollback module.


#include "test_harness.hpp"

using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.mutation;
import aura.core.ast;

namespace aura_issue_275_detail {

bool test_rollback_capable_concept() {
    std::println("\n--- AC1: RollbackCapable concept ---");
    CHECK(static_cast<bool>(aura::ast::RollbackCapable<aura::ast::MutationRecord>),
          "MutationRecord satisfies RollbackCapable");
    return true;
}

bool test_create_mutation_record_pure() {
    std::println("\n--- AC2: create_mutation_record pure function ---");
    aura::ast::mutation::MutationRecordParams p{
        .mutation_id = 7,
        .target_node = 3,
        .operator_name = "replace-value",
        .old_type_str = "",
        .new_type_str = "",
        .summary = "lit=42",
        .field_offset = static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal),
        .old_value = 1,
        .new_value = 42,
        .has_rollback_data = true,
    };
    auto rec = aura::ast::mutation::create_mutation_record(p);
    CHECK(rec.mutation_id == 7, "mutation_id preserved");
    CHECK(rec.target_node == 3, "target_node preserved");
    CHECK(rec.operator_name == "replace-value", "operator_name preserved");
    CHECK(rec.has_rollback_data, "rollback flag preserved");
    CHECK(rec.timestamp_ms > 0, "timestamp auto-filled");
    CHECK(rec.invariant_status == aura::ast::InvariantStatus::NotChecked,
          "default invariant status");
    return true;
}

bool test_classify_and_validate_rollback() {
    std::println("\n--- AC3: classify + validate rollback (pure) ---");
    aura::ast::MutationRecord rec = aura::ast::mutation::create_mutation_record({
        .mutation_id = 1,
        .target_node = 0,
        .operator_name = "rename-symbol",
        .old_type_str = "",
        .new_type_str = "",
        .summary = "",
        .field_offset = static_cast<std::uint32_t>(aura::ast::MutationSoAField::SymId),
        .old_value = 10,
        .new_value = 11,
        .has_rollback_data = true,
    });
    auto valid = aura::ast::mutation::validate_rollback_record(rec, 16);
    CHECK(valid.has_value(), "committed scalar record validates");
    auto kind = aura::ast::mutation::classify_rollback(rec);
    CHECK(kind.has_value(), "classify succeeds");
    CHECK(*kind == aura::ast::RollbackKind::ScalarSymId, "classifies SymId rollback");

    rec.status = aura::ast::MutationStatus::RolledBack;
    auto invalid = aura::ast::mutation::validate_rollback_record(rec, 16);
    CHECK(!invalid.has_value(), "rolled-back record rejected");
    CHECK(invalid.error() == aura::ast::MutationError::NotCommitted, "NotCommitted error");
    return true;
}

bool test_wire_roundtrip_pure() {
    std::println("\n--- AC4: wire serialize roundtrip (pure) ---");
    aura::ast::MutationRecord original = aura::ast::mutation::create_subtree_mutation_record({
        .mutation_id = 99,
        .target_node = 5,
        .parent_id = 2,
        .child_idx = 1,
        .old_subtree_source = "(+ 1 2)",
        .operator_name = "replace-subtree",
        .summary = "subtree swap",
    });
    std::vector<char> buf;
    aura::ast::mutation::wire_write_mutation_record(buf, original);
    std::size_t pos = 0;
    auto decoded = aura::ast::mutation::wire_read_mutation_record(buf, pos);
    CHECK(decoded.mutation_id == original.mutation_id, "wire id roundtrip");
    CHECK(decoded.parent_id == original.parent_id, "wire parent roundtrip");
    CHECK(decoded.old_subtree_source == original.old_subtree_source,
          "wire subtree source roundtrip");
    CHECK(decoded.has_subtree_rollback, "wire subtree flag roundtrip");
    return true;
}

bool test_flat_ast_delegates_to_pure_rollback() {
    std::println("\n--- AC5: FlatAST rollback delegates to pure helpers ---");
    aura::ast::FlatAST flat;
    auto sym_a = aura::ast::INVALID_SYM;
    auto sym_b = 42u;
    auto id = flat.add_variable(sym_a);
    auto old_sym = flat.sym_id(id);
    auto mid = flat.add_mutation_with_rollback(
        id, "rename-symbol", "", "", "rename", aura::ast::MutationStatus::Committed,
        static_cast<std::uint32_t>(aura::ast::MutationSoAField::SymId),
        static_cast<std::uint64_t>(old_sym), static_cast<std::uint64_t>(sym_b), true);
    flat.sym_id(id) = sym_b;
    auto result = flat.try_rollback_record(flat.all_mutations().back());
    CHECK(result.has_value(), "try_rollback_record succeeds");
    CHECK(flat.sym_id(id) == old_sym, "sym_id restored via expected rollback");
    CHECK(flat.all_mutations().back().status == aura::ast::MutationStatus::RolledBack,
          "record marked rolled back");
    CHECK(!flat.is_valid(id), "NodeId stale after rollback generation bump");
    CHECK(mid >= 1, "mutation id assigned");
    return true;
}

int run_tests() {
    std::println("Issue #275 — pure mutation / rollback module\n");
    bool ok = true;
    ok = aura_issue_275_detail::test_rollback_capable_concept() && ok;
    ok = aura_issue_275_detail::test_create_mutation_record_pure() && ok;
    ok = aura_issue_275_detail::test_classify_and_validate_rollback() && ok;
    ok = aura_issue_275_detail::test_wire_roundtrip_pure() && ok;
    ok = aura_issue_275_detail::test_flat_ast_delegates_to_pure_rollback() && ok;
    std::println("\n{} passed, {} failed", g_passed, g_failed);
    return ok ? 0 : 1;
}

} // namespace aura_issue_275_detail

int aura_issue_275_run() {
    return aura_issue_275_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_275_run();
}
#endif