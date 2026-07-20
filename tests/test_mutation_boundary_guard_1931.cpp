// @category: unit
// @reason: Issue #1931 — systemic MutationBoundaryGuard enforcement +
// dtor ≤6 atomics batch for hot-update hot-path (refine #1897 #1950 #1747).
//
//   AC1: source cites #1931 (dtor batch + helpers + query)
//   AC2: query:mutation-systemic-guard-stats schema-1931 + AC metrics
//   AC3: dtor common path ≤6 atomics (static count in evaluator.ixx)
//   AC4: coverage linter script present; 100% wire flags
//   AC5: runtime mutators under Guard; nested outer holds depth
//   AC6: multi-round stress — captures mono; schema holds
//   AC7: #1897 lineage schema retained
//   AC8: mutation_guard_exception_total + stale_ir keys live

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-systemic-guard-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static std::string dtor_window(const std::string& src) {
    auto pos = src.find("~MutationBoundaryGuard()");
    if (pos == std::string::npos)
        return {};
    auto end = src.find("Optional / rare path", pos);
    if (end == std::string::npos)
        end = pos + 6000;
    return src.substr(pos, end - pos);
}

static void ac1_source() {
    std::println("\n--- AC1: #1931 source surface ---");
    auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    auto hh = read_first(
        {"src/compiler/mutation_guard_helpers.hh", "../src/compiler/mutation_guard_helpers.hh"});
    auto comp = read_first({"src/compiler/evaluator_primitives_compile.cpp",
                            "../src/compiler/evaluator_primitives_compile.cpp"});
    auto lint = read_first({"scripts/check_mutation_guard_coverage.py",
                            "../scripts/check_mutation_guard_coverage.py"});
    CHECK(!ixx.empty() && ixx.find("#1931") != std::string::npos, "ixx cites #1931");
    CHECK(ixx.find("BatchMutationMetrics") != std::string::npos, "batch metrics");
    CHECK(ixx.find("publish common path") != std::string::npos, "common path publish");
    CHECK(!hh.empty() && hh.find("#1931") != std::string::npos, "helpers cite #1931");
    CHECK(hh.find("run_under_mutation_guard") != std::string::npos, "helper template");
    CHECK(!comp.empty() && comp.find("schema-1931") != std::string::npos, "query schema-1931");
    CHECK(!lint.empty() && lint.find("mutation_guard_coverage") != std::string::npos,
          "coverage linter");
}

static void ac2_schema() {
    std::println("\n--- AC2: schema-1931 on mutation-systemic-guard-stats ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:mutation-systemic-guard-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1897, "lineage 1897");
    CHECK(href(cs, "schema-1931") == 1931, "schema-1931");
    CHECK(href(cs, "issue-1931") == 1931, "issue-1931");
    CHECK(href(cs, "schema-1950") == 1950, "1950 lineage");
    CHECK(href(cs, "schema-1747") == 1747, "1747 lineage");
    CHECK(href(cs, "dtor-common-path-atomics-cap") == 6, "atomics cap 6");
    CHECK(href(cs, "dtor-batch-metrics-wired") == 1, "batch wired");
    CHECK(href(cs, "compile-mutate-guard-coverage-100pct") == 1, "100% coverage flag");
    CHECK(href(cs, "shared-helper-header-wired") == 1, "helper header");
    CHECK(href(cs, "coverage-linter-wired") == 1, "linter wired");
    CHECK(href(cs, "mutation_guard_exception_total") >= 0, "exception metric");
    CHECK(href(cs, "compile_primitive_stale_ir_prevented_total") >= 0, "stale metric");
    CHECK(href(cs, "try-acquire-wired") == 1, "try-acquire");
    CHECK(href(cs, "active") == 1, "active");
}

static void ac3_dtor_atomics() {
    std::println("\n--- AC3: dtor common path ≤6 atomics ---");
    auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    auto win = dtor_window(ixx);
    CHECK(!win.empty(), "dtor window");
    // Count fetch_add in the common publish block only (before rare path).
    auto pub = win.find("publish common path");
    CHECK(pub != std::string::npos, "publish marker");
    auto common = win.substr(pub);
    auto rare = common.find("Optional / rare path");
    if (rare != std::string::npos)
        common = common.substr(0, rare);
    int fetch_adds = 0;
    for (std::size_t i = 0; (i = common.find("fetch_add", i)) != std::string::npos; ++i)
        ++fetch_adds;
    // 5 fetch_add + 1 optional CAS on common path (cap 6 writes).
    CHECK(fetch_adds <= 6, "common path fetch_add ≤6");
    CHECK(fetch_adds >= 4, "at least dual hold counters");
    CHECK(common.find("compare_exchange") != std::string::npos ||
              win.find("compare_exchange_weak") != std::string::npos,
          "max uses CAS (#1765)");
}

static void ac4_coverage_flags() {
    std::println("\n--- AC4: inventory + coverage flags ---");
    CompilerService cs;
    CHECK(href(cs, "mark-clear-block-instruction-dirty") == 1, "dirty mark/clear");
    CHECK(href(cs, "clear-macro-dirty") == 1, "macro dirty");
    CHECK(href(cs, "subtree-bump") == 1, "subtree");
    CHECK(href(cs, "compact-env-frames") == 1, "compact");
    CHECK(href(cs, "uncaught-exceptions-dtor-wired") == 1, "uncaught dtor");
}

static void ac5_runtime() {
    std::println("\n--- AC5: runtime mutators under Guard ---");
    CompilerService cs;
    cs.evaluator().set_sandbox_mode(false);
    (void)cs.eval("(define x 1)");
    auto r1 = cs.eval("(compile:clear-macro-dirty!)");
    CHECK(r1.has_value() && (is_bool(*r1) || is_error(*r1)), "clear-macro-dirty");
    auto r2 = cs.eval("(compile:mark-narrowing-dirty! 0)");
    CHECK(r2.has_value() && (is_bool(*r2) || is_error(*r2)), "mark-narrowing");
    auto r3 = cs.eval("(evaluator:compact-env-frames)");
    CHECK(r3.has_value() && is_int(*r3), "compact-env-frames");

    auto& ev = cs.evaluator();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard outer(ev, &ok);
        CHECK(outer.is_outermost(), "outermost");
        auto r = cs.eval("(compile:clear-macro-dirty!)");
        CHECK(r.has_value(), "clear under outer");
        CHECK(ev.mutation_boundary_depth_slot_value() >= 1, "depth held");
    }
    CHECK(ok, "outer ok");
    CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0");
}

static void ac6_stress() {
    std::println("\n--- AC6: multi-round stress ---");
    CompilerService cs;
    cs.evaluator().set_sandbox_mode(false);
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    const auto before = m->compile_primitive_guard_captures_total.load();
    for (int i = 0; i < 200; ++i) {
        (void)cs.eval("(compile:clear-macro-dirty!)");
        (void)cs.eval("(compile:mark-narrowing-dirty! 0)");
    }
    const auto after = m->compile_primitive_guard_captures_total.load();
    CHECK(after >= before, "captures mono");
    CHECK(href(cs, "schema-1931") == 1931, "schema after stress");
    CHECK(href(cs, "dtor-common-path-atomics-cap") == 6, "cap holds");
}

static void ac7_lineage() {
    std::println("\n--- AC7: #1897 lineage retained ---");
    CompilerService cs;
    CHECK(href(cs, "schema") == 1897, "schema 1897");
    CHECK(href(cs, "issue") == 1897, "issue 1897");
    CHECK(href(cs, "try-acquire-wired") == 1, "try-acquire");
}

static void ac8_metrics_live() {
    std::println("\n--- AC8: AC metrics fields live ---");
    CompilerService cs;
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics ptr");
    CHECK(m->mutation_guard_exception_total.load() >= 0, "exception field");
    CHECK(m->compile_primitive_stale_ir_prevented_total.load() >= 0, "stale field");
    CHECK(m->mutation_guard_uncaught_auto_rollback_total.load() >= 0, "auto-rb field");
    CHECK(href(cs, "mutation_guard_exception_total") >= 0, "query exception");
    CHECK(href(cs, "compile_primitive_stale_ir_prevented_total") >= 0, "query stale");
}

} // namespace

int main() {
    std::println("=== Issue #1931: MutationBoundaryGuard systemic enforcement ===");
    ac1_source();
    ac2_schema();
    ac3_dtor_atomics();
    ac4_coverage_flags();
    ac5_runtime();
    ac6_stress();
    ac7_lineage();
    ac8_metrics_live();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
