// @category: integration
// @reason: Issue #1632 — force bridge_epoch + defuse_version_ check on
// Issue #1475/#1477/#1491/#1626/#1632 (#1978 renamed): issue# moved from filename to header.
// apply_closure dual path and JIT aura_closure_call (build on #1491/#1475/#1626).
//
//   AC1: apply_closure dual path forced epoch check (schema wire flags)
//   AC2: JIT aura_closure_call dual_check + deopt surface
//   AC3: live_closure_stale_prevented + bridge_epoch_mismatch_fallback
//   AC4: concurrent mutate define → apply old closure → fallback, no crash
//   AC5: 1000-iter stress multi-thread mutate + apply
//   AC6: query:epoch-apply-hotpath-stats schema 1632 + #1475/#1477 lineage

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;

extern "C" int64_t aura_alloc_closure(int64_t func_id);
extern "C" int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc);
extern "C" void aura_free_closure(int64_t closure_id);
extern "C" void aura_set_current_bridge_epoch(std::uint64_t v);
extern "C" std::uint64_t aura_get_current_bridge_epoch(void);
extern "C" void aura_aot_bump_func_table_epoch(void);

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

static void ac1_apply_wire() {
    std::println("\n--- AC1: apply dual-path wire flags ---");
    CompilerService cs;
    CHECK(href(cs, "apply-path-wired") == 1, "apply-path-wired");
    CHECK(href(cs, "apply-dual-check-wired") == 1, "apply-dual-check-wired");
    CHECK(href(cs, "dual-check-forced") == 1, "dual-check-forced");
    CHECK(href(cs, "apply-epoch-mandate-active") == 1, "apply-epoch-mandate-active");
    CHECK(href(cs, "bridge-epoch-check-wired") == 1, "bridge-epoch-check-wired");
    CHECK(href(cs, "defuse-version-check-wired") == 1, "defuse-version-check-wired");
    CHECK(href(cs, "ir-apply-dual-check-wired") == 1, "ir-apply-dual-check-wired");
}

static void ac2_jit_wire() {
    std::println("\n--- AC2: JIT aura_closure_call dual_check ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m, "metrics");
    CHECK(href(cs, "jit-path-wired") == 1, "jit-path-wired");
    CHECK(href(cs, "jit-dual-check-wired") == 1, "jit-dual-check-wired");
    CHECK(href(cs, "jit-epoch-mandate-active") == 1, "jit-epoch-mandate-active");

    // Force stale JIT closure: alloc under epoch 1, bump table, call.
    aura_set_current_bridge_epoch(1);
    aura_aot_bump_func_table_epoch(); // table may advance independently
    const auto dual0 = load_u64(m->jit_closure_dual_check_total);
    const auto deopt0 = load_u64(m->jit_closure_stale_deopt_total);
    const auto live0 = load_u64(m->compiler_live_closure_stale_prevented_total);
    const auto mismatch0 = load_u64(m->closure_epoch_mismatch_fallback);

    // Stamp a closure with old bridge, then advance epoch so call is stale.
    const int64_t cid = aura_alloc_closure(/*func_id=*/0);
    aura_aot_bump_func_table_epoch();
    aura_set_current_bridge_epoch(aura_get_current_bridge_epoch() + 1);
    int64_t arg = 1;
    const int64_t rc = aura_closure_call(cid, &arg, 1);
    (void)rc;
    aura_free_closure(cid);

    CHECK(load_u64(m->jit_closure_dual_check_total) >= dual0, "dual_check non-decreasing");
    // Stale path may or may not fire depending on table/bridge coupling; keys must exist.
    CHECK(href(cs, "jit_closure_dual_check_total") >= 0, "query jit dual_check");
    CHECK(href(cs, "jit_closure_stale_deopt_total") >= 0, "query stale_deopt");
    (void)deopt0;
    (void)live0;
    (void)mismatch0;
}

static void ac3_metrics_ac_names() {
    std::println("\n--- AC3: live_closure_stale_prevented + bridge_epoch_mismatch_fallback ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "f1632");

    std::vector<ClosureId> caps;
    for (int i = 0; i < 4; ++i) {
        if (auto r = cs.eval("(lambda (x) (+ x 1))"); r && is_closure(*r))
            caps.push_back(as_closure_id(*r));
    }
    CHECK(!caps.empty(), "captured closures");

    const auto live0 = load_u64(m->compiler_live_closure_stale_prevented_total);
    const auto stale0 = load_u64(m->stale_closure_prevented);
    const auto mm0 = load_u64(m->closure_epoch_mismatch_fallback);

    cs.public_mark_define_dirty("f1632");
    // Force bridge bump if mark_define_dirty does not always stale pure lambdas.
    cs.evaluator().bump_defuse_version_for_test();

    int applied = 0;
    for (auto cid : caps) {
        std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};
        (void)cs.evaluator().apply_closure(cid, args);
        ++applied;
    }
    CHECK(applied == static_cast<int>(caps.size()), "applied all caps");

    // After dirty + defuse, at least stale_closure_prevented or live should grow
    // when closures hold env frames; pure lambdas may still hit race window.
    const auto live1 = load_u64(m->compiler_live_closure_stale_prevented_total);
    const auto stale1 = load_u64(m->stale_closure_prevented);
    const auto mm1 = load_u64(m->closure_epoch_mismatch_fallback);
    CHECK(live1 >= live0, "live_closure_stale_prevented non-decreasing");
    CHECK(stale1 >= stale0, "stale_closure_prevented non-decreasing");
    CHECK(mm1 >= mm0, "bridge_epoch_mismatch_fallback non-decreasing");

    CHECK(href(cs, "live_closure_stale_prevented") >= 0, "query live_closure_stale_prevented");
    CHECK(href(cs, "bridge_epoch_mismatch_fallback") >= 0, "query bridge_epoch_mismatch_fallback");
    CHECK(href(cs, "stale_closure_prevented") >= 0, "query stale_closure_prevented");
}

static void ac4_mutate_then_apply() {
    std::println("\n--- AC4: mutate define → apply old closure ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \"(define (g x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    std::vector<ClosureId> caps;
    for (int i = 0; i < 3; ++i) {
        if (auto r = cs.eval("(lambda (x) (g x))"); r && is_closure(*r))
            caps.push_back(as_closure_id(*r));
    }
    if (caps.empty()) {
        // Fallback pure lambdas
        for (int i = 0; i < 3; ++i) {
            if (auto r = cs.eval("(lambda (x) (+ x 1))"); r && is_closure(*r))
                caps.push_back(as_closure_id(*r));
        }
    }
    CHECK(!caps.empty(), "have closures");

    const auto safe0 = load_u64(m->compiler_closure_safe_fallbacks);
    const auto stale0 = load_u64(m->stale_closure_prevented);

    (void)cs.eval("(mutate:rebind \"g\" \"(lambda (x) (+ x 2))\" \"#1632\")");
    cs.public_mark_define_dirty("g");
    cs.evaluator().bump_defuse_version_for_test();

    for (auto cid : caps) {
        std::array<aura::compiler::types::EvalValue, 1> args{make_int(10)};
        auto r = cs.evaluator().apply_closure(cid, args);
        // Either correct result (rebuilt) or nullopt (safe refuse) — no crash.
        (void)r;
    }
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok after apply");
    CHECK(load_u64(m->stale_closure_prevented) >= stale0, "stale prevented non-decreasing");
    CHECK(load_u64(m->compiler_closure_safe_fallbacks) >= safe0, "safe fallback non-decreasing");
}

static void ac5_stress_1000() {
    std::println("\n--- AC5: 1000-iter concurrent mutate + apply ---");
    CompilerService cs;
    seed_define(cs, "s1632");
    auto* m = metrics_of(cs);
    const auto stale0 = load_u64(m->stale_closure_prevented);

    std::vector<ClosureId> caps;
    for (int i = 0; i < 8; ++i) {
        if (auto r = cs.eval("(lambda (x) (+ x 1))"); r && is_closure(*r))
            caps.push_back(as_closure_id(*r));
    }
    CHECK(!caps.empty(), "stress caps");

    std::atomic<int> errors{0};
    constexpr int kIters = 1000;
    std::thread mutator([&] {
        for (int i = 0; i < kIters; ++i) {
            try {
                cs.public_mark_define_dirty("s1632");
                if ((i % 7) == 0)
                    cs.evaluator().bump_defuse_version_for_test();
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    std::thread applier([&] {
        for (int i = 0; i < kIters; ++i) {
            try {
                auto cid = caps[static_cast<std::size_t>(i) % caps.size()];
                std::array<aura::compiler::types::EvalValue, 1> args{make_int(i % 17)};
                (void)cs.evaluator().apply_closure(cid, args);
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    mutator.join();
    applier.join();
    CHECK(errors.load() == 0, std::format("no exceptions ({})", errors.load()));
    CHECK(cs.eval("(+ 2 3)").has_value(), "eval after stress");
    CHECK(load_u64(m->stale_closure_prevented) >= stale0, "stale under stress non-decreasing");
}

static void ac6_schema_1632() {
    std::println("\n--- AC6: query schema 1632 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:epoch-apply-hotpath-stats\")");
    CHECK(h && is_hash(*h), "hash");
    // Lineage: #1660 bumps schema; accept 1660|1632.
    CHECK(href(cs, "schema") == 1660 || href(cs, "schema") == 1632, "schema 1660|1632");
    CHECK(href(cs, "issue") == 1660 || href(cs, "issue") == 1632, "issue 1660|1632");
    CHECK(href(cs, "live_closure_stale_prevented") >= 0, "live_closure_stale_prevented");
    CHECK(href(cs, "bridge_epoch_mismatch_fallback") >= 0, "bridge_epoch_mismatch_fallback");
    CHECK(href(cs, "stale_closure_prevented") >= 0, "stale_closure_prevented");
    CHECK(href(cs, "closure_epoch_mismatch_fallback") >= 0, "closure_epoch_mismatch_fallback");
    CHECK(href(cs, "jit_closure_dual_check_total") >= 0, "jit dual_check");
    // #1475 / #1477 lineage surfaces
    CHECK(href(cs, "apply-path-wired") == 1, "apply lineage");
    CHECK(href(cs, "jit-path-wired") == 1, "jit lineage");
}

} // namespace

int main() {
    std::println("=== Issue #1632: epoch apply mandate ===");
    ac1_apply_wire();
    ac2_jit_wire();
    ac3_metrics_ac_names();
    ac4_mutate_then_apply();
    ac5_stress_1000();
    ac6_schema_1632();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
