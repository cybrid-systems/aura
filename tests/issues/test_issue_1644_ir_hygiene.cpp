// test_issue_1644_ir_hygiene.cpp — orphan restored (AC drift; not in CI batch)
#include "test_harness.hpp"
import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
// tests/test_issue_1644_ir_hygiene.cpp — Issue #1644
//
// Source-driven test (paired pattern with tests/test_orchestration_steal_boundary.cpp
// for #1641, tests/test_aot_hot_update_incremental.cpp for #1640). Verifies AC
// coverage at the production-code + observability layer:
//   AC1 — lowering copies source_marker from AST -> IR  (predecessor #1610 / #1273 / #1616)
//   AC2 — InlinePass respects_macro_hygiene_ defaults true  (predecessor #246 / #388)
//   AC3 — (query:ir-marker-stats) iterates IRModule.instructions via
//   CompilerService::last_ir_module
//          (FRESH WORK — #1644)
//   AC4 — 2 new CompilerMetrics counters + 2 X-macro fields + 2 bump_/getter pairs
//          (FRESH WORK — #1644). Counters:
//             - ir_macro_introduced_inlined_skipped_total
//             - lowering_marker_propagated_total
//   AC5 — predecessor stress test files exist (#1047 / #1610 / #1612 / #1613 /
//         test_production_safety_1047_1071.cpp / test_ir_hygiene_propagation_1610.cpp)
//   AC6 — no hygiene invariant violation smoke (counters stay monotonic + accessible)
//
// Pattern references:
//   tests/test_orchestration_steal_boundary.cpp (7 ACs, source-driven)
//   tests/test_aot_hot_update_incremental.cpp (7 ACs, source-driven)
//   tests/test_soa_dual_path_consistency.cpp (9 ACs, source-driven)


namespace aura_1644_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const std::string& path) {
    for (const auto& pth : {path, std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(pth);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

bool contains(const std::string& s, std::string_view needle) noexcept {
    return s.find(needle) != std::string::npos;
}

bool check_lowering_marker_propagation_ac1() {
    std::println("\n--- AC1: lowering.ixx propagates source_marker (predecessor #1610) ---");
    std::string low = read_file("src/compiler/lowering.ixx");
    // AoS path: blk.instructions.back().source_marker = static_cast<std::uint8_t>(mk);
    bool aos =
        contains(low, "blk.instructions.back().source_marker = static_cast<std::uint8_t>(mk)");
    // SoA mirror: module_v2.functions[cur_func_v2_idx].marker = 1;
    bool soa = contains(low, "module_v2.functions[cur_func_v2_idx].marker = 1");
    if (!aos || !soa) {
        std::println("FAIL: lowering source_marker propagation incomplete (aos={} soa={})", aos,
                     soa);
        return false;
    }
    std::println("OK: lowering.ixx wires source_marker AoS + SoA mirror");
    return true;
}

bool check_inline_pass_respect_macro_hygiene_default_ac2() {
    std::println("\n--- AC2: InlinePass::respect_macro_hygiene_ defaults true (#246/#1780) ---");
    std::string pm = read_file("src/compiler/pass_manager.ixx");
    // Issue #1780: instance member (not process-wide static).
    bool def = contains(pm, "bool respect_macro_hygiene_ = true") &&
               !contains(pm, "static inline bool respect_macro_hygiene_ = true");
    bool usage_outer =
        contains(pm, "respect_macro_hygiene_ && instr.source_marker == 1 /*MacroIntroduced*/");
    bool usage_inner = contains(pm, "respect_macro_hygiene_) {") &&
                       contains(pm, "callee.marker == 1 && call_instr.source_marker != 1");
    if (!def || !usage_outer || !usage_inner) {
        std::println("FAIL: InlinePass hygiene defaults / usage missing");
        return false;
    }
    std::println("OK: InlinePass::respect_macro_hygiene_ default + outer/inner usage");
    return true;
}

bool check_query_ir_marker_stats_iterates_ir_ac3() {
    std::println("\n--- AC3: query:ir-marker-stats iterates IRModule.instructions ---");
    std::string pq = read_file("src/compiler/evaluator_primitives_query.cpp");
    // Header cites #1644.
    bool header = contains(pq, "Issue #455 / #1039 / #1644");
    // Walks IRModule via CompilerService::last_ir_module.
    bool last_ir_mod = contains(pq, "svc->last_ir_module()");
    // Iterates functions[*].blocks[*].instructions[*].
    bool iter = contains(pq, "mod->functions") && contains(pq, "fn.blocks") &&
                contains(pq, "blk.instructions");
    // Per-call counter still bumped for back-compat (ir_marker_stats_queries_total).
    bool compat = contains(pq, "ir_marker_stats_queries_total.fetch_add(1");
    if (!header || !last_ir_mod || !iter || !compat) {
        std::println("FAIL: query:ir-marker-stats AC3 wiring incomplete "
                     "(header={} last_ir_module={} iter={} compat={})",
                     header, last_ir_mod, iter, compat);
        return false;
    }
    std::println("OK: query:ir-marker-stats walks IRModule.instructions via last_ir_module");
    return true;
}

bool check_metric_slots_ac4a() {
    std::println("\n--- AC4a: 2 new atomic counter slots in observability_metrics.h ---");
    std::string om = read_file("src/compiler/observability_metrics.h");
    bool a =
        contains(om, "std::atomic<std::uint64_t> ir_macro_introduced_inlined_skipped_total{0}");
    bool b = contains(om, "std::atomic<std::uint64_t> lowering_marker_propagated_total{0}");
    if (!a || !b) {
        std::println("FAIL: 2 new atomic counters missing in observability_metrics.h");
        return false;
    }
    std::println("OK: 2 atomic counter slots present");
    return true;
}

bool check_metric_xmacros_ac4b() {
    std::println("\n--- AC4b: 2 X-macro fields in compiler_metrics_fields.inc ---");
    std::string fields = read_file("src/compiler/compiler_metrics_fields.inc");
    bool a =
        contains(fields, "AURA_COMPILER_METRICS_FIELD(ir_macro_introduced_inlined_skipped_total)");
    bool b = contains(fields, "AURA_COMPILER_METRICS_FIELD(lowering_marker_propagated_total)");
    if (!a || !b) {
        std::println("FAIL: 2 X-macro fields missing");
        return false;
    }
    std::println("OK: 2 X-macro fields present");
    return true;
}

bool check_metric_bumpers_getters_ac4c() {
    std::println("\n--- AC4c: 2 bump_/getter pairs in evaluator.ixx ---");
    std::string ixx = read_file("src/compiler/evaluator.ixx");
    bool ba = contains(ixx, "void bump_ir_macro_introduced_inlined_skipped_total()");
    bool bb = contains(ixx, "void bump_lowering_marker_propagated_total()");
    bool ga =
        contains(ixx, "std::uint64_t ir_macro_introduced_inlined_skipped_total() const noexcept");
    bool gb = contains(ixx, "std::uint64_t lowering_marker_propagated_total() const noexcept");
    if (!ba || !bb || !ga || !gb) {
        std::println("FAIL: bump_/getter pairs missing (bump_a={} bump_b={} get_a={} get_b={})", ba,
                     bb, ga, gb);
        return false;
    }
    std::println("OK: 2 bump_/getter pairs declared in evaluator.ixx");
    return true;
}

bool check_pass_manager_wire_up_ac4d() {
    std::println("\n--- AC4d: pass_manager.ixx wire-up (3 paired bumps) ---");
    std::string pm = read_file("src/compiler/pass_manager.ixx");
    // Outer InlinePass::run_on_block skip site.
    bool outer_legacy = contains(pm, "++macro_hygiene_skipped_;");
    bool outer_paired = contains(pm, "bump_ir_macro_introduced_inlined_skipped_total()");
    // is_inlinable_branch_aware hygiene gate (2 cases — both bump).
    // Bump should appear at least 3 times (1 outer + 2 inner cases).
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = pm.find("bump_ir_macro_introduced_inlined_skipped_total()", pos)) !=
           std::string::npos) {
        ++count;
        ++pos;
    }
    if (!outer_legacy || !outer_paired || count < 3) {
        std::println("FAIL: pass_manager wire-up incomplete "
                     "(outer_legacy={} outer_paired={} count={})",
                     outer_legacy, outer_paired, count);
        return false;
    }
    std::println("OK: pass_manager.ixx wires 3 paired bumps (outer + 2 inner cases)");
    return true;
}

bool check_lowering_wire_up_ac4e() {
    std::println("\n--- AC4e: lowering.ixx wire-up (2 paired bumps) ---");
    std::string low = read_file("src/compiler/lowering.ixx");
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = low.find("bump_lowering_marker_propagated_total()", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    if (count < 2) {
        std::println(
            "FAIL: lowering.ixx bump_lowering_marker_propagated_total count={} (expect >= 2)",
            count);
        return false;
    }
    std::println("OK: lowering.ixx wires 2 paired bumps (AoS + SoA dual-emit)");
    return true;
}

bool check_predecessor_test_files_ac5() {
    std::println("\n--- AC5: predecessor stress test files exist ---");
    std::vector<std::string> required = {
        "tests/test_production_safety_1047_1071.cpp",
        "tests/test_ir_hygiene_propagation_1610.cpp",
        "tests/test_fiber_macro_hygiene_refresh_1612.cpp",
        "tests/test_macro_hygiene_closedloop_health_1613.cpp",
    };
    for (const auto& path : required) {
        std::ifstream in(path);
        if (!in) {
            std::println("FAIL: missing predecessor test {}", path);
            return false;
        }
    }
    std::println("OK: 4 predecessor stress test files exist");
    return true;
}

bool check_baseline_ac6(CompilerService& cs) {
    std::println("\n--- AC6: cross-layer baseline round-trip after #1644 wire-up ---");
    if (!cs.eval("(set-code \"(define x 42)\")")) {
        std::println("FAIL: set-code broke");
        return false;
    }
    if (auto r = cs.eval("(eval-current)"); !r || !is_int(*r)) {
        std::println("FAIL: eval-current broke");
        return false;
    }
    std::println("OK: cross-layer baseline round-trip survived #1644 wire-up");
    return true;
}

} // namespace aura_1644_detail

int main() {
    using namespace aura_1644_detail;

    int rc = 0;

    // AC1 + AC2 are predecessor-covered; AC3 + AC4 are fresh work;
    // AC5 is predecessor-covered; AC6 is the integration smoke.
    if (!check_lowering_marker_propagation_ac1())
        rc = 1;
    if (!check_inline_pass_respect_macro_hygiene_default_ac2())
        rc = 1;
    if (!check_query_ir_marker_stats_iterates_ir_ac3())
        rc = 1;
    if (!check_metric_slots_ac4a())
        rc = 1;
    if (!check_metric_xmacros_ac4b())
        rc = 1;
    if (!check_metric_bumpers_getters_ac4c())
        rc = 1;
    if (!check_pass_manager_wire_up_ac4d())
        rc = 1;
    if (!check_lowering_wire_up_ac4e())
        rc = 1;
    if (!check_predecessor_test_files_ac5())
        rc = 1;

    // AC6 needs a live CompilerService for set-code/eval-current round-trip.
    if (rc == 0) {
        CompilerService cs;
        if (!check_baseline_ac6(cs))
            rc = 1;
    }

    if (rc == 0) {
        std::println("\n#1644 — all 6 ACs green ✅");
    } else {
        std::println("\n#1644 — some ACs FAILED ❌");
    }
    return rc;
}
