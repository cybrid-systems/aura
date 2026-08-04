// @category: unit
// @reason: Issue #2448 — defines_referencing_sym skips only the mutated
//          Define NodeId, not all Defines whose name == sym (shadowing).
//
//   AC1: well-formed unique-name case still finds referencing Defines
//   AC2: two Defines with same name — exclude mutated only; other still flagged
//   AC3: source cites Issue #2448 + exclude_define parameter

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

int run_test_defines_referencing_sym() {
    std::println("=== Issue #2448: defines_referencing_sym exclude by NodeId ===");

    // ── AC1: well-formed unique names ──────────────────────────────
    {
        std::println("\n--- #2448 AC1: unique-name Define still found ---");
        FlatAST flat;
        const SymId name_f = 10;
        const SymId name_g = 11;

        // (define f 1)
        const auto lit1 = flat.add_literal(1);
        const auto def_f = flat.add_define(name_f, lit1);

        // (define g f)  — body Variable f references name_f
        const auto var_f = flat.add_variable(name_f);
        const auto def_g = flat.add_define(name_g, var_f);

        auto affected = flat.defines_referencing_sym(name_f, def_f);
        CHECK(contains_id(affected, def_g), "AC1: g found as referencing f");
        CHECK(!contains_id(affected, def_f), "AC1: f itself excluded");
        CHECK(affected.size() == 1, "AC1: exactly one affected");

        // Without exclude, f's body is a literal — still only g
        auto all = flat.defines_referencing_sym(name_f);
        CHECK(contains_id(all, def_g), "AC1: no-exclude still finds g");
        CHECK(!contains_id(all, def_f), "AC1: f body has no Variable f");
    }

    // ── AC2: shadowing / duplicate names ───────────────────────────
    {
        std::println("\n--- #2448 AC2: same-name Defines — skip only mutated ---");
        FlatAST flat;
        const SymId name = 42;

        // Outer (define name lit) — the "mutated" one
        const auto lit = flat.add_literal(0);
        const auto def_outer = flat.add_define(name, lit);

        // Inner (define name (var name)) — same sym_id, body uses name
        // Old bug: skipped because v.sym_id == name, even though body refs name.
        const auto var = flat.add_variable(name);
        const auto def_inner = flat.add_define(name, var);

        // Also a third Define with a different name that refs name
        const SymId name_h = 99;
        const auto var2 = flat.add_variable(name);
        const auto def_h = flat.add_define(name_h, var2);

        auto affected = flat.defines_referencing_sym(name, def_outer);
        CHECK(contains_id(affected, def_inner),
              "AC2: same-name inner Define still flagged (shadowing)");
        CHECK(contains_id(affected, def_h), "AC2: differently-named h still flagged");
        CHECK(!contains_id(affected, def_outer), "AC2: mutated outer excluded by NodeId");
        CHECK(affected.size() == 2, "AC2: exactly two affected");

        // Exclude the inner instead — outer body has no Variable, so only h
        auto excl_inner = flat.defines_referencing_sym(name, def_inner);
        CHECK(!contains_id(excl_inner, def_inner), "AC2: excluded inner not listed");
        CHECK(contains_id(excl_inner, def_h), "AC2: h still listed when excluding inner");
        // Outer does not use name in body — not listed
        CHECK(!contains_id(excl_inner, def_outer), "AC2: outer body unused");
    }

    // ── AC3: source cite ───────────────────────────────────────────
    {
        std::println("\n--- #2448 AC3: source cites exclude_define + service caller ---");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2448") != std::string::npos, "source-cite #2448");
        CHECK(ast.find("exclude_define") != std::string::npos, "exclude_define param");
        CHECK(ast.find("i == exclude_define") != std::string::npos ||
                  ast.find("exclude_define != NULL_NODE && i == exclude_define") !=
                      std::string::npos,
              "skip by NodeId");
        // Old broken pattern must not remain as the only skip
        auto svc = read_file("src/compiler/service.ixx");
        CHECK(svc.find("defines_referencing_sym(v.sym_id, target)") != std::string::npos ||
                  svc.find("defines_referencing_sym(v.sym_id,") != std::string::npos,
              "service passes exclude NodeId");
    }

    std::println("\n=== #2448 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_defines_referencing_sym();
}
#endif
