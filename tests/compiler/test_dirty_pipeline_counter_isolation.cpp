// @category: unit
// @reason: Issue #2824 — run_dirty_pipeline clean_skips/dirty_runs must use
// TLS attribution so concurrent record_dirty_* does not contaminate aggregates.
//
//   AC1: source TLS enter/leave; contamination metric; cites #2824
//   AC2: multi-pass pipeline attributes only this pipeline's TLS skips
//   AC3: concurrent external record_dirty_block_skip does not inflate aggregate
//   AC4: schema-2824 query; this suite + linter; no docs/design/2824-*

#include "test_harness.hpp"
#include "compiler/jit_typed_mutation_stats.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.pass_manager;
import aura.compiler.pass_pipeline_core;
import aura.compiler.ir_soa;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::IRModuleV2;
using aura::compiler::pipeline_dirty_counter_concurrent_contamination_total;
using aura::compiler::run_dirty_pipeline;
using aura::compiler::run_dirty_pipeline_clean_skips_total;
using aura::compiler::run_dirty_pipeline_dirty_runs_total;
using aura::compiler::SoaDirtyAwarePass;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;
namespace mig = aura::compiler::ir_soa_migration;

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

static std::int64_t href(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:pass-pipeline-dirtyaware-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Controlled SoaDirtyAwarePass: records fixed skip/run counts via mig APIs.
struct CountingDirtyPass {
    std::uint64_t skips = 0;
    std::uint64_t runs = 0;
    void run_dirty(IRModuleV2&) {
        if (skips)
            mig::record_dirty_block_skip(skips);
        if (runs)
            mig::record_dirty_block_run(runs);
    }
    bool has_error() const { return false; }
};
static_assert(SoaDirtyAwarePass<CountingDirtyPass>);

} // namespace

int run_test_dirty_pipeline_counter_isolation() {
    std::println("=== Issue #2824: dirty pipeline counter isolation ===");
    CHECK(true, "ac2824: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: TLS attribution + contamination metric ---");
        auto core = read_file("src/compiler/pass_pipeline_core.ixx");
        auto stats = read_file("src/compiler/jit_typed_mutation_stats.h");
        CHECK(!core.empty() && !stats.empty(), "AC1: sources readable");
        auto pos = core.find("bool run_dirty_pipeline");
        CHECK(pos != std::string::npos, "AC1: run_dirty_pipeline present");
        auto win = core.substr(pos, 3500);
        CHECK(win.find("Issue #2824") != std::string::npos, "AC1: cites #2824");
        CHECK(win.find("enter_dirty_pipeline_attribution") != std::string::npos, "AC1: enter TLS");
        CHECK(win.find("leave_dirty_pipeline_attribution") != std::string::npos, "AC1: leave TLS");
        CHECK(win.find("pipeline_dirty_counter_concurrent_contamination_total") !=
                  std::string::npos,
              "AC1: contamination metric");
        CHECK(stats.find("g_tls_dirty_pipeline_skips") != std::string::npos ||
                  stats.find("g_dirty_pipeline_tls_depth") != std::string::npos,
              "AC1: TLS fields in stats header");
        CHECK(stats.find("enter_dirty_pipeline_attribution") != std::string::npos,
              "AC1: enter API");
        CHECK(stats.find("Issue #2824") != std::string::npos, "AC1: stats cites #2824");
    }

    // ── AC2: multi-pass TLS attribution ──
    {
        std::println("\n--- AC2: multi-pass pipeline TLS sum ---");
        IRModuleV2 mod; // unused by CountingDirtyPass
        CountingDirtyPass a{.skips = 3, .runs = 1};
        CountingDirtyPass b{.skips = 5, .runs = 2};
        const auto sk0 = run_dirty_pipeline_clean_skips_total.load();
        const auto rn0 = run_dirty_pipeline_dirty_runs_total.load();
        CHECK(run_dirty_pipeline(mod, a, b), "AC2: multi-pass ok");
        const auto sk1 = run_dirty_pipeline_clean_skips_total.load();
        const auto rn1 = run_dirty_pipeline_dirty_runs_total.load();
        CHECK(sk1 == sk0 + 8, std::format("AC2: skips +8 (got Δ={})", sk1 - sk0));
        CHECK(rn1 == rn0 + 3, std::format("AC2: runs +3 (got Δ={})", rn1 - rn0));
    }

    // ── AC3: concurrent contamination isolation ──
    {
        std::println("\n--- AC3: unattributed process bumps do not inflate aggregate ---");
        // Simulate concurrent pipelines: dual-write TLS via record_*, then
        // bump process-wide counters only (no TLS) mid-pass.
        struct ContaminatedPass {
            void run_dirty(IRModuleV2&) {
                mig::record_dirty_block_skip(10);
                mig::record_dirty_block_run(4);
                // Unattributed process-wide noise (other thread / pipeline).
                mig::dirty_block_driven_skips.fetch_add(100, std::memory_order_relaxed);
                mig::dirty_block_driven_runs.fetch_add(50, std::memory_order_relaxed);
            }
            bool has_error() const { return false; }
        };
        static_assert(SoaDirtyAwarePass<ContaminatedPass>);

        IRModuleV2 mod;
        ContaminatedPass p;
        const auto sk0 = run_dirty_pipeline_clean_skips_total.load();
        const auto rn0 = run_dirty_pipeline_dirty_runs_total.load();
        const auto c0 = pipeline_dirty_counter_concurrent_contamination_total.load();

        CHECK(run_dirty_pipeline(mod, p), "AC3: pipeline under process-wide noise");

        const auto sk1 = run_dirty_pipeline_clean_skips_total.load();
        const auto rn1 = run_dirty_pipeline_dirty_runs_total.load();
        const auto c1 = pipeline_dirty_counter_concurrent_contamination_total.load();
        // Aggregate only TLS-attributed (10/4), not the +100/+50 noise.
        CHECK(sk1 == sk0 + 10, std::format("AC3: skips only +10 (got Δ={})", sk1 - sk0));
        CHECK(rn1 == rn0 + 4, std::format("AC3: runs only +4 (got Δ={})", rn1 - rn0));
        // process delta 110/54 vs TLS 10/4 → contamination 100+50.
        CHECK(c1 >= c0 + 150, std::format("AC3: contamination +≥150 (got Δ={})", c1 - c0));
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2824 query keys ---");
        aura::compiler::CompilerService cs;
        CHECK(href(cs, "schema-2824") == 2824, "AC4: schema-2824");
        CHECK(href(cs, "issue-2824") == 2824, "AC4: issue-2824");
        CHECK(href(cs, "dirty-pipeline-tls-attribution-wired") == 1, "AC4: wired");
        CHECK(href(cs, "pipeline-dirty-counter-concurrent-contamination-total") >= 0,
              "AC4: contamination key");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(obs.find("schema-2824") != std::string::npos, "AC4: obs schema-2824");
        CHECK(obs.find("pipeline-dirty-counter-concurrent-contamination-total") !=
                  std::string::npos,
              "AC4: obs contamination key");
    }

    std::println("\n=== #2824 dirty pipeline counter isolation: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dirty_pipeline_counter_isolation();
}
#endif
