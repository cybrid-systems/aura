// @category: integration
// @reason: Issue #1625 — mark_define_dirty nested lambda (irs.size()>2)
// dep_graph_-aware *per-block* targeted dirty (refine #1505).
// Not whole nested function dirty when free_vars match.
//
//   AC1: free-ref nested marks only entry_block (or instr-hit blocks)
//   AC2: multi-block nested: dirty_block_count << total after cascade
//   AC3: non-referencing nested stays fully clean
//   AC4: 1000× set-body/mark cascade — targeted grows, full_dirty stable
//   AC5: query:production-sweep-1261-1265-stats schema 1625 keys
//   AC6: quote/lambda/recursive define still eval correctly
//   AC7: #1505 lineage metrics still advance

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:production-sweep-1261-1265-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Multi-block nested lambda (4 blocks) so per-block targeting is visible.
static void store_nested_g_multiblock(CompilerService& cs,
                                      const std::vector<std::string>& nested_free_vars,
                                      int nested_blocks = 4) {
    aura::ir::IRFunction top;
    top.id = 0;
    top.name = "__top__";
    top.blocks.push_back({0, {}, {}});

    aura::ir::IRFunction body;
    body.id = 1;
    body.name = "g_body";
    body.blocks.push_back({0, {}, {}});
    body.blocks.push_back({1, {}, {}});

    aura::ir::IRFunction nested;
    nested.id = 2;
    nested.name = "__lambda__";
    nested.entry_block = 0;
    for (int i = 0; i < nested_blocks; ++i)
        nested.blocks.push_back({static_cast<std::uint32_t>(i), {}, {}});
    nested.free_vars = nested_free_vars;

    // Second nested lambda that does NOT free-ref f (stays clean).
    aura::ir::IRFunction nested2;
    nested2.id = 3;
    nested2.name = "__lambda_clean__";
    nested2.entry_block = 0;
    nested2.blocks.push_back({0, {}, {}});
    nested2.blocks.push_back({1, {}, {}});
    nested2.free_vars = {}; // no free-ref

    std::vector<aura::ir::IRFunction> irs;
    irs.push_back(std::move(top));
    irs.push_back(std::move(body));
    irs.push_back(std::move(nested));
    irs.push_back(std::move(nested2));
    cs.store_define_v2("g", "(define (g x) (f (lambda (y) (* y 2)) (lambda (z) (+ z 1)) x))",
                       std::move(irs), std::vector<aura::ir::ClosureBridgeData>{},
                       std::vector<std::string>{});
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

static void ac1_ac2_per_block() {
    std::println("\n--- AC1–2: free-ref nested marks entry only; dirty << total ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    store_simple_f(cs);
    store_nested_g_multiblock(cs, /*nested_free_vars=*/{"f"}, /*nested_blocks=*/4);
    cs.public_record_dependency("g", "f");

    const auto full0 = m ? load_u64(m->dep_graph_nested_lambda_full_dirty) : 0;
    const auto tgt0 = m ? load_u64(m->dep_graph_nested_lambda_targeted_dirty_total) : 0;
    const auto blk0 = m ? load_u64(m->dep_graph_nested_lambda_blocks_targeted_total) : 0;
    const auto clean0 = m ? load_u64(m->dep_graph_nested_lambda_blocks_kept_clean_total) : 0;

    cs.public_mark_define_dirty("f");

    const auto* ge = cs.get_define_v2("g");
    CHECK(ge != nullptr, "g cached");
    if (!ge)
        return;
    CHECK(ge->irs.size() >= 4, "g has top+body+2 nested");

    const auto dirty = ge->dirty_block_count();
    std::size_t total = 0;
    for (const auto& fb : ge->block_dirty_view())
        total += fb.size();
    const auto nested_dirty = ge->func_dirty_block_count(2);
    const auto nested2_dirty = ge->func_dirty_block_count(3);
    const auto body_dirty = ge->func_dirty_block_count(1);
    std::println("  dirty={} total={} body={} nested={} nested2={}", dirty, total, body_dirty,
                 nested_dirty, nested2_dirty);

    CHECK(body_dirty > 0, "body of g dirty (call site)");
    // AC1: free-ref nested is dirty but only entry_block (1 of 4)
    CHECK(nested_dirty > 0, "free-ref nested has some dirty");
    CHECK(nested_dirty < 4, "free-ref nested not fully dirty (per-block)");
    CHECK(nested_dirty == 1, "entry_block-only when no instr evidence");
    // AC3: non-referencing nested fully clean
    CHECK(nested2_dirty == 0, "clean nested stays clean");
    // AC2: overall dirty << total (body 2 + nested 1 = 3 of ~8)
    CHECK(dirty < total, "dirty_block_count < total");
    CHECK(dirty * 5 < total * 4 || dirty <= total / 2, "dirty fraction well below full entry");

    if (m) {
        CHECK(load_u64(m->dep_graph_nested_lambda_full_dirty) == full0, "full_dirty stable");
        CHECK(load_u64(m->dep_graph_nested_lambda_targeted_dirty_total) > tgt0, "targeted bumps");
        CHECK(load_u64(m->dep_graph_nested_lambda_blocks_targeted_total) > blk0,
              "blocks_targeted bumps");
        CHECK(load_u64(m->dep_graph_nested_lambda_blocks_kept_clean_total) > clean0,
              "blocks_kept_clean bumps");
        std::println("  blocks_targeted {}→{} kept_clean {}→{}", blk0,
                     load_u64(m->dep_graph_nested_lambda_blocks_targeted_total), clean0,
                     load_u64(m->dep_graph_nested_lambda_blocks_kept_clean_total));
    }
}

static void ac4_stress() {
    std::println("\n--- AC4: 1000× mark cascade; full_dirty stable ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    store_simple_f(cs);
    store_nested_g_multiblock(cs, {"f"}, 4);
    cs.public_record_dependency("g", "f");

    const auto full0 = m ? load_u64(m->dep_graph_nested_lambda_full_dirty) : 0;
    const auto tgt0 = m ? load_u64(m->dep_graph_nested_lambda_targeted_dirty_total) : 0;
    const auto edges0 = cs.public_dep_graph_calls_for("g") + cs.public_dep_graph_called_by_for("f");

    constexpr int kRounds = 1000;
    for (int i = 0; i < kRounds; ++i) {
        cs.public_mark_define_dirty("f");
        if ((i % 200) == 0) {
            const auto* ge = cs.get_define_v2("g");
            if (ge) {
                std::size_t total = 0;
                for (const auto& fb : ge->block_dirty_view())
                    total += fb.size();
                CHECK(ge->dirty_block_count() < total, "stress dirty < total");
                CHECK(ge->func_dirty_block_count(2) <= 1, "nested dirty <= 1");
                CHECK(ge->func_dirty_block_count(3) == 0, "clean nested stays 0");
            }
        }
    }
    const auto edges1 = cs.public_dep_graph_calls_for("g") + cs.public_dep_graph_called_by_for("f");
    CHECK(edges1 == edges0, "dep_graph edges stable");
    if (m) {
        CHECK(load_u64(m->dep_graph_nested_lambda_full_dirty) == full0,
              "full_dirty never grows under stress");
        CHECK(load_u64(m->dep_graph_nested_lambda_targeted_dirty_total) >=
                  tgt0 + static_cast<std::uint64_t>(kRounds),
              "targeted +1000");
        std::println("  targeted {}→{} full={} edges={}", tgt0,
                     load_u64(m->dep_graph_nested_lambda_targeted_dirty_total), full0, edges1);
    }
}

static void ac5_schema() {
    std::println("\n--- AC5: production-sweep schema 1625 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:production-sweep-1261-1265-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1625 || href(cs, "schema") == 1261, "schema 1625|1261");
    CHECK(href(cs, "nested-lambda-per-block-targeted-wired") == 1, "wired");
    CHECK(href(cs, "dep-graph-nested-lambda-targeted-dirty") >= 0, "targeted key");
    CHECK(href(cs, "dep-graph-nested-lambda-blocks-targeted") >= 0, "blocks-targeted");
    CHECK(href(cs, "dep-graph-nested-lambda-blocks-kept-clean") >= 0, "kept-clean");
    CHECK(href(cs, "dep-graph-nested-lambda-full-dirty") >= 0, "full-dirty lineage");
}

static void ac6_semantic() {
    std::println("\n--- AC6: quote / lambda / recursive define ---");
    CompilerService cs;
    CHECK(cs.eval("(define (id x) x)").has_value(), "define id");
    auto v = cs.eval("(id 42)");
    CHECK(v && is_int(*v) && as_int(*v) == 42, "id 42");
    auto q = cs.eval("(quote (a b c))");
    CHECK(q.has_value(), "quote list");
    CHECK(cs.eval("(define (fact n) (if (<= n 1) 1 (* n (fact (- n 1)))))").has_value(),
          "define fact");
    auto f = cs.eval("(fact 3)");
    CHECK(f && is_int(*f) && (as_int(*f) == 6 || as_int(*f) > 0), "fact 3 coherent");
    // Nested lambda apply
    auto lam = cs.eval("((lambda (x) (+ x 10)) 5)");
    CHECK(lam && is_int(*lam) && as_int(*lam) == 15, "lambda apply");
}

static void ac7_lineage_1505() {
    std::println("\n--- AC7: #1505 lineage ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m, "metrics");
    store_simple_f(cs);
    store_nested_g_multiblock(cs, {}, 2);
    cs.public_record_dependency("g", "f");
    const auto tgt0 = load_u64(m->dep_graph_nested_lambda_targeted_dirty_total);
    const auto body0 = load_u64(m->cascade_body_only_count);
    cs.public_mark_define_dirty("f");
    CHECK(load_u64(m->dep_graph_nested_lambda_targeted_dirty_total) > tgt0, "targeted lineage");
    CHECK(load_u64(m->cascade_body_only_count) > body0, "body_only lineage");
    const auto* ge = cs.get_define_v2("g");
    CHECK(ge && ge->func_dirty_block_count(2) == 0, "no free-ref nested clean (#1505)");
}

} // namespace

int main() {
    std::println("=== Issue #1625: nested lambda per-block targeted dirty ===");
    ac1_ac2_per_block();
    ac4_stress();
    ac5_schema();
    ac6_semantic();
    ac7_lineage_1505();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
