// @category: unit
// @reason: Issue #2456 — hoist subtree_uses_sym / find_define_by_name
//          find_first_node_with instantiation to a single TU (Option A).
//
//   AC1: subtree_uses_sym finds Variable uses / no false positives
//   AC2: find_define_by_name + defines_referencing_sym still correct
//   AC3: source cites #2456 + out-of-line + named functors in ast_impl.cpp

#include "test_harness.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::ast::SymId;
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

[[nodiscard]] bool contains_id(const std::pmr::vector<NodeId>& v, NodeId id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

} // namespace

int run_test_subtree_uses_sym_template_bloat_2456() {
    std::println("=== Issue #2456: subtree_uses_sym single-TU instantiation ===");

    // ── AC1: subtree_uses_sym semantics ────────────────────────────
    {
        std::println("\n--- #2456 AC1: subtree_uses_sym finds Variable uses ---");
        FlatAST flat;
        const SymId sym_x = 7;
        const SymId sym_y = 8;

        const auto lit = flat.add_literal(1);
        const auto var_x = flat.add_variable(sym_x);
        const auto var_y = flat.add_variable(sym_y);

        // Root begin: lit, var_x, var_y
        NodeId kids[3] = {lit, var_x, var_y};
        const auto root = flat.add_begin(std::span<const NodeId>(kids, 3));

        CHECK(flat.subtree_uses_sym(root, sym_x), "AC1: root uses x");
        CHECK(flat.subtree_uses_sym(root, sym_y), "AC1: root uses y");
        CHECK(!flat.subtree_uses_sym(root, 99), "AC1: root does not use 99");
        CHECK(flat.subtree_uses_sym(var_x, sym_x), "AC1: Variable node itself");
        CHECK(!flat.subtree_uses_sym(var_x, sym_y), "AC1: var_x is not y");
        CHECK(!flat.subtree_uses_sym(lit, sym_x), "AC1: literal is not Variable x");
        CHECK(!flat.subtree_uses_sym(NULL_NODE, sym_x), "AC1: NULL_NODE false");
        CHECK(!flat.subtree_uses_sym(static_cast<NodeId>(flat.size() + 10), sym_x),
              "AC1: OOB root false");
    }

    // ── AC2: find_define_by_name + defines_referencing_sym ─────────
    {
        std::println("\n--- #2456 AC2: find_define_by_name + defines_referencing ---");
        FlatAST flat;
        StringPool pool;
        const auto name_f = pool.intern("f");
        const auto name_g = pool.intern("g");

        const auto lit = flat.add_literal(42);
        const auto def_f = flat.add_define(name_f, lit);
        const auto var_f = flat.add_variable(name_f);
        const auto def_g = flat.add_define(name_g, var_f);

        // Search from an explicit root spanning both defines
        NodeId kids[2] = {def_f, def_g};
        const auto root = flat.add_begin(std::span<const NodeId>(kids, 2));
        flat.root = root;

        auto found_f = flat.find_define_by_name(pool, "f");
        CHECK(found_f.has_value() && *found_f == def_f, "AC2: find f by name");
        auto found_g = flat.find_define_by_name(pool, "g", root);
        CHECK(found_g.has_value() && *found_g == def_g, "AC2: find g by name");
        auto missing = flat.find_define_by_name(pool, "nope");
        CHECK(!missing.has_value(), "AC2: missing name → nullopt");

        auto affected = flat.defines_referencing_sym(name_f, def_f);
        CHECK(contains_id(affected, def_g), "AC2: g references f via Variable");
        CHECK(!contains_id(affected, def_f), "AC2: f itself excluded");
    }

    // ── AC3: source cite Option A single-TU ────────────────────────
    {
        std::println("\n--- #2456 AC3: source cites single-TU hoist ---");
        auto ast = read_file("src/core/ast.ixx");
        auto impl = read_file("src/core/ast_impl.cpp");
        CHECK(ast.find("Issue #2456") != std::string::npos, "AC3: ast.ixx cites #2456");
        CHECK(ast.find("ast_impl.cpp") != std::string::npos, "AC3: declaration points to impl");
        // Methods must not be fully defined with a lambda in the interface unit
        CHECK(impl.find("FlatAST::subtree_uses_sym") != std::string::npos,
              "AC3: subtree_uses_sym defined out-of-line");
        CHECK(impl.find("FlatAST::find_define_by_name") != std::string::npos,
              "AC3: find_define_by_name defined out-of-line");
        CHECK(impl.find("VariableUsesSymPred") != std::string::npos,
              "AC3: named VariableUsesSymPred functor");
        CHECK(impl.find("DefineSymPred") != std::string::npos, "AC3: named DefineSymPred functor");
        CHECK(impl.find("find_first_node_with") != std::string::npos,
              "AC3: walker still uses find_first_node_with");
        // Interface unit must not still hold the old per-call lambda bodies
        const auto sus = ast.find("bool subtree_uses_sym");
        CHECK(sus != std::string::npos, "AC3: declaration present");
        // Declaration-only: no `{` immediately after the param list on same logical form
        // (body moved out of line).
        CHECK(impl.find("Issue #2456") != std::string::npos, "AC3: impl cites #2456");
    }

    std::println("\n=== #2456 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_subtree_uses_sym_template_bloat_2456();
}
#endif
