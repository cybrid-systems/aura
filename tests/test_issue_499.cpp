// @category: integration
// @reason: Issue #499 — EDA foundation primitives module + stats

#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_499_detail {
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

static std::int64_t snap_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:eda-foundation-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_499_detail

int main() {
    using namespace aura_issue_499_detail;

    std::println("=== Issue #499: EDA foundation primitives module ===");

    aura::compiler::CompilerService cs;

    // AC1: query:eda-foundation-stats fields
    {
        std::println("\n--- AC1: query:eda-foundation-stats ---");
        auto stats = cs.eval("(query:eda-foundation-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:eda-foundation-stats returns hash");
        CHECK(snap_stat(cs, "parse-total") >= 0, "parse-total present");
        CHECK(snap_stat(cs, "query-total") >= 0, "query-total present");
        CHECK(snap_stat(cs, "mutate-total") >= 0, "mutate-total present");
        CHECK(snap_stat(cs, "waveform-total") >= 0, "waveform-total present");
        CHECK(snap_stat(cs, "feedback-total") >= 0, "feedback-total present");
        CHECK(snap_stat(cs, "foundation-total") >= 0, "foundation-total present");
    }

    const auto parse_before = snap_stat(cs, "parse-total");
    const auto mutate_before = snap_stat(cs, "mutate-total");

    CHECK(cs.eval("(set-code \"(define seed 1)\")").has_value(), "workspace setup");

    // AC2: eda:parse-netlist + eda:query-nodes roundtrip
    {
        std::println("\n--- AC2: parse-netlist + query-nodes ---");
        auto parsed = cs.eval("(eda:parse-netlist "
                              "\"interface:Bus\\nmodport:master:clk,data\\nconstraint:c_dist:val "
                              "inside {[0:255]};\")");
        CHECK(parsed && aura::compiler::types::is_int(*parsed) &&
                  aura::compiler::types::as_int(*parsed) == 3,
              "eda:parse-netlist parsed 3 nodes");
        auto iface_count = cs.eval("(eda:query-nodes \"Interface\")");
        CHECK(iface_count && aura::compiler::types::is_int(*iface_count) &&
                  aura::compiler::types::as_int(*iface_count) >= 1,
              "eda:query-nodes Interface >= 1");
        auto mp_count = cs.eval("(eda:query-nodes \"Modport\")");
        CHECK(mp_count && aura::compiler::types::is_int(*mp_count) &&
                  aura::compiler::types::as_int(*mp_count) >= 1,
              "eda:query-nodes Modport >= 1");
        CHECK(
            snap_stat(cs, "parse-total") > parse_before,
            std::format("parse-total grew ({} -> {})", parse_before, snap_stat(cs, "parse-total")));
    }

    aura::ast::NodeId iface_id = aura::ast::NULL_NODE;
    if (auto* ws = cs.workspace_flat()) {
        for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
            if (ws->get(id).tag == aura::ast::NodeTag::Interface) {
                iface_id = id;
                break;
            }
        }
    }
    CHECK(iface_id != aura::ast::NULL_NODE, "found Interface node");
    {
        auto* ws = cs.workspace_flat();
        CHECK(ws->is_live_node(iface_id), "iface is live node");
        ws->restamp_subtree_generation(iface_id);
        auto ref = ws->make_ref(iface_id);
        CHECK(ref.is_valid_in(*ws),
              std::format("iface ref valid after restamp (id={} gen={})", iface_id, ref.gen));
        CHECK(ws->get(iface_id).tag == aura::ast::NodeTag::Interface, "iface tag is Interface");
        CHECK(ws->get(iface_id).children.size() >= 1,
              std::format("iface children >= 1 (got {})", ws->get(iface_id).children.size()));
    }

    // AC3: eda:mutate-add-instance StableRef + hardware feedback
    {
        std::println("\n--- AC3: mutate-add-instance + hardware feedback ---");
        auto upd = cs.eval(std::format("(eda:mutate-add-instance {} \"slave\" \"ready,valid\")",
                                       static_cast<int>(iface_id)));
        CHECK(upd && aura::compiler::types::is_bool(*upd) && aura::compiler::types::as_bool(*upd),
              "eda:mutate-add-instance succeeds");
        auto mp_after = cs.eval("(eda:query-nodes \"Modport\")");
        CHECK(mp_after && aura::compiler::types::is_int(*mp_after) &&
                  aura::compiler::types::as_int(*mp_after) >= 2,
              "modport count >= 2 after mutate-add-instance");
        auto fb =
            cs.eval(std::format("(eda:run-hardware-feedback {})", static_cast<int>(iface_id)));
        CHECK(fb && aura::compiler::types::is_bool(*fb) && aura::compiler::types::as_bool(*fb),
              "eda:run-hardware-feedback succeeds");
        CHECK(snap_stat(cs, "mutate-total") > mutate_before,
              std::format("mutate-total grew ({} -> {})", mutate_before,
                          snap_stat(cs, "mutate-total")));
        CHECK(snap_stat(cs, "feedback-total") >= 1, "feedback-total >= 1");
    }

    // AC4: eda:waveform-snapshot observability export
    {
        std::println("\n--- AC4: waveform-snapshot ---");
        auto snap = cs.eval(std::format("(eda:waveform-snapshot {})", static_cast<int>(iface_id)));
        CHECK(snap && aura::compiler::types::is_int(*snap) &&
                  aura::compiler::types::as_int(*snap) >= 0,
              "eda:waveform-snapshot returns non-negative length");
        CHECK(snap_stat(cs, "waveform-total") >= 1, "waveform-total >= 1");
    }

    // AC5: query:sv-node-stats regression
    {
        std::println("\n--- AC5: sv-node-stats regression ---");
        auto sva = cs.eval("(query:sv-node-stats)");
        CHECK(sva && aura::compiler::types::is_hash(*sva), "sv-node-stats regression");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 157,
              "stats:count == 157");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}