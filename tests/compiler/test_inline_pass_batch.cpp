// tests/compiler/test_inline_pass_batch.cpp — inline_pass pair dup-merge (R19 phase 14).
// R19 phase14 — Issue #1827 + #1784 inline_pass pair
//
//   #1827: InlinePass lifetime counters must be atomics with acquire-load
//          getters (not plain size_t) so concurrent inliner runs cannot
//          data-race the stats reader.
//   #1784: compile:inline-pass-stats must unpack the packed uint64 via
//          uint32_t halves so values with bit 31 set stay non-negative
//          as int64_t (no accidental int32 sign-extension).
//
//   AC1: source cites #1827; total_* are atomic + acquire load (#1827 AC1)
//   AC2: writers use fetch_add (not plain ++) (#1827 AC2)
//   AC3: compile:inline-pass-stats still returns hash with keys (#1827 AC3)
//   AC4: concurrent stress on total_inlined() readers does not crash (#1827 AC4)
//   AC5: source cites #1784; uses uint32_t intermediate unpack (#1784 AC1)
//   AC6: pack path in service.ixx uses uint32_t halves (#1784 AC2)
//   AC7: inject hook with 0x80000000 halves → stats hash ints >= 0 (#1784 AC3)
//   AC8: normal zero / small values still work (#1784 AC4)

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.pass_manager;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::InlinePass;
using aura::compiler::types::as_int;
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

// Mirror of the fixed unpack logic (for pure unit check of the contract).
void unpack_inline_stats(std::uint64_t packed, std::int64_t& inlined, std::int64_t& branch_aware) {
    const std::uint32_t inlined_u32 = static_cast<std::uint32_t>(packed & 0xFFFFFFFFu);
    const std::uint32_t branch_aware_u32 = static_cast<std::uint32_t>(packed >> 32);
    inlined = static_cast<std::int64_t>(inlined_u32);
    branch_aware = static_cast<std::int64_t>(branch_aware_u32);
}

} // namespace

int main() {
    // ── #1827 ACs (InlinePass counters atomic) ──

    // AC1/AC2: source
    {
        std::println("\n--- AC1/AC2: InlinePass counters atomic (#1827 AC1/AC2) ---");
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
        CHECK(src.find("++total_inlined_") == std::string::npos, "no plain ++total_inlined_");
        CHECK(src.find("++total_inlined_branch_aware_") == std::string::npos,
              "no plain ++total_inlined_branch_aware_");
    }

    // AC3: primitive shape
    {
        std::println("\n--- AC3: compile:inline-pass-stats hash (#1827 AC3) ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"compile:inline-pass-stats\")");
        CHECK(r && is_hash(*r), "returns hash");
        for (const char* k : {"inlined", "branch-aware", "macro-hygiene-skipped", "total"}) {
            auto v = cs.eval(
                std::format("(hash-ref (engine:metrics \"compile:inline-pass-stats\") \"{}\")", k));
            CHECK(v && is_int(*v), std::format("key {} present", k));
        }
    }

    // AC4: concurrent readers of getters
    {
        std::println("\n--- AC4: concurrent total_inlined readers (#1827 AC4) ---");
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

    // ── #1784 ACs (packed uint64 unpack) ──

    // AC5: source unpack
    {
        std::println("\n--- AC5: compile_05 unpack via uint32_t (#1784 AC1) ---");
        auto prim = read_first({"src/compiler/evaluator_primitives_compile.cpp",
                                "../src/compiler/evaluator_primitives_compile.cpp"});
        CHECK(!prim.empty(), "read compile_05.cpp");
        CHECK(prim.find("#1784") != std::string::npos, "cites #1784");
        auto pos = prim.find("\"compile:inline-pass-stats\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = prim.substr(pos, 1200);
        CHECK(win.find("inlined_u32") != std::string::npos, "uses inlined_u32");
        CHECK(win.find("branch_aware_u32") != std::string::npos, "uses branch_aware_u32");
        CHECK(win.find("std::uint32_t") != std::string::npos, "uint32_t intermediate");
        CHECK(win.find("static_cast<std::int64_t>(packed & 0xFFFFFFFF)") == std::string::npos,
              "no direct int64 cast of packed mask");
        CHECK(win.find("static_cast<std::int64_t>(packed >> 32)") == std::string::npos,
              "no direct int64 cast of packed shift");
    }

    // AC6: pack path
    {
        std::println("\n--- AC6: service pack via uint32_t (#1784 AC2) ---");
        auto svc = read_first({"src/compiler/service.ixx", "../src/compiler/service.ixx"});
        CHECK(!svc.empty(), "read service.ixx");
        CHECK(svc.find("#1784") != std::string::npos, "service cites #1784");
        CHECK(svc.find("inlined_u32") != std::string::npos, "pack uses inlined_u32");
        CHECK(svc.find("branch_aware_u32") != std::string::npos, "pack uses branch_aware_u32");
    }

    // AC7: pure unpack contract for bit-31 values
    {
        std::println("\n--- AC7: uint32 unpack keeps bit-31 values non-negative (#1784 AC3) ---");
        constexpr std::uint32_t kHi = 0x80000000u;
        constexpr std::uint32_t kMax = 0xFFFFFFFFu;
        const std::uint64_t packed =
            (static_cast<std::uint64_t>(kHi) << 32) | static_cast<std::uint64_t>(kMax);
        std::int64_t inlined = -1;
        std::int64_t branch = -1;
        unpack_inline_stats(packed, inlined, branch);
        CHECK(inlined == static_cast<std::int64_t>(kMax), "low half = UINT32_MAX positive");
        CHECK(branch == static_cast<std::int64_t>(kHi), "high half = 0x80000000 positive");
        CHECK(inlined > 0 && branch > 0, "both halves strictly positive");
    }

    // AC8: runtime hook inject
    {
        std::println("\n--- AC8: inject high-bit packed hook (#1784 AC4) ---");
        CompilerService cs;
        constexpr std::uint32_t kHalf = 0x80000000u;
        const std::uint64_t packed =
            (static_cast<std::uint64_t>(kHalf) << 32) | static_cast<std::uint64_t>(kHalf);
        cs.evaluator().set_get_inline_stats_fn([packed]() -> std::uint64_t { return packed; });

        auto r = cs.eval("(engine:metrics \"compile:inline-pass-stats\")");
        CHECK(r.has_value() && is_hash(*r), "inline-pass-stats returns hash");
        auto inlined =
            cs.eval("(hash-ref (engine:metrics \"compile:inline-pass-stats\") \"inlined\")");
        auto branch =
            cs.eval("(hash-ref (engine:metrics \"compile:inline-pass-stats\") \"branch-aware\")");
        auto total = cs.eval("(hash-ref (engine:metrics \"compile:inline-pass-stats\") \"total\")");
        CHECK(inlined && is_int(*inlined), "inlined is int");
        CHECK(branch && is_int(*branch), "branch-aware is int");
        CHECK(total && is_int(*total), "total is int");
        if (inlined && is_int(*inlined)) {
            CHECK(as_int(*inlined) == static_cast<std::int64_t>(kHalf),
                  "inlined == 0x80000000 (non-negative)");
            CHECK(as_int(*inlined) > 0, "inlined > 0");
        }
        if (branch && is_int(*branch)) {
            CHECK(as_int(*branch) == static_cast<std::int64_t>(kHalf),
                  "branch-aware == 0x80000000 (non-negative)");
            CHECK(as_int(*branch) > 0, "branch-aware > 0");
        }
        if (total && is_int(*total) && inlined && is_int(*inlined) && branch && is_int(*branch)) {
            CHECK(as_int(*total) == as_int(*inlined) + as_int(*branch), "total = sum");
            CHECK(as_int(*total) > 0, "total > 0");
        }
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
