// @category: unit
// @reason: Issue #2823 — run_one must invoke fiber yield action when the
// pipeline yield policy hook returns true (not only bump a metric).
//
//   AC1: source splits policy hook vs fiber yield action; cites #2823
//   AC2: policy true + action registered → action called; both metrics
//   AC3: service trampoline is policy-only; action does Fiber::yield
//   AC4: schema-2823 query; this suite + linter; no docs/design/2823-*

#include "test_harness.hpp"

#include <atomic>
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
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::pipeline_fiber_yield_action;
using aura::compiler::pipeline_fiber_yield_calls_total;
using aura::compiler::pipeline_yield_count;
using aura::compiler::pipeline_yield_hook;
using aura::compiler::run_one;
using aura::compiler::set_pipeline_fiber_yield_action;
using aura::compiler::set_pipeline_yield_hook;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::ir::IRFunction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

static std::atomic<int> g_test_yield_action_calls{0};

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
        std::format("(hash-ref (engine:metrics \"query:pass-pipeline-stats\") \"{}\")", key));
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

static bool always_yield_policy() noexcept {
    return true;
}

static void test_yield_action() noexcept {
    g_test_yield_action_calls.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

int run_test_run_one_yield_hook_actual() {
    std::println("=== Issue #2823: run_one yield hook actual fiber yield ===");
    CHECK(true, "ac2823: issue stamp");

    // Save/restore process hooks so we don't leave test wiring installed.
    const auto prev_policy = pipeline_yield_hook();
    const auto prev_action = pipeline_fiber_yield_action();

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: policy vs action split in source ---");
        auto core = read_file("src/compiler/pass_pipeline_core.ixx");
        auto svc = read_file("src/compiler/service.ixx");
        CHECK(!core.empty(), "AC1: pass_pipeline_core readable");
        auto pos = core.find("execute a single pass");
        if (pos == std::string::npos)
            pos = core.rfind("bool run_one");
        CHECK(pos != std::string::npos, "AC1: run_one present");
        auto win = core.substr(pos, 2000);
        CHECK(win.find("Issue #2823") != std::string::npos, "AC1: cites #2823");
        CHECK(win.find("g_pipeline_fiber_yield_action") != std::string::npos ||
                  win.find("pipeline_fiber_yield") != std::string::npos,
              "AC1: fiber yield action in run_one");
        CHECK(win.find("pipeline_fiber_yield_calls_total") != std::string::npos,
              "AC1: fiber yield calls metric");
        CHECK(core.find("set_pipeline_fiber_yield_action") != std::string::npos,
              "AC1: set_pipeline_fiber_yield_action API");
        CHECK(core.find("PipelineFiberYieldAction") != std::string::npos,
              "AC1: PipelineFiberYieldAction type");
        // Service: policy does not call Fiber::yield; action does.
        CHECK(svc.find("pipeline_fiber_yield_action") != std::string::npos,
              "AC1: service action method");
        CHECK(svc.find("Issue #2823") != std::string::npos, "AC1: service cites #2823");
        // Prefer method definition (not the ctor registration call site).
        auto tramp = svc.find("static bool pipeline_yield_trampoline");
        if (tramp == std::string::npos)
            tramp = svc.find("bool pipeline_yield_trampoline() noexcept");
        CHECK(tramp != std::string::npos, "AC1: trampoline present");
        // Body only: from opening brace of trampoline to next method.
        auto brace = svc.find('{', tramp);
        auto next_meth = svc.find("static void pipeline_fiber_yield_action", tramp);
        if (next_meth == std::string::npos)
            next_meth = tramp + 300;
        auto tramp_body = svc.substr(brace, next_meth - brace);
        CHECK(tramp_body.find("g_current_fiber") != std::string::npos, "AC1: policy checks fiber");
        CHECK(tramp_body.find("Fiber::yield") == std::string::npos,
              "AC1: trampoline body is policy-only (no Fiber::yield call)");
        auto action = svc.find("static void pipeline_fiber_yield_action");
        if (action == std::string::npos)
            action = svc.find("void pipeline_fiber_yield_action() noexcept");
        CHECK(action != std::string::npos, "AC1: action definition");
        auto act_win = svc.substr(action, 500);
        CHECK(act_win.find("Fiber::yield") != std::string::npos ||
                  act_win.find("YieldReason::PassPipeline") != std::string::npos,
              "AC1: action calls Fiber::yield(PassPipeline)");
    }

    // ── AC2: policy true → action called ──
    {
        std::println("\n--- AC2: policy true invokes yield action ---");
        g_test_yield_action_calls.store(0);
        set_pipeline_yield_hook(&always_yield_policy);
        set_pipeline_fiber_yield_action(&test_yield_action);

        const auto y0 = pipeline_yield_count.load();
        const auto f0 = pipeline_fiber_yield_calls_total.load();

        ConstantFoldingWrap pass;
        auto mod = make_tiny_mod();
        CHECK(run_one(mod, pass), "AC2: run_one ok");

        CHECK(g_test_yield_action_calls.load() >= 1,
              std::format("AC2: yield action called (got {})", g_test_yield_action_calls.load()));
        CHECK(pipeline_yield_count.load() > y0, "AC2: pipeline_yield_count advanced");
        CHECK(pipeline_fiber_yield_calls_total.load() > f0,
              "AC2: pipeline_fiber_yield_calls_total advanced");

        // Policy false → no action.
        set_pipeline_yield_hook(+[]() noexcept { return false; });
        g_test_yield_action_calls.store(0);
        const auto f1 = pipeline_fiber_yield_calls_total.load();
        auto mod2 = make_tiny_mod();
        CHECK(run_one(mod2, pass), "AC2: run_one policy false");
        CHECK(g_test_yield_action_calls.load() == 0, "AC2: action not called when policy false");
        CHECK(pipeline_fiber_yield_calls_total.load() == f1, "AC2: fiber calls unchanged");

        // Policy true, no action → metric only (no crash).
        set_pipeline_yield_hook(&always_yield_policy);
        set_pipeline_fiber_yield_action(nullptr);
        const auto y2 = pipeline_yield_count.load();
        auto mod3 = make_tiny_mod();
        CHECK(run_one(mod3, pass), "AC2: run_one without action");
        CHECK(pipeline_yield_count.load() > y2, "AC2: yield count still bumps without action");
    }

    // ── AC3: multi-pass pipeline calls action once per pass ──
    {
        std::println("\n--- AC3: multi-pass pipeline yields per pass ---");
        g_test_yield_action_calls.store(0);
        set_pipeline_yield_hook(&always_yield_policy);
        set_pipeline_fiber_yield_action(&test_yield_action);
        ConstantFoldingWrap p1, p2, p3;
        auto mod = make_tiny_mod();
        CHECK(run_one(mod, p1) && run_one(mod, p2) && run_one(mod, p3), "AC3: 3× run_one");
        CHECK(g_test_yield_action_calls.load() == 3,
              std::format("AC3: 3 action calls (got {})", g_test_yield_action_calls.load()));
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2823 query keys ---");
        // Leave a non-null action so "wired" may reflect process state after CS ctor.
        CompilerService cs;
        CHECK(href(cs, "schema-2823") == 2823, "AC4: schema-2823");
        CHECK(href(cs, "issue-2823") == 2823, "AC4: issue-2823");
        CHECK(href(cs, "pipeline-fiber-yield-calls-total") >= 0, "AC4: fiber yield calls");
        CHECK(href(cs, "pipeline-yield-count") >= 0, "AC4: yield count");
        // CompilerService ctor registers action → wired==1
        CHECK(href(cs, "pipeline-fiber-yield-action-wired") == 1, "AC4: action wired by service");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(obs.find("schema-2823") != std::string::npos, "AC4: obs schema-2823");
        CHECK(obs.find("pipeline-fiber-yield-calls-total") != std::string::npos,
              "AC4: obs fiber yield key");
    }

    set_pipeline_yield_hook(prev_policy);
    set_pipeline_fiber_yield_action(prev_action);

    std::println("\n=== #2823 run_one yield hook actual: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_run_one_yield_hook_actual();
}
#endif
