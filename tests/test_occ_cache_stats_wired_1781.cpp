// @category: unit
// @reason: Issue #1781 — compile:occ-cache-stats must surface real
// predicate_memo_ totals (not hardcoded 0/0/0 stub).
//
//   AC1: source cites #1781; reads predicate_memo_*_total
//   AC2: no hardcoded hits=0/misses=0/evictions=0 stub triple
//   AC3: primitive returns pair-of-pairs shape
//   AC4: TypeChecker IncrementalStats + CompilerMetrics fields present

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
using aura::compiler::types::is_pair;
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
    // ── AC1/AC2: source no longer stub ──
    {
        std::println("\n--- AC1/AC2: wired to CompilerMetrics (no 0/0/0 stub) ---");
        auto prim = read_first({"src/compiler/evaluator_primitives_compile_04.cpp",
                                "../src/compiler/evaluator_primitives_compile_04.cpp"});
        CHECK(!prim.empty(), "read compile_04.cpp");
        CHECK(prim.find("#1781") != std::string::npos, "cites #1781");
        auto pos = prim.find("\"compile:occ-cache-stats\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = prim.substr(pos, 900);
        CHECK(win.find("predicate_memo_hits_total") != std::string::npos, "reads hits total");
        CHECK(win.find("predicate_memo_misses_total") != std::string::npos, "reads misses total");
        CHECK(win.find("predicate_memo_evictions_total") != std::string::npos,
              "reads evictions total");
        // Old stub: const std::uint64_t hits = 0; misses = 0; evictions = 0;
        CHECK(win.find("const std::uint64_t hits = 0") == std::string::npos,
              "no hardcoded hits = 0");
        CHECK(win.find("All 0 until") == std::string::npos, "no stub comment");
    }

    // ── AC3: runtime shape ──
    {
        std::println("\n--- AC3: 3-tuple shape via engine:metrics ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"compile:occ-cache-stats\")");
        CHECK(r.has_value(), "primitive returns a value");
        CHECK(r && is_pair(*r), "returns pair (hits . (misses . evictions))");
    }

    // ── AC4: plumbing present in type_checker + metrics ──
    {
        std::println("\n--- AC4: IncrementalStats + CompilerMetrics fields ---");
        auto tc = read_first({"src/compiler/type_checker.ixx", "../src/compiler/type_checker.ixx"});
        CHECK(!tc.empty(), "read type_checker.ixx");
        CHECK(tc.find("predicate_memo_hits") != std::string::npos &&
                  tc.find("predicate_memo_misses") != std::string::npos &&
                  tc.find("predicate_memo_evictions") != std::string::npos,
              "IncrementalStats carries predicate_memo_*");

        auto om = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        CHECK(!om.empty(), "read observability_metrics.h");
        CHECK(om.find("predicate_memo_hits_total") != std::string::npos, "metrics hits field");
        CHECK(om.find("predicate_memo_misses_total") != std::string::npos, "metrics misses field");
        CHECK(om.find("predicate_memo_evictions_total") != std::string::npos,
              "metrics evictions field");

        auto inc = read_first({"src/compiler/compiler_metrics_fields.inc",
                               "../src/compiler/compiler_metrics_fields.inc"});
        CHECK(!inc.empty() && inc.find("predicate_memo_hits_total") != std::string::npos,
              "X-macro lists hits total");
    }

    std::println("\n=== test_occ_cache_stats_wired_1781: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
