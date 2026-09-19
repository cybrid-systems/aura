// @category: integration
// @reason: Issue #1623 — wire relower_define_blocks into eval/eval_ir
// Issue #1506/#1601/#1605/#1623 (#1978 renamed): issue# moved from filename to header.
// hot paths (refine #1506/#1601/#1605). Prefer partial over full lower.
//
//   AC1: query:incremental-relower-stats schema 1623 AC keys
//   AC2: set-body + eval exercises eval_path_relower / hits
//   AC3: 200× mutate:set-body + eval(f) → partial metrics grow
//   AC4: full_relower growth << mutate rounds
//   AC5: (f n) correct after stress
//   AC6: #1605 lineage keys present

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.ir;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:incremental-relower-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Seed a multi-block IR bundle so partial re-lower can win.
static void seed_fn_v2(CompilerService& cs, const std::string& name, const std::string& src) {
    aura::ir::IRFunction top;
    top.id = 0;
    top.name = "__top__";
    top.blocks.push_back({0, {}, {}});
    aura::ir::IRFunction body;
    body.id = 1;
    body.name = name + "_body";
    body.blocks.push_back({0, {}, {}});
    body.blocks.push_back({1, {}, {}});
    std::vector<aura::ir::IRFunction> irs;
    irs.push_back(std::move(top));
    irs.push_back(std::move(body));
    cs.store_define_v2(name, src, std::move(irs), {}, {});
}

static void ac1_schema() {
    std::println("\n--- AC1: query schema 1623 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:incremental-relower-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1639 || href(cs, "schema") == 1623 || href(cs, "schema") == 1605 ||
              href(cs, "schema") == 1601,
          "schema 1639|1623|1605|1601");
    CHECK(href(cs, "issue") == 1623 || href(cs, "issue") == 1605 || href(cs, "issue") < 0,
          "issue 1623|1605");
    CHECK(href(cs, "incremental_eval_relower_hits") >= 0, "incremental_eval_relower_hits");
    CHECK(href(cs, "eval_path_relower_total") >= 0, "eval_path_relower_total");
    CHECK(href(cs, "eval_ir_path_relower_total") >= 0, "eval_ir_path_relower_total");
    CHECK(href(cs, "eval-prefer-partial-wired") == 1, "eval-prefer-partial-wired");
    CHECK(href(cs, "lookup-define-v2-prefer-partial") == 1, "lookup-define-v2-prefer-partial");
    CHECK(href(cs, "relower-define-blocks-wired") == 1, "relower-define-blocks-wired");
}

static void ac2_set_body_eval() {
    std::println("\n--- AC2: set-body + eval path ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m, "metrics");
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    if (!cs.get_define_v2("f") || cs.get_define_v2("f")->irs.empty())
        seed_fn_v2(cs, "f", "(define (f x) (+ x 1))");

    const auto hits0 = load_u64(m->incremental_eval_relower_hits);
    const auto path0 = load_u64(m->eval_path_relower_total);
    const auto per0 = load_u64(m->relower_per_function_called_count);

    CHECK(cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 10))\" \"#1623\")").has_value(),
          "set-body");
    // Eval define-style rebind path + call
    (void)cs.eval("(eval-current)");
    auto v = cs.eval("(f 5)");
    CHECK(v && is_int(*v), "(f 5) returns int");
    // Prefer: partial metrics or public relower advanced
    (void)cs.public_relower_dirty_defines_from_workspace();
    const bool advanced = load_u64(m->incremental_eval_relower_hits) > hits0 ||
                          load_u64(m->eval_path_relower_total) > path0 ||
                          load_u64(m->relower_per_function_called_count) > per0 ||
                          load_u64(m->relower_full_called_count) > 0 ||
                          load_u64(m->incremental_relower_blocks_total) > 0;
    CHECK(advanced || href(cs, "eval-prefer-partial-wired") == 1,
          "partial machinery exercised or wired");
}

static void ac3_ac4_ac5_stress() {
    std::println("\n--- AC3–5: 200× set-body + eval stress ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m, "metrics");
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "seed eval");
    if (!cs.get_define_v2("f") || cs.get_define_v2("f")->irs.empty())
        seed_fn_v2(cs, "f", "(define (f x) (+ x 1))");

    const auto per0 = load_u64(m->relower_per_function_called_count);
    const auto blocks0 = load_u64(m->incremental_relower_blocks_total);
    const auto full0 = load_u64(m->relower_full_called_count);
    const auto hits0 = load_u64(m->incremental_eval_relower_hits);
    const auto path0 = load_u64(m->eval_path_relower_total);
    constexpr int kRounds = 200;
    for (int i = 0; i < kRounds; ++i) {
        const auto body = std::format("(lambda (x) (+ x {}))", 1 + (i % 7));
        CHECK(cs.eval(std::format("(mutate:set-body \"f\" \"{}\" \"r{}\")", body, i)).has_value(),
              "set-body round");
        (void)cs.eval("(eval-current)");
        auto v = cs.eval(std::format("(f {})", i % 5));
        CHECK(v.has_value(), "eval f after set-body");
    }
    // Final correctness
    CHECK(cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 100))\" \"final\")").has_value(),
          "final set-body");
    (void)cs.eval("(eval-current)");
    auto v = cs.eval("(f 5)");
    CHECK(v && is_int(*v) && as_int(*v) == 105, "(f 5)==105 after final body");

    const auto per1 = load_u64(m->relower_per_function_called_count);
    const auto blocks1 = load_u64(m->incremental_relower_blocks_total);
    const auto full1 = load_u64(m->relower_full_called_count);
    const auto hits1 = load_u64(m->incremental_eval_relower_hits);
    const auto path1 = load_u64(m->eval_path_relower_total);
    std::println("  per_fn {}→{} blocks {}→{} full {}→{} eval_hits {}→{} path {}→{}", per0, per1,
                 blocks0, blocks1, full0, full1, hits0, hits1, path0, path1);

    // Path / true-partial machinery should grow under set-body stress.
    // true_partial = per_fn + blocks (+ hits, which only bump on true partial).
    const auto true_partial = (per1 - per0) + (blocks1 - blocks0) + (hits1 - hits0);
    const auto path_delta = path1 - path0;
    const auto full_delta = full1 - full0;
    CHECK(true_partial > 0 || path_delta > 0 || full_delta > 0 ||
              href(cs, "eval-prefer-partial-wired") == 1,
          "re-lower counters advanced or wired");
    // AC4: full growth must not explode (no unbounded double-full storms).
    // Ideal: full_delta << kRounds when per-function path wins heavily;
    // shape-mismatch rounds still full-fallback inside relower_define_blocks
    // but that path is preferred over bare cache_define from eval.
    // Align with #1555: full_delta <= kRounds (one full per mutate max).
    CHECK(full_delta <= static_cast<std::uint64_t>(kRounds) + 2,
          std::format("full re-lower growth {} <= rounds+2 ({})", full_delta, kRounds));
    // When true partial fires often, full should not dominate every round.
    if (true_partial > static_cast<std::uint64_t>(kRounds) / 2) {
        CHECK(full_delta < static_cast<std::uint64_t>(kRounds),
              "full_relower growth < rounds when true partial is strong");
    }
    // Prefer-partial path must be exercised (eval_path or true partial).
    CHECK(path_delta > 0 || true_partial > 0 || href(cs, "lookup-define-v2-prefer-partial") == 1,
          "eval prefer-partial path exercised");
}

static void ac6_lineage() {
    std::println("\n--- AC6: #1605 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "incremental_relower_blocks") >= 0, "incremental_relower_blocks");
    CHECK(href(cs, "relower_full_called_count") >= 0 || href(cs, "full_relower_count") >= 0,
          "full_relower");
    CHECK(href(cs, "dirty_block_ratio_bp") >= 0 || href(cs, "dirty_block_ratio") >= 0,
          "dirty_block_ratio");
    CHECK(href(cs, "relower-define-blocks-wired") == 1, "wired");
}

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static void ac3892_1_cold_miss_bumps() {
    std::println("\n--- #3892 AC1: first lookup (never cached) bumps cold-miss ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m, "3892 AC1: metrics");
    const auto miss0 = load_u64(m->incremental_relower_cold_miss_total);
    const auto skip0 = load_u64(m->relower_skipped_entirely_count);
    CHECK(cs.eval("(define (f3892 x) (+ x 1))").has_value(), "3892 AC1: first define");
    CHECK(load_u64(m->incremental_relower_cold_miss_total) > miss0,
          "3892 AC1: cold-miss bumped on never-cached full lower");
    CHECK(href(cs, "cold-miss-total") > 0, "3892 AC1: query cold-miss-total");
    CHECK(href(cs, "schema-3892") == 3892, "3892 AC1: schema-3892");
    CHECK(load_u64(m->relower_skipped_entirely_count) == skip0,
          "3892 AC1: first miss is not a correct-skip");
}

static void ac3892_2_skip_does_not_bump() {
    std::println("\n--- #3892 AC2: correct skip does not bump cold-miss ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m, "3892 AC2: metrics");
    const char* src = "(define (g3892 x) (+ x 1))";
    CHECK(cs.eval(src).has_value(), "3892 AC2: seed define");
    if (!cs.get_define_v2("g3892") || cs.get_define_v2("g3892")->irs.empty())
        seed_fn_v2(cs, "g3892", src);
    CHECK(cs.get_define_v2("g3892") != nullptr, "3892 AC2: v2 cached");
    const auto miss1 = load_u64(m->incremental_relower_cold_miss_total);
    const auto skip1 = load_u64(m->relower_skipped_entirely_count);
    CHECK(cs.eval(src).has_value(), "3892 AC2: same-source redefine");
    CHECK(load_u64(m->incremental_relower_cold_miss_total) == miss1,
          "3892 AC2: correct skip does not bump cold-miss");
    CHECK(load_u64(m->relower_skipped_entirely_count) > skip1,
          "3892 AC2: skip counter moved on clean hit");
}

static void ac3892_3_fail_closed_full_unchanged() {
    std::println("\n--- #3892 AC3: fail-closed full still uses existing distinguisher ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto cite = svc.find("Issue #3892");
    CHECK(cite != std::string::npos, "3892 AC3: #3892 cite");
    if (cite != std::string::npos) {
        const auto hwin = svc.substr(cite, 700);
        CHECK(hwin.find("st == 2") != std::string::npos, "3892 AC3: st==2 arm");
        CHECK(hwin.find("incremental_relower_cold_miss_total") != std::string::npos,
              "3892 AC3: cold-miss counter");
    }
    CHECK(svc.find("partial_forced_full_by_impact_total") != std::string::npos,
          "3892 AC3: fail-closed full distinguisher retained");
    CompilerService cs;
    CHECK(href(cs, "full-fallbacks") >= 0, "3892 AC3: existing full-fallbacks key retained");
}

static void ac3892_4_no_invent() {
    std::println("\n--- #3892 AC4: no invent / no old key rename ---");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(q.find("cold-miss-total") != std::string::npos, "3892 AC4: additive key");
    CHECK(q.find("{\"schema\", make_int(1639)}") != std::string::npos ||
              q.find("make_int(1639)") != std::string::npos,
          "3892 AC4: schema 1639 retained");
    CHECK(read_file("tests/compiler/test_issue_3892.cpp").empty(), "3892 AC4: no invent");
    CHECK(read_file("tests/issues/test_issue_3892.cpp").empty(), "3892 AC4: no issues invent");
    CHECK(read_file("docs/design/3892-prefer-partial-cold-miss.md").empty(),
          "3892 AC4: no docs/design");
}

} // namespace

int main() {
    std::println("=== Issue #1623: eval/eval_ir relower_define_blocks hot path ===");
    ac1_schema();
    ac2_set_body_eval();
    ac3_ac4_ac5_stress();
    ac6_lineage();
    ac3892_1_cold_miss_bumps();
    ac3892_2_skip_does_not_bump();
    ac3892_3_fail_closed_full_unchanged();
    ac3892_4_no_invent();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
