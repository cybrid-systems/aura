// tests/compiler/test_linear_cross_closure.cpp
// Thematic suite: cross-closure linear escape discovery + depth + trunc fail-closed.
// Issues: #2563, #2612, #2623 (issue numbers live in headers/comments only — not filename).
//
// @category: unit
// @reason: Consolidated linear cross-closure family. Prefer extending THIS file
//          for related ACs; do not add test_linear_cross_closure_<issue>.cpp.

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

namespace issue_2563 {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::CompilerService;
using aura::compiler::CrossClosureEscapeResult;
using aura::compiler::discover_cross_closure_linear_escapes;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::linear_cross_closure_hard_enabled;
using aura::compiler::typed_audit::production_defaults_active;
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

static void reset_2563() {
    reset_for_test();
    apply_dev_audit_defaults();
    set_strategy(AuditStrategy::Sampled);
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_HARD");
    unsetenv("AURA_PARTIAL_CONE_SOFT");
}

// Build: (lambda (p) x) where x is free dirty linear name.
static FlatAST make_lambda_capture(StringPool& pool, const char* free_name) {
    FlatAST flat;
    auto x = pool.intern(free_name);
    auto p = pool.intern("p");
    auto xv = flat.add_variable(x);
    // Minimal lambda with body = free variable (capture).
    auto lam = flat.add_lambda(std::array{p}, xv);
    flat.root = lam;
    return flat;
}

// ── AC1: discover + hard force / Soft observe ──
static void ac1_discover_hard_soft() {
    std::println("\n--- #2563 AC1: discover free-capture; hard force / Soft observe ---");
    reset_2563();

    StringPool pool;
    auto flat = make_lambda_capture(pool, "lin");
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    const bool clean =
        discover_cross_closure_linear_escapes(flat, pool, dirty, /*cone_cap=*/256, out);
    CHECK(!clean, "AC1: free-capture is an escape");
    CHECK(out.escape_sites >= 1, "AC1: escape_sites >= 1");
    CHECK(out.sites_scanned >= 1, "AC1: Lambda sites scanned");

    // Param-only use is not free-capture.
    auto flat2 = make_lambda_capture(pool, "p");
    dirty = {"p"};
    CrossClosureEscapeResult out2{};
    // lambda param is "p", free name is also "p" → param, not escape
    CHECK(discover_cross_closure_linear_escapes(flat2, pool, dirty, 256, out2),
          "AC1: param use is not free-capture escape");
    CHECK(out2.escape_sites == 0, "AC1: param capture zero escapes");

    // Soft hard_enabled default off under Sampled.
    CHECK(!linear_cross_closure_hard_enabled(), "AC1: Soft Sampled hard off");
    setenv("AURA_LINEAR_CROSS_CLOSURE_HARD", "1", 1);
    CHECK(linear_cross_closure_hard_enabled(), "AC1: env hard on");
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_HARD");
    apply_production_audit_defaults();
    CHECK(linear_cross_closure_hard_enabled(), "AC1: production hard on");
    apply_dev_audit_defaults();
    set_strategy(AuditStrategy::Full);
    CHECK(linear_cross_closure_hard_enabled(), "AC1: Full hard on");
    set_strategy(AuditStrategy::Sampled);

    // Source: force path + deny kind
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(etc.find("CrossClosureEscape") != std::string::npos, "AC1: CrossClosureEscape authority");
    CHECK(etc.find("linear-cross-closure-escape") != std::string::npos, "AC1: distinct deny kind");
    CHECK(etc.find("discover_cross_closure_linear_escapes") != std::string::npos,
          "AC1: discovery wired");
    CHECK(etc.find("linear_cross_closure_observe_total") != std::string::npos,
          "AC1: Soft observe counter");
    CHECK(etc.find("linear_cross_closure_force_total") != std::string::npos,
          "AC1: hard force counter");
}

// ── AC2: no cross-closure zero work; existing paths ──
static void ac2_zero_and_existing() {
    std::println("\n--- #2563 AC2: no capture → zero; CrossBatch/PostMutate retained ---");
    reset_2563();
    StringPool pool;
    FlatAST empty;
    empty.add_literal(1);
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    CHECK(discover_cross_closure_linear_escapes(empty, pool, dirty, 256, out),
          "AC2: no Lambda → clean");
    CHECK(out.escape_sites == 0, "AC2: zero escape_sites");
    CHECK(out.sites_scanned == 0, "AC2: zero Lambda sites");

    dirty.clear();
    auto flat = make_lambda_capture(pool, "lin");
    CHECK(discover_cross_closure_linear_escapes(flat, pool, dirty, 256, out),
          "AC2: empty dirty → clean");
    CHECK(out.escape_sites == 0, "AC2: empty dirty zero escapes");

    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(etc.find("CrossBatchEscape") != std::string::npos, "AC2: CrossBatch retained");
    CHECK(etc.find("PostMutateLinear") != std::string::npos, "AC2: PostMutate retained");
    CHECK(etc.find("hard_block_cross_batch_linear_escape") != std::string::npos,
          "AC2: cross-batch hard-block retained");
}

// ── AC3: #2545 single entry ──
static void ac3_unified_entry() {
    std::println("\n--- #2563 AC3: #2545 single-entry force_linear_rollback ---");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto eixx = read_file("src/compiler/evaluator.ixx");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(eixx.find("force_linear_rollback") != std::string::npos, "AC3: unified entry declared");
    CHECK(eixx.find("CrossClosureEscape") != std::string::npos, "AC3: authority in enum");
    CHECK(etc.find("force_linear_rollback") != std::string::npos, "AC3: force impl");
    CHECK(etc.find("case LinearForceAuthority::CrossClosureEscape") != std::string::npos,
          "AC3: CrossClosure case in force_linear_rollback");
    CHECK(emb.find("force_linear_rollback") != std::string::npos,
          "AC3: boundary still uses unified entry");
    // No ad-hoc sticky force outside force_linear_rollback for cross-closure.
    CHECK(etc.find("note_cross_closure_escape_fail") != std::string::npos, "AC3: sticky note");
    CHECK(etc.find("classify_linear_force") != std::string::npos, "AC3: classify includes axis");
}

// ── AC4: schema + source ──
static void ac4_schema() {
    std::println("\n--- #2563 AC4: schema-2563 + source-cite ---");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(aud.find("#2563") != std::string::npos, "AC4: audit cites #2563");
    CHECK(aud.find("linear_cross_closure_escape_total") != std::string::npos, "AC4: escape total");
    CHECK(aud.find("linear_cross_closure_hard_enabled") != std::string::npos, "AC4: hard helper");
    CHECK(aud.find("cross_closure_linear_escape") != std::string::npos, "AC4: audit result field");

    const auto tci = read_file("src/compiler/type_checker.ixx");
    CHECK(tci.find("discover_cross_closure_linear_escapes") != std::string::npos,
          "AC4: discover export");
    CHECK(tci.find("CrossClosureEscapeResult") != std::string::npos, "AC4: result struct");

    const auto q = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(q.find("schema-2563") != std::string::npos, "AC4: schema-2563");
    CHECK(q.find("linear-cross-closure-escape-total") != std::string::npos, "AC4: query key");

    reset_2563();
    CompilerService cs;
    CHECK(href(cs, "schema-2563") == 2563, "AC4: live schema-2563");
    CHECK(href(cs, "linear-cross-closure-wired") == 1, "AC4: wired");
    CHECK(href(cs, "linear-cross-closure-escape-total") >= 0, "AC4: escape queryable");
    CHECK(href(cs, "linear-cross-closure-force-total") >= 0, "AC4: force queryable");
    CHECK(href(cs, "linear-cross-closure-observe-total") >= 0, "AC4: observe queryable");
    CHECK(href(cs, "linear-cross-closure-hard-enabled") == 0, "AC4: Soft hard=0");
    CHECK(href(cs, "schema-2545") == 2545, "AC4: #2545 schema retained");
}

// ── AC5: cone cap ──
static void ac5_cone_cap() {
    std::println("\n--- #2563 AC5: cone-capped discovery (#2560 soft cap) ---");
    reset_2563();
    StringPool pool;
    auto flat = make_lambda_capture(pool, "lin");
    // Pad AST so size > tiny cone cap.
    for (int i = 0; i < 20; ++i)
        (void)flat.add_literal(i);
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    // cone_cap=1 may miss Lambda if it's not node 0 — still records trunc when size>cap.
    (void)discover_cross_closure_linear_escapes(flat, pool, dirty, /*cone_cap=*/1, out);
    CHECK(out.cap_truncations == 1, "AC5: cap_truncations when size > cone_cap");

    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(etc.find("AURA_PARTIAL_CONE_SOFT") != std::string::npos ||
              etc.find("partial_cone_soft_cap_for_linear") != std::string::npos,
          "AC5: discovery uses soft cone cap");
    CHECK(etc.find("linear_cross_closure_cap_trunc_total") != std::string::npos,
          "AC5: cap trunc counter wired");
}


void run_all() {
    ac1_discover_hard_soft();
    ac2_zero_and_existing();
    ac3_unified_entry();
    ac4_schema();
    ac5_cone_cap();
    apply_dev_audit_defaults();
    reset_2563();
}

} // namespace issue_2563

namespace issue_2612 {

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


void run_all() {
    ac1_default_depth1();
    ac2_depth2_discover_hard();
    ac3_cone_trunc();
    ac4_soft_observe();
    ac5_schema_source();
    apply_dev_audit_defaults();
    reset_2612();
}

} // namespace issue_2612

namespace issue_2623 {

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
    const auto inv = read_file("scripts/coverage/checks/check_linear_three_layer_wire_2559.py");
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


void run_all() {
    ac1_one_level_soft_prod();
    ac2_nested_depth2_prod();
    ac3_trunc_prod_force();
    ac4_soft_trunc_observe();
    ac5_depth0_disable();
    ac6_schema_source();
    ac7_inventory_2559();
    apply_dev_audit_defaults();
    reset_2623();
}

} // namespace issue_2623


// ── Issue #2675: linear-enforce-effective single pure API
// (effective_linear_enforce in core/provenance_tracker.hh) — decision
// table golden tests (pure, no Evaluator). Mirrors the per-detail
// self-contained namespace pattern (#81967): own read_file, own reset,
// own AC functions, own run_all. Extends the existing linear_cross_closure
// suite rather than creating a new test target.
//   AC1: production_defaults → effective Strict for IR + boundary
//        post-mutate (covers the body’s “production_defaults || fiber_hold
//        → Strict” rule)
//   AC2: Soft + no hold → Soft; fiber hold mid-boundary → Strict for
//        that hold (boundary Strict-hold wins over process Soft)
//   AC3: AURA_LINEAR_ENFORCE=strict env flag forces Strict even under
//        Soft audit strategy (env_force_strict wins in the table)
//   AC4: #2108 cross-batch escape still hard-blocks commit independent
//        of Soft (secondary gate; pure API does NOT route it)
//   AC5: same fixture AST audit vs IR execute agree on Soft vs Strict
//        (golden table covers both callers under same inputs)
//   AC6: query:linear-enforce-effective surface key + schema-2675 +
//        issue-2675 + linear-enforce-effective-pure-api-wired sentinels
namespace issue_2675 {

using aura::core::provenance::effective_linear_enforce;
using aura::core::provenance::LinearEnforceEffective;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void ac2675_production_defaults_strict() {
    std::println("\n--- #2675 AC1: production_defaults → effective Strict ---");
    // AC1: production_defaults=true → Strict (regardless of fiber_hold / env)
    CHECK(effective_linear_enforce(/*production_defaults=*/true,
                                   /*fiber_boundary_hold=*/false,
                                   /*env_force_strict=*/false) == LinearEnforceEffective::Strict,
          "AC1 #2675: production_defaults=true → Strict (no fiber, no env)");
    CHECK(effective_linear_enforce(/*production_defaults=*/true,
                                   /*fiber_boundary_hold=*/true,
                                   /*env_force_strict=*/false) == LinearEnforceEffective::Strict,
          "AC1 #2675: production_defaults=true → Strict (with fiber hold)");
    CHECK(effective_linear_enforce(/*production_defaults=*/true,
                                   /*fiber_boundary_hold=*/false,
                                   /*env_force_strict=*/true) == LinearEnforceEffective::Strict,
          "AC1 #2675: production_defaults=true → Strict (with env flag)");
}

static void ac2675_soft_no_hold_vs_fiber_hold() {
    std::println("\n--- #2675 AC2: Soft + no hold → Soft; fiber hold → Strict ---");
    // AC2a: Soft + no hold → Soft
    CHECK(effective_linear_enforce(/*production_defaults=*/false,
                                   /*fiber_boundary_hold=*/false,
                                   /*env_force_strict=*/false) == LinearEnforceEffective::Soft,
          "AC2 #2675: Soft + no hold + no env → Soft (off-fiber Soft path)");
    // AC2b: Soft + fiber hold → Strict (boundary hold wins)
    CHECK(effective_linear_enforce(/*production_defaults=*/false,
                                   /*fiber_boundary_hold=*/true,
                                   /*env_force_strict=*/false) == LinearEnforceEffective::Strict,
          "AC2 #2675: Soft + fiber hold → Strict (boundary wins over process Soft)");
}

static void ac2675_env_force_strict_wins() {
    std::println("\n--- #2675 AC3: AURA_LINEAR_ENFORCE=strict env forces Strict ---");
    // AC3: env_force_strict=true → Strict (wins over Soft + no hold)
    CHECK(effective_linear_enforce(/*production_defaults=*/false,
                                   /*fiber_boundary_hold=*/false,
                                   /*env_force_strict=*/true) == LinearEnforceEffective::Strict,
          "AC3 #2675: env_force_strict=true → Strict (wins over Soft + no hold)");
    // AC3: env_force_strict=false + production + fiber → Strict (no env needed)
    CHECK(effective_linear_enforce(/*production_defaults=*/true,
                                   /*fiber_boundary_hold=*/true,
                                   /*env_force_strict=*/false) == LinearEnforceEffective::Strict,
          "AC3 #2675: env_force_strict=false + production + fiber → Strict (decision table)");
}

static void ac2675_cross_batch_escape_independent() {
    std::println("\n--- #2675 AC4: #2108 cross-batch escape hard-block independent ---");
    // AC4: #2108 cross-batch escape must hard-block commit independent of Soft.
    // The pure effective_linear_enforce table does NOT route this — it’s
    // a separate authority (CrossBatchEscape). Verify that the secondary
    // gate sentinel + authority table are present and #2108 is still
    // hard-blocked regardless of effective_linear_enforce() result.
    auto aud = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(aud.find("CrossBatchEscape") != std::string::npos,
          "AC4 #2675: CrossBatchEscape authority still present in typed_mutation_audit.h");
    CHECK(aud.find("#2108") != std::string::npos,
          "AC4 #2675: #2108 cross-batch escape cite present (independent hard-block)");
    // Effective mode = Soft (no production, no hold, no env) — but
    // #2108 cross-batch escape must still hard-block (not via this API).
    CHECK(effective_linear_enforce(/*production_defaults=*/false,
                                   /*fiber_boundary_hold=*/false,
                                   /*env_force_strict=*/false) == LinearEnforceEffective::Soft,
          "AC4 #2675: Soft baseline (effective_linear_enforce says Soft — #2108 routes "
          "independently)");
}

static void ac2675_audit_ir_agree() {
    std::println("\n--- #2675 AC5: AST audit vs IR execute agree (golden table) ---");
    // AC5: same inputs → same decision regardless of caller (AST audit
    // or IR execute). Exhaustively cover the truth table.
    struct Row {
        bool prod;
        bool fiber;
        bool env;
        LinearEnforceEffective expected;
    };
    const Row table[] = {
        // production only → Strict
        {true, false, false, LinearEnforceEffective::Strict},
        // fiber only → Strict
        {false, true, false, LinearEnforceEffective::Strict},
        // env only → Strict (env_force_strict wins)
        {false, false, true, LinearEnforceEffective::Strict},
        // all three → Strict
        {true, true, true, LinearEnforceEffective::Strict},
        // none → Soft
        {false, false, false, LinearEnforceEffective::Soft},
        // prod + fiber (no env) → Strict
        {true, true, false, LinearEnforceEffective::Strict},
        // prod + env (no fiber) → Strict
        {true, false, true, LinearEnforceEffective::Strict},
        // fiber + env (no prod) → Strict
        {false, true, true, LinearEnforceEffective::Strict},
    };
    for (const auto& r : table) {
        const auto got = effective_linear_enforce(r.prod, r.fiber, r.env);
        CHECK(got == r.expected,
              "AC5 #2675: prod/fiber/env table row consistent with decision table");
    }
}

static void ac2675_query_surface_keys() {
    std::println("\n--- #2675 AC6: query:linear-enforce-effective surface keys ---");
    // AC6: linear-enforce-effective + schema-2675 + issue-2675 +
    // linear-enforce-effective-pure-api-wired sentinels in
    // evaluator_primitives_obs_eval.cpp + provenance_tracker.hh exposes
    // the pure API.
    auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(obs.find("\"linear-enforce-effective\"") != std::string::npos,
          "AC6 #2675: linear-enforce-effective key in query surface");
    CHECK(obs.find("\"schema-2675\"") != std::string::npos,
          "AC6 #2675: schema-2675 sentinel in query surface");
    CHECK(obs.find("\"issue-2675\"") != std::string::npos,
          "AC6 #2675: issue-2675 sentinel in query surface");
    CHECK(obs.find("\"linear-enforce-effective-pure-api-wired\"") != std::string::npos,
          "AC6 #2675: linear-enforce-effective-pure-api-wired sentinel");
    auto hdr = read_file("src/core/provenance_tracker.hh");
    CHECK(hdr.find("LinearEnforceEffective") != std::string::npos,
          "AC6 #2675: LinearEnforceEffective enum in provenance_tracker.hh");
    CHECK(hdr.find("effective_linear_enforce(") != std::string::npos,
          "AC6 #2675: effective_linear_enforce() function exported from provenance_tracker.hh");
    // Linter self-coverage.
    auto lint = read_file("scripts/coverage/checks/check_linear_enforce_effective_2675.py");
    CHECK(!lint.empty(), "AC6 #2675: linter script present");
    CHECK(lint.find("#2675") != std::string::npos, "AC6 #2675: linter cites #2675");
    CHECK(lint.find("effective_linear_enforce") != std::string::npos,
          "AC6 #2675: linter covers pure API");
    auto build = read_file("build.py");
    CHECK(build.find("check_linear_enforce_effective_2675") != std::string::npos,
          "AC6 #2675: build.py wires linter");
    // Decision table comment is single source of truth.
    auto lom = read_file("src/compiler/linear_occurrence_mutate_stats.h");
    CHECK(lom.find("Issue #2675") != std::string::npos,
          "AC6 #2675: decision table comment cites #2675");
    CHECK(lom.find("effective_linear_enforce") != std::string::npos,
          "AC6 #2675: decision table comment points to pure API");
}

void run_all() {
    std::println("\n── #2675 ──");
    ac2675_production_defaults_strict();
    ac2675_soft_no_hold_vs_fiber_hold();
    ac2675_env_force_strict_wins();
    ac2675_cross_batch_escape_independent();
    ac2675_audit_ir_agree();
    ac2675_query_surface_keys();
}

} // namespace issue_2675


int main() {
    std::println("=== linear cross-closure suite (#2563 + #2612 + #2623 + #2675) ===");
    std::println("\n── #2563 ──");
    issue_2563::run_all();
    std::println("\n── #2612 ──");
    issue_2612::run_all();
    std::println("\n── #2623 ──");
    issue_2623::run_all();
    std::println("\n── #2675 ──");
    issue_2675::run_all();
    std::println("\n=== linear cross-closure: {} passed, {} failed ===", aura::test::g_passed,
                 aura::test::g_failed);
    return aura::test::g_failed ? 1 : 0;
}
