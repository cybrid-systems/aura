// @category: unit
// @reason: Issue #1696 — multi-node mutators log under NULL_NODE, not
// bare NodeId 0 (0 is a live slot; NULL_NODE = ~0u is the multi-op
// sentinel).
//
//   AC1: NULL_NODE is distinct from NodeId 0
//   AC2: mutate:replace-pattern log target_node == NULL_NODE
//   AC3: mutate:rename-symbol log target_node == NULL_NODE
//   AC4: source sites use NULL_NODE / aura::ast::NULL_NODE (no bare 0)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;

bool eval_ok(CompilerService& cs, const std::string& expr) {
    return cs.eval(expr).has_value();
}

// Find the most recent mutation_log entry with the given operator_name.
// Returns true and fills *out if found.
bool find_last_op(const aura::ast::FlatAST& flat, std::string_view op,
                  aura::ast::MutationRecord* out) {
    auto view = flat.mutation_log_view();
    for (std::size_t i = view.size(); i > 0; --i) {
        const auto& rec = view[i - 1];
        if (rec.operator_name == op) {
            if (out)
                *out = rec;
            return true;
        }
    }
    return false;
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// True if `src` still contains bare add_mutation(0, "op"  for the named op.
bool has_bare_zero_add_mutation(std::string_view src, std::string_view op) {
    // Match historical bug pattern: add_mutation(0, "replace-pattern"
    // or add_mutation(0, "rename-symbol"
    std::string needle = std::string("add_mutation(0, \"") + std::string(op) + "\"";
    return src.find(needle) != std::string_view::npos;
}

} // namespace

int main() {
    // ── AC1: sentinel contract ──
    {
        std::println("\n--- AC1: NULL_NODE != 0 ---");
        static_assert(NULL_NODE != 0, "NULL_NODE must not be 0 (0 is a real NodeId)");
        CHECK(NULL_NODE != 0, "NULL_NODE is not NodeId 0");
        CHECK(NULL_NODE == ~0u, "NULL_NODE is ~0u");
    }

    // ── AC2: replace-pattern logs under NULL_NODE ──
    {
        std::println("\n--- AC2: replace-pattern log target ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (f x) (* 2 x))\")")), "set-code");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat");
        const auto log_before = flat->mutation_count();

        auto r = cs.eval("(mutate:replace-pattern \"(* 2 x)\" \"(+ x x)\" \"1696-ac2\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "replace-pattern #t");
        CHECK(flat->mutation_count() > log_before, "mutation log grew");

        aura::ast::MutationRecord rec{};
        CHECK(find_last_op(*flat, "replace-pattern", &rec), "found replace-pattern log entry");
        std::println("  replace-pattern target_node={} (NULL_NODE={})", rec.target_node, NULL_NODE);
        CHECK(rec.target_node == NULL_NODE, "replace-pattern target is NULL_NODE");
        CHECK(rec.target_node != 0, "replace-pattern target is not NodeId 0");
        CHECK(rec.operator_name == "replace-pattern", "operator_name is replace-pattern");
    }

    // ── AC3: rename-symbol logs under NULL_NODE ──
    {
        std::println("\n--- AC3: rename-symbol log target ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (g x) (+ x 1))\")")), "set-code g");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat ac3");
        const auto log_before = flat->mutation_count();

        auto r = cs.eval("(mutate:rename-symbol \"x\" \"y\" \"1696-ac3\")");
        CHECK(r.has_value(), "rename-symbol completed");
        if (r && is_bool(*r))
            CHECK(as_bool(*r), "rename-symbol #t");
        CHECK(flat->mutation_count() > log_before, "mutation log grew after rename");

        aura::ast::MutationRecord rec{};
        CHECK(find_last_op(*flat, "rename-symbol", &rec), "found rename-symbol log entry");
        std::println("  rename-symbol target_node={} (NULL_NODE={})", rec.target_node, NULL_NODE);
        CHECK(rec.target_node == NULL_NODE, "rename-symbol target is NULL_NODE");
        CHECK(rec.target_node != 0, "rename-symbol target is not NodeId 0");
    }

    // ── AC4: source audit — no bare add_mutation(0, "…") for multi-ops ──
    {
        std::println("\n--- AC4: source has no bare add_mutation(0, multi-op) ---");
        // Prefer in-tree sources relative to cwd (build runs from repo root or build/).
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_mutate.cpp",
            "src/compiler/evaluator_eval_flat.cpp",
            "../src/compiler/evaluator_primitives_mutate.cpp",
            "../src/compiler/evaluator_eval_flat.cpp",
        };
        std::string mutate_src, flat_src;
        for (const char* p : candidates) {
            auto s = read_file(p);
            if (s.empty())
                continue;
            if (std::string_view(p).find("evaluator_primitives_mutate") != std::string_view::npos)
                mutate_src = std::move(s);
            if (std::string_view(p).find("evaluator_eval_flat") != std::string_view::npos)
                flat_src = std::move(s);
        }
        CHECK(!mutate_src.empty(), "read evaluator_primitives_mutate.cpp");
        CHECK(!flat_src.empty(), "read evaluator_eval_flat.cpp");
        if (!mutate_src.empty()) {
            CHECK(!has_bare_zero_add_mutation(mutate_src, "replace-pattern"),
                  "public replace-pattern not logged under 0");
            CHECK(!has_bare_zero_add_mutation(mutate_src, "rename-symbol"),
                  "public rename-symbol not logged under 0");
            CHECK(mutate_src.find("add_mutation(NULL_NODE, \"replace-pattern\"") !=
                      std::string::npos,
                  "public replace-pattern uses NULL_NODE");
            CHECK(mutate_src.find("add_mutation(NULL_NODE, \"rename-symbol\"") != std::string::npos,
                  "public rename-symbol uses NULL_NODE");
        }
        if (!flat_src.empty()) {
            CHECK(!has_bare_zero_add_mutation(flat_src, "replace-pattern"),
                  "lockless replace-pattern not logged under 0");
            CHECK(!has_bare_zero_add_mutation(flat_src, "rename-symbol"),
                  "lockless rename-symbol not logged under 0");
            CHECK(flat_src.find("add_mutation(aura::ast::NULL_NODE, \"replace-pattern\"") !=
                      std::string::npos,
                  "lockless replace-pattern uses aura::ast::NULL_NODE");
            CHECK(flat_src.find("add_mutation(aura::ast::NULL_NODE, \"rename-symbol\"") !=
                      std::string::npos,
                  "lockless rename-symbol uses aura::ast::NULL_NODE");
        }
    }

    std::println("\n=== test_mutate_multi_node_log_null_1696: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
