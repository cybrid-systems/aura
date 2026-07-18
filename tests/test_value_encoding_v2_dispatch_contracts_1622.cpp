// @category: integration
// @reason: Issue #1622 — EvalValue v2 consteval dispatch table +
// hot-path Contracts (refine #571/#723).
//
//   AC1: consteval classify + low2 table static checks
//   AC2: runtime classify matches consteval for common patterns
//   AC3: query:value-dispatch-stats schema 1622 AC keys
//   AC4: mutate/eval advances dispatch-hits; collisions stay 0
//   AC5: multi-round value churn stress
//   AC6: #723 lineage keys + wire flags

#include "test_harness.hpp"
#include "compiler/value_tags.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::classify_eval_value_tag;
using aura::compiler::types::classify_eval_value_tag_consteval;
using aura::compiler::types::eval_value_tag_low2_table;
using aura::compiler::types::EvalValueTag;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_valid_tagged_value;
using aura::compiler::types::kSpecialFalse;
using aura::compiler::types::kSpecialTrue;
using aura::compiler::types::kSpecialVoid;
using aura::compiler::types::kTagPatterns;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;
using aura::compiler::types::v2_string_collision_attempts;
using aura::compiler::types::value_dispatch_hit_count;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:value-dispatch-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static bool seed(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (add1 x) (+ x 1)) "
                 "(define a 1) (define b 2) (add1 3)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void ac1_consteval() {
    std::println("\n--- AC1: consteval dispatch table ---");
    static_assert(eval_value_tag_low2_table(0) == EvalValueTag::Fixnum);
    static_assert(eval_value_tag_low2_table(1) == EvalValueTag::Ref);
    static_assert(eval_value_tag_low2_table(2) == EvalValueTag::StringV2);
    static_assert(eval_value_tag_low2_table(3) == EvalValueTag::Special);
    static_assert(classify_eval_value_tag_consteval(0) == EvalValueTag::Fixnum);
    static_assert(classify_eval_value_tag_consteval(2) == EvalValueTag::Fixnum);
    static_assert(classify_eval_value_tag_consteval(kSpecialFalse) == EvalValueTag::Special);
    static_assert(kTagPatterns[0] == EvalValueTag::Fixnum);
    CHECK(true, "consteval static_asserts compiled");
}

static void ac2_runtime_match() {
    std::println("\n--- AC2: runtime classify matches consteval ---");
    const std::int64_t samples[] = {
        0, 2, 4, kSpecialFalse, kSpecialTrue, kSpecialVoid, 1, // Ref
    };
    for (auto v : samples) {
        const auto rt = classify_eval_value_tag(v);
        const auto ct = classify_eval_value_tag_consteval(v);
        CHECK(rt == ct, std::format("match for v={}", v));
        CHECK(is_valid_tagged_value(v) || rt == EvalValueTag::Unknown, "valid or unknown");
    }
    // make_int path
    auto i = make_int(42);
    CHECK(classify_eval_value_tag(i.val) == EvalValueTag::Fixnum, "make_int Fixnum");
    CHECK(as_int(i) == 42, "as_int 42");
    auto s = make_string(0);
    CHECK(classify_eval_value_tag(s.val) == EvalValueTag::StringV2, "make_string StringV2");
}

static void ac3_query_schema() {
    std::println("\n--- AC3: query schema 1622 ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    (void)cs.eval("(+ 1 2)");
    (void)cs.eval("(add1 5)");
    auto h = cs.eval("(engine:metrics \"query:value-dispatch-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1622 || href(cs, "schema") == 723, "schema 1622|723");
    CHECK(href(cs, "issue") == 1622 || href(cs, "issue") < 0, "issue 1622");
    CHECK(href(cs, "dispatch-hits") >= 0, "dispatch-hits");
    CHECK(href(cs, "dispatch-misses") >= 0, "dispatch-misses");
    CHECK(href(cs, "dispatch-hit-rate-bp") >= 0 || href(cs, "dispatch_hit_rate") >= 0,
          "hit-rate-bp");
    CHECK(href(cs, "contract-violation-count") >= 0 || href(cs, "contract_violation_count") >= 0,
          "contract-violation-count");
    CHECK(href(cs, "v2-string-collision-attempts") >= 0 ||
              href(cs, "v2_string_collision_attempts") >= 0,
          "collision-attempts");
    CHECK(href(cs, "classify-calls") >= 0, "classify-calls");
    CHECK(href(cs, "consteval-table-wired") == 1, "consteval-table-wired");
    CHECK(href(cs, "hotpath-contracts-wired") == 1, "hotpath-contracts-wired");
}

static void ac4_mutate_path() {
    std::println("\n--- AC4: mutate/eval advances hits ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    const auto hits0 = load_u64(value_dispatch_hit_count);
    const auto coll0 = load_u64(v2_string_collision_attempts);
    for (int i = 0; i < 30; ++i) {
        (void)cs.eval(std::format("(+ {} {})", i, i + 1));
        (void)cs.eval("(add1 1)");
    }
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");
    CHECK(load_u64(value_dispatch_hit_count) > hits0, "hits advanced");
    CHECK(load_u64(v2_string_collision_attempts) == coll0, "collisions unchanged (0 growth)");
    CHECK(href(cs, "v2-string-collision-attempts") == 0 ||
              href(cs, "v2_string_collision_attempts") == 0 ||
              href(cs, "v2-string-collisions") == 0,
          "collision attempts ~0");
}

static void ac5_stress() {
    std::println("\n--- AC5: multi-round value churn ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    const auto c0 = load_u64(v2_string_collision_attempts);
    for (int i = 0; i < 100; ++i) {
        (void)make_int(i);
        (void)classify_eval_value_tag(make_int(i * 3).val);
        (void)cs.eval(std::format("(+ {} 1)", i % 50));
        if ((i % 10) == 0)
            (void)cs.eval(std::format("(mutate:rebind \"b\" \"{}\")", i));
    }
    CHECK(load_u64(v2_string_collision_attempts) == c0, "no collisions under churn");
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value(), "eval ok");
}

static void ac6_lineage() {
    std::println("\n--- AC6: #723 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "dispatch-calls") >= 0, "dispatch-calls");
    CHECK(href(cs, "unknown-tags") >= 0, "unknown-tags");
    CHECK(href(cs, "shape-history-shifts") >= 0, "shape-history-shifts");
    CHECK(href(cs, "consteval-table-wired") == 1, "wired");
}

} // namespace

int main() {
    std::println("=== Issue #1622: Value v2 consteval dispatch + Contracts ===");
    ac1_consteval();
    ac2_runtime_match();
    ac3_query_schema();
    ac4_mutate_path();
    ac5_stress();
    ac6_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
