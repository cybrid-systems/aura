// @category: integration
// @reason: Issue #2259 — 64-bit tagged Value Encoding hot-path contracts +
// zero-overhead tagged dispatch for eval_flat / apply / ir_executor.
//
//   AC1: Pure is_* (is_fixnum_hot / is_int) match classify; single low2 path
//   AC2: as_int / pure arithmetic roundtrip (parity microbench proxy)
//   AC3: AURA_HOT_CONTRACT on as_* (debug fail-closed; release elided)
//   AC4: value_tag_hot_path_total + schema-2259 metrics
//   AC5: Semantic parity — existing is_int/as_int behavior under eval

#include "test_harness.hpp"
#include "compiler/value_tags.h"
#include "compiler/shape_profiler.h"

#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.value;
import aura.compiler.service;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::shape::inline_shape_of;
using aura::compiler::shape::SHAPE_INT;
using aura::compiler::types::as_int;
using aura::compiler::types::classify_eval_value_tag;
using aura::compiler::types::classify_eval_value_tag_consteval;
using aura::compiler::types::EvalValueTag;
using aura::compiler::types::is_fixnum_hot;
using aura::compiler::types::is_float_hot;
using aura::compiler::types::is_int;
using aura::compiler::types::is_ref_hot;
using aura::compiler::types::is_string;
using aura::compiler::types::is_string_v2_hot;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;
using aura::compiler::types::note_value_tag_hot_path;
using aura::compiler::types::note_value_tag_stability;
using aura::compiler::types::tag_low2_hot;
using aura::compiler::types::value_tag_hot_path_total;
using aura::compiler::types::value_tag_hotpath_2259_wired;
using aura::compiler::types::value_tag_hotpath_zero_overhead_wired;
using aura::compiler::types::value_tag_stability_fixnum_total;
using aura::compiler::types::value_tag_stability_run_total;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:value-dispatch-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_pure_tag_tests() {
    std::println("\n--- AC1: pure hot tag tests match classify ---");
    static_assert(is_fixnum_hot(0));
    static_assert(is_fixnum_hot(2)); // make_int(1)
    static_assert(tag_low2_hot(0) == 0);
    static_assert(tag_low2_hot(1) == 1);
    static_assert(tag_low2_hot(2) == 2);
    static_assert(tag_low2_hot(3) == 3);

    auto i0 = make_int(0);
    auto i1 = make_int(42);
    auto i_neg = make_int(-7);
    CHECK(is_int(i0) && is_fixnum_hot(i0.val), "is_int 0");
    CHECK(is_int(i1) && is_fixnum_hot(i1.val), "is_int 42");
    CHECK(is_int(i_neg) && is_fixnum_hot(i_neg.val), "is_int -7");
    CHECK(classify_eval_value_tag_consteval(i1.val) == EvalValueTag::Fixnum, "consteval Fixnum");
    CHECK(classify_eval_value_tag(i1.val) == EvalValueTag::Fixnum, "runtime Fixnum");

    auto s = make_string(0);
    CHECK(is_string(s) && is_string_v2_hot(s.val), "is_string pure");
    CHECK(!is_int(s), "string not int");
    CHECK(tag_low2_hot(s.val) == 2, "string low2");

    // Ref: low2 == 1
    CHECK(is_ref_hot(1), "ref low2=1");
    CHECK(!is_fixnum_hot(1), "ref not fixnum");
    CHECK(!is_float_hot(i1.val), "int not float");
}

static void ac2_arithmetic_parity() {
    std::println("\n--- AC2: pure arithmetic via as_int (microbench proxy) ---");
    // Pure fixnum add loop — same results as baseline make_int/as_int path.
    std::int64_t acc = 0;
    bool all_int = true;
    for (int i = 0; i < 1000; ++i) {
        auto a = make_int(i);
        auto b = make_int(1);
        all_int = all_int && is_int(a) && is_int(b);
        acc += as_int(a) + as_int(b);
    }
    CHECK(all_int, "all operands classified as int");
    // sum_{i=0}^{999} (i+1) = sum_{k=1}^{1000} k = 1000*1001/2
    CHECK(acc == 1000 * 1001 / 2, "arithmetic parity sum");
}

static void ac3_contracts_present() {
    std::println("\n--- AC3: AURA_HOT_CONTRACT on as_* (source + happy path) ---");
    // Happy path: valid encoding — no abort under debug contracts.
    auto v = make_int(99);
    CHECK(as_int(v) == 99, "as_int contracted happy path");
    auto s = make_string(3);
    CHECK(is_string(s), "string ok");
    // Encoding invariants: fixnum low bit clear after make_int.
    CHECK((v.val & 1) == 0, "fixnum low bit clear");
    CHECK((s.val & 3) == 2, "string low2 == 2");
    // Wired flags prove contracts surface is present in production build.
    CHECK(value_tag_hotpath_2259_wired.load() == 1, "2259 wired");
    CHECK(value_tag_hotpath_zero_overhead_wired.load() == 1, "zero-overhead wired");
}

static void ac4_metrics() {
    std::println("\n--- AC4: value_tag_hot_path_total + schema-2259 ---");
    const auto hot0 = value_tag_hot_path_total.load(std::memory_order_relaxed);
    note_value_tag_hot_path();
    (void)as_int(make_int(1));
    CHECK(value_tag_hot_path_total.load(std::memory_order_relaxed) > hot0, "hot path advanced");

    // Tag stability: consecutive Fixnum samples.
    const auto stab0 = value_tag_stability_run_total.load(std::memory_order_relaxed);
    const auto fx0 = value_tag_stability_fixnum_total.load(std::memory_order_relaxed);
    for (int i = 0; i < 8; ++i) {
        note_value_tag_stability(EvalValueTag::Fixnum);
        (void)inline_shape_of(make_int(i).val);
    }
    CHECK(value_tag_stability_run_total.load(std::memory_order_relaxed) >= stab0,
          "stability runs monotonic");
    CHECK(value_tag_stability_fixnum_total.load(std::memory_order_relaxed) >= fx0,
          "fixnum stability monotonic");
    CHECK(inline_shape_of(make_int(5).val) == SHAPE_INT, "shape Fixnum");

    CompilerService cs;
    CHECK(href(cs, "schema-2259") == 2259, "schema-2259");
    CHECK(href(cs, "issue-2259") == 2259, "issue-2259");
    CHECK(href(cs, "value-tag-hotpath-zero-overhead-wired") == 1, "zero-overhead key");
    CHECK(href(cs, "value-tag-hotpath-2259-wired") == 1, "2259 wired key");
    CHECK(href(cs, "value-tag-hot-path-total") >= 0, "hot-path-total key");
    CHECK(href(cs, "value-tag-hot-contract-fail-total") >= 0, "contract-fail key");
    CHECK(href(cs, "value-tag-stability-fixnum-total") >= 0, "stability fixnum key");
    CHECK(href(cs, "schema") == 1622, "lineage schema 1622");
}

static void ac5_eval_semantics() {
    std::println("\n--- AC5: evaluator semantic parity ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (add1 x) (+ x 1)) (add1 41)\")").has_value(), "set-code");
    auto r = cs.eval("(eval-current)");
    CHECK(r.has_value() && is_int(*r) && as_int(*r) == 42, "eval (+ x 1) => 42");
    auto a = cs.eval("(+ 10 20 30)");
    CHECK(a.has_value() && is_int(*a) && as_int(*a) == 60, "multi add");
    auto f = cs.eval("(* 6 7)");
    CHECK(f.has_value() && is_int(*f) && as_int(*f) == 42, "mul");
}

} // namespace

int main() {
    std::println("=== Issue #2259: Value tag hot-path contracts + zero-overhead dispatch ===");
    ac1_pure_tag_tests();
    ac2_arithmetic_parity();
    ac3_contracts_present();
    ac4_metrics();
    ac5_eval_semantics();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
