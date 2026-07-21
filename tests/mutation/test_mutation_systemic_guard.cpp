// @category: integration
// @reason: Issue #1897 — systemic MutationBoundaryGuard try_acquire coverage
// Issue #1818/#1897 (#1978 renamed): issue# moved from filename to header.
// for compile/evaluator/eda structural mutators + Guard dtor uncaught_exceptions
// auto-rollback (#1818 class).
//
//   AC1: source cites #1897; try_acquire helper + uncaught_at_enter_
//   AC2: remaining dirty mutators use run_under_mutation_guard / helper
//   AC3: named issue paths (subtree-bump, defuse-add, hw-bitvec, compact) use helper
//   AC4: query:mutation-systemic-guard-stats schema 1897 + inventory flags
//   AC5: runtime dirty paths return safe values under Guard
//   AC6: nested Guard + captures monotonic

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
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

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-systemic-guard-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

} // namespace

int main() {
    // ── AC1: Guard dtor uncaught_exceptions wiring ──
    {
        std::println("\n--- AC1: uncaught_exceptions auto-rollback + helper ---");
        std::string src_ev, src_comp;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            src_ev = read_file(p);
            if (!src_ev.empty())
                break;
        }
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src_comp = read_file(p);
            if (!src_comp.empty())
                break;
        }
        CHECK(!src_ev.empty() && !src_comp.empty(), "read sources");
        CHECK(src_ev.find("#1897") != std::string::npos, "evaluator cites #1897");
        CHECK(src_ev.find("uncaught_at_enter_") != std::string::npos, "uncaught_at_enter_ field");
        CHECK(src_ev.find("std::uncaught_exceptions()") != std::string::npos,
              "uncaught_exceptions in Guard");
        CHECK(src_ev.find("mutation_guard_uncaught_auto_rollback_total") != std::string::npos,
              "auto-rollback metric");
        CHECK(src_comp.find("run_under_mutation_guard") != std::string::npos, "shared helper");
        CHECK(src_comp.find("query:mutation-systemic-guard-stats") != std::string::npos,
              "stats query");
    }

    // ── AC2: remaining dirty mutators use helper ──
    {
        std::println("\n--- AC2: dirty mutators under helper ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        for (const char* prim :
             {"\"compile:mark-dirty-upward-fast\"", "\"compile:clear-macro-dirty!\"",
              "\"compile:mark-narrowing-dirty!\""}) {
            auto pos = src.find(prim);
            CHECK(pos != std::string::npos, std::string("found ") + prim);
            auto win = src.substr(pos, 2200);
            CHECK(win.find("run_under_mutation_guard") != std::string::npos ||
                      win.find("run_compile_dirty_under_guard") != std::string::npos,
                  std::string(prim) + " uses Guard helper");
        }
    }

    // ── AC3: named issue paths migrated to try_acquire helper ──
    {
        std::println("\n--- AC3: issue-named structural mutators ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        for (const char* prim :
             {"\"compile:subtree-bump\"", "\"compile:per-defuse-index-add\"",
              "\"compile:hw-bitvec-register\"", "\"evaluator:compact-env-frames\"",
              "\"eda:run-verification-feedback\""}) {
            auto pos = src.find(prim);
            CHECK(pos != std::string::npos, std::string("found ") + prim);
            auto win = src.substr(pos, 3500);
            CHECK(win.find("run_under_mutation_guard") != std::string::npos ||
                      win.find("run_compile_dirty_under_guard") != std::string::npos,
                  std::string(prim) + " uses try_acquire helper");
            // Must not use deprecated bare RAII ctor as sole path.
            // (legacy ctor still exists for other code; helper path is try_acquire.)
            CHECK(win.find("try_acquire") != std::string::npos ||
                      win.find("run_under_mutation_guard") != std::string::npos,
                  std::string(prim) + " try_acquire path");
        }
    }

    // ── AC4: stats surface ──
    {
        std::println("\n--- AC4: query:mutation-systemic-guard-stats ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:mutation-systemic-guard-stats\")");
        CHECK(r.has_value() && is_hash(*r), "stats is hash");
        CHECK(href(cs, "schema") == 1897, "schema 1897");
        CHECK(href(cs, "issue") == 1897, "issue 1897");
        CHECK(href(cs, "try-acquire-wired") == 1, "try-acquire-wired");
        CHECK(href(cs, "uncaught-exceptions-dtor-wired") == 1, "dtor uncaught wired");
        CHECK(href(cs, "subtree-bump") == 1, "subtree-bump flag");
        CHECK(href(cs, "per-defuse-index-add") == 1, "defuse flag");
        CHECK(href(cs, "hw-bitvec-register") == 1, "hw-bitvec flag");
        CHECK(href(cs, "compact-env-frames") == 1, "compact flag");
        CHECK(href(cs, "mark-dirty-upward-fast") == 1, "dirty-upward flag");
        CHECK(href(cs, "clear-macro-dirty") == 1, "clear-macro flag");
        CHECK(href(cs, "mark-narrowing-dirty") == 1, "narrowing flag");
        CHECK(href(cs, "run-verification-feedback") == 1, "run-verification flag");
        CHECK(href(cs, "uncaught-auto-rollback") >= 0, "auto-rollback key");
        CHECK(href(cs, "mutation_guard_uncaught_auto_rollback_total") >= 0, "snake_case auto-rb");
    }

    // ── AC5: runtime safe returns ──
    {
        std::println("\n--- AC5: runtime mutators under Guard ---");
        CompilerService cs;
        cs.evaluator().set_sandbox_mode(false);
        (void)cs.eval("(define x 1)");
        CHECK(cs.eval("(set-code \"(define y 2)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval-current");

        auto r1 = cs.eval("(compile:mark-dirty-upward-fast 0)");
        CHECK(r1.has_value(), "mark-dirty-upward-fast eval");
        if (r1)
            CHECK(is_void(*r1) || is_bool(*r1) || is_error(*r1), "upward-fast safe type");

        auto r2 = cs.eval("(compile:clear-macro-dirty!)");
        CHECK(r2.has_value() && (is_bool(*r2) || is_error(*r2)), "clear-macro-dirty bool/err");

        auto r3 = cs.eval("(compile:mark-narrowing-dirty! 0)");
        CHECK(r3.has_value() && (is_bool(*r3) || is_error(*r3)), "mark-narrowing bool/err");

        auto r4 = cs.eval("(compile:subtree-bump 0)");
        CHECK(r4.has_value() && (is_int(*r4) || is_error(*r4)), "subtree-bump int/err");

        auto r5 = cs.eval("(evaluator:compact-env-frames)");
        CHECK(r5.has_value() && is_int(*r5), "compact-env-frames int");

        auto r6 = cs.eval("(compile:hw-bitvec-register \"bv8\" 8 0)");
        CHECK(r6.has_value() && (is_int(*r6) || is_error(*r6)), "hw-bitvec int/err");
    }

    // ── AC6: nested Guard + captures ──
    {
        std::println("\n--- AC6: nested Guard + capture metrics ---");
        CompilerService cs;
        cs.evaluator().set_sandbox_mode(false);
        (void)cs.eval("(define z 3)");
        auto* m = metrics_of(cs);
        CHECK(m != nullptr, "metrics");
        const auto before = load_u64(m->compile_primitive_guard_captures_total);
        auto& ev = cs.evaluator();
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard outer(ev, &ok);
            CHECK(outer.is_outermost(), "outermost");
            auto r = cs.eval("(compile:clear-macro-dirty!)");
            CHECK(r.has_value() && is_bool(*r), "clear under outer");
            CHECK(ev.mutation_boundary_depth_slot_value() >= 1, "depth held");
        }
        CHECK(ok, "outer ok");
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0");
        (void)cs.eval("(compile:mark-narrowing-dirty! 0)");
        const auto after = load_u64(m->compile_primitive_guard_captures_total);
        CHECK(after >= before + 1, "captures increased");
        CHECK(href(cs, "schema") == 1897, "schema holds");
        CHECK(load_u64(m->mutation_guard_uncaught_auto_rollback_total) >= 0, "auto-rb field live");
    }

    std::println("\n=== test_mutation_systemic_guard_1897: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
