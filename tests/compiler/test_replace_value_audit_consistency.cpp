// @category: unit
// @reason: Issue #2793 — replace-value logs Committed before Guard commit;
// Guard abort must restore the scalar AND mark mutation_log status=RolledBack
// (no torn audit: value reverted while status still claims Committed).
//
//   AC1: replace-value + rollback_to_size cite #2793; force RolledBack helper
//   AC2: failed boundary restores int value + status=RolledBack
//   AC3: failed boundary restores float/sym similarly
//   AC4: healthy inverse path leaves mutation_log_status_torn_total == 0
//   AC5: this suite + linter; no docs/design/2793-*; no test_issue_2793.cpp

#include "test_harness.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.core;
import aura.core.ast;

namespace {

using aura::compiler::Evaluator;
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

} // namespace

int run_test_replace_value_audit_consistency() {
    std::println("=== Issue #2793: replace-value audit status consistency ===");
    CHECK(true, "ac2793: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: source cites #2793 + force RolledBack helper ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(!mut.empty(), "AC1: mutate primitives readable");
        CHECK(!ast.empty(), "AC1: ast.ixx readable");
        auto pos = mut.find("mutate:replace-value");
        CHECK(pos != std::string::npos, "AC1: replace-value present");
        // Window must cover the #2793 comment block + LiteralInt branch (~3.5KB).
        auto win = mut.substr(pos > 600 ? pos - 600 : 0, 5000);
        CHECK(win.find("Issue #2793") != std::string::npos, "AC1: replace-value cites #2793");
        CHECK(win.find("MutationSoAField::IntVal") != std::string::npos,
              "AC1: Int uses MutationSoAField::IntVal");
        CHECK(ast.find("rollback_record_for_boundary_abort") != std::string::npos,
              "AC1: rollback_record_for_boundary_abort present");
        CHECK(ast.find("mutation_log_status_torn_total") != std::string::npos,
              "AC1: mutation_log_status_torn_total counter");
        CHECK(ast.find("Issue #2793") != std::string::npos, "AC1: ast cites #2793");
    }

    // ── AC2: int replace under failed boundary ──
    {
        std::println("\n--- AC2: int value restored + status=RolledBack ---");
        Evaluator ev;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto lit = flat.add_literal(static_cast<std::int64_t>(10));
        flat.root = lit;
        ev.set_workspace_flat(&flat);
        ev.set_workspace_pool(&pool);

        const auto old_val = flat.get(lit).int_value;
        const auto log_before = flat.mutation_log_view().size();
        const auto torn0 = flat.mutation_log_status_torn_total();

        ev.enter_mutation_boundary();
        const auto new_val = static_cast<std::int64_t>(42);
        (void)flat.add_mutation_with_rollback(
            lit, "replace-value", "Int", "Int", "ac2793-int", aura::ast::MutationStatus::Committed,
            static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal),
            static_cast<std::uint64_t>(old_val), static_cast<std::uint64_t>(new_val), true);
        flat.set_int(lit, new_val);
        CHECK(flat.get(lit).int_value == new_val, "AC2: mid-boundary value is 42");
        CHECK(flat.mutation_log_view().size() == log_before + 1, "AC2: one log entry");
        CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::Committed,
              "AC2: mid-boundary status Committed");
        ev.exit_mutation_boundary(false);

        CHECK(flat.get(lit).int_value == old_val, "AC2: value restored to 10");
        CHECK(flat.mutation_log_view().size() == log_before + 1, "AC2: log entry retained");
        CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::RolledBack,
              "AC2: status=RolledBack after Guard abort");
        CHECK(flat.mutation_log_status_torn_total() == torn0,
              "AC4: healthy inverse → torn counter unchanged");
    }

    // ── AC3: float + sym ──
    {
        std::println("\n--- AC3: float + sym restore + RolledBack ---");
        Evaluator ev;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);

        auto flit = flat.add_literal_float(1.5);
        auto v = flat.add_variable(pool.intern("x"));
        flat.root = flit;
        ev.set_workspace_flat(&flat);
        ev.set_workspace_pool(&pool);

        // Float
        {
            const double old_f = flat.get(flit).float_value;
            std::uint64_t old_bits = 0;
            std::memcpy(&old_bits, &old_f, sizeof(old_f));
            const double new_f = 9.25;
            std::uint64_t new_bits = 0;
            std::memcpy(&new_bits, &new_f, sizeof(new_f));
            ev.enter_mutation_boundary();
            (void)flat.add_mutation_with_rollback(
                flit, "replace-value", "Float", "Float", "ac2793-float",
                aura::ast::MutationStatus::Committed,
                static_cast<std::uint32_t>(aura::ast::MutationSoAField::FloatVal), old_bits,
                new_bits, true);
            flat.set_float(flit, new_f);
            ev.exit_mutation_boundary(false);
            CHECK(flat.get(flit).float_value == old_f, "AC3: float restored");
            CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::RolledBack,
                  "AC3: float status=RolledBack");
        }

        // Sym
        {
            const auto old_sym = flat.sym_id(v);
            const auto new_sym = pool.intern("y");
            ev.enter_mutation_boundary();
            (void)flat.add_mutation_with_rollback(
                v, "replace-value", "Sym", "Sym", "ac2793-sym",
                aura::ast::MutationStatus::Committed,
                static_cast<std::uint32_t>(aura::ast::MutationSoAField::SymId),
                static_cast<std::uint64_t>(old_sym), static_cast<std::uint64_t>(new_sym), true);
            flat.set_sym(v, new_sym);
            CHECK(flat.sym_id(v) == new_sym, "AC3: mid-boundary sym is y");
            ev.exit_mutation_boundary(false);
            CHECK(flat.sym_id(v) == old_sym, "AC3: sym restored to x");
            CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::RolledBack,
                  "AC3: sym status=RolledBack");
        }
    }

    // ── AC4: commit path keeps Committed ──
    {
        std::println("\n--- AC4b: success path keeps Committed ---");
        Evaluator ev;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto lit = flat.add_literal(static_cast<std::int64_t>(7));
        flat.root = lit;
        ev.set_workspace_flat(&flat);
        ev.set_workspace_pool(&pool);
        ev.enter_mutation_boundary();
        (void)flat.add_mutation_with_rollback(
            lit, "replace-value", "Int", "Int", "ac2793-ok", aura::ast::MutationStatus::Committed,
            static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal), 7, 8, true);
        flat.set_int(lit, 8);
        ev.exit_mutation_boundary(true);
        CHECK(flat.get(lit).int_value == 8, "AC4b: value stays 8 on commit");
        CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::Committed,
              "AC4b: status stays Committed on success");
    }

    std::println("\n=== #2793 replace-value audit consistency: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_replace_value_audit_consistency();
}
#endif
