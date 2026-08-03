// tests/core/test_capability_single_use_consume_2586.cpp
// @category: unit
// @reason: Issue #2586 — single-use / mutation-bound grant (auto-revoke
//          after first successful check_and_record_effect that uses the
//          grant's bits; deny path does NOT consume).
//
//   AC1: grant_once(Mutate) → 1st allow; 2nd deny (no other grant)
//   AC2: 1st deny (other reasons, e.g. wrong effect bits) → grant still
//        valid, retryable
//   AC3: non single_use grant behavior unchanged (multiple allows OK)
//   AC4: Soft/Off mode usable; production default API (grant() with
//        single_use=false) does not force auto-revoke
//   AC5: audit / SE dual-write reason "single-use-consumed" visible in
//        SecurityEvent ring + capability_single_use_consumed metric
//   AC6: tests + source-cite (no docs/design/)
//
// Source-cite:
//   src/core/capability_model.hh — CapabilityGrant.single_use field
//     (line ~133), CapabilityEffectMetrics.capability_single_use_consumed_total
//     counter (~196), grant() extended signature (~405), grant_once sugar (~449),
//     check_and_record_effect consume block (~842), CapabilityEffectStatsSnapshot
//     .capability_single_use_consumed (~975), reset + snapshot fields.
//   src/compiler/evaluator_security.cpp — grant_effect_capability single_use
//     parameter forwarding to registry::grant.
//   src/compiler/evaluator.ixx — grant_effect_capability declaration
//     with `bool single_use = false` default parameter.
//   src/compiler/evaluator_primitives_security.cpp — query:capability-effect-stats
//     surfaces capability-single-use-consumed-total / schema-2586 / issue-2586.

#include "test_harness.hpp"

#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"

#include <cstdint>
#include <cstring>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

using aura::core::capability::check_and_record_effect;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::snapshot_capability_effect_stats;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::kSecurityEventRingSize;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::security_event::SecurityEvent;
using aura::test::g_failed;
using aura::test::g_passed;

// Walk back through the SE ring looking for an event whose reason matches
// `needle`. Bounded lookback so wrap storms don't scan forever. Returns the
// matched slot if found, nullptr otherwise.
const SecurityEvent* ring_lookup_reason(std::string_view needle, std::uint64_t lookback = 16) {
    const auto& ring = g_security_event_ring();
    const auto cur = ring.seq.load(std::memory_order_acquire);
    if (cur == 0)
        return nullptr;
    const auto start = cur;
    const auto end = (cur > lookback) ? cur - lookback : std::uint64_t{1};
    for (auto s = start; s >= end && s > 0; --s) {
        const auto idx = (s - 1) % kSecurityEventRingSize;
        const auto& e = ring.ring[idx];
        const auto rlen = std::strlen(e.reason);
        if (rlen == needle.size() && std::string_view(e.reason, rlen) == needle) {
            return &e;
        }
    }
    return nullptr;
}

void reset_all() {
    reset_capability_effects_for_test();
    reset_security_event_ring_for_test();
    set_mode(SandboxMode::Off);
}

} // namespace

int main() {
    std::println("=== Issue #2586: single-use grant auto-revoke ===");

    // ── AC1: grant_once(Mutate) → 1st allow; 2nd deny ──────────────
    {
        std::println("\n--- #2586 AC1: single-use auto-revoke on 2nd check ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;

        EffectProvenance prov{};
        prov.epoch = 1;
        prov.mutation_id = 1;
        g_capability_registry().grant_once(/*tenant=*/1, "mut-2586-once", Effect::Mutate, prov);

        const bool ok1 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 1, "mut-once-1");
        CHECK(ok1, "AC1: 1st check_and_record_effect(Mutate) → allow");

        const bool ok2 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 1, "mut-once-2");
        CHECK(!ok2, "AC1: 2nd check_and_record_effect(Mutate) → deny (single_use consumed)");

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 1,
              "AC1: capability_single_use_consumed == 1 after 1st allow");
        CHECK(snap.revokes == 1, "AC1: revokes counter == 1 (single_use path bumps revoke_total)");
        CHECK(snap.enforced == 1, "AC1: enforced counter == 1");
        CHECK(snap.denied == 1, "AC1: denied counter == 1");
        CHECK(snap.audits >= 2, "AC1: audits counter >= 2 (effect + revoke audit)");
    }

    // ── AC2: deny path does NOT consume (retryable) ────────────────
    {
        std::println("\n--- #2586 AC2: deny does not consume (retryable) ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;

        EffectProvenance prov{};
        prov.epoch = 2;
        prov.mutation_id = 2;
        g_capability_registry().grant_once(/*tenant=*/2, "mut-2586-retry", Effect::Mutate, prov);

        // 1st: deny — ask for Write bit but grant only has Mutate.
        const bool ok1 =
            check_and_record_effect(Effect::Write, Effect::Write, prov, 2, "write-deny");
        CHECK(!ok1, "AC2: 1st check_and_record_effect(Write) → deny (no Write bit in grant)");

        const auto snap0 = snapshot_capability_effect_stats();
        CHECK(snap0.capability_single_use_consumed == 0,
              "AC2: single_use_consumed == 0 after deny (deny path does NOT consume)");

        // Retry: Mutate bit still works because deny did not consume.
        const bool ok2 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 2, "mut-retry-1");
        CHECK(ok2, "AC2: retry check_and_record_effect(Mutate) → allow (deny did not consume)");

        // Post-retry, 2nd Mutate check should deny because single_use now consumed.
        const bool ok3 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 2, "mut-retry-2");
        CHECK(!ok3, "AC2: post-retry 2nd Mutate check → deny (now consumed)");

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 1,
              "AC2: single_use_consumed == 1 (only the successful Mutate check consumed)");
        CHECK(snap.denied == 2, "AC2: denied counter == 2 (Write deny + post-retry Mutate deny)");
        CHECK(snap.enforced == 1, "AC2: enforced counter == 1 (the successful Mutate retry)");
    }

    // ── AC3: non single_use grant behavior unchanged ──────────────
    {
        std::println("\n--- #2586 AC3: non single_use grant unchanged ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;

        EffectProvenance prov{};
        prov.epoch = 3;
        prov.mutation_id = 3;
        // Default grant() — single_use=false (legacy behavior).
        g_capability_registry().grant(/*tenant=*/3, "mut-2586-perm", Effect::Mutate, prov);

        for (int i = 0; i < 5; ++i) {
            const std::string op = "mut-perm-" + std::to_string(i);
            const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 3, op);
            CHECK(ok, "AC3: non single_use grant 5x allow");
        }

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 0,
              "AC3: single_use_consumed == 0 for non single_use grant");
        CHECK(snap.revokes == 0, "AC3: revokes == 0 for non single_use grant");
        CHECK(snap.enforced == 5, "AC3: enforced == 5 (all 5 allowed)");
    }

    // ── AC4: Soft/Off usable; production default API not forced ────
    {
        std::println("\n--- #2586 AC4: Off mode + production default API ---");
        // (4a) Off mode: single_use grant is "usable" — counter tracks
        // consumption on every successful allow, but Off-mode always-allow
        // semantics are unchanged (grant not required for allow). 2nd call
        // still returns true in Off mode; the consumption is observable via
        // capability_single_use_consumed.
        reset_all();
        set_mode(SandboxMode::Off);
        g_capability_registry().sandbox_mode = EffectSandboxMode::Off;
        EffectProvenance prov{};
        prov.epoch = 4;
        prov.mutation_id = 4;
        g_capability_registry().grant_once(/*tenant=*/4, "mut-2586-off", Effect::Mutate, prov);

        const bool off_ok1 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 4, "off-mut-1");
        CHECK(off_ok1, "AC4a: Off mode 1st allow");
        const bool off_ok2 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 4, "off-mut-2");
        CHECK(off_ok2, "AC4a: Off mode 2nd STILL allow (Off-mode semantics — grant not required; "
                       "single_use consumption tracked via counter, not via allow decision)");

        const auto snap0 = snapshot_capability_effect_stats();
        CHECK(snap0.capability_single_use_consumed == 1,
              "AC4a: Off mode single_use_consumed == 1 (consumption tracked even when grant "
              "not required for allow)");

        // (4b) Production default API: grant_effect_capability / grant()
        // without single_use param → no auto-revoke.
        reset_all();
        set_mode(SandboxMode::Strict);
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;

        EffectProvenance prov2{};
        prov2.epoch = 5;
        prov2.mutation_id = 5;
        g_capability_registry().grant(/*tenant=*/5, "mut-2586-default", Effect::Mutate, prov2);

        for (int i = 0; i < 3; ++i) {
            const std::string op = "default-" + std::to_string(i);
            const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov2, 5, op);
            CHECK(ok, "AC4b: default API 3x allow (no single_use forced)");
        }

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 0,
              "AC4b: default API single_use_consumed == 0");
        CHECK(snap.revokes == 0, "AC4b: default API revokes == 0");
    }

    // ── AC5: audit / SE visible revoke with reason 'single-use-consumed' ──
    {
        std::println("\n--- #2586 AC5: audit/SE reason 'single-use-consumed' ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;

        EffectProvenance prov{};
        prov.epoch = 6;
        prov.mutation_id = 6;
        g_capability_registry().grant_once(/*tenant=*/6, "mut-2586-audit", Effect::Mutate, prov);

        const auto ring_seq_before = g_security_event_ring().seq.load(std::memory_order_acquire);
        const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 6, "audit-1");
        CHECK(ok, "AC5: single_use grant 1st allow");

        const auto ring_seq_after = g_security_event_ring().seq.load(std::memory_order_acquire);
        CHECK(ring_seq_after > ring_seq_before,
              "AC5: SE ring advanced (single_use audit appended)");

        // Look for an event with reason "single-use-consumed" — emitted by
        // record_audit() inside the consume block.
        const auto* found = ring_lookup_reason("single-use-consumed");
        CHECK(found != nullptr, "AC5: SE ring has event with reason 'single-use-consumed'");
        if (found) {
            CHECK(!found->denied, "AC5: single-use-consumed event is denied=false (allow path)");
            CHECK(found->effect_bits == static_cast<std::uint16_t>(Effect::Mutate),
                  "AC5: event effect_bits == Mutate");
        }

        // Metric surface also reflects the consumption (counters bump atomic).
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 1, "AC5: capability_single_use_consumed == 1");
        CHECK(snap.revokes == 1, "AC5: revokes == 1 (single_use path bumps revoke_total)");
    }

    // ── AC6: tests + source-cite (no docs/design/) ─────────────────
    {
        std::println("\n--- #2586 AC6: source-cite ---");
        std::println("AC6 — see file header for source-cite list:");
        std::println("  - src/core/capability_model.hh: CapabilityGrant.single_use, "
                     "grant() extended signature, grant_once sugar, check_and_record_effect "
                     "consume block, CapabilityEffectMetrics + Snapshot.");
        std::println("  - src/compiler/evaluator_security.cpp: grant_effect_capability "
                     "single_use parameter.");
        std::println("  - src/compiler/evaluator.ixx: grant_effect_capability declaration.");
        std::println("  - src/compiler/evaluator_primitives_security.cpp: "
                     "query:capability-effect-stats capability-single-use-consumed-total "
                     "/ schema-2586 / issue-2586.");
        std::println("  - tests/core/test_capability_single_use_consume_2586.cpp (this file).");
        std::println("  - no docs/design/ (per #1655 #1485 ship philosophy).");
        CHECK(true, "AC6: source-cite listed above (no docs/design)");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}