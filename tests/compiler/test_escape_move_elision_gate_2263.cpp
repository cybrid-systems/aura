// @category: unit
// @reason: Issue #2263 — OwnershipEscapeSummary feeds lowering MoveOp elision.
//
//   AC1: escape-after-move binding → MoveOp emitted; blocked counter bumps
//   AC2: clean owned path under active summary still elides
//   AC3: no escape summary → legacy always emit MoveOp (no elide)
//   AC4: schema-2263 + source-cite

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

int main() {
    std::println("=== Issue #2263: escape summary → MoveOp elision gate ===");
    ac1_escape_blocks_elision();
    ac2_clean_path_elides();
    ac3_null_summary_legacy();
    ac4_schema_source();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
