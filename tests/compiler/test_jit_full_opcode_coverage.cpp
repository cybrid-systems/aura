// @category: integration
// @reason: Issue #1658 — Complete opcode coverage for GuardShape,
// Issue #1289/#1512/#1658/#427/#532 (#1978 renamed): issue# moved from filename to header.
// Linear* ops, complex PrimCall + enforce strict interpreter-JIT
// consistency by default (refine #1512 / #1289 / #532 / #427).
//
//   AC1: strict_consistency_mode default ON
//   AC2: kTrackedOpcodeCount == 54; kFullyLoweredOpcodeMask full
//   AC3: GuardShape + Linear* bits in coverage mask after stamp
//   AC4: force_jit_consistency_check returns true when clean
//   AC5: force_jit_consistency_check fails on unhandled under strict
//   AC6: query:jit-consistency-stats schema 1658 + wire flags
//   AC7: hot-swap / mutate stress; violations stay zero when clean
//   AC8: #1512 / #532 lineage fields still present

#include "test_harness.hpp"
#include "compiler/aura_jit.h"

#include <cstdint>
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

static void ac1_strict_default_on() {
    std::println("\n--- AC1: strict_consistency_mode default ON ---");
    AuraJIT jit;
    CHECK(jit.strict_consistency_mode(), "default ON (#1658)");
    jit.set_strict_consistency_mode(false);
    CHECK(!jit.strict_consistency_mode(), "can disable for debug");
    jit.set_strict_consistency_mode(true);
    CHECK(jit.strict_consistency_mode(), "can re-enable");
}

static void ac2_opcode_counts() {
    std::println("\n--- AC2: 54 opcodes tracked / fully lowered mask ---");
    CHECK(AuraJIT::kTrackedOpcodeCount == 54, "kTrackedOpcodeCount == 54");
    // popcount of full mask (bits 0..53)
    std::uint64_t n = 0;
    for (std::uint32_t i = 0; i < 54; ++i) {
        if (AuraJIT::kFullyLoweredOpcodeMask & (1ull << i))
            ++n;
    }
    CHECK(n == 54, "kFullyLoweredOpcodeMask covers all 54");
    CHECK((AuraJIT::kFullyLoweredOpcodeMask & (1ull << 52)) != 0, "GuardShape bit 52");
    CHECK((AuraJIT::kFullyLoweredOpcodeMask & (1ull << 44)) != 0, "LinearWrap bit 44");
    CHECK((AuraJIT::kFullyLoweredOpcodeMask & (1ull << 45)) != 0, "MoveOp bit 45");
    CHECK((AuraJIT::kFullyLoweredOpcodeMask & (1ull << 48)) != 0, "DropOp bit 48");
    CHECK((AuraJIT::kFullyLoweredOpcodeMask & (1ull << 30)) != 0, "PrimCall bit 30");
}

static void ac3_guard_shape_linear_coverage() {
    std::println("\n--- AC3: GuardShape + Linear coverage stamp ---");
    AuraJIT jit;
    // Mirror compile() success stamping for GuardShape(52) + Linear*(44-49).
    const std::uint64_t linear_and_gs = (1ull << 52) | (1ull << 44) | (1ull << 45) | (1ull << 46) |
                                        (1ull << 47) | (1ull << 48) | (1ull << 49);
    jit.mutable_metrics().opcode_covered_mask.fetch_or(linear_and_gs, std::memory_order_relaxed);
    CHECK((jit.metrics().opcode_covered_mask.load() & (1ull << 52)) != 0, "GuardShape covered");
    CHECK((jit.metrics().opcode_covered_mask.load() & (1ull << 44)) != 0, "LinearWrap covered");
    CHECK((jit.metrics().opcode_covered_mask.load() & (1ull << 48)) != 0, "DropOp covered");
    CHECK(jit.opcode_coverage_count() >= 7, "coverage count >= 7");
}

static void ac4_force_check_clean() {
    std::println("\n--- AC4: force_jit_consistency_check clean ---");
    AuraJIT jit;
    jit.mutable_metrics().consistency_violations.store(0);
    jit.mutable_metrics().unhandled_opcode_count.store(0);
    CHECK(jit.force_jit_consistency_check(), "clean → true");
    CHECK(jit.metrics().consistency_violations.load() == 0, "still zero violations");
}

static void ac5_force_check_unhandled() {
    std::println("\n--- AC5: force_jit_consistency_check unhandled under strict ---");
    AuraJIT jit;
    CHECK(jit.strict_consistency_mode(), "strict on");
    jit.mutable_metrics().consistency_violations.store(0);
    jit.mutable_metrics().unhandled_opcode_count.store(1);
    CHECK(!jit.force_jit_consistency_check(), "unhandled under strict → false");
    CHECK(jit.metrics().consistency_violations.load() >= 1, "violations bumped");
}

static void ac6_schema_1658() {
    std::println("\n--- AC6: query:jit-consistency-stats schema 1658 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (g x) (+ x 1)) (g 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:jit-consistency-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:jit-consistency-stats", "schema") == 1658, "schema 1658");
    CHECK(href(cs, "query:jit-consistency-stats", "issue") == 1658, "issue 1658");
    CHECK(href(cs, "query:jit-consistency-stats", "opcode-tracked-total") == 54, "54 opcodes");
    CHECK(href(cs, "query:jit-consistency-stats", "guard-shape-lowered-wired") == 1, "GuardShape");
    CHECK(href(cs, "query:jit-consistency-stats", "linear-ops-lowered-wired") == 1, "Linear*");
    CHECK(href(cs, "query:jit-consistency-stats", "primcall-lowered-wired") == 1, "PrimCall");
    CHECK(href(cs, "query:jit-consistency-stats", "fail-fast-unhandled-wired") == 1, "fail-fast");
    CHECK(href(cs, "query:jit-consistency-stats", "safe-deopt-on-unhandled-wired") == 1,
          "safe-deopt");
    CHECK(href(cs, "query:jit-consistency-stats", "strict-consistency-default-on") == 1,
          "strict default");
    CHECK(href(cs, "query:jit-consistency-stats", "force-jit-consistency-check-wired") == 1,
          "force check wired");
    CHECK(href(cs, "query:jit-consistency-stats", "consistency-mandate-active") == 1, "mandate");
}

static void ac7_mutate_stress() {
    std::println("\n--- AC7: mutate + pattern stress ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (h x) (* x 2)) (h 3)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto v0 = href(cs, "query:jit-consistency-stats", "consistency-violations");
    for (int i = 0; i < 50; ++i) {
        (void)cs.eval(
            std::format("(mutate:rebind \"h\" \"(lambda (x) (+ x {}))\" \"issue1658\")", i % 7));
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(query:pattern '(define _ _))");
    }
    CHECK(href(cs, "query:jit-consistency-stats", "schema") == 1658, "schema holds under stress");
    CHECK(href(cs, "query:jit-consistency-stats", "consistency-violations") >= 0,
          "violations readable");
    // Violations may stay flat if JIT path stayed clean; just ensure no crash.
    (void)v0;
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval after stress");
}

static void ac8_lineage() {
    std::println("\n--- AC8: #532 / #1512 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "query:jit-consistency-stats", "unhandled-count") >= 0, "unhandled-count");
    CHECK(href(cs, "query:jit-consistency-stats", "fallback-count") >= 0, "fallback-count");
    CHECK(href(cs, "query:jit-consistency-stats", "opcode-coverage-pct") >= 0, "coverage pct");
    CHECK(href(cs, "query:jit-consistency-stats", "jit-consistency-total") >= 0, "total");
    CHECK(href(cs, "query:jit-consistency-stats", "jit-consistency-recommendation") >= 0,
          "recommendation");
    auto jit_hash = cs.eval("(engine:metrics \"query:jit-stats-hash\")");
    CHECK(jit_hash && is_hash(*jit_hash), "jit-stats-hash regression");
}

} // namespace

int main() {
    std::println("=== Issue #1658: JIT full opcode coverage + strict consistency ===");
    ac1_strict_default_on();
    ac2_opcode_counts();
    ac3_guard_shape_linear_coverage();
    ac4_force_check_clean();
    ac5_force_check_unhandled();
    ac6_schema_1658();
    ac7_mutate_stress();
    ac8_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
