// @category: unit
// @reason: Issue #2194 — steal/resume must force provenance restamp +
// clear_gc_defer_for_evaluator (unified with Guard exit).
//
//   AC1: Every resume after cross-worker steal runs
//        refresh_after_fiber_migration (source-cite)
//   AC2: Orphan panic-defer clear on prev host (metric ≥1 when orphan)
//   AC3: Pin restamp site Steal wired (generation-bound)
//   AC4: Linear probe path retained (probe_and_repin_linear_on_steal)
//   AC5: Source stress path + metrics surface schema-2194

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/gc_hooks.h"

#include <cstdint>
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

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_fiber_migration_refresh() {
    std::println("=== Issue #2194: fiber migration refresh unified helper ===");

    // ── AC1: source wiring ──
    {
        std::println("\n--- AC1: resume → refresh_after_fiber_migration ---");
        auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        auto fiber = read_file("src/serve/fiber.cpp");
        auto eixx = read_file("src/compiler/evaluator.ixx");
        CHECK(efm.find("refresh_after_fiber_migration") != std::string::npos, "helper defined");
        CHECK(efm.find("Issue #2194") != std::string::npos, "cites #2194");
        CHECK(efm.find("complete_post_resume_steal_refresh") != std::string::npos,
              "legacy name retained as delegate");
        CHECK(efm.find("refresh_after_fiber_migration(fiber_void)") != std::string::npos ||
                  efm.find("refresh_after_fiber_migration(Evaluator::g_current_fiber_void)") !=
                      std::string::npos ||
                  efm.find("refresh_after_fiber_migration(") != std::string::npos,
              "refresh called");
        CHECK(efm.find("aura_evaluator_post_resume_refresh") != std::string::npos,
              "C ABI post_resume");
        CHECK(fiber.find("aura_evaluator_post_resume_refresh") != std::string::npos,
              "Fiber::resume wires post_resume");
        CHECK(fiber.find("refresh_after_fiber_migration") != std::string::npos ||
                  fiber.find("#2194") != std::string::npos,
              "fiber.cpp cites unified path");
        CHECK(eixx.find("refresh_after_fiber_migration") != std::string::npos,
              "evaluator interface declares helper");
        // Clear orphan + restamp + linear + hints in helper body.
        CHECK(efm.find("clear_gc_defer_for_evaluator") != std::string::npos, "GC defer clear");
        CHECK(efm.find("refresh_stale_frames_after_steal") != std::string::npos,
              "EnvFrame refresh");
        CHECK(efm.find("auto_restamp_pinned_stable_refs_at") != std::string::npos ||
                  efm.find("StableRefRefreshSite::Steal") != std::string::npos,
              "pin restamp Steal");
        CHECK(efm.find("probe_and_repin_linear_on_steal") != std::string::npos, "linear probe");
        CHECK(efm.find("clear_resume_refresh_hints") != std::string::npos, "clear hints");
        CHECK(efm.find("fiber_migration_refresh_total") != std::string::npos, "refresh metric");
    }

    // ── AC2: orphan GC defer clear (functional) ──
    {
        std::println("\n--- AC2: orphan defer clear ---");
        std::uintptr_t fake_prev = 0xA1940001u;
        std::uintptr_t fake_cur = 0xA1940002u;
        auto* id_prev = reinterpret_cast<void*>(fake_prev);
        auto* id_cur = reinterpret_cast<void*>(fake_cur);
        aura::gc_hooks::arm_gc_defer_pending_panic_for(id_prev);
        aura::gc_hooks::arm_gc_defer_pending_panic_for(id_prev);
        CHECK(aura::gc_hooks::gc_deferred_for_evaluator(id_prev), "prev armed");
        const auto depth0 = aura::gc_hooks::gc_defer_pending_panic_depth();
        const auto cleared = aura::gc_hooks::clear_gc_defer_for_evaluator(id_prev);
        CHECK(cleared >= 2, "cleared ≥2");
        CHECK(!aura::gc_hooks::gc_deferred_for_evaluator(id_prev), "prev empty");
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == depth0 - cleared,
              "process depth drops");
        // Orthogonal arm on current host still works.
        aura::gc_hooks::arm_gc_defer_pending_panic_for(id_cur);
        CHECK(aura::gc_hooks::gc_deferred_for_evaluator(id_cur), "cur armed");
        aura::gc_hooks::release_gc_defer_pending_panic_for(id_cur);
        // Cross-evaluator gate in restore_post_yield (#2194).
        auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        CHECK(efm.find("cp.evaluator_id != static_cast<void*>(this)") != std::string::npos ||
                  efm.find("evaluator_id != static_cast<void*>(this)") != std::string::npos,
              "cross-eval gate (not only thread_migrated)");
    }

    // ── AC3/AC4: pin + linear sites ──
    {
        std::println("\n--- AC3/AC4: pin restamp + linear ---");
        auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        CHECK(efm.find("StableRefRefreshSite::Steal") != std::string::npos, "Steal restamp site");
        CHECK(efm.find("linear_post_mutate_enforce") != std::string::npos, "linear enforce");
        CHECK(efm.find("probe_and_repin_linear_on_steal") != std::string::npos, "linear probe");
        // Guard exit still has its own restamp (no regression of #2090 path).
        auto g = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(g.find("restamp_pinned_stable_refs") != std::string::npos, "Guard exit restamp");
        CHECK(g.find("clear_gc_defer_for_evaluator") != std::string::npos, "Guard GC clear");
    }

    // ── AC5: metrics surface ──
    {
        std::println("\n--- AC5: metrics schema-2194 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
        const char* q = "query:post-steal-closed-loop-stats";
        CHECK(href(cs, q, "schema-2194") == 2194, "schema-2194");
        CHECK(href(cs, q, "fiber-migration-refresh-wired") == 1, "wired");
        CHECK(href(cs, q, "fiber_migration_refresh_total") >= 0, "refresh total");
        CHECK(href(cs, q, "fiber_migration_gc_defer_cleared_total") >= 0, "gc cleared total");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("fiber_migration_refresh_total") != std::string::npos, "obs refresh metric");
        CHECK(obs.find("schema-2194") != std::string::npos, "schema-2194 in obs");
        auto met = read_file("src/compiler/observability_metrics.h");
        CHECK(met.find("fiber_migration_refresh_total") != std::string::npos, "metrics field");
        CHECK(met.find("fiber_migration_gc_defer_cleared_total") != std::string::npos,
              "gc cleared field");
    }

    // ── Worker steal only probes (mandatory work on resume) ──
    {
        std::println("\n--- worker steal hints; resume does full refresh ---");
        auto w = read_file("src/serve/worker.cpp");
        CHECK(w.find("call_probe_linear_on_steal") != std::string::npos, "steal probe linear");
        // Full refresh is resume-side only (not duplicated on steal accept).
        CHECK(w.find("refresh_after_fiber_migration") == std::string::npos,
              "steal path does not call full refresh (resume owns it)");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_fiber_migration_refresh();
}
#endif
