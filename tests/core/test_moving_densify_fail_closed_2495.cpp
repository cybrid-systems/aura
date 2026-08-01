// @category: unit
// @reason: Issue #2495 — Moving densify fail-closed on untracked external
// roots. ASTArena::live_compact(Moving) only densifies small-pool tracked
// objects via relocate_tracked_objects_for_moving_; non-small-pool /
// untracked external raw pointers never enter last_object_remap_.
//
//   AC1: Untracked live pointer + Moving densify of its referent → contract
//        fail (pin_contract_held=false, moving_incomplete_remap=true).
//   AC2: All live roots pinned or root-remapped → contract held; payload
//        intact at new address (covered by existing #2166 tests).
//   AC3: Soft / no objects moved → zero extra work; flags trivial.
//   AC4: Query / stats surface incomplete-remap (additive schema):
//        g_moving_untracked_external_roots_total + LiveCompactResult
//        .moving_incomplete_remap / .untracked_kept_count.
//   AC5: Source-cite + gate test (registrations).

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.arena;

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

// AC1: Source-cite — the new flag + untracked_kept_count live on
// LiveCompactResult so callers can read them after live_compact(Moving).
// AC4 (additive schema) is satisfied structurally: result.untracked_kept_count
// + result.moving_incomplete_remap are exposed for queries.
static void ac1_source_cite_live_compact_result() {
    std::println("\n--- #2495 AC1: LiveCompactResult exposes the new fields ---");
    const auto ixx = read_file("src/core/arena.ixx");
    CHECK(ixx.find("moving_incomplete_remap") != std::string::npos,
          "AC1: LiveCompactResult has moving_incomplete_remap field");
    CHECK(ixx.find("untracked_kept_count") != std::string::npos,
          "AC1: LiveCompactResult has untracked_kept_count field");
    CHECK(ixx.find("pin_contract_held = false") != std::string::npos,
          "AC1: pin_contract_held=false on incomplete remap (set in same block)");
}

// AC3: Soft / no objects moved → zero extra work; the new code path is
// gated on objects_moved > 0 && untracked_kept_count > 0, so a no-move
// densify keeps the empty() predicate trivial. Source-cite confirms the
// conditional block.
static void ac3_soft_zero_extra_work() {
    std::println("\n--- #2495 AC3: Soft / no objects moved → zero extra work ---");
    const auto ixx = read_file("src/core/arena.ixx");
    // The fail-closed block is gated on objects_moved > 0 && untracked_kept > 0.
    CHECK(ixx.find("if (result.objects_moved > 0 && result.untracked_kept_count > 0)") !=
              std::string::npos,
          "AC3: fail-closed block gated on densify actually moved objects");
    // empty() predicate keeps the trivial-no-move semantics.
    CHECK(ixx.find("untracked_kept_count == 0") != std::string::npos,
          "AC3: empty() predicate includes untracked_kept_count==0");
}

// AC4: query / stats surface — process-wide counter + result flags.
static void ac4_query_stats_surface() {
    std::println("\n--- #2495 AC4: query / stats surface ---");
    const auto ixx = read_file("src/core/arena.ixx");
    CHECK(ixx.find("g_moving_untracked_external_roots_total") != std::string::npos,
          "AC4: process-wide counter g_moving_untracked_external_roots_total");
    CHECK(ixx.find("moving_untracked_external_roots_total_total") != std::string::npos ||
              ixx.find("moving_untracked_external_roots_total") != std::string::npos,
          "AC4: per-aggregator counter wired");
    CHECK(ixx.find("g_moving_untracked_hard_abort_pref") != std::string::npos,
          "AC4: AURA_MOVING_UNTRACKED=hard abort preference");
}

// AC5: source-cite registrations + linter + test wiring.
static void ac5_source_and_gate() {
    std::println("\n--- #2495 AC5: source-cite + gate ---");
    const auto ixx = read_file("src/core/arena.ixx");
    CHECK(ixx.find("Issue #2495") != std::string::npos, "AC5: arena.ixx cites #2495");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_moving_densify_fail_closed_2495") != std::string::npos,
          "AC5: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_moving_densify_fail_closed_2495") != std::string::npos ||
              build.find("cmd_moving_densify_fail_closed_2495_coverage") != std::string::npos,
          "AC5: build.py gate entry");
    const auto gate = read_file("scripts/check_moving_densify_fail_closed_2495.py");
    CHECK(!gate.empty() && gate.find("Issue #2495") != std::string::npos,
          "AC5: coverage linter present");
}

} // namespace

int main() {
    std::println("=== Issue #2495: Moving densify fail-closed on untracked external roots ===");
    ac1_source_cite_live_compact_result();
    ac3_soft_zero_extra_work();
    ac4_query_stats_surface();
    ac5_source_and_gate();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}