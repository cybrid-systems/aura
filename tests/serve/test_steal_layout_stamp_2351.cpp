// @category: unit
// @reason: Issue #2351 — steal-complete LayoutStamp / defuse dual-check
// before resume run (fail-closed after densify race on previous host).
//
//   AC1: Steal with matching stamp → no mismatch bump
//   AC2: Steal with mismatched stamp → steal-mismatch counter + dual-check
//   AC3: No stamp set → zero extra layout compare work (missing only if MB)
//   AC4: Schema additive + source-cite
//   AC5: Unit test (concurrent steal + densify stress soft-assert)

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "serve/fiber.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" void aura_evaluator_on_steal_complete(void* fiber_ptr) noexcept;
extern "C" void aura_evaluator_test_seed_yield_cp_and_steal_complete(void* fiber_ptr,
                                                                     void* eval_id) noexcept;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::YieldReason;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:stable-ref-stats-hash\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: matching stamp → no mismatch ──
static void ac1_matching_stamp() {
    std::println("\n--- AC1: matching stamp → no steal mismatch ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    const auto mm0 = metrics.layout_stamp_steal_mismatch_total.load();
    Fiber f([] {});
    // Stamp fiber with current evaluator stamp (match).
    const auto cur = ev.current_layout_stamp();
    f.set_resume_layout_stamp(cur.arena_id, cur.arena_gen, cur.flat_gen, cur.mutation_epoch,
                              cur.env_gen, cur.defuse_version, cur.shape_version);
    CHECK(f.has_resume_layout_stamp(), "AC1: stamp set");
    aura_evaluator_on_steal_complete(&f);
    CHECK(metrics.layout_stamp_steal_mismatch_total.load() == mm0,
          "AC1: matching stamp → no mismatch bump");
    // Stamp retained for resume path (not cleared at steal-complete).
    CHECK(f.has_resume_layout_stamp(), "AC1: stamp retained for resume dual-check");

    ev.set_compiler_metrics(nullptr);
}

// ── AC2: mismatched stamp → counter + force path ──
static void ac2_mismatched_stamp() {
    std::println("\n--- AC2: mismatched stamp → steal mismatch ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    const auto mm0 = metrics.layout_stamp_steal_mismatch_total.load();
    Fiber f([] {});
    const auto cur = ev.current_layout_stamp();
    // Corrupt arena_gen to force mismatch (simulates densify on previous host).
    f.set_resume_layout_stamp(cur.arena_id, cur.arena_gen + 99, cur.flat_gen, cur.mutation_epoch,
                              cur.env_gen, cur.defuse_version, cur.shape_version);
    aura_evaluator_on_steal_complete(&f);
    CHECK(metrics.layout_stamp_steal_mismatch_total.load() > mm0, "AC2: mismatch counter bumps");
    CHECK(f.has_resume_layout_stamp(), "AC2: stamp still present for resume");

    ev.set_compiler_metrics(nullptr);
}

// ── AC3: no stamp → zero extra compare work ──
static void ac3_no_stamp_zero_cost() {
    std::println("\n--- AC3: no stamp → zero mismatch / no missing without MB ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    const auto mm0 = metrics.layout_stamp_steal_mismatch_total.load();
    const auto miss0 = metrics.layout_stamp_steal_missing_total.load();
    Fiber f([] {});
    f.set_yield_reason(YieldReason::Explicit); // not MB
    CHECK(!f.has_resume_layout_stamp(), "AC3: no stamp");
    aura_evaluator_on_steal_complete(&f);
    CHECK(metrics.layout_stamp_steal_mismatch_total.load() == mm0, "AC3: no mismatch");
    CHECK(metrics.layout_stamp_steal_missing_total.load() == miss0,
          "AC3: no missing without MB expectation");

    // MB yield without stamp → missing counter.
    Fiber f2([] {});
    f2.set_yield_reason(YieldReason::MutationBoundary);
    aura_evaluator_on_steal_complete(&f2);
    CHECK(metrics.layout_stamp_steal_missing_total.load() > miss0,
          "AC3: MB yield without stamp → missing total");

    ev.set_compiler_metrics(nullptr);
}

// ── AC4: schema + source-cite ──
static void ac4_schema_and_source() {
    std::println("\n--- AC4: schema-2351 + source-cite ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2351") == 2351, "schema-2351");
    CHECK(href(cs, "issue-2351") == 2351, "issue-2351");
    CHECK(href(cs, "layout-stamp-steal-wired") == 1, "steal-wired");
    CHECK(href(cs, "layout-stamp-steal-mismatch-total") >= 0, "mismatch queryable");
    CHECK(href(cs, "layout-stamp-steal-missing-total") >= 0, "missing queryable");
    // Lineage
    CHECK(href(cs, "schema-2250") == 2250, "schema-2250 retained");
    CHECK(href(cs, "schema-2255") == 2255, "schema-2255 retained");

    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto met = read_file("src/compiler/observability_metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(fm.find("Issue #2351") != std::string::npos, "fiber_mutation cites #2351");
    CHECK(fm.find("layout_stamp_steal_mismatch_total") != std::string::npos,
          "steal mismatch bump site");
    CHECK(fm.find("has_resume_layout_stamp") != std::string::npos, "stamp gate");
    CHECK(fm.find("aura_evaluator_on_steal_complete") != std::string::npos, "steal-complete");
    CHECK(met.find("layout_stamp_steal_mismatch_total") != std::string::npos, "metrics field");
    CHECK(q.find("schema-2351") != std::string::npos, "query schema-2351");
    // Does not clear stamp at steal (resume retains fence).
    CHECK(fm.find("Does NOT clear the stamp") != std::string::npos ||
              fm.find("stamp retained") != std::string::npos ||
              fm.find("NOT clear") != std::string::npos,
          "does not clear stamp at steal-complete");
}

// ── AC5: dual-thread steal-complete stress (soft) ──
static void ac5_concurrent_stress() {
    std::println("\n--- AC5: concurrent steal-complete stress ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    const auto mm0 = metrics.layout_stamp_steal_mismatch_total.load();
    std::atomic<int> done{0};
    auto worker = [&](bool corrupt) {
        Fiber f([] {});
        const auto cur = ev.current_layout_stamp();
        f.set_resume_layout_stamp(cur.arena_id, cur.arena_gen + (corrupt ? 7 : 0), cur.flat_gen,
                                  cur.mutation_epoch, cur.env_gen, cur.defuse_version,
                                  cur.shape_version);
        for (int i = 0; i < 50; ++i)
            aura_evaluator_on_steal_complete(&f);
        done.fetch_add(1);
    };
    std::thread t1(worker, false);
    std::thread t2(worker, true);
    t1.join();
    t2.join();
    CHECK(done.load() == 2, "AC5: both workers finished");
    CHECK(metrics.layout_stamp_steal_mismatch_total.load() > mm0,
          "AC5: corrupt path produced mismatches under concurrency");

    ev.set_compiler_metrics(nullptr);
}

} // namespace

int run_test_steal_layout_stamp_2351() {
    std::println("=== Issue #2351: steal-complete LayoutStamp dual-check ===");
    ac1_matching_stamp();
    ac2_mismatched_stamp();
    ac3_no_stamp_zero_cost();
    ac4_schema_and_source();
    ac5_concurrent_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_steal_layout_stamp_2351();
}
#endif
