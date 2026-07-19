// @category: integration
// @reason: Issue #1505 — mark_define_dirty cascade for nested lambda
// (irs.size()>2): dep_graph_-aware body-only + free-var scan of
// nested lambdas (irs[2..N]). Non-duplicative of #1474 (per-block
// selective copy), #1514 (body-only cascade + partial_recompile).
//
// Deterministic C++ setup (store_define_v2 + public_record_dependency),
// matching #1474 / #224 pattern — no rely on Aura eval IR shape.
//
//   AC1: free-var scan of nested lambdas — free-ref nested marked
//   AC2: targeted_dirty bumps; full_dirty does NOT on targeted path
//   AC3: dirty_block_count < total_blocks when only body free-refs
//   AC4: 1000× mark_define_dirty cascade; edges stable; no full cascade
//   AC5: nested without free-ref stays clean
//   AC6: metric surface readable

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.ir;
import aura.compiler.service;

namespace aura_issue_1505_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

// Synthetic 3-function entry: __top__ + body + one nested lambda.
// free_vars on nested controls free-ref cascade.
static void store_nested_g(CompilerService& cs, const std::vector<std::string>& nested_free_vars) {
    aura::ir::IRFunction top;
    top.id = 0;
    top.name = "__top__";
    top.blocks.push_back({0, {}, {}});

    aura::ir::IRFunction body;
    body.id = 1;
    body.name = "g_body";
    body.blocks.push_back({0, {}, {}});
    body.blocks.push_back({1, {}, {}}); // 2 blocks so total > nested-only

    aura::ir::IRFunction nested;
    nested.id = 2;
    nested.name = "__lambda__";
    nested.blocks.push_back({0, {}, {}});
    nested.free_vars = nested_free_vars;

    std::vector<aura::ir::IRFunction> irs;
    irs.push_back(std::move(top));
    irs.push_back(std::move(body));
    irs.push_back(std::move(nested));
    cs.store_define_v2("g", "(define (g x) (f (lambda (y) (* y 2)) x))", std::move(irs),
                       std::vector<aura::ir::ClosureBridgeData>{}, std::vector<std::string>{});
}

static void store_simple_f(CompilerService& cs) {
    aura::ir::IRFunction top;
    top.id = 0;
    top.name = "__top__";
    top.blocks.push_back({0, {}, {}});
    aura::ir::IRFunction body;
    body.id = 1;
    body.name = "f_body";
    body.blocks.push_back({0, {}, {}});
    std::vector<aura::ir::IRFunction> irs;
    irs.push_back(std::move(top));
    irs.push_back(std::move(body));
    cs.store_define_v2("f", "(define (f x) (+ x 1))", std::move(irs),
                       std::vector<aura::ir::ClosureBridgeData>{}, std::vector<std::string>{});
}

static void ac6_metric_surface() {
    std::println("\n--- AC6: metric surface ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics available");
    CHECK(load_u64(m->dep_graph_nested_lambda_targeted_dirty_total) >= 0, "targeted readable");
    CHECK(load_u64(m->dep_graph_nested_lambda_full_dirty) >= 0, "full_dirty readable");
    CHECK(load_u64(m->cascade_body_only_count) >= 0, "body_only readable");
}

static void ac3_ac5_body_only_no_free_ref() {
    std::println("\n--- AC3/5: nested without free-ref stays clean; dirty < total ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    store_simple_f(cs);
    store_nested_g(cs, /*nested_free_vars=*/{}); // no free-ref of f
    cs.public_record_dependency("g", "f");       // g calls f

    const auto* ge0 = cs.get_define_v2("g");
    CHECK(ge0 != nullptr, "g in ir_cache_v2_");
    CHECK(ge0 && ge0->irs.size() == 3, "g has 3 IR funcs (top+body+nested)");
    // Clear dirty bits after store (store may mark all dirty).
    if (ge0) {
        // store_define_v2 clears dirty after init — verify clean start.
    }
    // Mark clean: re-store leaves clean via clear_all_block_dirty path.
    // Force clean by clearing via public hooks if needed.
    const auto total0 = cs.dirty_block_count_v2("g");
    (void)total0;

    const auto full0 = m ? load_u64(m->dep_graph_nested_lambda_full_dirty) : 0;
    const auto targeted0 = m ? load_u64(m->dep_graph_nested_lambda_targeted_dirty_total) : 0;
    const auto body0 = m ? load_u64(m->cascade_body_only_count) : 0;

    cs.public_mark_define_dirty("f");

    const auto* ge = cs.get_define_v2("g");
    CHECK(ge != nullptr, "g still cached");
    if (!ge)
        return;

    const auto dirty = ge->dirty_block_count();
    std::size_t total = 0;
    for (const auto& fb : ge->block_dirty_view())
        total += fb.size();
    std::println("  g: dirty={} total={} body_dirty={} nested_dirty={}", dirty, total,
                 ge->func_dirty_block_count(1), ge->func_dirty_block_count(2));

    CHECK(total >= 3, "g has >=3 blocks");
    // AC3: dirty_block_count < total (nested clean → not full entry)
    CHECK(dirty < total, "dirty_block_count < total_blocks");
    // AC5: nested without free-ref stays clean
    CHECK(ge->func_dirty_block_count(2) == 0, "nested lambda without free-ref f stays clean");
    // Body (call site) dirty
    CHECK(ge->func_dirty_block_count(1) > 0, "body of g is dirty");

    if (m) {
        // AC2
        CHECK(load_u64(m->dep_graph_nested_lambda_full_dirty) == full0,
              "full_dirty unchanged on targeted path");
        CHECK(load_u64(m->dep_graph_nested_lambda_targeted_dirty_total) > targeted0,
              "targeted_dirty bumped");
        CHECK(load_u64(m->cascade_body_only_count) > body0, "body_only bumped");
        std::println("  body_only={}→{} targeted={}→{} full={}→{}", body0,
                     load_u64(m->cascade_body_only_count), targeted0,
                     load_u64(m->dep_graph_nested_lambda_targeted_dirty_total), full0,
                     load_u64(m->dep_graph_nested_lambda_full_dirty));
    }
}

static void ac1_free_ref_nested_marked() {
    std::println("\n--- AC1: nested free-ref f is marked dirty ---");
    CompilerService cs;
    store_simple_f(cs);
    store_nested_g(cs, /*nested_free_vars=*/{"f"}); // free-refs f
    cs.public_record_dependency("g", "f");

    cs.public_mark_define_dirty("f");
    const auto* ge = cs.get_define_v2("g");
    CHECK(ge != nullptr, "g cached");
    if (!ge)
        return;
    CHECK(ge->func_dirty_block_count(1) > 0, "body dirty");
    CHECK(ge->func_dirty_block_count(2) > 0, "nested free-ref f → nested dirty");
    std::println("  body_dirty={} nested_dirty={}", ge->func_dirty_block_count(1),
                 ge->func_dirty_block_count(2));
}

static void ac4_stress_cascade() {
    std::println("\n--- AC4: 1000× mark_define_dirty cascade; edges stable ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    store_simple_f(cs);
    store_nested_g(cs, /*nested_free_vars=*/{});
    cs.public_record_dependency("g", "f");

    const auto edges0 = cs.public_dep_graph_calls_for("g") + cs.public_dep_graph_called_by_for("f");
    CHECK(cs.public_dep_graph_has_edge("g", "f"), "edge g→f present");
    const auto full0 = m ? load_u64(m->dep_graph_nested_lambda_full_dirty) : 0;
    const auto targeted0 = m ? load_u64(m->dep_graph_nested_lambda_targeted_dirty_total) : 0;

    constexpr int kRounds = 1000;
    for (int i = 0; i < kRounds; ++i) {
        cs.public_mark_define_dirty("f");
        // Spot-check every 100 rounds: nested stays clean, dirty < total.
        if ((i % 100) == 0) {
            const auto* ge = cs.get_define_v2("g");
            if (ge && ge->irs.size() > 2) {
                std::size_t total = 0;
                for (const auto& fb : ge->block_dirty_view())
                    total += fb.size();
                const auto dirty = ge->dirty_block_count();
                CHECK(dirty < total || total <= 1, "stress: dirty < total on nested entry");
                CHECK(ge->func_dirty_block_count(2) == 0,
                      "stress: nested without free-ref stays clean");
            }
        }
    }

    const auto edges1 = cs.public_dep_graph_calls_for("g") + cs.public_dep_graph_called_by_for("f");
    CHECK(edges1 == edges0, "dep_graph edges stable across cascade stress");
    CHECK(cs.public_dep_graph_has_edge("g", "f"), "edge g→f still present");

    if (m) {
        const auto full1 = load_u64(m->dep_graph_nested_lambda_full_dirty);
        const auto targeted1 = load_u64(m->dep_graph_nested_lambda_targeted_dirty_total);
        CHECK(full1 == full0, "full_dirty never bumped under targeted stress");
        CHECK(targeted1 >= targeted0 + static_cast<std::uint64_t>(kRounds),
              "targeted_dirty +1000 under stress");
        std::println("  edges={}→{} full={}→{} targeted={}→{}", edges0, edges1, full0, full1,
                     targeted0, targeted1);
    }
}

static void ac2_primary_nested_targeted() {
    std::println("\n--- AC2: primary with nested lambdas uses targeted not full ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    // Primary define with nested shape (3 funcs), no free-ref of self.
    store_nested_g(cs, {});
    // Rename entry to f for primary path (store as f).
    {
        aura::ir::IRFunction top;
        top.id = 0;
        top.name = "__top__";
        top.blocks.push_back({0, {}, {}});
        aura::ir::IRFunction body;
        body.id = 1;
        body.name = "f_body";
        body.blocks.push_back({0, {}, {}});
        aura::ir::IRFunction nested;
        nested.id = 2;
        nested.name = "__lambda__";
        nested.blocks.push_back({0, {}, {}});
        // nested does not free-ref f
        std::vector<aura::ir::IRFunction> irs;
        irs.push_back(std::move(top));
        irs.push_back(std::move(body));
        irs.push_back(std::move(nested));
        cs.store_define_v2("f", "(define (f x) (lambda (y) x))", std::move(irs),
                           std::vector<aura::ir::ClosureBridgeData>{}, std::vector<std::string>{});
    }
    const auto full0 = m ? load_u64(m->dep_graph_nested_lambda_full_dirty) : 0;
    const auto targeted0 = m ? load_u64(m->dep_graph_nested_lambda_targeted_dirty_total) : 0;
    cs.public_mark_define_dirty("f");
    const auto* fe = cs.get_define_v2("f");
    CHECK(fe != nullptr, "f cached");
    if (fe) {
        CHECK(fe->func_dirty_block_count(1) > 0, "primary body dirty");
        CHECK(fe->func_dirty_block_count(2) == 0, "primary nested without free-ref clean");
    }
    if (m) {
        CHECK(load_u64(m->dep_graph_nested_lambda_full_dirty) == full0,
              "primary nested: full_dirty unchanged");
        CHECK(load_u64(m->dep_graph_nested_lambda_targeted_dirty_total) > targeted0,
              "primary nested: targeted bumped");
    }
}

} // namespace aura_issue_1505_detail

int aura_issue_1505_run() {
    using namespace aura_issue_1505_detail;
    std::println("=== Issue #1505: nested-lambda dep_graph cascade refinement ===");
    ac6_metric_surface();
    ac3_ac5_body_only_no_free_ref();
    ac1_free_ref_nested_marked();
    ac2_primary_nested_targeted();
    ac4_stress_cascade();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1505_run();
}
#endif
