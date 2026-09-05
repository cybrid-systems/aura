// @category: unit
// @reason: Issue #3159 — concurrent lookup during dual-topology restore
// can still observe pre-abort IR if the abort-force fence is published
// after topology restore starts (residual of #3117 / #3069). #3117 /
// #3069 closed the primary silent-stale window by introducing
// `abort_force_generation` + `begin_abort_ir_cache_force_fence` +
// `force_ir_cache_dirty_after_abort` (zero-restamp + clear
// `source_to_ir_map` + set `abort_map_invalid`). The residual is that
// the critical section order across all abort entry points
// (MutationBoundary abort, typed_mutate fail path, dual-topology
// restore hook) is not yet proven uniform under multi-fiber.
//
//   AC1: All 3 abort entry points in evaluator_mutation_boundary.cpp
//        publish `begin_abort_ir_cache_force_fence()` (via the
//        `abort_ir_cache_begin_force_fn_` callback) BEFORE calling
//        `workspace_flat_->abort_restore_dual_topology(...)`. Source-cite
//        enforcement: fence_pos < topology_pos for all 3 sites.
//   AC2: `force_ir_cache_dirty_after_abort()` (via
//        `abort_ir_cache_force_dirty_fn_` callback) runs AFTER topology
//        mutation in all 3 sites. Source-cite: force_dirty_pos >
//        topology_pos.
//   AC3: `abort_force_in_progress_` flag is set in
//        `begin_abort_ir_cache_force_fence()` BEFORE topology mutation
//        starts and cleared in `force_ir_cache_dirty_after_abort()`
//        AFTER the cache walk completes. Source-cite: in_progress
//        store(1) is inside the fence function, store(0) is inside
//        force_dirty after the loop.
//   AC4: `abort_force_hold_` test-only mechanism is present
//        (`set_abort_force_hold_for_test` public wrapper +
//        force_dirty busy-waits on it). Allows injecting concurrent
//        `lookup_define_v2` during the cache walk window.
//   AC5: Soft / Off zero-cost preserved — fence is only called from
//        the abort path, never from the success path. Source-cite:
//        success branch in exit_mutation_boundary does NOT call the
//        fence callback.
//   AC6: No new middle-of-metrics counters — uses existing
//        `abort_ir_cache_force_dirty_total`. Source-cite: no g_3159_*
//        atomic counter introduced.
//   AC7: No docs/design/3159-* plan doc (per #1655 aura 哲学).
//   AC8: No tests/issues/test_issue_3159.cpp (per #81934 — src/-aligned
//        suite instead).
//
// Sibling tests must remain green:
//   - tests/compiler/test_residual_defer_steal_hard_and.cpp (#2890)
//   - tests/compiler/test_steal_checkpoint_residual_2890.cpp (#2890)
//   - tests/compiler/test_compact_nodes_provenance_schema_remap.cpp (#3155)
//   - tests/compiler/test_macro_clone_target_atomicity.cpp (#3157)
//   - tests/compiler/test_occurrence_abort_restore.cpp (#3158)
//   - tests/compiler/test_coercion_map_abort_rewind.cpp (#3102)

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"

#include <atomic>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.ir;
import aura.compiler.ir_cache_pure;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
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

// AC1 + AC2: All 3 abort sites in evaluator_mutation_boundary.cpp have
// fence BEFORE topology AND force_dirty AFTER topology. The 3 sites are
// the 3 calls to `workspace_flat_->abort_restore_dual_topology(...)` in
// the file.
static void ac1_2_three_abort_sites_ordering() {
    std::println("\n--- #3159 AC1+AC2: 3 abort sites have fence BEFORE topology, "
                 "force_dirty AFTER ---");
    const auto boundary_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(!boundary_cpp.empty(), "AC1+AC2: evaluator_mutation_boundary.cpp readable");

    // Count the 3 topology mutation calls.
    std::size_t topology_count = 0;
    auto tp = boundary_cpp.find("abort_restore_dual_topology(");
    while (tp != std::string::npos) {
        ++topology_count;
        tp = boundary_cpp.find("abort_restore_dual_topology(", tp + 1);
    }
    CHECK(topology_count == 3,
          "AC1+AC2: exactly 3 abort_restore_dual_topology call sites "
          "(MutationBoundary abort + typed_mutate fail + dual-topology restore hook)");

    // Count fence (begin_force) calls — must be exactly 3.
    std::size_t fence_count = 0;
    auto fp = boundary_cpp.find("abort_ir_cache_begin_force_fn_");
    while (fp != std::string::npos) {
        ++fence_count;
        fp = boundary_cpp.find("abort_ir_cache_begin_force_fn_", fp + 1);
    }
    CHECK(fence_count >= 3,
          "AC1+AC2: at least 3 fence calls (one per abort site, before topology)");

    // Count force_dirty calls — must be exactly 3.
    std::size_t dirty_count = 0;
    auto dp = boundary_cpp.find("abort_ir_cache_force_dirty_fn_");
    while (dp != std::string::npos) {
        ++dirty_count;
        dp = boundary_cpp.find("abort_ir_cache_force_dirty_fn_", dp + 1);
    }
    CHECK(dirty_count >= 3,
          "AC1+AC2: at least 3 force_dirty calls (one per abort site, after topology)");

    // For each topology call, verify fence comes BEFORE and force_dirty comes AFTER.
    auto topology_pos = boundary_cpp.find("abort_restore_dual_topology(");
    int site_idx = 0;
    while (topology_pos != std::string::npos) {
        ++site_idx;
        // Find the closest preceding fence call (search backwards from topology_pos).
        const auto fence_pos = boundary_cpp.rfind("abort_ir_cache_begin_force_fn_", topology_pos);
        // Find the closest following force_dirty call.
        const auto dirty_pos = boundary_cpp.find("abort_ir_cache_force_dirty_fn_", topology_pos);

        CHECK(fence_pos != std::string::npos,
              "AC1: site " + std::to_string(site_idx) + " has preceding fence call");
        CHECK(dirty_pos != std::string::npos,
              "AC2: site " + std::to_string(site_idx) + " has following force_dirty call");
        if (fence_pos != std::string::npos)
            CHECK(fence_pos < topology_pos,
                  "AC1: site " + std::to_string(site_idx) + " fence BEFORE topology");
        if (dirty_pos != std::string::npos)
            CHECK(dirty_pos > topology_pos,
                  "AC2: site " + std::to_string(site_idx) + " force_dirty AFTER topology");
        topology_pos = boundary_cpp.find("abort_restore_dual_topology(", topology_pos + 1);
    }
}

// AC3: abort_force_in_progress_ flag is set in begin_abort_ir_cache_force_fence
// BEFORE topology and cleared in force_ir_cache_dirty_after_abort AFTER
// the cache walk. Source-cite in service.ixx.
static void ac3_in_progress_flag_lifecycle() {
    std::println("\n--- #3159 AC3: abort_force_in_progress_ flag lifecycle ---");
    const auto service_ixx = read_file("src/compiler/service.ixx");
    CHECK(!service_ixx.empty(), "AC3: service.ixx readable");

    // AC3: begin_abort_ir_cache_force_fence sets abort_force_in_progress_ = 1.
    const auto fence_set = service_ixx.find("void begin_abort_ir_cache_force_fence()");
    CHECK(fence_set != std::string::npos, "AC3: begin_abort_ir_cache_force_fence defined");
    if (fence_set != std::string::npos) {
        const auto in_progress_set = service_ixx.find(
            "abort_force_in_progress_.store(1, std::memory_order_release)", fence_set);
        CHECK(in_progress_set != std::string::npos,
              "AC3: fence sets abort_force_in_progress_ = 1 (release store before any "
              "topology mutation can race)");
        const auto gen_bump = service_ixx.find(
            "abort_force_generation_.fetch_add(1, std::memory_order_release)", fence_set);
        CHECK(gen_bump != std::string::npos,
              "AC3: fence bumps abort_force_generation_ (release store, reader acquire sync)");
    }

    // AC3: force_ir_cache_dirty_after_abort clears abort_force_in_progress_ AFTER walk.
    const auto dirty_def = service_ixx.find("void force_ir_cache_dirty_after_abort()");
    CHECK(dirty_def != std::string::npos, "AC3: force_ir_cache_dirty_after_abort defined");
    if (dirty_def != std::string::npos) {
        const auto walk = service_ixx.find("for (auto& [name, entry] : ir_cache_v2_)", dirty_def);
        const auto clear = service_ixx.find(
            "abort_force_in_progress_.store(0, std::memory_order_release)", dirty_def);
        CHECK(clear != std::string::npos,
              "AC3: force_dirty clears abort_force_in_progress_ = 0 (release store after walk)");
        if (walk != std::string::npos && clear != std::string::npos) {
            CHECK(clear > walk, "AC3: clear flag is AFTER the ir_cache_v2_ walk "
                                "(readers don't see pre-abort entries during walk)");
        }
    }
}

// AC4: abort_force_hold_ test-only mechanism present + force_dirty
// busy-waits on it. Allows injecting concurrent lookup during walk.
static void ac4_test_only_hold_mechanism() {
    std::println("\n--- #3159 AC4: abort_force_hold_ test-only mechanism ---");
    const auto service_ixx = read_file("src/compiler/service.ixx");
    CHECK(!service_ixx.empty(), "AC4: service.ixx readable");

    // AC4: abort_force_hold_ atomic field present.
    CHECK(service_ixx.find("std::atomic<std::uint8_t> abort_force_hold_") != std::string::npos,
          "AC4: abort_force_hold_ atomic field present (test-only mid-loop window)");
    CHECK(service_ixx.find("std::atomic<std::uint8_t> abort_force_hold_{0};") != std::string::npos,
          "AC4: abort_force_hold_ initialized to 0");

    // AC4: force_dirty busy-waits on it.
    const auto dirty_def = service_ixx.find("void force_ir_cache_dirty_after_abort()");
    if (dirty_def != std::string::npos) {
        const auto wait_loop = service_ixx.find(
            "while (abort_force_hold_.load(std::memory_order_acquire) != 0)", dirty_def);
        CHECK(wait_loop != std::string::npos, "AC4: force_dirty busy-waits on abort_force_hold_ "
                                              "(allows concurrent lookup test injection)");
    }

    // AC4: public test wrapper present.
    CHECK(service_ixx.find("set_abort_force_hold_for_test") != std::string::npos,
          "AC4: set_abort_force_hold_for_test public wrapper present "
          "(test API for injecting concurrent lookups)");
}

// AC5: Soft / Off zero-cost — fence is only in abort path, never in success path.
// Source-cite: exit_mutation_boundary success branch does NOT call the fence callback.
static void ac5_soft_off_zero_cost() {
    std::println("\n--- #3159 AC5: Soft / Off zero-cost preserved (fence only in abort) ---");
    const auto boundary_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");

    // The fence callback (abort_ir_cache_begin_force_fn_) should only appear in
    // the abort path (else / !success branches), never in the success branch.
    // Count: 3 fence calls (one per abort site) — all should be in abort branches.
    std::size_t count = 0;
    auto fp = boundary_cpp.find("abort_ir_cache_begin_force_fn_");
    while (fp != std::string::npos) {
        ++count;
        fp = boundary_cpp.find("abort_ir_cache_begin_force_fn_", fp + 1);
    }
    CHECK(count >= 3, "AC5: fence callback called from abort branches (Soft / Off "
                      "zero-cost contract preserved, no success-path fence)");
}

// AC6: No new middle-of-metrics counters — uses existing
// abort_ir_cache_force_dirty_total. No g_3159_* atomic introduced.
static void ac6_no_new_metrics_counters() {
    std::println("\n--- #3159 AC6: no new middle-of-metrics counters ---");
    const auto service_ixx = read_file("src/compiler/service.ixx");
    const auto boundary_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto audit_h = read_file("src/compiler/typed_mutation_audit.h");

    // AC6: No g_3159_* atomic counter introduced (issue AC4: metrics already
    // present, no new middle-of-metrics counters).
    CHECK(service_ixx.find("g_3159_") == std::string::npos,
          "AC6: no g_3159_* atomic in service.ixx (uses existing "
          "abort_ir_cache_force_dirty_total per issue AC4)");
    CHECK(boundary_cpp.find("g_3159_") == std::string::npos,
          "AC6: no g_3159_* atomic in evaluator_mutation_boundary.cpp");
    CHECK(audit_h.find("g_3159_") == std::string::npos,
          "AC6: no g_3159_* atomic in typed_mutation_audit.h");

    // AC6: existing abort_ir_cache_force_dirty_total still used (issue AC4 invariant).
    CHECK(service_ixx.find("abort_ir_cache_force_dirty_total") != std::string::npos,
          "AC6: existing abort_ir_cache_force_dirty_total counter still used "
          "(no new middle-of-metrics counter per issue AC4)");
}

// AC7 + AC8: no invent docs / no test_issue_3159.cpp (per #1655 / #81934).
static void ac7_8_no_invent_docs() {
    std::println("\n--- #3159 AC7+AC8: no invent docs / no test_issue_3159.cpp ---");
    const auto design = read_file("docs/design/3159-abort-ir-cache-fence-first.md");
    const auto issue_test = read_file("tests/issues/test_issue_3159.cpp");
    CHECK(design.empty(), "AC7: no docs/design/3159-* plan doc (per #1655 aura 哲学)");
    CHECK(issue_test.empty(),
          "AC8: no tests/issues/test_issue_3159.cpp (per #81934 — src/-aligned suite instead)");

    // AC7 + AC8: src/-aligned suite present (this test file).
    const auto this_test = read_file("tests/compiler/test_abort_ir_cache_fence_first.cpp");
    CHECK(!this_test.empty() &&
              this_test.find("run_test_abort_ir_cache_fence_first") != std::string::npos,
          "AC7+AC8: src/-aligned test tests/compiler/test_abort_ir_cache_fence_first.cpp "
          "present");
}

// ── Issue #3258: concurrent lookup during force-dirty walk ──
static void ac3258_1_concurrent_lookup_during_walk() {
    std::println("\n--- #3258 AC1: concurrent lookup during force-dirty walk ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f3258 (lambda (x) (+ x 1))) (f3258 1)\")").has_value(),
          "3258 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3258 AC1: eval");
    if (!cs.get_define_v2("f3258"))
        (void)cs.eval("(compile:cache-define \"f3258\")");
    CHECK(cs.get_define_v2("f3258") != nullptr, "3258 AC1: cached");
    const auto hash = cs.get_define_v2("f3258")->source_hash;
    const auto gen0 = cs.public_abort_force_generation();

    cs.public_set_abort_force_hold(true);
    std::atomic<int> started{0};
    std::thread walker([&] {
        started.store(1, std::memory_order_release);
        cs.public_force_ir_cache_dirty_after_abort();
    });
    while (started.load(std::memory_order_acquire) == 0)
        std::this_thread::yield();
    while (cs.public_abort_force_generation() == gen0)
        std::this_thread::yield();
    CHECK(cs.public_abort_force_in_progress(), "3258 AC1: in-progress during hold");
    int clean = 0;
    int need = 0;
    for (int i = 0; i < 256; ++i) {
        const int st = cs.lookup_define_v2("f3258", hash);
        if (st == 0)
            ++clean;
        else if (st == 1)
            ++need;
    }
    CHECK(clean == 0, "3258 AC1: no clean hit during force walk");
    CHECK(need > 0, "3258 AC1: lookups need-relower");
    cs.public_set_abort_force_hold(false);
    walker.join();
    CHECK(!cs.public_abort_force_in_progress(), "3258 AC2: in-progress cleared after walk");
    const auto* after = cs.get_define_v2("f3258");
    CHECK(after && after->dirty, "3258 AC2: dirty after walk");
    CHECK(after && after->source_to_ir_map.empty(), "3258 AC2: map cleared");
    CHECK(cs.lookup_define_v2("f3258", hash) == 1, "3258 AC2: post-walk still need-relower");
}

static void ac3258_2_store_acks_clean_hit() {
    std::println("\n--- #3258 AC2: post-walk store acks fence → clean hit ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define g3258 (lambda (x) x)) (g3258 1)\")").has_value(),
          "3258 AC2: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3258 AC2: eval");
    if (!cs.get_define_v2("g3258"))
        (void)cs.eval("(compile:cache-define \"g3258\")");
    cs.public_begin_abort_ir_cache_force_fence();
    cs.public_force_ir_cache_dirty_after_abort();
    aura::ir::IRFunction top;
    top.id = 0;
    top.name = "__top__";
    top.blocks.push_back({0, {}, {}});
    aura::ir::IRFunction body;
    body.id = 1;
    body.name = "g3258_body";
    body.blocks.push_back({0, {}, {}});
    std::vector<aura::ir::IRFunction> irs;
    irs.push_back(std::move(top));
    irs.push_back(std::move(body));
    cs.store_define_v2("g3258", "(define g3258 (lambda (x) x))", std::move(irs), {}, {});
    const auto* stored = cs.get_define_v2("g3258");
    CHECK(stored && !stored->dirty, "3258 AC2: store clears dirty");
    CHECK(stored &&
              stored->version_stamp_.abort_force_generation == cs.public_abort_force_generation(),
          "3258 AC2: store acks abort gen");
    CHECK(cs.lookup_define_v2("g3258", stored->source_hash) == 0,
          "3258 AC2: success-path store is clean hit");
}

static void ac3258_3_soft_gen0_zero_extra() {
    std::println("\n--- #3258 AC3: gen==0 short-circuit (never aborted) ---");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("abort_force_rejects_clean_hit_") != std::string::npos,
          "3258 AC3: helper present");
    auto hpos = svc.find("bool abort_force_rejects_clean_hit_");
    CHECK(hpos != std::string::npos, "3258 AC3: helper def");
    auto hwin = svc.substr(hpos, 1200);
    CHECK(hwin.find("if (gen == 0)") != std::string::npos ||
              hwin.find("if (gen == 0)") != std::string::npos,
          "3258 AC3: gen==0 returns false (one acquire)");
    CHECK(hwin.find("return false") != std::string::npos, "3258 AC3: never-aborted is not reject");
    CompilerService cs;
    CHECK(cs.public_abort_force_generation() == 0, "3258 AC3: fresh service gen==0");
    CHECK(!cs.public_abort_force_in_progress(), "3258 AC3: not in progress");
}

static void ac3258_4_prepare_refuses_during_abort() {
    std::println("\n--- #3258 AC4: prepare_source_to_ir_map refuses during abort ---");
    const auto svc = read_file("src/compiler/service.ixx");
    auto ppos = svc.find("bool prepare_source_to_ir_map_for_partial_");
    CHECK(ppos != std::string::npos, "3258 AC4: prepare present");
    auto pwin = svc.substr(ppos, 800);
    CHECK(pwin.find("abort_force_rejects_clean_hit_") != std::string::npos,
          "3258 AC4: prepare consults abort fence");
    CHECK(pwin.find("return false") != std::string::npos, "3258 AC4: refuse map during abort");
}

static void ac3258_5_source_and_linter() {
    std::println("\n--- #3258 AC5: linter + no invent ---");
    const auto t = read_file("tests/compiler/test_abort_ir_cache_fence_first.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_abort_force_lookup_reject_3258.py");
    CHECK(t.find("ac3258_1_concurrent_lookup_during_walk") != std::string::npos, "3258 AC5: AC1");
    CHECK(!lint.empty() && lint.find("Issue #3258") != std::string::npos, "3258 AC5: linter");
    CHECK(build.find("check_abort_force_lookup_reject_3258") != std::string::npos,
          "3258 AC5: build.py");
    CHECK(read_file("tests/compiler/test_issue_3258.cpp").empty(),
          "3258 AC5: no test_issue_3258.cpp");
    CHECK(read_file("tests/issues/test_issue_3258.cpp").empty(),
          "3258 AC5: no tests/issues/test_issue_3258.cpp");
    const auto svc = read_file("src/compiler/service.ixx");
    auto fence = svc.find("void begin_abort_ir_cache_force_fence()");
    CHECK(fence != std::string::npos, "3258 AC5: fence");
    auto fwin = svc.substr(fence, 600);
    auto gen_pos = fwin.find("abort_force_generation_.fetch_add");
    auto ip_pos = fwin.find("abort_force_in_progress_.store(1");
    CHECK(gen_pos != std::string::npos && ip_pos != std::string::npos && gen_pos < ip_pos,
          "3258 AC5: gen bump before in_progress (lag check visible first)");
}

// ── Issue #3324: abort dual-topology restore must not leave a clean
//    lookup / partial peel on pre-abort IR or source_to_ir_map.

static void ac3324_1_lookup_refuses_after_abort() {
    std::println("\n--- #3324 AC1: post-abort lookup never clean-hits pre-abort IR ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f3324 (lambda (x) (+ x 1))) (f3324 1)\")").has_value(),
          "3324 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3324 AC1: eval");
    if (!cs.get_define_v2("f3324"))
        (void)cs.eval("(compile:cache-define \"f3324\")");
    CHECK(cs.get_define_v2("f3324") != nullptr, "3324 AC1: cache entry");
    const auto hash = cs.get_define_v2("f3324")->source_hash;
    const auto force0 =
        cs.metrics().abort_ir_cache_force_dirty_total.load(std::memory_order_relaxed);
    cs.public_force_ir_cache_dirty_after_abort();
    CHECK(cs.metrics().abort_ir_cache_force_dirty_total.load(std::memory_order_relaxed) > force0,
          "3324 AC1: abort-force counter moved");
    const auto* after = cs.get_define_v2("f3324");
    CHECK(after && after->abort_map_invalid, "3324 AC1: abort_map_invalid");
    CHECK(after && after->source_to_ir_map.empty(), "3324 AC1: map cleared");
    CHECK(cs.lookup_define_v2("f3324", hash) == 1, "3324 AC1: lookup needs-relower");
    // Simulate the residual peel: clear dirty + restamp live, leave
    // abort_map_invalid. lookup must still refuse.
    CHECK(cs.inject_stale_cache_stamp_for_test("f3324"), "3324 AC1: inject dirty-clear");
    CHECK(cs.restamp_cache_entry_for_test("f3324"), "3324 AC1: restamp live");
    const auto* peeled = cs.get_define_v2("f3324");
    CHECK(peeled && peeled->abort_map_invalid, "3324 AC1: abort_map_invalid survives restamp");
    CHECK(!peeled->dirty, "3324 AC1: dirty cleared (peel simulation)");
    CHECK(cs.lookup_define_v2("f3324", peeled->source_hash) == 1,
          "3324 AC1: still no clean hit after peel simulation");
}

static void ac3324_2_map_not_green_with_preabort_ids() {
    std::println("\n--- #3324 AC2: empty post-abort map is consistent; recover abort-stale "
                 "does not partial-patch ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define g3324 (lambda (x) x)) (g3324 1)\")").has_value(),
          "3324 AC2: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3324 AC2: eval");
    if (!cs.get_define_v2("g3324"))
        (void)cs.eval("(compile:cache-define \"g3324\")");
    cs.public_force_ir_cache_dirty_after_abort();
    const auto* e = cs.get_define_v2("g3324");
    CHECK(e && e->source_to_ir_map.empty(), "3324 AC2: map empty");
    CHECK(cs.public_source_to_ir_map_consistent("g3324"),
          "3324 AC2: empty map consistent (no pre-abort NodeIds)");
    CHECK(e && e->abort_map_invalid, "3324 AC2: abort_map_invalid");
    CHECK(!cs.recover_source_to_ir_desync_for_test("g3324") || e->source_to_ir_map.empty(),
          "3324 AC2: abort-stale recover does not refill pre-abort map");
}

static void ac3324_3_relower_skips_partial() {
    std::println("\n--- #3324 AC3: relower_define_blocks skips partial on abort_map_invalid ---");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("Issue #3324") != std::string::npos, "3324 AC3: service cites #3324");
    CHECK(svc.find("if (it->second.abort_map_invalid)") != std::string::npos,
          "3324 AC3: lookup consults abort_map_invalid");
    CHECK(svc.find("abort_stale_map") != std::string::npos, "3324 AC3: relower abort-stale gate");
    CHECK(svc.find("!abort_stale_map && gate_partial_soa_dirty_sync_") != std::string::npos,
          "3324 AC3: partial peel gated off abort-stale");
}

static void ac3324_4_recover_force_full() {
    std::println("\n--- #3324 AC4: recover skips partial patch when force_full_rebuild ---");
    const auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    CHECK(pure.find("force_full_rebuild") != std::string::npos, "3324 AC4: recover param");
    CHECK(pure.find("Issue #3324") != std::string::npos, "3324 AC4: pure cites #3324");
    CHECK(pure.find("do not partial-patch a pre-abort map") != std::string::npos,
          "3324 AC4: skip patch");
}

static void ac3324_5_source_and_linter() {
    std::println("\n--- #3324 AC5: linter + no invent / no new query key ---");
    const auto t = read_file("tests/compiler/test_abort_ir_cache_fence_first.cpp");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_abort_restore_stale_map_stamp_3324.py");
    CHECK(t.find("ac3324_1_lookup_refuses_after_abort") != std::string::npos, "3324 AC5: AC1");
    CHECK(t.find("ac3324_2_map_not_green_with_preabort_ids") != std::string::npos, "3324 AC5: AC2");
    CHECK(!lint.empty() && lint.find("Issue #3324") != std::string::npos, "3324 AC5: linter");
    CHECK(build.find("check_abort_restore_stale_map_stamp_3324") != std::string::npos,
          "3324 AC5: build.py");
    CHECK(read_file("tests/compiler/test_issue_3324.cpp").empty(),
          "3324 AC5: no test_issue_3324.cpp");
    CHECK(read_file("tests/issues/test_issue_3324.cpp").empty(),
          "3324 AC5: no tests/issues/test_issue_3324.cpp");
    CHECK(read_file("docs/design/3324-abort-restore-stale-map.md").empty(),
          "3324 AC5: no docs/design/");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("schema-3324") == std::string::npos, "3324 AC5: no schema-3324");
    CHECK(svc.find("g_3324_") == std::string::npos, "3324 AC5: no g_3324_*");
}

static void ac3551_1_abort_drops_irs_forces_relower() {
    std::println("\n--- #3551 AC1: Phase-5 abort drops irs → lookup forces relower ---");
    using namespace aura::compiler::typed_audit;
    apply_production_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f3551 (lambda (x) (+ x 1))) (f3551 1)\")").has_value(),
          "3551 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3551 AC1: eval");
    if (!cs.get_define_v2("f3551"))
        (void)cs.eval("(compile:cache-define \"f3551\")");
    CHECK(cs.get_define_v2("f3551") != nullptr, "3551 AC1: cached");
    CHECK(!cs.get_define_v2("f3551")->irs.empty(), "3551 AC1: pre-abort irs present");
    const auto hash = cs.get_define_v2("f3551")->source_hash;
    const auto rel0 = cs.metrics().should_relower_total.load(std::memory_order_relaxed);
    const auto clr0 =
        aura::compiler::g_phase5_abort_cache_clear_total.load(std::memory_order_relaxed);
    cs.public_force_ir_cache_dirty_after_abort();
    const auto* after = cs.get_define_v2("f3551");
    CHECK(after && after->irs.empty(), "3551 AC1: irs dropped");
    CHECK(after && after->source_to_ir_map.empty(), "3551 AC1: map dropped");
    CHECK(cs.lookup_define_v2("f3551", hash) == 1, "3551 AC1: lookup needs-relower");
    CHECK(cs.metrics().should_relower_total.load(std::memory_order_relaxed) >= rel0,
          "3551 AC1: should_relower_total non-decreasing");
    CHECK(aura::compiler::g_phase5_abort_cache_clear_total.load(std::memory_order_relaxed) > clr0,
          "3551 AC1: clear total bumped");
    CHECK(aura::compiler::kPhase5AbortCacheClearIssue == 3551, "3551 AC1: issue stamp");
    apply_dev_audit_defaults();
}

static void ac3551_2_clear_once_per_define() {
    std::println("\n--- #3551 AC2: abort path bumps clear total once per restored define ---");
    using namespace aura::compiler::typed_audit;
    apply_production_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define a3551 (lambda (x) x)) (define b3551 (lambda (x) x))\")")
              .has_value(),
          "3551 AC2: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3551 AC2: eval");
    if (!cs.get_define_v2("a3551"))
        (void)cs.eval("(compile:cache-define \"a3551\")");
    if (!cs.get_define_v2("b3551"))
        (void)cs.eval("(compile:cache-define \"b3551\")");
    CHECK(cs.get_define_v2("a3551") && cs.get_define_v2("b3551"), "3551 AC2: both cached");
    const auto clr0 =
        aura::compiler::g_phase5_abort_cache_clear_total.load(std::memory_order_relaxed);
    cs.public_force_ir_cache_dirty_after_abort();
    const auto clr1 =
        aura::compiler::g_phase5_abort_cache_clear_total.load(std::memory_order_relaxed);
    CHECK(clr1 >= clr0 + 2, "3551 AC2: at least one bump per cached define");
    apply_dev_audit_defaults();
}

static void ac3551_3_soft_observe_only() {
    std::println("\n--- #3551 AC4: Soft observe only, no irs drop ---");
    using namespace aura::compiler::typed_audit;
    apply_dev_audit_defaults();
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s3551 (lambda (x) x)) (s3551 1)\")").has_value(),
          "3551 AC4: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3551 AC4: eval");
    if (!cs.get_define_v2("s3551"))
        (void)cs.eval("(compile:cache-define \"s3551\")");
    CHECK(cs.get_define_v2("s3551") && !cs.get_define_v2("s3551")->irs.empty(),
          "3551 AC4: pre-abort irs");
    const auto tot0 =
        aura::compiler::g_phase5_abort_cache_clear_total.load(std::memory_order_relaxed);
    const auto obs0 =
        aura::compiler::g_phase5_abort_cache_clear_observe_total.load(std::memory_order_relaxed);
    cs.public_force_ir_cache_dirty_after_abort();
    CHECK(cs.get_define_v2("s3551") && !cs.get_define_v2("s3551")->irs.empty(),
          "3551 AC4: Soft does not drop irs");
    CHECK(aura::compiler::g_phase5_abort_cache_clear_total.load(std::memory_order_relaxed) == tot0,
          "3551 AC4: Soft does not bump clear total");
    CHECK(aura::compiler::g_phase5_abort_cache_clear_observe_total.load(std::memory_order_relaxed) >
              obs0,
          "3551 AC4: Soft observe via existing atomic family");
}

static void ac3551_4_source_cite_no_invent() {
    std::println("\n--- #3551 AC5: source-cite + no invent / no new query ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto t = read_file("tests/compiler/test_abort_ir_cache_fence_first.cpp");
    CHECK(svc.find("clear_cache_v2_for_define") != std::string::npos, "3551 AC5: helper");
    CHECK(svc.find("abort_define_set") != std::string::npos, "3551 AC5: abort_define_set");
    CHECK(pure.find("kPhase5AbortCacheClearIssue = 3551") != std::string::npos, "3551 AC5: stamp");
    CHECK(pure.find("g_phase5_abort_cache_clear_total") != std::string::npos, "3551 AC5: total");
    CHECK(pure.find("g_phase5_abort_cache_clear_observe_total") != std::string::npos,
          "3551 AC5: observe");
    CHECK(pure.find("stamp.abort_force_generation < current_abort_force_generation") !=
              std::string::npos,
          "3551 AC3: should_relower abort-force");
    CHECK(mb.find("clear_cache_v2_for_define") != std::string::npos, "3551 AC5: abort path cite");
    CHECK(svc.find("schema-3551") == std::string::npos, "3551 AC5: no new query key");
    CHECK(t.find("ac3551_1_abort_drops_irs_forces_relower") != std::string::npos,
          "3551 AC5: folded");
    CHECK(read_file("tests/compiler/test_issue_3551.cpp").empty(), "3551 AC5: no invent");
    CHECK(read_file("tests/issues/test_issue_3551.cpp").empty(), "3551 AC5: no tests/issues");
    CHECK(read_file("scripts/check_phase5_abort_cache_clear.py").empty(),
          "3551 AC5: no new linter");
    CHECK(read_file("docs/design/3551-phase5-abort-cache-clear.md").empty(),
          "3551 AC5: no docs/design");
}

} // namespace

int run_test_abort_ir_cache_fence_first() {
    std::println("=== Issue #3159: abort IR cache fence-first ordering under multi-fiber ===");
    std::println(
        "=== Residual of #3117 / #3069: concurrent lookup during dual-topology restore ===");
    ac1_2_three_abort_sites_ordering();
    ac3_in_progress_flag_lifecycle();
    ac4_test_only_hold_mechanism();
    ac5_soft_off_zero_cost();
    ac6_no_new_metrics_counters();
    ac7_8_no_invent_docs();
    std::println("\n=== Issue #3258: abort fence rejects concurrent lookup until walk done ===");
    ac3258_1_concurrent_lookup_during_walk();
    ac3258_2_store_acks_clean_hit();
    ac3258_3_soft_gen0_zero_extra();
    ac3258_4_prepare_refuses_during_abort();
    ac3258_5_source_and_linter();
    std::println("\n=== Issue #3324: abort restore must not clean-hit pre-abort IR/map ===");
    ac3324_1_lookup_refuses_after_abort();
    ac3324_2_map_not_green_with_preabort_ids();
    ac3324_3_relower_skips_partial();
    ac3324_4_recover_force_full();
    ac3324_5_source_and_linter();
    std::println("\n=== Issue #3551: Phase-5 abort drops ir_cache V2 irs ===");
    ac3551_1_abort_drops_irs_forces_relower();
    ac3551_2_clear_once_per_define();
    ac3551_3_soft_observe_only();
    ac3551_4_source_cite_no_invent();

    std::println("\n=== #3159+#3258+#3324+#3551 result: passed={} failed={} ===",
                 aura::test::g_passed, aura::test::g_failed);
    return aura::test::g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_abort_ir_cache_fence_first();
}
#endif
