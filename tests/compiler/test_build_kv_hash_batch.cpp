// tests/compiler/test_build_kv_hash_batch.cpp — build_kv_hash pair dup-merge (R19 phase 8).
// R19 phase8 — Issue #1844 (compile_07 path) + Issue #1787 (compile_05 definition + capacity)
//
//   #1787: compile_05.cpp must use a single shared build_kv_hash helper
//          (≥6 call sites, capacity min 16, definition cites #1787)
//   #1844: compile_07 SEVA/aot/ir/bidirectional stats must use shared build_kv_hash
//          (≥7 call sites, no create(8/16), cites #1844)
//
//   AC1: helper definition present (cites #1787)
//   AC2: ≥6 call sites (build_kv_hash)
//   AC3: capacity min 16, scales with kv.size() * 2
//   AC4: compile_07 has no create(8/16) (replaced by build_kv_hash)
//   AC5: compile_07 has ≥7 build_kv_hash call sites
//   AC6: runtime smoke — stats primitives still return hashes

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
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1–AC3: shared build_kv_hash helper (#1787) ──
    {
        std::println("\n--- AC1–AC3: shared build_kv_hash ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_05.cpp");
        CHECK(src.find("#1787") != std::string::npos, "cites #1787");
        CHECK(src.find("[[nodiscard]] EvalValue build_kv_hash") != std::string::npos,
              "helper definition present");
        CHECK(src.find("auto build_hash") == std::string::npos, "no auto build_hash lambdas");
        // Count call sites.
        std::size_t calls = 0;
        std::size_t pos = 0;
        while ((pos = src.find("return build_kv_hash(ev, kv)", pos)) != std::string::npos) {
            ++calls;
            ++pos;
        }
        CHECK(calls >= 6, std::format("≥6 call sites (got {})", calls));
        CHECK(src.find("std::size_t ncap = 16") != std::string::npos, "min capacity 16");
        CHECK(src.find("while (ncap < kv.size() * 2)") != std::string::npos,
              "capacity scales with kv");
    }

    // ── AC4–AC5: compile_07 path uses shared build_kv_hash (#1844) ──
    {
        std::println("\n--- AC4–AC5: compile_07 uses shared build_kv_hash ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_07.cpp (same source as compile_05)");
        CHECK(src.find("#1844") != std::string::npos, "cites #1844");
        CHECK(src.find("FlatHashTable::create(8)") == std::string::npos, "no create(8)");
        CHECK(src.find("FlatHashTable::create(16)") == std::string::npos, "no create(16)");
        // Count build_kv_hash calls.
        std::size_t calls = 0;
        std::size_t p = 0;
        while ((p = src.find("build_kv_hash(ev", p)) != std::string::npos) {
            ++calls;
            p += 10;
        }
        CHECK(calls >= 7, std::format("build_kv_hash used enough times (got {})", calls));
    }

    // ── AC6: runtime smoke — stats primitives still return hashes ──
    {
        std::println("\n--- AC6: stats primitives still return hashes ---");
        CompilerService cs;
        auto aot = cs.eval("(engine:metrics \"compile:aot-stats\")");
        if (!aot)
            aot = cs.eval("(compile:aot-stats)");
        CHECK(aot && is_hash(*aot), "aot-stats hash");

        auto seva = cs.eval("(seva:achieve-coverage \"g\" 100)");
        CHECK(seva.has_value() && (is_hash(*seva) || is_void(*seva)), "achieve-coverage ok");

        auto demo = cs.eval("(seva:run-demo-with-metrics)");
        CHECK(demo && is_hash(*demo), "run-demo-with-metrics hash");

        auto bi = cs.eval("(engine:metrics \"compile:bidirectional-stats\")");
        if (!bi)
            bi = cs.eval("(compile:bidirectional-stats)");
        CHECK(bi && is_hash(*bi), "bidirectional-stats hash");

        auto inline_pass = cs.eval("(engine:metrics \"compile:inline-pass-stats\")");
        CHECK(inline_pass && is_hash(*inline_pass), "inline-pass-stats hash");

        auto conc = cs.eval("(engine:metrics \"concurrency:stats\")");
        CHECK(conc && is_hash(*conc), "concurrency:stats hash");
    }

    std::println("\n=== test_build_kv_hash_batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
