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

#include "test_harness.hpp"
#include "compiler/ownership_escape_lowering_gate.h"

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

using aura::compiler::clear_escape_move_elision_gate_for_key;
using aura::compiler::escape_blocks_move_elision_for_key;
using aura::compiler::g_linear_escape_gate_cross_eval_miss_total;
using aura::compiler::publish_escape_move_elision_gate_for_key;

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
    std::println("\n--- AC8: Cow-gen advance clears/ignores stale summary ---");
    FakeEval eval;
    publish_escape_move_elision_gate_for_key(&eval, 10, true, std::unordered_set<std::string>{"x"});
    CHECK(escape_blocks_move_elision_for_key(&eval, 10, "x"), "gen=10 blocks x");
    // Advance gen → stale summary should not match (lookup for new gen misses).
    CHECK(!escape_blocks_move_elision_for_key(&eval, 11, "x"),
          "gen=11 stale → safe default (no block)");
    // Original gen still works until explicitly cleared.
    CHECK(escape_blocks_move_elision_for_key(&eval, 10, "x"),
          "gen=10 still blocks x (per-key storage)");
    clear_escape_move_elision_gate_for_key(&eval, 10);
    CHECK(!escape_blocks_move_elision_for_key(&eval, 10, "x"), "gen=10 cleared");
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

} // namespace

int main() {
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
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
