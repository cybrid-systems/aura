// @category: unit
// @reason: Issue #1700 — mutate:wrap re-validates parent_of_target after
// parse_to_flat and re-derives the parent edge (7th capture-before-parse
// instance; also replaces O(N×C) parent scans with parent_of).
//
//   AC1: wrap after pad growth attaches under intended parent
//   AC2: wrapper root is child of original parent; target is under wrapper
//   AC3: bad node / missing '_' fail cleanly
//   AC4: source has #1700 re-validate + parent_of (no bare dual O(N×C) scans)

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
using aura::compiler::types::is_int;
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
    // ── AC1/AC2: pad + wrap under intended parent ──
    {
        std::println("\n--- AC1/AC2: pad + wrap locality ---");
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
        const auto size_before = flat->size();

        // Wrap with a non-trivial template to grow SoA past capacity boundaries.
        auto expr = std::string("(mutate:wrap ") + std::to_string(star) +
                    " \"(begin 0 0 0 0 0 _)\" \"1700-ac1\")";
        auto r = cs.eval(expr);
        CHECK(r.has_value(), "wrap eval ok");
        CHECK(r && is_int(*r), "wrap returns wrapper root id");
        auto wrapper = static_cast<aura::ast::NodeId>(as_int(*r));
        CHECK(flat->size() > size_before, "flat grew after wrap parse");
        CHECK(flat->is_live_node(wrapper), "wrapper root live");
        CHECK(flat->is_live_node(parent_before), "original parent still live");
        CHECK(child_of(*flat, parent_before, wrapper), "wrapper is child of original parent");
        CHECK(!child_of(*flat, parent_before, star),
              "original * call no longer direct child of parent");
        // Target should now hang under the wrapper (Begin) somewhere.
        bool star_under_wrapper = false;
        if (flat->is_live_node(star)) {
            auto p = flat->parent_of(star);
            for (int hops = 0; hops < 8 && p != aura::ast::NULL_NODE; ++hops) {
                if (p == wrapper) {
                    star_under_wrapper = true;
                    break;
                }
                p = flat->parent_of(p);
            }
        }
        CHECK(star_under_wrapper || child_of(*flat, wrapper, star),
              "original target reachable under wrapper");

        auto p0 = cs.eval("(pad0 1)");
        (void)cs.eval("(eval-current)");
        p0 = cs.eval("(pad0 1)");
        CHECK(p0.has_value(), "pad0 still evaluates");
    }

    // ── AC3: errors ──
    {
        std::println("\n--- AC3: bad target / missing placeholder ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (g x) 1)\")")), "set-code g");
        auto* flat = cs.evaluator().workspace_flat();
        auto huge = static_cast<std::int64_t>(flat->size() + 9999);
        auto r1 = cs.eval(std::string("(mutate:wrap ") + std::to_string(huge) +
                          " \"(begin _)\" \"bad\")");
        bool bad_target = !r1 || !is_int(*r1);
        CHECK(bad_target, "bad target does not return int id");

        // Find any live non-root node to try missing placeholder
        aura::ast::NodeId any = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_live_node(id) && flat->parent_of(id) != aura::ast::NULL_NODE) {
                any = id;
                break;
            }
        }
        if (any != aura::ast::NULL_NODE) {
            auto r2 = cs.eval(std::string("(mutate:wrap ") + std::to_string(any) +
                              " \"(begin 1)\" \"no-uscore\")");
            bool no_ph = !r2 || !is_int(*r2);
            CHECK(no_ph, "missing '_' placeholder fails");
        } else {
            CHECK(true, "skip missing-placeholder if no parented node");
        }
    }

    // ── AC4: source audit ──
    {
        std::println("\n--- AC4: source has #1700 re-validate ---");
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
            CHECK(mutate_src.find("Issue #1700") != std::string::npos, "public cites #1700");
            CHECK(mutate_src.find("wrap: parent invalid after parse") != std::string::npos ||
                      mutate_src.find("wrap: parent edge lost after parse") != std::string::npos,
                  "public re-derive error paths");
            CHECK(mutate_src.find("parent_slot_ok") != std::string::npos, "public parent_slot_ok");
            CHECK(mutate_src.find("size_before_parse") != std::string::npos,
                  "public size_before_parse for wrap");
        }
        if (!flat_src.empty()) {
            CHECK(flat_src.find("Issue #1700") != std::string::npos, "lockless cites #1700");
            CHECK(flat_src.find("batch :wrap: parent invalid after parse") != std::string::npos,
                  "lockless parent invalid path");
            CHECK(flat_src.find("parent_slot_ok") != std::string::npos, "lockless parent_slot_ok");
        }
    }

    std::println("\n=== test_mutate_wrap_stale_parent_1700: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
