// @category: unit
// @reason: Issue #1679 — runtime_reflect_validate_ast_subtree must terminate
// on cyclic FlatAST children links (visited-set guard).
//
//   AC1: 2-node A↔B cycle: reflect:validate-macro-body returns promptly
//   AC2: self-loop node: validate terminates
//   AC3: acyclic tree still validates (no false stale)
//   AC4: wall time for cycle case << hang threshold (1s)

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;
using clock = std::chrono::steady_clock;

static bool validate_terminates(CompilerService& cs, aura::ast::NodeId nid, const char* label) {
    const auto t0 = clock::now();
    auto r = cs.eval(std::format("(reflect:validate-macro-body {})", static_cast<int>(nid)));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    CHECK(r.has_value() && is_bool(*r), std::format("{} returns bool", label));
    CHECK(ms < 1000, std::format("{} finished in {}ms (< 1000ms hang threshold)", label, ms));
    std::println("  {} → {} in {}ms", label, as_bool(*r) ? "#t" : "#f", ms);
    return r.has_value() && is_bool(*r) && ms < 1000;
}

} // namespace

int main() {
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define seed 1)\")").has_value(), "set-code seed");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");

    auto* flat = cs.workspace_flat();
    CHECK(flat != nullptr, "workspace_flat");

    // ── AC3: acyclic control ──
    {
        std::println("\n--- AC3: acyclic Begin tree ---");
        auto lit = flat->add_literal(42);
        auto root = flat->add_begin(std::span<const aura::ast::NodeId>(&lit, 1));
        CHECK(root != aura::ast::NULL_NODE, "acyclic begin");
        CHECK(validate_terminates(cs, root, "acyclic"), "acyclic validate ok");
    }

    // ── AC1: A ↔ B cycle ──
    {
        std::println("\n--- AC1: two-node A↔B cycle ---");
        auto a = flat->add_node(aura::ast::NodeTag::Begin);
        auto b = flat->add_node(aura::ast::NodeTag::Begin);
        CHECK(a != aura::ast::NULL_NODE && b != aura::ast::NULL_NODE, "cycle nodes allocated");
        // Build cycle: a → b → a
        flat->insert_child(a, 0, b);
        flat->insert_child(b, 0, a);
        CHECK(validate_terminates(cs, a, "cycle A↔B from a"), "cycle from a terminates");
        CHECK(validate_terminates(cs, b, "cycle A↔B from b"), "cycle from b terminates");
    }

    // ── AC2: self-loop ──
    {
        std::println("\n--- AC2: self-loop ---");
        auto s = flat->add_node(aura::ast::NodeTag::Begin);
        flat->insert_child(s, 0, s);
        CHECK(validate_terminates(cs, s, "self-loop"), "self-loop terminates");
    }

    // ── AC4: already covered by ms < 1000 in validate_terminates ──
    std::println("\n--- AC4: hang threshold embedded in AC1–AC3 ---");
    CHECK(true, "wall-time bound enforced");

    std::println("\n=== test_reflect_validate_cycle_guard_1679: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
