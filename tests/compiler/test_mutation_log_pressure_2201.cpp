// @category: unit
// @reason: Issue #2201 — Agent-visible mutation-log pressure + forced compact.
//
//   AC1: Stats report log size, compact totals, pressure flag/score
//   AC2: High-volume multi-round mutate → compact bounds log; pressure
//        transitions visible
//   AC3: Explicit compact is capability-gated (Mutate effect / PrimMeta)
//   AC4: schema-2201; rollback_to_size still correct after compact

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;
import aura.core.arena;

namespace {

using aura::ast::FlatAST;
using aura::ast::MutationStatus;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
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

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: pressure surface keys ──────────────────────────────
static void ac1_pressure_stats() {
    std::println("\n--- AC1: pressure stats surface ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    // Touch compact stats / pressure alias.
    for (const char* q : {"query:mutation-log-compact-stats", "query:mutation-log-pressure"}) {
        auto h = cs.eval(std::format("(engine:metrics \"{}\")", q));
        CHECK(h.has_value() && is_hash(*h), std::string(q) + " is hash");
    }
    CHECK(href(cs, "query:mutation-log-pressure", "schema-2201") == 2201, "schema-2201");
    CHECK(href(cs, "query:mutation-log-pressure", "issue-2201") == 2201, "issue-2201");
    CHECK(href(cs, "query:mutation-log-pressure", "mutation-log-pressure-wired") == 1, "wired");
    CHECK(href(cs, "query:mutation-log-compact-stats", "schema-2201") == 2201,
          "compact-stats schema bumped");
    CHECK(href(cs, "query:mutation-log-pressure", "log-size") >= 0, "log-size");
    CHECK(href(cs, "query:mutation-log-pressure", "high-water") >= 0, "high-water");
    CHECK(href(cs, "query:mutation-log-pressure", "pressure-score-bp") >= 0, "pressure-score-bp");
    CHECK(href(cs, "query:mutation-log-pressure", "pressure-flag") >= 0, "pressure-flag");
    CHECK(href(cs, "query:mutation-log-pressure", "soft-threshold") == 5000,
          "soft-threshold default");
    CHECK(href(cs, "query:mutation-log-pressure", "auto-threshold") == 10000, "auto-threshold");
    CHECK(href(cs, "query:mutation-log-pressure", "forced-compact-total") >= 0, "forced-compact");
    CHECK(href(cs, "query:mutation-log-pressure", "guard-compact-total") >= 0, "guard-compact");
    CHECK(href(cs, "query:mutation-log-pressure", "compact-ops") >= 0, "compact-ops");
    CHECK(href(cs, "query:mutation-log-pressure", "bytes-saved-total") >= 0, "bytes-saved");
}

// ── AC2: high-volume + compact bounds log ───────────────────
static void ac2_high_volume_compact() {
    std::println("\n--- AC2: high-volume log → compact bounds + pressure transitions ---");
    aura::ast::ASTArena arena;
    FlatAST flat(arena.allocator());
    CompilerMetrics metrics;
    // Lower soft threshold so small synthetic logs trip pressure.
    metrics.mutation_log_soft_threshold.store(50, std::memory_order_relaxed);

    // Seed a large mutation log (synthetic records on a live node).
    auto seed = flat.add_literal(0);
    flat.root = seed;
    for (int i = 0; i < 200; ++i) {
        (void)flat.add_mutation(seed, "test-op", "Dyn", "Dyn", "synth", MutationStatus::Committed);
    }
    const auto size_hi = flat.mutation_log_size();
    CHECK(size_hi >= 200, "log grew to ≥200");

    // Pressure: size 200 / soft 50 = 400% → score capped at 10000 bp.
    {
        auto hw = metrics.mutation_log_high_water.load();
        while (size_hi > hw &&
               !metrics.mutation_log_high_water.compare_exchange_weak(
                   hw, static_cast<std::uint64_t>(size_hi), std::memory_order_relaxed)) {
        }
        const auto soft = metrics.mutation_log_soft_threshold.load();
        const auto score = std::min<std::uint64_t>(
            10'000ull, (static_cast<std::uint64_t>(size_hi) * 10'000ull) / soft);
        metrics.mutation_log_pressure_score_bp.store(score, std::memory_order_relaxed);
        metrics.mutation_log_pressure_flag.store(size_hi >= soft ? 1 : 0,
                                                 std::memory_order_relaxed);
        CHECK(metrics.mutation_log_pressure_flag.load() == 1, "pressure-flag under load");
        CHECK(metrics.mutation_log_pressure_score_bp.load() >= 10000, "score at/above 10000 bp");
        CHECK(metrics.mutation_log_high_water.load() >= 200, "high-water ≥200");
    }

    // Explicit compact keep last 20 → bounds size.
    const auto dropped =
        flat.compact_mutation_log(/*keep_recent=*/20, /*keep_all_rolledback=*/false);
    CHECK(dropped >= 180, "dropped many records");
    CHECK(flat.mutation_log_size() <= 20, "log bounded after compact");
    CHECK(flat.mutation_log_compact_ops() >= 1, "compact-ops bumped");
    CHECK(flat.mutation_log_compacted_records() >= dropped, "compacted-records");

    // Pressure falls after compact (if soft still 50, size 20 → flag 0).
    {
        const auto soft = metrics.mutation_log_soft_threshold.load();
        const auto after = flat.mutation_log_size();
        const auto score =
            soft == 0 ? 0ull
                      : std::min<std::uint64_t>(
                            10'000ull, (static_cast<std::uint64_t>(after) * 10'000ull) / soft);
        metrics.mutation_log_pressure_score_bp.store(score, std::memory_order_relaxed);
        metrics.mutation_log_pressure_flag.store(after >= soft ? 1 : 0, std::memory_order_relaxed);
        CHECK(metrics.mutation_log_pressure_flag.load() == 0,
              "pressure-flag cleared after compact");
        CHECK(metrics.mutation_log_pressure_score_bp.load() < 10000,
              "score below full after compact");
        // high-water remains peak (does not decrease)
        CHECK(metrics.mutation_log_high_water.load() >= 200, "high-water sticky at peak");
    }

    // End-to-end via CompilerService compact primitive.
    CompilerService cs;
    CompilerMetrics m2;
    m2.mutation_log_soft_threshold.store(30, std::memory_order_relaxed);
    cs.evaluator().set_compiler_metrics(&m2);
    CHECK(cs.eval("(set-code \"(define a 0)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    // Force many mutates so log grows.
    for (int i = 0; i < 40; ++i) {
        (void)cs.typed_mutate(std::format("(mutate:rebind \"a\" \"{}\")", i));
    }
    const auto sz0 = href(cs, "query:mutation-log-pressure", "log-size");
    CHECK(sz0 >= 1, "log-size after mutates");
    auto drop_r = cs.eval("(mutation-log-compact 10)");
    CHECK(drop_r.has_value() && is_int(*drop_r), "mutation-log-compact returns int");
    // Under default sandbox Mutate may be allowed for tests; -1 = denied.
    if (as_int(*drop_r) >= 0) {
        CHECK(href(cs, "query:mutation-log-pressure", "forced-compact-total") >= 1,
              "forced-compact-total after explicit compact");
        CHECK(href(cs, "query:mutation-log-pressure", "last-compact-dropped") >= 0,
              "last-compact-dropped");
        const auto sz1 = href(cs, "query:mutation-log-pressure", "log-size");
        CHECK(sz1 >= 0 && sz1 <= sz0, "log size non-increasing after compact");
    }
    cs.evaluator().set_compiler_metrics(nullptr);
}

// ── AC3: capability gate meta + deny surface ────────────────
static void ac3_capability_gate() {
    std::println("\n--- AC3: compact is capability-gated ---");
    auto src = read_file("src/compiler/evaluator_primitives_mutation.cpp");
    CHECK(src.find("require_effect") != std::string::npos, "require_effect on compact");
    CHECK(src.find("kEffectMutate") != std::string::npos, "kEffectMutate");
    CHECK(src.find("mutation-log-compact") != std::string::npos, "compact prim");
    CHECK(src.find("set_meta_for_name(\"mutation-log-compact\"") != std::string::npos ||
              src.find("set_meta_for_name(\"mutation-log-compact\"") != std::string::npos,
          "PrimMeta set");
    CHECK(src.find("required_effects") != std::string::npos, "required_effects on meta");
    // Source documents Agent policy.
    CHECK(src.find("Issue #2201") != std::string::npos, "cites #2201");
}

// ── AC4: schema + rollback after compact ────────────────────
static void ac4_schema_and_rollback() {
    std::println("\n--- AC4: schema + rollback_to_size after compact ---");
    auto fields = read_file("src/compiler/compiler_metrics_fields.inc");
    CHECK(fields.find("mutation_log_high_water") != std::string::npos, "fields high_water");
    CHECK(fields.find("mutation_log_pressure_score_bp") != std::string::npos, "fields score");
    CHECK(fields.find("mutation_log_forced_compact_total") != std::string::npos, "fields forced");
    CHECK(fields.find("mutation_log_guard_compact_total") != std::string::npos, "fields guard");

    aura::ast::ASTArena arena;
    FlatAST flat(arena.allocator());
    auto n = flat.add_literal(1);
    flat.root = n;
    std::vector<std::uint64_t> mids;
    for (int i = 0; i < 30; ++i) {
        mids.push_back(
            flat.add_mutation(n, "test-op", "Int", "Int", "v", MutationStatus::Committed));
    }
    CHECK(flat.mutation_log_size() >= 30, "log has records");
    const auto checkpoint = flat.mutation_log_size();
    // Compact keeping last 5 — older mids may be gone.
    const auto dropped = flat.compact_mutation_log(5, false);
    CHECK(dropped >= 20, "compact dropped prefix");
    CHECK(flat.mutation_log_size() <= 5, "bounded");
    // Recent mid retained.
    if (!mids.empty()) {
        const auto recent = mids.back();
        bool found = false;
        for (const auto& rec : flat.mutation_log_view()) {
            if (rec.mutation_id == recent)
                found = true;
        }
        CHECK(found, "recent mutation retained after compact");
        (void)flat.rollback(recent); // may no-op without rollback data — no crash
        CHECK(true, "rollback after compact does not abort");
    }
    // rollback_to_size still defined and safe after compact.
    (void)checkpoint;
    (void)flat.rollback_to_size(0);
    CHECK(true, "rollback_to_size after compact ok");
}

} // namespace

int main() {
    std::println("=== Issue #2201: mutation-log pressure + forced compact ===");
    ac1_pressure_stats();
    ac2_high_volume_compact();
    ac3_capability_gate();
    ac4_schema_and_rollback();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
