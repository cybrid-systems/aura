// @category: unit
// @reason: Issue #2449 — param_data_ single-threaded mutation contract
//          (builder insert vs param_begin_/count slice readers).
//
//   AC1: single-threaded add_lambda / set_lambda_params unchanged
//   AC2: sequential multi-lambda param growth is coherent
//   AC3: source cites Issue #2449 + mutation contract for param_data_

#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
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

int main() {
    std::println("=== Issue #2449: param_data_ mutation contract ===");

    // ── AC1: single-thread add_lambda / set_lambda_params ──────────
    {
        std::println("\n--- #2449 AC1: single-thread lambda params ---");
        FlatAST flat;
        const auto body = flat.add_literal(1);
        std::array<SymId, 2> params{10, 11};
        const auto lam = flat.add_lambda(std::span<const SymId>{params}, body, false);
        CHECK(flat.tag(lam) == NodeTag::Lambda, "AC1: lambda tag");
        // get() exposes param slice via NodeView when available
        auto v = flat.get(lam);
        CHECK(v.tag == NodeTag::Lambda, "AC1: get lambda");
        // set_lambda_params replaces arena slice (append-only arena)
        std::array<SymId, 3> params2{20, 21, 22};
        flat.set_lambda_params(lam, std::span<const SymId>{params2});
        CHECK(flat.tag(lam) == NodeTag::Lambda, "AC1: still lambda after set_params");
    }

    // ── AC2: sequential multi-builder growth remains coherent ──────
    {
        std::println("\n--- #2449 AC2: sequential multi-lambda param growth ---");
        FlatAST flat;
        constexpr int kN = 40;
        std::vector<NodeId> lams;
        lams.reserve(static_cast<std::size_t>(kN));
        for (int i = 0; i < kN; ++i) {
            const auto body = flat.add_literal(i);
            std::array<SymId, 2> params{static_cast<SymId>(100 + i), static_cast<SymId>(200 + i)};
            lams.push_back(flat.add_lambda(std::span<const SymId>{params}, body, false));
        }
        CHECK(flat.size() >= static_cast<std::size_t>(kN * 2), "AC2: nodes allocated");
        for (int i = 0; i < kN; ++i) {
            CHECK(flat.tag(lams[static_cast<std::size_t>(i)]) == NodeTag::Lambda,
                  "AC2: each lambda intact after bulk growth");
        }
        // Interleave set_lambda_params after bulk adds (still single-thread)
        for (int i = 0; i < kN; i += 3) {
            std::array<SymId, 1> p{static_cast<SymId>(900 + i)};
            flat.set_lambda_params(lams[static_cast<std::size_t>(i)], std::span<const SymId>{p});
        }
        for (int i = 0; i < kN; i += 3) {
            CHECK(flat.tag(lams[static_cast<std::size_t>(i)]) == NodeTag::Lambda,
                  "AC2: set_lambda_params coherent");
        }
    }

    // ── AC3: source cite ───────────────────────────────────────────
    {
        std::println("\n--- #2449 AC3: source cites param_data_ contract ---");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2449") != std::string::npos, "source-cite #2449");
        CHECK(ast.find("param_data_") != std::string::npos, "param_data_ present");
        CHECK(ast.find("single-threaded mutation") != std::string::npos ||
                  ast.find("single-thread mutation") != std::string::npos,
              "mutation contract wording");
        CHECK(ast.find("param_data_.insert") != std::string::npos, "insert sites documented");
        CHECK(ast.find("add_lambda") != std::string::npos &&
                  ast.find("set_lambda_params") != std::string::npos,
              "builder writers cited");
    }

    std::println("\n=== #2449 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
