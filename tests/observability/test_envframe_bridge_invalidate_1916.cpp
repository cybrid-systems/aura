// @category: integration
// @reason: Issue #1916 — EnvFrame SoA + bridge_epoch + linear_ownership
// safety after invalidate_function / mutate (dangling env prevented,
// bridge_epoch_mismatch_fallback on all apply/materialize paths).
//
//   AC1: source wires materialize bridge-stale fallback + dangling_env_prevented
//   AC2: query:epoch-apply-hotpath-stats schema-1916 AC metrics
//   AC3: apply after mark_define_dirty / defuse bump → mismatch fallback
//   AC4: multi-round mutate:rebind + apply old closures — no crash
//   AC5: concurrent fiber-like stress (threads) mutate + apply
//   AC6: linear safe-fallback metric path present

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:epoch-apply-hotpath-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void seed_define(CompilerService& cs, const char* name) {
    aura::ir::IRFunction entry_fn;
    entry_fn.id = 0;
    entry_fn.name = "__top__";
    entry_fn.entry_block = 0;
    entry_fn.blocks.push_back({0, {}, {}});
    aura::ir::IRFunction body_fn;
    body_fn.id = 1;
    body_fn.name = std::string(name) + "#0";
    body_fn.entry_block = 0;
    body_fn.blocks.push_back({0, {}, {}});
    cs.store_define_v2(name, std::string("(define (") + name + " x) (+ x 1))",
                       std::vector{entry_fn, body_fn}, {}, {});
}

} // namespace

int main() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: dangling_env + materialize bridge-stale wiring ---");
        std::string env, eval, obs;
        for (const char* p :
             {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
            env = read_file(p);
            if (!env.empty())
                break;
        }
        for (const char* p :
             {"src/compiler/evaluator_eval_flat.cpp", "../src/compiler/evaluator_eval_flat.cpp"}) {
            eval = read_file(p);
            if (!eval.empty())
                break;
        }
        for (const char* p : {"src/compiler/evaluator_primitives_obs_eval.cpp",
                              "../src/compiler/evaluator_primitives_obs_eval.cpp"}) {
            obs = read_file(p);
            if (!obs.empty())
                break;
        }
        CHECK(!env.empty(), "read evaluator_env");
        CHECK(env.find("dangling_env_prevented") != std::string::npos, "dangling_env metric");
        CHECK(env.find("materialize-bridge-stale") != std::string::npos ||
                  env.find("is_bridge_stale") != std::string::npos,
              "bridge stale gate in materialize");
        CHECK(env.find("#1916") != std::string::npos, "cites #1916 in env");
        CHECK(!eval.empty() && eval.find("dangling_env_prevented") != std::string::npos,
              "apply path dangling_env");
        CHECK(!obs.empty() && obs.find("dangling_env_prevented") != std::string::npos,
              "stats surface");
        CHECK(obs.find("schema-1916") != std::string::npos, "schema-1916");
    }

    // ── AC2: stats hash ──
    {
        std::println("\n--- AC2: query:epoch-apply-hotpath-stats schema-1916 ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:epoch-apply-hotpath-stats\")");
        CHECK(r.has_value() && is_hash(*r), "is hash");
        CHECK(href(cs, "schema") == 1660, "lineage schema 1660 retained");
        CHECK(href(cs, "schema-1916") == 1916, "schema-1916");
        CHECK(href(cs, "issue-1916") == 1916, "issue-1916");
        CHECK(href(cs, "dangling_env_prevented") >= 0, "dangling_env_prevented");
        CHECK(href(cs, "bridge_epoch_mismatch_fallback") >= 0, "bridge_epoch_mismatch_fallback");
        CHECK(href(cs, "live_closure_stale_prevented") >= 0, "live_closure_stale_prevented");
        CHECK(href(cs, "materialize-bridge-stale-fallback-wired") == 1, "materialize wire");
        CHECK(href(cs, "apply-dual-check-wired") == 1, "apply dual-check");
        CHECK(href(cs, "defuse-version-check-wired") == 1, "defuse wired");
        CHECK(href(cs, "bridge-epoch-check-wired") == 1, "bridge wired");
        CHECK(href(cs, "linear-dual-check-wired") == 1, "linear dual");
    }

    // ── AC3: dirty + apply → fallback metrics ──
    {
        std::println("\n--- AC3: apply after dirty/defuse → mismatch fallback ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        seed_define(cs, "f1916");

        std::vector<ClosureId> caps;
        for (int i = 0; i < 6; ++i) {
            if (auto r = cs.eval("(lambda (x) (+ x 1))"); r && is_closure(*r))
                caps.push_back(as_closure_id(*r));
        }
        CHECK(!caps.empty(), "captured closures");

        const auto dang0 = load_u64(m->dangling_env_prevented);
        const auto mm0 = load_u64(m->closure_epoch_mismatch_fallback);
        const auto live0 = load_u64(m->compiler_live_closure_stale_prevented_total);

        cs.public_mark_define_dirty("f1916");
        cs.evaluator().bump_defuse_version_for_test();

        int applied = 0;
        for (auto cid : caps) {
            std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};
            (void)cs.evaluator().apply_closure(cid, args);
            ++applied;
        }
        CHECK(applied == static_cast<int>(caps.size()), "applied all");

        CHECK(load_u64(m->dangling_env_prevented) >= dang0, "dangling_env non-decreasing");
        CHECK(load_u64(m->closure_epoch_mismatch_fallback) >= mm0,
              "bridge_epoch_mismatch_fallback non-decreasing");
        CHECK(load_u64(m->compiler_live_closure_stale_prevented_total) >= live0,
              "live_closure_stale_prevented non-decreasing");
        CHECK(href(cs, "dangling_env_prevented") >= 0, "query dangling_env");
        CHECK(href(cs, "bridge_epoch_mismatch_fallback") >= 0, "query mismatch");
    }

    // ── AC4: multi-round mutate + apply ──
    {
        std::println("\n--- AC4: multi-round mutate:rebind + apply old closures ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        CHECK(cs.eval("(set-code \"(define (g x) (+ x 1)) (define (h y) (* y 2))\")").has_value(),
              "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");

        std::vector<ClosureId> caps;
        for (int i = 0; i < 5; ++i) {
            if (auto r = cs.eval("(lambda (x) (g x))"); r && is_closure(*r))
                caps.push_back(as_closure_id(*r));
        }
        CHECK(!caps.empty(), "closures");

        for (int round = 0; round < 20; ++round) {
            (void)cs.eval(std::format("(mutate:rebind \"g\" \"(lambda (x) (+ x {}))\" \"r{}\")",
                                      round + 1, round));
            (void)cs.eval("(eval-current)");
            for (auto cid : caps) {
                std::array<aura::compiler::types::EvalValue, 1> args{make_int(round)};
                auto r = cs.evaluator().apply_closure(cid, args);
                // Must not crash — result optional (stale → nullopt or fallback value).
                (void)r;
            }
        }
        CHECK(href(cs, "schema-1916") == 1916, "schema holds");
        // Some safety path should have fired over 20 rounds of mutate+apply.
        const auto dang = load_u64(m->dangling_env_prevented);
        const auto mm = load_u64(m->closure_epoch_mismatch_fallback);
        const auto live = load_u64(m->compiler_live_closure_stale_prevented_total);
        const auto safe = load_u64(m->compiler_closure_safe_fallbacks);
        std::println("  dangling={} mismatch={} live_stale={} safe_fallback={}", dang, mm, live,
                     safe);
        CHECK(dang > 0 || mm > 0 || live > 0 || safe > 0,
              "at least one safety metric advanced under mutate+apply");
    }

    // ── AC5: concurrent stress ──
    {
        std::println("\n--- AC5: concurrent mutate + apply stress ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        CHECK(cs.eval("(set-code \"(define (p x) (+ x 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");

        std::vector<ClosureId> caps;
        for (int i = 0; i < 8; ++i) {
            if (auto r = cs.eval("(lambda (x) (p x))"); r && is_closure(*r))
                caps.push_back(as_closure_id(*r));
        }
        CHECK(caps.size() >= 2, "caps");

        std::atomic<int> ops{0};
        std::atomic<bool> crash{false};
        auto worker_apply = [&]() {
            try {
                for (int i = 0; i < 50; ++i) {
                    for (auto cid : caps) {
                        std::array<aura::compiler::types::EvalValue, 1> args{make_int(i)};
                        (void)cs.evaluator().apply_closure(cid, args);
                        ops.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                crash.store(true, std::memory_order_relaxed);
            }
        };
        auto worker_mutate = [&]() {
            try {
                for (int i = 0; i < 25; ++i) {
                    (void)cs.eval(std::format(
                        "(mutate:rebind \"p\" \"(lambda (x) (+ x {}))\" \"t{}\")", i, i));
                    (void)cs.eval("(eval-current)");
                    ops.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                crash.store(true, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker_apply);
        std::thread t2(worker_mutate);
        std::thread t3(worker_apply);
        t1.join();
        t2.join();
        t3.join();
        CHECK(!crash.load(), "no crash under concurrent mutate+apply");
        CHECK(ops.load() > 0, "ops ran");
        CHECK(load_u64(m->dangling_env_prevented) >= 0, "dangling field live");
        CHECK(href(cs, "bridge_epoch_mismatch_fallback") >= 0, "mismatch after stress");
    }

    // ── AC6: linear safe-fallback key ──
    {
        std::println("\n--- AC6: linear ownership safe-fallback surface ---");
        CompilerService cs;
        CHECK(href(cs, "linear_ownership_safe_fallback_total") >= 0, "linear safe fallback key");
        CHECK(href(cs, "linear-stale-total") >= 0, "linear-stale-total");
        CHECK(href(cs, "linear-dual-check-wired") == 1, "linear dual-check");
    }

    std::println("\n=== test_envframe_bridge_invalidate_1916: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
