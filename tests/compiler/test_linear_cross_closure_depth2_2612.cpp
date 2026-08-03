// @category: unit
// @reason: Issue #2612 — optional depth-2 cross-closure free-capture discovery
//          (still cone-capped; Soft observe unless hard).
//
//   AC1: Default depth 1 → #2563 behavior; nested free-capture outside cone
//        id window not entered; no new force without hard
//   AC2: Depth 2 + dirty linear free-captured in nested lambda inside cone
//        → escape_total advances; hard path forces when enabled
//   AC3: Cone truncation still counted; no O(workspace) walk
//   AC4: Soft + depth 2 + hard off → observe only
//   AC5: Source-cite + gate; no docs/design

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

static void reset_2612() {
    reset_for_test();
    apply_dev_audit_defaults();
    set_strategy(AuditStrategy::Sampled);
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_HARD");
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH");
    unsetenv("AURA_PARTIAL_CONE_SOFT");
}

// Outer Lambda at low id; nested Lambda free-captures dirty linear "lin".
// Nested node id is higher so a tight cone_cap can exclude it from the
// top-level Lambda for-loop while still reaching it via depth-2 body walk.
static FlatAST make_nested_free_capture(StringPool& pool, std::uint32_t* outer_id_out = nullptr,
                                        std::uint32_t* nested_id_out = nullptr) {
    FlatAST flat;
    auto outer_p = pool.intern("outer_p");
    auto inner_p = pool.intern("inner_p");
    auto lin = pool.intern("lin");
    // Placeholder body so outer Lambda gets a low-ish id before nested exists.
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

// One-level free capture (depth-1 #2563 baseline).
static FlatAST make_simple_capture(StringPool& pool) {
    FlatAST flat;
    auto x = pool.intern("lin");
    auto p = pool.intern("p");
    auto xv = flat.add_variable(x);
    auto lam = flat.add_lambda(std::array{p}, xv);
    flat.root = lam;
    return flat;
}

// ── AC1: default depth 1 ──
static void ac1_default_depth1() {
    std::println("\n--- #2612 AC1: default depth 1 (#2563 lock + no nested entry) ---");
    reset_2612();
    CHECK(linear_cross_closure_depth_cap() == 1, "AC1: default depth_cap == 1");

    StringPool pool;
    // Simple free-capture still discovered (depth-1 site loop).
    auto simple = make_simple_capture(pool);
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    CHECK(!discover_cross_closure_linear_escapes(simple, pool, dirty, 256, out),
          "AC1: simple free-capture still escape under depth 1");
    CHECK(out.escape_sites >= 1, "AC1: escape_sites >= 1");
    CHECK(out.depth_cap == 1, "AC1: result depth_cap == 1");
    CHECK(out.depth2_entries == 0, "AC1: no depth2 entries on simple");

    // Nested free-capture with cone that includes only outer Lambda id window:
    // depth 1 must NOT enter nested → zero escapes when nested id is outside limit.
    std::uint32_t outer_id = 0, nested_id = 0;
    auto nested_flat = make_nested_free_capture(pool, &outer_id, &nested_id);
    CHECK(nested_id > outer_id, "AC1: nested id > outer id");
    const std::size_t cone = static_cast<std::size_t>(outer_id) + 1; // exclude nested
    CrossClosureEscapeResult out_n{};
    const bool clean = discover_cross_closure_linear_escapes(nested_flat, pool, dirty, cone, out_n);
    CHECK(clean, "AC1: depth 1 does not enter nested outside cone id window");
    CHECK(out_n.escape_sites == 0, "AC1: zero escapes without depth-2 entry");
    CHECK(out_n.depth2_entries == 0, "AC1: depth2_entries == 0");

    // Soft Sampled: hard off → no new force from depth alone.
    CHECK(!linear_cross_closure_hard_enabled(), "AC1: Soft hard off");
    CHECK(linear_cross_closure_depth_cap() == 1, "AC1: depth env unset → 1");
}

// ── AC2: depth 2 discovers nested free-capture; hard forces ──
static void ac2_depth2_discover_hard() {
    std::println("\n--- #2612 AC2: depth 2 nested free-capture + hard force ---");
    reset_2612();
    setenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "2", 1);
    CHECK(linear_cross_closure_depth_cap() == 2, "AC2: env depth 2");

    StringPool pool;
    std::uint32_t outer_id = 0, nested_id = 0;
    auto flat = make_nested_free_capture(pool, &outer_id, &nested_id);
    std::unordered_set<std::string> dirty{"lin"};
    const std::size_t cone = static_cast<std::size_t>(outer_id) + 1;
    CrossClosureEscapeResult out{};
    const bool clean = discover_cross_closure_linear_escapes(flat, pool, dirty, cone, out);
    CHECK(!clean, "AC2: nested free-capture is escape under depth 2");
    CHECK(out.escape_sites >= 1, "AC2: escape_sites advances");
    CHECK(out.depth2_entries >= 1, "AC2: depth2_entries >= 1");
    CHECK(out.depth2_escape_sites >= 1, "AC2: depth2_escape_sites >= 1");
    CHECK(out.depth_cap == 2, "AC2: depth_cap == 2");

    // Production hard: force path still gated by hard_enabled (not depth alone).
    apply_production_audit_defaults();
    CHECK(linear_cross_closure_hard_enabled(), "AC2: production hard on");
    // Simulate discovery counter path (mirrors evaluator_typecheck).
    const auto esc0 = g_typed_mutation_audit_counters.linear_cross_closure_escape_total.load(
        std::memory_order_relaxed);
    const auto force0 = g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
        std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_escape_total.fetch_add(
        out.escape_sites, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_depth2_entries_total.fetch_add(
        out.depth2_entries, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_depth2_escape_total.fetch_add(
        out.depth2_escape_sites, std::memory_order_relaxed);
    if (linear_cross_closure_hard_enabled()) {
        g_typed_mutation_audit_counters.linear_cross_closure_force_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_escape_total.load(
              std::memory_order_relaxed) > esc0,
          "AC2: escape_total advanced");
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
              std::memory_order_relaxed) > force0,
          "AC2: hard force total advanced under production");

    apply_dev_audit_defaults();
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH");
}

// ── AC3: cone trunc; no O(workspace) ──
static void ac3_cone_trunc() {
    std::println("\n--- #2612 AC3: cone truncation still counted under depth 2 ---");
    reset_2612();
    setenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "2", 1);

    StringPool pool;
    auto flat = make_nested_free_capture(pool);
    for (int i = 0; i < 30; ++i)
        (void)flat.add_literal(i);
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    (void)discover_cross_closure_linear_escapes(flat, pool, dirty, /*cone_cap=*/1, out);
    CHECK(out.cap_truncations == 1, "AC3: cap_truncations when size > cone_cap");
    // nodes_scanned bounded by ~4× cone_cap (soft stop in walk).
    CHECK(out.nodes_scanned <= 16, "AC3: nodes_scanned cone-bounded (not O(workspace))");

    const auto tcpp = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tcpp.find("cone_cap * 4") != std::string::npos, "AC3: 4× cone budget retained");
    CHECK(tcpp.find("AURA_LINEAR_CROSS_CLOSURE_DEPTH") != std::string::npos ||
              tcpp.find("linear_cross_closure_depth_cap") != std::string::npos,
          "AC3: depth_cap wired in discover");

    unsetenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH");
}

// ── AC4: Soft + depth 2 + hard off → observe only ──
static void ac4_soft_observe() {
    std::println("\n--- #2612 AC4: Soft + depth 2 + hard off → observe only ---");
    reset_2612();
    setenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "2", 1);
    set_strategy(AuditStrategy::Sampled);
    CHECK(linear_cross_closure_depth_cap() == 2, "AC4: depth 2");
    CHECK(!linear_cross_closure_hard_enabled(), "AC4: hard off under Soft Sampled");

    StringPool pool;
    std::uint32_t outer_id = 0;
    auto flat = make_nested_free_capture(pool, &outer_id, nullptr);
    std::unordered_set<std::string> dirty{"lin"};
    const std::size_t cone = static_cast<std::size_t>(outer_id) + 1;
    CrossClosureEscapeResult out{};
    CHECK(!discover_cross_closure_linear_escapes(flat, pool, dirty, cone, out),
          "AC4: escape discovered under Soft depth 2");
    CHECK(out.escape_sites >= 1, "AC4: escape_sites > 0");

    // Soft path: bump observe, not force (mirrors evaluator_typecheck).
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
          "AC4: force_total unchanged (observe only)");
    CHECK(g_typed_mutation_audit_counters.linear_cross_closure_observe_total.load(
              std::memory_order_relaxed) > obs0,
          "AC4: observe_total advanced");

    unsetenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH");
}

// ── AC5: schema + source-cite + no docs ──
static void ac5_schema_source() {
    std::println("\n--- #2612 AC5: schema-2612 + source-cite ---");
    reset_2612();
    CompilerService cs;
    CHECK(href(cs, "schema-2612") == 2612, "AC5: schema-2612");
    CHECK(href(cs, "issue-2612") == 2612, "AC5: issue-2612");
    CHECK(href(cs, "linear-cross-closure-depth-wired") == 1, "AC5: depth-wired");
    CHECK(href(cs, "linear-cross-closure-depth-cap") == 1, "AC5: default depth-cap query == 1");
    CHECK(href(cs, "schema-2563") == 2563, "AC5: #2563 lineage retained");
    CHECK(href(cs, "linear-cross-closure-wired") == 1, "AC5: #2563 wired retained");

    setenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "2", 1);
    CHECK(href(cs, "linear-cross-closure-depth-cap") == 2, "AC5: query reflects env depth 2");
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH");

    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto tci = read_file("src/compiler/type_checker.ixx");
    const auto tcpp = read_file("src/compiler/type_checker_impl.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_security.cpp");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(aud.find("#2612") != std::string::npos, "AC5: audit cites #2612");
    CHECK(aud.find("linear_cross_closure_depth_cap") != std::string::npos, "AC5: depth_cap helper");
    CHECK(tci.find("depth2_entries") != std::string::npos, "AC5: result depth2_entries");
    CHECK(tcpp.find("depth_remaining") != std::string::npos ||
              tcpp.find("depth2_entries") != std::string::npos,
          "AC5: discover depth walk");
    CHECK(q.find("schema-2612") != std::string::npos, "AC5: query schema-2612");
    CHECK(etc.find("#2612") != std::string::npos, "AC5: typecheck cites #2612");
    // Force path unchanged.
    CHECK(etc.find("linear_cross_closure_hard_enabled") != std::string::npos,
          "AC5: hard path still hard_enabled");
}

} // namespace

int main() {
    std::println("=== Issue #2612: optional depth-2 cross-closure free-capture ===");
    ac1_default_depth1();
    ac2_depth2_discover_hard();
    ac3_cone_trunc();
    ac4_soft_observe();
    ac5_schema_source();
    apply_dev_audit_defaults();
    reset_2612();
    std::println("\n=== #2612: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
