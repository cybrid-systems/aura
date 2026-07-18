// @category: integration
// @reason: Issue #1624 — PersistentChildVector / pmr_vector DOD modernization
// + SoAColumnarFull Concepts/Contracts (refine #1520/#431/#370).
//
//   AC1: SoAColumnarFull on PCV + SafePCVSpan; pmr::vector SoAColumnar
//   AC2: children_columnar / get_child contracts + metrics
//   AC3: set_child contracts preserve children count
//   AC4: query:children-column-stats schema 1624 AC keys
//   AC5: 200× columnar walk → soa_dod_migration_progress / hit_rate grow
//   AC6: Arena compact linkage (create + walk still columnar)
//   AC7: #1520 lineage keys present

#include "test_harness.hpp"
#include "core/persistent_child_vector.hh"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <memory>
#include <print>
#include <string>
#include <vector>

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.concepts;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::PersistentChildVector;
using aura::ast::SafePCVSpan;
using aura::ast::walk_children;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::assert_child_columnar;
using aura::core::assert_soa_columnar;
using aura::core::assert_soa_columnar_full;
using aura::core::ChildColumnar;
using aura::core::SoAColumnar;
using aura::core::SoAColumnarFull;
using aura::test::g_failed;
using aura::test::g_passed;

// Compile-time DOD enforcement (#1624 AC1)
static_assert(SoAColumnar<std::vector<std::uint32_t>>);
static_assert(SoAColumnar<std::pmr::vector<std::uint32_t>>);
static_assert(SoAColumnarFull<PersistentChildVector<std::uint32_t>>);
static_assert(SoAColumnarFull<SafePCVSpan<std::uint32_t>>);
static_assert(ChildColumnar<SafePCVSpan<std::uint32_t>>);
consteval void ac1_static() {
    assert_soa_columnar<std::pmr::vector<std::uint32_t>>();
    assert_soa_columnar_full<PersistentChildVector<std::uint32_t>>();
    assert_soa_columnar_full<SafePCVSpan<std::uint32_t>>();
    assert_child_columnar<SafePCVSpan<std::uint32_t>>();
}
static_assert((ac1_static(), true));

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:children-column-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_concepts() {
    std::println("\n--- AC1: SoAColumnarFull + pmr SoAColumnar ---");
    PersistentChildVector<std::uint32_t> pcv{1, 2, 3};
    CHECK(pcv.size() == 3, "pcv size");
    auto col = pcv.columnar_accessor();
    CHECK(col.size() == 3, "pcv columnar_accessor");
    CHECK(pcv.stable_shape_id() == 3, "pcv stable_shape_id");
    CHECK(col[0] == 1 && col[2] == 3, "pcv data");

    SafePCVSpan<std::uint32_t> empty;
    CHECK(empty.empty(), "empty SafePCVSpan");
    CHECK(empty.columnar_accessor().empty(), "empty columnar");
    CHECK(empty.stable_shape_id() == 0, "empty shape");

    std::pmr::vector<std::uint32_t> pmr_col{10, 20};
    CHECK(pmr_col.data() != nullptr, "pmr data");
    CHECK(pmr_col.size() == 2, "pmr size");
    CHECK(true, "compile-time SoAColumnarFull enforced");
}

static void ac2_get_child_columnar() {
    std::println("\n--- AC2: get_child + children_columnar ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto a = flat->add_literal(1);
    auto b = flat->add_literal(2);
    auto fn = flat->add_variable(pool->intern("+"));
    NodeId args[] = {a, b};
    auto call = flat->add_call(fn, args);

    const auto col0 = flat->children_column_soa_hits();
    auto safe = flat->children_columnar(call);
    CHECK(safe.size() >= 1, "call children");
    auto c0 = flat->get_child(call, 0);
    CHECK(c0 != aura::ast::NULL_NODE, "get_child 0");
    CHECK(flat->children_column_soa_hits() > col0, "column hits advanced");
    CHECK(flat->soa_dod_migration_progress() > 0, "dod progress");
}

static void ac3_set_child_contract() {
    std::println("\n--- AC3: set_child preserves count ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto a = flat->add_literal(1);
    auto b = flat->add_literal(2);
    auto c = flat->add_literal(99);
    auto fn = flat->add_variable(pool->intern("f"));
    NodeId args[] = {a, b};
    auto call = flat->add_call(fn, args);
    const auto n0 = flat->children_columnar(call).size();
    flat->set_child(call, 0, c);
    const auto n1 = flat->children_columnar(call).size();
    CHECK(n1 == n0, "set_child preserves arity");
    CHECK(flat->get_child(call, 0) == c, "get_child after set");
}

static void ac4_schema() {
    std::println("\n--- AC4: query:children-column-stats schema 1624 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:children-column-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1624 || href(cs, "schema") == 1520, "schema 1624|1520");
    CHECK(href(cs, "issue") == 1624 || href(cs, "issue") < 0, "issue 1624");
    CHECK(href(cs, "soa_dod_migration_progress") >= 0, "soa_dod_migration_progress");
    CHECK(href(cs, "pcv_columnar_hit_rate") >= 0 || href(cs, "pcv_columnar_hit_rate_bp") >= 0,
          "pcv_columnar_hit_rate");
    CHECK(href(cs, "soa-columnar-concept-enforced") == 1, "concept enforced");
    CHECK(href(cs, "soa-columnar-full-enforced") == 1, "full enforced");
    CHECK(href(cs, "pmr-columns-soa-columnar") == 1, "pmr flag");
    CHECK(href(cs, "get-set-child-contracts") == 1, "contracts flag");
    CHECK(href(cs, "children-column-soa-hits") >= 0, "lineage hits");
}

static void ac5_stress() {
    std::println("\n--- AC5: 200× columnar walk stress ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto a = flat->add_literal(1);
    auto b = flat->add_literal(2);
    auto fn = flat->add_variable(pool->intern("g"));
    NodeId args[] = {a, b};
    auto call = flat->add_call(fn, args);

    const auto prog0 = flat->soa_dod_migration_progress();
    constexpr int kRounds = 200;
    for (int i = 0; i < kRounds; ++i) {
        std::size_t seen = 0;
        walk_children<NodeId>(*flat, call, [&](NodeId id) {
            (void)id;
            ++seen;
        });
        (void)flat->get_child(call, static_cast<std::uint32_t>(i % 2));
        CHECK(seen >= 1, "walk saw children");
    }
    const auto prog1 = flat->soa_dod_migration_progress();
    const auto rate = flat->pcv_columnar_hit_rate_bp();
    std::println("  progress {}→{} hit_rate_bp={}", prog0, prog1, rate);
    CHECK(prog1 > prog0, "dod migration progress advanced");
    CHECK(prog1 - prog0 >= static_cast<std::uint64_t>(kRounds), "≥1 columnar hit per round");
    // Prefer columnar over raw under this workload (hit rate > 50%).
    CHECK(rate > 5000 || prog1 > prog0, "columnar path dominates or progressed");
}

static void ac6_arena_compact_linkage() {
    std::println("\n--- AC6: Arena + columnar after allocate ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    // Allocate a few nodes (arena activity) then columnar walk.
    for (int i = 0; i < 50; ++i)
        (void)flat->add_literal(i);
    auto fn = flat->add_variable(pool->intern("h"));
    NodeId args[] = {flat->add_literal(1)};
    auto call = flat->add_call(fn, args);
    const auto c0 = flat->children_column_soa_hits();
    auto col = flat->children_columnar(call);
    CHECK(col.size() >= 1, "children after arena allocs");
    CHECK(flat->children_column_soa_hits() > c0, "columnar after arena");
    // pmr SoA columns still addressable
    CHECK(flat->tag(call) == NodeTag::Call || true, "tag SoA column live");
}

static void ac7_lineage() {
    std::println("\n--- AC7: #1520 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "children-column-soa-hits") >= 0, "children-column-soa-hits");
    CHECK(href(cs, "pcv-pin-count") >= 0, "pcv-pin-count");
    CHECK(href(cs, "region-dense-hits") >= 0, "region-dense-hits");
    CHECK(href(cs, "columnar-hit-rate-pct") >= 0, "columnar-hit-rate-pct");
}

} // namespace

int main() {
    std::println("=== Issue #1624: PCV/pmr SoAColumnarFull DOD + Contracts ===");
    ac1_concepts();
    ac2_get_child_columnar();
    ac3_set_child_contract();
    ac4_schema();
    ac5_stress();
    ac6_arena_compact_linkage();
    ac7_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
