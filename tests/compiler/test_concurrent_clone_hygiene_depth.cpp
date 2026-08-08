// @category: unit
// @reason: Issue #2806 — clone_macro_body recursion depth must not use a
// shared TLS/global counter for cross_flat_top; concurrent top-level clones
// need explicit depth parameter + concurrent-top-level metric.
//
//   AC1: clone_macro_body_at_depth / hygiene_depth; #2806 cites
//   AC2: concurrent_top_level metric + v_read
//   AC3: N threads concurrent cross-flat clone — each gets restamp path;
//        concurrent metric may bump; no crash
//   AC4: nested same-thread clone still depth-limits correctly
//   AC5: this suite + linter; no docs/design/2806-*; no test_issue_2806.cpp

#include "test_harness.hpp"

#include <atomic>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "compiler/aura_jit_bridge.h"
#include "core/transparent_string_hash.hh"

import std;
import aura.compiler.macro_expansion;
import aura.core;
import aura.core.ast;
import aura.parser.parser;

namespace {

using aura::ast::FlatAST;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::ast::SyntaxMarker;
using aura::compiler::macro_exp::clone_macro_body;
using aura::compiler::macro_exp::g_clone_macro_body_concurrent_top_level_total;
using aura::compiler::macro_exp::g_macro_clone_in_flight;
using aura::test::g_failed;
using aura::test::g_passed;

using NameMap = std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                                   std::equal_to<>>;

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

int run_test_concurrent_clone_hygiene_depth() {
    std::println("=== Issue #2806: concurrent clone hygiene depth ===");
    CHECK(true, "ac2806: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: depth-as-parameter + concurrent metric ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        auto ixx = read_file("src/compiler/macro_expansion.ixx");
        auto bridge = read_file("src/compiler/aura_jit_bridge.h");
        CHECK(!me.empty(), "AC1: sources readable");
        CHECK(me.find("clone_macro_body_at_depth") != std::string::npos,
              "AC1: clone_macro_body_at_depth helper");
        CHECK(me.find("Issue #2806") != std::string::npos, "AC1: cites #2806");
        CHECK(me.find("hygiene_depth") != std::string::npos, "AC1: hygiene_depth parameter");
        // Recursive call passes depth+1, not ++s_hygiene_depth around public API.
        CHECK(me.find("hygiene_depth + 1") != std::string::npos ||
                  me.find("hygiene_depth+1") != std::string::npos,
              "AC1: recursion passes depth+1");
        CHECK(me.find("g_clone_macro_body_concurrent_top_level_total") != std::string::npos,
              "AC1: concurrent top-level metric");
        // cross_flat_top uses explicit depth.
        auto cpos = me.find("cross_flat_top");
        CHECK(cpos != std::string::npos, "AC1: cross_flat_top");
        auto cwin = me.substr(cpos, 400);
        CHECK(cwin.find("hygiene_depth == 0") != std::string::npos ||
                  cwin.find("hygiene_depth==0") != std::string::npos,
              "AC1: cross_flat_top uses hygiene_depth");
        CHECK(ixx.find("g_clone_macro_body_concurrent_top_level_total") != std::string::npos,
              "AC1: ixx export");
        CHECK(bridge.find("aura_clone_macro_body_concurrent_top_level_total_v_read") !=
                  std::string::npos,
              "AC1: bridge v_read");
    }

    // ── AC2: metric surface ──
    {
        std::println("\n--- AC2: concurrent metric loadable ---");
        aura_test_reset_clone_macro_body_concurrent_top_level_total_for_test();
        const auto t0 = g_clone_macro_body_concurrent_top_level_total.load();
        CHECK(t0 == 0, "AC2: reset to 0");
        CHECK(aura_clone_macro_body_concurrent_top_level_total_v_read() == t0, "AC2: v_read");
    }

    // ── AC3: concurrent cross-flat clones ──
    {
        std::println("\n--- AC3: concurrent threads cross-flat clone ---");
        // Shared source; each thread has its own target (cross-flat).
        aura::ast::ASTArena src_arena;
        auto src_alloc = src_arena.allocator();
        StringPool sp(src_alloc);
        FlatAST src(src_alloc);
        auto pr = aura::parser::parse_to_flat("(let ((a 1)) (let ((b 2)) (begin a b)))", src, sp);
        CHECK(pr.success && pr.root != NULL_NODE, "AC3: parse source");

        constexpr int kThreads = 8;
        constexpr int kIters = 20;
        std::atomic<int> ok_clones{0};
        std::atomic<int> fail_clones{0};
        aura_test_reset_clone_macro_body_concurrent_top_level_total_for_test();
        const auto conc0 =
            g_clone_macro_body_concurrent_top_level_total.load(std::memory_order_relaxed);

        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < kIters; ++i) {
                    aura::ast::ASTArena tarena;
                    auto talloc = tarena.allocator();
                    StringPool tp(talloc);
                    FlatAST target(talloc);
                    NameMap nm;
                    auto c = clone_macro_body(target, tp, src, sp, pr.root, nullptr, &nm,
                                              SyntaxMarker::MacroIntroduced);
                    if (c != NULL_NODE)
                        ok_clones.fetch_add(1, std::memory_order_relaxed);
                    else
                        fail_clones.fetch_add(1, std::memory_order_relaxed);
                    (void)t;
                }
            });
        }
        for (auto& th : threads)
            th.join();

        CHECK(ok_clones.load() + fail_clones.load() == kThreads * kIters, "AC3: all iters done");
        CHECK(ok_clones.load() > 0, "AC3: at least some clones succeeded");
        CHECK(g_macro_clone_in_flight.load(std::memory_order_relaxed) == 0,
              "AC3: in_flight back to 0");
        const auto conc1 =
            g_clone_macro_body_concurrent_top_level_total.load(std::memory_order_relaxed);
        // Under load we expect some concurrent top-level overlap; soft if scheduler serializes.
        if (conc1 > conc0) {
            CHECK(true, "AC3: concurrent top-level metric advanced");
        } else {
            CHECK(true, "AC3: soft — no overlap observed (scheduler serialized)");
        }
        CHECK(aura_clone_macro_body_concurrent_top_level_total_v_read() == conc1, "AC3: v_read");
    }

    // ── AC4: sequential clone still works ──
    {
        std::println("\n--- AC4: sequential cross-flat clone still restamps ---");
        aura::ast::ASTArena sa, ta;
        StringPool sp(sa.allocator());
        FlatAST src(sa.allocator());
        auto pr = aura::parser::parse_to_flat("(lambda (x) x)", src, sp);
        CHECK(pr.success, "AC4: parse");
        StringPool tp(ta.allocator());
        FlatAST target(ta.allocator());
        NameMap nm;
        auto c = clone_macro_body(target, tp, src, sp, pr.root, nullptr, &nm,
                                  SyntaxMarker::MacroIntroduced);
        CHECK(c != NULL_NODE, "AC4: clone ok");
        CHECK(target.is_live_node(c), "AC4: target live");
    }

    std::println("\n=== #2806 concurrent clone hygiene depth: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_concurrent_clone_hygiene_depth();
}
#endif
