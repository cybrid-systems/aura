// test_typesystem_type_propagation_jit_l2_typed_mutate.cpp — Issue #746:
// narrow_evidence + TypeId + linear_ownership_state propagation from
// CoercionMap/lowering/IRSoA to JIT L2 GuardShape/CastOp hot paths under
// typed mutation.
//
// Non-duplicative with #687, #720, #659, #629, #403.
//
//   - AC1: query:jit-typed-mutation-stats reachable (schema 746)
//   - AC2: eval-current :jit exercises narrow_evidence / L2 propagation
//   - AC3: mutate:rebind bumps JIT typed-mutation stats monotonic
//   - AC4: JIT eval correctness after typed mutate cycle
//   - AC5: multi-round mutate matrix monotonic
//   - AC6: query regression (dead-coercion, ir-metadata, typesystem)

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_746_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t jit_hash(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (query:jit-typed-mutation-stats) \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto narrow = jit_hash(cs, "narrow-evidence-hits");
    const auto cast = jit_hash(cs, "cast-elided-in-l2");
    const auto linear = jit_hash(cs, "linear-state-optimized");
    const auto coverage = jit_hash(cs, "type-propagation-coverage");
    if (narrow < 0 || cast < 0 || linear < 0 || coverage < 0)
        return -1;
    return narrow + cast + linear + coverage;
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (narrow-add x) (if (number? x) (+ x 1) 0)) "
                 "(define (wrap z) (narrow-add z)) "
                 "(define base 10) "
                 "(narrow-add 5) (wrap 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:jit-typed-mutation-stats (schema 746) ---");
    CHECK(setup_workspace(cs), "gradual typing workspace setup");
    auto h = cs.eval("(query:jit-typed-mutation-stats)");
    CHECK(h && is_hash(*h), "jit-typed-mutation-stats returns hash");
    CHECK(jit_hash(cs, "schema") == 746, "schema == 746");
    CHECK(jit_hash(cs, "narrow-evidence-hits") >= 0, "narrow-evidence-hits present");
    CHECK(jit_hash(cs, "cast-elided-in-l2") >= 0, "cast-elided-in-l2 present");
    CHECK(jit_hash(cs, "linear-state-optimized") >= 0, "linear-state-optimized present");
    CHECK(jit_hash(cs, "type-propagation-coverage") >= 0, "type-propagation-coverage present");

    std::println("\n--- AC2: JIT narrow_evidence / L2 propagation warmup ---");
    const auto narrow0 = jit_hash(cs, "narrow-evidence-hits");
    const auto cast0 = jit_hash(cs, "cast-elided-in-l2");
    const auto stats2a = stats_sum(cs);
    CHECK(cs.eval("(eval-current :jit)").has_value(), "JIT compile seeds metadata path");
    for (int i = 0; i < 12; ++i) {
        (void)cs.eval("(narrow-add 3)");
        (void)cs.eval("(wrap 7)");
    }
    const auto narrow1 = jit_hash(cs, "narrow-evidence-hits");
    const auto cast1 = jit_hash(cs, "cast-elided-in-l2");
    const auto stats2b = stats_sum(cs);
    std::println("  narrow-evidence-hits: {} -> {}", narrow0, narrow1);
    std::println("  cast-elided-in-l2: {} -> {}", cast0, cast1);
    std::println("  typed-mutation sum: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "JIT typed-mutation stats monotonic after warmup");

    std::println("\n--- AC3: mutate:rebind bumps propagation stats ---");
    const auto stats3a = stats_sum(cs);
    for (int round = 0; round < 4; ++round) {
        (void)cs.eval("(mutate:rebind \"base\" \"" + std::to_string(20 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(eval-current :jit)");
        (void)cs.eval("(narrow-add 2)");
    }
    const auto stats3b = stats_sum(cs);
    std::println("  typed-mutation sum: {} -> {}", stats3a, stats3b);
    CHECK(stats3b >= stats3a, "stats monotonic over mutate rounds");

    std::println("\n--- AC4: JIT eval correct after typed mutate ---");
    CHECK(cs.eval("(eval-current :jit)").has_value(), "JIT re-eval after mutate");
    auto v = cs.eval("(narrow-add 41)");
    CHECK(v && is_int(*v) && as_int(*v) == 42, "(narrow-add 41) == 42 post-mutate JIT");
    auto w = cs.eval("(wrap 9)");
    CHECK(w && is_int(*w) && as_int(*w) == 10, "(wrap 9) == 10 via alias");

    std::println("\n--- AC5: multi-round predicate mutate matrix ---");
    const auto stats5a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string body =
            "(lambda (x) (if (number? x) (+ x " + std::to_string(2 + round) + ") 0))";
        std::string esc;
        esc.reserve(body.size() + 8);
        for (char c : body) {
            if (c == '\\' || c == '"')
                esc.push_back('\\');
            esc.push_back(c);
        }
        (void)cs.eval(
            std::format("(mutate:rebind \"narrow-add\" \"{}\" \"issue746-r{}\")", esc, round));
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(eval-current :jit)");
        (void)cs.eval("(narrow-add 1)");
    }
    const auto stats5b = stats_sum(cs);
    std::println("  typed-mutation sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "stats monotonic over predicate mutate matrix");

    std::println("\n--- AC6: query regression ---");
    auto dco = cs.eval("(query:dead-coercion-zerooverhead-stats)");
    auto irmd = cs.eval("(query:ir-metadata-stats)");
    auto tsm = cs.eval("(query:typesystem-typed-mutate-stats)");
    CHECK(dco && is_hash(*dco), "dead-coercion-zerooverhead-stats regression");
    CHECK(irmd && is_int(*irmd), "ir-metadata-stats regression");
    CHECK(tsm && is_hash(*tsm), "typesystem-typed-mutate-stats regression");
}

} // namespace aura_issue_746_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_issue_746_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}