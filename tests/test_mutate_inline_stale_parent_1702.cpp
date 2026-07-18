// @category: unit
// @reason: Issue #1702 — mutate:inline-call re-validates call_parent after
// DFS clone multi-add_* (10th capture-before-append instance).
//
//   AC1: inline after pad growth installs cloned body under intended parent
//   AC2: original Call is no longer that parent's child
//   AC3: bad call id fails
//   AC4: source has size_before_clone + parent_slot_ok (public+lockless)

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

// Find Call whose first child Variable is `name` (e.g. "double" or "+").
aura::ast::NodeId find_call_named(const aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                  std::string_view name) {
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto v = flat.get(id);
        if (v.tag != aura::ast::NodeTag::Call || v.children.empty())
            continue;
        auto f0 = flat.get(v.child(0));
        if (f0.tag == aura::ast::NodeTag::Variable && pool.resolve(f0.sym_id) == name)
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
    // ── AC1/AC2: pad + inline-call locality ──
    {
        std::println("\n--- AC1/AC2: pad + inline-call locality ---");
        CompilerService cs;
        // double is a simple lambda body we can inline; f calls it.
        std::string src = "(define (double x) (+ x x)) (define (f y) (double y))";
        for (int i = 0; i < 48; ++i) {
            src += " (define (pad";
            src += std::to_string(i);
            src += " z) (+ z ";
            src += std::to_string(i);
            src += "))";
        }
        CHECK((eval_ok(cs, std::string("(set-code \"") + src + "\")")), "set-code pad");
        auto* flat = cs.evaluator().workspace_flat();
        auto* pool = cs.evaluator().workspace_pool();
        CHECK(flat != nullptr && pool != nullptr, "flat+pool");

        auto call = find_call_named(*flat, *pool, "double");
        CHECK(call != aura::ast::NULL_NODE, "found (double y) call");
        auto parent_before = flat->parent_of(call);
        CHECK(parent_before != aura::ast::NULL_NODE && flat->is_live_node(parent_before),
              "call has live parent");
        const auto size_before = flat->size();

        auto expr = std::string("(mutate:inline-call ") + std::to_string(call) + " \"1702-ac1\")";
        auto r = cs.eval(expr);
        CHECK(r.has_value(), "inline-call eval ok");
        if (r && is_bool(*r))
            CHECK(as_bool(*r), "inline-call #t");
        CHECK(flat->size() > size_before, "flat grew from DFS clone add_*");
        CHECK(flat->is_live_node(parent_before), "original parent still live");

        // Call site should no longer be the double Call under parent.
        CHECK(!child_of(*flat, parent_before, call),
              "original call not still a direct child of parent");
        // Parent should have some live child (cloned body — typically Call +).
        bool parent_has_child = false;
        for (auto c : flat->children(parent_before)) {
            if (flat->is_live_node(c)) {
                parent_has_child = true;
                break;
            }
        }
        CHECK(parent_has_child, "parent still has a live child after inline");

        // Direct child of original parent should not be a Call to "double".
        bool parent_has_double_call = false;
        for (auto c : flat->children(parent_before)) {
            if (!flat->is_live_node(c))
                continue;
            auto cv = flat->get(c);
            if (cv.tag != aura::ast::NodeTag::Call || cv.children.empty())
                continue;
            auto f0 = flat->get(cv.child(0));
            if (f0.tag == aura::ast::NodeTag::Variable && pool->resolve(f0.sym_id) == "double")
                parent_has_double_call = true;
        }
        CHECK(!parent_has_double_call, "parent no longer holds (double …) call site");

        auto p0 = cs.eval("(pad0 1)");
        (void)cs.eval("(eval-current)");
        p0 = cs.eval("(pad0 1)");
        CHECK(p0.has_value(), "pad0 still evaluates");
    }

    // ── AC3: bad call id ──
    {
        std::println("\n--- AC3: bad call id ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (g x) 1)\")")), "set-code g");
        auto* flat = cs.evaluator().workspace_flat();
        auto huge = static_cast<std::int64_t>(flat->size() + 9999);
        auto r = cs.eval(std::string("(mutate:inline-call ") + std::to_string(huge) + " \"bad\")");
        bool failed = !r;
        if (r && is_bool(*r))
            failed = !as_bool(*r);
        else if (r)
            failed = true; // merr / non-#t
        CHECK(failed, "bad call id does not succeed as #t");
    }

    // ── AC4: source audit ──
    {
        std::println("\n--- AC4: source has #1702 re-validate ---");
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
        CHECK(!mutate_src.empty(), "read mutate.cpp");
        CHECK(!flat_src.empty(), "read eval_flat.cpp");
        if (!mutate_src.empty()) {
            CHECK(mutate_src.find("Issue #1702") != std::string::npos, "public cites #1702");
            CHECK(mutate_src.find("size_before_clone") != std::string::npos,
                  "public size_before_clone");
            CHECK(mutate_src.find("inline-call: call_parent invalid after DFS clone") !=
                          std::string::npos ||
                      mutate_src.find("inline-call: parent edge lost after DFS clone") !=
                          std::string::npos,
                  "public re-derive errors");
            CHECK(mutate_src.find("parent_slot_ok") != std::string::npos, "public parent_slot_ok");
        }
        if (!flat_src.empty()) {
            CHECK(flat_src.find("Issue #1702") != std::string::npos, "lockless cites #1702");
            CHECK(flat_src.find("size_before_clone") != std::string::npos,
                  "lockless size_before_clone");
            CHECK(flat_src.find("batch :inline-call: call_parent invalid after DFS clone") !=
                      std::string::npos,
                  "lockless re-derive error");
        }
    }

    std::println("\n=== test_mutate_inline_stale_parent_1702: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
