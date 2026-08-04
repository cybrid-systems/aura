// @category: unit
// @reason: Issue #2620 — Soft must not ship incomplete CoercionNodes
//          (unify Soft/production proof surface). Phase A of issue.
//
//   AC1: Soft + incomplete → applied==0, no CoercionNode
//   AC2: Soft arms force-Full observe (slo force_armed / soft skip)
//   AC3: Production dual-require drop unchanged
//   AC4: Canary env restores #2317 insert
//   AC5: Additive schema-2620; #2317/#2562 counters retained
//   AC6: Source-cite #2620 on skip-insert branch

#include "compiler/coercion_provenance_policy.hh"
#include "compiler/typed_mutation_audit.h"
#include "test_harness.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.ast;
import aura.compiler.coercion_map;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::apply_coercion_map;
using aura::compiler::clear_coercion_active_mutation_context;
using aura::compiler::coercion_dual_require_active;
using aura::compiler::CoercionMap;
using aura::compiler::CompilerService;
using aura::compiler::g_coercion_dual_require_drop_total;
using aura::compiler::g_coercion_prov_slo_force_armed_total;
using aura::compiler::g_coercion_prov_slo_force_full_pending;
using aura::compiler::g_coercion_prov_slo_observe_only_total;
using aura::compiler::g_coercion_sampled_insert_incomplete_total;
using aura::compiler::g_coercion_soft_incomplete_skip_total;
using aura::compiler::g_coercion_unify_incomplete_skip_wired;
using aura::compiler::kCoercionUnifyIncompleteSkipIssue;
using aura::compiler::reset_coercion_provenance_miss_policy_for_test;
using aura::compiler::set_coercion_dual_require;
using aura::compiler::set_reject_apply_on_provenance_miss;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
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

// Tiny call tree matching #2562 fixture: call child_index 1 = lit.
static FlatAST make_tiny(StringPool& pool, aura::ast::NodeId& lit_out,
                         aura::ast::NodeId& call_out) {
    FlatAST flat;
    auto x = pool.intern("x");
    auto xv = flat.add_variable(x);
    auto lit = flat.add_literal(1);
    flat.set_type(lit, 0);
    auto call = flat.add_call(xv, std::array<aura::ast::NodeId, 1>{lit});
    flat.root = call;
    lit_out = lit;
    call_out = call;
    return flat;
}

static void reset_2620() {
    reset_for_test();
    reset_coercion_provenance_miss_policy_for_test();
    set_coercion_dual_require(false);
    set_reject_apply_on_provenance_miss(false);
    clear_coercion_active_mutation_context();
    ::unsetenv("AURA_COERCION_SAMPLED_INCOMPLETE_INSERT");
    ::unsetenv("AURA_COERCION_DUAL_REQUIRE");
    g_coercion_prov_slo_force_full_pending.store(0, std::memory_order_relaxed);
}

// ── AC1: Soft incomplete never inserts ──
static void ac1_soft_no_incomplete_insert() {
    std::println("\n--- #2620 AC1: Soft incomplete → no CoercionNode ---");
    CHECK(kCoercionUnifyIncompleteSkipIssue == 2620, "AC1: issue stamp");
    reset_2620();
    set_strategy(AuditStrategy::Sampled);

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);
    flat.set_type(lit, 99);

    CoercionMap map;
    map.add(call, 1, lit, 1, 1, 0, 0); // incomplete dual

    const auto skip0 = g_coercion_soft_incomplete_skip_total.load();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n == 0, "AC1: applied==0");
    CHECK(flat.get(call).child(1) == lit, "AC1: parent still points at original (no CoercionNode)");
    CHECK(g_coercion_soft_incomplete_skip_total.load() > skip0, "AC1: soft incomplete skip bumped");
}

// ── AC2: Soft arms force-Full ──
static void ac2_soft_arms_force_full() {
    std::println("\n--- #2620 AC2: Soft observe + arm force-Full ---");
    reset_2620();
    set_strategy(AuditStrategy::Sampled);

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);
    flat.set_type(lit, 99);
    CoercionMap map;
    map.add(call, 1, lit, 1, 1, 0, 0);

    const auto armed0 = g_coercion_prov_slo_force_armed_total.load();
    const auto obs0 = g_coercion_prov_slo_observe_only_total.load();
    (void)apply_coercion_map(flat, map);
    CHECK(g_coercion_prov_slo_force_full_pending.load() != 0, "AC2: force Full pending");
    CHECK(g_coercion_prov_slo_force_armed_total.load() > armed0 ||
              g_coercion_prov_slo_force_full_pending.load() != 0,
          "AC2: force armed total or pending set");
    CHECK(g_coercion_prov_slo_observe_only_total.load() > obs0, "AC2: observe-only bumped");
    // Soft default: no dual-require drop
    CHECK(!coercion_dual_require_active(), "AC2: dual-require off under Soft");
}

// ── AC3: dual-require production path unchanged ──
static void ac3_dual_require_unchanged() {
    std::println("\n--- #2620 AC3: dual-require drop still works ---");
    reset_2620();
    set_strategy(AuditStrategy::Full);
    set_coercion_dual_require(true);
    CHECK(coercion_dual_require_active(), "AC3: dual-require active");

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);
    flat.set_type(lit, 99);
    CoercionMap map;
    map.add(call, 1, lit, 1, 1, 0, 0);

    const auto drop0 = g_coercion_dual_require_drop_total.load();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n == 0, "AC3: dual incomplete drops");
    CHECK(g_coercion_dual_require_drop_total.load() > drop0, "AC3: dual drop counter bumped");
    CHECK(flat.get(call).child(1) == lit, "AC3: no CoercionNode under dual-require");
}

// ── AC4: canary restores insert ──
static void ac4_canary_restores_insert() {
    std::println("\n--- #2620 AC4: canary env restores #2317 insert ---");
    reset_2620();
    set_strategy(AuditStrategy::Sampled);
    set_reject_apply_on_provenance_miss(false);
    ::setenv("AURA_COERCION_SAMPLED_INCOMPLETE_INSERT", "1", 1);

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);
    flat.set_type(lit, 99);
    CoercionMap map;
    map.add(call, 1, lit, 1, 1, 0, 0);

    const auto ins0 = g_coercion_sampled_insert_incomplete_total.load();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n >= 1, "AC4: canary inserts CoercionNode");
    CHECK(flat.get(call).child(1) != lit, "AC4: parent rewritten to CoercionNode");
    CHECK(g_coercion_sampled_insert_incomplete_total.load() > ins0,
          "AC4: sampled_insert_incomplete bumped");
    ::unsetenv("AURA_COERCION_SAMPLED_INCOMPLETE_INSERT");
}

// ── AC5: schema ──
static void ac5_schema_source() {
    std::println("\n--- #2620 AC5: additive schema ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2620") == 2620, "AC5: schema-2620");
    CHECK(href(cs, "issue-2620") == 2620, "AC5: issue-2620");
    CHECK(href(cs, "coercion-soft-incomplete-skip-total") >= 0, "AC5: soft-skip key");
    CHECK(href(cs, "coercion-unify-incomplete-skip-wired") == 1, "AC5: wired");
    CHECK(href(cs, "schema-2317") == 2317, "AC5: schema-2317 retained");
    CHECK(href(cs, "schema-2562") == 2562, "AC5: schema-2562 retained");
    CHECK(href(cs, "coercion-sampled-insert-incomplete-total") >= 0, "AC5: #2317 counter key");
    CHECK(href(cs, "coercion-dual-require-drop-total") >= 0, "AC5: #2562 drop key");
    CHECK(g_coercion_unify_incomplete_skip_wired.load() == 1, "AC5: wired live");
}

// ── AC6: source-cite ──
static void ac6_source_cite() {
    std::println("\n--- #2620 AC6: source-cite skip-insert ---");
    const auto cm = read_file("src/compiler/coercion_map.ixx");
    CHECK(cm.find("#2620") != std::string::npos, "AC6: cites #2620");
    CHECK(cm.find("skip-insert branch") != std::string::npos, "AC6: skip-insert branch comment");
    CHECK(cm.find("arm_soft_incomplete_force_full_observe") != std::string::npos,
          "AC6: force arm helper");
    CHECK(cm.find("AURA_COERCION_SAMPLED_INCOMPLETE_INSERT") != std::string::npos,
          "AC6: canary env");
    CHECK(cm.find("Decision table") != std::string::npos ||
              cm.find("decision table") != std::string::npos ||
              cm.find("│ Off") != std::string::npos,
          "AC6: decision table documented");
}

} // namespace

int run_test_coercion_unify_incomplete_skip() {
    std::println("=== Issue #2620: unify Soft incomplete CoercionNode skip ===");
    ac1_soft_no_incomplete_insert();
    ac2_soft_arms_force_full();
    ac3_dual_require_unchanged();
    ac4_canary_restores_insert();
    ac5_schema_source();
    ac6_source_cite();
    std::println("\n=== #2620: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_coercion_unify_incomplete_skip();
}
#endif
