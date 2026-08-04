// @category: unit
// @reason: Issue #2210 — JIT / Interpreter PrimCall + apply_closure
// dual-path semantic equivalence oracle (analogous to #2113).
//
//   AC1: Oracle is zero-cost when disabled (mode 0/2).
//   AC2: When enabled, known divergent PrimCall (inject) is detected
//        and metric bumped.
//   AC3: apply_closure dual-path under same fingerprint yields ok or
//        is reported on mismatch.
//   AC4: Query surface with jit_equivalence_runs / ok / mismatch +
//        schema-2210.
//   AC5: Production default mode 0 — no behaviour change.

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.ir_cache_pure;
import aura.compiler.value;

namespace {

using aura::compiler::check_apply_closure_equivalence;
using aura::compiler::check_primcall_equivalence;
using aura::compiler::CompilerService;
using aura::compiler::EquivResultBits;
using aura::compiler::get_jit_equivalence_mode;
using aura::compiler::inject_jit_equivalence_mismatch_for_test;
using aura::compiler::jit_equivalence_deopt_force_total_atomic;
using aura::compiler::jit_equivalence_enabled;
using aura::compiler::jit_equivalence_mismatch_atomic;
using aura::compiler::jit_equivalence_ok_atomic;
using aura::compiler::jit_equivalence_runs_atomic;
using aura::compiler::make_equiv_int_bits;
using aura::compiler::reset_jit_equivalence_for_test;
using aura::compiler::sample_jit_equivalence_if_enabled;
using aura::compiler::set_jit_equivalence_mode;
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

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// AC5 + AC1: production default off; mode 0/2 zero-cost
static void ac1_zero_cost_when_off() {
    std::println("\n--- AC1/AC5: default off; mode 0/2 zero-cost ---");
    reset_jit_equivalence_for_test();
    CHECK(get_jit_equivalence_mode() == 0, "AC5: production default mode 0");
    CHECK(!jit_equivalence_enabled(), "AC5: not enabled by default");

    const auto runs0 = jit_equivalence_runs_atomic().load();
    // sample when disabled: must not bump counters
    CHECK(sample_jit_equivalence_if_enabled(make_equiv_int_bits(1), make_equiv_int_bits(2)),
          "disabled sample returns true without compare work");
    CHECK(jit_equivalence_runs_atomic().load() == runs0, "AC1: mode 0 no runs++");

    set_jit_equivalence_mode(2);
    CHECK(!jit_equivalence_enabled(), "mode 2 force off");
    CHECK(sample_jit_equivalence_if_enabled(make_equiv_int_bits(1), make_equiv_int_bits(99)),
          "mode 2 still skips");
    CHECK(jit_equivalence_runs_atomic().load() == runs0, "AC1: mode 2 no runs++");
}

// AC2: inject divergent PrimCall → mismatch
static void ac2_inject_mismatch() {
    std::println("\n--- AC2: inject divergent PrimCall → mismatch ---");
    reset_jit_equivalence_for_test();
    set_jit_equivalence_mode(1);
    CHECK(jit_equivalence_enabled(), "mode 1 on");

    auto a = make_equiv_int_bits(42);
    auto b = make_equiv_int_bits(42);
    CHECK(check_primcall_equivalence(a, b), "equal PrimCall ok");
    CHECK(jit_equivalence_ok_atomic().load() == 1, "ok++");
    CHECK(jit_equivalence_mismatch_atomic().load() == 0, "no mismatch");

    inject_jit_equivalence_mismatch_for_test();
    const auto mm0 = jit_equivalence_mismatch_atomic().load();
    const auto runs0 = jit_equivalence_runs_atomic().load();
    CHECK(!check_primcall_equivalence(a, b), "AC2: inject → mismatch");
    CHECK(jit_equivalence_runs_atomic().load() == runs0 + 1, "runs++");
    CHECK(jit_equivalence_mismatch_atomic().load() == mm0 + 1, "mismatch++");

    // Real divergence without inject
    auto c = make_equiv_int_bits(7);
    auto d = make_equiv_int_bits(8);
    CHECK(!check_primcall_equivalence(c, d), "real bits diverge");
    CHECK(jit_equivalence_mismatch_atomic().load() >= mm0 + 2, "mismatch again");
}

// AC3: apply_closure dual-path
static void ac3_apply_closure_dual() {
    std::println("\n--- AC3: apply_closure dual-path equivalence ---");
    reset_jit_equivalence_for_test();
    set_jit_equivalence_mode(1);

    // Same EnvFrame / bridge_epoch → same result bits
    auto path_a = make_equiv_int_bits(100);
    auto path_b = make_equiv_int_bits(100);
    CHECK(check_apply_closure_equivalence(path_a, path_b), "AC3: dual-path ok");
    CHECK(jit_equivalence_ok_atomic().load() >= 1, "ok on apply");

    auto path_c = make_equiv_int_bits(1);
    auto path_d = make_equiv_int_bits(2);
    CHECK(!check_apply_closure_equivalence(path_c, path_d), "AC3: dual-path mismatch reported");
    CHECK(jit_equivalence_mismatch_atomic().load() >= 1, "mismatch counted");

    // sample_jit_equivalence_if_enabled forces deopt signal on mismatch
    const auto deopt0 = jit_equivalence_deopt_force_total_atomic().load();
    CHECK(!sample_jit_equivalence_if_enabled(path_c, path_d), "sample mismatch");
    CHECK(jit_equivalence_deopt_force_total_atomic().load() > deopt0, "deopt force++");
}

// AC4: query surface schema-2210
static void ac4_query_schema() {
    std::println("\n--- AC4: query jit_equivalence_* + schema-2210 ---");
    reset_jit_equivalence_for_test();
    set_jit_equivalence_mode(1);
    (void)check_primcall_equivalence(make_equiv_int_bits(1), make_equiv_int_bits(1));
    inject_jit_equivalence_mismatch_for_test();
    (void)check_primcall_equivalence(make_equiv_int_bits(1), make_equiv_int_bits(1));

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    // Prefer incremental-relower-stats (schema-2210 keys live there)
    const auto q = "query:incremental-relower-stats";
    CHECK(href(cs, q, "schema-2210") == 2210, "schema-2210");
    CHECK(href(cs, q, "issue-2210") == 2210, "issue-2210");
    CHECK(href(cs, q, "jit-equivalence-wired") == 1, "wired");
    CHECK(href(cs, q, "jit-equivalence-enabled") == 1, "enabled on query");
    CHECK(href(cs, q, "jit-equivalence-mode") == 1, "mode on query");
    CHECK(href(cs, q, "jit_equivalence_runs_total") >= 1, "runs");
    CHECK(href(cs, q, "jit_equivalence_ok_total") >= 1 || href(cs, q, "jit-equivalence-ok") >= 1,
          "ok");
    CHECK(href(cs, q, "jit_equivalence_mismatch_total") >= 1 ||
              href(cs, q, "jit-equivalence-mismatch") >= 1,
          "mismatch");
    CHECK(href(cs, q, "schema-2113") == 2113, "2113 lineage retained");

    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    auto met = read_file("src/compiler/observability_metrics.h");
    auto qsrc = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    auto bnd = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(pure.find("#2210") != std::string::npos, "pure cites #2210");
    CHECK(pure.find("check_primcall_equivalence") != std::string::npos, "primcall helper");
    CHECK(pure.find("check_apply_closure_equivalence") != std::string::npos, "apply helper");
    CHECK(met.find("jit_equivalence_runs_total") != std::string::npos, "metrics fields");
    CHECK(qsrc.find("schema-2210") != std::string::npos, "query schema");
    CHECK(bnd.find("#2210") != std::string::npos, "reemit sample site wired");
}

// Happy path eval still works with oracle on
static void ac_extra_eval_with_oracle() {
    std::println("\n--- extra: eval with oracle on ---");
    reset_jit_equivalence_for_test();
    set_jit_equivalence_mode(1);
    CompilerService cs;
    auto r = cs.eval("(+ 10 20)");
    CHECK(r.has_value() && is_int(*r) && as_int(*r) == 30, "eval under oracle mode 1");
    reset_jit_equivalence_for_test();
}

} // namespace

int run_test_jit_interpreter_equivalence_oracle() {
    std::println("=== Issue #2210: JIT/Interpreter equivalence oracle ===");
    ac1_zero_cost_when_off();
    ac2_inject_mismatch();
    ac3_apply_closure_dual();
    ac4_query_schema();
    ac_extra_eval_with_oracle();
    reset_jit_equivalence_for_test();
    std::println("\n=== results: {} passed, {} failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_jit_interpreter_equivalence_oracle();
}
#endif
