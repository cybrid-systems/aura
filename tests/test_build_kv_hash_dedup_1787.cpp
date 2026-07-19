// @category: unit
// @reason: Issue #1787 — compile_05.cpp must use a single shared
// build_kv_hash helper instead of 6× inlined FNV-1a build_hash lambdas.
//
//   AC1: source has build_kv_hash helper citing #1787
//   AC2: no "auto build_hash" lambdas remain in compile_05.cpp
//   AC3: ≥6 call sites return build_kv_hash(ev, kv)
//   AC4: capacity min 16 / scales with kv size
//   AC5: runtime smoke — inline-pass-stats + concurrency:stats hashes work

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::is_hash;
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
    // ── AC1–AC4: source ──
    {
        std::println("\n--- AC1–AC4: shared build_kv_hash ---");
        auto src = read_first({"src/compiler/evaluator_primitives_compile_05.cpp",
                               "../src/compiler/evaluator_primitives_compile_05.cpp"});
        CHECK(!src.empty(), "read compile_05.cpp");
        CHECK(src.find("#1787") != std::string::npos, "cites #1787");
        CHECK(src.find("build_kv_hash") != std::string::npos, "has build_kv_hash");
        CHECK(src.find("[[nodiscard]] EvalValue build_kv_hash") != std::string::npos,
              "helper definition present");
        CHECK(src.find("auto build_hash") == std::string::npos, "no auto build_hash lambdas");
        // Count call sites.
        std::size_t calls = 0;
        for (std::size_t pos = 0;
             (pos = src.find("return build_kv_hash(ev, kv)", pos)) != std::string::npos; pos += 1)
            ++calls;
        CHECK(calls >= 6, std::format("≥6 call sites (got {})", calls));
        CHECK(src.find("std::size_t ncap = 16") != std::string::npos, "min capacity 16");
        CHECK(src.find("while (ncap < kv.size() * 2)") != std::string::npos,
              "capacity scales with kv");
        // Old fixed create(8) for stats hashes should be gone.
        CHECK(src.find("FlatHashTable::create(8)") == std::string::npos,
              "no fixed create(8) for stats hashes");
    }

    // ── AC5: runtime smoke ──
    {
        std::println("\n--- AC5: stats primitives still return hashes ---");
        CompilerService cs;
        auto a = cs.eval("(engine:metrics \"compile:inline-pass-stats\")");
        CHECK(a && is_hash(*a), "inline-pass-stats hash");
        auto b = cs.eval("(engine:metrics \"concurrency:stats\")");
        CHECK(b && is_hash(*b), "concurrency:stats hash");
        auto c = cs.eval("(engine:metrics \"syntax-marker-counts\")");
        // may be void without workspace — set-code first
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
        c = cs.eval("(engine:metrics \"syntax-marker-counts\")");
        CHECK(c && is_hash(*c), "syntax-marker-counts hash");
        auto d = cs.eval("(engine:metrics \"compile:type-cache-stats\")");
        CHECK(d && is_hash(*d), "type-cache-stats hash");
        auto e = cs.eval("(engine:metrics \"compile:incremental-typecheck-stats\")");
        CHECK(e && is_hash(*e), "incremental-typecheck-stats hash");
        auto f = cs.eval("(compile:per-symbol-dirty-stats \"x\")");
        CHECK(f && is_hash(*f), "per-symbol-dirty-stats hash");
    }

    std::println("\n=== test_build_kv_hash_dedup_1787: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
