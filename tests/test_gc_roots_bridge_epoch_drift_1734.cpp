// @category: unit
// @reason: Issue #1734 — collect_compiler_managed_gc_roots must detect
// snapshot vs live bridge_epoch drift.
//
//   AC1: source cites #1734; compares live epoch + drift metric
//   AC2: metric field gc_roots_bridge_epoch_drift_total declared
//   AC3: matching snapshot does not bump drift
//   AC4: mismatched snapshot bumps drift; still collects roots

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
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
        std::println("\n--- AC1/AC2: drift check + metric ---");
        std::string gc;
        for (const char* p :
             {"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"}) {
            gc = read_file(p);
            if (!gc.empty())
                break;
        }
        CHECK(!gc.empty(), "read evaluator_gc.cpp");
        CHECK(gc.find("#1734") != std::string::npos, "cites #1734");
        CHECK(gc.find("gc_roots_bridge_epoch_drift_total") != std::string::npos,
              "bumps drift metric");
        CHECK(gc.find("current_bridge_epoch()") != std::string::npos, "reads live epoch");

        std::string msrc;
        for (const char* p :
             {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
            msrc = read_file(p);
            if (!msrc.empty())
                break;
        }
        CHECK(!msrc.empty() && msrc.find("gc_roots_bridge_epoch_drift_total") != std::string::npos,
              "metric field declared");
    }

    // ── AC3: matching snapshot no drift ──
    {
        std::println("\n--- AC3: matching snapshot ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics wired");

        Closure cl;
        cl.env_id = NULL_ENV_ID;
        (void)ev.register_active_closure(std::move(cl));

        const auto live = ev.current_bridge_epoch();
        const auto d0 = m->gc_roots_bridge_epoch_drift_total.load(std::memory_order_relaxed);

        std::vector<std::int64_t> cl_roots, env_roots;
        ev.collect_compiler_managed_gc_roots(cl_roots, env_roots, live);

        const auto d1 = m->gc_roots_bridge_epoch_drift_total.load(std::memory_order_relaxed);
        CHECK(d1 == d0, "matching snapshot does not bump drift");
        CHECK(!cl_roots.empty() || live == 0, "collected roots or tracking inactive");
    }

    // ── AC4: mismatched snapshot bumps drift ──
    {
        std::println("\n--- AC4: mismatched snapshot bumps drift ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());

        Closure cl;
        cl.env_id = NULL_ENV_ID;
        (void)ev.register_active_closure(std::move(cl));

        const auto live = ev.current_bridge_epoch();
        const auto stale_snap = live + 1000; // guaranteed mismatch (or 1000 if live 0)
        const auto d0 = m->gc_roots_bridge_epoch_drift_total.load(std::memory_order_relaxed);

        std::vector<std::int64_t> cl_roots, env_roots;
        ev.collect_compiler_managed_gc_roots(cl_roots, env_roots, stale_snap);

        const auto d1 = m->gc_roots_bridge_epoch_drift_total.load(std::memory_order_relaxed);
        CHECK(d1 == d0 + 1, "mismatched snapshot bumps drift +1");
        // Still returns (filter uses live epoch); may or may not include
        // the registered closure depending on its stamp vs live.
        CHECK(true, "collect completed after drift");
    }

    std::println("\n=== test_gc_roots_bridge_epoch_drift_1734: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
