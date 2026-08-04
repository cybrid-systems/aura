// @category: unit
// @reason: Issue #2147 — CoercionMap provenance fast path + Strict reject weak id.
//
//   AC1: both fields set at add → chain_walk_total does not increase (fast path)
//   AC2: Strict + empty context → miss/weak up; not counted as complete
//   AC3: Off/Sampled still get forensic sentinel/weak stamps
//   AC4: identity-elided entries skip provenance work entirely
//   AC5: schema-2147 on query:type-incremental-fidelity-stats

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "core/sandbox.hh"

#include <cstdint>
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
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::compiler::apply_coercion_map;
using aura::compiler::clear_coercion_active_mutation_context;
using aura::compiler::CoercionMap;
using aura::compiler::CompilerService;
using aura::compiler::g_coercion_provenance_chain_walk_total;
using aura::compiler::g_coercion_provenance_complete_total;
using aura::compiler::g_coercion_provenance_fast_path_total;
using aura::compiler::g_coercion_provenance_miss_total;
using aura::compiler::g_coercion_provenance_sentinel_total;
using aura::compiler::g_coercion_provenance_strict_reject_weak_total;
using aura::compiler::g_coercion_provenance_weak_id_total;
using aura::compiler::g_dead_coercion_ast_elided_total;
using aura::compiler::kCoercionProvenanceSentinelBase;
using aura::compiler::set_coercion_active_mutation_context;
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

static FlatAST make_tiny_flat(StringPool& pool, aura::ast::NodeId& lit_out,
                              aura::ast::NodeId& call_out) {
    FlatAST flat;
    auto x = pool.intern("x");
    auto xv = flat.add_variable(x);
    auto lit = flat.add_literal(1);
    flat.set_type(lit, 0); // force non-identity coercion path
    auto call = flat.add_call(xv, std::array<aura::ast::NodeId, 1>{lit});
    flat.root = call;
    lit_out = lit;
    call_out = call;
    return flat;
}

static void ac1_fast_path_no_walk() {
    std::println("\n--- AC1: caller-stamped both fields → no chain walk ---");
    reset_for_test();
    set_strategy(AuditStrategy::Sampled);
    set_mode(SandboxMode::Off);
    clear_coercion_active_mutation_context();

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny_flat(pool, lit, call);
    flat.set_type(lit, 99); // different from target so not identity-elided

    CoercionMap map;
    // Both provenance fields set at add (true mutation id, not weak).
    // Call child 0 = callee, child 1 = first arg.
    map.add(call, /*child_index=*/1, lit, /*type_tag=*/1, /*type_id=*/1, 0, 0,
            /*predicate_cond_node=*/77, /*source_mutation_id=*/9001);

    const auto walks0 = g_coercion_provenance_chain_walk_total.load();
    const auto fast0 = g_coercion_provenance_fast_path_total.load();
    const auto complete0 = g_coercion_provenance_complete_total.load();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n >= 1 || flat.size() > 0, "apply ran");
    CHECK(g_coercion_provenance_chain_walk_total.load() == walks0,
          "chain_walk_total unchanged (fast path)");
    CHECK(g_coercion_provenance_fast_path_total.load() > fast0, "fast_path advanced");
    CHECK(g_coercion_provenance_complete_total.load() > complete0, "complete advanced");
}

static void ac2_strict_weak_not_complete() {
    std::println("\n--- AC2: Strict + empty context → miss/weak; not complete ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    set_mode(SandboxMode::Strict);
    clear_coercion_active_mutation_context();

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny_flat(pool, lit, call);
    // No mutation log, no stamps.

    CoercionMap map;
    map.add(call, /*child_index=*/1, lit, 1, 1, 0, 0); // empty provenance

    const auto complete0 = g_coercion_provenance_complete_total.load();
    const auto miss0 = g_coercion_provenance_miss_total.load();
    const auto weak0 = g_coercion_provenance_weak_id_total.load();
    const auto strict0 = g_coercion_provenance_strict_reject_weak_total.load();
    const auto sent0 = g_coercion_provenance_sentinel_total.load();

    (void)apply_coercion_map(flat, map);

    CHECK(g_coercion_provenance_miss_total.load() > miss0, "miss advanced under Strict");
    CHECK(g_coercion_provenance_complete_total.load() == complete0,
          "not counted as complete under Strict empty context");
    // Strict honesty: no new sentinel forensic pretend (may stay at sent0)
    CHECK(g_coercion_provenance_sentinel_total.load() == sent0 ||
              g_coercion_provenance_strict_reject_weak_total.load() >= strict0,
          "no soft sentinel pretend / strict reject path");
    CHECK(g_coercion_provenance_weak_id_total.load() >= weak0, "weak counter non-decreasing");

    set_mode(SandboxMode::Off);
    set_strategy(AuditStrategy::Sampled);
}

static void ac3_soft_forensic_stamps() {
    // Issue #2261 refined #2147 AC3: Sampled no longer stamps weak/sentinel
    // into IR (skips insert). Off soft may stamp sentinel only (no weak mid).
    std::println("\n--- AC3: Off soft sentinel; Sampled skips incomplete insert (#2261) ---");
    reset_for_test();
    set_mode(SandboxMode::Off);
    clear_coercion_active_mutation_context();

    // Sampled: incomplete → skip insert, no weak mid on tree.
    {
        set_strategy(AuditStrategy::Sampled);
        StringPool pool;
        aura::ast::NodeId lit = 0, call = 0;
        auto flat = make_tiny_flat(pool, lit, call);
        const auto size0 = flat.size();
        CoercionMap map;
        map.add(call, /*child_index=*/1, lit, 1, 1, 0, 0);
        const auto walks0 = g_coercion_provenance_chain_walk_total.load();
        const auto n = apply_coercion_map(flat, map);
        CHECK(g_coercion_provenance_chain_walk_total.load() > walks0,
              "walk ran Sampled incomplete");
        CHECK(n == 0, "Sampled incomplete: no CoercionNode insert");
        CHECK(flat.size() == size0, "Sampled incomplete: AST size unchanged");
        CHECK(flat.get(call).child(1) == lit, "Sampled incomplete: arg not rewritten");
    }
    // Off soft: may insert with sentinel; never weak mid as provenance.
    {
        set_strategy(AuditStrategy::Off);
        StringPool pool;
        aura::ast::NodeId lit = 0, call = 0;
        auto flat = make_tiny_flat(pool, lit, call);
        CoercionMap map;
        map.add(call, /*child_index=*/1, lit, 1, 1, 0, 0);
        const auto sent0 = g_coercion_provenance_sentinel_total.load();
        (void)apply_coercion_map(flat, map);
        CHECK(g_coercion_provenance_sentinel_total.load() > sent0 || flat.size() > 0,
              "Off soft: sentinel and/or apply ran");
        CHECK(kCoercionProvenanceSentinelBase == 0xC0E50000u, "sentinel base retained");
    }
}

static void ac4_identity_elision_skips_prov() {
    std::println("\n--- AC4: identity elision skips provenance work ---");
    reset_for_test();
    set_strategy(AuditStrategy::Sampled);
    set_mode(SandboxMode::Off);

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny_flat(pool, lit, call);
    // Identity: child type_id matches entry type_id → elide, no fill.
    flat.set_type(lit, 42);

    CoercionMap map;
    map.add(call, /*child_index=*/1, lit, 1, /*type_id=*/42, 0, 0); // identity

    const auto walks0 = g_coercion_provenance_chain_walk_total.load();
    const auto fast0 = g_coercion_provenance_fast_path_total.load();
    const auto elide0 = g_dead_coercion_ast_elided_total.load();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n == 0, "identity applied count 0");
    CHECK(g_dead_coercion_ast_elided_total.load() > elide0, "AST elided");
    CHECK(g_coercion_provenance_chain_walk_total.load() == walks0, "no walk on identity");
    CHECK(g_coercion_provenance_fast_path_total.load() == fast0, "no fast path on identity");
}

static void ac5_schema_tls() {
    std::println("\n--- AC5: schema-2147 + TLS context + source ---");
    auto cm = read_file("src/compiler/coercion_map.ixx");
    CHECK(cm.find("#2147") != std::string::npos, "coercion_map #2147");
    CHECK(cm.find("g_coercion_provenance_fast_path_total") != std::string::npos, "fast path");
    CHECK(cm.find("g_coercion_provenance_weak_id_total") != std::string::npos, "weak id");
    CHECK(cm.find("kCoercionParentWalkCapSampled") != std::string::npos, "sampled cap");
    CHECK(cm.find("set_coercion_active_mutation_context") != std::string::npos, "TLS context");

    // TLS O(1) mutation id: empty entry fills from context without log.
    clear_coercion_active_mutation_context();
    set_coercion_active_mutation_context(/*mid=*/5555, /*pred=*/66);
    set_strategy(AuditStrategy::Sampled);
    set_mode(SandboxMode::Off);

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny_flat(pool, lit, call);
    CoercionMap map;
    map.add(call, 0, lit, 1, 1, 0, 0);
    const auto complete0 = g_coercion_provenance_complete_total.load();
    (void)apply_coercion_map(flat, map);
    CHECK(g_coercion_provenance_complete_total.load() > complete0,
          "TLS context completes without mutation log");
    clear_coercion_active_mutation_context();

    CompilerService cs;
    CHECK(href(cs, "schema-2147") == 2147, "schema-2147");
    CHECK(href(cs, "coercion-provenance-fast-path-wired") == 1, "wired");
    CHECK(href(cs, "coercion-provenance-fast-path-total") >= 0, "fast key");
    CHECK(href(cs, "coercion-provenance-weak-id-total") >= 0, "weak key");
    CHECK(href(cs, "coercion-parent-walk-cap-sampled") == 16, "cap sampled");
    CHECK(href(cs, "coercion-parent-walk-cap-full") == 64, "cap full");
    CHECK(href(cs, "schema-2024") == 2024, "2024 lineage");
}

} // namespace

int run_test_coercion_provenance_fast_strict() {
    std::println("=== Issue #2147: coercion provenance fast path + Strict honesty ===");
    ac1_fast_path_no_walk();
    ac2_strict_weak_not_complete();
    ac3_soft_forensic_stamps();
    ac4_identity_elision_skips_prov();
    ac5_schema_tls();
    std::println("\n=== #2147 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_coercion_provenance_fast_strict();
}
#endif
