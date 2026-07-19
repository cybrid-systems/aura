// @category: unit
// @reason: Issue #1827 — InlinePass lifetime counters used by
// compile:inline-pass-stats must be atomics with acquire-load
// getters (not plain size_t) so concurrent inliner runs cannot
// data-race the stats reader.
//
//   AC1: source cites #1827; total_* are atomic + acquire load
//   AC2: writers use fetch_add (not plain ++)
//   AC3: compile:inline-pass-stats still returns hash with keys
//   AC4: concurrent stress on total_inlined() readers does not crash

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
import aura.compiler.pass_manager;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::InlinePass;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
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
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: InlinePass counters atomic ---");
        auto src =
            read_first({"src/compiler/pass_manager.ixx", "../src/compiler/pass_manager.ixx"});
        CHECK(!src.empty(), "read pass_manager.ixx");
        CHECK(src.find("#1827") != std::string::npos, "cites #1827");
        CHECK(src.find("std::atomic<std::size_t> total_inlined_") != std::string::npos,
              "total_inlined_ is atomic");
        CHECK(src.find("std::atomic<std::size_t> total_inlined_branch_aware_") != std::string::npos,
              "branch_aware total is atomic");
        CHECK(src.find("memory_order_acquire") != std::string::npos, "acquire loads present");
        CHECK(src.find("total_inlined_.fetch_add") != std::string::npos,
              "fetch_add on total_inlined_");
        CHECK(src.find("total_inlined_branch_aware_.fetch_add") != std::string::npos,
              "fetch_add on branch_aware");
        // Plain ++total_inlined_ should be gone.
        CHECK(src.find("++total_inlined_") == std::string::npos, "no plain ++total_inlined_");
        CHECK(src.find("++total_inlined_branch_aware_") == std::string::npos,
              "no plain ++total_inlined_branch_aware_");
    }

    // ── AC3: primitive shape ──
    {
        std::println("\n--- AC3: compile:inline-pass-stats hash ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"compile:inline-pass-stats\")");
        CHECK(r && is_hash(*r), "returns hash");
        for (const char* k : {"inlined", "branch-aware", "macro-hygiene-skipped", "total"}) {
            auto v = cs.eval(
                std::format("(hash-ref (engine:metrics \"compile:inline-pass-stats\") \"{}\")", k));
            CHECK(v && is_int(*v), std::format("key {} present", k));
        }
    }

    // ── AC4: concurrent readers of getters ──
    {
        std::println("\n--- AC4: concurrent total_inlined readers ---");
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> sum{0};
        std::vector<std::thread> thr;
        for (int t = 0; t < 4; ++t) {
            thr.emplace_back([&]() {
                while (!stop.load(std::memory_order_relaxed)) {
                    sum.fetch_add(InlinePass::total_inlined(), std::memory_order_relaxed);
                    sum.fetch_add(InlinePass::total_inlined_branch_aware(),
                                  std::memory_order_relaxed);
                    sum.fetch_add(InlinePass::total_macro_hygiene_skipped(),
                                  std::memory_order_relaxed);
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : thr)
            t.join();
        CHECK(true, std::format("concurrent readers completed (sum={})", sum.load()));
    }

    std::println("\n=== test_inline_pass_stats_atomic_1827: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
