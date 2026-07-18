// @category: unit
// @reason: Issue #1703 — mutate:refactor/extract re-validates
// parent_of_target after parse_to_flat (8th capture-before-parse
// instance; fix landed with #1701, dedicated AC lock here).
//
//   AC1: refactor/extract after pad applies under intended parent
//   AC2: original target no longer direct child of that parent
//   AC3: bad target fails
//   AC4: source cites #1703 + parent_slot_ok re-derive

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
    // ── AC1/AC2: pad + refactor/extract locality ──
    {
        std::println("\n--- AC1/AC2: pad + refactor/extract locality ---");
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

        auto expr = std::string("(mutate:refactor/extract ") + std::to_string(star) +
                    " \"ref-ex\" \"1703-ac1\")";
        auto r = cs.eval(expr);
        CHECK(r.has_value(), "refactor/extract eval ok");
        CHECK(flat->size() > size_before, "flat grew after parse");
        CHECK(flat->is_live_node(parent_before), "original parent still live");
        // P0 impl replaces the target slot with the parsed define root.
        CHECK(!child_of(*flat, parent_before, star), "original * no longer direct child of parent");
        bool parent_has_replacement = false;
        for (auto c : flat->children(parent_before)) {
            if (flat->is_live_node(c) && c != star) {
                parent_has_replacement = true;
                break;
            }
        }
        CHECK(parent_has_replacement, "parent has a replacement child");

        // Optional: returned pair car is define id when structure is (def . parent).
        if (r && is_pair(*r)) {
            auto idx = as_pair_idx(*r);
            auto& pairs = cs.evaluator().pairs();
            if (idx < pairs.size() && is_int(pairs[idx].car)) {
                auto def_id = static_cast<aura::ast::NodeId>(as_int(pairs[idx].car));
                if (flat->is_live_node(def_id))
                    CHECK(flat->get(def_id).tag == aura::ast::NodeTag::Define ||
                              flat->get(def_id).tag == aura::ast::NodeTag::Call,
                          "return car is structural node");
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
        auto r = cs.eval(std::string("(mutate:refactor/extract ") + std::to_string(huge) +
                         " \"h\" \"bad\")");
        bool success = false;
        if (r && is_pair(*r)) {
            auto idx = as_pair_idx(*r);
            auto& pairs = cs.evaluator().pairs();
            if (idx < pairs.size() && is_int(pairs[idx].car)) {
                auto id = static_cast<aura::ast::NodeId>(as_int(pairs[idx].car));
                if (flat->is_live_node(id) && flat->get(id).tag == aura::ast::NodeTag::Define)
                    success = true;
            }
        }
        CHECK(!success, "bad target does not return live Define pair car");
    }

    // ── AC4: source audit ──
    {
        std::println("\n--- AC4: source has #1703 re-validate ---");
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
            CHECK(mutate_src.find("Issue #1703") != std::string::npos, "cites #1703");
            CHECK(mutate_src.find("refactor/extract: parent invalid after parse") !=
                          std::string::npos ||
                      mutate_src.find("refactor/extract: parent edge lost after parse") !=
                          std::string::npos,
                  "refactor/extract re-derive errors");
            CHECK(mutate_src.find("parent_slot_ok") != std::string::npos, "parent_slot_ok");
            CHECK(mutate_src.find("mutate:refactor/extract") != std::string::npos,
                  "prim registered");
        }
    }

    std::println("\n=== test_mutate_refactor_extract_stale_parent_1703: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
