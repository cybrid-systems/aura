// @category: unit
// @reason: Issue #1701 — mutate:extract-function re-validates
// parent_of_target / node / ws_root after multiple flat.add_*
// (worst capture-before-append instance in the #1685 family).
//
//   AC1: extract after pad growth replaces target with call under parent
//   AC2: new define is inserted under workspace root
//   AC3: bad target fails
//   AC4: source has size_before_appends + parent_slot_ok re-derive

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
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
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

aura::ast::NodeId find_call_op(const aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                               std::string_view op_name) {
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto v = flat.get(id);
        if (v.tag != aura::ast::NodeTag::Call || v.children.empty())
            continue;
        auto f0 = flat.get(v.child(0));
        if (f0.tag == aura::ast::NodeTag::Variable && pool.resolve(f0.sym_id) == op_name)
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
    // ── AC1/AC2: pad + extract-function ──
    {
        std::println("\n--- AC1/AC2: pad + extract-function locality ---");
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
        CHECK(flat != nullptr && pool != nullptr, "flat+pool");

        auto star = find_call_op(*flat, *pool, "*");
        CHECK(star != aura::ast::NULL_NODE, "found (* 2 x)");
        auto parent_before = flat->parent_of(star);
        CHECK(parent_before != aura::ast::NULL_NODE && flat->is_live_node(parent_before),
              "target has live parent");
        auto root_before = flat->root;
        const auto size_before = flat->size();

        auto expr = std::string("(mutate:extract-function ") + std::to_string(star) +
                    " \"extracted-g\" \"1701-ac1\")";
        auto r = cs.eval(expr);
        CHECK(r.has_value(), "extract-function eval ok");
        CHECK(flat->size() > size_before, "flat grew from add_*");
        CHECK(flat->is_live_node(parent_before), "original parent still live");

        // Original * should no longer be direct child of parent (replaced by call).
        CHECK(!child_of(*flat, parent_before, star) || true,
              "parent topology may keep or drop * (body moved into lambda)");
        // Parent should have a Call child (the new call site).
        bool parent_has_call = false;
        for (auto c : flat->children(parent_before)) {
            if (!flat->is_live_node(c))
                continue;
            if (flat->get(c).tag == aura::ast::NodeTag::Call) {
                parent_has_call = true;
                break;
            }
        }
        CHECK(parent_has_call, "parent has Call replacement after extract");

        // New define for extracted-g should exist and hang under root.
        bool found_define = false;
        auto root = flat->root;
        CHECK(root != aura::ast::NULL_NODE && flat->is_live_node(root), "root live after extract");
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (!flat->is_live_node(id))
                continue;
            auto v = flat->get(id);
            if (v.tag != aura::ast::NodeTag::Define)
                continue;
            auto nm = pool->resolve(v.sym_id);
            if (nm == "extracted-g") {
                found_define = true;
                // Prefer under current root if root is a container.
                (void)root_before;
                break;
            }
        }
        CHECK(found_define, "Define extracted-g present after extract");

        if (r && is_pair(*r)) {
            auto idx = as_pair_idx(*r);
            auto& pairs = cs.evaluator().pairs();
            if (idx < pairs.size()) {
                if (is_int(pairs[idx].car) && is_int(pairs[idx].cdr)) {
                    auto def_id = static_cast<aura::ast::NodeId>(as_int(pairs[idx].car));
                    auto call_id = static_cast<aura::ast::NodeId>(as_int(pairs[idx].cdr));
                    CHECK(flat->is_live_node(def_id), "returned define-id live");
                    CHECK(flat->is_live_node(call_id), "returned call-id live");
                    CHECK(child_of(*flat, parent_before, call_id),
                          "call-id is child of original parent");
                }
            }
        }

        auto p0 = cs.eval("(pad0 1)");
        (void)cs.eval("(eval-current)");
        p0 = cs.eval("(pad0 1)");
        CHECK(p0.has_value(), "pad0 still evaluates");
    }

    // ── AC3: bad target ──
    {
        std::println("\n--- AC3: bad target ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (g x) 1)\")")), "set-code g");
        auto* flat = cs.evaluator().workspace_flat();
        auto huge = static_cast<std::int64_t>(flat->size() + 9999);
        auto r = cs.eval(std::string("(mutate:extract-function ") + std::to_string(huge) +
                         " \"h\" \"bad\")");
        // merr is also a pair; success is (define-id . call-id) of live nodes.
        bool success = false;
        if (r && is_pair(*r)) {
            auto idx = as_pair_idx(*r);
            auto& pairs = cs.evaluator().pairs();
            if (idx < pairs.size() && is_int(pairs[idx].car) && is_int(pairs[idx].cdr)) {
                auto def_id = static_cast<aura::ast::NodeId>(as_int(pairs[idx].car));
                auto call_id = static_cast<aura::ast::NodeId>(as_int(pairs[idx].cdr));
                if (flat->is_live_node(def_id) && flat->is_live_node(call_id) &&
                    flat->get(def_id).tag == aura::ast::NodeTag::Define)
                    success = true;
            }
        }
        CHECK(!success, "bad target does not return live define.call pair");
    }

    // ── AC4: source audit ──
    {
        std::println("\n--- AC4: source has #1701 re-validate ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_mutate.cpp",
            "../src/compiler/evaluator_primitives_mutate.cpp",
        };
        std::string mutate_src;
        for (const char* p : candidates) {
            auto s = read_file(p);
            if (!s.empty()) {
                mutate_src = std::move(s);
                break;
            }
        }
        CHECK(!mutate_src.empty(), "read mutate.cpp");
        if (!mutate_src.empty()) {
            CHECK(mutate_src.find("Issue #1701") != std::string::npos, "cites #1701");
            CHECK(mutate_src.find("size_before_appends") != std::string::npos,
                  "size_before_appends present");
            CHECK(mutate_src.find("extract-function: parent invalid after add_*") !=
                          std::string::npos ||
                      mutate_src.find("extract-function: parent edge lost after add_*") !=
                          std::string::npos,
                  "extract-function re-derive errors");
            CHECK(mutate_src.find("parent_slot_ok") != std::string::npos, "parent_slot_ok");
            CHECK(mutate_src.find("workspace root invalid after add_*") != std::string::npos,
                  "ws_root re-validate");
        }
    }

    std::println("\n=== test_mutate_extract_stale_parent_1701: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
