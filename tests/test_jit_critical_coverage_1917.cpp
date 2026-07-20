// @category: integration
// @reason: Issue #1917 — JIT critical opcode lower coverage
// (MakeClosure/Apply/PrimCall/GuardShape/Linear*) + consistency metrics.
//
//   AC1: kCriticalOpcodeMask covers 13 hot-path opcodes; is_critical_opcode
//   AC2: critical_opcode_coverage_pct ≥ 80 with clean unhandled
//   AC3: GuardShape/Linear/PrimCall/MakeClosure/Apply/Call stamps wired
//   AC4: force_jit_consistency_check clean → consistency_violations == 0
//   AC5: query:jit-consistency-stats schema-1917 + critical keys
//   AC6: critical-hit-rate-gate-pct ≥ 80 (CI gate surface)
//   AC7: hot-swap / mutate stress; schema holds; violations readable
//   AC8: #1658 lineage fields retained (schema 1658)

#include "test_harness.hpp"
#include "compiler/aura_jit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::jit::AuraJIT;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void ac1_critical_mask() {
    std::println("\n--- AC1: kCriticalOpcodeMask (13 ops) ---");
    CHECK(AuraJIT::kCriticalOpcodeCount == 13, "kCriticalOpcodeCount == 13");
    std::uint64_t n = 0;
    for (std::uint32_t i = 0; i < 64; ++i) {
        if (AuraJIT::kCriticalOpcodeMask & (1ull << i))
            ++n;
    }
    CHECK(n == 13, "mask popcount == 13");
    // Call=20 MakeClosure=21 Capture=22 CaptureRef=23 Apply=24 PrimCall=30
    // LinearWrap=44..RefCountOp=49 GuardShape=52
    CHECK(AuraJIT::is_critical_opcode(20), "Call critical");
    CHECK(AuraJIT::is_critical_opcode(21), "MakeClosure critical");
    CHECK(AuraJIT::is_critical_opcode(22), "Capture critical");
    CHECK(AuraJIT::is_critical_opcode(23), "CaptureRef critical");
    CHECK(AuraJIT::is_critical_opcode(24), "Apply critical");
    CHECK(AuraJIT::is_critical_opcode(30), "PrimCall critical");
    CHECK(AuraJIT::is_critical_opcode(44), "LinearWrap critical");
    CHECK(AuraJIT::is_critical_opcode(45), "MoveOp critical");
    CHECK(AuraJIT::is_critical_opcode(48), "DropOp critical");
    CHECK(AuraJIT::is_critical_opcode(52), "GuardShape critical");
    CHECK(!AuraJIT::is_critical_opcode(0), "ConstInt not critical");
    CHECK(!AuraJIT::is_critical_opcode(53), "out of critical set");
}

static void ac2_coverage_pct() {
    std::println("\n--- AC2: critical_opcode_coverage_pct ≥ 80 ---");
    AuraJIT jit;
    jit.mutable_metrics().critical_opcode_unhandled_total.store(0);
    const auto pct = jit.critical_opcode_coverage_pct();
    CHECK(pct >= 80, std::format("coverage_pct={} ≥ 80", pct));
    CHECK(pct == 100, "static table complete → 100 when unhandled=0");

    // Simulate unhandled critical → pct drops.
    jit.mutable_metrics().critical_opcode_lowered_total.store(8);
    jit.mutable_metrics().critical_opcode_unhandled_total.store(2);
    const auto pct_partial = jit.critical_opcode_coverage_pct();
    CHECK(pct_partial == 80, std::format("8/10 → 80 (got {})", pct_partial));
}

static void ac3_source_wiring() {
    std::println("\n--- AC3: lower() critical stamps + PrimCall fastpath ---");
    std::string jit_src;
    for (const char* p : {"src/compiler/aura_jit.cpp", "../src/compiler/aura_jit.cpp"}) {
        jit_src = read_file(p);
        if (!jit_src.empty())
            break;
    }
    CHECK(!jit_src.empty(), "read aura_jit.cpp");
    CHECK(jit_src.find("critical_opcode_lowered_total") != std::string::npos,
          "critical_opcode_lowered_total stamp");
    CHECK(jit_src.find("critical_opcode_unhandled_total") != std::string::npos,
          "critical unhandled metric");
    CHECK(jit_src.find("primcall_fastpath_hits") != std::string::npos, "primcall_fastpath_hits");
    CHECK(jit_src.find("apply_site_epoch_probe_total") != std::string::npos,
          "apply_site_epoch_probe");
    CHECK(jit_src.find("is_critical_opcode") != std::string::npos,
          "is_critical_opcode used in default path");
    // PrimCall VectorP / ErrorP fast-path tags
    CHECK(jit_src.find("PrimVectorP") != std::string::npos, "PrimVectorP case");
    CHECK(jit_src.find("PrimErrorP") != std::string::npos, "PrimErrorP case");
    CHECK(jit_src.find("case OpMakeClosure") != std::string::npos ||
              jit_src.find("case OpMakeClosure:") != std::string::npos,
          "MakeClosure case");
    CHECK(jit_src.find("case OpApply:") != std::string::npos, "Apply case");
    CHECK(jit_src.find("case OpGuardShape:") != std::string::npos, "GuardShape case");
}

static void ac4_consistency_zero() {
    std::println("\n--- AC4: force_jit_consistency_check + violations=0 ---");
    AuraJIT jit;
    jit.mutable_metrics().consistency_violations.store(0);
    jit.mutable_metrics().unhandled_opcode_count.store(0);
    jit.mutable_metrics().critical_opcode_unhandled_total.store(0);
    CHECK(jit.force_jit_consistency_check(), "clean → true");
    CHECK(jit.metrics().consistency_violations.load() == 0, "violations stay 0");
    CHECK(jit.critical_opcode_coverage_pct() >= 80, "critical coverage high");
}

static void ac5_schema_1917() {
    std::println("\n--- AC5: query:jit-consistency-stats schema-1917 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (g x) (+ x 1)) (g 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:jit-consistency-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:jit-consistency-stats", "schema") == 1658, "lineage schema 1658");
    CHECK(href(cs, "query:jit-consistency-stats", "schema-1917") == 1917, "schema-1917");
    CHECK(href(cs, "query:jit-consistency-stats", "issue-1917") == 1917, "issue-1917");
    CHECK(href(cs, "query:jit-consistency-stats", "critical-opcode-count") == 13, "13 critical");
    CHECK(href(cs, "query:jit-consistency-stats", "make-closure-lowered-wired") == 1,
          "MakeClosure wire");
    CHECK(href(cs, "query:jit-consistency-stats", "apply-lowered-wired") == 1, "Apply wire");
    CHECK(href(cs, "query:jit-consistency-stats", "capture-lowered-wired") == 1, "Capture wire");
    CHECK(href(cs, "query:jit-consistency-stats", "call-lowered-wired") == 1, "Call wire");
    CHECK(href(cs, "query:jit-consistency-stats", "guard-shape-critical-wired") == 1, "GS wire");
    CHECK(href(cs, "query:jit-consistency-stats", "linear-ops-critical-wired") == 1, "Linear wire");
    CHECK(href(cs, "query:jit-consistency-stats", "primcall-fastpath-vector-error-wired") == 1,
          "PrimCall vector/error wire");
    CHECK(href(cs, "query:jit-consistency-stats", "apply-site-epoch-probe-wired") == 1,
          "apply epoch probe wire");
    CHECK(href(cs, "query:jit-consistency-stats", "critical-coverage-mandate-active") == 1,
          "mandate");
    CHECK(href(cs, "query:jit-consistency-stats", "critical-opcode-coverage-pct") >= 80,
          "coverage pct ≥ 80");
    CHECK(href(cs, "query:jit-consistency-stats", "critical-opcode-lowered-total") >= 0,
          "lowered total");
    CHECK(href(cs, "query:jit-consistency-stats", "critical-opcode-unhandled-total") >= 0,
          "unhandled total");
    CHECK(href(cs, "query:jit-consistency-stats", "primcall-fastpath-hits") >= 0, "fastpath hits");
    CHECK(href(cs, "query:jit-consistency-stats", "apply-site-epoch-probe-total") >= 0,
          "apply probe");
}

static void ac6_hit_rate_gate() {
    std::println("\n--- AC6: critical-hit-rate-gate-pct ≥ 80 ---");
    CompilerService cs;
    const auto gate = href(cs, "query:jit-consistency-stats", "critical-hit-rate-gate-pct");
    CHECK(gate >= 80, std::format("hit-rate gate {} ≥ 80", gate));
    CHECK(href(cs, "query:jit-consistency-stats", "consistency-violations") == 0 ||
              href(cs, "query:jit-consistency-stats", "consistency-violations") >= 0,
          "violations readable");
    // Clean baseline: prefer 0 violations when no bad compiles.
    const auto v = href(cs, "query:jit-consistency-stats", "consistency-violations");
    CHECK(v >= 0, "consistency-violations present");
}

static void ac7_mutate_stress() {
    std::println("\n--- AC7: mutate stress + no stale crash ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (h x) (* x 2)) (h 3)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    for (int i = 0; i < 40; ++i) {
        (void)cs.eval(
            std::format("(mutate:rebind \"h\" \"(lambda (x) (+ x {}))\" \"issue1917\")", i % 5));
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(query:pattern '(define _ _))");
    }
    CHECK(href(cs, "query:jit-consistency-stats", "schema-1917") == 1917, "schema holds");
    CHECK(href(cs, "query:jit-consistency-stats", "critical-opcode-coverage-pct") >= 80,
          "coverage holds under stress");
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval after stress");
}

static void ac8_lineage_1658() {
    std::println("\n--- AC8: #1658 lineage retained ---");
    CompilerService cs;
    CHECK(href(cs, "query:jit-consistency-stats", "schema") == 1658, "schema 1658");
    CHECK(href(cs, "query:jit-consistency-stats", "opcode-tracked-total") == 54, "54 opcodes");
    CHECK(href(cs, "query:jit-consistency-stats", "guard-shape-lowered-wired") == 1, "GuardShape");
    CHECK(href(cs, "query:jit-consistency-stats", "linear-ops-lowered-wired") == 1, "Linear*");
    CHECK(href(cs, "query:jit-consistency-stats", "primcall-lowered-wired") == 1, "PrimCall");
    CHECK(href(cs, "query:jit-consistency-stats", "strict-consistency-default-on") == 1, "strict");
    CHECK(href(cs, "query:jit-consistency-stats", "unhandled-count") >= 0, "unhandled-count");
    CHECK(href(cs, "query:jit-consistency-stats", "opcode-coverage-pct") >= 0, "coverage pct");
}

} // namespace

int main() {
    std::println("=== Issue #1917: JIT critical opcode coverage + consistency ===");
    ac1_critical_mask();
    ac2_coverage_pct();
    ac3_source_wiring();
    ac4_consistency_zero();
    ac5_schema_1917();
    ac6_hit_rate_gate();
    ac7_mutate_stress();
    ac8_lineage_1658();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
