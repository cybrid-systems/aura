// @category: unit
// @reason: Issue #1844 — compile_07 SEVA/aot/ir/bidirectional stats
// must use shared build_kv_hash (#1787), not 7× inlined build_hash
// lambdas (create(8)/create(16) copies).
//
//   AC1: compile_07 has no local auto build_hash; cites #1844
//   AC2: all stats hashes call build_kv_hash(ev, ...)
//   AC3: representative primitives still return hashes

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
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: compile_07 uses shared build_kv_hash ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_07.cpp");
        CHECK(src.find("#1844") != std::string::npos, "cites #1844");
        CHECK(src.find("auto build_hash") == std::string::npos, "no local auto build_hash");
        CHECK(src.find("FlatHashTable::create(8)") == std::string::npos, "no create(8)");
        CHECK(src.find("FlatHashTable::create(16)") == std::string::npos, "no create(16)");
        // Calls into shared helper.
        std::size_t calls = 0;
        std::size_t p = 0;
        while ((p = src.find("build_kv_hash(ev", p)) != std::string::npos) {
            ++calls;
            p += 10;
        }
        CHECK(calls >= 7, std::format("build_kv_hash used enough times (got {})", calls));

        auto c05 = read_file("src/compiler/evaluator_primitives_compile.cpp");
        if (c05.empty())
            c05 = read_file("../src/compiler/evaluator_primitives_compile.cpp");
        CHECK(!c05.empty(), "read compile_05.cpp");
        CHECK(c05.find("EvalValue build_kv_hash") != std::string::npos, "definition in compile_05");
        CHECK(c05.find("#1844") != std::string::npos, "compile_05 cites #1844");
    }

    // ── AC3: runtime ──
    {
        std::println("\n--- AC3: representative stats still return hashes ---");
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
    }

    std::println("\n=== test_build_kv_hash_compile07_1844: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
