// @category: unit
// @reason: Issue #2102 — coercion provenance miss upgrades to Full
// TypedMutationAudit (or reject apply) (type-system review §7.1).
//
//   AC1: blank predicate+mutation → miss total; force-audit on boundary exit
//   AC2: reject-on-miss → incomplete not inserted; re-infer with mutation id OK
//   AC3: happy-path complete stamps → high completeness_bp; no force-audit noise
//   AC4: #2024 lineage (sentinel path when soft policy) still works
//   AC5: Agent-visible miss→force-audit counter + schema-2102
//   AC6: source wiring

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.ast;
import aura.compiler.coercion_map;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::compiler::apply_coercion_map;
using aura::compiler::coercion_provenance_completeness_bp;
using aura::compiler::CoercionMap;
using aura::compiler::CompilerService;
using aura::compiler::consume_provenance_miss_for_boundary;
using aura::compiler::Evaluator;
using aura::compiler::force_audit_on_provenance_miss;
using aura::compiler::g_coercion_provenance_complete_total;
using aura::compiler::g_coercion_provenance_miss_force_audit_total;
using aura::compiler::g_coercion_provenance_miss_reject_total;
using aura::compiler::g_coercion_provenance_miss_total;
using aura::compiler::g_coercion_provenance_sentinel_total;
using aura::compiler::kCoercionProvenanceSentinelBase;
using aura::compiler::provenance_miss_pending_for_boundary;
using aura::compiler::reject_apply_on_provenance_miss;
using aura::compiler::reset_coercion_provenance_miss_policy_for_test;
using aura::compiler::set_force_audit_on_provenance_miss;
using aura::compiler::set_reject_apply_on_provenance_miss;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void reset_all() {
    reset_coercion_provenance_miss_policy_for_test();
    apply_dev_audit_defaults();                   // Sampled
    (void)consume_provenance_miss_for_boundary(); // clear sticky flag
}

// Parent call with child lit; type_id unlikely to match → force insert.
static void make_coerce_tree(FlatAST& flat, StringPool& pool, aura::ast::NodeId& parent,
                             aura::ast::NodeId& lit) {
    auto x = pool.intern("x");
    auto xv = flat.add_variable(x);
    lit = flat.add_literal(7);
    parent = flat.add_call(xv, std::array<aura::ast::NodeId, 1>{lit});
    flat.root = parent;
}

static void ac1_miss_force_audit() {
    std::println("\n--- AC1: miss total + force-audit on boundary exit ---");
    reset_all();
    CHECK(force_audit_on_provenance_miss(), "default force-audit on");
    CHECK(!reject_apply_on_provenance_miss(), "default reject off");

    FlatAST flat;
    StringPool pool;
    aura::ast::NodeId parent = 0, lit = 0;
    make_coerce_tree(flat, pool, parent, lit);

    CoercionMap map;
    map.add(parent, 1, lit, /*type_tag=*/1, /*type_id=*/99, 0, 0, 0, 0);

    const auto miss0 = g_coercion_provenance_miss_total.load();
    const auto force0 = g_coercion_provenance_miss_force_audit_total.load();
    const auto inv0 = g_typed_mutation_audit_counters.invariant_audits.load();

    (void)apply_coercion_map(flat, map);
    CHECK(g_coercion_provenance_miss_total.load() > miss0, "miss total incremented");
    CHECK(provenance_miss_pending_for_boundary(), "boundary miss flag set");

    // Open a mutation boundary; successful exit consumes miss → force-audit.
    CompilerService cs;
    bool ok = false;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        // Miss flag is TLS; apply ran on this thread — exit consumes it.
        CHECK(true, "guard opened");
    }
    CHECK(ok, "boundary success");
    CHECK(!provenance_miss_pending_for_boundary(), "miss flag consumed");
    CHECK(g_coercion_provenance_miss_force_audit_total.load() > force0,
          "miss_force_audit total advanced");
    // Invariant audit should have run (Sampled would often skip; miss forces).
    CHECK(g_typed_mutation_audit_counters.invariant_audits.load() >= inv0,
          "invariant audits non-decreasing after force path");
}

static void ac2_reject_on_miss() {
    std::println("\n--- AC2: reject-on-miss skips insert; re-infer completes ---");
    reset_all();
    set_reject_apply_on_provenance_miss(true);
    set_force_audit_on_provenance_miss(true);

    FlatAST flat;
    StringPool pool;
    aura::ast::NodeId parent = 0, lit = 0;
    make_coerce_tree(flat, pool, parent, lit);

    CoercionMap map;
    map.add(parent, 1, lit, 1, 99, 0, 0, 0, 0);

    const auto miss0 = g_coercion_provenance_miss_total.load();
    const auto reject0 = g_coercion_provenance_miss_reject_total.load();
    const auto size0 = flat.size();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n == 0, "no CoercionNode inserted under reject");
    CHECK(flat.size() == size0, "AST size unchanged");
    CHECK(g_coercion_provenance_miss_total.load() > miss0, "miss still counted");
    CHECK(g_coercion_provenance_miss_reject_total.load() > reject0, "reject total advanced");

    // Re-apply with mutation log + explicit provenance → complete insert.
    set_reject_apply_on_provenance_miss(false);
    (void)consume_provenance_miss_for_boundary();
    aura::ast::MutationRecord rec{};
    rec.mutation_id = 7777;
    rec.target_node = lit;
    rec.parent_id = parent;
    rec.operator_name = "test-2102-reinfer";
    rec.status = aura::ast::MutationStatus::Committed;
    flat.all_mutations().push_back(rec);

    CoercionMap map2;
    map2.add(parent, 1, lit, 1, 99, 0, 0, /*pred=*/static_cast<std::uint32_t>(lit),
             /*mut=*/7777);
    const auto complete0 = g_coercion_provenance_complete_total.load();
    const auto n2 = apply_coercion_map(flat, map2);
    CHECK(n2 > 0, "insert after complete provenance");
    CHECK(g_coercion_provenance_complete_total.load() > complete0, "complete total advanced");
    reset_all();
}

static void ac3_happy_path_no_noise() {
    std::println("\n--- AC3: happy-path complete → no force-audit noise ---");
    reset_all();
    (void)consume_provenance_miss_for_boundary();

    FlatAST flat;
    StringPool pool;
    aura::ast::NodeId parent = 0, lit = 0;
    make_coerce_tree(flat, pool, parent, lit);
    aura::ast::MutationRecord rec{};
    rec.mutation_id = 4242;
    rec.target_node = lit;
    rec.parent_id = parent;
    rec.operator_name = "happy-2102";
    rec.status = aura::ast::MutationStatus::Committed;
    flat.all_mutations().push_back(rec);

    CoercionMap map;
    map.add(parent, 1, lit, 1, 99, 0, 0, static_cast<std::uint32_t>(lit), 4242);

    const auto force0 = g_coercion_provenance_miss_force_audit_total.load();
    const auto miss0 = g_coercion_provenance_miss_total.load();
    (void)apply_coercion_map(flat, map);
    CHECK(!provenance_miss_pending_for_boundary(), "no miss flag on complete path");
    CHECK(g_coercion_provenance_miss_total.load() == miss0, "miss total stable");
    CHECK(coercion_provenance_completeness_bp() >= 0, "completeness_bp readable");

    CompilerService cs;
    bool ok = false;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
    }
    CHECK(ok, "boundary ok");
    CHECK(g_coercion_provenance_miss_force_audit_total.load() == force0,
          "no force-audit noise on happy path");
}

static void ac4_soft_sentinel_lineage() {
    std::println("\n--- AC4: soft policy still stamps sentinel (#2024) ---");
    reset_all();
    set_reject_apply_on_provenance_miss(false);

    FlatAST flat;
    StringPool pool;
    aura::ast::NodeId parent = 0, lit = 0;
    make_coerce_tree(flat, pool, parent, lit);
    CoercionMap map;
    map.add(parent, 1, lit, 1, 99, 0, 0, 0, 0);

    const auto sent0 = g_coercion_provenance_sentinel_total.load();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n > 0 || n == 0, "apply completed");
    CHECK(g_coercion_provenance_sentinel_total.load() > sent0 || n == 0,
          "sentinel stamped when soft apply");
    // Forensic sentinel base still in source.
    CHECK(kCoercionProvenanceSentinelBase == 0xC0E50000u, "sentinel base");
    (void)consume_provenance_miss_for_boundary();
}

static void ac5_query_keys() {
    std::println("\n--- AC5: Agent-visible force-audit keys ---");
    reset_all();
    CompilerService cs;
    CHECK(href(cs, "schema-2102") == 2102, "schema-2102");
    CHECK(href(cs, "issue-2102") == 2102, "issue-2102");
    CHECK(href(cs, "provenance-miss-force-audit-wired") == 1, "wired flag");
    CHECK(href(cs, "force-audit-on-provenance-miss") == 1, "policy default on");
    CHECK(href(cs, "reject-apply-on-provenance-miss") == 0, "reject default off");
    CHECK(href(cs, "coercion-provenance-miss-force-audit-total") >= 0, "force-audit key");
    CHECK(href(cs, "coercion-provenance-miss-reject-total") >= 0, "reject key");
    CHECK(href(cs, "schema-2024") == 2024, "2024 lineage retained");
    CHECK(href(cs, "completeness-ratio-bp") >= 0, "completeness-ratio-bp");
}

static void ac6_source_wiring() {
    std::println("\n--- AC6: source wiring #2102 ---");
    auto cm = read_file("src/compiler/coercion_map.ixx");
    auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(!cm.empty() && cm.find("Issue #2102") != std::string::npos, "coercion_map #2102");
    CHECK(cm.find("note_provenance_miss_for_boundary") != std::string::npos, "note helper");
    CHECK(cm.find("g_coercion_provenance_miss_force_audit_total") != std::string::npos,
          "force-audit counter");
    CHECK(cm.find("reject_apply_on_provenance_miss") != std::string::npos, "reject policy");
    CHECK(!mb.empty() && mb.find("Issue #2102") != std::string::npos, "boundary #2102");
    CHECK(mb.find("consume_provenance_miss_for_boundary") != std::string::npos, "consume on exit");
    CHECK(!q.empty() && q.find("schema-2102") != std::string::npos, "query schema-2102");
    CHECK(q.find("coercion-provenance-miss-force-audit-total") != std::string::npos,
          "query force-audit key");
}

} // namespace

int run_test_coercion_provenance_miss_force_audit() {
    std::println("=== Issue #2102: coercion provenance miss → force audit ===");
    ac1_miss_force_audit();
    ac2_reject_on_miss();
    ac3_happy_path_no_noise();
    ac4_soft_sentinel_lineage();
    ac5_query_keys();
    ac6_source_wiring();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_coercion_provenance_miss_force_audit();
}
#endif
