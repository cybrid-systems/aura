// @category: unit
// @reason: Issue #2507 — clear OwnershipEscapeSummary / MoveOp elision
//          gate on steal-complete + Moving densify success (no stale elision).
//
//   AC1: Publish escape block under key K → clear_for_eval → re-lower →
//        MoveOp emitted (no stale elision) unless re-published
//   AC2: Densify-clear path invalidates prior key (clear_for_eval densify)
//   AC3: Happy path clear with empty map → zero erases (soft free)
//   AC4: Cross-eval isolation — clear eval A does not wipe eval B
//   AC5: Source-cite + schema-2507 + steal-complete wiring

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
using aura::compiler::clear_current_escape_key;
using aura::compiler::clear_escape_move_elision_gate_for_eval;
using aura::compiler::clear_escape_move_elision_gate_for_key;
using aura::compiler::CompilerService;
using aura::compiler::escape_blocks_move_elision_for_key;
using aura::compiler::g_linear_escape_gate_densify_clear_entries_total;
using aura::compiler::g_linear_escape_gate_densify_clear_total;
using aura::compiler::g_linear_escape_gate_steal_clear_entries_total;
using aura::compiler::g_linear_escape_gate_steal_clear_total;
using aura::compiler::g_linear_move_elision_blocked_escape_total;
using aura::compiler::linear_move_elided_total;
using aura::compiler::lower_to_ir;
using aura::compiler::note_escape_gate_clear_on_densify;
using aura::compiler::note_escape_gate_clear_on_steal;
using aura::compiler::publish_escape_move_elision_gate_for_key;
using aura::compiler::set_current_escape_key;
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
        "(hash-ref (engine:metrics \"query:ownership-escape-postmutate-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void make_move_var_flat(FlatAST& flat, StringPool& pool, const char* name) {
    auto sym = pool.intern(name);
    auto var = flat.add_variable(sym);
    auto mv = flat.add_move(var);
    flat.root = mv;
}

static std::size_t count_move_ops(const aura::ir::IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& ins : b.instructions)
                if (ins.opcode == aura::ir::IROpcode::MoveOp)
                    ++n;
    return n;
}

// Lower (move x) under current escape key; return MoveOp count.
static std::size_t lower_move_count(void* eval, std::uint64_t cow_gen, const char* name) {
    set_current_escape_key(eval, cow_gen);
    aura::ast::ASTArena arena;
    StringPool pool;
    FlatAST flat;
    make_move_var_flat(flat, pool, name);
    auto mod = lower_to_ir(flat, pool, arena);
    clear_current_escape_key();
    return count_move_ops(mod);
}

// ── AC1: steal-clear invalidates elision (MoveOp re-emitted) ──
static void ac1_steal_clear_no_stale_elision() {
    std::println("\n--- #2507 AC1: clear_for_eval → no stale MoveOp elision ---");
    void* metrics_a = reinterpret_cast<void*>(0xA2507);
    const std::uint64_t gen = 42;

    // Publish escape-block for "x" under key (A, 42).
    publish_escape_move_elision_gate_for_key(metrics_a, gen, true,
                                             std::unordered_set<std::string>{"x"});
    CHECK(escape_blocks_move_elision_for_key(metrics_a, gen, "x"), "AC1: pre-clear blocks x");

    // Under active gate with blocked x → MoveOp emitted (not elided).
    const auto moves_blocked = lower_move_count(metrics_a, gen, "x");
    CHECK(moves_blocked >= 1, "AC1: blocked path emits MoveOp");

    // Publish *clean* active gate (empty blocked) → would elide.
    publish_escape_move_elision_gate_for_key(metrics_a, gen, true, {});
    const auto elided0 = linear_move_elided_total();
    const auto moves_clean = lower_move_count(metrics_a, gen, "x");
    // Active + empty blocked → elide MoveOp (legacy #2263 clean path).
    CHECK(moves_clean == 0 || linear_move_elided_total() > elided0,
          "AC1: clean active gate elides (or elided counter advanced)");

    // Steal-complete equivalent: clear all keys for metrics_a.
    const auto steal0 = g_linear_escape_gate_steal_clear_total.load();
    const auto entries0 = g_linear_escape_gate_steal_clear_entries_total.load();
    note_escape_gate_clear_on_steal(metrics_a);
    CHECK(g_linear_escape_gate_steal_clear_total.load() == steal0 + 1, "AC1: steal-clear path +1");
    CHECK(g_linear_escape_gate_steal_clear_entries_total.load() >= entries0 + 1,
          "AC1: at least one entry erased");
    CHECK(!escape_blocks_move_elision_for_key(metrics_a, gen, "x"),
          "AC1: post-clear key miss (no stale block)");

    // After clear, gate inactive for key → legacy always emit MoveOp (no elide).
    const auto moves_after = lower_move_count(metrics_a, gen, "x");
    CHECK(moves_after >= 1, "AC1: after clear, MoveOp emitted (no stale elision)");
}

// ── AC2: densify-clear path ──
static void ac2_densify_clear() {
    std::println("\n--- #2507 AC2: densify clear invalidates prior key ---");
    void* metrics = reinterpret_cast<void*>(0xB2507);
    const std::uint64_t gen = 7;
    publish_escape_move_elision_gate_for_key(metrics, gen, true,
                                             std::unordered_set<std::string>{"y"});
    CHECK(escape_blocks_move_elision_for_key(metrics, gen, "y"), "AC2: pre densify block");

    // Also publish under a second gen for same eval — both must clear.
    publish_escape_move_elision_gate_for_key(metrics, gen + 1, true,
                                             std::unordered_set<std::string>{"z"});

    const auto d0 = g_linear_escape_gate_densify_clear_total.load();
    const auto e0 = g_linear_escape_gate_densify_clear_entries_total.load();
    note_escape_gate_clear_on_densify(metrics);
    CHECK(g_linear_escape_gate_densify_clear_total.load() == d0 + 1, "AC2: densify-clear +1");
    CHECK(g_linear_escape_gate_densify_clear_entries_total.load() >= e0 + 2,
          "AC2: both gens erased");
    CHECK(!escape_blocks_move_elision_for_key(metrics, gen, "y"), "AC2: gen cleared");
    CHECK(!escape_blocks_move_elision_for_key(metrics, gen + 1, "z"), "AC2: gen+1 cleared");

    // Next lower does not elide from pre-densify summary (inactive → emit).
    const auto moves = lower_move_count(metrics, gen, "y");
    CHECK(moves >= 1, "AC2: post-densify-clear MoveOp emitted");
}

// ── AC3: empty map soft free ──
static void ac3_soft_empty() {
    std::println("\n--- #2507 AC3: empty map clear → zero erases ---");
    void* metrics = reinterpret_cast<void*>(0xC2507);
    // Ensure no entries for this metrics.
    (void)clear_escape_move_elision_gate_for_eval(metrics);
    const auto n = clear_escape_move_elision_gate_for_eval(metrics);
    CHECK(n == 0, "AC3: second clear erases 0");
    const auto steal0 = g_linear_escape_gate_steal_clear_total.load();
    const auto entries0 = g_linear_escape_gate_steal_clear_entries_total.load();
    note_escape_gate_clear_on_steal(metrics);
    CHECK(g_linear_escape_gate_steal_clear_total.load() == steal0 + 1,
          "AC3: path counter still advances");
    CHECK(g_linear_escape_gate_steal_clear_entries_total.load() == entries0,
          "AC3: entries counter flat when empty");
}

// ── AC4: cross-eval isolation ──
static void ac4_cross_eval_isolation() {
    std::println("\n--- #2507 AC4: clear A does not wipe B ---");
    void* a = reinterpret_cast<void*>(0xD2507A);
    void* b = reinterpret_cast<void*>(0xD2507B);
    publish_escape_move_elision_gate_for_key(a, 1, true, std::unordered_set<std::string>{"xa"});
    publish_escape_move_elision_gate_for_key(b, 1, true, std::unordered_set<std::string>{"xb"});
    CHECK(escape_blocks_move_elision_for_key(a, 1, "xa"), "AC4: A blocks xa");
    CHECK(escape_blocks_move_elision_for_key(b, 1, "xb"), "AC4: B blocks xb");

    note_escape_gate_clear_on_steal(a);
    CHECK(!escape_blocks_move_elision_for_key(a, 1, "xa"), "AC4: A cleared");
    CHECK(escape_blocks_move_elision_for_key(b, 1, "xb"), "AC4: B intact after A clear");
    note_escape_gate_clear_on_densify(b);
    CHECK(!escape_blocks_move_elision_for_key(b, 1, "xb"), "AC4: B cleared by densify path");
}

// ── AC5: source-cite + query schema ──
static void ac5_source_and_query() {
    std::println("\n--- #2507 AC5: source-cite + schema-2507 ---");
    const auto hh = read_file("src/compiler/ownership_escape_lowering_gate.h");
    const auto hooks = read_file("src/compiler/typed_mutation_audit_hooks.cpp");
    const auto fiber = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto cmake = read_file("CMakeLists.txt");

    CHECK(hh.find("aura_escape_move_gate_clear_eval") != std::string::npos,
          "AC5: clear_eval C API");
    CHECK(hh.find("note_escape_gate_clear_on_steal") != std::string::npos, "AC5: steal helper");
    CHECK(hh.find("note_escape_gate_clear_on_densify") != std::string::npos, "AC5: densify helper");
    CHECK(hh.find("2507") != std::string::npos, "AC5: #2507 in header");
    CHECK(hooks.find("aura_escape_move_gate_clear_eval") != std::string::npos,
          "AC5: clear_eval impl");
    CHECK(hooks.find("g_linear_escape_gate_steal_clear_total") != std::string::npos,
          "AC5: steal counter def");
    CHECK(fiber.find("note_escape_gate_clear_on_steal") != std::string::npos,
          "AC5: steal-complete wires clear");
    CHECK(fiber.find("2507") != std::string::npos, "AC5: #2507 in fiber mutation");
    CHECK(mb.find("note_escape_gate_clear_on_densify") != std::string::npos,
          "AC5: densify success wires clear");
    CHECK(mb.find("had_moving_densify") != std::string::npos &&
              mb.find("note_escape_gate_clear_on_densify") != std::string::npos,
          "AC5: densify clear gated on Moving");
    CHECK(obs.find("schema-2507") != std::string::npos, "AC5: schema-2507 query");
    CHECK(obs.find("linear-escape-gate-steal-clear-total") != std::string::npos,
          "AC5: steal-clear query key");
    CHECK(cmake.find("test_escape_gate_steal_densify_clear") != std::string::npos,
          "AC5: cmake target");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC5: warm");
    CHECK(href(cs, "schema-2507") == 2507, "AC5: schema-2507 live");
    CHECK(href(cs, "issue-2507") == 2507, "AC5: issue-2507");
    CHECK(href(cs, "linear-escape-gate-steal-densify-clear-wired") == 1, "AC5: wired");
    CHECK(href(cs, "linear-escape-gate-steal-clear-total") >= 0, "AC5: steal-clear total");
    CHECK(href(cs, "linear-escape-gate-densify-clear-total") >= 0, "AC5: densify-clear total");
}

} // namespace

int run_test_escape_gate_steal_densify_clear() {
    std::println("test_escape_gate_steal_densify_clear");
    ac1_steal_clear_no_stale_elision();
    ac2_densify_clear();
    ac3_soft_empty();
    ac4_cross_eval_isolation();
    ac5_source_and_query();
    if (g_failed)
        return 1;
    std::println("escape gate steal/densify clear #2507: OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_escape_gate_steal_densify_clear();
}
#endif
