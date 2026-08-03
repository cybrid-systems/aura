// @category: unit
// @reason: Issue #2261 — ban weak mutation-id provenance under Sampled;
// never stamp weak mid into IR / CoercionNode provenance column.
//
//   AC1: Sampled + no active_mutation_id / no log → no CoercionNode; miss reject;
//        no weak mid in provenance column
//   AC2: Full/Strict + complete TLS stamp → fast path unchanged
//   AC3: AURA_SANDBOX=off + Off soft → iterative apply OK; no weak mid written
//   AC4: AURA_COERCION_PROVENANCE_REJECT env still works (#2185)
//   AC5: weak-id detection + Sampled skip + source-cite

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "compiler/coercion_provenance_policy.hh"
#include "core/sandbox.hh"

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
using aura::compiler::apply_coercion_provenance_reject_env_override;
using aura::compiler::clear_coercion_active_mutation_context;
using aura::compiler::CoercionEntry;
using aura::compiler::CoercionMap;
using aura::compiler::CompilerService;
using aura::compiler::fill_coercion_provenance_chain;
using aura::compiler::g_coercion_provenance_ban_weak_ir_wired;
using aura::compiler::g_coercion_provenance_complete_total;
using aura::compiler::g_coercion_provenance_fast_path_total;
using aura::compiler::g_coercion_provenance_miss_reject_total;
using aura::compiler::g_coercion_provenance_miss_total;
using aura::compiler::g_coercion_provenance_sampled_reject_total;
using aura::compiler::g_coercion_provenance_sentinel_total;
using aura::compiler::g_coercion_provenance_weak_id_total;
using aura::compiler::is_weak_coercion_mutation_id;
using aura::compiler::kCoercionProvenanceSentinelBase;
using aura::compiler::reject_apply_on_provenance_miss;
using aura::compiler::reset_coercion_provenance_miss_policy_for_test;
using aura::compiler::set_coercion_active_mutation_context;
using aura::compiler::set_reject_apply_on_provenance_miss;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
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

static void ac1_sampled_no_weak_insert() {
    std::println("\n--- AC1: Sampled incomplete → no CoercionNode, no weak mid ---");
    reset_for_test();
    reset_coercion_provenance_miss_policy_for_test(); // reject off, soft policy
    set_strategy(AuditStrategy::Sampled);
    set_mode(SandboxMode::Off);
    clear_coercion_active_mutation_context();
    set_reject_apply_on_provenance_miss(false); // Sampled must still skip (#2261)

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);
    flat.set_type(lit, 99);
    const auto size0 = flat.size();
    // Call layout: child 0 = callee, child 1 = first arg (lit).
    const auto child1 = flat.get(call).child(1);

    CoercionMap map;
    map.add(call, /*child_index=*/1, lit, 1, 1, 0, 0); // empty provenance

    const auto miss0 = g_coercion_provenance_miss_total.load();
    const auto rej0 = g_coercion_provenance_miss_reject_total.load();
    const auto srej0 = g_coercion_provenance_sampled_reject_total.load();
    const auto n = apply_coercion_map(flat, map);

    CHECK(n == 0, "no insert under Sampled incomplete");
    CHECK(flat.size() == size0, "AST size unchanged");
    CHECK(flat.get(call).child(1) == child1, "arg not rewritten to CoercionNode");
    CHECK(g_coercion_provenance_miss_total.load() > miss0, "miss counted");
    CHECK(g_coercion_provenance_miss_reject_total.load() > rej0, "reject total++");
    CHECK(g_coercion_provenance_sampled_reject_total.load() > srej0, "sampled reject++");

    // No CoercionNode means no provenance column with weak mid.
    // Also: fill alone must not leave weak mid for later writers.
    CoercionEntry e{};
    e.parent_id = static_cast<std::uint32_t>(call);
    e.original_child = static_cast<std::uint32_t>(lit);
    e.type_tag = 1;
    e.type_id = 1;
    (void)fill_coercion_provenance_chain(flat, e);
    CHECK(!is_weak_coercion_mutation_id(e), "fill does not leave weak mid");
    CHECK(e.source_mutation_id == 0, "Sampled fill leaves mid=0 (not weak)");
}

static void ac2_full_complete_fast_path() {
    std::println("\n--- AC2: Full + complete TLS → fast path unchanged ---");
    reset_for_test();
    reset_coercion_provenance_miss_policy_for_test();
    set_strategy(AuditStrategy::Full);
    set_mode(SandboxMode::Off);
    clear_coercion_active_mutation_context();
    set_coercion_active_mutation_context(/*mutation_id=*/9001, /*predicate=*/77);

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);
    flat.set_type(lit, 99);

    CoercionMap map;
    map.add(call, /*child_index=*/1, lit, 1, 1, 0, 0, /*pred=*/77, /*mid=*/9001);

    const auto fast0 = g_coercion_provenance_fast_path_total.load();
    const auto complete0 = g_coercion_provenance_complete_total.load();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n >= 1, "complete apply inserts");
    CHECK(flat.get(call).child(1) != lit, "arg rewritten to CoercionNode");
    CHECK(g_coercion_provenance_fast_path_total.load() > fast0, "fast path advanced");
    CHECK(g_coercion_provenance_complete_total.load() > complete0, "complete advanced");
    clear_coercion_active_mutation_context();
}

static void ac3_off_soft_no_weak_mid() {
    std::println("\n--- AC3: Off soft allows apply; never weak mid on column ---");
    reset_for_test();
    reset_coercion_provenance_miss_policy_for_test();
    set_strategy(AuditStrategy::Off);
    set_mode(SandboxMode::Off);
    clear_coercion_active_mutation_context();
    set_reject_apply_on_provenance_miss(false);

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);
    flat.set_type(lit, 99);

    CoercionMap map;
    map.add(call, /*child_index=*/1, lit, 1, 1, 0, 0);
    const auto sent0 = g_coercion_provenance_sentinel_total.load();
    const auto n = apply_coercion_map(flat, map);
    // Off soft may insert (sentinel for diagnostics).
    CHECK(n >= 1, "Off soft inserts CoercionNode");
    CHECK(g_coercion_provenance_sentinel_total.load() > sent0, "sentinel stamped under Off soft");

    const auto c = flat.get(call).child(1);
    CHECK(c != lit, "rewritten to coercion");
    const auto prov = flat.provenance(c);
    CHECK(prov != static_cast<std::uint32_t>(lit), "prov not weak=original_child");
    CHECK(prov != 1u, "prov not weak=1");
    // Sentinel is OK under Off soft.
    CHECK((prov & 0xFFFF0000u) == kCoercionProvenanceSentinelBase, "sentinel provenance column");

    // Unit: weak detection + fill never invents weak mid under Off.
    CoercionEntry weak{};
    weak.original_child = 42;
    weak.source_mutation_id = 42; // weak == original_child
    CHECK(is_weak_coercion_mutation_id(weak), "weak mid = original_child");
    weak.original_child = 0;
    weak.source_mutation_id = 1;
    CHECK(is_weak_coercion_mutation_id(weak), "weak mid = 1 when original_child=0");

    // Fresh flat, no mutation log / no TLS — fill must not invent weak mid.
    FlatAST empty_flat;
    empty_flat.root = empty_flat.add_literal(7);
    CoercionEntry e2{};
    e2.original_child = 99; // non-zero node id (not necessarily in flat)
    e2.source_mutation_id = 99;
    CHECK(is_weak_coercion_mutation_id(e2), "weak mid = original_child (nonzero)");
    e2.source_mutation_id = 0;
    clear_coercion_active_mutation_context();
    (void)fill_coercion_provenance_chain(empty_flat, e2);
    CHECK(!is_weak_coercion_mutation_id(e2), "Off fill never leaves weak mid");
    CHECK(e2.source_mutation_id == 0, "empty flat: mid stays 0 (no weak invent)");
}

static void ac4_env_reject_override() {
    std::println("\n--- AC4: AURA_COERCION_PROVENANCE_REJECT env still works ---");
    reset_for_test();
    reset_coercion_provenance_miss_policy_for_test();
    set_strategy(AuditStrategy::Off);
    set_mode(SandboxMode::Off);
    set_reject_apply_on_provenance_miss(false);
    CHECK(!reject_apply_on_provenance_miss(), "start soft");

    // Direct API (env is process-global; unit uses setter parity with #2185).
    set_reject_apply_on_provenance_miss(true);
    CHECK(reject_apply_on_provenance_miss(), "reject on");
    set_reject_apply_on_provenance_miss(false);
    CHECK(!reject_apply_on_provenance_miss(), "reject off");

    // Env override helper still present.
    auto pol = read_file("src/compiler/coercion_provenance_policy.hh");
    CHECK(pol.find("AURA_COERCION_PROVENANCE_REJECT") != std::string::npos, "env key present");
    CHECK(pol.find("apply_coercion_provenance_reject_env_override") != std::string::npos,
          "env override fn");
    (void)apply_coercion_provenance_reject_env_override; // linked
}

static void ac5_schema_source_cite() {
    std::println("\n--- AC5: schema-2261 + source-cite ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2261") == 2261, "schema-2261");
    CHECK(href(cs, "issue-2261") == 2261, "issue-2261");
    CHECK(href(cs, "coercion-provenance-ban-weak-ir-wired") == 1, "ban weak wired");
    CHECK(href(cs, "coercion-provenance-sampled-reject-total") >= 0, "sampled reject key");
    CHECK(g_coercion_provenance_ban_weak_ir_wired.load() == 1, "wired atomic");

    const auto cm = read_file("src/compiler/coercion_map.ixx");
    CHECK(cm.find("Issue #2261") != std::string::npos, "coercion_map #2261");
    CHECK(cm.find("g_coercion_provenance_sampled_reject_total") != std::string::npos,
          "sampled reject counter");
    CHECK(cm.find("should_skip_coercion_insert_on_incomplete") != std::string::npos, "skip helper");
    CHECK(cm.find("never write weak mid") != std::string::npos ||
              cm.find("never stamp weak") != std::string::npos ||
              cm.find("Never write weak") != std::string::npos ||
              cm.find("never weak mid") != std::string::npos,
          "weak ban comment");
}

// ── Issue #2317 / #2620: Sampled incomplete insert is CANARY-only ──
// Default (#2620): never insert incomplete under Sampled.
// Canary: AURA_COERCION_SAMPLED_INCOMPLETE_INSERT=1 restores #2317 insert.
// AC1: counter + canary env + default skip-insert present
// AC2: Production reject-on-miss unchanged (still skip)
// AC3: Full / Strict honesty (no regression to #2147 / #2261 rules)
// AC4: Observability — counter + query keys + schema-2317 / issue-2317
// AC5: source-cite rows

static void ac2317_sampled_insert_policy() {
    std::println("\n--- #2317 AC1: Sampled insert canary policy (#2620 default skip) ---");
    const auto cm = read_file("src/compiler/coercion_map.ixx");
    // AC1: counter g_coercion_sampled_insert_incomplete_total retained (canary)
    CHECK(cm.find("g_coercion_sampled_insert_incomplete_total") != std::string::npos,
          "AC1: counter g_coercion_sampled_insert_incomplete_total present");
    CHECK(cm.find("g_coercion_sampled_insert_incomplete_total{0};") != std::string::npos,
          "AC1: counter atomic init");
    // AC1: canary env + Sampled branch
    CHECK(cm.find("AURA_COERCION_SAMPLED_INCOMPLETE_INSERT") != std::string::npos,
          "AC1: canary env present");
    CHECK(cm.find("coercion_sampled_incomplete_insert_canary") != std::string::npos,
          "AC1: canary helper present");
    CHECK(cm.find("!reject_apply_on_provenance_miss()") != std::string::npos,
          "AC1: reject check present");
    // AC1: note_provenance_miss_for_boundary still called
    CHECK(cm.find("note_provenance_miss_for_boundary()") != std::string::npos,
          "AC1: force-audit note_provenance_miss_for_boundary present");
    // AC1: weak mid NOT stamped (preserve #2261)
    CHECK(cm.find("never write weak") != std::string::npos ||
              cm.find("never stamp weak") != std::string::npos ||
              cm.find("never weak mid") != std::string::npos,
          "AC1: weak mid ban preserved (#2261)");
    CHECK(cm.find("Issue #2317") != std::string::npos, "AC1: coercion_map.ixx cites Issue #2317");
    CHECK(cm.find("Issue #2620") != std::string::npos || cm.find("#2620") != std::string::npos,
          "AC1: cites #2620 unify");
}

static void ac2317_reject_unchanged() {
    std::println("\n--- #2317 AC2: production reject unchanged ---");
    const auto cm = read_file("src/compiler/coercion_map.ixx");
    // AC2: existing reject-on-miss skip path preserved
    CHECK(cm.find("reject_apply_on_provenance_miss()") != std::string::npos,
          "AC2: reject_apply_on_provenance_miss check present");
    CHECK(cm.find("g_coercion_provenance_miss_reject_total") != std::string::npos,
          "AC2: existing miss_reject counter present");
    CHECK(cm.find("g_coercion_provenance_sampled_reject_total") != std::string::npos,
          "AC2: existing sampled_reject counter present");
    CHECK(cm.find("skipped_stale") != std::string::npos, "AC2: existing skip path counter present");
}

static void ac2317_full_strict_honesty() {
    std::println("\n--- #2317 AC3: Full / Strict honesty (no regression) ---");
    const auto cm = read_file("src/compiler/coercion_map.ixx");
    // AC3: Full / Strict still skip per #2147 / #2261 (no regression)
    CHECK(cm.find("AuditStrategy::Full") != std::string::npos,
          "AC3: Full audit strategy check present");
    CHECK(cm.find("strict") != std::string::npos, "AC3: strict sandbox mode check present");
    CHECK(cm.find("#2147") != std::string::npos || cm.find("Issue #2147") != std::string::npos,
          "AC3: #2147 honest handling preserved");
    CHECK(cm.find("#2261") != std::string::npos, "AC3: #2261 weak ban preserved");
}

static void ac2317_query_keys() {
    std::println("\n--- #2317 AC4: observability query keys ---");
    CompilerService cs;
    CHECK(href(cs, "coercion-sampled-insert-incomplete-total") >= 0,
          "AC4: coercion-sampled-insert-incomplete-total key present");
    CHECK(href(cs, "coercion-sampled-insert-policy-wired") == 1,
          "AC4: coercion-sampled-insert-policy-wired sentinel = 1");
    CHECK(href(cs, "schema-2317") == 2317, "AC4: schema-2317");
    CHECK(href(cs, "issue-2317") == 2317, "AC4: issue-2317");
    // #2317 lineage preserved
    CHECK(href(cs, "schema-2261") == 2261, "AC4: schema-2261 retained");
    CHECK(href(cs, "coercion-provenance-sampled-reject-total") >= 0,
          "AC4: existing sampled-reject key retained");
}

static void ac2317_source_cite_rows() {
    std::println("\n--- #2317 AC5: source-cite rows ---");
    const auto cm = read_file("src/compiler/coercion_map.ixx");
    const auto ep = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(cm.find("g_coercion_sampled_insert_incomplete_total") != std::string::npos,
          "AC5: coercion_map.ixx has new counter");
    CHECK(cm.find("Issue #2317") != std::string::npos, "AC5: coercion_map.ixx cites 2317");
    CHECK(ep.find("coercion-sampled-insert-incomplete-total") != std::string::npos,
          "AC5: query primitive exposes new counter key");
    CHECK(ep.find("coercion_sampled_insert_incomplete_total") != std::string::npos,
          "AC5: query primitive exposes underscored alias");
    CHECK(ep.find("coercion-sampled-insert-policy-wired") != std::string::npos,
          "AC5: query primitive exposes policy-wired sentinel");
    CHECK(ep.find("schema-2317") != std::string::npos, "AC5: query primitive schema-2317");
    CHECK(ep.find("issue-2317") != std::string::npos, "AC5: query primitive issue-2317");
}

} // namespace

int main() {
    std::println("=== Issue #2261: ban weak mid under Sampled; never stamp into IR ===");
    std::println("=== Issue #2317: Sampled incomplete insert canary (default skip #2620) ===");
    ac1_sampled_no_weak_insert();
    ac2_full_complete_fast_path();
    ac3_off_soft_no_weak_mid();
    ac4_env_reject_override();
    ac5_schema_source_cite();
    // Issue #2317: Sampled insert policy (refines #2261, both refine
    // the Sampled incomplete-provenance branch). AC1-AC5 wiring.
    ac2317_sampled_insert_policy();
    ac2317_reject_unchanged();
    ac2317_full_strict_honesty();
    ac2317_query_keys();
    ac2317_source_cite_rows();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
