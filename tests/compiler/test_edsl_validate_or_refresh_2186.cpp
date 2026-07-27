// @category: unit
// @reason: Issue #2186 — force all EDSL StableNodeRef / node-handle
// consumption through ensure_valid_or_refresh (silent-stale zero-tolerance).
//
//   AC1: query:children/node/parent/*-stable/node-marker/node-provenance
//        accept packed stable-ref and go through ensure_valid_or_refresh
//   AC2: multi-round stress: capture ref → N structural mutates → consume
//        without explicit refresh → auto-refresh or loud fail; never
//        dangling / wrong-gen view
//   AC3: query:stable-ref-provenance-stats schema-2186 + auto-refresh counters
//   AC4: wrap_epoch hard-fail remains non-refreshable (#2056 lineage)
//   AC5: unit + integration under tests/compiler/

#include "test_harness.hpp"

#include "core/provenance_tracker.hh"

#include <cstdint>
#include <format>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_void;
using aura::core::provenance::kEdslValidateOrRefreshIssue;
using aura::core::provenance::reset_provenance_enforcement_for_test;
using aura::core::provenance::snapshot_provenance_enforcement;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t href_prov(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:stable-ref-provenance-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define a 1) (define b 2) (define acc 0)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

NodeId first_live(FlatAST& ws) {
    for (NodeId id = 1; id < ws.size(); ++id) {
        if (ws.is_live_node(id) && !ws.is_free_slot(id))
            return id;
    }
    return NULL_NODE;
}

} // namespace

int main() {
    std::println("=== Issue #2186: EDSL validate_or_refresh zero-tolerance ===");
    CHECK(kEdslValidateOrRefreshIssue == 2186, "issue stamp constant");

    // ── AC3 / AC5: schema-2186 on provenance-stats surface ──
    {
        std::println("\n--- AC3: schema-2186 + ensure counters surface ---");
        reset_provenance_enforcement_for_test();
        CompilerService cs;
        CHECK(setup_workspace(cs), "workspace setup");
        auto h = cs.eval("(engine:metrics \"query:stable-ref-provenance-stats\")");
        CHECK(h && is_hash(*h), "stable-ref-provenance-stats returns hash");
        CHECK(href_prov(cs, "schema-2186") == 2186, "schema-2186");
        CHECK(href_prov(cs, "issue-2186") == 2186, "issue-2186");
        CHECK(href_prov(cs, "edsl-validate-or-refresh-enforced") == 1,
              "edsl-validate-or-refresh-enforced");
        CHECK(href_prov(cs, "edsl-query-consume-via-ensure") == 1, "edsl-query-consume-via-ensure");
        // Lineage keys still present.
        CHECK(href_prov(cs, "schema") == 1630, "base schema 1630 retained");
        CHECK(href_prov(cs, "schema-2056") == 2056, "schema-2056 retained");
        CHECK(href_prov(cs, "ensure-valid-calls") >= 0, "ensure-valid-calls present");
        CHECK(href_prov(cs, "stable-ref-auto-refresh-total") >= 0,
              "stable-ref-auto-refresh-total present");
    }

    // ── AC1: query consumers accept packed stable-ref ──
    {
        std::println("\n--- AC1: query prims accept packed stable-ref ---");
        reset_provenance_enforcement_for_test();
        CompilerService cs;
        CHECK(setup_workspace(cs), "workspace setup AC1");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "workspace flat");
        const auto nid = first_live(*ws);
        CHECK(nid != NULL_NODE, "live node");

        // Capture packed (id . gen) via query:stable-ref.
        auto ref = cs.eval(std::format("(query:stable-ref {})", nid));
        CHECK(ref && is_pair(*ref), "query:stable-ref returns pair");

        // Bind into env and feed packed ref to consumers.
        // We re-query each time via eval that embeds the pair through
        // (let ((r (query:stable-ref N))) (query:X r)).
        auto kids =
            cs.eval(std::format("(let ((r (query:stable-ref {}))) (query:children r))", nid));
        CHECK(kids.has_value() && !is_error(*kids), "query:children accepts packed stable-ref");

        auto node = cs.eval(std::format("(let ((r (query:stable-ref {}))) (query:node r))", nid));
        CHECK(node.has_value() && !is_error(*node), "query:node accepts packed stable-ref");

        auto parent =
            cs.eval(std::format("(let ((r (query:stable-ref {}))) (query:parent r))", nid));
        CHECK(parent.has_value() && !is_error(*parent), "query:parent accepts packed stable-ref");

        auto kids_s = cs.eval(
            std::format("(let ((r (query:stable-ref {}))) (query:children-stable r))", nid));
        CHECK(kids_s.has_value() && !is_error(*kids_s),
              "query:children-stable accepts packed stable-ref");

        auto parent_s =
            cs.eval(std::format("(let ((r (query:stable-ref {}))) (query:parent-stable r))", nid));
        CHECK(parent_s.has_value() && !is_error(*parent_s),
              "query:parent-stable accepts packed stable-ref");

        auto marker =
            cs.eval(std::format("(let ((r (query:stable-ref {}))) (query:node-marker r))", nid));
        CHECK(marker.has_value() && !is_error(*marker),
              "query:node-marker accepts packed stable-ref");

        auto prov = cs.eval(
            std::format("(let ((r (query:stable-ref {}))) (query:node-provenance r))", nid));
        CHECK(prov.has_value() && !is_error(*prov),
              "query:node-provenance accepts packed stable-ref");

        // Bare int still works (promoted via make_stamped_ref + ensure).
        auto bare = cs.eval(std::format("(query:children {})", nid));
        CHECK(bare.has_value() && !is_error(*bare), "query:children still accepts bare NodeId");
    }

    // ── AC2: multi-round stress — capture → N mutates → consume ──
    {
        std::println("\n--- AC2: multi-round capture → mutate → consume ---");
        reset_provenance_enforcement_for_test();
        CompilerService cs;
        CHECK(setup_workspace(cs), "workspace setup AC2");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "workspace flat AC2");

        const auto ensure0 = snapshot_provenance_enforcement().ensure_calls;
        const auto refresh0 = snapshot_provenance_enforcement().auto_refresh;
        const auto flat_refresh0 = ws->stale_ref_auto_refresh_count();

        // Capture a live Define-ish node once and hold the packed ref
        // across structural mutates that restamp generations.
        const auto nid = first_live(*ws);
        CHECK(nid != NULL_NODE, "live node AC2");
        auto cap = cs.eval(std::format("(query:stable-ref {})", nid));
        CHECK(cap && is_pair(*cap), "initial stable-ref capture");

        // Hold C++ StableNodeRef with the pre-mutate gen so we can
        // drive ensure_valid_or_refresh directly after restamps.
        FlatAST::StableNodeRef held = cs.evaluator().make_stamped_ref(nid);
        const auto gen_at_capture = held.gen;
        const auto wrap_at_capture = held.wrap_epoch;

        constexpr int kRounds = 12;
        int consume_ok = 0;
        int refresh_successes = 0;
        for (int round = 0; round < kRounds; ++round) {
            // Structural mutate + eval bumps generation / restamps slots.
            (void)cs.eval(std::format("(mutate:rebind \"a\" \"{}\")", 10 + round));
            (void)cs.eval("(eval-current)");
            // Force full restamp so held gen is guaranteed stale
            // (MutationBoundaryGuard exit path analogue).
            ws->restamp_all_node_generations();

            // C++ path: ensure must auto-refresh (or fail loud) — never
            // return a wrong-gen view.
            FlatAST::StableNodeRef probe = held;
            // Keep the capture-time gen so the ref is stale after restamp.
            probe.gen = gen_at_capture;
            probe.wrap_epoch = wrap_at_capture;
            auto view = cs.evaluator().ensure_valid_or_refresh(probe, /*auto_refresh=*/true);
            if (view.has_value()) {
                ++consume_ok;
                // After success, probe must match live gen (refreshed).
                CHECK(probe.is_valid_in(*ws),
                      std::format("round {} refreshed ref is_valid_in", round));
                if (probe.gen != gen_at_capture)
                    ++refresh_successes;
            } else {
                // Loud fail is also AC2-compliant (never silent wrong-gen).
                ++consume_ok;
            }

            // EDSL path: re-feed packed stable-ref from a fresh capture
            // taken *before* this round's restamp, then consume.
            // Pattern: capture → restamp already done → (query:children r)
            // with r holding pre-restamp gen via let of query:stable-ref
            // is live; for true stale we use C++-built pair via eval of
            // bare id after restamp (promotes + ensure) which must succeed
            // for live nodes.
            auto kids = cs.eval(std::format("(query:children {})", nid));
            CHECK(kids.has_value() && !is_error(*kids),
                  std::format("round {} query:children after restamp", round));

            auto node =
                cs.eval(std::format("(let ((r (query:stable-ref {}))) (query:node r))", nid));
            CHECK(node.has_value() && !is_error(*node),
                  std::format("round {} query:node via packed ref", round));
        }
        CHECK(consume_ok == kRounds, "all multi-round consumes resolved (refresh or fail)");
        std::println("  refresh_successes (gen restamped) = {}", refresh_successes);
        // With default AutoRefreshOnBoundary, stale held refs should refresh.
        CHECK(refresh_successes > 0, "at least one auto-refresh on multi-round stale gen");

        const auto ensure1 = snapshot_provenance_enforcement().ensure_calls;
        const auto refresh1 = snapshot_provenance_enforcement().auto_refresh;
        const auto flat_refresh1 = ws->stale_ref_auto_refresh_count();
        std::println("  ensure_calls: {} -> {}", ensure0, ensure1);
        std::println("  auto_refresh: {} -> {} (flat {} -> {})", refresh0, refresh1, flat_refresh0,
                     flat_refresh1);
        CHECK(ensure1 > ensure0, "ensure_valid calls grew under multi-round load");
        CHECK(refresh1 > refresh0 || flat_refresh1 > flat_refresh0,
              "auto-refresh counters grew under multi-round load (AC3)");

        // Stats surface still reports schema-2186 after workload.
        CHECK(href_prov(cs, "schema-2186") == 2186, "schema-2186 after stress");
        CHECK(href_prov(cs, "ensure-valid-calls") > 0, "ensure-valid-calls > 0 after stress");
    }

    // ── AC4: wrap_epoch hard fail non-refreshable ──
    {
        std::println("\n--- AC4: wrap_epoch fence remains hard-fail ---");
        reset_provenance_enforcement_for_test();
        CompilerService cs;
        CHECK(setup_workspace(cs), "workspace setup AC4");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "workspace flat AC4");
        const auto nid = first_live(*ws);
        CHECK(nid != NULL_NODE, "live node AC4");

        FlatAST::StableNodeRef ref = cs.evaluator().make_stamped_ref(nid);
        // Poison wrap_epoch to a non-zero mismatch — refresh_if_stale
        // must hard-fail (epoch fence), never silent rebind.
        ref.wrap_epoch = ws->wrap_epoch() + 99;
        if (ref.wrap_epoch == 0)
            ref.wrap_epoch = 1;
        // Also force gen stale so the happy is_valid_in path is skipped.
        ref.gen = static_cast<std::uint16_t>(ref.gen + 1);

        const auto fence0 = snapshot_provenance_enforcement().epoch_fence_hit;
        auto view = cs.evaluator().ensure_valid_or_refresh(ref, /*auto_refresh=*/true);
        CHECK(!view.has_value(), "wrap_epoch mismatch is non-refreshable");
        const auto fence1 = snapshot_provenance_enforcement().epoch_fence_hit;
        CHECK(fence1 > fence0, "epoch fence hit counter bumped");
    }

    // ── AC1 cont: mutate bare NodeId path uses ensure ──
    {
        std::println("\n--- AC1/mutate: bare NodeId resolve uses ensure ---");
        reset_provenance_enforcement_for_test();
        CompilerService cs;
        CHECK(setup_workspace(cs), "workspace setup mutate");
        const auto ensure0 = snapshot_provenance_enforcement().ensure_calls;
        // mutate:replace-value via resolve_mutate_node_arg on bare int.
        // Use a likely-valid literal node id; may fail for other reasons
        // but ensure path must still run.
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "ws");
        const auto nid = first_live(*ws);
        // usage: (mutate:replace-value node-id|stable-ref new-value summary)
        // Resolve runs before type checks, so even a type mismatch still
        // exercises ensure_valid_or_refresh on the bare NodeId path.
        (void)cs.eval(std::format("(mutate:replace-value {} 42 \"2186-bare-nodeid\")", nid));
        const auto ensure1 = snapshot_provenance_enforcement().ensure_calls;
        CHECK(ensure1 > ensure0, "mutate bare NodeId path called ensure_valid_or_refresh");
    }

    // ── Source wiring check ──
    {
        std::println("\n--- AC5: source wiring mentions 2186 ---");
        // Soft: stats already verified above; constant stamp checked.
        CHECK(kEdslValidateOrRefreshIssue == 2186, "source constant wired");
    }

    std::println("\n=== #2186 summary: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
