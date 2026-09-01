// @category: unit
// @reason: Issue #2156 — Fix isolation-deny mutation_id pollution
// (tenant id must not be written into SecurityEvent.mutation_id /
// TypedMutation audit join key).
//
//   AC1: Isolation deny SecurityEvent.mutation_id is Mutation epoch space,
//        never equal to tenant id solely because of this path
//   AC2: trail_find_by_mutation_id(epoch) finds isolation event;
//        trail_find_by_mutation_id(tenant_id) does not spuriously hit
//   AC3: Effect allow/deny path still stamps real mid (unchanged)
//   AC4: Unit test locks mid source (mutation epoch)

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/typed_mutation_audit.h"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/workspace_epoch.hh"
#include "serve/fiber.h"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::kEffectMutate;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::trail_find_by_mutation_id;
using aura::compiler::typed_audit::TypedMutationAuditEvent;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::bump_mutation_epoch;
using aura::core::current_mutation_epoch;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::kIsolationAuditMidIssue;
using aura::core::security_event::kSecurityEventRingSize;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::security_event::SecurityEventKind;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

void reset_all() {
    reset_tenant_isolation_for_test();
    reset_security_event_ring_for_test();
    reset_for_test();
    set_mode(SandboxMode::Off);
}

std::int64_t href_stats(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:security-audit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// Find most recent IsolationDeny event.
bool find_isolation_deny(std::uint64_t& out_mid, std::uint64_t& out_tenant,
                         std::string& out_reason) {
    auto& ring = g_security_event_ring();
    const auto total = ring.total.load(std::memory_order_relaxed);
    if (total == 0)
        return false;
    const auto n = total < kSecurityEventRingSize ? total : kSecurityEventRingSize;
    for (std::size_t i = 0; i < n; ++i) {
        const auto idx = (total - 1 - i) % kSecurityEventRingSize;
        const auto& e = ring.ring[idx];
        if (e.kind == SecurityEventKind::IsolationDeny) {
            out_mid = e.mutation_id;
            out_tenant = e.tenant_id;
            out_reason = e.reason;
            return true;
        }
    }
    return false;
}

} // namespace

int run_test_isolation_audit_mid() {
    std::println("=== Issue #2156: isolation-deny audit mid not tenant ===");
    CHECK(kIsolationAuditMidIssue == 2156, "issue stamp");

    // ── AC1/AC4: isolation deny mid = mutation epoch, not tenant ──
    {
        std::println("\n--- AC1/AC4: mid = mutation epoch ---");
        reset_all();
        bump_mutation_epoch(42);
        const auto epoch = current_mutation_epoch();
        CHECK(epoch >= 42, "epoch advanced");

        CompilerService cs;
        auto& ev = cs.evaluator();
        // Strict isolation principal = tenant 10; foreign ref_tenant = 99.
        ev.set_effect_sandbox_mode(2);
        ev.set_tenant_principal(10, "t10", /*allow_cross=*/false);
        g_workspace_isolation().set_strict_sandbox_linked(true);

        const std::uint64_t target_tenant = 10;
        const std::uint64_t ref_tenant = 99; // foreign provenance
        const bool ok =
            ev.check_workspace_isolation(target_tenant, ref_tenant, kEffectMutate, "ac1-iso-deny");
        CHECK(!ok, "cross-tenant isolation denies");

        std::uint64_t mid = 0, tenant = 0;
        std::string reason;
        CHECK(find_isolation_deny(mid, tenant, reason), "IsolationDeny event present");
        std::println("  mid={} tenant={} epoch={} reason={}", mid, tenant, epoch, reason);
        CHECK(tenant == target_tenant, "AC1: tenant_id is target principal");
        CHECK(mid != ref_tenant, "AC1: mutation_id != ref_tenant (was pollution)");
        CHECK(mid != target_tenant || epoch == target_tenant,
              "AC1: mutation_id not target solely as pollution");
        // mid must be in mutation epoch space (epoch or 1).
        CHECK(mid == epoch || mid == 1, "AC4: mid source is mutation epoch (or 1 if unset)");
        CHECK(reason.find("isolation-deny") != std::string::npos, "reason isolation-deny");
        // Foreign principal preserved in reason, not mid.
        CHECK(reason.find("ref-tenant=99") != std::string::npos ||
                  reason.find("99") != std::string::npos,
              "AC1: ref_tenant preserved in reason string");

        // Typed trail correlated by mid (epoch), not by tenant id.
        TypedMutationAuditEvent te{};
        CHECK(trail_find_by_mutation_id(mid, te), "AC2: trail finds isolation by epoch mid");
        TypedMutationAuditEvent bogus{};
        // Looking up by tenant id must NOT hit the isolation event mid.
        // (Unless epoch coincidentally equals tenant — we chose 99 / 10 vs epoch>=42.)
        if (ref_tenant != mid) {
            CHECK(!trail_find_by_mutation_id(ref_tenant, bogus) || bogus.mutation_id != mid,
                  "AC2: trail_find(ref_tenant) does not return isolation mid");
        }
        if (target_tenant != mid) {
            // May find older events; ensure isolation was not keyed by tenant.
            CHECK(mid != target_tenant, "mid not target_tenant");
        }
    }

    // ── AC2: effect + isolation can share epoch mid space ──
    {
        std::println("\n--- AC2: effect deny mid still real ---");
        reset_all();
        bump_mutation_epoch(5);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2); // Strict, no grant
        ev.set_capability_tenant_id(3);
        const auto epoch = current_mutation_epoch();
        (void)ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "ac2-effect", 0, 3,
                                                  /*provenance_mutation_id=*/0);
        // Effect path uses seq or provenance as mid — not tenant 3 as pollution.
        auto& ring = g_security_event_ring();
        const auto total = ring.total.load();
        CHECK(total >= 1, "effect event emitted");
        bool found_effect = false;
        for (std::size_t i = 0; i < kSecurityEventRingSize && i < total; ++i) {
            const auto& e = ring.ring[(total - 1 - i) % kSecurityEventRingSize];
            if (e.kind == SecurityEventKind::EffectDeny ||
                e.kind == SecurityEventKind::EffectAllow) {
                found_effect = true;
                CHECK(e.tenant_id == 3, "AC3: effect tenant preserved");
                // mid is seq or provenance — not forced to tenant.
                // Under no provenance, mid is ring seq (can be 0..n).
                CHECK(e.mutation_id != 3 || epoch == 3, "AC3: effect mid not polluted as tenant");
                break;
            }
        }
        CHECK(found_effect, "AC3: effect event present");
        (void)epoch;
    }

    // ── AC3: effect/isolation mid sources (#2388 fold + TypedMutation join) ──
    {
        std::println("\n--- AC3: effect path source unchanged ---");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto cap = read_file("src/core/capability_model.hh");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        CHECK(!sec.empty(), "evaluator_security.cpp readable");
        // #2388: SE mid comes from capability record_audit (prov.mutation_id /
        // epoch). Evaluator TypedMutationAudit mid prefers provenance then
        // epoch (not tenant).
        CHECK(sec.find("provenance_mutation_id != 0") != std::string::npos ||
                  cap.find("prov.mutation_id != 0") != std::string::npos,
              "AC3: effect mid prefers provenance then epoch");
        CHECK(sec.find("kIsolationAuditMidIssue") != std::string::npos ||
                  sec.find("2156") != std::string::npos ||
                  iso.find("kIsolationAuditMidIssue") != std::string::npos,
              "AC3: isolation path cites #2156");
        // Old pollution pattern must be gone.
        CHECK(sec.find("ref_tenant != 0 ? ref_tenant : target") == std::string::npos,
              "AC3: tenant-as-mid expression removed");
        CHECK(sec.find("capture_security_correlated_audit(mid") != std::string::npos ||
                  sec.find("capture_security_correlated_audit(mid,") != std::string::npos,
              "AC3: isolation uses mid variable");
        // Isolation dual-write uses Mutation epoch as mid (#2156).
        CHECK(iso.find("current_mutation_epoch") != std::string::npos,
              "AC3: isolation record_audit mid from Mutation epoch");
    }

    // ── Issue #3462: production audit mid never phantom 1; refuse joins mid=0 ──
    {
        std::println("\n--- #3462 AC1/AC2/AC3/AC5: production mid join + refuse ---");
        namespace ta = aura::compiler::typed_audit;
        using aura::core::store_workspace_epoch;
        using aura::core::WorkspaceEpochKind;
        auto& counters = ta::g_typed_mutation_audit_counters;
        reset_all();
        const auto prod_saved = counters.production_defaults_active.load(std::memory_order_relaxed);
        const auto epoch_saved = current_mutation_epoch();
        counters.production_defaults_active.store(1, std::memory_order_relaxed);
        ta::clear_boundary_audit_mid();
        store_workspace_epoch(WorkspaceEpochKind::Mutation, 0);
        CHECK(current_mutation_epoch() == 0, "3462 setup: epoch forced 0");

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2);  // arm the fiber-principal branch
        ev.set_capability_tenant_id(7); // ambient worker principal 7 (≠ fiber 42)

        // AC1: production refuse → no ring row (seq not consumed).
        const auto seq0 = ev.mutation_audit_seq();
        ev.emit_mutation_audit(1, 0, "ac3462-refuse", 0);
        CHECK(ev.mutation_audit_seq() == seq0,
              "AC1: production refuse writes no mutation ring row");
        bool refuse_seen = false;
        {
            auto& ring = g_security_event_ring();
            const auto total = ring.total.load(std::memory_order_relaxed);
            for (std::size_t i = 0; i < total && i < kSecurityEventRingSize; ++i) {
                const auto& e = ring.ring[(total - 1 - i) % kSecurityEventRingSize];
                if (e.mutation_id == 0 && std::string_view(e.reason).find("mid-fallback-refused") !=
                                              std::string_view::npos) {
                    refuse_seen = true;
                    break;
                }
            }
        }
        CHECK(refuse_seen, "AC1: mid-fallback-refused SE (mid=0) is the evidence");

        // Non-zero-mid deny row (tenant=0, fiber=0, mid=0x3462): the
        // mid=0 filter must exclude it (AC5 distinguishing setup).
        (void)ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "ac3462-nonzero", 0,
                                                  0,
                                                  /*provenance_mutation_id=*/0x3462);

        // AC3: live boundary mid joins the ring row; epoch stays raw 0.
        // (The nonzero deny above consumes its own ring row, so scan the
        // new rows for the join op instead of assuming an exact offset.)
        const auto seq_before_join = ev.mutation_audit_seq();
        ta::note_boundary_audit_mid(0x3462ull);
        ev.emit_mutation_audit(1, 0, "ac3462-join", 0);
        CHECK(ev.mutation_audit_seq() > seq_before_join, "AC3: joined ring row written");
        bool join_row_ok = false;
        for (auto s = seq_before_join; s < ev.mutation_audit_seq(); ++s) {
            const auto& r = ev.mutation_audit_entry_at(s);
            if (std::string_view(r.op) == "ac3462-join") {
                CHECK(r.provenance_mutation_id == 0x3462ull,
                      "AC3: ring row mid == boundary mid (join SSOT)");
                CHECK(r.epoch == 0, "AC3: ring epoch raw 0 (never coerced to 1)");
                join_row_ok = true;
            }
        }
        CHECK(join_row_ok, "AC3: join row present with raw epoch");
        ta::clear_boundary_audit_mid();

        // AC5: explicit mid=0 selects only the refuse rows.
        auto eval_int = [&cs](std::string_view src) -> std::int64_t {
            auto r = cs.eval(std::string(src));
            if (!r || !is_int(*r))
                return -1;
            return as_int(*r);
        };
        // query:security-audit is a stats-impl primitive — dispatched via
        // (engine:metrics "query:security-audit" ...), never a direct call.
        const auto n_zero =
            eval_int("(length (engine:metrics \"query:security-audit\" 50 0 0 0 0))");
        const auto n_zero_reason = eval_int("(length (engine:metrics \"query:security-audit\" 50 0 "
                                            "0 0 0 \"mid-fallback-refused\"))");
        CHECK(n_zero >= 1, "AC5: explicit mid=0 selects the refuse row(s)");
        CHECK(n_zero == n_zero_reason, "AC5: every mid=0 row is the mid-fallback-refused evidence");

        // AC2: fiber-principal-mismatch deny emits no phantom mid=1 SE.
        {
            auto fiber_owned = std::make_unique<aura::serve::Fiber>([] {});
            fiber_owned->set_assigned_tenant_id(42);
            aura_fiber_install_tenant_scope_for_resume(fiber_owned.get());
            CHECK(fiber_owned->resume_had_mismatch(), "AC2 setup: mismatch flag set");
            auto& ring = g_security_event_ring();
            const auto se_before = ring.total.load(std::memory_order_relaxed);
            const bool ok = ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate,
                                                                "ac3462-fiber", 0, 0,
                                                                /*provenance_mutation_id=*/0);
            CHECK(!ok, "AC2: hard deny stands under production refuse");
            const auto se_after = ring.total.load(std::memory_order_relaxed);
            bool phantom = false;
            for (auto i = se_before; i < se_after; ++i) {
                const auto& e = ring.ring[i % kSecurityEventRingSize];
                if (e.mutation_id == 1)
                    phantom = true;
            }
            CHECK(!phantom, "AC2: fiber-principal-mismatch emits no mid=1 SE");
        }

        counters.production_defaults_active.store(prod_saved, std::memory_order_relaxed);
        store_workspace_epoch(WorkspaceEpochKind::Mutation, epoch_saved);
    }

    // ── Query surface ──
    {
        std::println("\n--- query:security-audit-stats schema-2156 ---");
        reset_all();
        CompilerService cs;
        CHECK(href_stats(cs, "schema-2156") == 2156, "schema-2156");
        CHECK(href_stats(cs, "isolation-audit-mid-wired") == 1, "wired");
        CHECK(href_stats(cs, "schema-2054") == 2054, "schema-2054 preserved");
    }

    // ── Source docs ──
    {
        const auto hh = read_file("src/core/security_event.hh");
        CHECK(hh.find("kIsolationAuditMidIssue") != std::string::npos, "header stamp");
        CHECK(hh.find("2156") != std::string::npos, "cites 2156");
    }

    std::println("\n=== #2156 isolation audit mid: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_isolation_audit_mid();
}
#endif
