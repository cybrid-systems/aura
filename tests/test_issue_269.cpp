// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_269.cpp — Issue #269: FlatAST wire format v2
// (side-data) in production serialize_soa/deserialize_soa.


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;

namespace aura_issue_269_detail {
#define PRINTLN(msg) std::print( "%s\n", (msg))

bool test_flat_ast_v2_production_roundtrip() {
    PRINTLN("\n--- Test 1: production FlatAST v2 side-data roundtrip ---");

    aura::ast::StringPool pool;
    aura::ast::FlatAST flat;

    auto lit = flat.add_literal(42);
    auto var = flat.add_variable(pool.intern("q"));
    flat.root = lit;

    aura::ast::MutationRecord mr1;
    mr1.mutation_id = 1;
    mr1.timestamp_ms = 1000;
    mr1.target_node = lit;
    mr1.operator_name = "replace-type";
    mr1.old_type_str = "Int";
    mr1.new_type_str = "Float";
    mr1.summary = "type change";
    mr1.status = aura::ast::MutationStatus::Committed;
    mr1.field_offset = 0;
    mr1.old_value = 0;
    mr1.new_value = 0;
    mr1.has_rollback_data = false;
    mr1.parent_id = aura::ast::NULL_NODE;
    mr1.child_idx = 0;
    mr1.old_subtree_source = "";
    mr1.has_subtree_rollback = false;
    mr1.invariant_status = aura::ast::InvariantStatus::NotChecked;

    aura::ast::MutationRecord mr2;
    mr2.mutation_id = 2;
    mr2.timestamp_ms = 2000;
    mr2.target_node = var;
    mr2.operator_name = "replace-value";
    mr2.old_type_str = "Symbol";
    mr2.new_type_str = "Symbol";
    mr2.summary = "rename";
    mr2.status = aura::ast::MutationStatus::RolledBack;
    mr2.field_offset = 4;
    mr2.old_value = 0xAAAA;
    mr2.new_value = 0xBBBB;
    mr2.has_rollback_data = true;
    mr2.invariant_status = aura::ast::InvariantStatus::Ok;
    flat.all_mutations().push_back(mr1);
    flat.all_mutations().push_back(mr2);

    aura::ast::MatchClauseInfo mci;
    mci.used_constructors = {101, 102, 103};
    mci.candidate_constructors = {201, 202};
    mci.has_wildcard = false;
    flat.set_match_info(lit, mci);

    auto sym_fn = pool.intern("fn");
    flat.set_function_region(sym_fn, 1);
    flat.set_function_region_lambda(var, 2);

    std::vector<char> buf;
    flat.serialize_soa(buf);

    std::uint32_t version = 0;
    std::memcpy(&version, buf.data(), 4);
    CHECK(version == 2, "wire format version is 2");
    CHECK(buf.size() > 64, "v2 buffer includes side-data");

    std::size_t pos = 0;
    auto rt = aura::ast::FlatAST::deserialize_soa(buf, pos);

    CHECK(pos == buf.size(), "all bytes consumed");
    CHECK(rt.size() == flat.size(), "node count preserved");
    CHECK(rt.root == lit, "root preserved");
    CHECK(rt.int_val(lit) == 42, "literal value preserved");
    CHECK(rt.sym_id(var) == pool.intern("q"), "variable sym preserved");

    CHECK(rt.all_mutations().size() == 2, "mutation_log size");
    if (rt.all_mutations().size() == 2) {
        CHECK(rt.all_mutations()[0].operator_name == "replace-type",
              "mutation_log[0].operator_name");
        CHECK(rt.all_mutations()[0].old_type_str == "Int",
              "mutation_log[0].old_type_str");
        CHECK(rt.all_mutations()[1].has_rollback_data == true,
              "mutation_log[1].has_rollback_data");
        CHECK(rt.all_mutations()[1].old_value == 0xAAAA,
              "mutation_log[1].old_value");
        CHECK(rt.all_mutations()[1].invariant_status == aura::ast::InvariantStatus::Ok,
              "mutation_log[1].invariant_status");
    }

    CHECK(rt.has_match_info(lit), "match_info present");
    if (auto* mi = rt.get_match_info(lit)) {
        CHECK(mi->used_constructors.size() == 3, "used_constructors size");
        CHECK(mi->used_constructors[0] == 101, "used_constructors[0]");
        CHECK(mi->candidate_constructors.size() == 2, "candidate_constructors size");
        CHECK(mi->has_wildcard == false, "has_wildcard");
    }

    CHECK(rt.get_function_region_for_sym(sym_fn).value_or(0) == 1, "region_by_sym");
    CHECK(rt.get_function_region_for_lambda(var).value_or(0) == 2,
          "region_by_lambda_id");

    return true;
}

bool test_flat_ast_v1_forward_compat() {
    PRINTLN("\n--- Test 2: v1 wire format forward-compat (no side-data) ---");

    // Minimal v1 buffer: 1 LiteralInt node, no v2 tail.
    std::vector<char> buf;
    std::uint32_t version = 1;
    std::uint32_t num_nodes = 1;
    buf.insert(buf.end(), reinterpret_cast<char*>(&version),
               reinterpret_cast<char*>(&version) + 4);
    buf.insert(buf.end(), reinterpret_cast<char*>(&num_nodes),
               reinterpret_cast<char*>(&num_nodes) + 4);

    auto write_col_u32 = [&](std::uint32_t v) {
        std::uint32_t count = 1;
        buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                   reinterpret_cast<char*>(&count) + 4);
        buf.insert(buf.end(), reinterpret_cast<char*>(&v),
                   reinterpret_cast<char*>(&v) + 4);
    };
    auto write_col_i64 = [&](std::int64_t v) {
        std::uint32_t count = 1;
        buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                   reinterpret_cast<char*>(&count) + 4);
        buf.insert(buf.end(), reinterpret_cast<char*>(&v),
                   reinterpret_cast<char*>(&v) + 8);
    };
    auto write_col_f64 = [&](double v) {
        std::uint32_t count = 1;
        buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                   reinterpret_cast<char*>(&count) + 4);
        buf.insert(buf.end(), reinterpret_cast<char*>(&v),
                   reinterpret_cast<char*>(&v) + 8);
    };
    auto write_col_u8 = [&](std::uint8_t v) {
        std::uint32_t count = 1;
        buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                   reinterpret_cast<char*>(&count) + 4);
        buf.insert(buf.end(), reinterpret_cast<char*>(&v),
                   reinterpret_cast<char*>(&v) + 1);
    };
    auto write_col_u16 = [&](std::uint16_t v) {
        std::uint32_t count = 1;
        buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                   reinterpret_cast<char*>(&count) + 4);
        buf.insert(buf.end(), reinterpret_cast<char*>(&v),
                   reinterpret_cast<char*>(&v) + 2);
    };
    auto write_empty_col = [&]() {
        std::uint32_t count = 0;
        buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                   reinterpret_cast<char*>(&count) + 4);
    };

    std::uint32_t tag = 0x01; // LiteralInt
    write_col_u32(tag);
    write_col_i64(99);
    write_col_f64(0.0);
    write_col_u32(aura::ast::INVALID_SYM);
    write_col_u32(0); // child count
    write_empty_col(); // flat children
    write_col_u32(aura::ast::NULL_NODE); // parent
    write_col_u32(0); // param_begin
    write_col_u32(0); // param_count
    write_col_u32(0); // cap_require
    write_empty_col(); // param_data
    write_empty_col(); // param_annot_data
    write_col_u32(1); // line
    write_col_u32(1); // col
    write_col_u8(0); // marker
    write_col_u8(0); // dirty
    write_col_u8(0); // verify_dirty (Issue #437; absent in pre-#437 v1 fixtures)
    write_col_u32(0); // type_id
    write_col_u8(0); // error_kind
    write_col_i64(0); // value_cache
    write_col_u32(0); // node_first_mutation
    write_col_u16(1); // node_gen
    std::uint32_t next_mut = 0;
    std::uint16_t gen = 1;
    std::uint16_t reserved = 0;
    buf.insert(buf.end(), reinterpret_cast<char*>(&next_mut),
               reinterpret_cast<char*>(&next_mut) + 4);
    buf.insert(buf.end(), reinterpret_cast<char*>(&gen),
               reinterpret_cast<char*>(&gen) + 2);
    buf.insert(buf.end(), reinterpret_cast<char*>(&reserved),
               reinterpret_cast<char*>(&reserved) + 2);

    std::size_t pos = 0;
    auto rt = aura::ast::FlatAST::deserialize_soa(buf, pos);
    CHECK(pos == buf.size(), "v1 buffer fully consumed");
    CHECK(rt.size() == 1, "v1 node count");
    CHECK(rt.int_val(0) == 99, "v1 literal value");
    CHECK(rt.all_mutations().empty(), "v1 mutation_log empty");
    CHECK(rt.match_info_.empty(), "v1 match_info empty");
    CHECK(rt.root == aura::ast::NULL_NODE, "v1 root default");
    return true;
}

int run_tests() {
    std::println("═══ Issue #269 — FlatAST wire format v2 (production) ═══");
    test_flat_ast_v2_production_roundtrip();
    test_flat_ast_v1_forward_compat();
    std::println("\nTotal: %d passed, %d failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

}  // namespace aura_issue_269_detail

int aura_issue_269_run() { return aura_issue_269_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_269_run(); }
#endif