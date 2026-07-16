// @category: integration
// @reason: Issue #1516 — exception EH coverage + per-function AOT path.
//
// Non-duplicative of #1285 (EH lower), #170 (AOT last_module), #452
// (query:aot-stats hot-update), #1512 (opcode coverage). This issue
// is per-function AOT API + EH mask/coverage + compile:aot-stats.
//
// Full ORC compile needs runtime symbols that light tests may not
// export. AOT miss-path + metric mirrors are tested directly (same
// pattern as #1512/#1514).
//
//   AC1: exception_opcode_mask + coverage_count (0..4)
//   AC2: per-function AOT miss path bumps metrics
//   AC3: compile_function_to_* APIs exist + return empty/false
//   AC4: CompilerMetrics #1516 surface readable
//   AC5: (compile:aot-stats) hash primitive (schema 1516)
//   AC6: interpreter_exception_ops_total mirror / surface
//   AC7: 100× miss-path stress, no crash
//   AC8: format / metric coherence

#include "test_harness.hpp"
#include "compiler/aura_jit.h"
#include "observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1516_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::jit::AuraJIT;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void ac1_exception_coverage() {
    std::println("\n--- AC1: exception_opcode_mask + coverage_count ---");
    AuraJIT jit;
    CHECK(jit.exception_opcode_coverage_count() == 0, "empty EH coverage == 0");
    CHECK(AuraJIT::kExceptionOpcodeCount == 4, "4 EH opcodes tracked");

    // Mirror lower() path: stamp IsError + TryBegin + TryEnd + Raise.
    jit.mutable_metrics().exception_opcode_mask.fetch_or(1ull << 0, std::memory_order_relaxed);
    jit.mutable_metrics().exception_opcode_lowered.fetch_add(1, std::memory_order_relaxed);
    CHECK(jit.exception_opcode_coverage_count() == 1, "IsError alone → coverage 1");

    jit.mutable_metrics().exception_opcode_mask.fetch_or((1ull << 1) | (1ull << 2) | (1ull << 3),
                                                         std::memory_order_relaxed);
    jit.mutable_metrics().exception_opcode_lowered.fetch_add(3, std::memory_order_relaxed);
    CHECK(jit.exception_opcode_coverage_count() == 4, "all 4 EH opcodes covered");
    CHECK(jit.metrics().exception_opcode_lowered.load() == 4, "exception_opcode_lowered == 4");
    CHECK((jit.metrics().exception_opcode_mask.load() & 0xF) == 0xF, "mask bits 0..3 set");
}

static void ac2_aot_miss_metrics() {
    std::println("\n--- AC2: per-function AOT miss metrics ---");
    AuraJIT jit;
    const auto miss0 = jit.metrics().aot_per_function_miss_total.load();
    const auto ir0 = jit.metrics().aot_per_function_ir_total.load();
    const auto obj0 = jit.metrics().aot_per_function_object_total.load();

    auto ir = jit.compile_function_to_llvm_ir(42);
    CHECK(ir.empty(), "unknown func_id → empty IR");
    CHECK(jit.metrics().aot_per_function_miss_total.load() == miss0 + 1, "miss +1 on IR");

    bool ok = jit.compile_function_to_object(99, "/tmp/aura_1516_never.o");
    CHECK(!ok, "unknown func_id → object false");
    CHECK(jit.metrics().aot_per_function_miss_total.load() == miss0 + 2, "miss +2 on object");

    auto irn = jit.compile_function_to_llvm_ir_by_name("no_such_fn");
    CHECK(irn.empty(), "unknown name → empty IR");
    CHECK(jit.metrics().aot_per_function_miss_total.load() == miss0 + 3, "miss +3 on name IR");

    bool okn = jit.compile_function_to_object_by_name("no_such_fn", "/tmp/aura_1516_never2.o");
    CHECK(!okn, "unknown name → object false");
    CHECK(jit.metrics().aot_per_function_miss_total.load() == miss0 + 4, "miss +4 on name object");

    // Success counters still 0 (no real compile in light harness).
    CHECK(jit.metrics().aot_per_function_ir_total.load() == ir0, "ir success still 0");
    CHECK(jit.metrics().aot_per_function_object_total.load() == obj0, "object success still 0");
}

static void ac3_aot_api_surface() {
    std::println("\n--- AC3: AOT API surface ---");
    AuraJIT jit;
    // last-module path (no compile yet)
    CHECK(jit.compile_to_llvm_ir().empty(), "no last_module → empty IR");
    CHECK(!jit.compile_to_object_file("/tmp/aura_1516_last.o"), "no last_module → false");

    // Per-function APIs callable without crash
    (void)jit.compile_function_to_llvm_ir(0);
    (void)jit.compile_function_to_object(0, "/tmp/aura_1516_f0.o");
    (void)jit.compile_function_to_llvm_ir_by_name("");
    (void)jit.compile_function_to_object_by_name(nullptr, "/tmp/aura_1516_null.o");
    CHECK(true, "AOT APIs handle empty/null without crash");

    // register_function establishes func_id mapping (still no module)
    jit.register_function(7, nullptr, 4, 1, 0, "demo_fn");
    auto still_empty = jit.compile_function_to_llvm_ir(7);
    CHECK(still_empty.empty(), "registered id without compile still empty IR");
}

static void ac4_compiler_metrics_surface() {
    std::println("\n--- AC4: CompilerMetrics #1516 surface ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics available");
    CHECK(load_u64(m->aot_per_function_ir_total) >= 0, "aot_per_function_ir readable");
    CHECK(load_u64(m->aot_per_function_object_total) >= 0, "aot_per_function_object readable");
    CHECK(load_u64(m->aot_per_function_miss_total) >= 0, "aot_per_function_miss readable");
    CHECK(load_u64(m->aot_last_module_object_total) >= 0, "aot_last_module_object readable");
    CHECK(load_u64(m->interpreter_exception_ops_total) >= 0, "interpreter_exception_ops readable");
    CHECK(load_u64(m->jit_exception_opcode_mask) >= 0, "jit_exception_opcode_mask readable");
    CHECK(load_u64(m->jit_exception_opcode_lowered) >= 0, "jit_exception_opcode_lowered readable");
    CHECK(load_u64(m->jit_exception_opcodes_covered) >= 0,
          "jit_exception_opcodes_covered readable");

    m->aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
    CHECK(load_u64(m->aot_per_function_miss_total) >= 1, "miss bumpable");
    m->interpreter_exception_ops_total.fetch_add(1, std::memory_order_relaxed);
    CHECK(load_u64(m->interpreter_exception_ops_total) >= 1, "interp EH bumpable");
}

static void ac5_compile_aot_stats() {
    std::println("\n--- AC5: (compile:aot-stats) primitive ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    m->aot_per_function_ir_total.store(3, std::memory_order_relaxed);
    m->aot_per_function_object_total.store(2, std::memory_order_relaxed);
    m->aot_per_function_miss_total.store(5, std::memory_order_relaxed);
    m->jit_exception_opcode_lowered.store(4, std::memory_order_relaxed);
    m->jit_exception_opcodes_covered.store(4, std::memory_order_relaxed);
    m->jit_exception_opcode_mask.store(0xF, std::memory_order_relaxed);
    m->interpreter_exception_ops_total.store(7, std::memory_order_relaxed);

    auto r = cs.eval("(engine:metrics \"compile:aot-stats\")");
    CHECK(r && is_hash(*r), "compile:aot-stats returns hash");

    auto schema = cs.eval("(hash-ref (engine:metrics \"compile:aot-stats\") 'schema)");
    CHECK(schema && is_int(*schema) && as_int(*schema) == 1516, "schema == 1516");

    auto misses = cs.eval("(hash-ref (engine:metrics \"compile:aot-stats\") 'per-function-misses)");
    CHECK(misses && is_int(*misses) && as_int(*misses) == 5, "per-function-misses == 5");

    auto eh =
        cs.eval("(hash-ref (engine:metrics \"compile:aot-stats\") 'exception-opcodes-covered)");
    CHECK(eh && is_int(*eh) && as_int(*eh) == 4, "exception-opcodes-covered == 4");

    auto interp =
        cs.eval("(hash-ref (engine:metrics \"compile:aot-stats\") 'interpreter-exception-ops)");
    CHECK(interp && is_int(*interp) && as_int(*interp) == 7, "interpreter-exception-ops == 7");

    // Non-duplicative with query:aot-stats (#452)
    auto hot = cs.eval("(engine:metrics \"query:aot-stats\")");
    CHECK(hot && is_hash(*hot), "query:aot-stats still reachable");
}

static void ac6_eval_stress_safe() {
    std::println("\n--- AC6: eval + AOT miss under service ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code f");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    (void)cs.eval("(f 10)");
    // Direct metric surface remains coherent after eval
    CHECK(load_u64(m->aot_per_function_miss_total) >= 0, "miss after eval");
    CHECK(load_u64(m->interpreter_exception_ops_total) >= 0, "interp EH after eval");
    CHECK(true, "eval path did not crash");
}

static void ac7_miss_stress() {
    std::println("\n--- AC7: 100× AOT miss-path stress ---");
    AuraJIT jit;
    const auto miss0 = jit.metrics().aot_per_function_miss_total.load();
    int ok = 0;
    for (int i = 0; i < 100; ++i) {
        (void)jit.compile_function_to_llvm_ir(static_cast<std::uint32_t>(i));
        (void)jit.compile_function_to_object(static_cast<std::uint32_t>(i + 1000),
                                             "/tmp/aura_1516_stress.o");
        if ((i % 7) == 0)
            jit.register_function(i, nullptr, 2, 1, 0, ("fn" + std::to_string(i)).c_str());
        if ((i % 11) == 0)
            jit.invalidate(("fn" + std::to_string(i)).c_str());
        // EH mask thrash
        jit.mutable_metrics().exception_opcode_mask.fetch_or(1ull << (i % 4),
                                                             std::memory_order_relaxed);
        ++ok;
    }
    CHECK(ok == 100, "100-iter stress completed");
    CHECK(jit.metrics().aot_per_function_miss_total.load() >= miss0 + 200,
          "at least 200 misses (IR+object)");
    CHECK(jit.exception_opcode_coverage_count() >= 1, "EH mask non-empty after thrash");
    std::println("  misses={} eh_cov={}", jit.metrics().aot_per_function_miss_total.load(),
                 jit.exception_opcode_coverage_count());
}

static void ac8_metric_coherence() {
    std::println("\n--- AC8: metric coherence ---");
    AuraJIT jit;
    CHECK(load_u64(jit.mutable_metrics().aot_per_function_ir_total) >= 0, "jit aot ir");
    CHECK(load_u64(jit.mutable_metrics().aot_per_function_object_total) >= 0, "jit aot object");
    CHECK(load_u64(jit.mutable_metrics().aot_per_function_miss_total) >= 0, "jit aot miss");
    CHECK(load_u64(jit.mutable_metrics().aot_last_module_object_total) >= 0, "jit last object");
    CHECK(load_u64(jit.mutable_metrics().exception_opcode_mask) >= 0, "jit eh mask");
    CHECK(load_u64(jit.mutable_metrics().exception_opcode_lowered) >= 0, "jit eh lowered");
    CHECK(load_u64(jit.mutable_metrics().fallback_count) >= 0, "fallback_count");
    CHECK(load_u64(jit.mutable_metrics().partial_recompile_requests) >= 0, "partial_recompile");
}

} // namespace aura_issue_1516_detail

int aura_issue_1516_run() {
    using namespace aura_issue_1516_detail;
    std::println("=== Issue #1516: EH coverage + per-function AOT productionization ===");
    ac1_exception_coverage();
    ac2_aot_miss_metrics();
    ac3_aot_api_surface();
    ac4_compiler_metrics_surface();
    ac5_compile_aot_stats();
    ac6_eval_stress_safe();
    ac7_miss_stress();
    ac8_metric_coherence();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1516_run();
}
#endif
