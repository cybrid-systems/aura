// @category: unit
// @reason: Issue #2054 — unify capability / mutation / TypedMutationAudit
// into a single Agent-queryable security audit surface.
//
//   AC1: check_and_record_effect allow + deny both append SecurityEvent
//   AC2: deny produces visible reason Agent can reason about
//   AC3: TypedMutation trail correlated by mutation_id (always-on)
//   AC4: query:security-audit filters tenant / fiber / since-seq / mutation-id
//   AC5: query joins typed_kind + typed_outcome on matching mutation_id
//   AC6: schema-2054 on query:security-audit-stats
//   AC7: WAL replay rebuilds unified SecurityEvent ring
//   AC8: ring size preserved (kSecurityEventRingSize == 64)

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/mutation_audit_wal.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::security::kCapWildcard;
using aura::compiler::security::kEffectMutate;
using aura::compiler::typed_audit::AuditOutcome;
using aura::compiler::typed_audit::MutationKind;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::trail_find_by_mutation_id;
using aura::compiler::typed_audit::TypedMutationAuditEvent;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::core::audit_wal::g_mutation_audit_wal;
using aura::core::audit_wal::reset_audit_wal_for_test;
using aura::core::capability::Effect;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::append_security_event;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::kSecurityAuditUnifyIssue;
using aura::core::security_event::kSecurityEventRingSize;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::security_event::SecurityEventKind;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t href_stats(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:security-audit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::vector<std::string> list_string_lines(CompilerService& cs, const EvalValue& v) {
    std::vector<std::string> lines;
    auto cur = v;
    int guard = 0;
    auto& pairs = cs.evaluator().pairs();
    while (is_pair(cur) && guard++ < 128) {
        auto idx = as_pair_idx(cur);
        if (idx >= pairs.size())
            break;
        if (is_string(pairs[idx].car)) {
            auto sidx = as_string_idx(pairs[idx].car);
            auto heap = cs.evaluator().string_heap();
            if (sidx < heap.size())
                lines.push_back(std::string(heap[sidx]));
        }
        cur = pairs[idx].cdr;
    }
    return lines;
}

void reset_process() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    reset_audit_wal_for_test();
    reset_for_test();
    reset_security_event_ring_for_test();
    set_mode(SandboxMode::Off);
}

} // namespace

int run_test_security_audit_unify() {
    std::println("=== Issue #2054: unified security audit surface ===");
    CHECK(kSecurityAuditUnifyIssue == 2054, "issue stamp");
    CHECK(kSecurityEventRingSize == 64, "AC8: ring size preserved at 64");

    // ── AC1/AC2: deny + allow append SecurityEvent with reason ──
    {
        std::println("\n--- AC1/AC2: allow+deny emit SecurityEvent ---");
        reset_process();
        apply_production_security_defaults();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        const auto seq_before = g_security_event_ring().seq.load(std::memory_order_relaxed);

        // Deny: Restricted without mutate grant.
        const bool denied = !ev.check_and_record_effect(kEffectMutate, kEffectMutate, "test:deny",
                                                        0, /*tenant_id=*/7,
                                                        /*provenance_mutation_id=*/9000);
        CHECK(denied, "Restricted without grant denies");
        const auto seq_after_deny = g_security_event_ring().seq.load(std::memory_order_relaxed);
        CHECK(seq_after_deny > seq_before, "deny increments security event seq");
        {
            const auto& e =
                g_security_event_ring().ring[(seq_after_deny - 1) % kSecurityEventRingSize];
            std::println("  deny event: kind={} denied={} reason={} op={} mid={}",
                         static_cast<int>(e.kind), e.denied, e.reason, e.op, e.mutation_id);
            CHECK(e.denied == true, "deny event denied=true");
            CHECK(e.kind == SecurityEventKind::EffectDeny, "kind EffectDeny");
            CHECK(e.mutation_id == 9000, "deny mutation_id stamped");
            CHECK(std::string_view(e.reason).find("deny") != std::string_view::npos,
                  "deny reason visible to Agent");
        }

        // Allow: grant mutate effect to tenant 7.
        g_capability_registry().grant(7, "mutate", static_cast<Effect>(kEffectMutate));
        const auto seq_mid = g_security_event_ring().seq.load(std::memory_order_relaxed);
        const bool allowed =
            ev.check_and_record_effect(kEffectMutate, kEffectMutate, "test:allow-2054", 0,
                                       /*tenant_id=*/7,
                                       /*provenance_mutation_id=*/9001);
        CHECK(allowed, "direct check_and_record_effect allows after grant");
        const auto seq_after_allow = g_security_event_ring().seq.load(std::memory_order_relaxed);
        CHECK(seq_after_allow > seq_mid, "allow increments security event seq");
        {
            const auto& e =
                g_security_event_ring().ring[(seq_after_allow - 1) % kSecurityEventRingSize];
            std::println("  allow event: kind={} denied={} mutation_id={} tenant={}",
                         static_cast<int>(e.kind), e.denied, e.mutation_id, e.tenant_id);
            CHECK(e.denied == false, "allow event denied=false");
            CHECK(e.kind == SecurityEventKind::EffectAllow, "kind EffectAllow");
            CHECK(e.mutation_id == 9001, "mutation_id correlated");
            CHECK(e.tenant_id == 7, "tenant_id stamped");
        }
    }

    // ── AC3: TypedMutation correlation by mutation_id ──
    {
        std::println("\n--- AC3: TypedMutation correlated by mutation_id ---");
        reset_process();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0); // Off so allow path is free
        const std::uint64_t mid = 4242;
        const bool ok = ev.check_and_record_effect(kEffectMutate, kEffectMutate, "mutate-corr", 11,
                                                   /*tenant_id=*/3, mid);
        CHECK(ok, "allow check under Off sandbox");
        TypedMutationAuditEvent te{};
        CHECK(trail_find_by_mutation_id(mid, te), "typed trail has mutation_id");
        CHECK(te.mutation_id == mid, "typed.mutation_id matches");
        CHECK(te.outcome == AuditOutcome::Success, "typed outcome Success on allow");
        CHECK(te.kind == MutationKind::Structural || te.kind == MutationKind::Other,
              "typed kind classified from op");
        std::println("  typed: seq={} kind={} outcome={} name={}", te.seq,
                     static_cast<int>(te.kind), static_cast<int>(te.outcome), te.name);

        // Deny correlation
        apply_production_security_defaults();
        ev.set_effect_sandbox_mode(1); // Restricted
        reset_capability_effects_for_test();
        const std::uint64_t mid_deny = 5252;
        const bool denied =
            !ev.check_and_record_effect(kEffectMutate, kEffectMutate, "mutate-deny-corr", 12,
                                        /*tenant_id=*/3, mid_deny);
        CHECK(denied, "Restricted without grant denies");
        TypedMutationAuditEvent te2{};
        CHECK(trail_find_by_mutation_id(mid_deny, te2), "typed trail has deny mutation_id");
        CHECK(te2.outcome == AuditOutcome::Error, "typed outcome Error on deny");
    }

    // ── AC4/AC5: query:security-audit filters + typed join ──
    {
        std::println("\n--- AC4/AC5: query:security-audit filters + join ---");
        reset_process();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (id x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");

        auto& ring = g_security_event_ring();
        append_security_event(ring, SecurityEventKind::EffectDeny, /*tenant=*/10,
                              /*mutation_id=*/7001, /*epoch=*/1, kEffectMutate, "op-a", "seed-deny",
                              true, /*fiber=*/5);
        append_security_event(ring, SecurityEventKind::EffectAllow, /*tenant=*/10,
                              /*mutation_id=*/7002, /*epoch=*/2, kEffectMutate, "op-b",
                              "seed-allow", false, /*fiber=*/5);
        append_security_event(ring, SecurityEventKind::EffectAllow, /*tenant=*/20,
                              /*mutation_id=*/7003, /*epoch=*/3, kEffectMutate, "op-c",
                              "other-tenant", false, /*fiber=*/9);
        // Feed typed for 7002 so join hits.
        aura::compiler::typed_audit::capture_security_correlated_audit(7002, "op-b", /*epoch=*/2,
                                                                       /*denied=*/false, 0, 5);

        // Filter by tenant=10: (limit tenant) → 10 10
        auto r_t = cs.eval(R"((engine:metrics \"query:security-audit\" 10 10))");
        CHECK(r_t.has_value(), "security-audit tenant query ok");
        auto lines_t = list_string_lines(cs, *r_t);
        std::println("  tenant=10 lines: {}", lines_t.size());
        CHECK(!lines_t.empty(), "tenant filter returns rows");
        bool all_t10 = true;
        bool saw_typed = false;
        for (const auto& ln : lines_t) {
            std::println("    {}", ln);
            if (ln.find("tenant=10") == std::string::npos)
                all_t10 = false;
            if (ln.find("tenant=20") != std::string::npos)
                all_t10 = false;
            if (ln.find("mutation_id=7002") != std::string::npos &&
                ln.find("typed_kind=") != std::string::npos &&
                ln.find("typed_kind=-") == std::string::npos)
                saw_typed = true;
            CHECK(ln.find("schema=2054") != std::string::npos, "line has schema=2054");
        }
        CHECK(all_t10, "all returned rows match tenant=10");
        CHECK(saw_typed, "typed join present for mutation_id=7002");

        // mutation-id filter: limit=10 tenant=10 fiber=5 since=0 mid=7001
        auto r_mid = cs.eval(R"((engine:metrics \"query:security-audit\" 10 10 5 0 7001))");
        CHECK(r_mid.has_value(), "mutation-id filter query ok");
        auto lines_mid = list_string_lines(cs, *r_mid);
        bool only_7001 = !lines_mid.empty();
        for (const auto& ln : lines_mid) {
            std::println("  mid-filter: {}", ln);
            if (ln.find("mutation_id=7001") == std::string::npos)
                only_7001 = false;
        }
        CHECK(only_7001, "mutation-id filter returns only 7001");

        // tenant=20
        auto r20 = cs.eval(R"((engine:metrics \"query:security-audit\" 10 20))");
        CHECK(r20.has_value(), "tenant=20 query ok");
        auto lines20 = list_string_lines(cs, *r20);
        CHECK(!lines20.empty(), "tenant=20 has rows");
        for (const auto& ln : lines20) {
            CHECK(ln.find("tenant=20") != std::string::npos, "tenant=20 row");
        }

        // fiber=9 with tenant=20
        auto rf = cs.eval(R"((engine:metrics \"query:security-audit\" 10 20 9))");
        CHECK(rf.has_value(), "fiber filter query ok");
        auto lines_f = list_string_lines(cs, *rf);
        CHECK(!lines_f.empty(), "fiber=9 returns rows");
        for (const auto& ln : lines_f) {
            CHECK(ln.find("fiber=9") != std::string::npos, "fiber=9 row");
        }
    }

    // ── AC6: schema-2054 stats ──
    {
        std::println("\n--- AC6: query:security-audit-stats schema-2054 ---");
        reset_process();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (g x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        CHECK(href_stats(cs, "schema-2054") == 2054, "schema-2054 key");
        CHECK(href_stats(cs, "schema") == 2054, "schema key");
        CHECK(href_stats(cs, "unified") == 1, "unified=1");
        CHECK(href_stats(cs, "ring-size") == 64, "ring-size=64");
        auto st = cs.eval(R"((engine:metrics \"query:security-audit-stats\"))");
        CHECK(st && is_hash(*st), "stats returns hash");
    }

    // ── AC7: WAL replay rebuilds security ring ──
    {
        std::println("\n--- AC7: WAL replay rebuilds unified trail ---");
        reset_process();
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / "aura-2054-wal-test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        {
            CompilerService cs;
            auto& ev = cs.evaluator();
            CHECK(ev.enable_mutation_audit_wal(dir.string()), "enable WAL");
            // Allow under Off
            ev.set_effect_sandbox_mode(0);
            (void)ev.check_and_record_effect(kEffectMutate, kEffectMutate, "wal-a", 1, 1, 8001);
            // Deny under Restricted
            apply_production_security_defaults();
            ev.set_effect_sandbox_mode(1);
            reset_capability_effects_for_test();
            (void)ev.check_and_record_effect(kEffectMutate, kEffectMutate, "wal-deny", 2, 1, 8002);
            CHECK(g_mutation_audit_wal().is_enabled(), "WAL enabled");
            const auto seq_live = g_security_event_ring().seq.load(std::memory_order_relaxed);
            CHECK(seq_live >= 2, "at least 2 security events before restart sim");
            // Flush pending batch so restart sees both records.
            g_mutation_audit_wal().disable();
        }

        // Simulate restart
        reset_security_event_ring_for_test();
        CHECK(g_security_event_ring().seq.load() == 0, "ring cleared");
        reset_audit_wal_for_test();
        CompilerService cs2;
        CHECK(cs2.evaluator().enable_mutation_audit_wal(dir.string()), "re-enable WAL replay");
        const auto seq_replay = g_security_event_ring().seq.load(std::memory_order_relaxed);
        std::println("  security events after WAL replay: {}", seq_replay);
        CHECK(seq_replay >= 2, "WAL replay rebuilt both security events");
        bool saw_deny = false;
        bool saw_allow = false;
        bool saw_replay_reason = false;
        for (std::size_t i = 0; i < kSecurityEventRingSize && i < seq_replay; ++i) {
            const auto& e = g_security_event_ring().ring[i % kSecurityEventRingSize];
            if (e.kind == SecurityEventKind::EffectDeny)
                saw_deny = true;
            if (e.kind == SecurityEventKind::EffectAllow)
                saw_allow = true;
            if (std::string_view(e.reason).find("wal-replay") != std::string_view::npos)
                saw_replay_reason = true;
        }
        CHECK(saw_allow, "replayed allow event");
        CHECK(saw_deny, "replayed deny event");
        CHECK(saw_replay_reason, "replay stamps wal-replay reason");
        fs::remove_all(dir, ec);
        (void)kCapWildcard;
    }

    // ── Direct unit: append fields ──
    {
        std::println("\n--- field layout + EffectAllow ---");
        reset_security_event_ring_for_test();
        append_security_event(g_security_event_ring(), SecurityEventKind::EffectAllow, 1, 2, 3,
                              kEffectMutate, "unit-op", "unit-reason", false, 99);
        const auto& e = g_security_event_ring().ring[0];
        CHECK(e.seq == 0, "first event seq=0");
        CHECK(e.fiber_id == 99, "fiber_id stored");
        CHECK(e.denied == false, "denied=false on allow");
        CHECK(e.kind == SecurityEventKind::EffectAllow, "EffectAllow kind");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_security_audit_unify();
}
#endif
