// @category: unit
// @reason: Issue #2549 — demote is_stealable() reason-class to
//          is_steal_candidate; force all steal entries to trust
//          MutationSafetySnapshot only.
//
//   AC1: is_stealable() false when depth>0 or held under MutationBoundary
//   AC2: depth0 + candidate reasons remain stealable; weak expectations updated
//   AC3: production call sites use is_stealable(snap) / is_steal_candidate+safe
//   AC4: source-cite + coverage linter greps residual bare misuse
//   AC5: happy path is candidate + existing safe probe (no extra atomics)

#include "test_harness.hpp"

#include "serve/fiber.h"
#include "serve/steal_safety.h"
#include "serve/worker.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;

namespace {

using aura::serve::evaluate_residual_hard_and_bits;
using aura::serve::Fiber;
using aura::serve::fiber_steal_priority;
using aura::serve::MutationSafetySnapshot;
using aura::serve::steal_invariant_mask;
using aura::serve::steal_safety_transaction;
using aura::serve::StealInvariant;
using aura::serve::StealSafetyDecision;
using aura::serve::YieldReason;
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

// ── AC1: held / unsafe MB → is_stealable false; candidate still true ──
static void ac1_held_or_unsafe_mb_not_stealable() {
    std::println("\n--- #2549 AC1: held/unsafe MB → is_stealable false ---");
    Fiber fiber(+[] {}, 64 * 1024);

    fiber.set_yield_reason(YieldReason::MutationBoundary);
    CHECK(fiber.is_steal_candidate(), "AC1: MB is steal candidate (reason class)");
    // depth==0 && !held → snapshot-safe → is_stealable true
    CHECK(fiber.is_at_mutation_boundary_safe(), "AC1: default mirrors safe");
    CHECK(fiber.is_stealable(), "AC1: MB + safe → is_stealable");

    // held=true under MB → not snapshot-safe → is_stealable false
    fiber.publish_mutation_safety_mirrors(/*depth=*/1, /*held=*/true, /*defuse=*/0);
    CHECK(fiber.is_steal_candidate(), "AC1: still candidate under held");
    CHECK(!fiber.is_at_mutation_boundary_safe(), "AC1: held → not safe");
    CHECK(!fiber.is_stealable(), "AC1: held → is_stealable false");

    // Snap overload agrees with zero-arg form
    const auto snap = fiber.mutation_safety_snapshot();
    CHECK(fiber.is_steal_candidate(snap), "AC1: candidate(snap)");
    CHECK(!fiber.is_stealable(snap), "AC1: is_stealable(snap) false when held");

    // Release mirrors → safe again
    fiber.publish_mutation_safety_mirrors(/*depth=*/0, /*held=*/false, /*defuse=*/0);
    CHECK(fiber.is_stealable(), "AC1: after release → stealable again");
}

// ── AC2: Explicit / OpBoundary / PassPipeline / depth0 MB still stealable ──
static void ac2_candidate_reasons_depth0_ok() {
    std::println("\n--- #2549 AC2: candidate reasons at depth0 remain stealable ---");
    Fiber fiber(+[] {}, 64 * 1024);
    fiber.publish_mutation_safety_mirrors(0, false, 0);

    for (auto r : {YieldReason::Explicit, YieldReason::MutationBoundary,
                   YieldReason::OperationBoundary, YieldReason::PassPipeline}) {
        fiber.set_yield_reason(r);
        CHECK(fiber.is_steal_candidate(), "AC2: candidate for yield reason");
        CHECK(fiber.is_stealable(), "AC2: stealable at depth0/!held");
    }

    // BlockingIO is not a steal candidate
    fiber.set_yield_reason(YieldReason::BlockingIO);
    CHECK(!fiber.is_steal_candidate(), "AC2: BlockingIO not candidate");
    CHECK(!fiber.is_stealable(), "AC2: BlockingIO not stealable");

    // fiber_steal_priority: candidate filter (not is_stealable) so deferred
    // inner MB still scores base=0 instead of -1.
    fiber.set_yield_reason(YieldReason::MutationBoundary);
    fiber.publish_mutation_safety_mirrors(1, true, 0);
    CHECK(fiber.is_steal_candidate(), "AC2: deferred MB still candidate");
    CHECK(!fiber.is_stealable(), "AC2: deferred MB not stealable");
    const int pri = fiber_steal_priority(&fiber);
    CHECK(pri == 0, "AC2: deferred MB priority base=0 (not -1)");

    fiber.set_yield_reason(YieldReason::BlockingIO);
    CHECK(fiber_steal_priority(&fiber) == -1, "AC2: non-candidate priority -1");
}

// ── AC3: production call sites never enqueue on reason-class alone ──
static void ac3_production_call_sites() {
    std::println("\n--- #2549 AC3: production steal sites use joint snapshot gate ---");
    const auto wc = read_file("src/serve/worker.cpp");
    const auto wh = read_file("src/serve/worker.h");
    const auto fh = read_file("src/serve/fiber.h");
    CHECK(!wc.empty() && !wh.empty() && !fh.empty(), "AC3: sources readable");

    // try_steal_from enqueue: is_stealable(snap)
    CHECK(wc.find("is_stealable(snap)") != std::string::npos,
          "AC3: worker enqueue uses is_stealable(snap)");
    // Defer path: is_steal_candidate(snap) + !is_at_mutation_boundary_safe(snap)
    CHECK(wc.find("is_steal_candidate(snap)") != std::string::npos,
          "AC3: defer path uses is_steal_candidate(snap)");
    // Must not combine bare is_stealable() with separate safe check (old form)
    CHECK(wc.find("is_stealable() && stolen->is_at_mutation_boundary_safe") == std::string::npos,
          "AC3: no bare is_stealable() && safe dual-check");
    CHECK(wc.find("stolen->is_stealable() &&") == std::string::npos,
          "AC3: no bare stolen->is_stealable() conjunction");

    // Priority uses candidate (preserves base=0 for deferred)
    CHECK(wh.find("is_steal_candidate()") != std::string::npos,
          "AC3: fiber_steal_priority uses is_steal_candidate");
    CHECK(wh.find("!fiber->is_stealable()") == std::string::npos,
          "AC3: fiber_steal_priority no longer gates on is_stealable");

    // API present
    CHECK(fh.find("is_steal_candidate()") != std::string::npos,
          "AC3: is_steal_candidate in fiber.h");
    CHECK(fh.find("is_stealable(const MutationSafetySnapshot") != std::string::npos ||
              fh.find("is_stealable(const MutationSafetySnapshot&") != std::string::npos,
          "AC3: is_stealable(snap) overload");
}

// ── AC4: source-cite + linter wiring ──
static void ac4_source_cite_and_gate() {
    std::println("\n--- #2549 AC4: source-cite + coverage linter ---");
    const auto fh = read_file("src/serve/fiber.h");
    const auto wc = read_file("src/serve/worker.cpp");
    const auto wh = read_file("src/serve/worker.h");
    const auto lint = read_file("scripts/coverage/checks/check_is_stealable_snapshot_gate_2549.py");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");

    CHECK(fh.find("Issue #2549") != std::string::npos, "AC4: fiber.h cites #2549");
    CHECK(wc.find("Issue #2549") != std::string::npos, "AC4: worker.cpp cites #2549");
    CHECK(wh.find("Issue #2549") != std::string::npos, "AC4: worker.h cites #2549");
    CHECK(fh.find("steal safety is defined solely by MutationSafetySnapshot") !=
                  std::string::npos ||
              fh.find("Steal safety is defined solely by MutationSafetySnapshot") !=
                  std::string::npos,
          "AC4: contract documented in fiber.h");
    CHECK(!lint.empty(), "AC4: coverage script present");
    CHECK(lint.find("is_stealable") != std::string::npos, "AC4: linter greps is_stealable");
    CHECK(cmake.find("test_is_stealable_snapshot_gate") != std::string::npos, "AC4: cmake target");
    CHECK(build.find("check_is_stealable_snapshot_gate_2549") != std::string::npos,
          "AC4: build.py gate");
    CHECK(build.find("cmd_is_stealable_snapshot_gate_coverage") != std::string::npos,
          "AC4: build.py cmd");
}

// ── AC5: happy-path cost (no extra atomics beyond safe probe) ──
static void ac5_happy_path_cost() {
    std::println("\n--- #2549 AC5: happy path documents zero extra atomics ---");
    const auto fh = read_file("src/serve/fiber.h");
    // Snap overload reuses one sample; zero-arg path = candidate load + safe sample
    CHECK(fh.find("prefer the snap overload") != std::string::npos ||
              fh.find("Prefer the snap overload") != std::string::npos ||
              fh.find("pre-sampled snapshot") != std::string::npos,
          "AC5: snap overload for zero extra yield load");
    CHECK(fh.find("no extra atomics") != std::string::npos || fh.find("AC5") != std::string::npos,
          "AC5: cost contract cited");

    Fiber fiber(+[] {}, 64 * 1024);
    fiber.set_yield_reason(YieldReason::Explicit);
    // Smoke: joint path returns true without extra setup
    const auto snap = fiber.mutation_safety_snapshot();
    CHECK(fiber.is_stealable(snap), "AC5: Explicit + safe snap → true");
}

} // namespace

static void ac3659_1_boundary_safe_uses_snap() {
    std::println("\n--- #3659 AC1: BoundarySafe uses snap, no parameterless resample ---");
    const auto ss = read_file("src/serve/steal_safety.cpp");
    CHECK(ss.find("is_at_mutation_boundary_safe(snap)") != std::string::npos,
          "3659 AC1: evaluate uses is_at_mutation_boundary_safe(snap)");
    CHECK(ss.find("Issue #3659") != std::string::npos, "3659 AC1: steal_safety cites #3659");
    CHECK(ss.find("is_at_mutation_boundary_safe()") == std::string::npos,
          "3659 AC1: steal_safety.cpp has no parameterless is_at_mutation_boundary_safe()");
}

static void ac3659_2_held_snap_rejects_after_clear() {
    std::println("\n--- #3659 AC2: held snap + later held_mirror==0 → RejectHard, no ticket ---");
    Fiber fiber(+[] {}, 64 * 1024);
    fiber.set_yield_reason(YieldReason::MutationBoundary);
    fiber.publish_mutation_safety_mirrors(/*depth=*/1, /*held=*/true, /*defuse=*/0);
    const auto snap = fiber.mutation_safety_snapshot();
    CHECK(snap.held, "3659 AC2: snap.held==true");
    fiber.publish_mutation_safety_mirrors(/*depth=*/0, /*held=*/false, /*defuse=*/0);
    CHECK(fiber.is_at_mutation_boundary_safe(),
          "3659 AC2: live resample would pass (held_mirror==0)");
    fiber.clear_resume_safety_ticket();
    const auto bits = evaluate_residual_hard_and_bits(&fiber, snap, /*bump_counters=*/false);
    CHECK((bits & steal_invariant_mask(StealInvariant::BoundarySafe)) != 0,
          "3659 AC2: BoundarySafe fail on held snap");
    CHECK(!fiber.has_resume_safety_ticket(), "3659 AC2: evaluate does not stamp ticket");
    fiber.publish_mutation_safety_mirrors(/*depth=*/1, /*held=*/true, /*defuse=*/0);
    fiber.clear_resume_safety_ticket();
    const auto d = steal_safety_transaction(&fiber);
    CHECK(d == StealSafetyDecision::RejectHard, "3659 AC2: transaction RejectHard on held snap");
    CHECK(!fiber.has_resume_safety_ticket(), "3659 AC2: no set_resume_safety_ticket");
}

static void ac3659_3_clean_snap_ok_victim_id() {
    std::println("\n--- #3659 AC3: clean snap Ok + ticket==snap.ticket; victim eval id ---");
    const auto ss = read_file("src/serve/steal_safety.cpp");
    CHECK(ss.find("set_resume_safety_ticket(snap.ticket)") != std::string::npos,
          "3659 AC3: Ok stamps snap.ticket");
    int victim_reads = 0;
    for (std::size_t p = 0;
         (p = ss.find("aura_fiber_evaluator_id_for_steal_safety(stolen)", p)) != std::string::npos;
         p += 1)
        ++victim_reads;
    CHECK(victim_reads >= 3, "3659 AC3: GcDefer/EnvFrame/LCP still victim eval id");
    Fiber fiber(+[] {}, 64 * 1024);
    fiber.set_yield_reason(YieldReason::Explicit);
    fiber.publish_mutation_safety_mirrors(0, false, 0);
    fiber.clear_resume_safety_ticket();
    const auto snap = fiber.mutation_safety_snapshot();
    CHECK(!snap.held, "3659 AC3: clean snap");
    const auto bits = evaluate_residual_hard_and_bits(&fiber, snap, /*bump_counters=*/false);
    CHECK(bits == 0, "3659 AC3: clean snap residual bits==0");
    const auto d = steal_safety_transaction(&fiber);
    CHECK(d == StealSafetyDecision::Ok, "3659 AC3: transaction Ok");
    CHECK(fiber.has_resume_safety_ticket(), "3659 AC3: ticket stamped");
    CHECK(fiber.resume_safety_ticket() == snap.ticket, "3659 AC3: ticket==snap.ticket");
}

static void ac3659_4_soft_unchanged() {
    std::println("\n--- #3659 AC4: Soft/single-worker not forced; Hard still RejectHard ---");
    const auto ss = read_file("src/serve/steal_safety.cpp");
    CHECK(ss.find("Soft: skip entirely (no loads)") != std::string::npos,
          "3659 AC4: Lifetime Soft skip retained");
    CHECK(ss.find("is_steal_snapshot_hard_mode()") != std::string::npos,
          "3659 AC4: Hard production lock still on Lifetime arm");
    Fiber fiber(+[] {}, 64 * 1024);
    fiber.set_yield_reason(YieldReason::MutationBoundary);
    fiber.publish_mutation_safety_mirrors(1, true, 0);
    const auto snap = fiber.mutation_safety_snapshot();
    const auto bits = evaluate_residual_hard_and_bits(&fiber, snap, /*bump_counters=*/false);
    CHECK((bits & steal_invariant_mask(StealInvariant::BoundarySafe)) != 0,
          "3659 AC4: held snap still BoundarySafe fail (not Soft-erased)");
}

static void ac3659_5_linter_no_invent() {
    std::println("\n--- #3659 AC5: linter + no invent / no query-key rewrite ---");
    const auto t = read_file("tests/serve/test_is_stealable_snapshot_gate.cpp");
    const auto inv = read_file("tests/serve/test_steal_snapshot_hard_invariant.cpp");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3659_1_boundary_safe_uses_snap") != std::string::npos, "3659 AC5: AC1");
    CHECK(inv.find("ac3621_1_depth_victim_storage_identity") != std::string::npos,
          "3659 AC5: #3621 TLS enum stays");
    CHECK(build.find("check_boundary_safe_uses_snap_3659") != std::string::npos,
          "3659 AC5: build.py");
    CHECK(read_file("tests/serve/test_issue_3659.cpp").empty(), "3659 AC5: no invent");
    CHECK(read_file("docs/design/3659-boundary-safe-snap.md").empty(), "3659 AC5: no docs/design");
}

int run_test_is_stealable_snapshot_gate() {
    std::println("=== Issue #2549: is_stealable snapshot gate ===");
    ac1_held_or_unsafe_mb_not_stealable();
    ac2_candidate_reasons_depth0_ok();
    ac3_production_call_sites();
    ac4_source_cite_and_gate();
    ac5_happy_path_cost();
    std::println("\n=== Issue #3659: BoundarySafe uses transaction snap ===");
    ac3659_1_boundary_safe_uses_snap();
    ac3659_2_held_snap_rejects_after_clear();
    ac3659_3_clean_snap_ok_victim_id();
    ac3659_4_soft_unchanged();
    ac3659_5_linter_no_invent();
    std::println("\n=== #2549/#3659: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_is_stealable_snapshot_gate();
}
#endif
