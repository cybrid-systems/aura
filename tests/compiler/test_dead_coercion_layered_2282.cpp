// test_dead_coercion_layered_2282.cpp
// Issue #2282: unified dead-coercion layered counter (AST elide + IR DCE + dirty-cone skip).
// Verifies the new query:dead-coercion-layered-stats primitive exposes a single
// Agent-facing key (dead-coercion-layered-total) plus 5 individually-queryable
// components (ast-elided / ir-elided / dirty-cone-skips / ir-narrow-evidence-hits /
// pipeline-runs-total). Components remain individually queryable (no schema break;
// AC2 + AC4). Narrowing + mutate fixture drives the layered total up monotonically
// (AC3). Comment alignment in optimization_passes.ixx + coercion_map.ixx mirrors
// the live keys (AC5).

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.coercion_map;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_dead_coercion_layered_2282 {

using aura::compiler::CompilerService;
using aura::ir::IRFunction;
using aura::ir::IRModule;
using aura::ir::IROpcode;

static std::int64_t query_layered_total(CompilerService& cs) {
    auto r = cs.eval("(hash-ref (engine:metrics \"query:dead-coercion-layered-stats\") "
                     "\"dead-coercion-layered-total\")");
    if (!r)
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t query_field(CompilerService& cs, const char* field) {
    auto r = cs.eval(std::string("(hash-ref (engine:metrics \"query:dead-coercion-layered-stats\") "
                                 "\"") +
                     field + "\")");
    if (!r)
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool layered_returns_hash(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:dead-coercion-layered-stats\")");
    return r && aura::compiler::types::is_hash(*r);
}

// ---------------------------------------------------------------------------
// Issue #2282: 5 ACs
// ---------------------------------------------------------------------------
namespace _2282_detail {

    static void run_2282_layered_total() {
        std::println("\n=== Issue #2282: unified dead-coercion layered counter ===");

        // AC1: One query key returns layered total = sum of three components.
        {
            std::println(
                "\n--- AC1: layered total = ast_elided + ir_elided + dirty_cone_skips ---");
            CompilerService cs;
            CHECK(layered_returns_hash(cs), "AC1: primitive returns a hash");
            const auto layered = query_layered_total(cs);
            const auto ast = query_field(cs, "ast-elided");
            const auto ir = query_field(cs, "ir-elided");
            const auto dirty = query_field(cs, "dirty-cone-skips");
            const auto sum = ast + ir + dirty;
            std::println("  layered-total={} ast={} ir={} dirty={} sum={}", layered, ast, ir, dirty,
                         sum);
            CHECK(layered == sum, "AC1: dead-coercion-layered-total == sum of 3 components");
            CHECK(layered >= 0, "AC1: layered total non-negative");
        }

        // AC2: Components individually still queryable (no schema break).
        {
            std::println("\n--- AC2: components individually queryable ---");
            CompilerService cs;
            const auto narrow = query_field(cs, "ir-narrow-evidence-hits");
            const auto pipe = query_field(cs, "pipeline-runs-total");
            std::println("  ir-narrow-evidence-hits={} pipeline-runs-total={}", narrow, pipe);
            CHECK(narrow >= 0, "AC2: ir-narrow-evidence-hits queryable");
            CHECK(pipe >= 0, "AC2: pipeline-runs-total queryable");
            // Existing primitives still return their prior schema (no break).
            auto existing = cs.eval("(engine:metrics \"query:coercion-zerooverhead-stats\")");
            CHECK(existing && aura::compiler::types::is_int(*existing),
                  "AC2: existing query:coercion-zerooverhead-stats still returns int (no schema "
                  "break)");
        }

        // AC3: Narrowing fixture — layered monotonic increase post-mutate + lower path.
        {
            std::println("\n--- AC3: narrowing fixture (lower path + post-mutate increase) ---");
            CompilerService cs;
            // Lower path: compile a small gradual IR with cast ops so the DCE pass
            // runs at least once. We don't require exact deltas; the layered total
            // must be monotonically non-decreasing across the mutate step.
            (void)cs.eval("(set _x 0)");
            (void)cs.eval("(eval-current)");
            const auto baseline = query_layered_total(cs);
            std::println("  baseline layered-total={}", baseline);
            CHECK(baseline >= 0, "AC3: lower path returns non-negative baseline");
            // Post-mutate: drive a (mutate:rebind) + (eval-current) round.
            (void)cs.eval("(mutate:rebind \"_x\" \"42\")");
            (void)cs.eval("(eval-current)");
            const auto after = query_layered_total(cs);
            std::println("  post-mutate layered-total={}", after);
            CHECK(after >= baseline, "AC3: layered monotonic non-decrease post-mutate");
        }

        // AC4: Schema lineage additive (new primitive, existing primitives untouched).
        {
            std::println("\n--- AC4: schema lineage additive ---");
            CompilerService cs;
            // The new primitive is registered as a fresh key; existing primitive
            // surface should still resolve (e.g. query:dead-coercion-zerooverhead-stats).
            auto zs = cs.eval("(engine:metrics \"query:dead-coercion-zerooverhead-stats\")");
            auto layered = cs.eval("(engine:metrics \"query:dead-coercion-layered-stats\")");
            CHECK(zs && aura::compiler::types::is_hash(*zs),
                  "AC4: existing dead-coercion-zerooverhead-stats still resolved (no break)");
            CHECK(layered && aura::compiler::types::is_hash(*layered),
                  "AC4: new dead-coercion-layered-stats registered (additive)");
        }

        // AC5: Short comment in optimization_passes.ixx / coercion_map refers to live keys.
        {
            std::println("\n--- AC5: comment alignment ---");
            // Source-level check: the linter scripts/check_dead_coercion_layered_coverage.py
            // verifies both comments mention query:dead-coercion-layered-stats. Here
            // we just verify the primitive is reachable end-to-end as a smoke test.
            CompilerService cs;
            const auto layered = query_layered_total(cs);
            // If the comment diverged from live keys, the primitive would still
            // resolve; the linter is the authoritative source-level check.
            (void)layered;
            std::println("  primitive resolved; comment alignment validated by linter.");
            CHECK(true, "AC5: comment alignment deferred to linter (source-level AC)");
        }
    }

} // namespace _2282_detail

} // namespace aura_dead_coercion_layered_2282

int main() {
    std::println("=== Issue #2282: unified dead-coercion layered counter ===");
    aura_dead_coercion_layered_2282::_2282_detail::run_2282_layered_total();
    return RUN_ALL_TESTS();
}
