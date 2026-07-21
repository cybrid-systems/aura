// @category: unit
// @reason: Issue #1723 — evolve-strategy analytics find_after must be
// Issue #1723 (#1978 renamed): issue# moved from filename to header.
// paren-aware (nested values / top-errors lists).
//
//   AC1: source cites #1723; find_after tracks depth
//   AC2: top-errors matcher uses paren depth (not first ')')
//   AC3: evolve-strategy with nested top-errors + numeric rates still evolves
//   AC4: plain numeric analytics still parse (success-rate / avg-attempts)

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
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Body of add("evolve-strategy" ... until next top-level add(
std::string evolve_window(const std::string& src) {
    auto needle = std::string("add(\"evolve-strategy\"");
    auto pos = src.find(needle);
    if (pos == std::string::npos)
        return {};
    auto end = src.find("\n    add(\"", pos + 10);
    return src.substr(pos, end == std::string::npos ? 8000 : end - pos);
}

} // namespace

int main() {
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: paren-aware find_after / top-errors ---");
        std::string agent;
        for (const char* p : {"src/compiler/evaluator_primitives_agent.cpp",
                              "../src/compiler/evaluator_primitives_agent.cpp"}) {
            agent = read_file(p);
            if (!agent.empty())
                break;
        }
        CHECK(!agent.empty(), "read agent");
        CHECK(agent.find("#1723") != std::string::npos, "cites #1723");

        auto win = evolve_window(agent);
        CHECK(!win.empty(), "found evolve-strategy");
        CHECK(win.find("find_after") != std::string::npos, "has find_after");
        CHECK(win.find("depth") != std::string::npos, "tracks paren depth");
        // Must not keep the naive end condition as the only exit path.
        CHECK(win.find("te_depth") != std::string::npos ||
                  win.find("top-errors") != std::string::npos,
              "top-errors path present");
        CHECK(win.find("te_depth") != std::string::npos, "top-errors uses te_depth");
    }

    // ── AC3/AC4: evolve with nested top-errors + numeric rates ──
    {
        std::println("\n--- AC3/AC4: evolve-strategy analytics parse ---");
        CompilerService cs;

        auto def = cs.eval(R"AURA((define-strategy "s1723" "(lambda (x) x)" :max-attempts 3))AURA");
        CHECK(static_cast<bool>(def), "define-strategy s1723");

        // Nested list inside top-errors; rates are plain atoms.
        // success-rate 0.2 + avg-attempts 3 with max-attempts 3 → bump +2.
        auto evo = cs.eval(
            R"AURA((evolve-strategy "s1723" "#(analytics total-runs:10 success-rate:0.2 avg-attempts:3 total-llm-calls:1 avg-duration-ms:1 top-errors:( unbound-variable:3 (nested-key:2) type-error:1) by-task:((nested task name) 1/2))"))AURA");
        CHECK(static_cast<bool>(evo), "evolve-strategy with nested analytics");

        // Field max-attempts should be 5 after bump (3+2) if rates parsed.
        auto field = cs.eval(R"AURA((strategy-field "s1723-v1" "max-attempts"))AURA");
        CHECK(field.has_value() && is_int(*field), "strategy-field max-attempts is int");
        if (field && is_int(*field))
            CHECK(as_int(*field) == 5, "low success-rate → max-attempts bumped to 5");

        // High success path still parses (lower max-attempts).
        auto def2 =
            cs.eval(R"AURA((define-strategy "s1723hi" "(lambda (x) x)" :max-attempts 4))AURA");
        CHECK(static_cast<bool>(def2), "define-strategy s1723hi");
        auto evo2 = cs.eval(
            R"AURA((evolve-strategy "s1723hi" "#(analytics total-runs:10 success-rate:0.95 avg-attempts:1.0 total-llm-calls:1 avg-duration-ms:1 top-errors:() by-task:())"))AURA");
        CHECK(static_cast<bool>(evo2), "evolve high-success analytics");
        auto field2 = cs.eval(R"AURA((strategy-field "s1723hi-v1" "max-attempts"))AURA");
        CHECK(field2.has_value() && is_int(*field2), "high-success max-attempts is int");
        if (field2 && is_int(*field2))
            CHECK(as_int(*field2) == 3, "high success-rate → max-attempts lowered to 3");
    }

    std::println("\n=== test_find_after_parens_1723: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
