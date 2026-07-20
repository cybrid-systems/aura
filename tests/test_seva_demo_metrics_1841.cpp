// @category: unit
// @reason: Issue #1841 — seva:run-demo-with-metrics must not race
// active_strategy_ (strategies_mtx_ #1720) or free-race
// compiler_metrics_ (ownership #1835); verify totals use snapshot (#1840).
//
//   AC1: source cites #1841; strategies_mtx_ on active_strategy_
//   AC2: uses snapshot_verify_dirty_totals
//   AC3: primitive returns hash with expected keys

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
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
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
        std::println("\n--- AC1/AC2: run-demo-with-metrics safety wiring ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_07.cpp");
        CHECK(src.find("#1841") != std::string::npos, "cites #1841");
        auto pos = src.find("\"seva:run-demo-with-metrics\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 3500);
        CHECK(win.find("strategies_mtx_") != std::string::npos, "locks strategies_mtx_");
        CHECK(win.find("active_strategy_") != std::string::npos, "reads active_strategy_");
        CHECK(win.find("snapshot_verify_dirty_totals") != std::string::npos,
              "uses verify dirty snapshot");
        CHECK(win.find("#1835") != std::string::npos ||
                  win.find("compiler_metrics_") != std::string::npos,
              "metrics path documented");
    }

    // ── AC3: runtime ──
    {
        std::println("\n--- AC3: seva:run-demo-with-metrics hash shape ---");
        CompilerService cs;
        auto r = cs.eval("(seva:run-demo-with-metrics)");
        CHECK(r && is_hash(*r), "returns hash");
        for (const char* k :
             {"iterations-to-closure", "coverage-improvement", "human-intervention-count",
              "mutation-success-rate-pct", "mutations-total", "active-strategy"}) {
            auto v = cs.eval(std::format("(hash-ref (seva:run-demo-with-metrics) \"{}\")", k));
            CHECK(v.has_value(), std::format("key {} present", k));
            if (std::string_view(k) == "active-strategy")
                CHECK(v && is_string(*v), "active-strategy is string");
            else
                CHECK(v && is_int(*v), std::format("{} is int", k));
        }
    }

    std::println("\n=== test_seva_demo_metrics_1841: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
