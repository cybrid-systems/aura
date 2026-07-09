// @category: integration
// @reason: Issue #661 — SV InterfaceIR/ModportIR structure observability
// (P1 EDA-SV). Ships (query:sv-interface-structure-stats, schema 661)
// + 3 CompilerMetrics atomics (sv_interface_ports_total,
// sv_interface_modport_views_total, sv_interface_direction_changes_total)
// wired into `eda:parse-netlist`'s modport parse path. The full
// structured builders for ports/directions + verify_dirty_
// propagation is follow-up scope (issue body Actions #1-3).
//
//   - AC1:  query:sv-interface-structure-stats reachable (schema 661)
//   - AC2:  ports-count bumps on bump_sv_interface_ports direct call
//   - AC3:  modport-views bumps on bump_sv_interface_modport_views
//   - AC4:  direction-changes bumps on bump_sv_interface_direction_changes
//   - AC5:  interface-events-total == sum of 3 per-counter fields
//   - AC6:  eda:parse-netlist wired path — 1 modport with 3 ports
//           bumps modport_views by 1 + ports by 3
//   - AC7:  regression — existing SV primitives still reachable
//           (sv-node-stats, sv-structured-edsl-stats, sv-sva-structure-stats)
//           + non-SV primitives regression
//
// Uses one CompilerService per AC block to keep the modport-parse bump
// state isolated.

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_661_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:sv-interface-structure-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t ports_count(aura::compiler::CompilerService& cs) {
    return hash_int(cs, "ports-count");
}
static std::int64_t modport_views(aura::compiler::CompilerService& cs) {
    return hash_int(cs, "modport-views");
}
static std::int64_t direction_changes(aura::compiler::CompilerService& cs) {
    return hash_int(cs, "direction-changes");
}
static std::int64_t events_total(aura::compiler::CompilerService& cs) {
    return hash_int(cs, "interface-events-total");
}

static void run_ac1_schema(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: query:sv-interface-structure-stats (schema 661) ---");
    auto h = cs.eval("(query:sv-interface-structure-stats)");
    CHECK(h && aura::compiler::types::is_hash(*h), "sv-interface-structure-stats returns hash");
    CHECK(hash_int(cs, "schema") == 661, "schema == 661");
    const auto p = ports_count(cs);
    const auto m = modport_views(cs);
    const auto d = direction_changes(cs);
    const auto t = events_total(cs);
    std::println("  baseline: ports={}, modport-views={}, direction-changes={}, total={}", p, m, d,
                 t);
    CHECK(p >= 0, "ports-count non-negative");
    CHECK(m >= 0, "modport-views non-negative");
    CHECK(d >= 0, "direction-changes non-negative");
    CHECK(t >= 0, "interface-events-total non-negative");
}

static void run_ac2_ports(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: ports-count bumps on direct path ---");
    const auto p0 = ports_count(cs);
    cs.evaluator().bump_sv_interface_ports();
    cs.evaluator().bump_sv_interface_ports();
    cs.evaluator().bump_sv_interface_ports();
    const auto p1 = ports_count(cs);
    std::println("  ports-count: {} -> {}", p0, p1);
    CHECK(p1 == p0 + 3, "ports-count bumps by exactly 3");
}

static void run_ac3_modport_views(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: modport-views bumps on direct path ---");
    const auto m0 = modport_views(cs);
    cs.evaluator().bump_sv_interface_modport_views();
    cs.evaluator().bump_sv_interface_modport_views();
    const auto m1 = modport_views(cs);
    std::println("  modport-views: {} -> {}", m0, m1);
    CHECK(m1 == m0 + 2, "modport-views bumps by exactly 2");
}

static void run_ac4_direction_changes(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: direction-changes bumps on direct path ---");
    const auto d0 = direction_changes(cs);
    cs.evaluator().bump_sv_interface_direction_changes();
    cs.evaluator().bump_sv_interface_direction_changes();
    cs.evaluator().bump_sv_interface_direction_changes();
    cs.evaluator().bump_sv_interface_direction_changes();
    const auto d1 = direction_changes(cs);
    std::println("  direction-changes: {} -> {}", d0, d1);
    CHECK(d1 == d0 + 4, "direction-changes bumps by exactly 4");
}

static void run_ac5_sum(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: interface-events-total == sum ---");
    const auto p = ports_count(cs);
    const auto m = modport_views(cs);
    const auto d = direction_changes(cs);
    const auto t = events_total(cs);
    std::println("  ports={} + modport={} + dir-changes={} = sum {} (primitive total {})", p, m, d,
                 p + m + d, t);
    CHECK(t == p + m + d, "interface-events-total == sum of 3 per-counters");
}

static void run_ac6_parse_netlist_path(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC6: eda:parse-netlist modport path ---");
    const auto p0 = ports_count(cs);
    const auto m0 = modport_views(cs);
    // eda:parse-netlist requires workspace_flat_ to be non-null
    // (parser mutates workspace state). Bootstrap it via (set-code) +
    // (eval-current), same pattern as test_issue_586.
    CHECK(cs.eval("(set-code \"(define base 10) (+ base 1)\")").has_value(),
          "set-code for bootstrap");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current bootstrap");
    // Parse a netlist with 1 interface + 1 modport view (3 ports).
    // The parser populates modport with the port list and inserts the
    // modport child into the interface. Per the wiring, this bumps
    // modport-views by 1 and ports-count by 3. Note: Aura's string
    // parser interprets `\n` as a real newline (verified via
    // string-length test in shell), so the multi-line netlist is
    // passed as a single string with embedded \n escapes.
    auto r = cs.eval(R"((eda:parse-netlist "interface:my_iface\nmodport:master:clk,rst,data\n"))");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) >= 2,
          "eda:parse-netlist returned at least 2 (interface + modport parsed)");
    const auto p1 = ports_count(cs);
    const auto m1 = modport_views(cs);
    std::println("  ports-count: {} -> {} (expect +3)", p0, p1);
    std::println("  modport-views: {} -> {} (expect +1)", m0, m1);
    CHECK(m1 == m0 + 1, "modport-views bumped by 1 after eda:parse-netlist");
    CHECK(p1 == p0 + 3, "ports-count bumped by 3 after eda:parse-netlist (3 ports in modport)");
}

static void run_ac7_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC7: query regression — existing SV + non-SV primitives ---");
    auto sv_node = cs.eval("(query:sv-node-stats)");
    auto sv_struct = cs.eval("(query:sv-structured-edsl-stats)");
    auto sv_sva = cs.eval("(query:sv-sva-structure-stats)");
    auto sv_verify = cs.eval("(query:sv-verification-closedloop-stats)");
    auto self_evo = cs.eval("(query:self-evolution-chaos-stats)");
    auto runtime_corr = cs.eval("(query:runtime-observability-correlated-stats)");
    CHECK(sv_node &&
              (aura::compiler::types::is_int(*sv_node) || aura::compiler::types::is_hash(*sv_node)),
          "query:sv-node-stats regression");
    CHECK(sv_struct && (aura::compiler::types::is_int(*sv_struct) ||
                        aura::compiler::types::is_hash(*sv_struct)),
          "query:sv-structured-edsl-stats regression");
    CHECK(sv_sva &&
              (aura::compiler::types::is_int(*sv_sva) || aura::compiler::types::is_hash(*sv_sva)),
          "query:sv-sva-structure-stats regression");
    CHECK(sv_verify && aura::compiler::types::is_hash(*sv_verify),
          "query:sv-verification-closedloop-stats (schema 640) regression [hash]");
    CHECK(self_evo && aura::compiler::types::is_hash(*self_evo),
          "query:self-evolution-chaos-stats (schema 674) regression [hash]");
    CHECK(runtime_corr && aura::compiler::types::is_hash(*runtime_corr),
          "query:runtime-observability-correlated-stats (schema 673) regression [hash]");
}

} // namespace aura_issue_661_detail

int aura_issue_661_sv_interface_structure_stats_run() {
    using namespace aura_issue_661_detail;

    // Each AC uses a fresh CompilerService to keep the modport-parse
    // bump state isolated (so AC6's eda:parse-netlist doesn't add
    // accidental bumps to AC7's regression check).
    {
        aura::compiler::CompilerService cs;
        run_ac1_schema(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac2_ports(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac3_modport_views(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac4_direction_changes(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac5_sum(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac6_parse_netlist_path(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac7_regression(cs);
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_661_sv_interface_structure_stats_run();
}
#endif
