// @category: unit
// @reason: Issue #1786 — compile:per-symbol-dirty-stats ancestor walk
// must use a visited set so cyclic parent_of does not overcount
// (max_count alone still inflates ratio-bp).
//
//   AC1: source cites #1786; dense seen[] stops re-visit
//   AC2: acyclic define → ancestor-affected-count finite >= 0
//   AC3: A↔B parent cycle under def → count unique ancestors only, <1s
//   AC4: self-loop parent → terminates with small finite count

#include "test_harness.hpp"

#include <chrono>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;
using clock = std::chrono::steady_clock;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

std::int64_t ancestor_count(CompilerService& cs, const char* sym) {
    auto r = cs.eval(std::format(
        "(hash-ref (compile:per-symbol-dirty-stats \"{}\") \"ancestor-affected-count\")", sym));
    if (!r || !is_int(*r))
        return -999;
    return as_int(*r);
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: source has visited-set on ancestor walk ---");
        auto prim = read_first({"src/compiler/evaluator_primitives_compile.cpp",
                                "../src/compiler/evaluator_primitives_compile.cpp"});
        CHECK(!prim.empty(), "read compile_05.cpp");
        CHECK(prim.find("#1786") != std::string::npos, "cites #1786");
        auto pos = prim.find("add(\"compile:per-symbol-dirty-stats\"");
        CHECK(pos != std::string::npos, "primitive present");
        // Walk body is deep in the lambda — use a large window + #1786 site.
        auto win = prim.substr(pos, 9000);
        auto cycle_pos = prim.find("#1786");
        CHECK(cycle_pos != std::string::npos, "cites #1786 in body");
        auto win2 = prim.substr(cycle_pos, 1200);
        CHECK(win2.find("seen") != std::string::npos, "uses seen[]");
        CHECK(win2.find("seen[ci]") != std::string::npos, "skips already-seen ancestors");
        CHECK(win2.find("cycle") != std::string::npos, "mentions cycle");
        (void)win;
    }

    // ── AC2: acyclic ──
    {
        std::println("\n--- AC2: acyclic define ancestor count ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define foo 1)\")").has_value(), "set-code");
        (void)cs.eval("(eval-current)");
        auto h = cs.eval("(compile:per-symbol-dirty-stats \"foo\")");
        CHECK(h && is_hash(*h), "returns hash");
        const auto ac = ancestor_count(cs, "foo");
        // Root-ish define may have 0 ancestors or a few container nodes.
        CHECK(ac >= 0 && ac < 100, std::format("acyclic ancestor count finite (got {})", ac));
        std::println("  acyclic ancestor-affected-count={}", ac);
    }

    // ── AC3: A↔B parent cycle ──
    {
        std::println("\n--- AC3: parent cycle A↔B unique count ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define bar 2)\")").has_value(), "set-code bar");
        (void)cs.eval("(eval-current)");
        auto* flat = cs.workspace_flat();
        CHECK(flat != nullptr, "workspace_flat");

        // Find Define bar.
        aura::ast::NodeId def = aura::ast::NULL_NODE;
        auto* pool = cs.evaluator().workspace_pool();
        CHECK(pool != nullptr, "pool");
        auto sym = pool->find_by_name("bar");
        CHECK(sym.has_value(), "bar interned");
        for (std::size_t i = 0; i < flat->size(); ++i) {
            auto v = flat->get(static_cast<aura::ast::NodeId>(i));
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == *sym) {
                def = static_cast<aura::ast::NodeId>(i);
                break;
            }
        }
        CHECK(def != aura::ast::NULL_NODE, "found Define bar");

        // Build two IfExpr and wire parent cycle via set_child, then
        // attach def under A so parent_of(def)=A and A↔B cycle.
        auto lit = flat->add_literal(0);
        auto a = flat->add_if(lit, lit, lit);
        auto b = flat->add_if(lit, lit, lit);
        // set_child updates parent_[child] = parent
        flat->set_child(a, 1, b); // then-branch: parent_[b]=a
        flat->set_child(b, 1, a); // then-branch: parent_[a]=b  → cycle
        // Attach def as else of A: parent_[def]=a
        flat->set_child(a, 2, def);

        const auto t0 = clock::now();
        const auto ac = ancestor_count(cs, "bar");
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        CHECK(ms < 1000, std::format("cycle finished in {}ms", ms));
        // Unique ancestors of def: a, then b, then stop (cycle). count=2.
        // (lit may not be on parent chain.)
        CHECK(ac >= 1 && ac <= 4, std::format("cycle unique ancestors (got {})", ac));
        std::println("  cycle ancestor-affected-count={} in {}ms", ac, ms);
    }

    // ── AC4: self-loop ──
    {
        std::println("\n--- AC4: self-loop parent terminates ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define baz 3)\")").has_value(), "set-code baz");
        (void)cs.eval("(eval-current)");
        auto* flat = cs.workspace_flat();
        CHECK(flat != nullptr, "flat");
        auto* pool = cs.evaluator().workspace_pool();
        auto sym = pool->find_by_name("baz");
        CHECK(sym.has_value(), "baz interned");
        aura::ast::NodeId def = aura::ast::NULL_NODE;
        for (std::size_t i = 0; i < flat->size(); ++i) {
            auto v = flat->get(static_cast<aura::ast::NodeId>(i));
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == *sym) {
                def = static_cast<aura::ast::NodeId>(i);
                break;
            }
        }
        CHECK(def != aura::ast::NULL_NODE, "found Define baz");
        auto lit = flat->add_literal(1);
        auto s = flat->add_if(lit, lit, lit);
        flat->set_child(s, 1, s);   // self-loop: parent_[s] may stay s if then is s
        flat->set_child(s, 2, def); // parent_[def]=s

        const auto t0 = clock::now();
        const auto ac = ancestor_count(cs, "baz");
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        CHECK(ms < 1000, std::format("self-loop finished in {}ms", ms));
        CHECK(ac >= 1 && ac <= 3, std::format("self-loop unique count (got {})", ac));
        std::println("  self-loop ancestor-affected-count={} in {}ms", ac, ms);
    }

    std::println("\n=== test_per_symbol_dirty_cycle_guard_1786: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
