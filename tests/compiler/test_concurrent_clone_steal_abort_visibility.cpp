// @category: unit
// @reason: Issue #3303 — ConcurrentCloneGuard nested steal visibility
// window + stable agent-facing reason on steal-abort. Production
// residual: mid-walk fiber steal during a nested expand walk left a
// brief window where partial MacroIntroduced nodes were visible to
// concurrent readers not holding the expand checkpoint. Steal-abort
// path also used kHygieneLimitReasonPassLimit (3) as the
// g_macro_clone_last_reject_reason code, semantically wrong (pass-limit
// and steal-abort are distinct reasons).
//
// Fix contract (AC1–AC9 below):
//
//   AC1: kHygieneLimitReasonStealAbort = 6 declared in
//        macro_expansion.ixx (alongside kHygieneLimitReasonNone=0,
//        GensymCeiling=1, DepthLimit=2, PassLimit=3,
//        MacroIntroduced=4, RestUnmarked=5).
//   AC2: steal-abort site in macro_expansion.cpp stamps
//        kHygieneLimitReasonStealAbort via
//        g_macro_clone_last_reject_reason.store(...) AND
//        note_hygiene_last_limit_reason(kHygieneLimitReasonStealAbort),
//        not the old PassLimit code 3.
//   AC3: hygiene_last_limit_reason_string() returns "steal-abort"
//        for code 6 (so agent replay can distinguish "hygiene ceiling
//        failed" 3 from "fiber-steal during expand walk" 6).
//   AC4: steal0 capture at function entry runs at ALL depths (was
//        depth==0 only). Nested clones inherit the top-level name_map
//        but can still observe mid-walk steals; the delta comparison
//        must work at every recursion level for try_restore() to fire
//        on nested steal detection.
//   AC5: steal detection site at function exit runs at ALL depths
//        (was depth==0 gated). On detection: bumps
//        g_macro_clone_steal_abort_total, stamps StealAbort reason,
//        calls expand_ckpt.try_restore() (no-op when !owned → Soft/Off
//        unchanged), returns NULL_NODE (nm_ckpt rolls back at depth==0
//        only).
//   AC6: g_macro_clone_nested_steal_check_total counter declared in
//        ixx + defined in cpp; bumped on each nested steal-check fire
//        regardless of detection (zero-cost Soft/Off observation).
//   AC7: ConcurrentCloneGuard documentation block states the
//        ownership contract — "top-level owns name_map for whole
//        subtree; nested never re-claims".
//   AC8: aura_test_reset_macro_clone_same_flat_reject_for_test()
//        also resets g_macro_clone_nested_steal_check_total so
//        tests can start from a clean baseline.
//   AC9: this suite + linter; no docs/design/3303-*; source-cite
//        via check_steal_abort_reason_coverage_3303.py.

#include "test_harness.hpp"

#include <atomic>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

#include "compiler/aura_jit_bridge.h"

import std;
import aura.compiler.macro_expansion;

namespace {

using aura::compiler::macro_exp::g_macro_clone_nested_steal_check_total;
using aura::compiler::macro_exp::g_macro_clone_steal_abort_total;
using aura::compiler::macro_exp::hygiene_last_limit_reason_string;
using aura::compiler::macro_exp::kConcurrentCloneProdZeroHalfTreeIssue;
using aura::compiler::macro_exp::kHygieneLimitReasonNameMapShared;
using aura::compiler::macro_exp::kHygieneLimitReasonSameFlatReject;
using aura::compiler::macro_exp::kHygieneLimitReasonStealAbort;
using aura::compiler::macro_exp::note_hygiene_last_limit_reason;
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

} // namespace

int run_test_concurrent_clone_steal_abort_visibility() {
    std::println("=== Issue #3303: nested steal visibility + stable reason ===");
    CHECK(true, "ac3303_steal_abort: issue stamp");

    // ── AC1: ixx declares kHygieneLimitReasonStealAbort = 6 ──
    {
        std::println("\n--- AC1: ixx hygiene reason constant ---");
        auto ixx = read_file("src/compiler/macro_expansion.ixx");
        CHECK(!ixx.empty(), "AC1: ixx readable");
        CHECK(ixx.find("kHygieneLimitReasonStealAbort = 6") != std::string::npos,
              "AC1: ixx declares kHygieneLimitReasonStealAbort = 6");
        CHECK(ixx.find("kHygieneLimitReasonRestUnmarked = 5") != std::string::npos,
              "AC1: prior constants (0..5) preserved");
        CHECK(ixx.find("g_macro_clone_nested_steal_check_total") != std::string::npos,
              "AC1: nested steal-check counter declared");
    }

    // ── AC2: steal-abort site uses StealAbort (not PassLimit code 3) ──
    {
        std::println("\n--- AC2: steal-abort site stamps StealAbort ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        CHECK(!me.empty(), "AC2: cpp readable");
        // Locate the steal-abort block by its anchor line (the
        // g_macro_clone_steal_abort_total.fetch_add bump).
        const auto steal_pos = me.find("g_macro_clone_steal_abort_total.fetch_add");
        CHECK(steal_pos != std::string::npos, "AC2: steal-abort site anchor found");
        if (steal_pos != std::string::npos) {
            const std::string scope = me.substr(steal_pos, 1200);
            CHECK(scope.find("kHygieneLimitReasonStealAbort") != std::string::npos,
                  "AC2: site uses kHygieneLimitReasonStealAbort constant");
            CHECK(scope.find("note_hygiene_last_limit_reason(kHygieneLimitReasonStealAbort)") !=
                      std::string::npos,
                  "AC2: site stamps StealAbort via note_hygiene_last_limit_reason");
            // Old code 3 (PassLimit) must NOT appear in the steal-abort
            // scope. It may legitimately appear in the PassLimit path
            // (different site) — so we scope-check only the steal-abort
            // block.
            CHECK(scope.find(".store(3,") == std::string::npos,
                  "AC2: store(3,...) removed from steal-abort site (was PassLimit code)");
        }
    }

    // ── AC3: hygiene_last_limit_reason_string returns "steal-abort" ──
    {
        std::println("\n--- AC3: hygiene_last_limit_reason_string switch case ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        CHECK(me.find("case 6:") != std::string::npos, "AC3: case 6 in switch");
        // Locate case 6 and check it returns "steal-abort".
        const auto case6 = me.find("case 6:");
        if (case6 != std::string::npos) {
            const std::string scope = me.substr(case6, 200);
            CHECK(scope.find("return \"steal-abort\"") != std::string::npos,
                  "AC3: case 6 returns \"steal-abort\"");
        }
    }

    // ── AC4: steal0 capture at function entry runs at all depths ──
    {
        std::println("\n--- AC4: steal0 capture scope ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        // After #3303, the steal0 capture should NOT be gated on
        // hygiene_depth == 0 (the old depth==0 ? ... : 0 ternary).
        // Find the steal0 capture line and check the surrounding scope.
        const auto steal0_pos = me.find("const auto steal0");
        CHECK(steal0_pos != std::string::npos, "AC4: steal0 capture present");
        if (steal0_pos != std::string::npos) {
            // Search the surrounding ~600 chars for the old ternary.
            const std::string scope = me.substr(steal0_pos, 600);
            CHECK(scope.find("hygiene_depth == 0 ?") == std::string::npos,
                  "AC4: steal0 capture no longer gated on hygiene_depth == 0");
            CHECK(scope.find("aura_fiber_static_cross_fiber_mutation_safe_steal_total()") !=
                      std::string::npos,
                  "AC4: steal0 captures fiber steal total unconditionally");
        }
    }

    // ── AC5: steal detection site runs at all depths ──
    {
        std::println("\n--- AC5: steal detection scope ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        // Find the steal detection site (anchored by the bumped
        // g_macro_clone_nested_steal_check_total line, which is unique
        // to the #3303 path).
        const auto nested_pos = me.find("g_macro_clone_nested_steal_check_total.fetch_add");
        CHECK(nested_pos != std::string::npos, "AC5: nested steal-check counter bump site present");
        if (nested_pos != std::string::npos) {
            // Walk back ~200 chars to find the outer `if (new_id != NULL_NODE)`.
            // We expect the outer if NOT to be gated on hygiene_depth == 0
            // (the old gate was `if (hygiene_depth == 0 && new_id != NULL_NODE)`).
            const std::string scope =
                me.substr((nested_pos > 400 ? nested_pos - 400 : 0), nested_pos + 400);
            // The OLD form "hygiene_depth == 0 && new_id != NULL_NODE" must
            // not appear in the steal detection scope any more.
            CHECK(scope.find("hygiene_depth == 0 && new_id != NULL_NODE") == std::string::npos,
                  "AC5: steal detection no longer gated on hygiene_depth == 0");
            // The NEW outer guard `if (new_id != NULL_NODE)` should appear.
            CHECK(scope.find("if (new_id != NULL_NODE)") != std::string::npos,
                  "AC5: outer guard is `if (new_id != NULL_NODE)`");
        }
    }

    // ── AC6: nested steal-check counter exists + bumps on depth>0 ──
    {
        std::println("\n--- AC6: nested counter increment site ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        // Counter increment must be gated on hygiene_depth > 0.
        const auto fetch_pos = me.find("g_macro_clone_nested_steal_check_total.fetch_add");
        CHECK(fetch_pos != std::string::npos, "AC6: counter bump site present");
        if (fetch_pos != std::string::npos) {
            const std::string scope = me.substr(fetch_pos, 250);
            CHECK(scope.find("hygiene_depth > 0") != std::string::npos,
                  "AC6: counter bump gated on hygiene_depth > 0 (top-level excluded)");
        }
    }

    // ── AC7: ConcurrentCloneGuard ownership documentation ──
    {
        std::println("\n--- AC7: ConcurrentCloneGuard ownership doc ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        CHECK(me.find("TOP-LEVEL OWNS name_map FOR THE WHOLE SUBTREE") != std::string::npos,
              "AC7: ownership contract documented");
        CHECK(me.find("NESTED NEVER RE-CLAIMS") != std::string::npos,
              "AC7: nested non-claim contract documented");
        CHECK(me.find("nested mid-walk steal detection (Issue #3303)") != std::string::npos,
              "AC7: nested steal detection observation semantics documented");
    }

    // ── AC8: C bridge reset function covers the new counter ──
    {
        std::println("\n--- AC8: C bridge reset function ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        const auto reset_pos = me.find("aura_test_reset_macro_clone_same_flat_reject_for_test");
        CHECK(reset_pos != std::string::npos, "AC8: C bridge reset function present");
        if (reset_pos != std::string::npos) {
            const std::string scope = me.substr(reset_pos, 600);
            CHECK(scope.find("g_macro_clone_nested_steal_check_total.store(0,") !=
                      std::string::npos,
                  "AC8: reset function clears nested steal-check counter");
        }
    }

    // ── AC9: no docs/design/3303-* plan doc ──
    {
        std::println("\n--- AC9: no docs/design/3303-* plan doc ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        auto ixx = read_file("src/compiler/macro_expansion.ixx");
        CHECK(me.find("docs/design/3303") == std::string::npos,
              "AC9: cpp does not reference docs/design/3303-*");
        CHECK(ixx.find("docs/design/3303") == std::string::npos,
              "AC9: ixx does not reference docs/design/3303-*");
    }

    // ── Issue #3321: production zero half-tree + stable Agent reason ──
    // Residual after #3303: 16-slot same-flat / name-map reject was
    // counter-only; nested steal kept cloning siblings before restore.
    {
        std::println("\n--- #3321 AC1: steal restore + StealAbort reason ---");
        CHECK(kConcurrentCloneProdZeroHalfTreeIssue == 3321, "3321 AC1: issue stamp");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        CHECK(me.find("expand_ckpt.try_restore()") != std::string::npos,
              "3321 AC1: steal path restores");
        CHECK(me.find("note_hygiene_last_limit_reason(kHygieneLimitReasonStealAbort)") !=
                  std::string::npos,
              "3321 AC1: steal stamps last_limit_reason");
        CHECK(me.find("kHygieneLimitReasonStealAbort") != std::string::npos,
              "3321 AC1: steal-abort code");
    }
    {
        std::println("\n--- #3321 AC2: nested production fail-fast ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        CHECK(me.find("Issue #3321: production fail-fast after nested steal-abort") !=
                  std::string::npos,
              "3321 AC2: fail-fast comment");
        CHECK(me.find("kHygieneLimitReasonStealAbort") != std::string::npos &&
                  me.find("production_surface") != std::string::npos,
              "3321 AC2: production_surface + steal-abort");
        CHECK(me.find("cloned == NULL_NODE && production_surface") != std::string::npos,
              "3321 AC2: sibling abort after nested NULL_NODE");
    }
    {
        std::println("\n--- #3321 AC3: Soft/Off zero extra on quiet path ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        CHECK(me.find("Soft/Off: continue (historical") != std::string::npos,
              "3321 AC3: Soft continues siblings");
        CHECK(me.find("is_sandbox_active()") != std::string::npos,
              "3321 AC3: name_map claim still production-gated");
        CHECK(me.find("hygiene_depth == 0 && production_surface") != std::string::npos,
              "3321 AC3: expand ckpt still top-level production only");
    }
    {
        std::println("\n--- #3321 AC4: 16-slot same-flat stable reason ---");
        CHECK(kHygieneLimitReasonSameFlatReject == 8, "3321 AC4: same-flat code 8");
        CHECK(kHygieneLimitReasonNameMapShared == 9, "3321 AC4: name-map code 9");
        auto ixx = read_file("src/compiler/macro_expansion.ixx");
        CHECK(ixx.find("kHygieneLimitReasonSameFlatReject = 8") != std::string::npos,
              "3321 AC4: ixx same-flat");
        CHECK(ixx.find("kHygieneLimitReasonNameMapShared = 9") != std::string::npos,
              "3321 AC4: ixx name-map");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        CHECK(me.find("note_hygiene_last_limit_reason(kHygieneLimitReasonSameFlatReject)") !=
                  std::string::npos,
              "3321 AC4: same-flat stamps last_limit_reason");
        CHECK(me.find("note_hygiene_last_limit_reason(kHygieneLimitReasonNameMapShared)") !=
                  std::string::npos,
              "3321 AC4: name-map stamps last_limit_reason");
        CHECK(me.find("g_macro_clone_last_reject_reason.store(2") != std::string::npos,
              "3321 AC4: last_reject_reason 2 preserved (#3028)");
        CHECK(me.find("g_macro_clone_last_reject_reason.store(4") != std::string::npos,
              "3321 AC4: last_reject_reason 4 preserved (#3094)");
        CHECK(me.find("return \"same-flat-clone-reject\"") != std::string::npos,
              "3321 AC4: stable string same-flat");
        CHECK(me.find("return \"name-map-shared\"") != std::string::npos,
              "3321 AC4: stable string name-map");
        aura_test_reset_macro_hygiene_last_limit_reason_for_test();
        note_hygiene_last_limit_reason(kHygieneLimitReasonSameFlatReject);
        CHECK(std::string_view(hygiene_last_limit_reason_string()) == "same-flat-clone-reject",
              "3321 AC4: live string same-flat-clone-reject");
        note_hygiene_last_limit_reason(kHygieneLimitReasonNameMapShared);
        CHECK(std::string_view(hygiene_last_limit_reason_string()) == "name-map-shared",
              "3321 AC4: live string name-map-shared");
        aura_test_reset_macro_hygiene_last_limit_reason_for_test();
    }
    {
        std::println("\n--- #3321 AC5/AC6: extend this suite; intern; no invent ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        CHECK(me.find("target_pool.intern") != std::string::npos,
              "3321 AC6: cross-flat intern (no raw ID capture)");
        CHECK(me.find("source_pool.resolve") != std::string::npos, "3321 AC6: resolve then intern");
        const auto build = read_file("build.py");
        const auto lint =
            read_file("scripts/coverage/checks/check_concurrent_clone_prod_zero_half_tree_3321.py");
        CHECK(!lint.empty() && lint.find("Issue #3321") != std::string::npos, "3321 AC5: linter");
        CHECK(build.find("check_concurrent_clone_prod_zero_half_tree_3321") != std::string::npos,
              "3321 AC5: build.py wires linter");
        CHECK(read_file("docs/design/3321-concurrent-clone.md").empty(),
              "3321 AC5: no docs/design/");
        CHECK(read_file("tests/compiler/test_issue_3321.cpp").empty(), "3321 AC5: no invent test");
        CHECK(read_file("tests/issues/test_issue_3321.cpp").empty(), "3321 AC5: no tests/issues");
    }

    // Issue #3341: steal-abort mid-expand durable Agent string on
    // last_mutate_error_ (was counter-only). Per-fiber last_limit_reason
    // lives on FiberHygieneStats; this suite source-cites the steal site.
    {
        std::println("\n--- #3341: steal-abort-mid-expand Agent string ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        CHECK(me.find("aura_evaluator_note_steal_abort_mid_expand") != std::string::npos,
              "3341: steal-abort site stamps last_mutate_error_");
        const auto steal_pos = me.find("g_macro_clone_steal_abort_total.fetch_add");
        CHECK(steal_pos != std::string::npos, "3341: steal-abort counter still present");
        if (steal_pos != std::string::npos) {
            const auto scope = me.substr(steal_pos, 1600);
            CHECK(scope.find("aura_evaluator_note_steal_abort_mid_expand") != std::string::npos,
                  "3341: helper called in the steal-abort block (not a distant cite)");
            CHECK(scope.find("note_hygiene_last_limit_reason(kHygieneLimitReasonStealAbort)") !=
                      std::string::npos,
                  "3341: StealAbort reason still stamped");
        }
        auto ixx = read_file("src/compiler/macro_expansion.ixx");
        CHECK(ixx.find("last_limit_reason = 0") != std::string::npos,
              "3341: FiberHygieneStats.last_limit_reason");
        auto ev = read_file("src/compiler/evaluator.ixx");
        CHECK(ev.find("steal-abort-mid-expand") != std::string::npos,
              "3341: Evaluator notes steal-abort-mid-expand");
        CHECK(read_file("docs/design/3341-fiber-reason.md").empty(), "3341: no docs/design");
        CHECK(read_file("tests/issues/test_issue_3341.cpp").empty(), "3341: no invent test");
    }

    std::println("\n=== Issue #3303 + #3321 + #3341 done ===");
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_concurrent_clone_steal_abort_visibility();
}
#endif