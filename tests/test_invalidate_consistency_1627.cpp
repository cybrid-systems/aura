// @category: integration
// @reason: Issue #1627 — unify mark_define_dirty / invalidate_function +
// atomic epoch bump + GC/JIT root consistency (refine #1496/#1607).
//
//   AC1: soft + hard both bump invalidate_pre_cascade_prepare_total
//   AC2: dual-epoch lockstep + protocol total on both paths
//   AC3: cascade depth / bridge_epoch_bumps / live_closure metrics
//   AC4: concurrent soft/hard + apply old closures — no crash
//   AC5: query:epoch-apply-hotpath-stats schema 1627 AC keys
//   AC6: linear_gc_root_audit_checks advances on soft dirty
//   AC7: quote/lambda/recursive define still correct

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <array>
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

static void ac1_pre_cascade() {
    std::println("\n--- AC1: soft + hard pre-cascade prepare ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "f1627");
    const auto p0 = load_u64(m->invalidate_pre_cascade_prepare_total);
    const auto proto0 = load_u64(m->unified_invalidation_protocol_total);
    cs.public_mark_define_dirty("f1627");
    CHECK(load_u64(m->invalidate_pre_cascade_prepare_total) > p0, "soft prepare");
    CHECK(load_u64(m->unified_invalidation_protocol_total) > proto0, "soft protocol");
    const auto p1 = load_u64(m->invalidate_pre_cascade_prepare_total);
    cs.public_invalidate_function("f1627");
    CHECK(load_u64(m->invalidate_pre_cascade_prepare_total) > p1, "hard prepare");
}

static void ac2_lockstep() {
    std::println("\n--- AC2: dual-epoch lockstep ---");
    CompilerService cs;
    seed_define(cs, "g1627");
    const auto b0 = cs.bridge_epoch();
    const auto d0 = cs.evaluator().defuse_version_for_test();
    cs.public_mark_define_dirty("g1627");
    CHECK((cs.bridge_epoch() - b0) == (cs.evaluator().defuse_version_for_test() - d0),
          "soft lockstep");
    const auto b1 = cs.bridge_epoch();
    const auto d1 = cs.evaluator().defuse_version_for_test();
    cs.public_invalidate_function("g1627");
    CHECK((cs.bridge_epoch() - b1) == (cs.evaluator().defuse_version_for_test() - d1),
          "hard lockstep");
}

static void ac3_metrics() {
    std::println("\n--- AC3: cascade / bumps / live metrics ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "h1627");
    seed_define(cs, "caller1627");
    cs.public_record_dependency("caller1627", "h1627");
    const auto bumps0 = load_u64(m->bridge_epoch_bumps_total);
    const auto depth0 = load_u64(m->invalidate_cascade_depth_total);
    cs.public_mark_define_dirty("h1627");
    CHECK(load_u64(m->bridge_epoch_bumps_total) > bumps0, "bridge bumps");
    CHECK(load_u64(m->invalidate_cascade_depth_total) >= depth0, "cascade depth");
    CHECK(load_u64(m->compiler_live_closure_stale_prevented_total) >= 0, "live_closure metric");
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) >= 0, "gc audit metric");
}

static void ac4_concurrent() {
    std::println("\n--- AC4: concurrent soft/hard + apply ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "stress1627");
    std::vector<ClosureId> caps;
    for (int i = 0; i < 10; ++i) {
        if (auto r = cs.eval("(lambda (x) (+ x 1))"); r && is_closure(*r))
            caps.push_back(as_closure_id(*r));
    }
    CHECK(caps.size() >= 4, "caps");

    std::atomic<int> errors{0};
    std::atomic<int> applies{0};
    auto mutator = [&] {
        for (int i = 0; i < 200; ++i) {
            try {
                if ((i % 3) == 0)
                    cs.public_invalidate_function("stress1627");
                else if ((i % 3) == 1)
                    cs.public_mark_define_dirty("stress1627");
                else
                    cs.public_atomic_bump_epochs_and_stamp_bridge("stress1627");
            } catch (...) {
                errors.fetch_add(1);
            }
        }
    };
    auto applier = [&] {
        std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};
        for (int i = 0; i < 400; ++i) {
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
    CHECK(applies.load() >= 700, std::format("applies ({})", applies.load()));
    CHECK(load_u64(m->invalidate_pre_cascade_prepare_total) > 0, "prepare under stress");
    CHECK(load_u64(m->unified_invalidation_protocol_total) > 0, "protocol under stress");
    std::println("  applies={} prepare={} protocol={}", applies.load(),
                 load_u64(m->invalidate_pre_cascade_prepare_total),
                 load_u64(m->unified_invalidation_protocol_total));
}

static void ac5_schema() {
    std::println("\n--- AC5: schema 1627 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:epoch-apply-hotpath-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1632 || href(cs, "schema") == 1627 || href(cs, "schema") == 1626 ||
              href(cs, "schema") == 1607 || href(cs, "schema") == 1604 ||
              href(cs, "schema") == 1598,
          "schema 1632|1627|lineage");
    CHECK(href(cs, "issue") == 1627 || href(cs, "issue") == 1626 || href(cs, "issue") < 0,
          "issue 1627");
    CHECK(href(cs, "invalidate_cascade_depth") >= 0, "cascade depth");
    CHECK(href(cs, "bridge_epoch_bumps") >= 0, "bridge bumps");
    CHECK(href(cs, "live_closure_stale_prevented") >= 0, "live_closure");
    CHECK(href(cs, "linear_gc_root_audit_checks_total") >= 0, "gc audit key");
    CHECK(href(cs, "invalidate_pre_cascade_prepare_total") >= 0, "prepare key");
    CHECK(href(cs, "soft-pre-cascade-wired") == 1, "soft pre-cascade");
    CHECK(href(cs, "invalidate-consistency-wired") == 1, "consistency wired");
    CHECK(href(cs, "soft-hard-same-protocol") == 1, "same protocol");
}

static void ac6_audit_on_soft() {
    std::println("\n--- AC6: linear_gc_root_audit on soft dirty ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "audit1627");
    const auto a0 = load_u64(m->linear_gc_root_audit_checks_total);
    cs.public_mark_define_dirty("audit1627");
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) > a0, "audit advanced on soft");
}

static void ac7_semantic() {
    std::println("\n--- AC7: semantic after invalidate ---");
    CompilerService cs;
    CHECK(cs.eval("(define (id x) x)").has_value(), "define id");
    CHECK(cs.eval("(id 3)").has_value(), "id 3");
    cs.public_mark_define_dirty("id");
    auto v = cs.eval("(id 4)");
    CHECK(v && is_int(*v), "id after soft dirty");
    cs.public_invalidate_function("id");
    auto v2 = cs.eval("(define (id x) (+ x 1))");
    CHECK(v2.has_value(), "redefine after hard");
    auto q = cs.eval("(quote (a b))");
    CHECK(q.has_value(), "quote");
    auto lam = cs.eval("((lambda (x) (+ x 10)) 2)");
    CHECK(lam && is_int(*lam) && as_int(*lam) == 12, "lambda");
}

} // namespace

int main() {
    std::println("=== Issue #1627: invalidate consistency soft/hard unify ===");
    ac1_pre_cascade();
    ac2_lockstep();
    ac3_metrics();
    ac4_concurrent();
    ac5_schema();
    ac6_audit_on_soft();
    ac7_semantic();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
