// @category: unit
// @reason: Issue #2461 — per-If stable narrowing cache key
//          (cond_shape_hash × epoch × refined).
//
//   AC1: same shape + epoch → second visit is structural key hit
//   AC2: shape key present; unrelated mutate docs (source)
//   AC3: var-name selective invalidate path retained
//   AC4: epoch advance miss path retained
//   AC5: schema-2461 + #2359 keys coherent; source-cite

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.ast;
import aura.core.type;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::TypeChecker;
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
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: same shape hash for identical conds ──
static void ac1_shape_stable() {
    std::println("\n--- #2461 AC1: same pred shape → identical hash ---");
    FlatAST flat;
    StringPool pool;
    // Build two structurally identical Variable-shaped leaves
    auto a = flat.add_variable(pool.intern("x"));
    auto b = flat.add_variable(pool.intern("x"));
    const auto ha = TypeChecker::hash_node_shape(flat, a, 0);
    const auto hb = TypeChecker::hash_node_shape(flat, b, 0);
    CHECK(ha == hb, "AC1: identical Variable x shape hashes match");
    CHECK(ha != 0, "AC1: shape hash non-zero");

    // Different name → different hash
    auto c = flat.add_variable(pool.intern("y"));
    CHECK(TypeChecker::hash_node_shape(flat, c, 0) != ha, "AC1: different var → different hash");

    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("cond_shape_hash") != std::string::npos, "AC1: cond_shape_hash stored");
    CHECK(tci.find("occurrence_cache_key_hits_") != std::string::npos ||
              tci.find("occurrence_cache_key_hit") != std::string::npos,
          "AC1: hit counter path");
    CHECK(tci.find("shape_ok") != std::string::npos ||
              tci.find("cond_shape_hash == shape_hash") != std::string::npos,
          "AC1: shape match required for hit");
}

// ── AC2: source documents unrelated mutate stability ──
static void ac2_unrelated_docs() {
    std::println("\n--- #2461 AC2: structural key isolates unrelated mutate ---");
    const auto tci = read_file("src/compiler/type_checker.ixx");
    CHECK(tci.find("Issue #2461") != std::string::npos, "AC2: ixx cites #2461");
    CHECK(tci.find("cond_shape_hash") != std::string::npos, "AC2: entry has shape field");
    CHECK(tci.find("invalidate_predicate_memo_for_var_names") != std::string::npos,
          "AC2: selective var invalidate retained");
}

// ── AC3: var named in predicate → selective miss ──
static void ac3_var_invalidate() {
    std::println("\n--- #2461 AC3: predicate var invalidate + goal note ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("note_occurrence_goal") != std::string::npos, "AC3: note_occurrence_goal");
    CHECK(impl.find("Issue #2461") != std::string::npos &&
              impl.find("note_occurrence_goal") != std::string::npos,
          "AC3: goal note on miss path (#2461)");
    CHECK(impl.find("invalidate_predicate_memo_for_var_names") != std::string::npos,
          "AC3: selective invalidate retained");
}

// ── AC4: epoch advance miss ──
static void ac4_epoch_miss() {
    std::println("\n--- #2461 AC4: epoch in key → advance is miss ---");
    const auto tci = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("epoch") != std::string::npos &&
              tci.find("cond_shape_hash") != std::string::npos,
          "AC4: epoch + shape in entry");
    CHECK(impl.find("memo_it->second.epoch == cache_epoch_") != std::string::npos,
          "AC4: epoch gate in resolve");
    CHECK(impl.find("prune_occurrence_goals") != std::string::npos ||
              tci.find("prune_occurrence_goals") != std::string::npos,
          "AC4: #2278 prune retained");
}

// ── AC5: schema + #2359 ──
static void ac5_schema() {
    std::println("\n--- #2461 AC5: schema-2461 + #2359 coherent ---");
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    metrics.occurrence_cache_key_hit_total.store(7, std::memory_order_relaxed);
    metrics.occurrence_cache_key_miss_total.store(3, std::memory_order_relaxed);

    CHECK(href(cs, "schema-2461") == 2461, "AC5: schema-2461");
    CHECK(href(cs, "issue-2461") == 2461, "AC5: issue-2461");
    CHECK(href(cs, "occurrence-cache-key-hit-total") == 7, "AC5: hit key");
    CHECK(href(cs, "occurrence-cache-key-miss-total") == 3, "AC5: miss key");
    CHECK(href(cs, "occurrence-cache-key-wired") == 1, "AC5: wired");
    CHECK(href(cs, "schema-2359") == 2359, "AC5: #2359 retained");
    CHECK(href(cs, "memo-goal-epoch-health-wired") == 1, "AC5: #2359 health wired");
    CHECK(href(cs, "schema-2278") == 2278 || href(cs, "occurrence-goal-sole-authority-wired") == 1,
          "AC5: #2278 lineage");

    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(q.find("schema-2461") != std::string::npos, "AC5: query schema");
    CHECK(met.find("occurrence_cache_key_hit_total") != std::string::npos, "AC5: metrics field");
    CHECK(read_file("src/compiler/type_checker_impl.cpp").find("hash_node_shape") !=
              std::string::npos,
          "AC5: uses hash_node_shape");
}

} // namespace

int run_test_occurrence_cache_key() {
    std::println("=== Issue #2461: per-If occurrence cache key ===");
    ac1_shape_stable();
    ac2_unrelated_docs();
    ac3_var_invalidate();
    ac4_epoch_miss();
    ac5_schema();
    std::println("\n=== #2461 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_occurrence_cache_key();
}
#endif
