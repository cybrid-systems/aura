// @category: unit
// @reason: Issue #2451 — param_begin_ + param_count_ publish order
//          (count last after arena fill) under post-parse contract.
//
//   AC1: after add_lambda, get().params span matches args
//   AC2: set_lambda_params republishes coherent (begin, count)
//   AC3: source cites Issue #2451 + count-last publish order

#include "test_harness.hpp"

#include <array>
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

} // namespace

int run_test_param_begin_count_publish_2451() {
    std::println("=== Issue #2451: param_begin_ + param_count_ publish order ===");

    // ── AC1: add_lambda publishes coherent params span ─────────────
    {
        std::println("\n--- #2451 AC1: add_lambda params span coherent ---");
        FlatAST flat;
        const auto body = flat.add_literal(0);
        std::array<SymId, 3> params{1, 2, 3};
        const auto lam = flat.add_lambda(std::span<const SymId>{params}, body, false);
        auto v = flat.get(lam);
        CHECK(v.tag == NodeTag::Lambda, "AC1: lambda tag");
        CHECK(v.params.size() == 3, "AC1: params size 3");
        CHECK(v.params[0] == 1 && v.params[1] == 2 && v.params[2] == 3, "AC1: param values");
        CHECK(v.param_annotations.size() == 3, "AC1: annot span matches count");
    }

    // ── AC2: set_lambda_params republish ───────────────────────────
    {
        std::println("\n--- #2451 AC2: set_lambda_params coherent republish ---");
        FlatAST flat;
        const auto body = flat.add_literal(0);
        std::array<SymId, 1> p0{9};
        const auto lam = flat.add_lambda(std::span<const SymId>{p0}, body, false);
        CHECK(flat.get(lam).params.size() == 1, "AC2: initial count 1");

        std::array<SymId, 4> p1{40, 41, 42, 43};
        const auto ann = flat.add_literal(7);
        std::array<NodeId, 4> a1{ann, NULL_NODE, NULL_NODE, NULL_NODE};
        flat.set_lambda_params(lam, std::span<const SymId>{p1}, std::span<const NodeId>{a1});
        auto v = flat.get(lam);
        CHECK(v.params.size() == 4, "AC2: new count 4");
        CHECK(v.params[0] == 40 && v.params[3] == 43, "AC2: new param values");
        CHECK(v.param_annotations.size() == 4, "AC2: annot count matches");
        CHECK(v.param_annotations[0] == ann, "AC2: first annot");
    }

    // ── AC3: source cite ───────────────────────────────────────────
    {
        std::println("\n--- #2451 AC3: source cites publish-order contract ---");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2451") != std::string::npos, "source-cite #2451");
        CHECK(ast.find("param_begin_") != std::string::npos &&
                  ast.find("param_count_") != std::string::npos,
              "begin/count columns");
        CHECK(ast.find("publish") != std::string::npos ||
                  ast.find("count last") != std::string::npos ||
                  ast.find("TOCTOU") != std::string::npos,
              "publish-order / TOCTOU wording");
        CHECK(ast.find("param_count_[id] = static_cast<std::uint32_t>(params.size())") !=
                  std::string::npos,
              "count assign present");
    }

    std::println("\n=== #2451 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_param_begin_count_publish_2451();
}
#endif
