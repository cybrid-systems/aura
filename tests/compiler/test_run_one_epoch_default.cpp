// @category: unit
// @reason: Issue #2822 — run_one must sync JITFriendly pipeline epoch even
// when set_pipeline_mutation_epoch was never called (TLS default 0).
//
//   AC1: source auto-wires from current_mutation_epoch; floor; unset metric
//   AC2: without set_pipeline_mutation_epoch, pass.pipeline_epoch_hint() != 0
//   AC3: pipeline_epoch_sync_total + unset_runs advance per run_one
//   AC4: schema-2822 query; this suite + linter; no docs/design/2822-*

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.pass_manager;
import aura.compiler.pass_pipeline_core;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;
import aura.core;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::pipeline_epoch_sync_total;
using aura::compiler::pipeline_epoch_unset_runs_total;
using aura::compiler::pipeline_mutation_epoch;
using aura::compiler::run_one;
using aura::compiler::set_pipeline_mutation_epoch;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::ir::IRFunction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:production-sweep-1321-1324-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static IRModule make_tiny_mod() {
    IRModule mod;
    IRFunction fn;
    fn.name = "t";
    fn.entry_block = 0;
    fn.local_count = 2;
    fn.blocks.push_back({0, {}, {}});
    fn.blocks[0].instructions.push_back({IROpcode::ConstI64, {0, 1, 0, 0}});
    fn.blocks[0].instructions.push_back({IROpcode::Return, {0, 0, 0, 0}});
    mod.add_function(std::move(fn));
    return mod;
}

} // namespace

int run_test_run_one_epoch_default() {
    std::println("=== Issue #2822: run_one epoch default auto-wire ===");
    CHECK(true, "ac2822: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: source cites auto-wire + unset metric ---");
        auto core = read_file("src/compiler/pass_pipeline_core.ixx");
        CHECK(!core.empty(), "AC1: pass_pipeline_core readable");
        // Prefer definition window (after "execute a single pass"), not the
        // forward declaration near run_pipeline.
        auto pos = core.find("execute a single pass");
        if (pos == std::string::npos)
            pos = core.rfind("bool run_one");
        CHECK(pos != std::string::npos, "AC1: run_one present");
        // Window must cover yield (#2823) + epoch (#2822/#2827) blocks.
        auto win = core.substr(pos, 4000);
        CHECK(win.find("Issue #2822") != std::string::npos, "AC1: cites #2822");
        CHECK(win.find("pipeline_epoch_unset_runs_total") != std::string::npos,
              "AC1: unset_runs metric");
        CHECK(win.find("current_mutation_epoch") != std::string::npos, "AC1: process epoch");
        CHECK(win.find("kPipelineEpochBaseFloor") != std::string::npos, "AC1: base floor");
        // Must always set_pipeline_epoch (no silent skip-only path).
        CHECK(win.find("pass.set_pipeline_epoch(epoch)") != std::string::npos ||
                  win.find("set_pipeline_epoch(epoch)") != std::string::npos,
              "AC1: always set_pipeline_epoch");
        CHECK(core.find("pipeline_epoch_unset_runs_total") != std::string::npos,
              "AC1: export unset total");
    }

    // ── AC2/AC3: runtime without set_pipeline_mutation_epoch ──
    {
        std::println("\n--- AC2/AC3: auto-wire when TLS epoch unset ---");
        // Clear TLS epoch to simulate callers that never wire.
        set_pipeline_mutation_epoch(0);
        // set_pipeline_mutation_epoch(0) still bumps sync_total — capture after.
        const auto sync0 = pipeline_epoch_sync_total.load();
        const auto unset0 = pipeline_epoch_unset_runs_total.load();

        // Force TLS back to 0 without relying on set(0) semantics if it
        // stores 0 (it does). pipeline_mutation_epoch should be 0.
        CHECK(pipeline_mutation_epoch() == 0, "AC2: TLS epoch 0 before run");

        ConstantFoldingWrap pass;
        CHECK(pass.pipeline_epoch_hint() == 0, "AC2: pass default hint 0");
        auto mod = make_tiny_mod();
        CHECK(run_one(mod, pass), "AC2: run_one succeeds");
        CHECK(pass.pipeline_epoch_hint() != 0,
              std::format("AC2: hint non-zero after run (got {})", pass.pipeline_epoch_hint()));
        CHECK(pass.pipeline_epoch_hint() >= aura::compiler::kPipelineEpochBaseFloor,
              "AC2: hint >= base floor");

        const auto sync1 = pipeline_epoch_sync_total.load();
        const auto unset1 = pipeline_epoch_unset_runs_total.load();
        CHECK(sync1 > sync0, std::format("AC3: sync_total advanced (Δ={})", sync1 - sync0));
        CHECK(unset1 > unset0, std::format("AC3: unset_runs advanced (Δ={})", unset1 - unset0));

        // Explicit wire path: non-zero TLS still syncs without extra unset.
        set_pipeline_mutation_epoch(42);
        ConstantFoldingWrap pass2;
        const auto unset2 = pipeline_epoch_unset_runs_total.load();
        auto mod2 = make_tiny_mod();
        CHECK(run_one(mod2, pass2), "AC3: run_one with explicit epoch");
        CHECK(pass2.pipeline_epoch_hint() == 42, "AC3: explicit epoch 42 propagated");
        CHECK(pipeline_epoch_unset_runs_total.load() == unset2,
              "AC3: no unset bump when TLS already set");
        // Restore clean TLS for other tests.
        set_pipeline_mutation_epoch(0);
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2822 query keys ---");
        CompilerService cs;
        CHECK(href(cs, "schema-2822") == 2822, "AC4: schema-2822");
        CHECK(href(cs, "issue-2822") == 2822, "AC4: issue-2822");
        CHECK(href(cs, "pipeline-epoch-auto-wire-wired") == 1, "AC4: wired");
        CHECK(href(cs, "pipeline-epoch-unset-runs-total") >= 0, "AC4: unset total");
        CHECK(href(cs, "pipeline-epoch-base-floor") == 1, "AC4: base floor key");
        auto obs = read_file("src/compiler/evaluator_primitives_stdlib_review.cpp");
        CHECK(obs.find("schema-2822") != std::string::npos, "AC4: obs schema-2822");
        CHECK(obs.find("pipeline-epoch-unset-runs-total") != std::string::npos,
              "AC4: obs unset key");
    }

    std::println("\n=== #2822 run_one epoch default: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_run_one_epoch_default();
}
#endif
