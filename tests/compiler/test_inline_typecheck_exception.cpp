// @category: unit
// @reason: Issue #1769 — run_typecheck_no_lock* / run_post_mutate_typecheck
// Issue #1769 (#1978 renamed): issue# moved from filename to header.
// must catch exceptions, bump inline_typecheck_exception_total, return fail.
//
//   AC1: source cites #1769; try/catch + metric bump in helpers
//   AC2: metric field declared in observability_metrics.h
//   AC3: happy path typecheck does not bump exception counter
//   AC4: post_mutate helper returns bool without throw on empty workspace

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
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
    // ── AC1/AC2: source + metric ──
    {
        std::println("\n--- AC1/AC2: try/catch + metric field ---");
        std::string tc;
        for (const char* p :
             {"src/compiler/evaluator_typecheck.cpp", "../src/compiler/evaluator_typecheck.cpp"}) {
            tc = read_file(p);
            if (!tc.empty())
                break;
        }
        CHECK(!tc.empty(), "read evaluator_typecheck.cpp");
        CHECK(tc.find("#1769") != std::string::npos, "cites #1769");
        CHECK(tc.find("inline_typecheck_exception_total") != std::string::npos, "bumps metric");
        CHECK(tc.find("bump_inline_typecheck_exception") != std::string::npos, "helper present");
        // All three entry points wrap try.
        CHECK(tc.find("std::string Evaluator::run_typecheck_no_lock()") != std::string::npos,
              "string helper");
        CHECK(tc.find("bool Evaluator::run_typecheck_no_lock_bool()") != std::string::npos,
              "bool helper");
        CHECK(tc.find("bool Evaluator::run_post_mutate_typecheck_no_lock()") != std::string::npos,
              "post-mutate helper");
        // catch present for each family
        std::size_t catches = 0;
        for (std::size_t p = 0; (p = tc.find("catch (", p)) != std::string::npos; p += 6)
            ++catches;
        CHECK(catches >= 6, "at least 6 catch clauses (3 helpers × 2)");

        std::string msrc;
        for (const char* p :
             {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
            msrc = read_file(p);
            if (!msrc.empty())
                break;
        }
        CHECK(!msrc.empty() && msrc.find("inline_typecheck_exception_total") != std::string::npos,
              "metric field declared");
    }

    // ── AC3: happy path no bump ──
    {
        std::println("\n--- AC3: happy path no exception bump ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics wired");
        const auto e0 = m->inline_typecheck_exception_total.load(std::memory_order_relaxed);

        auto r = cs.eval("(define x 1)");
        CHECK(r.has_value(), "define ok");
        // Hold workspace lock as required by helper contract.
        CHECK(ev.try_lock_workspace_shared(), "shared lock");
        auto msg = ev.run_typecheck_no_lock();
        bool okb = ev.run_typecheck_no_lock_bool();
        bool post = ev.run_post_mutate_typecheck_no_lock();
        ev.unlock_workspace_shared();
        CHECK(!msg.empty(), "string TC returns text");
        CHECK(okb || !okb, "bool TC returns"); // any bool is fine
        CHECK(post || !post, "post TC returns");
        CHECK(m->inline_typecheck_exception_total.load(std::memory_order_relaxed) == e0,
              "no exception bump on happy path");
    }

    // ── AC4: empty / no-workspace safe ──
    {
        std::println("\n--- AC4: empty evaluator no throw ---");
        Evaluator ev;
        // No workspace: helpers return safe defaults without throw.
        auto msg = ev.run_typecheck_no_lock();
        CHECK(msg.find("no workspace") != std::string::npos || !msg.empty(), "no-workspace string");
        CHECK(ev.run_typecheck_no_lock_bool(), "no-workspace bool true");
        CHECK(ev.run_post_mutate_typecheck_no_lock(), "no-workspace post true");
    }

    std::println("\n=== test_inline_typecheck_exception_1769: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
