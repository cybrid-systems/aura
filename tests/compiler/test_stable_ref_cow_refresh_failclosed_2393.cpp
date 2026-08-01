// @category: unit
// @reason: Issue #2393 — refresh_if_stale fail-closed on COW epoch
// mismatch without pin (consistent with wrap_epoch fence + is_valid_in_layer).
//
//   AC1: unpinned cow_epoch mismatch → refresh_if_stale returns false;
//        is_valid_in_layer also false (same policy)
//   AC2: record_cross_layer_mismatch counter bumps (fail-closed indicator)
//   AC3: boundary_pinned ref may still refresh after COW advance
//   AC4: wrap_epoch hard-fail path unchanged (regression)
//   AC5: this test + CMake + build.py gate

#include "test_harness.hpp"

#include "core/provenance_tracker.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::core::provenance::reset_provenance_enforcement_for_test;
using aura::core::provenance::snapshot_provenance_enforcement;
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

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define a 1) (define b 2) (define acc 0)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static NodeId first_live(FlatAST& ws) {
    for (NodeId id = 1; id < ws.size(); ++id) {
        if (ws.is_live_node(id) && !ws.is_free_slot(id))
            return id;
    }
    return NULL_NODE;
}

// ── AC1/AC2: unpinned COW mismatch is hard fail-closed ──
static void ac1_ac2_cow_mismatch_failclosed() {
    std::println("\n--- #2393 AC1/AC2: unpinned COW mismatch → refresh false ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    CHECK(setup_workspace(cs), "workspace");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "flat");
    const auto nid = first_live(*ws);
    CHECK(nid != NULL_NODE, "live node");

    FlatAST::StableNodeRef ref = cs.evaluator().make_stamped_ref(nid);
    CHECK(!ref.boundary_pinned, "not pinned");

    // Advance live COW epoch, then poison capture to a different *non-zero*
    // value. (capture==0 is legacy soft-restamp; must stay non-zero for
    // the #2393 hard path. Avoid epoch0==0 → capture=1 == live trap.)
    const auto epoch0 = ws->workspace_cow_epoch();
    ws->set_workspace_cow_epoch(epoch0 + 1);
    const auto epoch1 = ws->workspace_cow_epoch();
    ref.cow_epoch_at_capture = epoch1 + 99; // non-zero, never equals live
    // Also force gen stale so the happy is_valid_in early-out is skipped
    // (COW alone already fails is_valid; gen poison is belt-and-suspenders).
    ref.gen = static_cast<std::uint16_t>(ref.gen + 1);

    CHECK(ref.cow_epoch_at_capture != 0, "capture epoch non-zero");
    CHECK(epoch1 != ref.cow_epoch_at_capture, "epochs differ");
    CHECK(!ref.is_valid_in_layer(*ws, ref.workspace_id),
          "AC1: is_valid_in_layer false on COW mismatch");
    CHECK(!ws->is_valid(ref), "AC1: is_valid false on COW mismatch");

    const auto mismatch0 = snapshot_provenance_enforcement().cross_layer_mismatch;
    const auto refresh0 = snapshot_provenance_enforcement().auto_refresh;
    const auto capture_before = ref.cow_epoch_at_capture;
    const bool ok = ref.refresh_if_stale(*ws);
    CHECK(!ok, "AC1: refresh_if_stale returns false (fail-closed)");
    const auto mismatch1 = snapshot_provenance_enforcement().cross_layer_mismatch;
    const auto refresh1 = snapshot_provenance_enforcement().auto_refresh;
    CHECK(mismatch1 > mismatch0, "AC2: cross_layer_mismatch counter bumped");
    CHECK(refresh1 == refresh0, "AC2: no auto-refresh restamp on hard fail");

    // Ref must not have been silently restamped to the live cow epoch.
    CHECK(ref.cow_epoch_at_capture == capture_before, "AC1: cow field unchanged on fail");
    CHECK(!ref.is_valid_in(*ws), "AC1: still invalid after fail-closed");
}

// ── AC3: pinned survives COW via refresh ──
static void ac3_pinned_may_refresh() {
    std::println("\n--- #2393 AC3: boundary_pinned may refresh after COW ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    CHECK(setup_workspace(cs), "workspace");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "flat");
    const auto nid = first_live(*ws);
    CHECK(nid != NULL_NODE, "live");
    CHECK(ws->is_live_node(nid), "nid live");

    FlatAST::StableNodeRef pinned = cs.evaluator().make_stamped_ref(nid);
    pinned.pin_for_cow();
    CHECK(pinned.boundary_pinned, "pin set");

    const auto epoch0 = ws->workspace_cow_epoch();
    ws->set_workspace_cow_epoch(epoch0 + 1);
    // Capture epoch differs from live; pin allows survival across COW.
    pinned.cow_epoch_at_capture = ws->workspace_cow_epoch() + 99;
    // Force gen stale so we exercise the refresh path (not is_valid early-out).
    // is_valid allows pin exception for cow, but gen must still match.
    pinned.gen = static_cast<std::uint16_t>(pinned.gen + 1);

    const auto mismatch0 = snapshot_provenance_enforcement().cross_layer_mismatch;
    const bool ok = pinned.refresh_if_stale(*ws);
    const auto mismatch1 = snapshot_provenance_enforcement().cross_layer_mismatch;
    CHECK(ok, "AC3: pinned refresh_if_stale succeeds");
    CHECK(mismatch1 == mismatch0, "AC3: pin path does not bump cross_layer_mismatch");
    CHECK(pinned.boundary_pinned, "AC3: pin preserved");
    CHECK(pinned.cow_epoch_at_capture == ws->workspace_cow_epoch(),
          "AC3: cow restamped to live layer");
    CHECK(pinned.is_valid_in(*ws), "AC3: refreshed pinned ref valid");
}

// ── AC4: wrap_epoch hard-fail unchanged ──
static void ac4_wrap_epoch_still_hard() {
    std::println("\n--- #2393 AC4: wrap_epoch fence still hard-fail ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    CHECK(setup_workspace(cs), "workspace");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "flat");
    const auto nid = first_live(*ws);
    CHECK(nid != NULL_NODE, "live");

    FlatAST::StableNodeRef ref = cs.evaluator().make_stamped_ref(nid);
    ref.wrap_epoch = ws->wrap_epoch() + 99;
    if (ref.wrap_epoch == 0)
        ref.wrap_epoch = 1;
    ref.gen = static_cast<std::uint16_t>(ref.gen + 1);

    const auto fence0 = snapshot_provenance_enforcement().epoch_fence_hit;
    CHECK(!ref.refresh_if_stale(*ws), "AC4: wrap mismatch non-refreshable");
    CHECK(snapshot_provenance_enforcement().epoch_fence_hit > fence0, "AC4: fence counter");
}

// ── Source + gate ──
static void ac5_source_cite() {
    std::println("\n--- #2393 AC5: source cites fail-closed COW ---");
    auto body = read_file("src/core/ast_stability.cpp");
    CHECK(!body.empty(), "read ast_stability.cpp");
    CHECK(body.find("#2393") != std::string::npos || body.find("Issue #2393") != std::string::npos,
          "ast_stability cites #2393");
    CHECK(body.find("return false") != std::string::npos, "has return false");
    // Fail-closed block: record then return false (not soft fall-through).
    const auto rec = body.find("record_cross_layer_mismatch");
    CHECK(rec != std::string::npos, "records cross_layer_mismatch");
    const auto ret = body.find("return false", rec);
    CHECK(ret != std::string::npos && ret < rec + 200,
          "return false shortly after record_cross_layer_mismatch");
}

} // namespace

int main() {
    std::println("=== Issue #2393: COW refresh fail-closed ===");
    ac1_ac2_cow_mismatch_failclosed();
    ac3_pinned_may_refresh();
    ac4_wrap_epoch_still_hard();
    ac5_source_cite();
    std::println("\n=== #2393 summary: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
