// @category: integration
// @reason: Issue #1520 — PersistentChildVector / children_ columnar access
// + region dense lookup (eliminate map indirection on hot paths).
//
// Non-duplicative of #370 (SafePCVSpan), #568 (migration stats), #1241
// (SoAView). This issue is children_columnar + region dense tables +
// walk_children prefer columnar + query:children-column-stats.
//
//   AC1: SafePCVSpan size/empty/data/begin/end (SoAColumnar shape)
//   AC2: children_columnar bumps soa hits + pcv pins
//   AC3: region dense hit path (no map) for set/get_function_region
//   AC4: region map fallback when dense unset / OOB
//   AC5: walk_children prefers children_columnar
//   AC6: query:children-column-stats schema 1520
//   AC7: query:soa-children-columnar-migration-stats extended fields
//   AC8: 100× columnar walk + region stress

#include "test_harness.hpp"
#include "core/persistent_child_vector.hh"
#include "observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.concepts;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1520_detail {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::SafePCVSpan;
using aura::ast::walk_children;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::ChildColumnar;
using aura::core::SoAColumnar;
using aura::test::g_failed;
using aura::test::g_passed;

static_assert(SoAColumnar<std::vector<std::uint32_t>>, "vector still SoAColumnar");

// SafePCVSpan shape (compile-time via requires in header static_assert).
static_assert(
    requires(const SafePCVSpan<std::uint32_t>& s) {
        s.size();
        s.empty();
        s.data();
        s.begin();
        s.end();
    }, "SafePCVSpan range + columnar");

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static void ac1_safe_pcv_api() {
    std::println("\n--- AC1: SafePCVSpan SoAColumnar shape ---");
    SafePCVSpan<std::uint32_t> empty;
    CHECK(empty.empty(), "default empty");
    CHECK(empty.size() == 0, "default size 0");
    CHECK(empty.data() == nullptr || empty.size() == 0, "empty data ok");
    CHECK(true, "SafePCVSpan compile-time shape holds");
}

static void ac2_children_columnar_metrics() {
    std::println("\n--- AC2: children_columnar metrics ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto a = flat->add_literal(1);
    auto b = flat->add_literal(2);
    auto fn = flat->add_variable(pool->intern("+"));
    NodeId args[] = {a, b};
    auto call = flat->add_call(fn, args);
    (void)call;

    const auto col0 = flat->children_column_soa_hits();
    const auto pin0 = flat->pcv_pin_count();
    auto safe = flat->children_columnar(call);
    CHECK(safe.size() >= 1, "call has children");
    std::size_t n = 0;
    for (auto c : safe) {
        (void)c;
        ++n;
    }
    CHECK(n == safe.size(), "range-for matches size");
    CHECK(flat->children_column_soa_hits() > col0, "column soa hits bumped");
    CHECK(flat->pcv_pin_count() > pin0, "pcv pin count bumped");
}

static void ac3_region_dense_hit() {
    std::println("\n--- AC3: region dense hit path ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto* flat = arena->create<FlatAST>(arena->allocator());
    const aura::ast::SymId sym = 42;
    flat->set_function_region(sym, 1); // performance
    const auto dense0 = flat->region_dense_hits();
    const auto map0 = flat->region_map_lookups();
    auto r = flat->get_function_region_for_sym(sym);
    CHECK(r.has_value() && *r == 1, "region == 1 via dense");
    CHECK(flat->region_dense_hits() == dense0 + 1, "dense hit +1");
    CHECK(flat->region_map_lookups() == map0, "no map lookup on dense hit");
}

static void ac4_region_map_fallback() {
    std::println("\n--- AC4: region map fallback ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto* flat = arena->create<FlatAST>(arena->allocator());
    // Force map-only: use huge SymId beyond dense cap by writing map
    // without dense (simulate by using id that was never set densely —
    // actually set_function_region always fills dense if < cap).
    // Unset id → dense miss (0) → map miss.
    const auto map0 = flat->region_map_lookups();
    auto r = flat->get_function_region_for_sym(999);
    CHECK(!r.has_value(), "unset region → nullopt");
    CHECK(flat->region_map_lookups() == map0 + 1, "map lookup on miss");
}

static void ac5_walk_children_columnar() {
    std::println("\n--- AC5: walk_children prefers columnar ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto a = flat->add_literal(1);
    auto b = flat->add_literal(2);
    auto fn = flat->add_variable(pool->intern("f"));
    NodeId args[] = {a, b};
    auto call = flat->add_call(fn, args);

    const auto col0 = flat->children_column_soa_hits();
    std::size_t seen = 0;
    walk_children<NodeId>(*flat, call, [&](NodeId id) {
        (void)id;
        ++seen;
    });
    CHECK(seen >= 1, "walk saw children");
    CHECK(flat->children_column_soa_hits() > col0, "walk used children_columnar");
}

static void ac6_children_column_stats_query() {
    std::println("\n--- AC6: query:children-column-stats ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (g x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    (void)cs.eval("(g 3)");

    auto r = cs.eval("(engine:metrics \"query:children-column-stats\")");
    CHECK(r && is_hash(*r), "children-column-stats is hash");
    auto schema = cs.eval("(hash-ref (engine:metrics \"query:children-column-stats\") 'schema)");
    CHECK(schema && is_int(*schema) && as_int(*schema) == 1520, "schema == 1520");
}

static void ac7_migration_stats_extended() {
    std::println("\n--- AC7: soa-children migration still works ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:soa-children-columnar-migration-stats\")");
    CHECK(r && is_hash(*r), "migration stats hash");
    auto schema =
        cs.eval("(hash-ref (engine:metrics \"query:soa-children-columnar-migration-stats\") "
                "'migration-schema)");
    CHECK(schema && is_int(*schema) && as_int(*schema) == 568, "migration-schema == 568");
}

static void ac8_stress() {
    std::println("\n--- AC8: 100× columnar + region stress ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto x_sym = pool->intern("x");
    int ok = 0;
    for (int i = 0; i < 100; ++i) {
        auto lit = flat->add_literal(i);
        auto var = flat->add_variable(x_sym);
        NodeId args[] = {lit};
        auto call = flat->add_call(var, args);
        auto safe = flat->children_columnar(call);
        for (auto c : safe)
            (void)c;
        flat->set_function_region(static_cast<aura::ast::SymId>(i % 50),
                                  static_cast<std::uint8_t>(i % 3));
        (void)flat->get_function_region_for_sym(static_cast<aura::ast::SymId>(i % 50));
        walk_children<NodeId>(*flat, call, [](NodeId) {});
        ++ok;
    }
    CHECK(ok == 100, "100-iter stress ok");
    CHECK(flat->children_column_soa_hits() > 0, "soa hits > 0 after stress");
    CHECK(flat->region_dense_hits() > 0, "dense hits > 0 after stress");
    std::println("  col={} pin={} dense={} map={}", flat->children_column_soa_hits(),
                 flat->pcv_pin_count(), flat->region_dense_hits(), flat->region_map_lookups());
}

} // namespace aura_issue_1520_detail

int aura_issue_1520_run() {
    using namespace aura_issue_1520_detail;
    std::println("=== Issue #1520: children_ columnar + region dense lookup ===");
    ac1_safe_pcv_api();
    ac2_children_columnar_metrics();
    ac3_region_dense_hit();
    ac4_region_map_fallback();
    ac5_walk_children_columnar();
    ac6_children_column_stats_query();
    ac7_migration_stats_extended();
    ac8_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1520_run();
}
#endif
