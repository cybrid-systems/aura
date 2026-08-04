// @category: unit
// @reason: Issue #2454 — RAII mutation guards must not outlive FlatAST
//          (raw FlatAST* + lock on OwnedSharedMutex; move is UB).
//
//   AC1: scoped StructuralMutationGuard / ReaderLockGuard work
//   AC2: drop guard then move FlatAST — safe pattern (no UB)
//   AC3: source cites Issue #2454 lifetime contract on all 4 guards

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <utility>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeTag;
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

int run_test_raii_guard_flatast_lifetime() {
    std::println("=== Issue #2454: RAII guard FlatAST lifetime contract ===");

    // ── AC1: scoped guards (production default) ────────────────────
    {
        std::println("\n--- #2454 AC1: scoped structural / reader / metadata guards ---");
        FlatAST flat;
        const auto a = flat.add_node(NodeTag::LiteralInt);
        const auto p = flat.add_node(NodeTag::Begin);
        flat.root = p;
        {
            auto g = flat.begin_structural_mutation();
            CHECK(static_cast<bool>(g), "AC1: structural guard holds lock");
            flat.insert_child_locked(p, 0, a);
        }
        {
            auto r = flat.try_acquire_reader_lock();
            CHECK(static_cast<bool>(r), "AC1: reader guard holds lock");
        }
        {
            auto w = flat.begin_metadata_mutation();
            CHECK(static_cast<bool>(w), "AC1: metadata write guard holds lock");
        }
        {
            auto mr = flat.try_acquire_metadata_reader_lock();
            CHECK(static_cast<bool>(mr), "AC1: metadata reader guard holds lock");
        }
        CHECK(flat.size() >= 2, "AC1: flat intact after scoped guards");
    }

    // ── AC2: drop guard before move (safe pattern) ─────────────────
    // Issue #2454: holding a guard across move is UB; correct pattern
    // is drop-then-move. We exercise the safe path end-to-end.
    {
        std::println("\n--- #2454 AC2: drop guard then move FlatAST ---");
        FlatAST flat;
        const auto lit = flat.add_literal(1);
        {
            auto g = flat.begin_structural_mutation();
            CHECK(static_cast<bool>(g), "AC2: guard before move");
            // guard ends here — must end before move
        }
        FlatAST other = std::move(flat);
        CHECK(other.size() >= 1, "AC2: moved-to flat has nodes");
        auto v = other.get(lit);
        CHECK(v.tag == NodeTag::LiteralInt || v.id == lit, "AC2: node readable after move");

        // Swap after guards released
        FlatAST a;
        FlatAST b;
        (void)a.add_literal(10);
        (void)b.add_literal(20);
        {
            auto ga = a.begin_structural_mutation();
            CHECK(static_cast<bool>(ga), "AC2: guard on a");
        }
        {
            auto gb = b.begin_structural_mutation();
            CHECK(static_cast<bool>(gb), "AC2: guard on b");
        }
        std::swap(a, b);
        CHECK(a.size() >= 1 && b.size() >= 1, "AC2: swap after drop ok");
    }

    // ── AC3: source cite ───────────────────────────────────────────
    {
        std::println("\n--- #2454 AC3: source cites lifetime contract ---");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2454") != std::string::npos, "source-cite #2454");
        CHECK(ast.find("StructuralMutationGuard") != std::string::npos, "structural guard");
        CHECK(ast.find("ReaderLockGuard") != std::string::npos, "reader guard");
        CHECK(ast.find("MetadataWriteGuard") != std::string::npos, "metadata write guard");
        CHECK(ast.find("MetadataReadGuard") != std::string::npos, "metadata read guard");
        CHECK(ast.find("MUST NOT outlive") != std::string::npos ||
                  ast.find("must not outlive") != std::string::npos,
              "outlive wording");
        CHECK(ast.find("FlatAST-move") != std::string::npos ||
                  ast.find("FlatAST move") != std::string::npos ||
                  ast.find("across FlatAST move") != std::string::npos,
              "move wording");
    }

    std::println("\n=== #2454 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_raii_guard_flatast_lifetime();
}
#endif
