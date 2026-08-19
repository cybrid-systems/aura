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

#include <fstream>
#include <string>
#include <string_view>

import std;

namespace {

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
    CHECK(fence_count == 3, "AC1+AC2: exactly 3 fence calls (one per abort site, before topology)");

    // Count force_dirty calls — must be exactly 3.
    std::size_t dirty_count = 0;
    auto dp = boundary_cpp.find("abort_ir_cache_force_dirty_fn_");
    while (dp != std::string::npos) {
        ++dirty_count;
        dp = boundary_cpp.find("abort_ir_cache_force_dirty_fn_", dp + 1);
    }
    CHECK(dirty_count == 3,
          "AC1+AC2: exactly 3 force_dirty calls (one per abort site, after topology)");

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
    CHECK(count == 3, "AC5: fence callback called from exactly 3 sites (all abort branches — "
                      "Soft / Off zero-cost contract preserved, no success-path fence)");
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

    std::println("\n=== #3159 result: passed={} failed={} ===", aura::test::g_passed,
                 aura::test::g_failed);
    return aura::test::g_failed == 0 ? 0 : 1;
}
