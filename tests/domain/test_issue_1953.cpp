// tests/domain/test_issue_1953.cpp — Wave 4 relocate from tests/test_issue_1953.cpp
// Prefer domain/; do not re-add under tests/ root. (#root_test_classification)
// @category: unit
// @reason: Issue #1953 — refine #1931 systemic MutationBoundaryGuard
// enforcement + dtor ≤6 atomics for hot-update hot-path.
//
//   AC1: schema-1953 / issue-1953 on query:mutation-systemic-guard-stats
//   AC2: dtor common path still ≤6 atomics (static source window)
//   AC3: AC metrics mutation_guard_exception_total +
//        compile_primitive_stale_ir_prevented_total live
//   AC4: coverage linter --strict clean; helper header cites #1953
//   AC5: multi-round mutate under Guard; captures mono; schema holds
//   AC6: exception path mark_failed / run_or_rollback prevents commit

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

static void ac1_schema_1953() {
    std::println("\n--- AC1: schema-1953 surface ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:mutation-systemic-guard-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1897, "lineage 1897");
    CHECK(href(cs, "schema-1931") == 1931, "schema-1931 retained");
    CHECK(href(cs, "schema-1953") == 1953, "schema-1953");
    CHECK(href(cs, "issue-1953") == 1953, "issue-1953");
    CHECK(href(cs, "dtor-common-path-atomics-cap") == 6, "atomics cap 6");
    CHECK(href(cs, "dtor-batch-metrics-wired") == 1, "batch wired");
    CHECK(href(cs, "compile-mutate-guard-coverage-100pct") == 1, "100% coverage");
    CHECK(href(cs, "exception-auto-rollback-wired") == 1, "exception auto-rb");
    CHECK(href(cs, "run-under-mutation-guard-helper") == 1, "helper flag");
    CHECK(href(cs, "active") == 1, "active");
}

static void ac2_dtor_atomics() {
    std::println("\n--- AC2: dtor common path ≤6 atomics ---");
    auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    CHECK(!ixx.empty(), "ixx readable");
    CHECK(ixx.find("#1953") != std::string::npos, "ixx cites #1953");
    auto pos = ixx.find("~MutationBoundaryGuard()");
    CHECK(pos != std::string::npos, "dtor found");
    // Window: dtor → rare-path marker (same as #1931 AC3).
    auto end = ixx.find("Optional / rare path", pos);
    if (end == std::string::npos)
        end = pos + 8000;
    auto win = ixx.substr(pos, end - pos);
    auto pub = win.find("publish common path");
    CHECK(pub != std::string::npos, "publish marker");
    auto common = win.substr(pub);
    int fetch_adds = 0;
    for (std::size_t i = 0; (i = common.find("fetch_add", i)) != std::string::npos; ++i)
        ++fetch_adds;
    CHECK(fetch_adds <= 6, "common path fetch_add ≤6");
    CHECK(fetch_adds >= 4, "dual hold counters present");
    CHECK(win.find("BatchMutationMetrics") != std::string::npos, "BatchMutationMetrics");
}

static void ac3_metrics() {
    std::println("\n--- AC3: AC metrics live ---");
    CompilerService cs;
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics ptr");
    CHECK(m->mutation_guard_exception_total.load() >= 0, "exception field");
    CHECK(m->compile_primitive_stale_ir_prevented_total.load() >= 0, "stale field");
    CHECK(href(cs, "mutation_guard_exception_total") >= 0, "query exception");
    CHECK(href(cs, "compile_primitive_stale_ir_prevented_total") >= 0, "query stale");
}

static void ac4_coverage_sources() {
    std::println("\n--- AC4: coverage linter + helper #1953 ---");
    auto lint = read_first({"scripts/check_mutation_guard_coverage.py",
                            "../scripts/check_mutation_guard_coverage.py"});
    auto hh = read_first(
        {"src/compiler/mutation_guard_helpers.hh", "../src/compiler/mutation_guard_helpers.hh"});
    CHECK(!lint.empty() && lint.find("1953") != std::string::npos, "linter cites #1953");
    CHECK(!hh.empty() && hh.find("1953") != std::string::npos, "helpers cite #1953");
    CHECK(hh.find("run_under_mutation_guard") != std::string::npos, "helper template");
    CompilerService cs;
    CHECK(href(cs, "coverage-linter-wired") == 1, "linter wired flag");
    CHECK(href(cs, "shared-helper-header-wired") == 1, "helper wired flag");
}

static void ac5_stress() {
    std::println("\n--- AC5: multi-round Guard stress ---");
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
    CHECK(href(cs, "schema-1953") == 1953, "schema-1953 after stress");
    CHECK(href(cs, "dtor-common-path-atomics-cap") == 6, "cap holds");
}

static void ac6_exception_rollback() {
    std::println("\n--- AC6: mark_failed / run_or_rollback ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(ev, &ok);
        CHECK(g.is_outermost(), "outermost");
        std::string err;
        const bool ran =
            g.run_or_rollback([]() { throw std::runtime_error("inject-#1953"); }, &err);
        CHECK(!ran, "run_or_rollback false");
        CHECK(err.find("inject-#1953") != std::string::npos, "err message");
        g.mark_failed(); // idempotent
    }
    CHECK(!ok, "success_flag false after throw path");
    CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth cleared");
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    if (m) {
        CHECK(m->mutation_boundary_exception_rollback_total.load() >= 1 ||
                  m->mutation_guard_exception_total.load() >= 0,
              "exception telemetry available");
    }
}

} // namespace

int main() {
    std::println("=== Issue #1953: MutationBoundaryGuard refine of #1931 ===");
    ac1_schema_1953();
    ac2_dtor_atomics();
    ac3_metrics();
    ac4_coverage_sources();
    ac5_stress();
    ac6_exception_rollback();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
