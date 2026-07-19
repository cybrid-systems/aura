// @category: integration
// @reason: Issue #1529 — complete blame/provenance chain for
// occurrence narrowing after delta conflicts in typed_mutate.
//
// Non-duplicative of #690 (mutation-scoped blame metrics),
// #745 (occurrence-priority reverify + basic complete total),
// #573 (cross-delta CONFLICT detection), #537/#691 (CoercionEntry
// provenance fields). This issue dumps a full chain:
// source_mutation_id + predicate_cond_node + affected NodeId.
//
//   AC1: add_delta stamps active_mutation_id provenance
//   AC2: conflict with mutation_id → complete metric + non-empty chain
//   AC3: rich context (mutation+predicate+affected) → rich_complete
//   AC4: missing mutation_id → incomplete + stale-blame metrics
//   AC5: chain frames carry constraint indices / kinds
//   AC6: dump triple present on rich conflict (mut+pred+node)
//   AC7: metrics length monotically advances with frame count
//   AC8: existing cross-delta conflict path still CONFLICT

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <string>

import std;
import aura.core.type;
import aura.compiler.type_checker;

namespace aura_issue_1529_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::DeltaBlameChain;
using aura::compiler::SolveResult;
using aura::core::TypeRegistry;
using aura::test::g_failed;
using aura::test::g_passed;

static SolveResult solve_delta_with(ConstraintSystem& cs, Constraint c) {
    cs.add_delta(std::move(c));
    return cs.solve_delta();
}

static void ac1_add_delta_stamps_mutation() {
    std::println("\n--- AC1: add_delta stamps active_mutation_id ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    cs.set_active_mutation_id(152901);
    const auto t = cs.fresh_var();
    Constraint c{Constraint::EQUAL, t, reg.int_type()};
    CHECK(c.source_mutation_id == 0, "pre-stamp provenance empty");
    cs.add_delta(c);
    // After add_delta, the stored constraint should carry the stamp.
    // Read via a conflict path that dumps the chain frame.
    cs.mark_touched_on_delta(t, false);
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
          "conflict after stamped Int binding");
    const auto& chain = cs.last_blame_chain();
    CHECK(!chain.frames.empty(), "chain frames non-empty");
    bool found_mut = false;
    for (const auto& f : chain.frames) {
        if (f.source_mutation_id == 152901)
            found_mut = true;
    }
    CHECK(found_mut, "frame carries stamped source_mutation_id");
}

static void ac2_complete_with_mutation_id() {
    std::println("\n--- AC2: conflict with mutation_id → complete metric ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.set_active_mutation_id(152902);
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "T~Int baseline");
    const auto complete0 = metrics.constraint_blame_chain_complete_total.load();
    const auto len0 = metrics.constraint_blame_chain_length_total.load();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
          "T~String CONFLICT");
    const auto complete1 = metrics.constraint_blame_chain_complete_total.load();
    const auto len1 = metrics.constraint_blame_chain_length_total.load();
    std::println("  complete {} -> {}, length {} -> {}", complete0, complete1, len0, len1);
    CHECK(complete1 > complete0, "constraint_blame_chain_complete_total bumped");
    CHECK(len1 > len0, "constraint_blame_chain_length_total bumped");
    CHECK(!cs.last_blame_chain().frames.empty(), "last_blame_chain has frames");
    CHECK(cs.last_blame_chain().root_mutation_id == 152902, "root_mutation_id set");
}

static void ac3_rich_complete_triple() {
    std::println("\n--- AC3: rich context → rich_complete metric ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.set_active_mutation_id(152903);
    // predicate_cond_node=42, affected_node=99 (synthetic NodeIds)
    cs.set_active_blame_context(/*predicate_cond_node=*/42, /*affected_node=*/99);
    cs.push_blame_affected_node(100);
    cs.push_blame_affected_node(101);

    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "baseline");
    const auto rich0 = metrics.constraint_blame_chain_rich_complete_total.load();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
          "conflict with rich context");
    const auto rich1 = metrics.constraint_blame_chain_rich_complete_total.load();
    const auto& chain = cs.last_blame_chain();
    std::println("  rich_complete {} -> {}, frames={}, complete={}", rich0, rich1,
                 chain.frames.size(), chain.complete);
    CHECK(rich1 > rich0, "constraint_blame_chain_rich_complete_total bumped");
    CHECK(chain.complete, "DeltaBlameChain.complete true for triple");

    bool has_mut = false, has_pred = false, has_node = false;
    for (const auto& f : chain.frames) {
        if (f.source_mutation_id == 152903)
            has_mut = true;
        if (f.predicate_cond_node == 42)
            has_pred = true;
        if (f.affected_node == 99 || f.affected_node == 100 || f.affected_node == 101)
            has_node = true;
    }
    CHECK(has_mut, "chain has source_mutation_id");
    CHECK(has_pred, "chain has predicate_cond_node");
    CHECK(has_node, "chain has affected NodeId sequence");
}

static void ac4_incomplete_without_mutation() {
    std::println("\n--- AC4: no mutation_id → incomplete metrics ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    // No set_active_mutation_id
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "baseline without mutation id");
    const auto stale0 = metrics.constraint_stale_blame_invalidation_total.load();
    const auto inc0 = metrics.cross_delta_blame_incomplete_total.load();
    const auto complete0 = metrics.constraint_blame_chain_complete_total.load();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
          "conflict without mutation id");
    const auto stale1 = metrics.constraint_stale_blame_invalidation_total.load();
    const auto inc1 = metrics.cross_delta_blame_incomplete_total.load();
    const auto complete1 = metrics.constraint_blame_chain_complete_total.load();
    std::println("  stale {} -> {}, incomplete {} -> {}, complete {} -> {}", stale0, stale1, inc0,
                 inc1, complete0, complete1);
    CHECK(stale1 > stale0, "stale-blame-invalidation bumped");
    CHECK(inc1 > inc0, "cross_delta_blame_incomplete_total bumped");
    CHECK(complete1 == complete0, "complete total not bumped without mutation_id");
}

static void ac5_frames_carry_constraint_meta() {
    std::println("\n--- AC5: frames carry constraint index / kind ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    cs.set_active_mutation_id(152905);
    cs.set_active_blame_context(7, 8);
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "baseline");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.bool_type()}) == SolveResult::CONFLICT,
          "conflict");
    const auto& chain = cs.last_blame_chain();
    CHECK(chain.frames.size() >= 1, "at least one frame");
    bool has_kind = false;
    bool has_cidx = false;
    for (const auto& f : chain.frames) {
        if (f.kind == static_cast<std::uint8_t>(Constraint::EQUAL))
            has_kind = true;
        if (f.constraint_index != UINT32_MAX)
            has_cidx = true;
    }
    CHECK(has_kind || has_cidx, "frame carries kind and/or constraint_index");
}

static void ac6_dump_triple_on_rich() {
    std::println("\n--- AC6: dump triple present on rich conflict ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    cs.set_active_mutation_id(152906);
    cs.set_active_blame_context(/*pred=*/55, /*affected=*/66);
    const auto t = cs.fresh_var();
    (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()});
    (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()});
    const auto& chain = cs.last_blame_chain();
    CHECK(chain.root_mutation_id == 152906, "root mutation");
    CHECK(chain.complete, "complete flag");
    // Explicit dump-style check for AI agents.
    std::string dump;
    dump += "mut=" + std::to_string(chain.root_mutation_id);
    for (const auto& f : chain.frames) {
        dump += " | frame{mut=" + std::to_string(f.source_mutation_id) +
                ",pred=" + std::to_string(f.predicate_cond_node) +
                ",node=" + std::to_string(f.affected_node) + "}";
    }
    std::println("  dump: {}", dump);
    CHECK(dump.find("mut=152906") != std::string::npos, "dump has mutation");
    CHECK(dump.find("pred=55") != std::string::npos, "dump has predicate");
    CHECK(dump.find("node=66") != std::string::npos, "dump has affected node");
}

static void ac7_length_matches_frames() {
    std::println("\n--- AC7: length metric advances with frames ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.set_active_mutation_id(152907);
    cs.set_active_blame_context(1, 2);
    cs.push_blame_affected_node(3);
    cs.push_blame_affected_node(4);
    const auto t = cs.fresh_var();
    (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()});
    const auto len0 = metrics.constraint_blame_chain_length_total.load();
    (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()});
    const auto len1 = metrics.constraint_blame_chain_length_total.load();
    const auto nframes = cs.last_blame_chain().frames.size();
    std::println("  length {} -> {} frames={}", len0, len1, nframes);
    CHECK(len1 >= len0 + nframes, "length += frame count");
    CHECK(nframes >= 2, "multiple frames from constraints + affected seq");
}

static void ac8_cross_delta_still_conflicts() {
    std::println("\n--- AC8: merged-var cross-delta still CONFLICT ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    cs.set_active_mutation_id(152908);
    const auto t = cs.fresh_var();
    const auto u = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "T~Int");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, u, reg.string_type()}) == SolveResult::SOLVED,
          "U~String");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, u}) == SolveResult::CONFLICT,
          "T~U merge CONFLICT");
    CHECK(cs.last_blame_chain().root_mutation_id == 152908, "blame root preserved");
}

} // namespace aura_issue_1529_detail

int aura_issue_1529_run() {
    using namespace aura_issue_1529_detail;
    std::println("=== Issue #1529: cross-delta blame provenance chain ===");
    ac1_add_delta_stamps_mutation();
    ac2_complete_with_mutation_id();
    ac3_rich_complete_triple();
    ac4_incomplete_without_mutation();
    ac5_frames_carry_constraint_meta();
    ac6_dump_triple_on_rich();
    ac7_length_matches_frames();
    ac8_cross_delta_still_conflicts();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE
int main() {
    return aura_issue_1529_run();
}
#endif
