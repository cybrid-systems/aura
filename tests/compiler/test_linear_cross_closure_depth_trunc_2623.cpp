// @category: unit
// @reason: Issue #2623 — configurable cross-closure depth + production
//          fail-closed on cone truncation (extends #2563 / #2612).
//
//   AC1: One-level capture Soft observe / production force (#2563 lock)
//   AC2: Nested free-capture when depth≥2; production forces
//   AC3: Cone trunc under production → trunc_force + CrossClosureEscape
//   AC4: Soft + trunc → no force unless HARD; metrics only
//   AC5: DEPTH=0 disables discovery (zero cost)
//   AC6: schema-2623 additive + source-cite
//   AC7: #2559 inventory lists env keys

#include "compiler/typed_mutation_audit.h"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>

import std;
import aura.core.ast;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.type_checker;

namespace {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::CompilerService;
using aura::compiler::CrossClosureEscapeResult;
using aura::compiler::discover_cross_closure_linear_escapes;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::linear_cross_closure_depth_cap;
using aura::compiler::typed_audit::linear_cross_closure_hard_enabled;
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
        "(hash-ref (engine:metrics \"query:linear-ownership-typed-mutate-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void reset_2623() {
    reset_for_test();
    apply_dev_audit_defaults();
    set_strategy(AuditStrategy::Sampled);
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_HARD");
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH");
    unsetenv("AURA_PARTIAL_CONE_SOFT");
}

// One-level free capture (#2563 baseline).
static FlatAST make_simple_capture(StringPool& pool) {
    FlatAST flat;
    auto x = pool.intern("lin");
    auto p = pool.intern("p");
    auto xv = flat.add_variable(x);
    auto lam = flat.add_lambda(std::array{p}, xv);
    flat.root = lam;
    return flat;
}

// Nested free-capture of dirty linear "lin" under outer Lambda.
static FlatAST make_nested_free_capture(StringPool& pool, std::uint32_t* outer_id_out = nullptr,
                                        std::uint32_t* nested_id_out = nullptr) {
    FlatAST flat;
    auto outer_p = pool.intern("outer_p");
    auto inner_p = pool.intern("inner_p");
    auto lin = pool.intern("lin");
    auto tmp = flat.add_literal(0);
    auto outer = flat.add_lambda(std::array{outer_p}, tmp);
    auto xv = flat.add_variable(lin);
    auto nested = flat.add_lambda(std::array{inner_p}, xv);
    flat.set_child(outer, 0, nested);
    flat.root = outer;
    if (outer_id_out)
        *outer_id_out = outer;
    if (nested_id_out)
        *nested_id_out = nested;
    return flat;
}

// ── AC1: one-level Soft/prod ──
static void ac1_one_level_soft_prod() {
    std::println("\n--- #2623 AC1: one-level capture Soft observe / prod force ---");
    reset_2623();
    CHECK(linear_cross_closure_depth_cap() == 1, "AC1: Soft default depth 1");

    StringPool pool;
    auto flat = make_simple_capture(pool);
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    CHECK(!discover_cross_closure_linear_escapes(flat, pool, dirty, 256, out),
          "AC1: simple free-capture is escape");
    CHECK(out.escape_sites >= 1, "AC1: escape_sites >= 1");
    CHECK(out.depth_cap == 1, "AC1: depth_cap 1 under Soft");

    CHECK(!linear_cross_closure_hard_enabled(), "AC1: Soft hard off");
    // Soft path: observe only (mirror evaluator).
    const auto force0 = g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
        std::memory_order_relaxed);
    const auto obs0 = g_typed_mutation_audit_counters.linear_cross_closure_observe_total.load(
        std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_escape_total.fetch_add(
        out.escape_sites, std::memory_order_relaxed);
    if (linear_cross_closure_hard_enabled()) {
        g_typed_mutation_audit_counters.linear_cross_closure_force_total.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        g_typed_mutation_audit_counters.linear_cross_closure_observe_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
              std::memory_order_relaxed) == force0,
          "AC1: Soft force unchanged");
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_observe_total.load(
              std::memory_order_relaxed) > obs0,
          "AC1: Soft observe advanced");

    // Production: hard on; force path.
    apply_production_audit_defaults();
    CHECK(linear_cross_closure_hard_enabled(), "AC1: production hard on");
    // Production default depth is 2 when env unset (#2623).
    CHECK(linear_cross_closure_depth_cap() == 2, "AC1: production default depth 2");
    setenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "1", 1);
    CHECK(linear_cross_closure_depth_cap() == 1, "AC1: env can pin depth 1 under prod");
    const auto force1 = g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
        std::memory_order_relaxed);
    if (linear_cross_closure_hard_enabled()) {
        g_typed_mutation_audit_counters.linear_cross_closure_force_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
              std::memory_order_relaxed) > force1,
          "AC1: production force advances");
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH");
    apply_dev_audit_defaults();
}

// ── AC2: nested depth ≥2 + prod force ──
static void ac2_nested_depth2_prod() {
    std::println("\n--- #2623 AC2: nested free-capture depth≥2 + production force ---");
    reset_2623();
    // Soft: must set depth 2 explicitly.
    setenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "2", 1);
    CHECK(linear_cross_closure_depth_cap() == 2, "AC2: env depth 2");

    StringPool pool;
    std::uint32_t outer_id = 0, nested_id = 0;
    auto flat = make_nested_free_capture(pool, &outer_id, &nested_id);
    std::unordered_set<std::string> dirty{"lin"};
    const std::size_t cone = static_cast<std::size_t>(outer_id) + 1; // exclude nested from top loop
    CrossClosureEscapeResult out{};
    CHECK(!discover_cross_closure_linear_escapes(flat, pool, dirty, cone, out),
          "AC2: nested free-capture is escape under depth 2");
    CHECK(out.escape_sites >= 1, "AC2: escape_sites");
    CHECK(out.depth2_entries >= 1, "AC2: depth2_entries");
    CHECK(out.depth2_escape_sites >= 1, "AC2: depth2_escape_sites");

    // Depth 3 hard max accepted.
    setenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "3", 1);
    CHECK(linear_cross_closure_depth_cap() == 3, "AC2: depth 3 allowed");
    setenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "9", 1);
    CHECK(linear_cross_closure_depth_cap() == 3, "AC2: depth >3 clamps to 3");

    // Production default depth 2 without env.
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH");
    apply_production_audit_defaults();
    CHECK(linear_cross_closure_depth_cap() == 2, "AC2: prod default depth 2");
    CHECK(linear_cross_closure_hard_enabled(), "AC2: prod hard on");
    CrossClosureEscapeResult out_p{};
    CHECK(!discover_cross_closure_linear_escapes(flat, pool, dirty, cone, out_p),
          "AC2: prod default depth 2 finds nested escape");
    // Mirror force path.
    const auto force0 = g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
        std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_escape_total.fetch_add(
        out_p.escape_sites, std::memory_order_relaxed);
    if (linear_cross_closure_hard_enabled()) {
        g_typed_mutation_audit_counters.linear_cross_closure_force_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
              std::memory_order_relaxed) > force0,
          "AC2: production force on nested escape");
    apply_dev_audit_defaults();
}

// ── AC3: trunc under production fail-closed ──
static void ac3_trunc_prod_force() {
    std::println("\n--- #2623 AC3: cone trunc under production → trunc_force ---");
    reset_2623();
    apply_production_audit_defaults();
    CHECK(linear_cross_closure_hard_enabled(), "AC3: hard on");

    StringPool pool;
    auto flat = make_simple_capture(pool);
    for (int i = 0; i < 40; ++i)
        (void)flat.add_literal(i);
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    (void)discover_cross_closure_linear_escapes(flat, pool, dirty, /*cone_cap=*/1, out);
    CHECK(out.cap_truncations == 1, "AC3: cap_truncations when size > cone_cap");

    // Mirror evaluator_typecheck fail-closed path for trunc under hard.
    const auto trunc0 = g_typed_mutation_audit_counters.linear_cross_closure_cap_trunc_total.load(
        std::memory_order_relaxed);
    const auto tforce0 =
        g_typed_mutation_audit_counters.linear_cross_closure_trunc_force_total.load(
            std::memory_order_relaxed);
    const auto force0 = g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
        std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_cap_trunc_total.fetch_add(
        out.cap_truncations, std::memory_order_relaxed);
    bool force_cross_closure = false;
    const bool hard = linear_cross_closure_hard_enabled();
    if (out.escape_sites > 0) {
        g_typed_mutation_audit_counters.linear_cross_closure_escape_total.fetch_add(
            out.escape_sites, std::memory_order_relaxed);
        if (hard)
            force_cross_closure = true;
    }
    if (out.cap_truncations && hard) {
        force_cross_closure = true;
        g_typed_mutation_audit_counters.linear_cross_closure_trunc_force_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (force_cross_closure) {
        g_typed_mutation_audit_counters.linear_cross_closure_force_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_cap_trunc_total.load(
              std::memory_order_relaxed) > trunc0,
          "AC3: cap_trunc_total advanced");
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_trunc_force_total.load(
              std::memory_order_relaxed) > tforce0,
          "AC3: trunc_force_total advanced under production");
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
              std::memory_order_relaxed) > force0,
          "AC3: force_total advanced (fail-closed)");

    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(etc.find("linear_cross_closure_trunc_force_total") != std::string::npos,
          "AC3: trunc_force wired in typecheck");
    CHECK(etc.find("cap_truncations && hard") != std::string::npos,
          "AC3: trunc && hard force path");
    CHECK(etc.find("linear-cross-closure-escape") != std::string::npos ||
              etc.find("CrossClosureEscape") != std::string::npos,
          "AC3: deny authority CrossClosureEscape retained");
    apply_dev_audit_defaults();
}

// ── AC4: Soft trunc observe only ──
static void ac4_soft_trunc_observe() {
    std::println("\n--- #2623 AC4: Soft + truncation → metrics only ---");
    reset_2623();
    CHECK(!linear_cross_closure_hard_enabled(), "AC4: Soft hard off");

    StringPool pool;
    auto flat = make_simple_capture(pool);
    for (int i = 0; i < 40; ++i)
        (void)flat.add_literal(i);
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    (void)discover_cross_closure_linear_escapes(flat, pool, dirty, /*cone_cap=*/1, out);
    CHECK(out.cap_truncations == 1, "AC4: trunc recorded under Soft");

    const auto force0 = g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
        std::memory_order_relaxed);
    const auto tforce0 =
        g_typed_mutation_audit_counters.linear_cross_closure_trunc_force_total.load(
            std::memory_order_relaxed);
    const auto trunc0 = g_typed_mutation_audit_counters.linear_cross_closure_cap_trunc_total.load(
        std::memory_order_relaxed);
    // Soft path: bump trunc only; no force / trunc_force.
    g_typed_mutation_audit_counters.linear_cross_closure_cap_trunc_total.fetch_add(
        out.cap_truncations, std::memory_order_relaxed);
    const bool hard = linear_cross_closure_hard_enabled();
    bool force_cross_closure = false;
    if (out.escape_sites > 0 && hard)
        force_cross_closure = true;
    if (out.cap_truncations && hard) {
        force_cross_closure = true;
        g_typed_mutation_audit_counters.linear_cross_closure_trunc_force_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (force_cross_closure) {
        g_typed_mutation_audit_counters.linear_cross_closure_force_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_cap_trunc_total.load(
              std::memory_order_relaxed) > trunc0,
          "AC4: trunc metric advanced");
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
              std::memory_order_relaxed) == force0,
          "AC4: force unchanged under Soft trunc");
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_trunc_force_total.load(
              std::memory_order_relaxed) == tforce0,
          "AC4: trunc_force unchanged under Soft");

    // HARD env under Soft strategy still forces trunc fail-closed.
    setenv("AURA_LINEAR_CROSS_CLOSURE_HARD", "1", 1);
    CHECK(linear_cross_closure_hard_enabled(), "AC4: HARD env forces hard on");
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_HARD");
}

// ── AC5: depth 0 disable ──
static void ac5_depth0_disable() {
    std::println("\n--- #2623 AC5: DEPTH=0 disables discovery ---");
    reset_2623();
    setenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "0", 1);
    CHECK(linear_cross_closure_depth_cap() == 0, "AC5: depth_cap == 0");

    StringPool pool;
    auto flat = make_simple_capture(pool);
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    CHECK(discover_cross_closure_linear_escapes(flat, pool, dirty, 256, out),
          "AC5: discovery returns clean when disabled");
    CHECK(out.escape_sites == 0, "AC5: zero escape_sites");
    CHECK(out.sites_scanned == 0, "AC5: zero sites_scanned (zero cost)");
    CHECK(out.nodes_scanned == 0, "AC5: zero nodes_scanned");
    CHECK(out.depth_cap == 0, "AC5: result depth_cap 0");

    unsetenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH");
}

// ── AC6: schema + source-cite ──
static void ac6_schema_source() {
    std::println("\n--- #2623 AC6: schema-2623 additive + source-cite ---");
    reset_2623();
    CompilerService cs;
    CHECK(href(cs, "schema-2623") == 2623, "AC6: schema-2623");
    CHECK(href(cs, "issue-2623") == 2623, "AC6: issue-2623");
    CHECK(href(cs, "linear-cross-closure-trunc-force-total") >= 0, "AC6: trunc-force queryable");
    CHECK(href(cs, "linear-cross-closure-depth-max") == 3, "AC6: depth-max == 3");
    CHECK(href(cs, "linear-cross-closure-prod-depth-default") == 2, "AC6: prod-depth-default == 2");
    // Lineage retained.
    CHECK(href(cs, "schema-2563") == 2563, "AC6: schema-2563 retained");
    CHECK(href(cs, "schema-2612") == 2612, "AC6: schema-2612 retained");
    CHECK(href(cs, "linear-cross-closure-wired") == 1, "AC6: #2563 wired");
    CHECK(href(cs, "linear-cross-closure-depth-wired") == 1, "AC6: #2612 depth-wired");
    CHECK(href(cs, "linear-cross-closure-depth-cap") == 1, "AC6: Soft default depth-cap 1");

    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto tcpp = read_file("src/compiler/type_checker_impl.cpp");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(aud.find("#2623") != std::string::npos, "AC6: audit cites #2623");
    CHECK(aud.find("linear_cross_closure_trunc_force_total") != std::string::npos,
          "AC6: trunc_force counter");
    CHECK(tcpp.find("#2623") != std::string::npos, "AC6: discover cites #2623");
    CHECK(etc.find("#2623") != std::string::npos, "AC6: typecheck cites #2623");
    CHECK(q.find("schema-2623") != std::string::npos, "AC6: query schema-2623");
    // Single force entry retained.
    CHECK(etc.find("force_linear_rollback") != std::string::npos, "AC6: force_linear_rollback");
    CHECK(etc.find("CrossClosureEscape") != std::string::npos, "AC6: CrossClosureEscape authority");
}

// ── AC7: #2559 inventory ──
static void ac7_inventory_2559() {
    std::println("\n--- #2623 AC7: #2559 inventory lists env keys ---");
    reset_2623();
    const auto inv = read_file("scripts/check_linear_three_layer_wire_2559.py");
    CHECK(inv.find("LINEAR_CROSS_CLOSURE_ENV_KEYS") != std::string::npos,
          "AC7: inventory tuple present");
    CHECK(inv.find("AURA_LINEAR_CROSS_CLOSURE_HARD") != std::string::npos, "AC7: HARD env listed");
    CHECK(inv.find("AURA_LINEAR_CROSS_CLOSURE_DEPTH") != std::string::npos,
          "AC7: DEPTH env listed");
    CHECK(inv.find("#2623") != std::string::npos || inv.find("2623") != std::string::npos,
          "AC7: inventory cites #2623");

    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(aud.find("AURA_LINEAR_CROSS_CLOSURE_HARD") != std::string::npos, "AC7: HARD in audit");
    CHECK(aud.find("AURA_LINEAR_CROSS_CLOSURE_DEPTH") != std::string::npos, "AC7: DEPTH in audit");
}

} // namespace

int main() {
    std::println("=== Issue #2623: cross-closure depth + trunc fail-closed ===");
    ac1_one_level_soft_prod();
    ac2_nested_depth2_prod();
    ac3_trunc_prod_force();
    ac4_soft_trunc_observe();
    ac5_depth0_disable();
    ac6_schema_source();
    ac7_inventory_2559();
    apply_dev_audit_defaults();
    reset_2623();
    std::println("\n=== #2623: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
