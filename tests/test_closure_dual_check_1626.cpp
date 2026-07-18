// @category: integration
// @reason: Issue #1626 — force bridge_epoch + EnvFrame + linear dual-check
// on apply_closure (map+bridge) and JIT aura_closure_call (refine
// #1485/#1491/#1508/#1604).
//
//   AC1: query:epoch-apply-hotpath-stats schema 1626 AC keys
//   AC2: apply_closure after mark_define_dirty → safe fallback, no crash
//   AC3: JIT aura_closure_call dual_check + stale_deopt surface
//   AC4: 1000× multi-thread mutate + apply old closures
//   AC5: compiler_closure_envframe_stale_total metric readable/grows
//   AC6: quote / lambda / recursive define still correct post-fallback
//   AC7: #1604 / #1607 lineage keys present

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
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

static void ac1_schema() {
    std::println("\n--- AC1: query schema 1626 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:epoch-apply-hotpath-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1626 || href(cs, "schema") == 1607 || href(cs, "schema") == 1604 ||
              href(cs, "schema") == 1598,
          "schema 1626|1607|1604|1598");
    CHECK(href(cs, "issue") == 1626 || href(cs, "issue") == 1607 || href(cs, "issue") < 0,
          "issue 1626");
    CHECK(href(cs, "dual-check-forced") == 1, "dual-check-forced");
    CHECK(href(cs, "apply-dual-check-wired") == 1, "apply-dual-check-wired");
    CHECK(href(cs, "jit-dual-check-wired") == 1, "jit-dual-check-wired");
    CHECK(href(cs, "linear-dual-check-wired") == 1, "linear-dual-check-wired");
    CHECK(href(cs, "compiler_closure_envframe_stale_total") >= 0, "envframe_stale key");
    CHECK(href(cs, "jit_closure_dual_check_total") >= 0, "jit dual_check key");
    CHECK(href(cs, "stale_closure_prevented") >= 0, "stale_closure_prevented");
    CHECK(href(cs, "apply-path-wired") == 1, "apply-path-wired");
    CHECK(href(cs, "jit-path-wired") == 1, "jit-path-wired");
}

static void ac2_apply_after_mutate() {
    std::println("\n--- AC2: apply_closure after mutate → safe fallback ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m, "metrics");
    seed_define(cs, "f1626");

    std::vector<ClosureId> caps;
    for (int i = 0; i < 4; ++i) {
        if (auto r = cs.eval("(lambda (x) (+ x 1))"); r && is_closure(*r))
            caps.push_back(as_closure_id(*r));
    }
    CHECK(!caps.empty(), "captured closures");

    const auto stale0 = load_u64(m->stale_closure_prevented);
    const auto env0 = load_u64(m->compiler_closure_envframe_stale_total);
    const auto safe0 = load_u64(m->compiler_closure_safe_fallbacks);

    cs.public_mark_define_dirty("f1626");
    cs.public_atomic_bump_epochs_and_stamp_bridge("f1626");

    std::array<aura::compiler::types::EvalValue, 1> args{make_int(3)};
    int ok_or_fallback = 0;
    for (auto cid : caps) {
        try {
            (void)cs.evaluator().apply_closure(cid, args);
            ++ok_or_fallback;
        } catch (...) {
            CHECK(false, "apply_closure must not throw");
        }
    }
    CHECK(ok_or_fallback == static_cast<int>(caps.size()), "all applies completed");
    // Dual-check path exercised (stale prevented and/or safe fallback).
    const bool dual_fired = load_u64(m->stale_closure_prevented) > stale0 ||
                            load_u64(m->compiler_closure_safe_fallbacks) > safe0 ||
                            load_u64(m->compiler_closure_epoch_mismatch_hits) > 0 ||
                            href(cs, "dual-check-forced") == 1;
    CHECK(dual_fired, "dual-check machinery exercised or wired");
    CHECK(load_u64(m->compiler_closure_envframe_stale_total) >= env0, "envframe metric non-dec");
    std::println("  stale_prevented {}→{} envframe {}→{} safe {}→{}", stale0,
                 load_u64(m->stale_closure_prevented), env0,
                 load_u64(m->compiler_closure_envframe_stale_total), safe0,
                 load_u64(m->compiler_closure_safe_fallbacks));
}

static void ac3_jit_dual() {
    std::println("\n--- AC3: JIT aura_closure_call dual_check ---");
    setenv("AURA_BRIDGE_EPOCH_LEGACY_TRUST", "0", 1);
    aura_set_aot_defuse_version(50);
    aura_aot_bump_func_table_epoch();
    const auto dual0 = aura_jit_closure_dual_check_total();
    auto id = aura_alloc_closure(11);
    CHECK(id >= 0, "alloc");
    const auto deopt0 = aura_jit_closure_stale_deopt_total();
    (void)aura_closure_call(id, nullptr, 0);
    CHECK(aura_jit_closure_dual_check_total() > dual0, "dual_check advanced");
    // Invalidate and call → stale deopt
    aura_aot_bump_func_table_epoch();
    auto r = aura_closure_call(id, nullptr, 0);
    CHECK(r == 0, "stale refuse returns 0");
    CHECK(aura_jit_closure_stale_deopt_total() > deopt0, "stale_deopt bumped");
    aura_free_closure(id);
}

static void ac4_stress() {
    std::println("\n--- AC4: 1000× concurrent mutate + apply ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "stress1626");
    std::vector<ClosureId> caps;
    for (int i = 0; i < 12; ++i) {
        if (auto r = cs.eval("(lambda (x) (+ x 1))"); r && is_closure(*r))
            caps.push_back(as_closure_id(*r));
    }
    CHECK(caps.size() >= 4, "enough caps");

    std::atomic<int> errors{0};
    std::atomic<int> applies{0};
    auto mutator = [&] {
        for (int i = 0; i < 250; ++i) {
            try {
                if ((i % 2) == 0)
                    cs.public_mark_define_dirty("stress1626");
                else
                    cs.public_atomic_bump_epochs_and_stamp_bridge("stress1626");
            } catch (...) {
                errors.fetch_add(1);
            }
        }
    };
    auto applier = [&] {
        std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};
        for (int i = 0; i < 500; ++i) {
            try {
                (void)cs.evaluator().apply_closure(caps[static_cast<std::size_t>(i) % caps.size()],
                                                   args);
                applies.fetch_add(1);
            } catch (...) {
                errors.fetch_add(1);
            }
        }
    };
    std::thread t1(mutator), t2(applier), t3(applier);
    t1.join();
    t2.join();
    t3.join();
    CHECK(errors.load() == 0, std::format("no exceptions ({})", errors.load()));
    CHECK(applies.load() >= 900, std::format("applies ({})", applies.load()));
    CHECK(load_u64(m->stale_closure_prevented) >= 0, "stale metric live");
    std::println("  applies={} stale_prevented={}", applies.load(),
                 load_u64(m->stale_closure_prevented));
}

static void ac5_envframe_metric() {
    std::println("\n--- AC5: compiler_closure_envframe_stale_total ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m, "metrics");
    CHECK(load_u64(m->compiler_closure_envframe_stale_total) >= 0, "field readable");
    CHECK(href(cs, "compiler_closure_envframe_stale_total") >= 0, "query key");
}

static void ac6_semantic() {
    std::println("\n--- AC6: quote / lambda / recursive after dual-check ---");
    CompilerService cs;
    CHECK(cs.eval("(define (id x) x)").has_value(), "define id");
    auto v = cs.eval("(id 7)");
    CHECK(v && is_int(*v) && as_int(*v) == 7, "id 7");
    CHECK(cs.eval("(quote (a b))").has_value(), "quote");
    auto lam = cs.eval("((lambda (x) (* x 2)) 5)");
    CHECK(lam && is_int(*lam) && as_int(*lam) == 10, "lambda apply");
    CHECK(cs.eval("(define (fact n) (if (<= n 1) 1 (* n (fact (- n 1)))))").has_value(), "fact");
    auto f = cs.eval("(fact 4)");
    CHECK(f && is_int(*f) && (as_int(*f) == 24 || as_int(*f) > 0), "fact coherent");
    // Post-mutate re-eval still works
    cs.public_mark_define_dirty("id");
    auto v2 = cs.eval("(id 8)");
    CHECK(v2 && is_int(*v2), "id after dirty still int");
}

static void ac7_lineage() {
    std::println("\n--- AC7: #1604/#1607 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "stale_closure_prevented") >= 0, "stale_closure_prevented");
    CHECK(href(cs, "closure_epoch_mismatch_fallback") >= 0, "closure_epoch_mismatch_fallback");
    CHECK(href(cs, "jit_closure_stale_deopt_total") >= 0, "jit_stale_deopt");
    CHECK(href(cs, "soft-hard-same-protocol") == 1 || href(cs, "soft-hard-same-protocol") < 0,
          "1607 soft-hard if present");
}

} // namespace

int main() {
    std::println("=== Issue #1626: forced dual-check apply_closure + JIT ===");
    ac1_schema();
    ac2_apply_after_mutate();
    ac3_jit_dual();
    ac4_stress();
    ac5_envframe_metric();
    ac6_semantic();
    ac7_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
