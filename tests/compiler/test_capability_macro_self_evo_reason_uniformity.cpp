// @category: unit
// @reason: Issue #3304 — unify Agent-stable reject reasons + harden
// lightweight/fine_rollback MacroIntroduced restore. Production residual
// from Macro + Hygiene + Self-Evo review: (1) not every deny path stamps
// kHygieneLimitReason* + hygiene_last_limit_reason_string(); some only
// bump counters. MacroSelfEvo capability deny uses free-form text —
// weaker for Agent replay tooling than structured codes. (3) lightweight
// / fine_rollback=false abort may under-restore marker / provenance /
// macro_dirty in edge cases; post-restore invariant exists but is not
// universally forced on every lightweight path.
//
// Fix contract (AC1–AC8):
//
//   AC1: macro_expansion.ixx declares kHygieneLimitReasonCapabilityDeny
//        = 7 (sentinel — pairs with kCapabilityDenyReason* family)
//   AC2: macro_expansion.cpp hygiene_last_limit_reason_string() handles
//        case 7 → "capability-deny"
//   AC3: macro_expansion.cpp refactored 3 direct store(2,...) and
//        store(3,...) sites to use the public
//        note_hygiene_last_limit_reason(code) API
//        (line ~1582 DepthLimit, line ~2574 + ~2958 PassLimit)
//   AC4: capability_model.hh declares kCapabilityDenyReason* family
//        (4 codes: NotGranted=1, ProvenanceFence=2, PolicyMissing=3,
//        LimitsZero=4) + g_capability_deny_last_reason atomic +
//        note_capability_deny_last_reason() + capability_deny_last_
//        reason_string() accessor
//   AC5: capability_model.hh check_macro_self_evo calls
//        note_capability_deny_last_reason(kCapabilityDenyReason*)
//        at all 4 deny sites (NotGranted, ProvenanceFence,
//        PolicyMissing, LimitsZero)
//   AC6: every direct store(2,...) and store(3,...) site in
//        macro_expansion.cpp is GONE — replaced with
//        note_hygiene_last_limit_reason(kHygieneLimitReasonDepthLimit)
//        / note_hygiene_last_limit_reason(kHygieneLimitReasonPassLimit)
//   AC7: check_macro_hygiene_invariant_post_restore is called on every
//        abort_restore_dual_topology site (already present at lines
//        746, 1545, 1717 + gen-drift/restore-metadata 5217, 5235);
//        Soft/Off remains zero-cost
//   AC8: this suite + linter; no docs/design/3304-*; source-cite
//        via check_capability_deny_reason_uniformity_3304.py

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

using aura::compiler::macro_exp::g_macro_hygiene_last_limit_reason;
using aura::compiler::macro_exp::kHygieneLimitReasonCapabilityDeny;
using aura::compiler::macro_exp::kHygieneLimitReasonDepthLimit;
using aura::compiler::macro_exp::kHygieneLimitReasonPassLimit;
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

int run_test_capability_macro_self_evo_reason_uniformity() {
    std::println("=== Issue #3304: capability deny reason uniformity ===");
    CHECK(true, "ac3304: issue stamp");

    auto ixx = read_file("src/compiler/macro_expansion.ixx");
    auto me = read_file("src/compiler/macro_expansion.cpp");
    auto cap = read_file("src/core/capability_model.hh");
    auto boundary = read_file("src/compiler/evaluator_mutation_boundary.cpp");

    // ── AC1: ixx declares kHygieneLimitReasonCapabilityDeny = 7 ──
    {
        std::println("\n--- AC1: ixx capability-deny sentinel ---");
        CHECK(!ixx.empty(), "AC1: ixx readable");
        CHECK(ixx.find("kHygieneLimitReasonCapabilityDeny = 7") != std::string::npos,
              "AC1: ixx declares kHygieneLimitReasonCapabilityDeny = 7");
        CHECK(ixx.find("capability-deny sentinel") != std::string::npos,
              "AC1: sentinel purpose documented in ixx");
    }

    // ── AC2: case 7 → "capability-deny" in switch ──
    {
        std::println("\n--- AC2: hygiene_last_limit_reason_string case 7 ---");
        CHECK(!me.empty(), "AC2: cpp readable");
        const auto case7 = me.find("case 7:");
        if (case7 != std::string::npos) {
            const std::string scope = me.substr(case7, 200);
            CHECK(scope.find("return \"capability-deny\"") != std::string::npos,
                  "AC2: case 7 returns \"capability-deny\"");
        } else {
            CHECK(false, "AC2: case 7 missing");
        }
    }

    // ── AC3: 3 direct store sites refactored to note_hygiene_last_limit_reason ──
    {
        std::println("\n--- AC3: refactored direct-store sites ---");
        // Count remaining direct store(2,...) — should be 0 (all DepthLimit
        // sites refactored to note_hygiene_last_limit_reason).
        const auto depth_limit_stores = [&]() -> std::size_t {
            std::size_t count = 0;
            const auto target = "g_macro_hygiene_last_limit_reason.store(2,";
            std::size_t pos = 0;
            while ((pos = me.find(target, pos)) != std::string::npos) {
                ++count;
                pos += std::strlen(target);
            }
            return count;
        }();
        CHECK(depth_limit_stores == 0, "AC3: g_macro_hygiene_last_limit_reason.store(2,...) sites "
                                       "removed (DepthLimit refactored to note API)");

        const auto pass_limit_stores = [&]() -> std::size_t {
            std::size_t count = 0;
            const auto target = "g_macro_hygiene_last_limit_reason.store(3,";
            std::size_t pos = 0;
            while ((pos = me.find(target, pos)) != std::string::npos) {
                ++count;
                pos += std::strlen(target);
            }
            return count;
        }();
        CHECK(pass_limit_stores == 0, "AC3: g_macro_hygiene_last_limit_reason.store(3,...) sites "
                                      "removed (PassLimit refactored to note API)");

        // Verify the public API is now used for DepthLimit / PassLimit.
        CHECK(me.find("note_hygiene_last_limit_reason(kHygieneLimitReasonDepthLimit)") !=
                  std::string::npos,
              "AC3: DepthLimit now uses note_hygiene_last_limit_reason");
        CHECK(me.find("note_hygiene_last_limit_reason(kHygieneLimitReasonPassLimit)") !=
                  std::string::npos,
              "AC3: PassLimit now uses note_hygiene_last_limit_reason");
    }

    // ── AC4: kCapabilityDenyReason* family + accessors ──
    {
        std::println("\n--- AC4: capability reason family + accessors ---");
        CHECK(!cap.empty(), "AC4: capability_model.hh readable");
        CHECK(cap.find("kCapabilityDenyReasonNone = 0") != std::string::npos,
              "AC4: kCapabilityDenyReasonNone declared");
        CHECK(cap.find("kCapabilityDenyReasonNotGranted = 1") != std::string::npos,
              "AC4: kCapabilityDenyReasonNotGranted declared");
        CHECK(cap.find("kCapabilityDenyReasonProvenanceFence = 2") != std::string::npos,
              "AC4: kCapabilityDenyReasonProvenanceFence declared");
        CHECK(cap.find("kCapabilityDenyReasonPolicyMissing = 3") != std::string::npos,
              "AC4: kCapabilityDenyReasonPolicyMissing declared");
        CHECK(cap.find("kCapabilityDenyReasonLimitsZero") != std::string::npos,
              "AC4: kCapabilityDenyReasonLimitsZero declared");
        CHECK(cap.find("g_capability_deny_last_reason") != std::string::npos,
              "AC4: g_capability_deny_last_reason atomic declared");
        CHECK(cap.find("note_capability_deny_last_reason") != std::string::npos,
              "AC4: note_capability_deny_last_reason declared");
        CHECK(cap.find("capability_deny_last_reason_string") != std::string::npos,
              "AC4: capability_deny_last_reason_string declared");
        CHECK(cap.find("\"capability-not-granted\"") != std::string::npos,
              "AC4: case 1 returns \"capability-not-granted\"");
        CHECK(cap.find("\"capability-provenance-fence\"") != std::string::npos,
              "AC4: case 2 returns \"capability-provenance-fence\"");
        CHECK(cap.find("\"capability-policy-missing\"") != std::string::npos,
              "AC4: case 3 returns \"capability-policy-missing\"");
        CHECK(cap.find("\"capability-limits-zero\"") != std::string::npos,
              "AC4: case 4 returns \"capability-limits-zero\"");
    }

    // ── AC5: 4 deny sites stamp new code ──
    {
        std::println("\n--- AC5: 4 MacroSelfEvo deny sites stamp new code ---");
        CHECK(cap.find("note_capability_deny_last_reason(kCapabilityDenyReasonNotGranted)") !=
                  std::string::npos,
              "AC5: not-granted deny site stamps NotGranted");
        CHECK(cap.find("note_capability_deny_last_reason(kCapabilityDenyReasonProvenanceFence)") !=
                  std::string::npos,
              "AC5: provenance-fence deny site stamps ProvenanceFence");
        CHECK(cap.find("note_capability_deny_last_reason(kCapabilityDenyReasonPolicyMissing)") !=
                  std::string::npos,
              "AC5: policy-missing deny site stamps PolicyMissing");
        CHECK(cap.find("note_capability_deny_last_reason(kCapabilityDenyReasonLimitsZero)") !=
                  std::string::npos,
              "AC5: limits-zero deny site stamps LimitsZero");
        // note_capability_deny_last_reason must also stamp the unified
        // kHygieneLimitReasonCapabilityDeny=7 sentinel (so the agent's
        // existing last_limit_reason_string() returns "capability-deny").
        CHECK(cap.find("aura_macro_hygiene_capability_deny_sentinel()") != std::string::npos,
              "AC5: unified hygiene atomic stamped with sentinel 7");
    }

    // ── AC6: no remaining direct store(2,...) or store(3,...) ──
    {
        std::println("\n--- AC6: no remaining direct store(2,...) or store(3,...) ---");
        // Already covered by AC3 — re-verifying for clarity.
        const auto total_remaining = [&]() -> std::size_t {
            std::size_t count = 0;
            for (const char* target : {"g_macro_hygiene_last_limit_reason.store(2,",
                                       "g_macro_hygiene_last_limit_reason.store(3,"}) {
                std::size_t pos = 0;
                while ((pos = me.find(target, pos)) != std::string::npos) {
                    ++count;
                    pos += std::strlen(target);
                }
            }
            return count;
        }();
        CHECK(total_remaining == 0,
              "AC6: 0 direct store(2,...) / store(3,...) sites remain (all refactored)");
    }

    // ── AC7: post-restore invariant called on every abort site ──
    {
        std::println(
            "\n--- AC7: check_macro_hygiene_invariant_post_restore on every abort site ---");
        CHECK(!boundary.empty(), "AC7: evaluator_mutation_boundary.cpp readable");
        // We expect at least 5 calls to the invariant (3 dual-restore + 2 metadata).
        const auto invariant_count = [&]() -> std::size_t {
            std::size_t count = 0;
            const auto target = "check_macro_hygiene_invariant_post_restore";
            std::size_t pos = 0;
            while ((pos = boundary.find(target, pos)) != std::string::npos) {
                ++count;
                pos += std::strlen(target);
            }
            return count;
        }();
        CHECK(invariant_count >= 5,
              "AC7: post-restore invariant called on every abort site (>=5 sites)");
        const auto dual_restore_count = [&]() -> std::size_t {
            std::size_t count = 0;
            const auto target = "abort_restore_dual_topology";
            std::size_t pos = 0;
            while ((pos = boundary.find(target, pos)) != std::string::npos) {
                ++count;
                pos += std::strlen(target);
            }
            return count;
        }();
        CHECK(dual_restore_count >= 3, "AC7: dual_topology abort_restore sites present (>=3)");
    }

    // ── AC8: no docs/design/3304-* plan doc ──
    {
        std::println("\n--- AC8: no docs/design/3304-* plan doc ---");
        CHECK(me.find("docs/design/3304") == std::string::npos,
              "AC8: cpp does not reference docs/design/3304-*");
        CHECK(ixx.find("docs/design/3304") == std::string::npos,
              "AC8: ixx does not reference docs/design/3304-*");
        CHECK(cap.find("docs/design/3304") == std::string::npos,
              "AC8: capability_model.hh does not reference docs/design/3304-*");
    }

    std::println("\n=== Issue #3304 done ===");
    return g_failed == 0 ? 0 : 1;
}

// Issue #3459 residual cleanup: this binary shipped without a main —
// every libaura_test_objects change re-broke its link (undefined
// reference to `main`). Wire the #3304 runner into a real entry point.
int main() {
    return run_test_capability_macro_self_evo_reason_uniformity();
}