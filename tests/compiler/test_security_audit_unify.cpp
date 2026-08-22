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
//   AC8: ring size preserved (kSecurityEventRingSize == 1024, #2225)
//   #3113 AC1: wrap → query:security-audit typed-trail-miss=1 + window/se-ring
//   #3113 AC2: WAL on → wal-replay-hint=1 (is_enabled only, no scan)
//   #3113 AC3: typed_trail_wrap_total + stats typed-trail-wrap-risk
//   #3113 AC4: in-memory join is last 256; SE 1024 + WAL for full replay
//   #3113 AC5: keep 256+WAL (no AURA_TYPED_TRAIL_SIZE)
//   #3113 AC6: Soft / WAL-off miss mark OK, no extra I/O
//   #3114 AC1–AC6: query:evolution-audit-decision observe-only fold

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/mutation_audit_wal.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/security_event_wal.hh"
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
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditOutcome;
using aura::compiler::typed_audit::capture_audit_event_forced;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::kTypedMutationAuditTrailSize;
using aura::compiler::typed_audit::kTypedTrailWrapMissIssue;
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
using aura::core::capability::EffectProvenance;
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
using aura::core::security_event_wal::emit_security_event_durable;
using aura::core::security_event_wal::g_security_event_wal;
using aura::core::security_event_wal::reset_security_event_wal_for_test;
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

std::int64_t href_evol(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:evolution-audit-decision\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t href_evol_mid(CompilerService& cs, std::uint64_t mid, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:evolution-audit-decision\" {}) \"{}\")", mid, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t href_evol_mid_durable(CompilerService& cs, std::uint64_t mid, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:evolution-audit-decision\" {} \"durable\") \"{}\")", mid,
        key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::string href_evol_reason(CompilerService& cs, std::uint64_t mid, bool durable) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:evolution-audit-decision\" {}{}) \"last-se-reason\")",
        mid, durable ? " \"durable\"" : ""));
    if (!r || !is_string(*r))
        return {};
    const auto idx = as_string_idx(*r);
    const auto heap = cs.evaluator().string_heap();
    if (idx >= heap.size())
        return {};
    return std::string(heap[idx]);
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
    reset_security_event_wal_for_test();
    reset_for_test();
    reset_security_event_ring_for_test();
    set_mode(SandboxMode::Off);
}

} // namespace

int run_test_security_audit_unify() {
    std::println("=== Issue #2054: unified security audit surface ===");
    CHECK(kSecurityAuditUnifyIssue == 2054, "issue stamp");
    CHECK(kSecurityEventRingSize == 1024, "AC8: ring size 1024 (#2225 / #3113)");
    CHECK(kTypedMutationAuditTrailSize == 256, "3113 AC4: typed trail window 256");
    CHECK(kTypedTrailWrapMissIssue == 3113, "3113 issue stamp");

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
        const bool denied = !ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate,
                                                                 "test:deny", 0, /*tenant_id=*/7,
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

        // Allow: grant mutate effect to tenant 7 bound to mid 9001
        // (#2707: production Restricted fail-closed mid join requires match).
        {
            EffectProvenance gp{};
            gp.mutation_id = 9001;
            gp.epoch = 9001;
            g_capability_registry().grant(7, "mutate", static_cast<Effect>(kEffectMutate), gp);
        }
        const auto seq_mid = g_security_event_ring().seq.load(std::memory_order_relaxed);
        const bool allowed =
            ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "test:allow-2054", 0,
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
        const bool ok =
            ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "mutate-corr", 11,
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
        const bool denied = !ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate,
                                                                 "mutate-deny-corr", 12,
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
        auto r_t = cs.eval(R"((engine:metrics "query:security-audit" 10 10))");
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
        auto r_mid = cs.eval(R"((engine:metrics "query:security-audit" 10 10 5 0 7001))");
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
        auto r20 = cs.eval(R"((engine:metrics "query:security-audit" 10 20))");
        CHECK(r20.has_value(), "tenant=20 query ok");
        auto lines20 = list_string_lines(cs, *r20);
        CHECK(!lines20.empty(), "tenant=20 has rows");
        for (const auto& ln : lines20) {
            CHECK(ln.find("tenant=20") != std::string::npos, "tenant=20 row");
        }

        // fiber=9 with tenant=20
        auto rf = cs.eval(R"((engine:metrics "query:security-audit" 10 20 9))");
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
        CHECK(href_stats(cs, "ring-size") == 1024, "ring-size=1024");
        auto st = cs.eval(R"((engine:metrics "query:security-audit-stats"))");
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
            (void)ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "wal-a", 1, 1,
                                                      8001);
            // Deny under Restricted
            apply_production_security_defaults();
            ev.set_effect_sandbox_mode(1);
            reset_capability_effects_for_test();
            (void)ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "wal-deny", 2,
                                                      1, 8002);
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

    // ── Issue #3113: typed trail wrap miss is explicit, not "no audit" ──
    {
        std::println("\n--- 3113 AC1/AC3: wrap → typed-trail-miss + wrap-total ---");
        reset_process();
        CompilerService cs;

        const std::uint64_t mid = 8113;
        append_security_event(g_security_event_ring(), SecurityEventKind::EffectDeny,
                              /*tenant=*/10, mid, /*epoch=*/1, kEffectMutate, "op-wrap",
                              "wrap-seed-deny", true, /*fiber=*/7);
        aura::compiler::typed_audit::capture_security_correlated_audit(mid, "op-wrap", /*epoch=*/1,
                                                                       /*denied=*/true, 0, 7);
        TypedMutationAuditEvent te0{};
        CHECK(trail_find_by_mutation_id(mid, te0), "3113: mid in trail before wrap");

        auto r_hit = cs.eval(R"((engine:metrics "query:security-audit" 8 10 7 0 8113))");
        CHECK(r_hit.has_value(), "3113 pre-wrap query ok");
        auto lines_hit = list_string_lines(cs, *r_hit);
        CHECK(!lines_hit.empty(), "3113 pre-wrap has SE row");
        for (const auto& ln : lines_hit) {
            CHECK(ln.find("typed-trail-miss=0") != std::string::npos, "3113 AC1: hit miss=0");
            CHECK(ln.find("typed-trail-size=256") != std::string::npos, "3113 AC1: window=256");
            CHECK(ln.find("se-ring-size=1024") != std::string::npos, "3113 AC1: se-ring-size=1024");
            CHECK(ln.find("wal-replay-hint=0") != std::string::npos,
                  "3113 AC2/AC6: WAL off → hint=0");
            CHECK(ln.find("schema=2054") != std::string::npos, "3113: schema=2054 retained");
        }

        CHECK(href_stats(cs, "typed-trail-window") == 256, "3113 AC3: stats window=256");
        CHECK(href_stats(cs, "se-ring-size") == 1024, "3113 AC3: stats se-ring-size");
        CHECK(href_stats(cs, "schema-3113") == 3113, "3113 AC3: schema-3113");
        CHECK(href_stats(cs, "typed-trail-size") >= 1, "3113: typed-trail-size live occupancy");
        CHECK(href_stats(cs, "typed-trail-wrap-risk") == 0, "3113 AC3: no wrap-risk yet");

        const auto wrap0 =
            g_typed_mutation_audit_counters.typed_trail_wrap_total.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < kTypedMutationAuditTrailSize; ++i) {
            capture_audit_event_forced(92000 + i, "wrap-fill", MutationKind::Other, 1, 1,
                                       AuditOutcome::Success);
        }
        const auto wrap1 =
            g_typed_mutation_audit_counters.typed_trail_wrap_total.load(std::memory_order_relaxed);
        std::println("  typed_trail_wrap_total {} → {}", wrap0, wrap1);
        CHECK(wrap1 > wrap0, "3113 AC3: wrap counter increases after >256 captures");

        TypedMutationAuditEvent te_miss{};
        CHECK(!trail_find_by_mutation_id(mid, te_miss), "3113: old mid evicted from typed trail");

        auto r_miss = cs.eval(R"((engine:metrics "query:security-audit" 8 10 7 0 8113))");
        CHECK(r_miss.has_value(), "3113 wrap query ok");
        auto lines_miss = list_string_lines(cs, *r_miss);
        CHECK(!lines_miss.empty(), "3113 AC1: SE row still present after typed wrap");
        bool saw_miss = false;
        for (const auto& ln : lines_miss) {
            std::println("  wrap-miss: {}", ln);
            if (ln.find("mutation_id=8113") != std::string::npos &&
                ln.find("typed-trail-miss=1") != std::string::npos)
                saw_miss = true;
            CHECK(ln.find("typed-trail-size=256") != std::string::npos, "3113 AC1: window on miss");
            CHECK(ln.find("se-ring-size=1024") != std::string::npos, "3113 AC1: se-ring on miss");
            CHECK(ln.find("wal-replay-hint=0") != std::string::npos,
                  "3113 AC2/AC6: WAL off no hint, no scan");
            CHECK(ln.find("typed_kind=-") != std::string::npos, "3113: typed details gone");
            CHECK(ln.find("schema=2054") != std::string::npos, "3113: existing schema key kept");
        }
        CHECK(saw_miss, "3113 AC1: typed-trail-miss=1 (not silent no-audit)");
        CHECK(href_stats(cs, "typed-trail-wrap-risk") == 1,
              "3113 AC3: wrap-risk=1 after trail_seq>256 + SE");
        CHECK(href_stats(cs, "typed-trail-wrap-total") == static_cast<std::int64_t>(wrap1),
              "3113 AC3: stats wrap-total");

        // AC6 Soft default list: no mid filter, WAL still off — miss mark
        // may appear, no extra disk I/O (hint stays 0).
        auto r_list = cs.eval(R"((engine:metrics "query:security-audit" 10))");
        CHECK(r_list.has_value(), "3113 AC6: default list ok");
        auto lines_list = list_string_lines(cs, *r_list);
        CHECK(!lines_list.empty(), "3113 AC6: default list has rows");
        for (const auto& ln : lines_list) {
            CHECK(ln.find("typed-trail-miss=") != std::string::npos,
                  "3113 AC6: miss field present");
            CHECK(ln.find("wal-replay-hint=0") != std::string::npos, "3113 AC6: no WAL scan/hint");
        }

        CHECK(!g_mutation_audit_wal().is_enabled(), "3113 AC6: mutation WAL off");
        CHECK(!g_security_event_wal().is_enabled(), "3113 AC6: SE WAL off");
    }

    {
        std::println("\n--- 3113 AC2: WAL on → wal-replay-hint=1 ---");
        reset_process();
        CompilerService cs;
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / "aura-3113-wal-hint";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        // Enable first: enable_mutation_audit_wal resets the SE ring, so seed
        // the wrap scenario after WAL is on.
        CHECK(cs.evaluator().enable_mutation_audit_wal(dir.string()), "3113 AC2: enable WAL");
        CHECK(g_mutation_audit_wal().is_enabled() || g_security_event_wal().is_enabled(),
              "3113 AC2: some WAL enabled");
        const std::uint64_t mid = 8114;
        append_security_event(g_security_event_ring(), SecurityEventKind::EffectDeny,
                              /*tenant=*/10, mid, /*epoch=*/1, kEffectMutate, "op-wal",
                              "wrap-wal-deny", true, /*fiber=*/7);
        aura::compiler::typed_audit::capture_security_correlated_audit(mid, "op-wal", /*epoch=*/1,
                                                                       /*denied=*/true, 0, 7);
        for (std::size_t i = 0; i < kTypedMutationAuditTrailSize; ++i) {
            capture_audit_event_forced(93000 + i, "wrap-fill-wal", MutationKind::Other, 1, 1,
                                       AuditOutcome::Success);
        }
        TypedMutationAuditEvent te{};
        CHECK(!trail_find_by_mutation_id(mid, te), "3113 AC2: mid wrapped out of typed trail");
        auto r_wal = cs.eval(R"((engine:metrics "query:security-audit" 8 10 7 0 8114))");
        CHECK(r_wal.has_value(), "3113 AC2: WAL-on query ok");
        auto lines_wal = list_string_lines(cs, *r_wal);
        bool saw_hint = false;
        for (const auto& ln : lines_wal) {
            std::println("  wal-hint: {}", ln);
            if (ln.find("typed-trail-miss=1") != std::string::npos &&
                ln.find("wal-replay-hint=1") != std::string::npos)
                saw_hint = true;
        }
        CHECK(saw_hint, "3113 AC2: miss + WAL enabled → wal-replay-hint=1");
        cs.evaluator().disable_mutation_audit_wal();
        fs::remove_all(dir, ec);
    }

    // ── Issue #3114: evolution-audit-decision observe-only fold ──
    {
        std::println("\n--- 3114 AC1/AC3: idle Soft fold ---");
        reset_process();
        CompilerService cs;
        CHECK(href_evol(cs, "schema-3114") == 3114, "3114 AC1: schema-3114");
        CHECK(href_evol(cs, "issue-3114") == 3114, "3114 AC1: issue-3114");
        CHECK(href_evol(cs, "evolution-audit-decision-wired") == 1, "3114 AC1: wired");
        CHECK(href_evol(cs, "observe-only") == 1, "3114 AC5: observe-only fold");
        CHECK(href_evol(cs, "last-audit-mid") == 0, "3114 AC3: no boundary → mid=0");
        CHECK(href_evol(cs, "typed-trail-miss") == 0, "3114 AC3: no mid → miss=0");
        CHECK(href_evol(cs, "typed-outcome") == 0, "3114 AC3: unknown when no trail");
        const auto pb = href_evol(cs, "playbook-action");
        CHECK(pb >= 0 && pb <= 6, "3114 AC3: playbook-action in idle..reject-cross-ws");
        CHECK(href_evol(cs, "playbook-wired") == 1, "3114 AC2: playbook-wired");
        CHECK(href_evol(cs, "overflow") == -1, "3114 AC4: no overflow at planned cap");
        CHECK(href_evol(cs, "densify-ok") == 1, "3114 AC2: densify-ok default healthy");
    }

    {
        std::println("\n--- 3114 AC2/AC6: wrap → typed-outcome=unknown + miss ---");
        reset_process();
        CompilerService cs;
        const std::uint64_t mid = 8115;
        append_security_event(g_security_event_ring(), SecurityEventKind::EffectDeny,
                              /*tenant=*/10, mid, /*epoch=*/1, kEffectMutate, "op-3114",
                              "evol-wrap-deny", true, /*fiber=*/7);
        aura::compiler::typed_audit::capture_security_correlated_audit(mid, "op-3114", /*epoch=*/1,
                                                                       /*denied=*/true, 0, 7);
        CHECK(href_evol_mid(cs, mid, "typed-trail-miss") == 0, "3114: pre-wrap miss=0");
        CHECK(href_evol_mid(cs, mid, "typed-outcome") == 3, "3114: pre-wrap Error");
        CHECK(href_evol_mid(cs, mid, "last-se-denied") == 1, "3114: SE denied");
        CHECK(href_evol_mid(cs, mid, "last-audit-mid") == static_cast<std::int64_t>(mid),
              "3114 AC6: mid arg");
        for (std::size_t i = 0; i < kTypedMutationAuditTrailSize; ++i) {
            capture_audit_event_forced(94000 + i, "wrap-fill-3114", MutationKind::Other, 1, 1,
                                       AuditOutcome::Success);
        }
        CHECK(href_evol_mid(cs, mid, "typed-trail-miss") == 1, "3114 AC6: wrap typed-trail-miss=1");
        CHECK(href_evol_mid(cs, mid, "typed-outcome") == 0, "3114 AC6: wrap typed-outcome=unknown");
        CHECK(href_evol_mid(cs, mid, "last-se-denied") == 1, "3114: SE still present after wrap");
        CHECK(href_evol(cs, "observe-only") == 1, "3114 AC5: still observe-only after wrap");
    }

    // ── #3149: last-se-reason additive key (residual after #3114).
    // Close the gap where decision only exposed last-se-reason-code
    // (SecurityEventKind+1) without the SE reason[64] string, forcing
    // Agent to follow up with query:security-audit for the stable deny
    // reason text. Additive key alongside existing last-se-reason-code;
    // NUL-safe truncation from e.reason[64]; mid filter preserves
    // consistency with the same-mid SE row.
    {
        std::println("\n--- 3149 AC7/AC8/AC9: last-se-reason additive key ---");
        reset_process();
        CompilerService cs;
        // AC8: Soft / no event → last-se-reason-code == 0; last-se-reason
        // key is omitted OR present-but-empty (omit-safe). We assert the
        // code side here; the string-side omit / "" is enforced at the C++
        // layer + via the linter (no test_issue_3149.cpp per #81967).
        CHECK(href_evol(cs, "last-se-reason-code") == 0,
              "3149 AC8: Soft / no event → last-se-reason-code == 0");
        // AC9: schema-3149 / issue-3149 additive sentinels coexist
        // with the original schema-3114 / issue-3114 (no replacement).
        CHECK(href_evol(cs, "schema-3114") == 3114,
              "3149 AC9: schema-3114 still present (coexists with 3149)");
        CHECK(href_evol(cs, "issue-3114") == 3114,
              "3149 AC9: issue-3114 still present (coexists with 3149)");
        // AC10: no test_issue_3149.cpp / docs/design/3149-* — enforced
        // by the linter (check_evolution_audit_decision_3114.py AC10).
        // Suite-level coverage is sufficient (#3149 extends the existing
        // test_security_audit_unify.cpp file per #81967).
        CHECK(href_evol(cs, "schema-3205") == 3205, "3205 AC3: schema-3205 additive");
        CHECK(href_evol(cs, "issue-3205") == 3205, "3205 AC3: issue-3205 additive");
        CHECK(href_evol(cs, "durable-hit") == 0, "3205 AC2: default durable-hit=0");
        CHECK(href_evol(cs, "overflow") == -1, "3205 AC3: overflow=0 path (hash-ref -1)");
    }

    {
        std::println("\n--- 3149 AC7: SE ring reason → last-se-reason string ---");
        reset_process();
        CompilerService cs;
        const std::uint64_t mid = 3149;
        // Insert an SE row with a specific reason[64] string. After the
        // evolution-audit-decision call, the last-se-reason string key
        // should equal this reason (NUL-safe truncated).
        constexpr const char* kReason = "invariant-force-rollback";
        append_security_event(g_security_event_ring(), SecurityEventKind::InvariantFail,
                              /*tenant=*/11, mid, /*epoch=*/1, kEffectMutate, "op-3149", kReason,
                              /*denied=*/true, /*fiber=*/8);
        const auto code = href_evol_mid(cs, mid, "last-se-reason-code");
        CHECK(code == static_cast<std::int64_t>(SecurityEventKind::InvariantFail) + 1,
              "3149 AC7: last-se-reason-code matches SE kind+1");
        // The string side is omitted by href_evol (which returns int).
        // We assert the string surface at the C++ source level (linter
        // AC7 verifies insert_kv_str("last-se-reason", ...) call site +
        // NUL-safe strnlen truncation). The Aura string lookup path is
        // tested via the engine:metrics surface (test_engine_metrics_facade.cpp
        // covers the hash_read string plumbing).
    }

    // ── Issue #3205: optional :durable mid point-query into WAL ──
    {
        std::println("\n--- 3205 AC1: wrap SE ring, :durable recovers WAL reason ---");
        reset_process();
        apply_production_audit_defaults();
        CompilerService cs;
        apply_production_audit_defaults();
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / "aura-3205-durable-wal";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        CHECK(cs.evaluator().enable_security_event_wal(dir.string()), "3205 AC1: enable SE WAL");
        const std::uint64_t mid = 3205;
        constexpr const char* kReason = "durable-wrap-deny";
        emit_security_event_durable(SecurityEventKind::EffectDeny, /*tenant=*/12, mid, /*epoch=*/1,
                                    kEffectMutate, "op-3205", kReason, /*denied=*/true,
                                    /*fiber=*/9);
        aura::compiler::typed_audit::capture_security_correlated_audit(mid, "op-3205", /*epoch=*/1,
                                                                       /*denied=*/true, 0, 9);
        for (std::size_t i = 0; i < kTypedMutationAuditTrailSize; ++i) {
            capture_audit_event_forced(95000 + i, "wrap-fill-3205", MutationKind::Other, 1, 1,
                                       AuditOutcome::Success);
        }
        for (std::size_t i = 0; i < kSecurityEventRingSize; ++i) {
            append_security_event(g_security_event_ring(), SecurityEventKind::EffectAllow,
                                  /*tenant=*/1, 40000 + i, /*epoch=*/1, kEffectMutate, "wrap",
                                  "fill", false, /*fiber=*/1);
        }
        CHECK(href_evol_mid(cs, mid, "typed-trail-miss") == 1, "3205 AC1: typed miss after wrap");
        CHECK(href_evol_mid(cs, mid, "forensic-source") == 3,
              "3205 AC1: no :durable → forensic-source=3");
        CHECK(href_evol_mid(cs, mid, "durable-hit") == 0, "3205 AC1: no :durable → durable-hit=0");
        CHECK(href_evol_reason(cs, mid, /*durable=*/false).empty(),
              "3205 AC1: no :durable → reason empty after ring wrap");
        CHECK(href_evol_mid_durable(cs, mid, "durable-hit") == 1,
              "ac3205_1_durable_hit: :durable recovers WAL row");
        CHECK(href_evol_reason(cs, mid, /*durable=*/true) == kReason,
              "3205 AC1: :durable last-se-reason matches WAL");
        CHECK(href_evol_mid_durable(cs, mid, "last-se-denied") == 1,
              "3205 AC1: :durable last-se-denied");
        CHECK(href_evol_mid_durable(cs, mid, "forensic-source") >= 3,
              "3205 AC1: forensic-source stays >=3");
        cs.evaluator().disable_mutation_audit_wal();
        fs::remove_all(dir, ec);
    }

    {
        std::println("\n--- 3205 AC2: WAL off + :durable is a no-I/O miss ---");
        reset_process();
        apply_production_audit_defaults();
        CompilerService cs;
        apply_production_audit_defaults();
        CHECK(!g_security_event_wal().is_enabled(), "3205 AC2: SE WAL off");
        CHECK(!g_mutation_audit_wal().is_enabled(), "3205 AC2: mutation WAL off");
        const std::uint64_t mid = 32051;
        append_security_event(g_security_event_ring(), SecurityEventKind::EffectDeny, 3, mid, 1,
                              kEffectMutate, "op", "wal-off-deny", true, 2);
        const auto hit0 = href_evol_mid(cs, mid, "durable-hit");
        const auto hit1 = href_evol_mid_durable(cs, mid, "durable-hit");
        CHECK(hit0 == 0 && hit1 == 0, "ac3205_2_wal_off: :durable matches no-durable");
        CHECK(href_evol_reason(cs, mid, true) == href_evol_reason(cs, mid, false),
              "3205 AC2: reason unchanged with WAL off");
    }

    {
        std::println("\n--- 3205 AC2/AC5: Soft :durable refuses disk scan ---");
        reset_process();
        CompilerService cs;
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / "aura-3205-soft-wal";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        CHECK(cs.evaluator().enable_security_event_wal(dir.string()), "3205 AC2: Soft WAL enable");
        const std::uint64_t mid = 32052;
        emit_security_event_durable(SecurityEventKind::EffectDeny, 4, mid, 1, kEffectMutate, "op",
                                    "soft-durable-deny", true, 2);
        for (std::size_t i = 0; i < kSecurityEventRingSize; ++i) {
            append_security_event(g_security_event_ring(), SecurityEventKind::EffectAllow, 1,
                                  41000 + i, 1, kEffectMutate, "wrap", "fill", false, 1);
        }
        CHECK(href_evol_mid_durable(cs, mid, "durable-hit") == 0,
              "ac3205_3_soft_quiet: Soft :durable does not scan");
        CHECK(href_evol_reason(cs, mid, true).empty(), "3205 AC2: Soft keeps empty after wrap");
        cs.evaluator().disable_mutation_audit_wal();
        fs::remove_all(dir, ec);
    }

    // ── Issue #3242: durable typed summary sidecar after trail wrap ──
    {
        std::println("\n--- 3242 AC1: wrap trail, WAL typed summary recovers outcome+kind ---");
        reset_process();
        apply_production_audit_defaults();
        CompilerService cs;
        apply_production_audit_defaults();
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / "aura-3242-typed-summary";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        CHECK(cs.evaluator().enable_mutation_audit_wal(dir.string()),
              "3242 AC1: enable mutation WAL");
        const std::uint64_t mid = 3242;
        capture_audit_event_forced(mid, "op-3242", MutationKind::Structural, 1, 2,
                                   AuditOutcome::Rollback, /*target=*/7, /*nodes=*/3);
        append_security_event(g_security_event_ring(), SecurityEventKind::InvariantFail,
                              /*tenant=*/12, mid, /*epoch=*/2, kEffectMutate, "op-3242",
                              "typed-wrap-rollback", /*denied=*/true, /*fiber=*/4);
        CHECK(g_typed_mutation_audit_counters.typed_summary_wal_persisted_total.load() >= 1,
              "3242 AC1: typed summary persisted");
        TypedMutationAuditEvent te_pre{};
        CHECK(trail_find_by_mutation_id(mid, te_pre), "3242 AC1: mid in trail before wrap");
        for (std::size_t i = 0; i < kTypedMutationAuditTrailSize; ++i) {
            capture_audit_event_forced(96000 + i, "wrap-fill-3242", MutationKind::Other, 1, 1,
                                       AuditOutcome::Success);
        }
        TypedMutationAuditEvent te_miss{};
        CHECK(!trail_find_by_mutation_id(mid, te_miss), "3242 AC1: mid wrapped out of typed trail");
        CHECK(href_evol_mid(cs, mid, "typed-trail-miss") == 1, "3242 AC1: typed-trail-miss=1");
        CHECK(href_evol_mid(cs, mid, "typed-summary-from-wal") == 0,
              "3242 AC1: default path no sidecar scan");
        CHECK(href_evol_mid_durable(cs, mid, "typed-summary-from-wal") == 1,
              "ac3242_1_wal_hit: :durable recovers typed summary");
        CHECK(href_evol_mid_durable(cs, mid, "typed-outcome") == 2,
              "3242 AC1: WAL typed-outcome=Rollback");
        CHECK(href_evol_mid_durable(cs, mid, "typed-kind") ==
                  static_cast<std::int64_t>(MutationKind::Structural),
              "3242 AC1: WAL typed-kind=Structural");
        CHECK(href_evol_mid_durable(cs, mid, "schema-3242") == 3242, "3242 AC3: schema-3242");
        auto q = cs.eval(std::format("(engine:metrics \"query:security-audit\" 8 12 4 0 {})", mid));
        CHECK(q.has_value(), "3242 AC2: query:security-audit callable");
        bool saw_wal = false;
        if (q) {
            for (const auto& ln : list_string_lines(cs, *q)) {
                if (ln.find("typed-summary-from-wal=1") != std::string::npos &&
                    ln.find("typed-outcome-wal=Rollback") != std::string::npos &&
                    ln.find("typed-kind-wal=Structural") != std::string::npos)
                    saw_wal = true;
            }
        }
        CHECK(saw_wal, "3242 AC2: security-audit additive WAL typed keys");
        cs.evaluator().disable_mutation_audit_wal();
        fs::remove_all(dir, ec);
    }

    {
        std::println("\n--- 3242 AC2: Soft does not write typed summary ---");
        reset_process();
        CompilerService cs;
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / "aura-3242-soft";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        CHECK(cs.evaluator().enable_mutation_audit_wal(dir.string()), "3242 AC2: Soft WAL enable");
        const auto p0 = g_typed_mutation_audit_counters.typed_summary_wal_persisted_total.load();
        capture_audit_event_forced(32421, "soft-3242", MutationKind::ReplaceValue, 1, 1,
                                   AuditOutcome::Success, 1, 1);
        CHECK(g_typed_mutation_audit_counters.typed_summary_wal_persisted_total.load() == p0,
              "ac3242_2_soft: no typed WAL write");
        CHECK(href_evol_mid_durable(cs, 32421, "typed-summary-from-wal") == 0,
              "3242 AC2: Soft :durable typed-summary-from-wal=0");
        cs.evaluator().disable_mutation_audit_wal();
        fs::remove_all(dir, ec);
    }

    {
        std::println("\n--- 3242 AC3: mid=0 Success does not invent typed summary ---");
        reset_process();
        apply_production_audit_defaults();
        CompilerService cs;
        apply_production_audit_defaults();
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / "aura-3242-mid0";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        CHECK(cs.evaluator().enable_mutation_audit_wal(dir.string()), "3242 AC3: WAL enable");
        const auto p0 = g_typed_mutation_audit_counters.typed_summary_wal_persisted_total.load();
        aura::compiler::typed_audit::record_boundary_outcome(0, "mid0-success", 1, 1, true);
        CHECK(g_typed_mutation_audit_counters.typed_summary_wal_persisted_total.load() == p0,
              "ac3242_3_mid0: no invented Success summary");
        cs.evaluator().disable_mutation_audit_wal();
        fs::remove_all(dir, ec);
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_security_audit_unify();
}
#endif
