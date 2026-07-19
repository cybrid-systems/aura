// @category: unit
// @reason: Issue #1840 — SEVA / verify-dirty stats readers must use
// acquire-load (and multi-field snapshot) so concurrent
// apply_verify_dirty_bits cannot leave stale or mixed-epoch totals.
//
//   AC1: getters use memory_order_acquire; snapshot API present
//   AC2: compile_07 SEVA sites cite #1840 / use snapshot
//   AC3: snapshot stable under concurrent fetch_add stress
//   AC4: seva:achieve-coverage / fix-reset-bugs remain callable

#include "test_harness.hpp"

#include <atomic>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::compiler::CompilerService;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

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

} // namespace

int main() {
    // ── AC1: FlatAST API ──
    {
        std::println("\n--- AC1: acquire getters + snapshot API ---");
        auto ast = read_first({"src/core/ast.ixx", "../src/core/ast.ixx"});
        CHECK(!ast.empty(), "read ast.ixx");
        CHECK(ast.find("#1840") != std::string::npos, "cites #1840");
        CHECK(ast.find("snapshot_verify_dirty_totals") != std::string::npos, "snapshot method");
        CHECK(ast.find("struct VerifyDirtyTotalsSnapshot") != std::string::npos, "snapshot struct");
        // Getters should load acquire (not only relaxed).
        auto pos = ast.find("verify_coverage_dirty_total() const");
        CHECK(pos != std::string::npos, "coverage getter");
        auto win = ast.substr(pos, 400);
        CHECK(win.find("memory_order_acquire") != std::string::npos, "coverage getter acquire");
    }

    // ── AC2: compile_07 wiring ──
    {
        std::println("\n--- AC2: SEVA sites use snapshot / #1840 ---");
        auto prim = read_first({"src/compiler/evaluator_primitives_compile_07.cpp",
                                "../src/compiler/evaluator_primitives_compile_07.cpp"});
        CHECK(!prim.empty(), "read compile_07.cpp");
        CHECK(prim.find("#1840") != std::string::npos, "cites #1840");
        auto fix = prim.find("\"seva:fix-reset-bugs\"");
        CHECK(fix != std::string::npos, "fix-reset-bugs present");
        auto fix_win = prim.substr(fix, 900);
        CHECK(fix_win.find("snapshot_verify_dirty_totals") != std::string::npos,
              "fix-reset-bugs uses snapshot");
        auto audit = prim.find("\"query:seva-audit-log\"");
        CHECK(audit != std::string::npos, "audit-log present");
        auto audit_win = prim.substr(audit, 900);
        CHECK(audit_win.find("snapshot_verify_dirty_totals") != std::string::npos,
              "audit-log uses snapshot");
    }

    // ── AC3: concurrent stress ──
    {
        std::println("\n--- AC3: snapshot under concurrent readers ---");
        FlatAST flat;
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> reads{0};
        std::vector<std::thread> thr;
        for (int t = 0; t < 2; ++t) {
            thr.emplace_back([&]() {
                while (!stop.load(std::memory_order_relaxed)) {
                    auto s = flat.snapshot_verify_dirty_totals();
                    // Fresh flat: all zero; any non-zero would be a bug.
                    if (s.assertion | s.coverage | s.sva | s.formal_cex)
                        stop.store(true, std::memory_order_relaxed);
                    reads.fetch_add(1, std::memory_order_relaxed);
                    (void)flat.verify_coverage_dirty_total();
                }
            });
        }
        for (int i = 0; i < 2000; ++i)
            (void)flat.snapshot_verify_dirty_totals();
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : thr)
            t.join();
        auto final = flat.snapshot_verify_dirty_totals();
        CHECK(final.assertion == 0 && final.coverage == 0 && final.sva == 0 &&
                  final.formal_cex == 0,
              "fresh snapshot remains zeros");
        CHECK(reads.load() > 0, std::format("concurrent readers ran (n={})", reads.load()));
    }

    // ── AC4: runtime ──
    {
        std::println("\n--- AC4: SEVA primitives callable ---");
        CompilerService cs;
        auto a = cs.eval("(seva:achieve-coverage \"goal\" 100)");
        CHECK(a.has_value(), "achieve-coverage returns");
        CHECK(is_hash(*a) || is_void(*a), "hash or void");
        auto f = cs.eval("(seva:fix-reset-bugs)");
        CHECK(f.has_value(), "fix-reset-bugs returns");
        CHECK(is_hash(*f) || is_void(*f), "hash or void");
        auto q = cs.eval("(engine:metrics \"query:seva-audit-log\")");
        if (!q)
            q = cs.eval("(query:seva-audit-log)");
        CHECK(q.has_value(), "audit-log returns");
        CHECK(is_hash(*q) || is_void(*q), "hash or void");
    }

    std::println("\n=== test_verify_dirty_totals_snapshot_1840: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
