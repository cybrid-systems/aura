// @category: integration
// @reason: Issue #1612 — fiber resume/steal/GC MacroIntroduced marker
// Issue #1490/#1592/#1608/#1612 (#1978 renamed): issue# moved from filename to header.
// refresh + provenance repin (refine #1608 / #1592 / #1490).
//
//   AC1: resume/steal/GC paths wire macro refresh helpers
//   AC2: refresh_stale_macro_frames + probe_and_repin_macro_provenance
//   AC3: metrics macro_stale_ref_prevented + macro_provenance_repin_total
//   AC4: steal + GC + concurrent mutate on macro workspace (stress)
//   AC5: schema 1612 on query:post-steal-closed-loop-stats
//   AC6: complete_post_resume_steal_refresh advances macro invoke count

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:post-steal-closed-loop-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

static bool setup_macro_ws(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(d 1) (d 2) (d 3) "
                 "(define base 10) (+ base 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void ac1_wire_flags() {
    std::println("\n--- AC1: macro refresh wire flags ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    CHECK(href(cs, "macro-refresh-helper-wired") == 1, "macro-refresh-helper-wired");
    CHECK(href(cs, "macro-provenance-probe-wired") == 1, "macro-provenance-probe-wired");
    CHECK(href(cs, "gc-compact-macro-refresh-wired") == 1, "gc-compact-macro-refresh-wired");
    CHECK(href(cs, "resume-path-wired") == 1, "resume-path-wired lineage");
}

static void ac2_helpers() {
    std::println("\n--- AC2: refresh_stale_macro_frames + probe ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto& ev = cs.evaluator();
    const auto inv0 = ev.get_macro_refresh_invoke_count();
    const auto n = ev.refresh_stale_macro_frames(0, 0);
    CHECK(n >= 0, "refresh_stale_macro_frames ok");
    CHECK(ev.get_macro_refresh_invoke_count() == inv0 + 1, "macro_refresh_invoke +1");
    ev.probe_and_repin_macro_provenance();
    CHECK(true, "probe_and_repin_macro_provenance no crash");
}

static void ac3_ac5_metrics_schema() {
    std::println("\n--- AC3/AC5: schema 1612 + AC metrics ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto& ev = cs.evaluator();
    (void)ev.refresh_stale_macro_frames(0, 0);
    ev.probe_and_repin_macro_provenance();
    ev.complete_post_resume_steal_refresh(nullptr);

    auto h = cs.eval("(engine:metrics \"query:post-steal-closed-loop-stats\")");
    CHECK(h && is_hash(*h), "hash");
    // Schema lineage: 1612 → 1631 (fiber lifecycle mandate).
    CHECK(href(cs, "schema") == 1631 || href(cs, "schema") == 1612, "schema 1631|1612");
    CHECK(href(cs, "issue") == 1631 || href(cs, "issue") == 1612, "issue 1631|1612");
    CHECK(href(cs, "macro_stale_ref_prevented") >= 0 || href(cs, "macro-stale-ref-prevented") >= 0,
          "macro_stale_ref_prevented");
    CHECK(href(cs, "macro_provenance_repin_total") >= 0 ||
              href(cs, "macro-provenance-repin-total") >= 0,
          "macro_provenance_repin_total");
    CHECK(href(cs, "macro-refresh-invoke-count") >= 1, "macro-refresh-invoke-count");
}

static void ac6_complete_advances() {
    std::println("\n--- AC6: complete_post_resume_steal_refresh advances macro ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto& ev = cs.evaluator();
    const auto inv0 = ev.get_macro_refresh_invoke_count();
    const auto probe0 = load_u64(metrics_of(cs)->macro_provenance_probe_total);
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(ev.get_macro_refresh_invoke_count() >= inv0 + 1,
          "complete advances macro_refresh_invoke");
    CHECK(load_u64(metrics_of(cs)->macro_provenance_probe_total) >= probe0 + 1,
          "complete advances macro_provenance_probe");
}

static void ac4_stress() {
    std::println("\n--- AC4: steal + GC + concurrent mutate stress ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto& ev = cs.evaluator();
    std::atomic<int> ok{1};
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        for (int i = 0; i < 200; ++i) {
            ev.bump_defuse_version_for_test();
            (void)ev.refresh_stale_frames_after_steal(0, 0);
            (void)ev.refresh_stale_macro_frames(0, 0);
            ev.probe_and_repin_macro_provenance();
        }
    });
    threads.emplace_back([&] {
        for (int i = 0; i < 200; ++i) {
            ev.complete_post_resume_steal_refresh(nullptr);
            ev.on_arena_compact_hook();
        }
    });
    threads.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
            (void)cs.eval("(mutate:rebind \"base\" \"42\")");
            (void)cs.eval("(eval-current)");
        }
    });
    for (auto& t : threads)
        t.join();
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value() && ok.load(), "eval ok after concurrent stress");
    CHECK(href(cs, "schema") == 1631 || href(cs, "schema") == 1612, "schema holds after stress");
}

} // namespace

int main() {
    std::println("=== Issue #1612: fiber MacroIntroduced hygiene refresh ===");
    ac1_wire_flags();
    ac2_helpers();
    ac3_ac5_metrics_schema();
    ac6_complete_advances();
    ac4_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
