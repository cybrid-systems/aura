// @category: unit
// @reason: Issue #2445 — document add_node + add_* builder mutation contract.
//
//   AC1: single-threaded add_* path unchanged (builders work)
//   AC2: concurrent add_node remains serialized (#2413 / flatast_mutex_)
//   AC3: source cites Issue #2445 builder single-threaded mutation contract

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
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

} // namespace

int run_test_add_node_builder_contract() {
    std::println("=== Issue #2445: add_node + builder mutation contract ===");

    // ── AC1: single-threaded builders unchanged ────────────────────
    {
        std::println("\n--- #2445 AC1: single-thread add_* builders ---");
        FlatAST flat;
        const auto lit = flat.add_literal(42);
        CHECK(flat.tag(lit) == NodeTag::LiteralInt, "AC1: literal tag");
        CHECK(flat.int_val(lit) == 42, "AC1: literal value");

        const auto f = flat.add_literal_float(3.5);
        CHECK(flat.tag(f) == NodeTag::LiteralFloat, "AC1: float tag");

        const SymId name = 7;
        const auto var = flat.add_variable(name);
        CHECK(flat.tag(var) == NodeTag::Variable, "AC1: variable tag");
        CHECK(flat.sym_id(var) == name, "AC1: variable sym");

        const auto call = flat.add_call(var, std::span<const NodeId>{});
        CHECK(flat.tag(call) == NodeTag::Call, "AC1: call tag");

        const auto ifn = flat.add_if(lit, lit, lit);
        CHECK(flat.tag(ifn) == NodeTag::IfExpr, "AC1: if tag");

        const SymId p0 = 1;
        std::array<SymId, 1> params{p0};
        const auto lam = flat.add_lambda(std::span<const SymId>{params}, lit, false);
        CHECK(flat.tag(lam) == NodeTag::Lambda, "AC1: lambda tag");

        const auto let = flat.add_let(name, lit, lit);
        CHECK(flat.tag(let) == NodeTag::Let, "AC1: let tag");

        const auto lr = flat.add_letrec(name, lit, lit);
        CHECK(flat.tag(lr) == NodeTag::LetRec, "AC1: letrec tag");

        const auto def = flat.add_define(name, lit);
        CHECK(flat.tag(def) == NodeTag::Define, "AC1: define tag");
        CHECK(flat.sym_id(def) == name, "AC1: define sym");

        // size is coherent after a burst of builders
        CHECK(flat.size() >= 9, "AC1: size after builders");
    }

    // ── AC2: concurrent add_node still serializes (no UB / crash) ──
    {
        std::println("\n--- #2445 AC2: concurrent add_node (flatast_mutex_) ---");
        FlatAST flat;
        constexpr int kThreads = 4;
        constexpr int kPer = 150;
        std::atomic<std::uint64_t> err{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                try {
                    for (int i = 0; i < kPer; ++i)
                        (void)flat.add_node(NodeTag::LiteralInt);
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(err.load() == 0, "AC2: no exceptions");
        CHECK(flat.size() == static_cast<std::size_t>(kThreads * kPer),
              "AC2: size == concurrent adds");
        // Spot-check last few nodes fully initialized
        if (flat.size() > 0) {
            const auto id = static_cast<NodeId>(flat.size() - 1);
            CHECK(flat.tag(id) == NodeTag::LiteralInt, "AC2: last tag published");
            CHECK(flat.int_val(id) == 0, "AC2: last int zero-init");
        }
    }

    // ── AC3: single-thread concurrent-style builder burst (contract) ─
    {
        std::println("\n--- #2445 AC3: sequential builder burst (contract path) ---");
        FlatAST flat;
        for (int i = 0; i < 100; ++i) {
            const auto lit = flat.add_literal(i);
            const auto v = flat.add_variable(static_cast<SymId>(i + 1));
            (void)flat.add_define(static_cast<SymId>(i + 100), lit);
            CHECK(flat.int_val(lit) == i, "AC3: sequential literal value");
            CHECK(flat.tag(v) == NodeTag::Variable, "AC3: sequential variable");
        }
        CHECK(flat.size() == 300, "AC3: 100×(lit+var+define)");
    }

    // Source-cite
    {
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2445") != std::string::npos, "source-cite #2445");
        CHECK(ast.find("single-threaded mutation contract") != std::string::npos ||
                  ast.find("single-threaded mutation") != std::string::npos,
              "mutation contract wording");
        CHECK(ast.find("add_literal") != std::string::npos &&
                  ast.find("flatast_mutex_") != std::string::npos,
              "builders + mutex documented");
        CHECK(ast.find("add_* builders") != std::string::npos ||
                  ast.find("add_* builder") != std::string::npos,
              "builder family cited");
    }

    std::println("\n=== #2445 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_add_node_builder_contract();
}
#endif
