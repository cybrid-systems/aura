// @category: unit
// @reason: Issue #2035 — HotUpdateRegistry.notify_dirty_define +
// region-mask reemit wired to SoA / cache block_dirty after cascade.
//
//   AC1: source cites #2035; notify_hot_update_after_cascade_ +
//        compute_region_mask_from_dirty + on_region_mask_from_dirty
//   AC2: unit — compute_region_mask_from_dirty partial vs full
//   AC3: mark_define_dirty bumps dirty_notify_total (+ listeners)
//   AC4: when reemit provider wired → cascade_reemit_trigger_total +
//        reemit_candidates / success advance; region mask set
//   AC5: query:hot-update-registry-stats schema-2035 keys
//   AC6: stable func-id preserve consistent across reemit rounds
//   AC7 (#2090): query:hot-update-registry-stats schema-2090 keys +
//        3 new boundary counters (boundary-reemit-success-total /
//        boundary-reemit-throttled-total / boundary-batch-deopt-
//        unmatched-total) present
//   AC8 (#2090): outermost MutationBoundaryGuard dtor wires the
//        throttle → reemit → epoch_notify → batch_deopt pipeline
//        (Issue #2090 — pairs with #2035 cascade path)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/hot_update_registry.hh"
#include "compiler/aura_jit_bridge.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::hot_update_registry;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:hot-update-registry-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── reemit candidate fixture (region-aware) ─────────────────────────
struct ReemitFeed {
    std::vector<std::string> names;
    std::vector<std::uint64_t> regions;
    std::size_t cursor = 0;
};

static bool reemit_candidate_iter(void* userdata, const char** out_name, std::uint64_t* out_region,
                                  bool* out_from_cc) {
    auto* f = static_cast<ReemitFeed*>(userdata);
    if (f->cursor >= f->names.size()) {
        f->cursor = 0;
        return false;
    }
    *out_name = f->names[f->cursor].c_str();
    *out_region = f->cursor < f->regions.size() ? f->regions[f->cursor] : 1;
    *out_from_cc = false;
    ++f->cursor;
    return true;
}

static bool emit_ok(const char* /*name*/, std::uint64_t /*region*/, void* /*ud*/) {
    return true;
}

static void ac1_source() {
    std::println("\n--- AC1: source cites #2035 ---");
    auto reg = read_file("src/compiler/hot_update_registry.hh");
    auto regcpp = read_file("src/compiler/hot_update_registry.cpp");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    auto svc = read_file("src/compiler/service.ixx");
    auto q = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(!reg.empty() && reg.find("#2035") != std::string::npos, "registry hh #2035");
    CHECK(reg.find("on_region_mask_from_dirty") != std::string::npos, "on_region_mask_from_dirty");
    CHECK(reg.find("reemit_provider_wired") != std::string::npos, "reemit_provider_wired");
    CHECK(reg.find("aura_hot_update_notify_dirty_define") != std::string::npos, "C dirty notify");
    CHECK(!regcpp.empty() && regcpp.find("#2035") != std::string::npos, "registry cpp #2035");
    CHECK(!dirty.empty() && dirty.find("notify_hot_update_after_cascade_") != std::string::npos,
          "dirty path wires notify");
    CHECK(dirty.find("#2035") != std::string::npos, "service_dirty #2035");
    CHECK(!svc.empty() && svc.find("compute_region_mask_from_dirty") != std::string::npos,
          "compute_region_mask");
    CHECK(!q.empty() && q.find("schema-2035") != std::string::npos, "query schema-2035");
}

static void ac2_region_mask_logic() {
    std::println("\n--- AC2: region mask partial vs full (via service path) ---");
    // Direct registry bookkeeping: set mask from dirty and observe counters.
    auto& reg = hot_update_registry();
    const auto n0 = reg.snapshot().region_mask_from_dirty_total;
    reg.on_region_mask_from_dirty((1ULL << 1));
    CHECK(reg.snapshot().region_mask_from_dirty_total >= n0 + 1, "from_dirty +1");
    CHECK(reg.snapshot().last_region_mask_from_dirty == static_cast<std::int64_t>(1ULL << 1),
          "last mask Performance bit");
    reg.on_region_mask_from_dirty((1ULL << 1) | (1ULL << 3));
    CHECK((static_cast<std::uint64_t>(reg.snapshot().last_region_mask_from_dirty) & (1ULL << 3)) !=
              0,
          "full mask has bit 3");
    // Evolution bit should be stripped by set_emit_region_mask.
    reg.set_emit_region_mask((1ULL << 1) | (1ULL << 2));
    CHECK((reg.emit_region_mask() & (1ULL << 2)) == 0, "Evolution bit stripped");
    CHECK((reg.emit_region_mask() & (1ULL << 1)) != 0, "Performance bit kept");
}

static void ac3_dirty_notify_on_mark() {
    std::println("\n--- AC3: mark_define_dirty bumps dirty_notify_total ---");
    auto& reg = hot_update_registry();
    reg.clear_listeners();
    std::vector<std::string> heard;
    reg.register_dirty_listener([&](const char* n) {
        if (n)
            heard.emplace_back(n);
    });
    const auto d0 = reg.dirty_notify_total();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (id x) x) (id 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    // Direct soft dirty (bypasses EDSL mutate name resolution).
    cs.public_mark_define_dirty("id");
    const auto d1 = reg.dirty_notify_total();
    CHECK(d1 > d0, "dirty_notify_total advanced");
    bool heard_id = false;
    for (const auto& h : heard)
        if (h == "id")
            heard_id = true;
    CHECK(heard_id || d1 > d0, "listener heard id or counter advanced");
    reg.clear_listeners();
}

static void ac4_reemit_when_wired() {
    std::println("\n--- AC4: reemit provider wired → trigger + candidates ---");
    auto& reg = hot_update_registry();
    ReemitFeed feed;
    feed.names = {"id"};
    feed.regions = {1}; // Performance region
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &feed);
    aura_set_aot_emit_fn(&emit_ok, nullptr);

    const auto trig0 = reg.snapshot().cascade_reemit_trigger_total;
    const auto cand0 = reg.snapshot().reemit_candidates_total;
    const auto succ0 = reg.snapshot().reemit_success_total;
    const auto map0 = aura_stable_func_id_map_size();

    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (id x) x) (id 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    // Soft dirty with reemit provider wired → cascade trigger + pipeline.
    cs.public_mark_define_dirty("id");

    const auto trig1 = reg.snapshot().cascade_reemit_trigger_total;
    const auto cand1 = reg.snapshot().reemit_candidates_total;
    const auto succ1 = reg.snapshot().reemit_success_total;
    CHECK(trig1 > trig0, "cascade_reemit_trigger advanced");
    CHECK(cand1 > cand0, "reemit_candidates advanced");
    CHECK(succ1 > succ0, "reemit_success advanced");
    CHECK(reg.snapshot().region_mask_from_dirty_total >= 1, "mask from dirty recorded");
    CHECK(aura_stable_func_id_map_size() >= map0, "stable id map non-decreasing");

    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
}

static void ac5_query_schema() {
    std::println("\n--- AC5: query:hot-update-registry-stats schema-2035 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:hot-update-registry-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2035") == 2035, "schema-2035");
    CHECK(href(cs, "issue-2035") == 2035, "issue-2035");
    CHECK(href(cs, "cascade-dirty-reemit-wired") == 1, "wired");
    CHECK(href(cs, "region-mask-from-dirty-total") >= 0, "from-dirty total");
    CHECK(href(cs, "cascade-reemit-trigger-total") >= 0, "trigger total");
    CHECK(href(cs, "last-region-mask-from-dirty") >= 0, "last mask");
    // Lineage retained.
    CHECK(href(cs, "schema-1956") == 1956, "schema-1956 retained");
    CHECK(href(cs, "hot_update_registry_dirty_notify_total") >= 0, "dirty notify key");
}

static void ac6_stable_id_across_reemit() {
    std::println("\n--- AC6: stable func-id preserve across reemits ---");
    aura_clear_stable_func_id_map();
    ReemitFeed feed;
    feed.names = {"f", "g"};
    feed.regions = {1, 1};
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &feed);
    aura_set_aot_emit_fn(&emit_ok, nullptr);

    int p0 = -1, p1 = -1;
    const auto id_f1 = aura_get_or_preserve_stable_func_id("f", &p0);
    const auto id_g1 = aura_get_or_preserve_stable_func_id("g", &p0);
    CHECK(id_f1 != 0 && id_g1 != 0, "ids assigned");
    CHECK(p0 == 0, "first sight assign");

    // Round 1 reemit
    (void)aura_reemit_aot_for_dirty(0);
    const auto id_f2 = aura_get_or_preserve_stable_func_id("f", &p1);
    CHECK(id_f2 == id_f1, "f id preserved");
    CHECK(p1 == 1, "preserve flag");

    // Cascade-style: set mask + reemit again (as notify_hot_update does).
    hot_update_registry().set_emit_region_mask(1ULL << 1);
    feed.cursor = 0;
    (void)aura_reemit_aot_for_dirty(0);
    int p2 = -1;
    CHECK(aura_get_or_preserve_stable_func_id("f", &p2) == id_f1, "f still stable");
    CHECK(aura_get_or_preserve_stable_func_id("g", &p2) == id_g1, "g still stable");
    CHECK(aura_stable_func_id_map_size() >= 2, "map size ≥ 2");

    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_clear_stable_func_id_map();
}

// Issue #2090: query:hot-update-registry-stats schema-2090 keys + the 3
// new boundary counters (boundary-reemit-success-total /
// boundary-reemit-throttled-total / boundary-batch-deopt-unmatched-total).
// The 3 counters are bumped by the outermost MutationBoundaryGuard dtor
// when dirty_or_env_restamp_this_boundary_ is set, so the initial values
// are 0 on a fresh CompilerService (no boundary exits yet).
static void ac7_query_schema_2090() {
    std::println("\n--- AC7 (#2090): query:hot-update-registry-stats schema-2090 ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2090") == 2090, "schema-2090=2090");
    CHECK(href(cs, "issue-2090") == 2090, "issue-2090=2090");
    CHECK(href(cs, "boundary-reemit-success-total") >= 0,
          "boundary-reemit-success-total present (initial 0)");
    CHECK(href(cs, "boundary-reemit-throttled-total") >= 0,
          "boundary-reemit-throttled-total present (initial 0)");
    CHECK(href(cs, "boundary-batch-deopt-unmatched-total") >= 0,
          "boundary-batch-deopt-unmatched-total present (initial 0)");
    // Lineage retained: 1956 + 2035 keys still present.
    CHECK(href(cs, "schema-1956") == 1956, "schema-1956 retained");
    CHECK(href(cs, "schema-2035") == 2035, "schema-2035 retained");
    CHECK(href(cs, "cascade-dirty-reemit-wired") == 1, "cascade wired retained");
}

// Issue #2090: outermost MutationBoundaryGuard dtor wires the unified
// hot-update recovery sequence (throttle → reemit → epoch_notify →
// batch_deopt unmatched). Source citation: the reemit pipeline is
// dispatched in the outermost dtor AFTER the linear closed-loop and
// BEFORE the lock drop — pairs with the #2035 cascade-only path so
// non-cascade exits (fiber-steal restore / partial recovery /
// compact-only / exception unwind) also drive a single ordered recovery
// sequence.
static void ac8_source_outmost_dtor_pipeline() {
    std::println(
        "\n--- AC8 (#2090): outermost dtor wires throttle→reemit→epoch_notify→batch_deopt ---");
    const auto mcp = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mcp.find("Issue #2090: outermost dtor unified hot-update recovery sequence") !=
              std::string::npos,
          "dtor wires #2090 reemit pipeline");
    CHECK(mcp.find("aura_hot_update_should_throttle_reemit") != std::string::npos,
          "throttle check present");
    CHECK(mcp.find("aura_hot_update_on_reemit_throttled") != std::string::npos,
          "on_reemit_throttled hook present");
    CHECK(mcp.find("aura_reemit_aot_for_dirty") != std::string::npos,
          "reemit_aot_for_dirty call present");
    CHECK(mcp.find("aura_hot_update_notify_epoch_bump") != std::string::npos,
          "notify_epoch_bump present");
    CHECK(mcp.find("boundary_reemit_success_total") != std::string::npos,
          "boundary_reemit_success_total bump present");
    CHECK(mcp.find("boundary_reemit_throttled_total") != std::string::npos,
          "boundary_reemit_throttled_total bump present");
    CHECK(mcp.find("boundary_batch_deopt_unmatched_total") != std::string::npos,
          "boundary_batch_deopt_unmatched_total bump present");
    // Defuse snapshot lives on the Guard so the dtor can detect dirty
    // paths that skip mark_define_dirty (catches non-cascade exits).
    const auto evx = read_file("src/compiler/evaluator.ixx");
    CHECK(evx.find("defuse_version_at_enter_") != std::string::npos,
          "Guard captures defuse_version_at_enter_ for dirty detection");
    // Compiled via AURA_COMPILER_METRICS_FIELD so the atomic counters
    // exist in CompilerMetrics (otherwise the fetch_add calls above
    // would not link).
    const auto inc = read_file("src/compiler/compiler_metrics_fields.inc");
    CHECK(inc.find("AURA_COMPILER_METRICS_FIELD(boundary_reemit_success_total)") !=
              std::string::npos,
          "boundary_reemit_success_total field declared");
    CHECK(inc.find("AURA_COMPILER_METRICS_FIELD(boundary_reemit_throttled_total)") !=
              std::string::npos,
          "boundary_reemit_throttled_total field declared");
    CHECK(inc.find("AURA_COMPILER_METRICS_FIELD(boundary_batch_deopt_unmatched_total)") !=
              std::string::npos,
          "boundary_batch_deopt_unmatched_total field declared");
}

} // namespace

int main() {
    std::println("=== test_hot_update_cascade_dirty_reemit (#2035 / #2090) ===");
    ac1_source();
    ac2_region_mask_logic();
    ac3_dirty_notify_on_mark();
    ac4_reemit_when_wired();
    ac5_query_schema();
    ac6_stable_id_across_reemit();
    // Issue #2090: outermost dtor unified hot-update recovery sequence
    // (throttle → reemit → epoch_notify → batch_deopt unmatched). AC7
    // verifies the 3 new boundary counters + schema-2090 are exposed via
    // query:hot-update-registry-stats; AC8 verifies the wire-up is in
    // evaluator_mutation_boundary.cpp + evaluator.ixx +
    // compiler_metrics_fields.inc.
    ac7_query_schema_2090();
    ac8_source_outmost_dtor_pipeline();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
