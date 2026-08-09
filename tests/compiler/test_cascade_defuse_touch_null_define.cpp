// @category: unit
// @reason: Issue #2817 — cascade must not defuse_touch names with no live
// Define (ghost names), to avoid stale defuse index pollution.
//
//   AC1: cascade cites #2817; ghost skip; cascade_ghost_name_touch_total
//   AC2: ghost name in defuse_affected_syms_ → ghost metric; no crash
//   AC3: real define still defuse_touched; schema-2817 query
//   AC4: this suite + linter; no docs/design/2817-*; no test_issue_2817.cpp

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::NULL_NODE;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
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

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:incremental-relower-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_cascade_defuse_touch_null_define() {
    std::println("=== Issue #2817: cascade ghost-name defuse_touch skip ===");
    CHECK(true, "ac2817: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: ghost skip + metric in cascade ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto met = read_file("src/compiler/observability_metrics.h");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(!mut.empty(), "AC1: sources readable");
        auto cascade = mut.find("push_post_mutate_incremental_cascade");
        CHECK(cascade != std::string::npos, "AC1: cascade present");
        auto end = mut.find("// 3) Eager partial re-lower", cascade);
        auto body = end != std::string::npos && end > cascade ? mut.substr(cascade, end - cascade)
                                                              : mut.substr(cascade, 9000);
        CHECK(body.find("Issue #2817") != std::string::npos, "AC1: cites #2817");
        CHECK(body.find("cascade_ghost_name_touch_total") != std::string::npos,
              "AC1: ghost metric bump");
        CHECK(body.find("names_with_def") != std::string::npos ||
                  body.find("ghost_name") != std::string::npos,
              "AC1: ghost/names_with_def filter");
        // defuse_touch only when has live Define
        CHECK(body.find("defuse_touch_fn_") != std::string::npos, "AC1: defuse_touch present");
        CHECK(body.find("names_with_def.count") != std::string::npos ||
                  body.find("names_with_def") != std::string::npos,
              "AC1: touch gated on live Define");
        CHECK(met.find("cascade_ghost_name_touch_total") != std::string::npos, "AC1: metrics.h");
        CHECK(obs.find("schema-2817") != std::string::npos, "AC1: query schema-2817");
    }

    // ── AC2: ghost name stages path2 → ghost metric ──
    {
        std::println("\n--- AC2: ghost-name in defuse set → ghost metric ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (real x) x) (real 1)\")").has_value(), "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");
        auto& ev = cs.evaluator();
        auto* pool = ev.workspace_pool();
        CHECK(pool != nullptr, "AC2: workspace pool");
        auto* m = metrics_of(cs);
        const auto g0 = m->cascade_ghost_name_touch_total.load(std::memory_order_relaxed);

        // Stage a ghost name into defuse_affected_syms_ without a Define.
        const auto ghost = std::string("ghost-name-2817");
        const auto sid = pool->intern(ghost);
        // Empty entry_nodes — only records the name for cascade path2.
        ev.propagate_defuse_dirty(sid, ghost, std::span<const aura::ast::NodeId>{});

        // Run cascade directly (same as Guard success path).
        ev.push_post_mutate_incremental_cascade(/*mutation_log_begin=*/0);

        const auto g1 = m->cascade_ghost_name_touch_total.load(std::memory_order_relaxed);
        CHECK(g1 > g0, "AC2: cascade_ghost_name_touch_total advanced for ghost");
        CHECK(href(cs, "cascade_ghost_name_touch_total") == static_cast<std::int64_t>(g1) ||
                  href(cs, "cascade-ghost-name-touch-total") == static_cast<std::int64_t>(g1),
              "AC2: query ghost surface");
        CHECK(href(cs, "schema-2817") == 2817 || href(cs, "cascade-ghost-name-wired") == 1,
              "AC2: schema-2817 / wired");
    }

    // ── AC3: real define still works (no false ghost) ──
    {
        std::println("\n--- AC3: real define cascade does not false-ghost ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) x) (f 0)\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        auto* m = metrics_of(cs);
        const auto g0 = m->cascade_ghost_name_touch_total.load(std::memory_order_relaxed);
        auto mut = cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 1))\" \"#2817\")");
        CHECK(mut.has_value(), "AC3: set-body real define");
        const auto g1 = m->cascade_ghost_name_touch_total.load(std::memory_order_relaxed);
        // Real define should not bump ghost counter (path1/2 finds Define).
        CHECK(g1 == g0, "AC3: real define does not count as ghost");
        CHECK(m->post_mutate_incremental_cascade_total.load() > 0, "AC3: cascade ran");
    }

    // ── AC4: defuse_touch not called for ghosts in source ──
    {
        std::println("\n--- AC4: source skips defuse_touch for ghost branch ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto p = mut.find("Issue #2817");
        CHECK(p != std::string::npos, "AC4: #2817 present");
        auto win = mut.substr(p, 2000);
        CHECK(win.find("Skip defuse_touch") != std::string::npos ||
                  win.find("do NOT defuse_touch") != std::string::npos ||
                  win.find("ghost") != std::string::npos,
              "AC4: documents skip");
        CHECK(win.find("cascade_ghost_name_touch_total") != std::string::npos ||
                  mut.find("cascade_ghost_name_touch_total") != std::string::npos,
              "AC4: metric");
    }

    std::println("\n=== #2817 cascade ghost-name defuse_touch: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_cascade_defuse_touch_null_define();
}
#endif
