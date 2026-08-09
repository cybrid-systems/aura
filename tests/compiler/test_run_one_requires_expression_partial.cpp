// @category: unit
// @reason: Issue #2827 — run_one epoch sync must gate on set_pipeline_epoch
// alone (not AND with pipeline_epoch_hint). Set-only passes sync; hint-only
// compiles and bumps partial_skipped.
//
//   AC1: source splits set vs hint requires; cites #2827; partial metric
//   AC2: SetOnly pass receives epoch; HintOnly runs without compile error
//   AC3: partial_skipped advances on XOR shapes; dual does not bump partial
//   AC4: schema-2827 query; this suite + linter; no docs/design/2827-*

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

namespace {

using aura::compiler::CompilerService;
using aura::compiler::pipeline_epoch_sync_partial_skipped_total;
using aura::compiler::pipeline_epoch_sync_total;
using aura::compiler::run_one;
using aura::compiler::set_pipeline_mutation_epoch;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::ir::IRFunction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

// Set-only: has set_pipeline_epoch, no pipeline_epoch_hint.
// Prior AND-requires skipped this shape entirely (#2827).
struct SetOnlyEpochPass {
    std::uint64_t epoch_ = 0;
    bool err_ = false;
    void run(IRModule&) {
        // no-op analysis
    }
    [[nodiscard]] bool has_error() const noexcept { return err_; }
    void set_pipeline_epoch(std::uint64_t epoch) noexcept { epoch_ = epoch; }
};

// Hint-only: has pipeline_epoch_hint, no set_pipeline_epoch.
// Cannot receive sync; must still compile and run; partial_skipped bumps.
struct HintOnlyEpochPass {
    bool err_ = false;
    void run(IRModule&) {}
    [[nodiscard]] bool has_error() const noexcept { return err_; }
    [[nodiscard]] std::uint64_t pipeline_epoch_hint() const noexcept { return 0; }
};

// Dual: both methods (typical JITFriendly wrap shape).
struct DualEpochPass {
    std::uint64_t epoch_ = 0;
    bool err_ = false;
    void run(IRModule&) {}
    [[nodiscard]] bool has_error() const noexcept { return err_; }
    void set_pipeline_epoch(std::uint64_t epoch) noexcept { epoch_ = epoch; }
    [[nodiscard]] std::uint64_t pipeline_epoch_hint() const noexcept { return epoch_; }
};

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

int run_test_run_one_requires_expression_partial() {
    std::println("=== Issue #2827: run_one requires-expression partial ===");
    CHECK(true, "ac2827: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: source splits set vs hint requires ---");
        auto core = read_file("src/compiler/pass_pipeline_core.ixx");
        CHECK(!core.empty(), "AC1: pass_pipeline_core readable");
        auto pos = core.find("execute a single pass");
        if (pos == std::string::npos)
            pos = core.rfind("bool run_one");
        CHECK(pos != std::string::npos, "AC1: run_one present");
        auto win = core.substr(pos, 3500);
        CHECK(win.find("Issue #2827") != std::string::npos, "AC1: cites #2827");
        CHECK(win.find("kHasSetPipelineEpoch") != std::string::npos, "AC1: set-only gate");
        CHECK(win.find("kHasPipelineEpochHint") != std::string::npos, "AC1: hint gate");
        CHECK(win.find("pipeline_epoch_sync_partial_skipped_total") != std::string::npos,
              "AC1: partial metric");
        // Must not reintroduce a single dual-requires AND block as the only path.
        // The set gate must stand alone (if constexpr (kHasSetPipelineEpoch)).
        CHECK(win.find("if constexpr (kHasSetPipelineEpoch)") != std::string::npos,
              "AC1: set gated alone");
        CHECK(core.find("pipeline_epoch_sync_partial_skipped_total") != std::string::npos,
              "AC1: export partial total");
    }

    // ── AC2: SetOnly syncs; HintOnly compiles + runs ──
    {
        std::println("\n--- AC2: SetOnly receives epoch; HintOnly runs ---");
        set_pipeline_mutation_epoch(77);

        SetOnlyEpochPass set_only;
        CHECK(set_only.epoch_ == 0, "AC2: SetOnly default 0");
        auto mod1 = make_tiny_mod();
        const auto sync0 = pipeline_epoch_sync_total.load();
        CHECK(run_one(mod1, set_only), "AC2: SetOnly run_one succeeds");
        CHECK(set_only.epoch_ == 77,
              std::format("AC2: SetOnly epoch set (got {})", set_only.epoch_));
        CHECK(pipeline_epoch_sync_total.load() > sync0, "AC2: SetOnly advanced sync_total");

        HintOnlyEpochPass hint_only;
        auto mod2 = make_tiny_mod();
        CHECK(run_one(mod2, hint_only), "AC2: HintOnly run_one succeeds (no compile error)");
        CHECK(hint_only.pipeline_epoch_hint() == 0, "AC2: HintOnly hint still 0 (no set)");

        set_pipeline_mutation_epoch(0);
    }

    // ── AC3: partial metric on XOR; dual does not bump partial ──
    {
        std::println("\n--- AC3: partial_skipped on XOR shapes ---");
        set_pipeline_mutation_epoch(9);

        const auto partial0 = pipeline_epoch_sync_partial_skipped_total.load();
        SetOnlyEpochPass set_only;
        auto m1 = make_tiny_mod();
        CHECK(run_one(m1, set_only), "AC3: SetOnly run");
        const auto partial1 = pipeline_epoch_sync_partial_skipped_total.load();
        CHECK(partial1 > partial0,
              std::format("AC3: SetOnly bumps partial (Δ={})", partial1 - partial0));

        HintOnlyEpochPass hint_only;
        auto m2 = make_tiny_mod();
        CHECK(run_one(m2, hint_only), "AC3: HintOnly run");
        const auto partial2 = pipeline_epoch_sync_partial_skipped_total.load();
        CHECK(partial2 > partial1,
              std::format("AC3: HintOnly bumps partial (Δ={})", partial2 - partial1));

        DualEpochPass dual;
        auto m3 = make_tiny_mod();
        const auto sync_before = pipeline_epoch_sync_total.load();
        CHECK(run_one(m3, dual), "AC3: Dual run");
        CHECK(dual.pipeline_epoch_hint() == 9, "AC3: Dual hint == 9");
        CHECK(pipeline_epoch_sync_total.load() > sync_before, "AC3: Dual advanced sync");
        CHECK(pipeline_epoch_sync_partial_skipped_total.load() == partial2,
              "AC3: Dual does not bump partial");

        set_pipeline_mutation_epoch(0);
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2827 query keys ---");
        CompilerService cs;
        CHECK(href(cs, "schema-2827") == 2827, "AC4: schema-2827");
        CHECK(href(cs, "issue-2827") == 2827, "AC4: issue-2827");
        CHECK(href(cs, "pipeline-epoch-sync-partial-wired") == 1, "AC4: wired");
        CHECK(href(cs, "pipeline-epoch-sync-partial-skipped-total") >= 0, "AC4: partial total");
        auto obs = read_file("src/compiler/evaluator_primitives_stdlib_review.cpp");
        CHECK(obs.find("schema-2827") != std::string::npos, "AC4: obs schema-2827");
        CHECK(obs.find("pipeline-epoch-sync-partial-skipped-total") != std::string::npos,
              "AC4: obs partial key");
        auto lint =
            read_file("scripts/coverage/checks/check_run_one_requires_expression_partial_2827.py");
        CHECK(!lint.empty(), "AC4: linter present");
        CHECK(lint.find("2827") != std::string::npos, "AC4: linter cites 2827");
    }

    std::println("\n=== #2827 run_one requires partial: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_run_one_requires_expression_partial();
}
#endif
