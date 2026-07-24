// test_cpp26_contracts_hotpath_arena_soa_value_shape_pass.cpp — Issue #742:
// C++26 Contracts + consteval hot-path invariants for Arena/SoA/Value/
// Shape/Pass under mutate + eval workloads.
//
// Non-duplicative with #658 (highperf-cpp26-stats), #431 (cxx26-invariants),
// #465 (cxx26-hotpath-invariants), #723 (limited Contracts).
//
//   - AC1: query:cpp26-contracts-stats reachable (schema 742)
//   - AC2: eval/mutate bumps hotpath-invariant-hits
//   - AC3: consteval-checks == 36 (compile-time baked; #1321 expanded)
//   - AC4: contract-violations-caught readable (zero in normal path)
//   - AC5: multi-round mutate — hotpath hits monotonic
//   - AC6: query regression (highperf-cpp26, cxx26-invariants)

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_742_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t contract_hash(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (engine:metrics \"query:cpp26-contracts-stats\") \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto hotpath = contract_hash(cs, "hotpath-invariant-hits");
    const auto violations = contract_hash(cs, "contract-violations-caught");
    const auto consteval_n = contract_hash(cs, "consteval-checks");
    if (hotpath < 0 || violations < 0 || consteval_n < 0)
        return -1;
    return hotpath + violations + consteval_n;
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) "
                 "(define a 1) (define b 2) (fact 5)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:cpp26-contracts-stats (schema 742) ---");
    CHECK(setup_workspace(cs), "recursive workspace setup");
    auto h = cs.eval("(engine:metrics \"query:cpp26-contracts-stats\")");
    CHECK(h && is_hash(*h), "cpp26-contracts-stats returns hash");
    CHECK(contract_hash(cs, "schema") == 1620 || contract_hash(cs, "schema") == 742 ||
              contract_hash(cs, "schema") == 1519,
          "schema == 1620|742|1519");
    CHECK(contract_hash(cs, "hotpath-invariant-hits") >= 0, "hotpath-invariant-hits present");
    CHECK(contract_hash(cs, "contract-violations-caught") >= 0,
          "contract-violations-caught present");
    CHECK(contract_hash(cs, "consteval-checks") >= 0, "consteval-checks present");

    std::println("\n--- AC2: eval/mutate bumps hotpath-invariant-hits ---");
    const auto hotpath0 = contract_hash(cs, "hotpath-invariant-hits");
    (void)cs.eval("(fact 4)");
    (void)cs.eval("(+ a b)");
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");
    const auto hotpath1 = contract_hash(cs, "hotpath-invariant-hits");
    std::println("  hotpath-invariant-hits: {} -> {}", hotpath0, hotpath1);
    CHECK(hotpath1 > hotpath0, "hotpath-invariant-hits grew after eval/mutate");

    std::println("\n--- AC3: consteval-checks baked (>= 36 lineage, 77 after #1620) ---");
    CHECK(contract_hash(cs, "consteval-checks") >= 36, "consteval-checks >= 36");
    CHECK(contract_hash(cs, "consteval-checks") == 77 ||
              contract_hash(cs, "consteval-checks") == 65 ||
              contract_hash(cs, "consteval-checks") == 36,
          "consteval-checks in known lineage 77|65|36");

    std::println("\n--- AC4: contract-violations-caught zero in normal path ---");
    CHECK(contract_hash(cs, "contract-violations-caught") == 0,
          "no contract violations in normal mutate path");

    std::println("\n--- AC5: multi-round mutate hotpath monotonic ---");
    const auto stats5a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"b\" \"" + std::to_string(20 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(fact 2)");
    }
    const auto stats5b = stats_sum(cs);
    std::println("  cpp26-contracts sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "cpp26-contracts stats monotonic");

    std::println("\n--- AC6: query regression ---");
    auto hp = cs.eval("(engine:metrics \"query:highperf-cpp26-stats\")");
    auto inv = cs.eval("(stats:get \"query:cxx26-invariants\")");
    auto hot = cs.eval("(stats:get \"query:cxx26-hotpath-invariants\")");
    CHECK(hp && is_hash(*hp), "highperf-cpp26-stats regression");
    CHECK(inv && is_hash(*inv), "cxx26-invariants regression");
    CHECK(hot && is_hash(*hot), "cxx26-hotpath-invariants regression");
}

} // namespace aura_issue_742_detail

int aura_issue_cpp26_contracts_hotpath_arena_soa_value_shape_pass_run() {
    aura::compiler::CompilerService cs;
    aura_issue_742_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_cpp26_contracts_hotpath_arena_soa_value_shape_pass_run();
}
#endif
