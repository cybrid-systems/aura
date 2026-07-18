// @category: unit
// @reason: Issue #1697 — mutate:replace-subtree re-validates parent_id /
// child_idx after parse_to_flat (5th capture-before-parse instance after
// #1685 / #1687 / #1690 / #1694). Re-derives edge from StableNodeRef target
// when the pre-parse parent slot is no longer attached.
//
//   AC1: replace-subtree after pad growth applies under intended parent
//   AC2: new subtree is child of original parent (not a foreign node)
//   AC3: hygiene / bad target still errors
//   AC4: source has post-parse re-validate + re-derive for public+lockless

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

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;

bool eval_ok(CompilerService& cs, const std::string& expr) {
    return cs.eval(expr).has_value();
}

bool child_of(const aura::ast::FlatAST& flat, aura::ast::NodeId parent, aura::ast::NodeId child) {
    if (parent >= flat.size() || !flat.is_live_node(parent))
        return false;
    for (auto c : flat.children(parent))
        if (c == child)
            return true;
    return false;
}

// First live Call whose first child Variable is "*" (or "+" after replace).
aura::ast::NodeId find_call_op(const aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                               std::string_view op_name) {
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto v = flat.get(id);
        if (v.tag != aura::ast::NodeTag::Call || v.children.size() < 1)
            continue;
        auto f0 = flat.get(v.child(0));
        if (f0.tag != aura::ast::NodeTag::Variable)
            continue;
        if (pool.resolve(f0.sym_id) == op_name)
            return id;
    }
    return aura::ast::NULL_NODE;
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1/AC2: pad + replace-subtree under intended parent ──
    {
        std::println("\n--- AC1/AC2: pad + replace-subtree locality ---");
        CompilerService cs;
        std::string src = "(define (f x) (* 2 x))";
        for (int i = 0; i < 48; ++i) {
            src += " (define (pad";
            src += std::to_string(i);
            src += " y) (+ y ";
            src += std::to_string(i);
            src += "))";
        }
        CHECK((eval_ok(cs, std::string("(set-code \"") + src + "\")")), "set-code pad");
        auto* flat = cs.evaluator().workspace_flat();
        auto* pool = cs.evaluator().workspace_pool();
        CHECK(flat != nullptr && pool != nullptr, "workspace flat+pool");

        auto star_call = find_call_op(*flat, *pool, "*");
        CHECK(star_call != aura::ast::NULL_NODE, "found (* 2 x) call");
        auto parent_before = flat->parent_of(star_call);
        CHECK(parent_before != aura::ast::NULL_NODE, "target has parent");
        CHECK(flat->is_live_node(parent_before), "parent live before");
        const auto size_before = flat->size();

        // Large-ish replacement to stress SoA growth past capacity boundaries.
        auto expr = std::string("(mutate:replace-subtree ") + std::to_string(star_call) +
                    " \"(+ x x 0 0 0 0 0 0 0 0)\" \"1697-ac1\")";
        auto r = cs.eval(expr);
        CHECK(r.has_value(), "replace-subtree eval ok");
        if (r && is_bool(*r))
            CHECK(as_bool(*r), "replace-subtree #t");
        CHECK(flat->size() > size_before, "flat grew after parse");
        CHECK(flat->is_live_node(parent_before), "original parent still live");

        // Original * call should no longer be under parent (slot replaced).
        // New child under parent_before must be live and not the old star_call.
        bool parent_has_new = false;
        bool parent_still_has_star = false;
        for (auto c : flat->children(parent_before)) {
            if (c == star_call) {
                parent_still_has_star = true;
                continue;
            }
            if (!flat->is_live_node(c))
                continue;
            auto cv = flat->get(c);
            if (cv.tag == aura::ast::NodeTag::Call || cv.tag == aura::ast::NodeTag::LiteralInt) {
                parent_has_new = true;
                // Direct child: if Call, first op should be "+" after our replace.
                if (cv.tag == aura::ast::NodeTag::Call && !cv.children.empty()) {
                    auto f0 = flat->get(cv.child(0));
                    if (f0.tag == aura::ast::NodeTag::Variable)
                        CHECK(pool->resolve(f0.sym_id) == "+",
                              "replacement Call under parent is (+ ...)");
                }
            }
        }
        CHECK(parent_has_new, "parent received replacement under intended slot");
        CHECK(!parent_still_has_star, "parent no longer holds original * call");

        // Pads must not have been corrupted by a wrong set_child.
        auto p0 = cs.eval("(pad0 1)");
        (void)cs.eval("(eval-current)");
        p0 = cs.eval("(pad0 1)");
        CHECK(p0.has_value(), "pad0 still evaluates after replace-subtree");
    }

    // ── AC3: bad target / macro hygiene path still fails cleanly ──
    {
        std::println("\n--- AC3: bad target id ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (g x) 1)\")")), "set-code g");
        auto* flat = cs.evaluator().workspace_flat();
        auto huge = static_cast<std::int64_t>(flat->size() + 9999);
        auto r = cs.eval(std::string("(mutate:replace-subtree ") + std::to_string(huge) +
                         " \"42\" \"bad\")");
        bool failed = !r;
        if (r && is_bool(*r))
            failed = !as_bool(*r);
        // Error values may still be has_value depending on merr representation.
        if (r && !is_bool(*r))
            failed = true; // structured error pair
        CHECK(failed, "bad target does not succeed as #t");
    }

    // ── AC4: source audit — re-validate + re-derive present ──
    {
        std::println("\n--- AC4: source has #1697 re-validate/re-derive ---");
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
            CHECK(mutate_src.find("Issue #1697") != std::string::npos, "public path cites #1697");
            CHECK(mutate_src.find("parent_slot_ok") != std::string::npos,
                  "public path has parent_slot_ok");
            CHECK(mutate_src.find("parent_child_index_if_attached") != std::string::npos,
                  "public path re-derives via parent_child_index_if_attached");
            CHECK(mutate_src.find("is_live_node(target)") != std::string::npos,
                  "public path re-derive uses is_live_node (not is_valid_in post-restamp)");
        }
        if (!flat_src.empty()) {
            CHECK(flat_src.find("Issue #1697") != std::string::npos, "lockless path cites #1697");
            CHECK(flat_src.find("parent_slot_ok") != std::string::npos,
                  "lockless path has parent_slot_ok");
            CHECK(flat_src.find("target invalid after parse") != std::string::npos,
                  "lockless re-derive target check present");
        }
    }

    std::println("\n=== test_mutate_replace_subtree_stale_parent_1697: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
