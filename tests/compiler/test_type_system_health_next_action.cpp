// @category: unit
// @reason: Issue #2462 — query:type-system-health next-action + repair_nodes
//          for Agent closed-loop.
//
//   AC1: Healthy empty → next-action=ok, repair empty
//   AC2: truncated / incomplete under production → full-solve (or rollback)
//   AC3: TIMEOUT + repair set → expand-dirty
//   AC4: castop over budget alone → annotate-dynamic
//   AC5: Pure decide; #2350 keys intact; schema-2462

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "compiler/type_system_health.hh"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::compute_type_system_health;
using aura::compiler::decide_type_system_next_action;
using aura::compiler::TypeSystemHealthSnapshot;
using aura::compiler::TypeSystemNextAction;
using aura::compiler::TypeSystemNextActionInput;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
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

static std::int64_t href_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:type-system-health\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: healthy empty → ok ──
static void ac1_healthy_ok() {
    std::println("\n--- #2462 AC1: healthy empty → next-action=ok ---");
    TypeSystemNextActionInput in; // defaults clean
    auto r = decide_type_system_next_action(in);
    CHECK(r.action == TypeSystemNextAction::Ok, "AC1: action Ok");
    CHECK(r.action_str == "ok", "AC1: str ok");
    CHECK(r.action_code == 0, "AC1: code 0");

    TypeSystemHealthSnapshot s;
    auto health = compute_type_system_health(s);
    CHECK(health.force_reason == "ok", "AC1: force-reason ok");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href_int(cs, "schema-2350") == 2350, "AC1: schema-2350 intact");
    CHECK(href_int(cs, "schema-2462") == 2462, "AC1: schema-2462");
    CHECK(href_int(cs, "next-action-code") == 0, "AC1: query next-action-code ok");
    CHECK(href_int(cs, "repair-nodes-count") == 0, "AC1: repair empty");
}

// ── AC2: truncated / incomplete production → full-solve ──
static void ac2_full_solve() {
    std::println("\n--- #2462 AC2: truncated / production incomplete → full-solve ---");
    {
        TypeSystemNextActionInput in;
        in.truncated_reverify = true;
        auto r = decide_type_system_next_action(in);
        CHECK(r.action == TypeSystemNextAction::FullSolve, "AC2: truncated → full-solve");
        CHECK(r.action_str == "full-solve", "AC2: str full-solve");
    }
    {
        TypeSystemNextActionInput in;
        in.production_defaults = true;
        in.blame_complete = false;
        in.unresolved_count = 3;
        in.solve_status = 2; // TIMEOUT
        auto r = decide_type_system_next_action(in);
        CHECK(r.action == TypeSystemNextAction::FullSolve ||
                  r.action == TypeSystemNextAction::Rollback,
              "AC2: incomplete production → full-solve/rollback");
    }
    {
        TypeSystemNextActionInput in;
        in.hard_gate_reject = true;
        auto r = decide_type_system_next_action(in);
        CHECK(r.action == TypeSystemNextAction::Rollback, "AC2: hard_gate → rollback");
        CHECK(r.action_str == "rollback", "AC2: str rollback");
    }
}

// ── AC3: TIMEOUT + repair → expand-dirty ──
static void ac3_expand_dirty() {
    std::println("\n--- #2462 AC3: TIMEOUT + repair_nodes → expand-dirty ---");
    TypeSystemNextActionInput in;
    in.solve_status = 2; // TIMEOUT
    in.production_defaults = false;
    in.production_escalated = false;
    in.repair_nodes_count = 4;
    in.suggested_roots_count = 2;
    in.unresolved_count = 4;
    auto r = decide_type_system_next_action(in);
    // Without production escalate, expand-dirty preferred over full-solve when
    // repair set present (full-solve only if production_escalated / truncated).
    CHECK(r.action == TypeSystemNextAction::ExpandDirty, "AC3: expand-dirty");
    CHECK(r.action_str == "expand-dirty", "AC3: str expand-dirty");
    CHECK(r.action_code == 2, "AC3: code 2");
}

// ── AC4: castop over budget alone → annotate-dynamic ──
static void ac4_annotate_dynamic() {
    std::println("\n--- #2462 AC4: castop over budget → annotate-dynamic ---");
    TypeSystemNextActionInput in;
    in.castop_over_budget = true;
    in.solve_status = 0;
    in.blame_complete = true;
    in.health_ok = true;
    in.force_reason = "ok";
    auto r = decide_type_system_next_action(in);
    CHECK(r.action == TypeSystemNextAction::AnnotateDynamic, "AC4: annotate-dynamic");
    CHECK(r.action_str == "annotate-dynamic", "AC4: str");
    CHECK(r.action_code == 1, "AC4: code 1");

    // force_reason path
    TypeSystemNextActionInput in2;
    in2.force_reason = "castop-density";
    in2.solve_status = 0;
    auto r2 = decide_type_system_next_action(in2);
    CHECK(r2.action == TypeSystemNextAction::AnnotateDynamic, "AC4: force_reason castop");
}

// ── AC5: pure + schema + source ──
static void ac5_pure_schema() {
    std::println("\n--- #2462 AC5: pure + schema-2462 + #2350 intact ---");
    TypeSystemNextActionInput in;
    in.castop_over_budget = true;
    auto a = decide_type_system_next_action(in);
    auto b = decide_type_system_next_action(in);
    CHECK(a.action == b.action && a.action_code == b.action_code, "AC5: pure identical");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto h1 = cs.eval("(engine:metrics \"query:type-system-health\")");
    auto h2 = cs.eval("(engine:metrics \"query:type-system-health\")");
    CHECK(h1 && is_hash(*h1) && h2 && is_hash(*h2), "AC5: two queries hash");
    CHECK(href_int(cs, "schema-2350") == 2350, "AC5: schema-2350");
    CHECK(href_int(cs, "schema-2462") == 2462, "AC5: schema-2462");
    CHECK(href_int(cs, "type-system-health-next-action-wired") == 1, "AC5: wired");
    CHECK(href_int(cs, "health-bp") >= 0, "AC5: health-bp present");
    CHECK(href_int(cs, "type-system-health-wired") == 1, "AC5: #2350 wired");

    const auto hh = read_file("src/compiler/type_system_health.hh");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(hh.find("decide_type_system_next_action") != std::string::npos, "AC5: decide helper");
    CHECK(hh.find("Issue #2462") != std::string::npos, "AC5: header cites #2462");
    CHECK(q.find("next-action") != std::string::npos, "AC5: query next-action");
    CHECK(q.find("repair-nodes-count") != std::string::npos, "AC5: repair-nodes-count");
    CHECK(q.find("schema-2462") != std::string::npos, "AC5: query schema");
    CHECK(q.find("schema-2350") != std::string::npos, "AC5: #2350 retained");
}

} // namespace

int run_test_type_system_health_next_action() {
    std::println("=== Issue #2462: type-system-health next-action ===");
    apply_dev_audit_defaults();
    ac1_healthy_ok();
    ac2_full_solve();
    ac3_expand_dirty();
    ac4_annotate_dynamic();
    ac5_pure_schema();
    std::println("\n=== #2462 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_type_system_health_next_action();
}
#endif
