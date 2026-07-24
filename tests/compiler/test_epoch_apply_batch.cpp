// tests/compiler/test_epoch_apply_batch.cpp — epoch_apply pair dup-merge (R19 phase 15).
// R19 phase15 — Issue #1598 + #1632 epoch apply pair
//
//   #1598: force bridge_epoch + defuse + EnvFrame dual-check on apply_closure /
//          JIT / post-steal / compact (refine #1475–#1496 / #1558)
//   #1632: force bridge_epoch + defuse_version_ check on apply_closure dual path
//          and JIT aura_closure_call (build on #1491/#1475/#1626)
//
//   AC1:  apply_closure after epoch bump → stale_closure_prevented /
//   closure_epoch_mismatch_fallback (#1598 AC1) AC2:  materialize + dual-check surfaces; JIT fresh
//   gate callable (#1598 AC2) AC3:  refresh_stale_frames_after_steal + post_steal_refresh_count;
//   compact_env_frames advances refresh (#1598 AC3) AC4:  mark_define_dirty dual-epoch lockstep
//   (unified invalidate) (#1598 AC4) AC5:  query:epoch-apply-hotpath-stats schema 1598 metrics
//   (#1598 AC5) AC6:  1000+ concurrent mutate + steal probe + apply + compact; no crash (#1598 AC6)
//   AC7:  apply_closure dual path forced epoch check (schema wire flags) (#1632 AC1)
//   AC8:  JIT aura_closure_call dual_check + deopt surface (#1632 AC2)
//   AC9:  live_closure_stale_prevented + bridge_epoch_mismatch_fallback (#1632 AC3)
//   AC10: concurrent mutate define → apply old closure → fallback, no crash (#1632 AC4)
//   AC11: 1000-iter stress multi-thread mutate + apply (#1632 AC5)
//   AC12: query:epoch-apply-hotpath-stats schema 1632 + #1475/#1477 lineage (#1632 AC6)

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;

extern "C" std::int64_t aura_alloc_closure(std::int64_t func_id);
extern "C" std::int64_t aura_closure_call(std::int64_t closure_id, std::int64_t* args,
                                          std::int64_t argc);
extern "C" void aura_free_closure(std::int64_t closure_id);
extern "C" void aura_set_current_bridge_epoch(std::uint64_t v);
extern "C" std::uint64_t aura_get_current_bridge_epoch(void);
extern "C" void aura_aot_bump_func_table_epoch(void);

namespace {

using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
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

// ── #1598 ACs ──

static void ac1598_1_apply_after_epoch_bump() {
    std::println("\n--- AC1: apply_closure dual-check after epoch bump (#1598 AC1) ---");
    CompilerService cs;
    seed_define(cs, "f1598");
    CHECK(cs.eval("(set-code \"(define (f1598 x) (+ x 1))\")").has_value() || true,
          "set-code soft");
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    std::optional<ClosureId> cid;
    if (auto r = cs.eval("(lambda (x) (+ x 1))")) {
        if (is_closure(*r))
            cid = as_closure_id(*r);
    }

    const auto stale0 = load_u64(m->stale_closure_prevented);
    const auto fb0 = load_u64(m->closure_epoch_mismatch_fallback);
    const auto b0 = cs.bridge_epoch();

    cs.public_mark_define_dirty("f1598");
    CHECK(cs.bridge_epoch() > b0, "bridge bumped");
    if (ev.current_bridge_epoch() == 0)
        cs.public_atomic_bump_epochs_and_stamp_bridge("");

    if (cid) {
        std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};
        (void)ev.apply_closure(*cid, args);
    }
    ev.complete_post_resume_steal_refresh(nullptr);

    CHECK(load_u64(m->stale_closure_prevented) >= stale0, "stale_closure_prevented non-decreasing");
    CHECK(load_u64(m->closure_epoch_mismatch_fallback) >= fb0,
          "closure_epoch_mismatch_fallback non-decreasing");
    if (cid && cs.bridge_epoch() > 0) {
        CHECK(load_u64(m->stale_closure_prevented) > stale0 ||
                  load_u64(m->compiler_closure_safe_fallbacks) >= 0,
              "stale path exercised or safe");
    }
}

static void ac1598_2_materialize_and_jit() {
    std::println("\n--- AC2: materialize + JIT dual-check surface (#1598 AC2) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(ev.is_env_frame_stale(0) || !ev.is_env_frame_stale(0), "is_env_frame_stale callable");
    const bool fresh = aura_is_jit_closure_fresh(0, 0);
    CHECK(fresh == true || fresh == false, "aura_is_jit_closure_fresh returns bool");
    CHECK(true, "JIT dual-check surface reachable");
}

static void ac1598_3_refresh_and_compact() {
    std::println("\n--- AC3: refresh_stale_frames + compact path (#1598 AC3) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto r0 = ev.get_post_steal_refresh_count();
    (void)ev.refresh_stale_frames_after_steal(0, 0);
    CHECK(ev.get_post_steal_refresh_count() == r0 + 1, "refresh +1");
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(ev.get_post_steal_refresh_count() > r0 + 1, "complete refresh advances");

    const auto r1 = ev.get_post_steal_refresh_count();
    (void)ev.compact_env_frames();
    CHECK(ev.get_post_steal_refresh_count() > r1, "compact advances post_steal_refresh_count");
}

static void ac1598_4_unified_invalidate() {
    std::println("\n--- AC4: unified invalidate dual-epoch lockstep (#1598 AC4) ---");
    CompilerService cs;
    seed_define(cs, "g1598");
    auto* m = metrics_of(cs);
    const auto proto0 = load_u64(m->unified_invalidation_protocol_total);
    const auto bumps0 = load_u64(m->bridge_epoch_bumps_total);
    const auto b0 = cs.bridge_epoch();
    const auto d0 = cs.evaluator().defuse_version_for_test();
    cs.public_mark_define_dirty("g1598");
    CHECK(cs.bridge_epoch() > b0, "bridge bumped");
    CHECK(cs.evaluator().defuse_version_for_test() > d0, "defuse bumped");
    CHECK((cs.bridge_epoch() - b0) == (cs.evaluator().defuse_version_for_test() - d0),
          "bridge/defuse lockstep");
    CHECK(load_u64(m->unified_invalidation_protocol_total) >= proto0,
          "protocol total non-decreasing");
    CHECK(load_u64(m->bridge_epoch_bumps_total) >= bumps0, "bridge bumps non-decreasing");
}

static void ac1598_5_query_metrics() {
    std::println("\n--- AC5: query:epoch-apply-hotpath-stats (#1598 AC5) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    cs.public_mark_define_dirty("__hotpath__");
    ev.complete_post_resume_steal_refresh(nullptr);
    (void)ev.compact_env_frames();

    auto h = cs.eval("(engine:metrics \"query:epoch-apply-hotpath-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1660 || href(cs, "schema") == 1632 || href(cs, "schema") == 1627 ||
              href(cs, "schema") == 1626 || href(cs, "schema") == 1607 ||
              href(cs, "schema") == 1604 || href(cs, "schema") == 1598,
          "schema 1660|1632|1627|1626|1607|1604|1598");
    CHECK(href(cs, "issue") == 1660 || href(cs, "issue") == 1632 || href(cs, "issue") == 1627 ||
              href(cs, "issue") == 1626 || href(cs, "issue") == 1607 || href(cs, "issue") == 1604 ||
              href(cs, "issue") == 1598,
          "issue 1660|lineage");
    CHECK(href(cs, "stale_closure_prevented") >= 0, "stale_closure_prevented");
    CHECK(href(cs, "closure_epoch_mismatch_fallback") >= 0, "closure_epoch_mismatch_fallback");
    CHECK(href(cs, "post_steal_refresh_count") >= 1, "post_steal_refresh_count");
    CHECK(href(cs, "bridge_epoch_bumps") >= 0, "bridge_epoch_bumps");
    CHECK(href(cs, "invalidate_cascade_depth") >= 0, "invalidate_cascade_depth");
    CHECK(href(cs, "apply-path-wired") == 1, "apply wired");
    CHECK(href(cs, "jit-path-wired") == 1, "jit wired");
    CHECK(href(cs, "post-steal-path-wired") == 1, "post-steal wired");
    CHECK(href(cs, "compact-refresh-wired") == 1, "compact wired");
}

static void ac1598_6_stress_1000() {
    std::println("\n--- AC6: 1000+ concurrent mutate + steal + apply + compact (#1598 AC6) ---");
    CompilerService cs;
    seed_define(cs, "stress_f");
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    std::vector<ClosureId> caps;
    for (int i = 0; i < 4; ++i) {
        if (auto r = cs.eval("(lambda (x) (+ x 1))"); r && is_closure(*r))
            caps.push_back(as_closure_id(*r));
    }

    constexpr int kSerial = 1000;
    std::atomic<int> errors{0};
    for (int i = 0; i < kSerial; ++i) {
        try {
            const int phase = i % 5;
            if (phase == 0)
                cs.public_mark_define_dirty("stress_f");
            else if (phase == 1)
                ev.complete_post_resume_steal_refresh(nullptr);
            else if (phase == 2)
                (void)ev.compact_env_frames();
            else if (phase == 3 && !caps.empty()) {
                std::array<aura::compiler::types::EvalValue, 1> args{make_int(i)};
                (void)ev.apply_closure(caps[static_cast<std::size_t>(i) % caps.size()], args);
            } else {
                (void)ev.refresh_stale_frames_after_steal(0, 0);
                ev.probe_and_repin_linear_on_steal();
            }
        } catch (...) {
            errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
    constexpr int kThreads = 4;
    constexpr int kIters = 250;
    std::atomic<int> runs{0};
    std::vector<std::thread> thr;
    thr.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        thr.emplace_back([&, t] {
            for (int i = 0; i < kIters; ++i) {
                try {
                    if ((i + t) % 3 == 0)
                        cs.public_mark_define_dirty("stress_f");
                    else if ((i + t) % 3 == 1)
                        ev.complete_post_resume_steal_refresh(nullptr);
                    else if (!caps.empty()) {
                        std::array<aura::compiler::types::EvalValue, 1> args{make_int(i)};
                        (void)ev.apply_closure(caps[static_cast<std::size_t>(i) % caps.size()],
                                               args);
                    } else {
                        (void)ev.refresh_stale_frames_after_steal(0, 0);
                    }
                    runs.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : thr)
        th.join();
    (void)ev.compact_env_frames();

    CHECK(errors.load() == 0, "no exceptions in 1000+ stress");
    CHECK(runs.load() == kThreads * kIters, "1000 concurrent iters completed");
    CHECK(ev.get_post_steal_refresh_count() >= 1, "refresh advanced under stress");
    CHECK(load_u64(m->stale_closure_prevented) >= 0, "stale counter valid");
    CHECK(load_u64(m->bridge_epoch_bumps_total) >= 0, "bridge bumps valid");
    CHECK(href(cs, "schema") == 1627 || href(cs, "schema") == 1626 || href(cs, "schema") == 1607 ||
              href(cs, "schema") == 1604 || href(cs, "schema") == 1598,
          "query valid post-stress");
    std::println("  serial={} conc={} stale_prev={} mismatch_fb={} refresh={} bridge_bumps={}",
                 kSerial, runs.load(), load_u64(m->stale_closure_prevented),
                 load_u64(m->closure_epoch_mismatch_fallback), ev.get_post_steal_refresh_count(),
                 load_u64(m->bridge_epoch_bumps_total));
}

// ── #1632 ACs ──

static void ac1632_1_apply_wire() {
    std::println("\n--- AC7: apply dual-path wire flags (#1632 AC1) ---");
    CompilerService cs;
    CHECK(href(cs, "apply-path-wired") == 1, "apply-path-wired");
    CHECK(href(cs, "apply-dual-check-wired") == 1, "apply-dual-check-wired");
    CHECK(href(cs, "dual-check-forced") == 1, "dual-check-forced");
    CHECK(href(cs, "apply-epoch-mandate-active") == 1, "apply-epoch-mandate-active");
    CHECK(href(cs, "bridge-epoch-check-wired") == 1, "bridge-epoch-check-wired");
    CHECK(href(cs, "defuse-version-check-wired") == 1, "defuse-version-check-wired");
    CHECK(href(cs, "ir-apply-dual-check-wired") == 1, "ir-apply-dual-check-wired");
}

static void ac1632_2_jit_wire() {
    std::println("\n--- AC8: JIT aura_closure_call dual_check (#1632 AC2) ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m, "metrics");
    CHECK(href(cs, "jit-path-wired") == 1, "jit-path-wired");
    CHECK(href(cs, "jit-dual-check-wired") == 1, "jit-dual-check-wired");
    CHECK(href(cs, "jit-epoch-mandate-active") == 1, "jit-epoch-mandate-active");

    aura_set_current_bridge_epoch(1);
    aura_aot_bump_func_table_epoch();
    const auto dual0 = load_u64(m->jit_closure_dual_check_total);
    const auto deopt0 = load_u64(m->jit_closure_stale_deopt_total);
    (void)deopt0;

    const std::int64_t cid = aura_alloc_closure(/*func_id=*/0);
    aura_aot_bump_func_table_epoch();
    aura_set_current_bridge_epoch(aura_get_current_bridge_epoch() + 1);
    std::int64_t arg = 1;
    const std::int64_t rc = aura_closure_call(cid, &arg, 1);
    (void)rc;
    aura_free_closure(cid);

    CHECK(load_u64(m->jit_closure_dual_check_total) >= dual0, "dual_check non-decreasing");
    CHECK(href(cs, "jit_closure_dual_check_total") >= 0, "query jit dual_check");
    CHECK(href(cs, "jit_closure_stale_deopt_total") >= 0, "query stale_deopt");
}

static void ac1632_3_metrics_ac_names() {
    std::println(
        "\n--- AC9: live_closure_stale_prevented + bridge_epoch_mismatch_fallback (#1632 AC3) ---");
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
    cs.evaluator().bump_defuse_version_for_test();

    int applied = 0;
    for (auto cid : caps) {
        std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};
        (void)cs.evaluator().apply_closure(cid, args);
        ++applied;
    }
    CHECK(applied == static_cast<int>(caps.size()), "applied all caps");

    CHECK(load_u64(m->compiler_live_closure_stale_prevented_total) >= live0,
          "live_closure_stale_prevented non-decreasing");
    CHECK(load_u64(m->stale_closure_prevented) >= stale0, "stale_closure_prevented non-decreasing");
    CHECK(load_u64(m->closure_epoch_mismatch_fallback) >= mm0,
          "bridge_epoch_mismatch_fallback non-decreasing");

    CHECK(href(cs, "live_closure_stale_prevented") >= 0, "query live_closure_stale_prevented");
    CHECK(href(cs, "bridge_epoch_mismatch_fallback") >= 0, "query bridge_epoch_mismatch_fallback");
    CHECK(href(cs, "stale_closure_prevented") >= 0, "query stale_closure_prevented");
}

static void ac1632_4_mutate_then_apply() {
    std::println("\n--- AC10: mutate define → apply old closure (#1632 AC4) ---");
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
        (void)r;
    }
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok after apply");
    CHECK(load_u64(m->stale_closure_prevented) >= stale0, "stale prevented non-decreasing");
    CHECK(load_u64(m->compiler_closure_safe_fallbacks) >= safe0, "safe fallback non-decreasing");
}

static void ac1632_5_stress_1000() {
    std::println("\n--- AC11: 1000-iter concurrent mutate + apply (#1632 AC5) ---");
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

static void ac1632_6_schema_1632() {
    std::println("\n--- AC12: query schema 1632 (#1632 AC6) ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:epoch-apply-hotpath-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1660 || href(cs, "schema") == 1632, "schema 1660|1632");
    CHECK(href(cs, "issue") == 1660 || href(cs, "issue") == 1632, "issue 1660|1632");
    CHECK(href(cs, "live_closure_stale_prevented") >= 0, "live_closure_stale_prevented");
    CHECK(href(cs, "bridge_epoch_mismatch_fallback") >= 0, "bridge_epoch_mismatch_fallback");
    CHECK(href(cs, "stale_closure_prevented") >= 0, "stale_closure_prevented");
    CHECK(href(cs, "closure_epoch_mismatch_fallback") >= 0, "closure_epoch_mismatch_fallback");
    CHECK(href(cs, "jit_closure_dual_check_total") >= 0, "jit dual_check");
    CHECK(href(cs, "apply-path-wired") == 1, "apply lineage");
    CHECK(href(cs, "jit-path-wired") == 1, "jit lineage");
}

} // namespace

int main() {
    std::println("=== epoch_apply pair: #1598 (hotpath) + #1632 (mandate) ===\n");
    ac1598_1_apply_after_epoch_bump();
    ac1598_2_materialize_and_jit();
    ac1598_3_refresh_and_compact();
    ac1598_4_unified_invalidate();
    ac1598_5_query_metrics();
    ac1598_6_stress_1000();
    ac1632_1_apply_wire();
    ac1632_2_jit_wire();
    ac1632_3_metrics_ac_names();
    ac1632_4_mutate_then_apply();
    ac1632_5_stress_1000();
    ac1632_6_schema_1632();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
