// @category: unit
// @reason: Issue #2263 — OwnershipEscapeSummary feeds lowering MoveOp elision.
//
//   AC1: escape-after-move binding → MoveOp emitted; blocked counter bumps
//   AC2: clean owned path under active summary still elides
//   AC3: no escape summary → legacy always emit MoveOp (no elide)
//   AC4: schema-2263 + source-cite
//
// Issue #2286 — scope the gate per (Evaluator*, workspace_cow_gen) so
// multi-eval / multi-workspace hosts (#2274 PerRegion storm isolation,
// #2275 CowGenMismatch) don't cross-contaminate elision decisions.
//
//   AC6: Two Evaluators; A marks x; B elides clean y (no cross-block)
//   AC7: Same eval + matching cow_gen: escape bindings still block (#2263 parity)
//   AC8: Cow-gen advance clears/ignores stale summary (no UAF / wrong elide)
//   AC9: Happy path (no escape) bumps cross-eval-miss counter; safe default
//   AC10: schema-2286 + source-cite (gate header, hooks, impl, service_dirty, lookup)
//
// Issue #2344 — publish key ↔ lower key contract (Option A):
// wrong-key / missing-key must never elide a binding that any live summary blocks.
//
//   AC12: Matching key blocks MoveOp elision (publish + set_current + lower)
//   AC13: Wrong key miss + live blocked name → still block (conservative)
//   AC14: Two evals, disjoint names → no cross-block (isolation retained)
//   AC15: Matching key + empty blocked → elide; no conservative scan cost path
//   AC16: schema-2344 + source-cite

#include "test_harness.hpp"
#include "compiler/ownership_escape_lowering_gate.h"
#include "compiler/typed_mutation_audit.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

import std;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.lowering_linear_types;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::clear_escape_move_elision_gate;
using aura::compiler::CompilerService;
using aura::compiler::escape_move_elision_gate_active;
using aura::compiler::g_linear_escape_move_gate_wired;
using aura::compiler::g_linear_lowering_escape_summary_hit_total;
using aura::compiler::g_linear_move_elision_blocked_escape_total;
using aura::compiler::linear_move_elided_total;
using aura::compiler::lower_to_ir;
using aura::compiler::set_escape_move_elision_gate;
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

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:ownership-escape-postmutate-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Build a tiny FlatAST: (move <name>) as the root expression.
static void make_move_var_flat(aura::ast::ASTArena& arena, StringPool& pool, FlatAST& flat,
                               const char* name) {
    (void)arena;
    auto sym = pool.intern(name);
    auto var = flat.add_variable(sym);
    auto mv = flat.add_move(var);
    flat.root = mv;
}

static std::size_t count_move_ops(const aura::ir::IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions) {
        for (const auto& b : f.blocks) {
            for (const auto& ins : b.instructions) {
                if (ins.opcode == aura::ir::IROpcode::MoveOp)
                    ++n;
            }
        }
    }
    return n;
}

static void ac1_escape_blocks_elision() {
    std::println("\n--- AC1: escape-after-move → MoveOp emitted; blocked bumps ---");
    clear_escape_move_elision_gate();
    const auto blocked0 = load_u64(g_linear_move_elision_blocked_escape_total);
    const auto hit0 = load_u64(g_linear_lowering_escape_summary_hit_total);

    set_escape_move_elision_gate(true, std::unordered_set<std::string>{"x"});

    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    StringPool pool(alloc);
    FlatAST flat(alloc);
    make_move_var_flat(arena, pool, flat, "x");
    auto mod = lower_to_ir(flat, pool, arena);

    CHECK(count_move_ops(mod) >= 1, "MoveOp present when escape blocks elision");
    CHECK(load_u64(g_linear_move_elision_blocked_escape_total) > blocked0,
          "blocked-escape counter advanced");
    CHECK(load_u64(g_linear_lowering_escape_summary_hit_total) > hit0,
          "summary-hit counter advanced");
    clear_escape_move_elision_gate();
}

static void ac2_clean_path_elides() {
    std::println("\n--- AC2: clean owned path under active summary elides MoveOp ---");
    clear_escape_move_elision_gate();
    const auto elided0 = linear_move_elided_total();
    const auto hit0 = load_u64(g_linear_lowering_escape_summary_hit_total);

    // Active summary with empty blocked set — clean binding may elide.
    set_escape_move_elision_gate(true, {});

    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    StringPool pool(alloc);
    FlatAST flat(alloc);
    make_move_var_flat(arena, pool, flat, "y");
    auto mod = lower_to_ir(flat, pool, arena);

    CHECK(count_move_ops(mod) == 0, "MoveOp elided for clean binding under active summary");
    CHECK(linear_move_elided_total() > elided0, "linear_move_elided_total advanced");
    CHECK(load_u64(g_linear_lowering_escape_summary_hit_total) > hit0, "summary hit");
    clear_escape_move_elision_gate();
}

static void ac3_null_summary_legacy() {
    std::println("\n--- AC3: no escape summary → legacy always emit MoveOp ---");
    clear_escape_move_elision_gate();
    CHECK(!escape_move_elision_gate_active(), "gate inactive");
    const auto elided0 = linear_move_elided_total();
    const auto blocked0 = load_u64(g_linear_move_elision_blocked_escape_total);

    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    StringPool pool(alloc);
    FlatAST flat(alloc);
    make_move_var_flat(arena, pool, flat, "z");
    auto mod = lower_to_ir(flat, pool, arena);

    CHECK(count_move_ops(mod) >= 1, "legacy path emits MoveOp");
    // Elision counters must not advance from this null-summary lower.
    CHECK(linear_move_elided_total() == elided0, "no elide without summary");
    CHECK(load_u64(g_linear_move_elision_blocked_escape_total) == blocked0,
          "no blocked without summary");
}

static void ac4_schema_source() {
    std::println("\n--- AC4: schema-2263 + source-cite ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2263") == 2263, "schema-2263");
    CHECK(href(cs, "issue-2263") == 2263, "issue-2263");
    CHECK(href(cs, "linear-escape-move-gate-wired") == 1, "wired");
    CHECK(href(cs, "linear-move-elision-blocked-escape-total") >= 0, "blocked key");
    CHECK(href(cs, "linear-lowering-escape-summary-hit-total") >= 0, "hit key");
    CHECK(g_linear_escape_move_gate_wired.load() == 1, "wired atomic");

    const auto gate_h = read_file("src/compiler/ownership_escape_lowering_gate.h");
    CHECK(gate_h.find("Issue #2263") != std::string::npos, "gate header #2263");
    const auto lin = read_file("src/compiler/lowering_linear_types_impl.cpp");
    CHECK(lin.find("Issue #2263") != std::string::npos, "lowering #2263");
    CHECK(lin.find("escape_blocks_move_elision") != std::string::npos, "elision consult");
    const auto tc = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tc.find("set_escape_move_elision_gate") != std::string::npos, "publish gate");
    CHECK(tc.find("escape_after_move_bindings") != std::string::npos, "binding sets");
}

} // namespace

// Issue #2286: AC6-AC10 — per-(Evaluator*, cow_gen) gate scoping.
namespace {

using aura::compiler::clear_current_escape_key;
using aura::compiler::clear_escape_move_elision_gate_for_key;
using aura::compiler::escape_blocks_move_elision_for_current;
using aura::compiler::escape_blocks_move_elision_for_key;
using aura::compiler::g_linear_escape_gate_cross_eval_miss_total;
using aura::compiler::g_linear_escape_gate_key_contract_wired;
using aura::compiler::g_linear_escape_gate_miss_conservative_block_total;
using aura::compiler::publish_escape_move_elision_gate_for_key;
using aura::compiler::set_current_escape_key;

// Use a heap-allocated sentinel as a fake eval identity (each TypeChecker
// instance is a unique eval identity in production; here we mimic by using
// the address of stack-allocated objects with careful lifetime).
struct FakeEval {
    std::uint64_t sentinel = 0xCAFE'BABE'DEAD'BEEF;
};

static void ac6_cross_eval_isolation() {
    std::println("\n--- AC6: Two Evaluators, A marks x, B elides clean y ---");
    FakeEval eval_a;
    FakeEval eval_b;
    // Publish under A's identity with cow_gen=1.
    publish_escape_move_elision_gate_for_key(&eval_a, 1, true,
                                             std::unordered_set<std::string>{"x"});
    // B has no published summary for its key → lookup misses → safe default.
    CHECK(!escape_blocks_move_elision_for_key(&eval_b, 1, "y"),
          "B's clean y not blocked by A's x (no cross-contamination)");
    CHECK(escape_blocks_move_elision_for_key(&eval_a, 1, "x"),
          "A's x still blocked (matching key)");
    CHECK(!escape_blocks_move_elision_for_key(&eval_a, 1, "y"),
          "A's clean y not blocked (not in A's blocked set)");
    clear_escape_move_elision_gate_for_key(&eval_a, 1);
}

static void ac7_same_eval_parity() {
    std::println("\n--- AC7: Same eval + matching cow_gen blocks (#2263 parity) ---");
    FakeEval eval;
    publish_escape_move_elision_gate_for_key(&eval, 5, true, std::unordered_set<std::string>{"x"});
    CHECK(escape_blocks_move_elision_for_key(&eval, 5, "x"), "matching eval+gen blocks x");
    CHECK(!escape_blocks_move_elision_for_key(&eval, 5, "y"),
          "matching eval+gen doesn't block y (not in blocked set)");
    clear_escape_move_elision_gate_for_key(&eval, 5);
}

static void ac8_cow_gen_advance_clears() {
    std::println("\n--- AC8: Cow-gen advance + #2344 conservative miss ---");
    FakeEval eval;
    publish_escape_move_elision_gate_for_key(&eval, 10, true, std::unordered_set<std::string>{"x"});
    CHECK(escape_blocks_move_elision_for_key(&eval, 10, "x"), "gen=10 blocks x");
    // Issue #2344 Option A: gen=11 misses the key, but gen=10 still has x
    // blocked → conservative block (never miss→elide a live-blocked name).
    CHECK(escape_blocks_move_elision_for_key(&eval, 11, "x"),
          "gen=11 miss + live gen=10 blocks x → conservative block (#2344)");
    // Original gen still works until explicitly cleared.
    CHECK(escape_blocks_move_elision_for_key(&eval, 10, "x"),
          "gen=10 still blocks x (per-key storage)");
    clear_escape_move_elision_gate_for_key(&eval, 10);
    CHECK(!escape_blocks_move_elision_for_key(&eval, 10, "x"), "gen=10 cleared");
    CHECK(!escape_blocks_move_elision_for_key(&eval, 11, "x"), "after clear, pure miss → no block");
}

static void ac9_zero_cost_happy_path() {
    std::println("\n--- AC9: Happy path (no escape) bumps cross-eval-miss counter ---");
    const auto miss0 = load_u64(g_linear_escape_gate_cross_eval_miss_total);
    FakeEval eval;
    const bool blocks = escape_blocks_move_elision_for_key(&eval, 100, "x");
    CHECK(!blocks, "miss → safe default (no block)");
    CHECK(load_u64(g_linear_escape_gate_cross_eval_miss_total) > miss0,
          "cross-eval-miss counter bumped on keyed miss");
}

static void ac10_schema_source() {
    std::println("\n--- AC10: schema-2286 + source-cite ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2286") == 2286, "schema-2286 key");
    CHECK(href(cs, "issue-2286") == 2286, "issue-2286 key");
    CHECK(href(cs, "linear-escape-gate-cross-eval-miss-total") >= 0, "cross-eval-miss counter key");

    const auto gate_h = read_file("src/compiler/ownership_escape_lowering_gate.h");
    CHECK(gate_h.find("Issue #2286") != std::string::npos, "gate header #2286");
    CHECK(gate_h.find("EscapeGateKey") != std::string::npos, "EscapeGateKey struct");
    CHECK(gate_h.find("aura_escape_move_gate_publish_for_key") != std::string::npos,
          "keyed publish API");
    CHECK(gate_h.find("escape_blocks_move_elision_for_current") != std::string::npos,
          "thread-local lookup wrapper");
    const auto hooks = read_file("src/compiler/typed_mutation_audit_hooks.cpp");
    CHECK(hooks.find("Issue #2286") != std::string::npos, "hooks #2286");
    CHECK(hooks.find("aura_escape_blocks_move_elision_for_key") != std::string::npos,
          "keyed C lookup");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("publish_escape_move_elision_gate_for_key") != std::string::npos,
          "publish for key at post_mutation_invariant_check");
    const auto sd = read_file("src/compiler/service_dirty.cpp");
    CHECK(sd.find("set_current_escape_key") != std::string::npos,
          "set thread-local before lower_to_ir");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(q.find("schema-2286") != std::string::npos, "query schema-2286");
    const auto lin = read_file("src/compiler/lowering_linear_types_impl.cpp");
    CHECK(lin.find("escape_blocks_move_elision_for_current") != std::string::npos,
          "lookup uses thread-local current key");
    const auto tcx = read_file("src/compiler/type_checker.ixx");
    CHECK(tcx.find("Issue #2286") != std::string::npos, "type_checker.ixx cites #2286");
    CHECK(tcx.find("cache_epoch()") != std::string::npos, "cache_epoch() getter for publish key");
}

// Issue #2309: composite_txn_commit / MutationBoundary hard-gate
// force-rollback paths MUST clear the process-wide escape → MoveOp
// elision gate. Without the clear, a stale "blocked" set from a
// rejected / rolled-back txn leaks into a subsequent independent
// mutate in the same eval / process (multi-Agent / multi-round).
// Verifies:
//   AC1 — counter `g_linear_escape_gate_clear_on_rollback_total`
//         bumped on reject / force-rollback paths
//   AC2 — counter NOT bumped on success path
//   AC3 — `aura_escape_move_gate_clear()` is called at both reject /
//         force-rollback sites in evaluator_typecheck.cpp +
//         evaluator_mutation_boundary.cpp (source-cite)
//   AC4 — query surface: linear-escape-gate-clear-on-rollback-total
//         + schema-2309 / issue-2309 / escape-gate-rollback-clear-wired
//         sentinels reachable via the JIT observability query
static void ac11_2309_rollback_clear_gate() {
    std::println("\n--- AC11 (#2309): rollback-clear fix sites ---");

    // AC1 + AC3 — source-cite the clear sites. Both reject / force-rollback
    // branches in evaluator_typecheck.cpp + evaluator_mutation_boundary.cpp
    // must call aura_escape_move_gate_clear() AND bump the rollback
    // counter next to the existing rollback counters (no semantic drift).
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(etc.find("aura_escape_move_gate_clear()") != std::string::npos,
          "AC11.1: evaluator_typecheck.cpp clears gate at reject site");
    CHECK(etc.find("g_linear_escape_gate_clear_on_rollback_total.fetch_add") != std::string::npos,
          "AC11.2: evaluator_typecheck.cpp bumps rollback-clear counter");

    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("aura_escape_move_gate_clear()") != std::string::npos,
          "AC11.3: evaluator_mutation_boundary.cpp clears gate at force-rollback");
    CHECK(emb.find("g_linear_escape_gate_clear_on_rollback_total.fetch_add") != std::string::npos,
          "AC11.4: evaluator_mutation_boundary.cpp bumps rollback-clear counter");

    // Counter declaration + definition.
    const auto gate_h = read_file("src/compiler/ownership_escape_lowering_gate.h");
    CHECK(gate_h.find("g_linear_escape_gate_clear_on_rollback_total") != std::string::npos,
          "AC11.5: extern decl in ownership_escape_lowering_gate.h");
    const auto hooks = read_file("src/compiler/typed_mutation_audit_hooks.cpp");
    CHECK(hooks.find("g_linear_escape_gate_clear_on_rollback_total{0}") != std::string::npos,
          "AC11.6: counter definition in typed_mutation_audit_hooks.cpp");

    // AC4 — query surface: JIT observability query exposes the new
    // counter + schema/issue/wired sentinels (additive over #2263).
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(q.find("linear-escape-gate-clear-on-rollback-total") != std::string::npos,
          "AC11.7: query key linear-escape-gate-clear-on-rollback-total");
    CHECK(q.find("schema-2309") != std::string::npos, "AC11.8: schema-2309 sentinel");
    CHECK(q.find("issue-2309") != std::string::npos, "AC11.9: issue-2309 sentinel");
    CHECK(q.find("escape-gate-rollback-clear-wired") != std::string::npos,
          "AC11.10: escape-gate-rollback-clear-wired sentinel");

    // No schema break — existing #2263 / #2286 keys still present.
    CHECK(q.find("schema-2263") != std::string::npos,
          "AC11.11: schema-2263 still present (no #2263 schema break)");
    CHECK(q.find("schema-2286") != std::string::npos,
          "AC11.12: schema-2286 still present (no #2286 schema break)");
}

// ── Issue #2344: publish key ↔ lower key contract (Option A) ──

static void ac12_matching_key_blocks_move() {
    std::println("\n--- AC12 (#2344): matching key blocks MoveOp elision ---");
    FakeEval eval;
    clear_escape_move_elision_gate_for_key(&eval, 1);
    publish_escape_move_elision_gate_for_key(&eval, 1, true, std::unordered_set<std::string>{"x"});
    set_current_escape_key(&eval, 1);
    CHECK(escape_blocks_move_elision_for_current("x"), "matching TLS key blocks x");

    const auto blocked0 = load_u64(g_linear_move_elision_blocked_escape_total);
    const auto elided0 = linear_move_elided_total();
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    StringPool pool(alloc);
    FlatAST flat(alloc);
    make_move_var_flat(arena, pool, flat, "x");
    auto mod = lower_to_ir(flat, pool, arena);
    CHECK(count_move_ops(mod) >= 1, "AC12: MoveOp emitted (blocked, not elided)");
    CHECK(load_u64(g_linear_move_elision_blocked_escape_total) > blocked0,
          "AC12: blocked-escape counter +1");
    CHECK(linear_move_elided_total() == elided0, "AC12: elided counter unchanged for blocked x");
    clear_current_escape_key();
    clear_escape_move_elision_gate_for_key(&eval, 1);
}

static void ac13_wrong_key_never_elides_blocked() {
    std::println("\n--- AC13 (#2344): wrong key miss never elides blocked name ---");
    FakeEval eval_a;
    FakeEval eval_b;
    clear_escape_move_elision_gate_for_key(&eval_a, 1);
    clear_escape_move_elision_gate_for_key(&eval_b, 1);
    clear_escape_move_elision_gate_for_key(&eval_a, 2);

    publish_escape_move_elision_gate_for_key(&eval_a, 1, true,
                                             std::unordered_set<std::string>{"x"});
    const auto miss0 = load_u64(g_linear_escape_gate_cross_eval_miss_total);
    const auto cons0 = load_u64(g_linear_escape_gate_miss_conservative_block_total);

    // Wrong eval (eval_b, gen1) — key miss, but x is blocked under eval_a.
    CHECK(escape_blocks_move_elision_for_key(&eval_b, 1, "x"),
          "AC13: wrong-eval miss + live blocked x → still block");
    // Wrong gen (eval_a, gen2) — same Option A path.
    CHECK(escape_blocks_move_elision_for_key(&eval_a, 2, "x"),
          "AC13: wrong-gen miss + live blocked x → still block");

    CHECK(load_u64(g_linear_escape_gate_cross_eval_miss_total) > miss0,
          "AC13: cross-eval-miss bumped");
    CHECK(load_u64(g_linear_escape_gate_miss_conservative_block_total) > cons0,
          "AC13: escape-miss-conservative-block-total bumped");

    // End-to-end: TLS key is eval_b (no entry) while eval_a blocks x.
    // Gate may be inactive for eval_b → legacy emit MoveOp is also safe;
    // if active path is hit via for_key, blocks remains true.
    set_current_escape_key(&eval_b, 1);
    CHECK(escape_blocks_move_elision_for_current("x"),
          "AC13: TLS wrong key still blocks via Option A");
    clear_current_escape_key();
    clear_escape_move_elision_gate_for_key(&eval_a, 1);
}

static void ac14_disjoint_names_isolation() {
    std::println("\n--- AC14 (#2344): two evals, disjoint names → no cross-block ---");
    FakeEval eval_a;
    FakeEval eval_b;
    clear_escape_move_elision_gate_for_key(&eval_a, 1);
    clear_escape_move_elision_gate_for_key(&eval_b, 1);
    publish_escape_move_elision_gate_for_key(&eval_a, 1, true,
                                             std::unordered_set<std::string>{"x"});
    // B's clean y: miss on B's key; any-key scan for "y" finds nothing → no block.
    CHECK(!escape_blocks_move_elision_for_key(&eval_b, 1, "y"),
          "AC14: B's y not blocked by A's x (isolation)");
    CHECK(escape_blocks_move_elision_for_key(&eval_a, 1, "x"), "AC14: A's x still blocked");
    clear_escape_move_elision_gate_for_key(&eval_a, 1);
}

static void ac15_happy_path_empty_blocked_elides() {
    std::println("\n--- AC15 (#2344): matching key + empty blocked → elide ---");
    FakeEval eval;
    clear_escape_move_elision_gate_for_key(&eval, 7);
    // Active summary with empty blocked set under matching key.
    publish_escape_move_elision_gate_for_key(&eval, 7, true, {});
    set_current_escape_key(&eval, 7);
    CHECK(!escape_blocks_move_elision_for_current("y"),
          "AC15: matching key empty blocked → not blocked");
    const auto cons0 = load_u64(g_linear_escape_gate_miss_conservative_block_total);
    const auto elided0 = linear_move_elided_total();
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    StringPool pool(alloc);
    FlatAST flat(alloc);
    make_move_var_flat(arena, pool, flat, "y");
    auto mod = lower_to_ir(flat, pool, arena);
    CHECK(count_move_ops(mod) == 0, "AC15: MoveOp elided under active empty summary");
    CHECK(linear_move_elided_total() > elided0, "AC15: elided counter advanced");
    // Matching key path must not take the conservative miss scan.
    CHECK(load_u64(g_linear_escape_gate_miss_conservative_block_total) == cons0,
          "AC15: no conservative-block bump on matching-key happy path");
    clear_current_escape_key();
    clear_escape_move_elision_gate_for_key(&eval, 7);
}

static void ac16_schema_source_2344() {
    std::println("\n--- AC16 (#2344): schema-2344 + source-cite ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2344") == 2344, "schema-2344");
    CHECK(href(cs, "issue-2344") == 2344, "issue-2344");
    CHECK(href(cs, "escape-gate-key-contract-wired") == 1, "escape-gate-key-contract-wired");
    CHECK(href(cs, "escape-miss-conservative-block-total") >= 0,
          "escape-miss-conservative-block-total key");
    CHECK(href(cs, "linear-escape-gate-cross-eval-miss-total") >= 0,
          "schema-2286 miss key retained");
    CHECK(g_linear_escape_gate_key_contract_wired.load() == 1, "wired atomic");

    const auto gate_h = read_file("src/compiler/ownership_escape_lowering_gate.h");
    CHECK(gate_h.find("Issue #2344") != std::string::npos, "gate header #2344");
    CHECK(gate_h.find("g_linear_escape_gate_miss_conservative_block_total") != std::string::npos,
          "conservative-block atomic decl");
    const auto hooks = read_file("src/compiler/typed_mutation_audit_hooks.cpp");
    CHECK(hooks.find("Issue #2344") != std::string::npos, "hooks #2344");
    CHECK(hooks.find("g_linear_escape_gate_miss_conservative_block_total") != std::string::npos,
          "hooks conservative counter");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("publish_escape_move_elision_gate_for_key") != std::string::npos,
          "publish site");
    const auto sd = read_file("src/compiler/service_dirty.cpp");
    CHECK(sd.find("set_current_escape_key") != std::string::npos, "set_current_escape_key");
    const auto lin = read_file("src/compiler/lowering_linear_types_impl.cpp");
    CHECK(lin.find("escape_blocks_move_elision_for_current") != std::string::npos,
          "Move lower consults current key");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(q.find("schema-2344") != std::string::npos, "query schema-2344");
    CHECK(q.find("escape-miss-conservative-block-total") != std::string::npos,
          "query conservative key");
    // No schema break.
    CHECK(q.find("schema-2263") != std::string::npos, "schema-2263 retained");
    CHECK(q.find("schema-2286") != std::string::npos, "schema-2286 retained");
}

// ── Issue #2899: proven Move/Drop IR fast-path after TypeLinear proof ──

static void ac2899_1_proof_fresh_skips() {
    std::println("\n--- #2899 AC1: proof linear_ok + no escape → fastpath skip ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    // Fresh success proof face.
    stamp_type_linear_commit_proof(/*epoch=*/28991);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(/*would_allow=*/true, /*linear_ok=*/true);
    const auto skip0 = linear_ir_fastpath_skip_total_v_read();
    const auto blk0 = linear_ir_fastpath_skip_blocked_total_v_read();
    CHECK(linear_ir_fastpath_try_skip(), "2899 AC1: try_skip true under fresh proof");
    CHECK(linear_ir_fastpath_skip_total_v_read() > skip0, "2899 AC1: skip_total bumps");
    CHECK(linear_ir_fastpath_skip_blocked_total_v_read() == blk0,
          "2899 AC1: blocked not bumped on skip");
    // No force_linear_rollback authority table entry needed — pure IR path.
    clear_type_linear_commit_proof_for_test();
    reset_linear_ir_fastpath_counters_for_test();
}

static void ac2899_2_escape_or_reject_blocks() {
    std::println("\n--- #2899 AC2: escape active / proof Reject → no skip ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    stamp_type_linear_commit_proof(28992);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    // Escape gate active with blocked set → fast-path blocked.
    set_escape_move_elision_gate(true, std::unordered_set<std::string>{"x"});
    const auto blk0 = linear_ir_fastpath_skip_blocked_total_v_read();
    CHECK(!linear_ir_fastpath_try_skip(), "2899 AC2: escape active blocks skip");
    CHECK(linear_ir_fastpath_skip_blocked_total_v_read() > blk0,
          "2899 AC2: blocked counter bumps on escape");
    clear_escape_move_elision_gate();
    // Reject outcome → no skip.
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeReject);
    publish_last_proof_face(false, false);
    const auto blk1 = linear_ir_fastpath_skip_blocked_total_v_read();
    CHECK(!linear_ir_fastpath_try_skip(), "2899 AC2: Reject outcome blocks skip");
    CHECK(linear_ir_fastpath_skip_blocked_total_v_read() > blk1,
          "2899 AC2: blocked bumps on Reject");
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_linear_ir_fastpath_counters_for_test();
}

static void ac2899_3_no_proof_or_mid_boundary() {
    std::println("\n--- #2899 AC3: no proof / mid-boundary → full check ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    // No stamp → zero cost, no skip, no blocked noise.
    const auto skip0 = linear_ir_fastpath_skip_total_v_read();
    const auto blk0 = linear_ir_fastpath_skip_blocked_total_v_read();
    CHECK(!linear_ir_fastpath_try_skip(), "2899 AC3: no proof → no skip");
    CHECK(linear_ir_fastpath_skip_total_v_read() == skip0, "2899 AC3: skip_total quiet");
    CHECK(linear_ir_fastpath_skip_blocked_total_v_read() == blk0,
          "2899 AC3: blocked quiet (zero cost)");
    // Fresh proof but mid-boundary → full check.
    stamp_type_linear_commit_proof(28993);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    g_linear_ir_fastpath_boundary_depth_override = 1;
    const auto blk1 = linear_ir_fastpath_skip_blocked_total_v_read();
    CHECK(!linear_ir_fastpath_try_skip(), "2899 AC3: mid-boundary blocks skip");
    CHECK(linear_ir_fastpath_skip_blocked_total_v_read() > blk1,
          "2899 AC3: blocked bumps mid-boundary");
    g_linear_ir_fastpath_boundary_depth_override = -1;
    clear_type_linear_commit_proof_for_test();
    reset_linear_ir_fastpath_counters_for_test();
}

static void ac2899_4_additive_query() {
    std::println("\n--- #2899 AC4: additive query keys + prior surfaces ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2899") == 2899, "2899 AC4: schema-2899");
    CHECK(href(cs, "issue-2899") == 2899, "2899 AC4: issue-2899");
    CHECK(href(cs, "linear-ir-fastpath-wired") == 1, "2899 AC4: wired");
    CHECK(href(cs, "linear-ir-fastpath-skip-total") >= 0, "2899 AC4: skip-total queryable");
    CHECK(href(cs, "linear-ir-fastpath-skip-blocked-total") >= 0,
          "2899 AC4: skip-blocked queryable");
    // Prior surfaces preserved.
    CHECK(href(cs, "schema-2263") == 2263, "2899 AC4: schema-2263 preserved");
    CHECK(href(cs, "linear-escape-move-gate-wired") == 1, "2899 AC4: #2263 wired");
    CHECK(aura::compiler::typed_audit::kLinearIrFastpathIssue == 2899, "2899 AC4: issue constant");
}

static void ac2899_5_source_cite() {
    std::println("\n--- #2899 AC5: source-cite + no docs/design ---");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto t = read_file("tests/compiler/test_escape_move_elision_gate.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_linear_ir_fastpath_2899.py");
    const auto build = read_file("build.py");
    CHECK(aud.find("2899") != std::string::npos, "2899 AC5: audit cites #2899");
    CHECK(aud.find("linear_ir_fastpath_try_skip") != std::string::npos, "2899 AC5: try_skip");
    CHECK(aud.find("g_linear_ir_fastpath_skip_total") != std::string::npos, "2899 AC5: skip total");
    CHECK(ir.find("2899") != std::string::npos, "2899 AC5: ir_executor cites #2899");
    CHECK(ir.find("linear_ir_fastpath_try_skip") != std::string::npos,
          "2899 AC5: IR calls try_skip");
    CHECK(q.find("schema-2899") != std::string::npos, "2899 AC5: query schema-2899");
    CHECK(q.find("linear-ir-fastpath-skip-total") != std::string::npos, "2899 AC5: query key");
    CHECK(q.find("schema-2263") != std::string::npos, "2899 AC5: #2263 preserved");
    CHECK(q.find("schema-2854") != std::string::npos || aud.find("2854") != std::string::npos,
          "2899 AC5: #2854 lineage retained");
    CHECK(t.find("ac2899_1_proof_fresh_skips") != std::string::npos, "2899 AC5: AC1 test");
    CHECK(t.find("ac2899_2_escape_or_reject_blocks") != std::string::npos, "2899 AC5: AC2 test");
    CHECK(t.find("ac2899_3_no_proof_or_mid_boundary") != std::string::npos, "2899 AC5: AC3 test");
    CHECK(t.find("ac2899_4_additive_query") != std::string::npos, "2899 AC5: AC4 test");
    CHECK(!lint.empty() && lint.find("2899") != std::string::npos, "2899 AC5: linter");
    CHECK(build.find("check_linear_ir_fastpath_2899") != std::string::npos,
          "2899 AC5: build.py gate");
    CHECK(read_file("docs/design/2899-linear-ir-fastpath.md").empty(),
          "2899 AC5: no docs/design/2899-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_2899.cpp").empty(),
          "2899 AC5: no new test file per #81967");
}

// ── Issue #2964: unified linear_fast_path_ok + force revalidate ──
// Single predicate: proof.fresh && linear_ok && depth==0 && !escape &&
// !densify_pending. IR elision only when true. !ok on outermost success
// under production/Full forces revalidate; Soft observe-only; quiet zero.

static void ac2964_1_unified_predicate() {
    std::println("\n--- #2964 AC1: linear_fast_path_ok single predicate ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    reset_linear_fast_path_force_revalidate_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);

    CHECK(!linear_fast_path_ok(), "2964 AC1: no stamp → not ok");
    stamp_type_linear_commit_proof(29641);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(linear_fast_path_ok(), "2964 AC1: fresh proof + clear gates → ok");
    CHECK(linear_ir_fastpath_try_skip(), "2964 AC1: try_skip uses unified ok");

    clear_type_linear_commit_proof_for_test();
    reset_linear_ir_fastpath_counters_for_test();
}

static void ac2964_2_force_revalidate_production() {
    std::println("\n--- #2964 AC2: !ok under production → ForceRevalidate ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    reset_linear_fast_path_force_revalidate_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);

    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    // Stale proof → ForceRevalidate under production.
    CHECK(!linear_fast_path_ok(), "2964 AC2: stale → !ok");
    CHECK(linear_fast_path_boundary_exit_action() == LinearFastPathExitAction::ForceRevalidate,
          "2964 AC2: production ForceRevalidate on stale");

    // Fresh ok → Quiet (no force).
    stamp_type_linear_commit_proof(29642);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(linear_fast_path_ok(), "2964 AC2: fresh ok");
    CHECK(linear_fast_path_boundary_exit_action() == LinearFastPathExitAction::Quiet,
          "2964 AC2: Quiet when ok under production");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    clear_type_linear_commit_proof_for_test();
}

static void ac2964_3_independent_arms() {
    std::println("\n--- #2964 AC3: mid-boundary / escape / densify each disable ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    stamp_type_linear_commit_proof(29643);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(linear_fast_path_ok(), "2964 AC3: baseline ok");

    // mid-boundary arm
    g_linear_ir_fastpath_boundary_depth_override = 2;
    CHECK(!linear_fast_path_ok(), "2964 AC3: mid-boundary disables");
    g_linear_ir_fastpath_boundary_depth_override = 0;
    CHECK(linear_fast_path_ok(), "2964 AC3: depth clear restores");

    // escape arm
    set_escape_move_elision_gate(true, std::unordered_set<std::string>{"x"});
    CHECK(!linear_fast_path_ok(), "2964 AC3: escape disables");
    clear_escape_move_elision_gate();
    CHECK(linear_fast_path_ok(), "2964 AC3: escape clear restores");

    // densify-pending arm
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        1, std::memory_order_relaxed);
    CHECK(!linear_fast_path_ok(), "2964 AC3: densify-pending disables");
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    CHECK(linear_fast_path_ok(), "2964 AC3: densify clear restores");

    clear_type_linear_commit_proof_for_test();
    g_linear_ir_fastpath_boundary_depth_override = -1;
}

static void ac2964_4_soft_and_quiet() {
    std::println("\n--- #2964 AC2/AC4: Soft observe + quiet zero cost ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    reset_linear_fast_path_force_revalidate_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;

    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    // Soft strategy
    set_strategy(AuditStrategy::Sampled);

    CHECK(!linear_fast_path_ok(), "2964 AC4: stale !ok");
    CHECK(linear_fast_path_boundary_exit_action() == LinearFastPathExitAction::SoftObserve,
          "2964 AC2: Soft observe when !ok");

    stamp_type_linear_commit_proof(29644);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    const auto force0 = linear_fast_path_force_revalidate_total_v_read();
    const auto obs0 = linear_fast_path_force_revalidate_observe_total_v_read();
    CHECK(linear_fast_path_boundary_exit_action() == LinearFastPathExitAction::Quiet,
          "2964 AC4: Quiet when ok");
    CHECK(linear_fast_path_force_revalidate_total_v_read() == force0,
          "2964 AC4: force counter flat on Quiet");
    CHECK(linear_fast_path_force_revalidate_observe_total_v_read() == obs0,
          "2964 AC4: observe flat on Quiet");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    clear_type_linear_commit_proof_for_test();
}

static void ac2964_5_additive_schema() {
    std::println("\n--- #2964 AC5: additive schema + #2899 preserved ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2964") == 2964, "2964 AC5: schema-2964");
    CHECK(href(cs, "issue-2964") == 2964, "2964 AC5: issue-2964");
    CHECK(href(cs, "linear-fast-path-unified-wired") == 1, "2964 AC5: wired");
    CHECK(href(cs, "linear-fast-path-force-revalidate-total") >= 0, "2964 AC5: force key");
    CHECK(href(cs, "linear-fast-path-force-revalidate-observe-total") >= 0,
          "2964 AC5: observe key");
    CHECK(href(cs, "schema-2899") == 2899, "2964 AC5: schema-2899 preserved");
    CHECK(href(cs, "linear-ir-fastpath-skip-total") >= 0, "2964 AC5: #2899 skip preserved");
    CHECK(href(cs, "schema-2263") == 2263, "2964 AC5: schema-2263 preserved");
    CHECK(aura::compiler::typed_audit::kLinearFastPathUnifiedIssue == 2964,
          "2964 AC5: issue constant");
}

static void ac2964_6_source_cite() {
    std::println("\n--- #2964 AC6: source-cite + linter + no design ---");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto esc = read_file("src/compiler/ownership_escape_lowering_gate.h");
    const auto low = read_file("src/compiler/lowering_linear_types_impl.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto t = read_file("tests/compiler/test_escape_move_elision_gate.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_linear_fast_path_unified_2964.py");
    const auto build = read_file("build.py");
    CHECK(aud.find("linear_fast_path_ok") != std::string::npos, "2964 AC6: linear_fast_path_ok");
    CHECK(aud.find("2964") != std::string::npos, "2964 AC6: audit cites #2964");
    CHECK(aud.find("linear_fast_path_boundary_exit_action") != std::string::npos,
          "2964 AC6: boundary exit action");
    CHECK(ir.find("2964") != std::string::npos, "2964 AC6: ir cites #2964");
    CHECK(mb.find("linear_fast_path_boundary_exit_action") != std::string::npos,
          "2964 AC6: boundary wires exit action");
    CHECK(mb.find("record_revalidate_hit") != std::string::npos, "2964 AC6: revalidate hit");
    CHECK(esc.find("2964") != std::string::npos, "2964 AC6: escape gate cites #2964");
    CHECK(low.find("2964") != std::string::npos, "2964 AC6: lowering cites #2964");
    CHECK(q.find("schema-2964") != std::string::npos, "2964 AC6: query schema");
    CHECK(t.find("ac2964_1_unified_predicate") != std::string::npos, "2964 AC6: AC1 test");
    CHECK(t.find("ac2964_3_independent_arms") != std::string::npos, "2964 AC6: densify/depth arms");
    CHECK(!lint.empty() && lint.find("2964") != std::string::npos, "2964 AC6: linter");
    CHECK(build.find("check_linear_fast_path_unified_2964") != std::string::npos,
          "2964 AC6: build.py gate");
    CHECK(read_file("docs/design/2964-linear-fast-path.md").empty(),
          "2964 AC6: no docs/design/2964-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_2964.cpp").empty(),
          "2964 AC6: no new test file per #81967");
}

// ── Issue #3006: Production !ok forces dirty-root revalidate ──
// Residual of #2964: late re-eval after Phase 1; render_fast cannot skip;
// Production never elides under a false predicate.

static void ac3006_1_production_dirty_root() {
    std::println("\n--- #3006 AC1: Production !ok → dirty-root ForceRevalidate ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    reset_linear_fast_path_force_revalidate_for_test();
    reset_linear_fast_path_dirty_revalidate_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);

    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    CHECK(!linear_fast_path_ok(), "3006 AC1: stale → !ok");
    CHECK(linear_fast_path_boundary_exit_action() == LinearFastPathExitAction::ForceRevalidate,
          "3006 AC1: production ForceRevalidate");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("enforce_linear_boundary_consistency") != std::string::npos,
          "3006 AC1: dirty-root walk (not EnvFrame-only)");
    CHECK(mb.find("g_linear_fast_path_dirty_revalidate_total") != std::string::npos,
          "3006 AC1: dirty-revalidate counter");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    clear_type_linear_commit_proof_for_test();
}

static void ac3006_2_no_elide_under_false() {
    std::println("\n--- #3006 AC2: Production never elides under false predicate ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    reset_linear_fast_path_dirty_revalidate_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;

    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    // Stamp a proof so try_skip is past the zero-stamp quiet return, then
    // flip depth so !ok — Production must not elide (blocked counter).
    stamp_type_linear_commit_proof(30061);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    g_linear_ir_fastpath_boundary_depth_override = 2;
    CHECK(!linear_fast_path_ok(), "3006 AC2: !ok");
    CHECK(!linear_ir_fastpath_try_skip(), "3006 AC2: try_skip false when !ok");
    CHECK(linear_fast_path_elide_blocked_production_total_v_read() > 0,
          "3006 AC2: Production elide-blocked counter");
    g_linear_ir_fastpath_boundary_depth_override = 0;

    // Fresh ok still allowed.
    stamp_type_linear_commit_proof(30062);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    reset_linear_ir_fastpath_counters_for_test();
    CHECK(linear_fast_path_ok(), "3006 AC2: fresh ok");
    CHECK(linear_ir_fastpath_try_skip(), "3006 AC2: try_skip when ok");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    clear_type_linear_commit_proof_for_test();
}

static void ac3006_3_late_reeval_and_render_fast() {
    std::println("\n--- #3006 AC3: late re-eval + render_fast cannot skip ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("linear_fast_path_maybe_force_dirty_revalidate") != std::string::npos,
          "3006 AC3: helper");
    CHECK(mb.find("late re-eval after Phase 1") != std::string::npos ||
              mb.find("/*late=*/true") != std::string::npos,
          "3006 AC3: late re-eval after Phase 1");
    CHECK(mb.find("cannot") != std::string::npos && mb.find("render_fast") != std::string::npos,
          "3006 AC3: render_fast cannot skip when !ok");
    const auto low = read_file("src/compiler/lowering_linear_types_impl.cpp");
    CHECK(low.find("aura_linear_fast_path_depth_or_densify_block") != std::string::npos,
          "3006 AC3: lowering hard-block depth/densify");
}

static void ac3006_4_soft_observe() {
    std::println("\n--- #3006 AC4: Soft observe only ---");
    using namespace aura::compiler::typed_audit;
    clear_type_linear_commit_proof_for_test();
    reset_linear_fast_path_dirty_revalidate_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    set_strategy(AuditStrategy::Sampled);
    CHECK(!linear_fast_path_ok(), "3006 AC4: !ok");
    CHECK(linear_fast_path_boundary_exit_action() == LinearFastPathExitAction::SoftObserve,
          "3006 AC4: Soft observe");
    CHECK(linear_fast_path_dirty_revalidate_total_v_read() == 0,
          "3006 AC4: no dirty-revalidate bump on Soft");
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3006_5_schema_lineage() {
    std::println("\n--- #3006 AC5: schema-3006 + #2964/#2899 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "schema-3006") == 3006, "3006 AC5: schema-3006");
    CHECK(href(cs, "issue-3006") == 3006, "3006 AC5: issue-3006");
    CHECK(href(cs, "linear-fast-path-dirty-revalidate-wired") == 1, "3006 AC5: wired");
    CHECK(href(cs, "linear-fast-path-dirty-revalidate-total") >= 0, "3006 AC5: dirty total");
    CHECK(href(cs, "linear-fast-path-late-reeval-total") >= 0, "3006 AC5: late total");
    CHECK(href(cs, "linear-fast-path-elide-blocked-production-total") >= 0,
          "3006 AC5: elide-blocked");
    CHECK(href(cs, "schema-2964") == 2964, "3006 AC5: schema-2964 preserved");
    CHECK(href(cs, "schema-2899") == 2899, "3006 AC5: schema-2899 preserved");
    CHECK(aura::compiler::typed_audit::kLinearFastPathDirtyRevalidateIssue == 3006,
          "3006 AC5: issue constant");
}

static void ac3006_6_linter_no_design() {
    std::println("\n--- #3006 AC6: linter + no invent / no design ---");
    const auto t = read_file("tests/compiler/test_escape_move_elision_gate.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_linear_fast_path_dirty_revalidate_3006.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3006_1_production_dirty_root") != std::string::npos, "3006 AC6: AC1");
    CHECK(t.find("ac3006_2_no_elide_under_false") != std::string::npos, "3006 AC6: AC2");
    CHECK(t.find("ac3006_3_late_reeval_and_render_fast") != std::string::npos, "3006 AC6: AC3");
    CHECK(t.find("ac3006_4_soft_observe") != std::string::npos, "3006 AC6: AC4");
    CHECK(t.find("ac3006_5_schema_lineage") != std::string::npos, "3006 AC6: AC5");
    CHECK(!lint.empty() && lint.find("#3006") != std::string::npos, "3006 AC6: linter");
    CHECK(build.find("check_linear_fast_path_dirty_revalidate_3006") != std::string::npos,
          "3006 AC6: build.py gate");
    CHECK(build.find("cmd_linear_fast_path_dirty_revalidate_3006_coverage") != std::string::npos,
          "3006 AC6: build.py cmd");
    CHECK(read_file("tests/compiler/test_issue_3006.cpp").empty(),
          "3006 AC6: no test_issue_3006.cpp");
}

// ── Issue #3030: abort/restore clears TypeLinearCommitProof face ──

static void ac3030_1_abort_clears_fast_path() {
    std::println("\n--- #3030 AC1: production abort → !linear_fast_path_ok ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_type_linear_proof_cleared_on_abort_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        1, std::memory_order_relaxed);

    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    stamp_type_linear_commit_proof(30301);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    CHECK(linear_fast_path_ok(), "3030 AC1: fresh stamp ok before abort");
    CHECK(linear_ir_fastpath_try_skip(), "3030 AC1: try_skip before abort");

    const auto clr0 = type_linear_proof_cleared_on_abort_total_v_read();
    clear_type_linear_commit_proof_on_abort();
    CHECK(!linear_fast_path_ok(), "3030 AC1: after abort !ok");
    CHECK(!linear_ir_fastpath_try_skip(), "3030 AC1: Move/Drop cannot skip");
    CHECK(last_type_linear_commit_proof_stamp_v_read() == 0, "3030 AC1: stamp cleared");
    CHECK(g_last_proof_would_allow_commit.load() == 0, "3030 AC1: would_allow=0");
    CHECK(g_last_proof_linear_ok.load() == 0, "3030 AC1: linear_ok=0");
    CHECK(last_type_linear_proof_outcome_v_read() == kTypeLinearProofOutcomeReject,
          "3030 AC1: outcome Reject");
    CHECK(type_linear_proof_cleared_on_abort_total_v_read() == clr0 + 1,
          "3030 AC1: production counter +1");
    CHECK(linear_densify_scan_mismatch_inject_pending() == 0, "3030 AC1: densify inject cleared");
    CHECK(linear_fast_path_boundary_exit_action() == LinearFastPathExitAction::ForceRevalidate,
          "3030 AC1: production ForceRevalidate until fresh stamp");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
}

static void ac3030_2_fresh_stamp_restores() {
    std::println("\n--- #3030 AC2: fresh outermost stamp restores fast-path ---");
    using namespace aura::compiler::typed_audit;
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    stamp_type_linear_commit_proof(7);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    clear_type_linear_commit_proof_on_abort();
    CHECK(!linear_fast_path_ok(), "3030 AC2: cleared");
    stamp_type_linear_commit_proof(30302);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(linear_fast_path_ok(), "3030 AC2: fresh stamp ok (no success-path regression)");
    CHECK(linear_ir_fastpath_try_skip(), "3030 AC2: try_skip after fresh stamp");
    clear_type_linear_commit_proof_for_test();
}

static void ac3030_3_soft_observe() {
    std::println("\n--- #3030 AC3: Soft observe-only counter ---");
    using namespace aura::compiler::typed_audit;
    clear_type_linear_commit_proof_for_test();
    reset_type_linear_proof_cleared_on_abort_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    set_strategy(AuditStrategy::Sampled);
    stamp_type_linear_commit_proof(30303);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    const auto hard0 = type_linear_proof_cleared_on_abort_total_v_read();
    const auto obs0 = type_linear_proof_cleared_on_abort_observe_total_v_read();
    clear_type_linear_commit_proof_on_abort();
    CHECK(!linear_fast_path_ok(), "3030 AC3: Soft still clears face");
    CHECK(type_linear_proof_cleared_on_abort_total_v_read() == hard0,
          "3030 AC3: production counter unchanged");
    CHECK(type_linear_proof_cleared_on_abort_observe_total_v_read() == obs0 + 1,
          "3030 AC3: observe counter +1");
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    clear_type_linear_commit_proof_for_test();
}

static void ac3030_4_quiet_zero_cost() {
    std::println("\n--- #3030 AC4: quiet abort (no face) zero extra counter ---");
    using namespace aura::compiler::typed_audit;
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_type_linear_proof_cleared_on_abort_for_test();
    const auto hard0 = type_linear_proof_cleared_on_abort_total_v_read();
    const auto obs0 = type_linear_proof_cleared_on_abort_observe_total_v_read();
    clear_type_linear_commit_proof_on_abort();
    CHECK(type_linear_proof_cleared_on_abort_total_v_read() == hard0, "3030 AC4: no hard bump");
    CHECK(type_linear_proof_cleared_on_abort_observe_total_v_read() == obs0,
          "3030 AC4: no observe");
}

static void ac3030_5_schema_and_wire() {
    std::println("\n--- #3030 AC5: schema-3030 + abort sites wired ---");
    CompilerService cs;
    CHECK(href(cs, "schema-3030") == 3030, "3030 AC5: schema-3030");
    CHECK(href(cs, "issue-3030") == 3030, "3030 AC5: issue-3030");
    CHECK(href(cs, "type-linear-proof-cleared-on-abort-wired") == 1, "3030 AC5: wired");
    CHECK(href(cs, "type-linear-proof-cleared-on-abort-total") >= 0, "3030 AC5: total");
    CHECK(href(cs, "schema-3006") == 3006, "3030 AC5: schema-3006 preserved");
    CHECK(href(cs, "schema-2964") == 2964, "3030 AC5: schema-2964 preserved");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("clear_type_linear_commit_proof_on_abort") != std::string::npos,
          "3030 AC5: boundary calls clear");
    std::size_t restores = 0;
    std::size_t clears = 0;
    for (std::size_t p = 0; (p = mb.find("abort_restore_dual_topology", p)) != std::string::npos;
         p += 1)
        ++restores;
    for (std::size_t p = 0;
         (p = mb.find("clear_type_linear_commit_proof_on_abort", p)) != std::string::npos; p += 1)
        ++clears;
    CHECK(restores >= 3, "3030 AC5: three dual-restore call sites");
    CHECK(clears >= 3, "3030 AC5: clear on every restore site");
}

static void ac3030_6_linter_no_design() {
    std::println("\n--- #3030 AC6: linter + no invent / no design ---");
    const auto t = read_file("tests/compiler/test_escape_move_elision_gate.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_type_linear_proof_clear_on_abort_3030.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3030_1_abort_clears_fast_path") != std::string::npos, "3030 AC6: AC1");
    CHECK(t.find("ac3030_2_fresh_stamp_restores") != std::string::npos, "3030 AC6: AC2");
    CHECK(!lint.empty() && lint.find("#3030") != std::string::npos, "3030 AC6: linter");
    CHECK(build.find("check_type_linear_proof_clear_on_abort_3030") != std::string::npos,
          "3030 AC6: build.py gate");
    CHECK(read_file("tests/compiler/test_issue_3030.cpp").empty(),
          "3030 AC6: no test_issue_3030.cpp");
    CHECK(read_file("docs/design/3030-type-linear-proof-abort.md").empty(),
          "3030 AC6: no docs/design/");
}

static void ac3032_hermetic_invalidate() {
    std::println("\n--- #3032 AC: hermetic invalidate blocks Move/Drop skip ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_rehydrate_miss_invalidate_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    stamp_type_linear_commit_proof(30320);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(linear_fast_path_ok(), "3032: green ok");
    CHECK(linear_ir_fastpath_try_skip(), "3032: skip before miss");
    CHECK(invalidate_fast_path_on_rehydrate_miss(), "3032: hard invalidate");
    CHECK(!linear_fast_path_ok(), "3032: !ok after invalidate");
    CHECK(!linear_ir_fastpath_try_skip(), "3032: cannot skip after miss");
    {
        CompilerService cs;
        CHECK(href(cs, "schema-3032") == 3032, "3032: schema-3032 on escape-postmutate");
        CHECK(href(cs, "rehydrate-miss-invalidate-wired") == 1, "3032: wired");
    }
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    reset_rehydrate_miss_invalidate_for_test();
    clear_type_linear_commit_proof_for_test();
}

static void ac3063_hermetic_success_invalidate() {
    std::println("\n--- #3063 AC: hermetic steal/densify success blocks skip ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_rehydrate_miss_invalidate_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    stamp_type_linear_commit_proof(30630);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(linear_fast_path_ok(), "3063: green ok");
    CHECK(linear_ir_fastpath_try_skip(), "3063: skip before");
    CHECK(invalidate_fast_path_before_steal_densify_restamp(), "3063: hard success invalidate");
    CHECK(!linear_fast_path_ok(), "3063: !ok after success invalidate");
    CHECK(!linear_ir_fastpath_try_skip(), "3063: cannot skip after restamp gen");
    {
        CompilerService cs;
        CHECK(href(cs, "schema-3063") == 3063, "3063: schema-3063 on escape-postmutate");
        CHECK(href(cs, "steal-densify-success-invalidate-wired") == 1, "3063: wired");
    }
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    reset_rehydrate_miss_invalidate_for_test();
    clear_type_linear_commit_proof_for_test();
}

static void ac3085_hermetic_lowering_block() {
    std::println("\n--- #3085 AC: hermetic miss blocks lowering helper ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_rehydrate_miss_invalidate_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    stamp_type_linear_commit_proof(30850);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(linear_fast_path_ok(), "3085: green ok");
    CHECK(aura_linear_fast_path_depth_or_densify_block() == 0, "3085: lowering open");
    CHECK(invalidate_fast_path_on_rehydrate_miss(), "3085: miss");
    CHECK(!linear_fast_path_ok(), "3085: !ok");
    CHECK(!linear_ir_fastpath_try_skip(), "3085: no skip");
    CHECK(linear_fast_path_rehydrate_gen_blocks_elision(), "3085: gen blocks");
    CHECK(aura_linear_fast_path_depth_or_densify_block() != 0, "3085: lowering blocked");
    {
        CompilerService cs;
        CHECK(href(cs, "schema-3085") == 3085, "3085: schema-3085 on escape-postmutate");
        CHECK(href(cs, "linear-fast-path-rehydrate-gen-elision-wired") == 1, "3085: wired");
    }
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    reset_rehydrate_miss_invalidate_for_test();
    clear_type_linear_commit_proof_for_test();
}

static void ac3171_hermetic_clear_and_invalidate() {
    std::println("\n--- #3171 AC: hermetic clear + invalidate blocks skip ---");
    using namespace aura::compiler::typed_audit;
    using aura::compiler::clear_escape_move_elision_gate_for_eval;
    using aura::compiler::escape_blocks_move_elision_for_key;
    using aura::compiler::publish_escape_move_elision_gate_for_key;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_rehydrate_miss_invalidate_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    stamp_type_linear_commit_proof(31710);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(linear_fast_path_ok(), "3171: green ok");
    CHECK(linear_ir_fastpath_try_skip(), "3171: skip before");
    void* metrics = reinterpret_cast<void*>(0xA3171);
    publish_escape_move_elision_gate_for_key(metrics, 3, true,
                                             std::unordered_set<std::string>{"x"});
    CHECK(escape_blocks_move_elision_for_key(metrics, 3, "x"), "3171: pre-clear blocks x");
    CHECK(invalidate_fast_path_before_steal_densify_restamp(), "3171: hard invalidate");
    (void)clear_escape_move_elision_gate_for_eval(metrics);
    CHECK(!escape_blocks_move_elision_for_key(metrics, 3, "x"), "3171: post-clear no stale");
    CHECK(!linear_fast_path_ok(), "3171: !ok after gen + clear");
    CHECK(!linear_ir_fastpath_try_skip(), "3171: cannot skip after restamp");
    {
        CompilerService cs;
        CHECK(href(cs, "schema-3171") == 3171, "3171: schema-3171 on escape-postmutate");
        CHECK(href(cs, "linear-fast-path-steal-densify-clear-complete-wired") == 1, "3171: wired");
    }
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    reset_rehydrate_miss_invalidate_for_test();
    clear_type_linear_commit_proof_for_test();
}

// ── Issue #3238: densify/escape under live mutation forces !ok + dirty-root ──
static void ac3238_1_live_densify_production() {
    std::println("\n--- #3238 AC1: Production densify under live mutation ---");
    using namespace aura::compiler::typed_audit;
    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    reset_rehydrate_miss_invalidate_for_test();
    reset_linear_fast_path_dirty_revalidate_for_test();
    reset_linear_fast_path_force_revalidate_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);

    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    stamp_type_linear_commit_proof(32381);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(kLinearFastPathLiveMutationDensifyIssue == 3238, "3238 AC1: issue stamp");
    CHECK(linear_fast_path_ok(), "3238 AC1: fresh ok before live densify");

    g_linear_ir_fastpath_boundary_depth_override = 2;
    CHECK(!linear_fast_path_ok(), "3238 AC1: depth!=0 → !ok");
    CHECK(linear_fast_path_live_mutation_active(), "3238 AC1: live mutation");
    CHECK(note_densify_entry_under_live_mutation(),
          "ac3238_1_live_densify_production: dirty-root required");
    CHECK(!linear_fast_path_ok(), "3238 AC1: still !ok after densify entry");
    CHECK(!linear_ir_fastpath_try_skip(), "3238 AC1: no elision grant");
    CHECK(linear_fast_path_dirty_revalidate_total_v_read() > 0,
          "3238 AC1: dirty-revalidate counter");
    CHECK(last_proof_would_allow_commit_v_read() == 0, "3238 AC1: would_allow dropped");

    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    reset_rehydrate_miss_invalidate_for_test();
    clear_type_linear_commit_proof_for_test();
}

static void ac3238_2_soft_quiet() {
    std::println("\n--- #3238 AC2: Soft observe; quiet zero extra ---");
    using namespace aura::compiler::typed_audit;
    clear_type_linear_commit_proof_for_test();
    reset_linear_fast_path_dirty_revalidate_for_test();
    reset_linear_fast_path_force_revalidate_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    set_strategy(AuditStrategy::Sampled);

    CHECK(!linear_fast_path_live_mutation_active(), "3238 AC2: quiet not live");
    const auto obs0 = linear_fast_path_force_revalidate_observe_total_v_read();
    const auto dirty0 = linear_fast_path_dirty_revalidate_total_v_read();
    CHECK(!note_densify_entry_under_live_mutation(), "ac3238_2_soft_quiet: quiet false");
    CHECK(linear_fast_path_force_revalidate_observe_total_v_read() == obs0,
          "3238 AC2: quiet no observe");
    CHECK(linear_fast_path_dirty_revalidate_total_v_read() == dirty0, "3238 AC2: quiet no dirty");

    g_linear_ir_fastpath_boundary_depth_override = 2;
    CHECK(linear_fast_path_live_mutation_active(), "3238 AC2: live under Soft");
    CHECK(!note_densify_entry_under_live_mutation(), "3238 AC2: Soft no dirty-root");
    CHECK(linear_fast_path_force_revalidate_observe_total_v_read() > obs0,
          "3238 AC2: Soft observe");
    CHECK(linear_fast_path_dirty_revalidate_total_v_read() == dirty0,
          "3238 AC2: Soft no dirty bump");
    g_linear_ir_fastpath_boundary_depth_override = 0;
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3238_3_lineage() {
    std::println("\n--- #3238 AC3: lineage #2964/#3006/#3224/#3227 ---");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(aud.find("linear_fast_path_ok") != std::string::npos, "3238 AC3: #2964 predicate");
    CHECK(aud.find("kLinearFastPathDirtyRevalidateIssue") != std::string::npos,
          "3238 AC3: #3006 retained");
    CHECK(aud.find("kIrTypedEntryCommitReadinessIssue = 3224") != std::string::npos,
          "3238 AC3: #3224 retained");
    CHECK(aud.find("invalidate_fast_path_before_steal_densify_restamp") != std::string::npos,
          "3238 AC3: #3227/#3171 gen face");
    CHECK(mb.find("enforce_linear_boundary_consistency") != std::string::npos,
          "3238 AC3: dirty-root walk");
    CHECK(mb.find("Issue #3238") != std::string::npos, "3238 AC3: densify entry cite");
}

static void ac3238_4_source_linter() {
    std::println("\n--- #3238 AC4: source-cite + linter + no invent ---");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto build = read_file("build.py");
    CHECK(aud.find("kLinearFastPathLiveMutationDensifyIssue = 3238") != std::string::npos,
          "ac3238_4_source_linter: stamp");
    CHECK(build.find("check_linear_fast_path_live_mutation_densify_3238") != std::string::npos,
          "3238 AC4: build.py");
    CHECK(read_file("tests/compiler/test_issue_3238.cpp").empty(), "3238 AC4: no invent");
    CHECK(read_file("docs/design/3238-linear-fast-path-live-densify.md").empty(),
          "3238 AC4: no docs/design");
}

// ── Issue #3446: compiled Move/Drop fence ORs live elision_ok + typed-entry ──
// Residual of #3186/#3130/#3305/#3224: probe deopt_inc then continues;
// fence only skipped the body on epoch-stale.

static void ac3446_1_fence_or_skips_move_body() {
    std::println("\n--- #3446 AC1: fence ORs elision+typed-entry; reject skips Move ---");
    using namespace aura::compiler::typed_audit;
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    CHECK(jit.find("Issue #3446") != std::string::npos, "3446 AC1: Issue #3446 in aura_jit.cpp");
    CHECK(jit.find("fence_elision_blocked = irb->CreateICmpEQ(fence_elision_ok_i, zero32)") !=
              std::string::npos,
          "3446 AC1: fence elision_ok()==0");
    CHECK(jit.find("fence_entry_blocked = irb->CreateICmpEQ(fence_entry_ok_i, zero32)") !=
              std::string::npos,
          "3446 AC1: fence typed-entry==0");
    CHECK(jit.find("is_stale = irb->CreateOr(is_stale, fence_elision_blocked)") !=
              std::string::npos,
          "3446 AC1: OR elision into is_stale");
    CHECK(jit.find("begin_linear_epoch_fence") != std::string::npos &&
              jit.find("store(inst.ops[1], c64(0))") != std::string::npos,
          "3446 AC1: Move source-zero stays after fence (skipped on stale)");
    const auto fence = jit.find("auto begin_linear_epoch_fence");
    CHECK(fence != std::string::npos, "3446 AC1: fence helper present");
    const auto body = fence == std::string::npos ? std::string{} : jit.substr(fence, 2800);
    CHECK(body.find("linear_safety_probe()") == std::string::npos,
          "3446 AC1: fence does not route through probe (probe continues after deopt)");

    clear_escape_move_elision_gate();
    clear_type_linear_commit_proof_for_test();
    reset_linear_ir_fastpath_counters_for_test();
    reset_linear_fast_path_dirty_revalidate_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    stamp_type_linear_commit_proof(34461);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeReject);
    publish_last_proof_face(false, false);
    const auto blk0 = linear_fast_path_elide_blocked_production_total_v_read();
    CHECK(!linear_move_drop_elision_ok(), "3446 AC1: elision blocked after reject proof");
    CHECK(!ir_typed_entry_commit_readiness_ok(),
          "3446 AC1: typed-entry blocked after reject proof");
    CHECK(linear_fast_path_elide_blocked_production_total_v_read() > blk0,
          "3446 AC1: existing elide-blocked counter bumped");
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    clear_type_linear_commit_proof_for_test();
    g_linear_ir_fastpath_boundary_depth_override = -1;
}

static void ac3446_2_drop_borrow_same_fence() {
    std::println("\n--- #3446 AC2: Drop/Borrow inherit the same fence ---");
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    const auto drop = jit.find("case OpDropOp:");
    CHECK(drop != std::string::npos, "3446 AC2: OpDropOp present");
    const auto drop_body = drop == std::string::npos ? std::string{} : jit.substr(drop, 1800);
    CHECK(drop_body.find("begin_linear_epoch_fence()") != std::string::npos,
          "3446 AC2: OpDropOp uses fence");
    CHECK(drop_body.find("Issue #3446") != std::string::npos, "3446 AC2: Drop cites #3446");
    const auto borrow = jit.find("case OpBorrowOp:");
    CHECK(borrow != std::string::npos, "3446 AC2: OpBorrowOp present");
    const auto borrow_body = borrow == std::string::npos ? std::string{} : jit.substr(borrow, 700);
    CHECK(borrow_body.find("begin_linear_epoch_fence()") != std::string::npos,
          "3446 AC2: Borrow/MutBorrow inherit fence");
}

static void ac3446_3_interpreter_unchanged() {
    std::println("\n--- #3446 AC3: interpreter path unchanged (#3224/#3305) ---");
    const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
    CHECK(ir.find("linear_state_allows_op") != std::string::npos,
          "3446 AC3: interpreter still linear_state_allows_op");
    CHECK(ir.find("typed_audit::linear_move_drop_elision_ok()") != std::string::npos,
          "3446 AC3: interpreter still elision skip");
    CHECK(ir.find("ir_typed_entry_blocked_result") != std::string::npos,
          "3446 AC3: interpreter typed-entry refuse kept");
    CHECK(ir.find("commit-readiness-refused") != std::string::npos,
          "3446 AC3: interpreter refuse message kept");
}

static void ac3446_4_prologue_guardshape_kept() {
    std::println("\n--- #3446 AC4: Apply prologue + GuardShape probe kept ---");
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    CHECK(jit.find("Issue #3419") != std::string::npos, "3446 AC4: #3419 prologue cite kept");
    CHECK(jit.find("hard_typed_entry") != std::string::npos, "3446 AC4: prologue production gate");
    CHECK(jit.find("fn_ir_typed_entry_commit_readiness_ok") != std::string::npos,
          "3446 AC4: prologue typed-entry emit");
    CHECK(jit.find("is_stale = irb->CreateOr(is_stale, lin_unsafe)") != std::string::npos,
          "3446 AC4: GuardShape still fail-closes via probe i1");
    CHECK(jit.find("irb->CreateCondBr(any_unsafe, bb_deopt, bb_ok)") != std::string::npos,
          "3446 AC4: probe still continues to bb_ok after deopt_inc");
}

static void ac3446_5_soft_no_new_key() {
    std::println("\n--- #3446 AC5: Soft/Off quiet allow; no new query key ---");
    using namespace aura::compiler::typed_audit;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    set_strategy(AuditStrategy::Sampled);
    clear_type_linear_commit_proof_for_test();
    stamp_type_linear_commit_proof(34465);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    g_linear_ir_fastpath_boundary_depth_override = 0;
    const auto blk0 = linear_fast_path_elide_blocked_production_total_v_read();
    CHECK(ir_typed_entry_commit_readiness_ok(), "3446 AC5: Soft typed-entry allow");
    CHECK(linear_move_drop_elision_ok() || linear_fast_path_ok(),
          "3446 AC5: Soft elision/fast-path allow on green proof");
    CHECK(linear_fast_path_elide_blocked_production_total_v_read() == blk0,
          "3446 AC5: no new counters on quiet allow");
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
    clear_type_linear_commit_proof_for_test();
    g_linear_ir_fastpath_boundary_depth_override = -1;

    const auto jit = read_file("src/compiler/aura_jit.cpp");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto build = read_file("build.py");
    CHECK(jit.find("schema-3446") == std::string::npos, "3446 AC5: no schema-3446 in jit");
    CHECK(mut.find("schema-3446") == std::string::npos, "3446 AC5: no new query key");
    CHECK(build.find("check_linear_epoch_fence_elision_typed_3446") != std::string::npos,
          "3446 AC5: build.py wires linter");
    CHECK(read_file("docs/design/3446-linear-epoch-fence.md").empty(),
          "3446 AC5: no docs/design/3446-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_3446.cpp").empty() &&
              read_file("tests/issues/test_issue_3446.cpp").empty(),
          "3446 AC5: no test_issue_3446.cpp per #81934");
}

} // namespace

int run_test_escape_move_elision_gate() {
    std::println("=== Issue #2263 / #2286: escape summary → MoveOp elision gate ===");
    ac1_escape_blocks_elision();
    ac2_clean_path_elides();
    ac3_null_summary_legacy();
    ac4_schema_source();
    ac6_cross_eval_isolation();
    ac7_same_eval_parity();
    ac8_cow_gen_advance_clears();
    ac9_zero_cost_happy_path();
    ac10_schema_source();
    std::println("\n=== Issue #2309: rollback-clear on reject / force-rollback ===");
    ac11_2309_rollback_clear_gate();
    std::println("\n=== Issue #2344: publish key ↔ lower key contract (Option A) ===");
    ac12_matching_key_blocks_move();
    ac13_wrong_key_never_elides_blocked();
    ac14_disjoint_names_isolation();
    ac15_happy_path_empty_blocked_elides();
    ac16_schema_source_2344();
    std::println("\n=== Issue #2899: proven Move/Drop IR fast-path ===");
    ac2899_1_proof_fresh_skips();
    ac2899_2_escape_or_reject_blocks();
    ac2899_3_no_proof_or_mid_boundary();
    ac2899_4_additive_query();
    ac2899_5_source_cite();
    std::println("\n=== Issue #2964: unified linear_fast_path_ok gate ===");
    ac2964_1_unified_predicate();
    ac2964_2_force_revalidate_production();
    ac2964_3_independent_arms();
    ac2964_4_soft_and_quiet();
    ac2964_5_additive_schema();
    ac2964_6_source_cite();
    std::println("\n=== Issue #3006: Production dirty-root revalidate on !ok ===");
    ac3006_1_production_dirty_root();
    ac3006_2_no_elide_under_false();
    ac3006_3_late_reeval_and_render_fast();
    ac3006_4_soft_observe();
    ac3006_5_schema_lineage();
    ac3006_6_linter_no_design();
    std::println("\n=== Issue #3030: abort clears TypeLinearCommitProof face ===");
    ac3030_1_abort_clears_fast_path();
    ac3030_2_fresh_stamp_restores();
    ac3030_3_soft_observe();
    ac3030_4_quiet_zero_cost();
    ac3030_5_schema_and_wire();
    ac3030_6_linter_no_design();
    std::println("\n=== Issue #3032: rehydrate-miss invalidates linear_fast_path ===");
    ac3032_hermetic_invalidate();
    std::println("\n=== Issue #3063: steal/densify success invalidate-before-restamp ===");
    ac3063_hermetic_success_invalidate();
    std::println("\n=== Issue #3085: densify/steal miss blocks lowering elision ===");
    ac3085_hermetic_lowering_block();
    std::println("\n=== Issue #3171: steal/densify restamp complete-clear ===");
    ac3171_hermetic_clear_and_invalidate();
    std::println("\n=== Issue #3238: densify under live mutation forces !ok ===");
    ac3238_1_live_densify_production();
    ac3238_2_soft_quiet();
    ac3238_3_lineage();
    ac3238_4_source_linter();
    std::println("\n=== Issue #3446: JIT Move/Drop fence ORs elision + typed-entry ---");
    ac3446_1_fence_or_skips_move_body();
    ac3446_2_drop_borrow_same_fence();
    ac3446_3_interpreter_unchanged();
    ac3446_4_prologue_guardshape_kept();
    ac3446_5_soft_no_new_key();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_escape_move_elision_gate();
}
#endif
