// @category: unit
// @reason: Issue #1784 — compile:inline-pass-stats must unpack the
// Issue #1784 (#1978 renamed): issue# moved from filename to header.
// packed uint64 via uint32_t halves so values with bit 31 set stay
// non-negative as int64_t (no accidental int32 sign-extension).
//
//   AC1: source cites #1784; uses uint32_t intermediate unpack
//   AC2: pack path in service.ixx uses uint32_t halves
//   AC3: inject hook with 0x80000000 halves → stats hash ints >= 0
//   AC4: normal zero / small values still work

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
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
    // ── AC1: source unpack ──
    {
        std::println("\n--- AC1: compile_05 unpack via uint32_t ---");
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
        // Old direct cast pattern should not remain for the split.
        CHECK(win.find("static_cast<std::int64_t>(packed & 0xFFFFFFFF)") == std::string::npos,
              "no direct int64 cast of packed mask");
        CHECK(win.find("static_cast<std::int64_t>(packed >> 32)") == std::string::npos,
              "no direct int64 cast of packed shift");
    }

    // ── AC2: pack path ──
    {
        std::println("\n--- AC2: service pack via uint32_t ---");
        auto svc = read_first({"src/compiler/service.ixx", "../src/compiler/service.ixx"});
        CHECK(!svc.empty(), "read service.ixx");
        CHECK(svc.find("#1784") != std::string::npos, "service cites #1784");
        CHECK(svc.find("inlined_u32") != std::string::npos, "pack uses inlined_u32");
        CHECK(svc.find("branch_aware_u32") != std::string::npos, "pack uses branch_aware_u32");
    }

    // ── AC3: pure unpack contract for bit-31 values ──
    {
        std::println("\n--- AC3: uint32 unpack keeps bit-31 values non-negative ---");
        constexpr std::uint32_t kHi = 0x80000000u; // INT32_MIN bit pattern as unsigned
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

    // ── AC4: runtime hook inject ──
    {
        std::println("\n--- AC4: inject high-bit packed hook into CompilerService ---");
        CompilerService cs;
        // Override the default hook with both halves at 0x80000000.
        constexpr std::uint32_t kHalf = 0x80000000u;
        const std::uint64_t packed =
            (static_cast<std::uint64_t>(kHalf) << 32) | static_cast<std::uint64_t>(kHalf);
        cs.evaluator().set_get_inline_stats_fn([packed]() -> std::uint64_t { return packed; });

        auto r = cs.eval("(engine:metrics \"compile:inline-pass-stats\")");
        CHECK(r.has_value() && is_hash(*r), "inline-pass-stats returns hash");
        // Pull fields via hash-ref (Aura).
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

    std::println("\n=== test_inline_pass_stats_unpack_1784: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
