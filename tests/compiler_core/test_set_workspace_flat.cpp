// @category: unit
// @reason: Issue #1729 — set_workspace_flat must lock + roll back on
// Issue #1729 (#1978 renamed): issue# moved from filename to header.
// index rebuild failure.
//
//   AC1: source cites #1729; unique_lock workspace_mtx_ + catch rollback
//   AC2: Lazy policy set/get roundtrip works
//   AC3: EagerAfterCow set works without throw
//   AC4: concurrent set_workspace_flat does not crash

#include "test_harness.hpp"

#include <atomic>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string setter_window(const std::string& src) {
    auto pos = src.find("void set_workspace_flat(ast::FlatAST* f)");
    if (pos == std::string::npos)
        return {};
    auto end = src.find("void set_workspace_pool", pos);
    return src.substr(pos, end == std::string::npos ? 2000 : end - pos);
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: lock + rollback in set_workspace_flat ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1729") != std::string::npos, "cites #1729");
        auto win = setter_window(ixx);
        CHECK(!win.empty(), "found set_workspace_flat");
        CHECK(win.find("workspace_mtx_") != std::string::npos, "locks workspace_mtx_");
        CHECK(win.find("unique_lock") != std::string::npos, "unique_lock");
        CHECK(win.find("catch") != std::string::npos, "catch for rebuild throw");
        CHECK(win.find("workspace_flat_ = saved") != std::string::npos, "rolls back to saved flat");
    }

    // ── AC2: Lazy roundtrip ──
    {
        std::println("\n--- AC2: Lazy policy set/get ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto alloc = ev.test_arena().allocator();
        auto* pool = ev.test_arena().create<aura::ast::StringPool>(alloc);
        auto* flat = ev.test_arena().create<aura::ast::FlatAST>(alloc);
        (void)pool;
        auto x_sym = pool->intern("x");
        flat->add_variable(x_sym);

        ev.set_pattern_index_policy(Evaluator::PatternIndexPolicy::Lazy);
        ev.set_workspace_flat(flat);
        CHECK(ev.workspace_flat() == flat, "workspace_flat set");
        ev.set_workspace_flat(nullptr);
        CHECK(ev.workspace_flat() == nullptr, "workspace_flat cleared");
    }

    // ── AC3: EagerAfterCow set ──
    {
        std::println("\n--- AC3: EagerAfterCow set ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto alloc = ev.test_arena().allocator();
        auto* pool = ev.test_arena().create<aura::ast::StringPool>(alloc);
        auto* flat = ev.test_arena().create<aura::ast::FlatAST>(alloc);
        auto* flat2 = ev.test_arena().create<aura::ast::FlatAST>(alloc);
        auto x_sym = pool->intern("x");
        flat->add_variable(x_sym);
        flat2->add_literal(1);

        ev.set_pattern_index_policy(Evaluator::PatternIndexPolicy::EagerAfterCow);
        ev.set_workspace_flat(flat);
        CHECK(ev.workspace_flat() == flat, "eager set keeps flat");
        ev.set_workspace_flat(flat2);
        CHECK(ev.workspace_flat() == flat2, "eager swap to flat2");
        ev.set_workspace_flat(nullptr);
        CHECK(ev.workspace_flat() == nullptr, "eager clear");
    }

    // ── AC4: concurrent sets ──
    {
        std::println("\n--- AC4: concurrent set_workspace_flat ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_pattern_index_policy(Evaluator::PatternIndexPolicy::Lazy);
        auto alloc = ev.test_arena().allocator();
        constexpr int kN = 8;
        std::vector<aura::ast::FlatAST*> flats;
        flats.reserve(kN);
        for (int i = 0; i < kN; ++i) {
            auto* pool = ev.test_arena().create<aura::ast::StringPool>(alloc);
            auto* flat = ev.test_arena().create<aura::ast::FlatAST>(alloc);
            flat->add_literal(i);
            (void)pool;
            flats.push_back(flat);
        }

        std::atomic<int> errors{0};
        auto worker = [&](int id) {
            for (int i = 0; i < 40; ++i) {
                try {
                    ev.set_workspace_flat(flats[static_cast<std::size_t>(id % kN)]);
                    auto* p = ev.workspace_flat();
                    bool known = false;
                    for (auto* f : flats)
                        if (p == f)
                            known = true;
                    if (!known && p != nullptr)
                        errors.fetch_add(1);
                } catch (...) {
                    errors.fetch_add(1);
                }
            }
        };
        std::vector<std::thread> thr;
        for (int i = 0; i < kN; ++i)
            thr.emplace_back(worker, i);
        for (auto& t : thr)
            t.join();
        CHECK(errors.load() == 0, "no errors under concurrent set");
    }

    std::println("\n=== test_set_workspace_flat_1729: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
