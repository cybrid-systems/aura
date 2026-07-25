// @category: unit
// @reason: Issue #2113 — incremental soundness oracle (debug partial ≡ full).
//
// Enable docs (AC5):
//   - Compile: -DAURA_INCREMENTAL_SOUNDNESS  (always-on path)
//   - Hard assert: -DAURA_INCREMENTAL_SOUNDNESS_HARD
//   - Debug builds (!NDEBUG): auto-on when mode==0
//   - Runtime on:  set_incremental_soundness_mode(1)
//   - Runtime off: set_incremental_soundness_mode(2)  (zero oracle cost)
//   - Auto:        set_incremental_soundness_mode(0)
//
//   AC1: Intentional under-dirty injection records mismatch
//   AC2: Happy-path equivalent IR → ok (no false positives)
//   AC3: mode=2 disables oracle (enabled() false; no cost path)
//   AC4: Counters + query surface (schema-2113) queryable
//   AC5: Enable docs present in this header + query enable-* keys

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.ir_cache_pure;
import aura.compiler.ir;
import aura.compiler.value;

namespace {

using aura::compiler::check_incremental_soundness;
using aura::compiler::CompilerService;
using aura::compiler::get_incremental_soundness_mode;
using aura::compiler::incremental_soundness_enabled;
using aura::compiler::incremental_soundness_mismatch_atomic;
using aura::compiler::incremental_soundness_ok_atomic;
using aura::compiler::incremental_soundness_runs_atomic;
using aura::compiler::inject_soundness_under_dirty_for_test;
using aura::compiler::ir_equivalent;
using aura::compiler::ir_function_equivalent;
using aura::compiler::reset_incremental_soundness_for_test;
using aura::compiler::set_incremental_soundness_mode;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::ir::BasicBlock;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IROpcode;
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

// Minimal single-block IR: ConstI64 → Return
static IRFunction make_toy_fn(std::uint32_t id = 1) {
    IRFunction fn;
    fn.id = id;
    fn.name = "toy";
    fn.arg_count = 0;
    fn.local_count = 0;
    BasicBlock bb;
    bb.id = 0;
    IRInstruction c{.opcode = IROpcode::ConstI64, .operands = {42, 0, 0, 0}};
    IRInstruction r{.opcode = IROpcode::Return, .operands = {0, 0, 0, 0}};
    bb.instructions.push_back(c);
    bb.instructions.push_back(r);
    fn.blocks.push_back(std::move(bb));
    return fn;
}

static void ac1_under_dirty_mismatch() {
    std::println("\n--- AC1: intentional under-dirty → mismatch ---");
    reset_incremental_soundness_for_test();
    set_incremental_soundness_mode(1);
    CHECK(incremental_soundness_enabled(), "mode 1 → enabled");

    auto partial = make_toy_fn(7);
    auto full = make_toy_fn(7);
    CHECK(ir_equivalent(partial, full), "baseline equivalent");

    inject_soundness_under_dirty_for_test(partial);
    CHECK(!ir_equivalent(partial, full), "inject breaks equivalence");

    const auto runs0 = incremental_soundness_runs_atomic().load();
    const auto mm0 = incremental_soundness_mismatch_atomic().load();
    const bool ok = check_incremental_soundness(partial, full);
    CHECK(!ok, "oracle reports mismatch");
    CHECK(incremental_soundness_runs_atomic().load() == runs0 + 1, "runs++");
    CHECK(incremental_soundness_mismatch_atomic().load() == mm0 + 1, "mismatch++");
    CHECK(incremental_soundness_ok_atomic().load() == 0, "no ok on mismatch path");
}

static void ac2_happy_path() {
    std::println("\n--- AC2: happy-path partial ≡ full ---");
    reset_incremental_soundness_for_test();
    set_incremental_soundness_mode(1);

    auto a = make_toy_fn(3);
    auto b = make_toy_fn(99); // id ignored by ir_function_equivalent
    CHECK(ir_function_equivalent(a, b), "same body different id");
    CHECK(check_incremental_soundness(a, b), "oracle ok");
    CHECK(incremental_soundness_ok_atomic().load() == 1, "ok++");
    CHECK(incremental_soundness_mismatch_atomic().load() == 0, "no mismatch");

    // With oracle forced on, basic eval still works (no false positive wire).
    CompilerService cs;
    auto r = cs.eval("(+ 1 2)");
    CHECK(r.has_value() && is_int(*r) && as_int(*r) == 3, "eval with oracle mode on");
}

static void ac3_zero_cost_when_off() {
    std::println("\n--- AC3: mode=2 disables oracle ---");
    reset_incremental_soundness_for_test();
    set_incremental_soundness_mode(2);
    CHECK(get_incremental_soundness_mode() == 2, "mode=2");
    CHECK(!incremental_soundness_enabled(), "disabled");
    // Gate path: callers must not full-lower when disabled.
    // Source-level: service.ixx gates on incremental_soundness_enabled().
    auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("incremental_soundness_enabled()") != std::string::npos, "service gate");
    CHECK(svc.find("check_incremental_soundness") != std::string::npos, "service check");
    // Force-off: even after a pure compare, process counters only move if called.
    auto a = make_toy_fn();
    auto b = make_toy_fn();
    // Pure compare still works; enable gate is separate.
    CHECK(ir_equivalent(a, b), "pure compare always available");
    CHECK(!incremental_soundness_enabled(), "still off");
}

static void ac4_query_surface() {
    std::println("\n--- AC4: counters + query surface ---");
    reset_incremental_soundness_for_test();
    set_incremental_soundness_mode(1);
    auto a = make_toy_fn();
    auto b = make_toy_fn();
    CHECK(check_incremental_soundness(a, b), "ok run");
    inject_soundness_under_dirty_for_test(a);
    CHECK(!check_incremental_soundness(a, b), "mismatch run");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
    CHECK(href(cs, "query:incremental-soundness-stats", "schema-2113") == 2113,
          "soundness schema-2113");
    CHECK(href(cs, "query:incremental-soundness-stats", "incremental-soundness-wired") == 1,
          "soundness wired");
    CHECK(href(cs, "query:incremental-soundness-stats", "incremental-soundness-runs") >= 2,
          "runs queryable");
    CHECK(href(cs, "query:incremental-soundness-stats", "incremental-soundness-ok") >= 1,
          "ok queryable");
    CHECK(href(cs, "query:incremental-soundness-stats", "incremental-soundness-mismatch") >= 1,
          "mismatch queryable");
    CHECK(href(cs, "query:incremental-soundness-stats", "incremental-soundness-enabled") == 1,
          "enabled flag");
    // Also on incremental-relower-stats
    CHECK(href(cs, "query:incremental-relower-stats", "schema-2113") == 2113, "relower schema");
    CHECK(href(cs, "query:incremental-relower-stats", "incremental-soundness-wired") == 1,
          "relower wired");
}

static void ac5_docs_and_wiring() {
    std::println("\n--- AC5: enable docs + source wiring ---");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    auto svc = read_file("src/compiler/service.ixx");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    auto metrics = read_file("src/compiler/observability_metrics.h");
    CHECK(pure.find("Issue #2113") != std::string::npos || pure.find("#2113") != std::string::npos,
          "pure cites #2113");
    CHECK(pure.find("ir_equivalent") != std::string::npos, "ir_equivalent");
    CHECK(pure.find("check_incremental_soundness") != std::string::npos, "check helper");
    CHECK(pure.find("AURA_INCREMENTAL_SOUNDNESS") != std::string::npos, "compile flag doc");
    CHECK(pure.find("set_incremental_soundness_mode") != std::string::npos, "runtime mode");
    CHECK(pure.find("inject_soundness_under_dirty_for_test") != std::string::npos, "inject");
    CHECK(svc.find("Issue #2113") != std::string::npos || svc.find("#2113") != std::string::npos,
          "service cites #2113");
    CHECK(q.find("query:incremental-soundness-stats") != std::string::npos, "dedicated query");
    CHECK(q.find("schema-2113") != std::string::npos, "schema-2113");
    CHECK(metrics.find("incremental_soundness_runs_total") != std::string::npos, "metrics field");
    CHECK(metrics.find("incremental_soundness_mismatch_total") != std::string::npos,
          "mismatch field");

    CompilerService cs;
    CHECK(cs.eval("(+ 0 0)").has_value(), "eval");
    // AC5 enable-docs keys on dedicated query
    CHECK(href(cs, "query:incremental-soundness-stats", "enable-runtime-mode-on") == 1,
          "enable mode on doc");
    CHECK(href(cs, "query:incremental-soundness-stats", "enable-runtime-mode-off") == 2,
          "enable mode off doc");
    CHECK(href(cs, "query:incremental-soundness-stats", "enable-compile-flag") == 1,
          "compile flag doc");
}

} // namespace

int main() {
    std::println("=== Issue #2113: incremental soundness oracle ===");
    ac1_under_dirty_mismatch();
    ac2_happy_path();
    ac3_zero_cost_when_off();
    ac4_query_surface();
    ac5_docs_and_wiring();
    reset_incremental_soundness_for_test();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
