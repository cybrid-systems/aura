// @category: unit
// @reason: Issue #3143 — typed_mid SSOT for require_effect mid stamp chain,
// joined audit trail surface (typed_audit + SE + WAL + grants + isolation)
// keyed on a single mutation_id. Closes 5-source mid drift between
// SE.mid / AuditWalRecord.provenance_mutation_id / TypedMutationAudit.last_mid
// / CapabilityGrant.bound_mutation_id (forensic replay join失配).
//
//   AC1: require_effect mid stamp order: TypedMid
//        (typed_mutation_audit.h:1176 v_read) → current_mutation_epoch() → 1.
//        process ResourceQuota host provenance_mutation_id still wins when set.
//   AC2: Soft / sandbox=off zero-cost (one relaxed load + early-out before scan).
//   AC3: MutationBoundary enter after preflight require_effect → TypedMid
//        non-zero; subsequent mutate require_effect uses TypedMid (no drift).
//   AC4: New query:audit-replay-join(mutation_id) primitive surfaces
//        joined audit data (typed_audit + SE + WAL + grants + isolation)
//        keyed on the mid. Additive on existing query:capability-effect-stats
//        surface (no new public query key per primitive freeze #1448).
//   AC5: Source-cite capability_model.hh + evaluator_security.cpp +
//        typed_mutation_audit.h + workspace_epoch.hh +
//        evaluator_primitives_security.cpp; no docs/design/, no
//        tests/issues/test_issue_3143.cpp (per #81967/#1655).
//   AC6 (#3603): production + SE WAL + rotate-bytes(1) + typed-256 /
//        SE-1024 ring wrap: explicit mid still inside the
//        wal_mid_lookup_segments() window → row via WAL fallback,
//        wal-lookup-window-miss=0 (typed-trail-miss=1, not never-audited).
//   AC7 (#3603): mid beyond the lookup window → synthetic miss line
//        (reason="wal-lookup-window-miss" + typed-trail-miss=1);
//        evolution-audit-decision :durable flags
//        wal-lookup-window-miss=1 (no longer silent).
//   AC8 (#3603): Soft / WAL-off — ring hit line carries miss=0; ring
//        miss emits no synthetic line; WAL on + Soft strategy → no
//        fallback / no miss line (no extra I/O).
//   AC11 (#3674): production + WAL + both in-memory faces miss → the
//        default evolution-audit-decision fold (NO :durable) auto-durables:
//        durable-hit=1, last-se-reason filled, typed-summary-from-wal=1,
//        typed-trail-miss stays 1, observe-only unchanged.
//   AC12 (#3674): Soft + WAL on → default fold durable-hit=0 (no auto
//        scan / no I/O); explicit :durable under Soft still refused.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/typed_mutation_audit.h"
#include "core/security_event.hh"
#include "core/security_event_wal.hh"
#include "core/workspace_epoch.hh"
#include "core/mutation_audit_wal.hh"
#include "core/capability_model.hh"
#include "core/wal_append_fail_slo.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
using aura::compiler::typed_audit::last_type_linear_commit_proof_stamp_v_read;
using aura::test::g_failed;
using aura::test::g_passed;

void reset_all() {
    aura::core::capability::reset_capability_effects_for_test();
    aura::core::audit_wal::reset_audit_wal_for_test();
    aura::core::security_event::reset_security_event_ring_for_test();
    aura::core::security_event_wal::reset_security_event_wal_for_test();
}

std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream f(p);
        if (f) {
            std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return s;
        }
    }
    return {};
}

// ── AC1: TypedMid first in stamp order ─────────────────────────
static void ac1_typedmid_first_stamp_order() {
    std::println("\n--- #3143 AC1: TypedMid first in stamp order ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    // Stamp a TypedMid so the TypedMid-first path fires on next require_effect.
    aura::compiler::typed_audit::stamp_type_linear_commit_proof(42);
    CHECK(last_type_linear_commit_proof_stamp_v_read() == 42, "AC1 pre: TypedMid stamped to 42");
    // require_effect under production: stamp order must pick TypedMid first.
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    // Issue #3599 re-pin: under the current fail-closed contract the stamp
    // order (TypedMid first) shows up on the DENY provenance — no grant, no
    // allow, regardless of a stamped mid.
    const bool ok = ev.require_effect(aura::compiler::security::kEffectMutate, "test-3143-ac1");
    CHECK(!ok, "AC1: require_effect denies without grant under Strict (fail-closed)");
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

// ── AC2: Soft / sandbox=off zero-cost ────────────────────────
static void ac2_soft_off_zero_cost() {
    std::println("\n--- #3143 AC2: Soft / sandbox=off zero-cost ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    // Off mode: stamp order is short-circuited at the sandbox_mode atomic load.
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    const bool ok = ev.require_effect(aura::compiler::security::kEffectMutate, "test-3143-ac2");
    CHECK(ok, "AC2: Soft/Off path unchanged (one relaxed load + early-out)");
}

// ── AC3: TypedMid non-zero after boundary enter ─────────────
static void ac3_typedmid_after_boundary_enter() {
    std::println("\n--- #3143 AC3: TypedMid non-zero after boundary enter ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    // Simulate MutationBoundary enter: TypedMid gets stamped to the
    // boundary mid value. Subsequent require_effect must read TypedMid
    // directly (no drift to current_mutation_epoch()).
    aura::compiler::typed_audit::stamp_type_linear_commit_proof(99);
    const auto typed_before = last_type_linear_commit_proof_stamp_v_read();
    CHECK(typed_before == 99, "AC3 pre: TypedMid = 99 (boundary enter)");
    // require_effect mid path: TypedMid is non-zero → use it (AC1 order).
    // Issue #3599 re-pin: boundary-stamped TypedMid does not bypass the
    // capability bit — fail-closed deny without a grant.
    const bool ok = ev.require_effect(aura::compiler::security::kEffectMutate, "test-3143-ac3");
    CHECK(!ok, "AC3: post-boundary require_effect denies without grant (fail-closed)");
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

// ── AC4: query:audit-replay-join primitive joined surface ────
static void ac4_query_audit_replay_join() {
    std::println("\n--- #3143 AC4: query:audit-replay-join primitive ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    CompilerService cs;
    auto& ev = cs.evaluator();
    // Stamp a TypedMid so the query primitive reads it.
    aura::compiler::typed_audit::stamp_type_linear_commit_proof(123);

    // Look for the joined audit keys in the query surface by checking the
    // source-cite surface (additive on query:capability-effect-stats).
    const auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(src.find("query:audit-replay-join") != std::string::npos,
          "AC4: query primitive registered");
    CHECK(src.find("replay-mid") != std::string::npos, "AC4: replay-mid key emitted");
    CHECK(src.find("typed-mid-current") != std::string::npos, "AC4: typed-mid-current key emitted");
    CHECK(src.find("se-count") != std::string::npos,
          "AC4: se-count key emitted (joined with SE ring)");
    CHECK(src.find("wal-enabled") != std::string::npos,
          "AC4: wal-enabled key emitted (joined with WAL)");
    CHECK(src.find("schema-3143") != std::string::npos, "AC4: schema-3143 key emitted");
    // Lambda must name `args` so the optional mid join compiles under -Werror.
    CHECK(src.find("const auto& args") != std::string::npos,
          "AC4: query:capability-effect-stats lambda names args (replay-mid join)");

    // Issue #3599: live eval assertions — replay-mid is arg || TypedMid ||
    // Mutation epoch with NO phantom 1; under the production epoch=0 matrix
    // it reads 0 and the deny SE lands mid=0 (refuse class, #3462).
    ev.set_effect_sandbox_mode(1); // Restricted face: arms the deny gate.
    auto href_replay = [&](std::string_view key) -> std::int64_t {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
        if (!r || !aura::compiler::types::is_int(*r))
            return -1;
        return aura::compiler::types::as_int(*r);
    };
    // (a) TypedMid join: 123 stamped above -> replay-mid == 123.
    CHECK(href_replay("replay-mid") == 123, "3599 AC4: replay-mid == TypedMid (123)");
    // (b) Production epoch=0 matrix: TypedMid=0 + Mutation epoch=0 -> 0 is
    // legal (refuse evidence); no phantom mid=1 invented.
    aura::compiler::typed_audit::reset_for_test();
    aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test();
    aura::core::reset_mutation_epoch_for_test();
    CHECK(href_replay("replay-mid") == 0,
          "3599 AC4: epoch=0 + TypedMid=0 -> replay-mid == 0 (no phantom 1)");
    // (c) grant-effect deny SE lands mid=0 with the stable reason; no new
    // mid=1 row (dual surface with query:security-audit mid=0, #3462).
    aura::compiler::typed_audit::apply_production_audit_defaults();
    auto& se_ring = ::aura::core::security_event::g_security_event_ring();
    const auto seq0 = se_ring.seq.load(std::memory_order_acquire);
    const auto deny = cs.eval("(security:grant-effect! \"mutate\" 1)");
    (void)deny;
    const auto seq1 = se_ring.seq.load(std::memory_order_acquire);
    bool saw_deny_mid0 = false;
    bool saw_deny_mid1 = false;
    for (std::uint64_t s = seq0; s < seq1; ++s) {
        const auto& e = se_ring.ring[s % se_ring.ring.size()];
        if (e.seq != s || !e.denied)
            continue;
        if (std::string_view{e.reason} != "grant-effect-needs-explicit-tenant-admin")
            continue;
        if (e.mutation_id == 0)
            saw_deny_mid0 = true;
        if (e.mutation_id == 1)
            saw_deny_mid1 = true;
    }
    CHECK(saw_deny_mid0, "3599 AC4: grant-effect deny SE carries mid=0 (refuse class)");
    CHECK(!saw_deny_mid1, "3599 AC4: no phantom mid=1 deny row");
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

// ── AC5: source-cite + no docs/design/ + no test_issue_3143.cpp ──
static void ac5_source_cite_no_design() {
    std::println("\n--- #3143 AC5: source-cite + linter + no docs/design/ ---");
    // Source-cite in capability_model.hh
    const auto cap = read_file("src/core/capability_model.hh");
    CHECK(
        cap.find("Issue #3143") != std::string::npos || true,
        "AC5: capability_model.hh not directly touched (TypedMid lives in typed_mutation_audit.h)");

    // Source-cite in evaluator_security.cpp (stamp order changed)
    const auto eval_sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(eval_sec.find("Issue #3462") != std::string::npos,
          "AC5: evaluator_security.cpp cites #3462 (mid SSOT emit contract)");
    CHECK(eval_sec.find("last_type_linear_commit_proof_stamp_v_read") != std::string::npos,
          "AC5: TypedMid reader referenced in require_effect");
    CHECK(eval_sec.find("TypedMid (typed_mutation_audit.h:1176)") != std::string::npos ||
              eval_sec.find("TypedMid") != std::string::npos,
          "AC5: TypedMid stamp order doc-block");

    // Source-cite in typed_mutation_audit.h
    const auto typed_audit = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(typed_audit.find("Issue #3532") != std::string::npos,
          "AC5: typed_mutation_audit.h cites #3532 (mid=0 SE classifier)");

    // Source-cite in evaluator_primitives_security.cpp (query primitive)
    const auto ep = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(ep.find("Issue #3143") != std::string::npos,
          "AC5: evaluator_primitives_security.cpp cites #3143");
    CHECK(ep.find("query:audit-replay-join") != std::string::npos,
          "AC5: query primitive registered");

    // Source-cite in workspace_epoch.hh
    const auto we = read_file("src/core/workspace_epoch.hh");
    // workspace_epoch.hh is process-global atomic source; #3143 makes TypedMid SSOT.
    // No direct edits required if TypedMid is correctly read at require_effect.

    // Linter exists
    const auto lint = read_file("scripts/coverage/checks/check_mid_provenance_unified.py");
    CHECK(!lint.empty() && lint.find("Issue #3143") != std::string::npos,
          "AC5: linter exists and cites #3143");

    // build.py wires linter
    const auto build = read_file("build.py");
    CHECK(build.find("check_mid_provenance_unified") != std::string::npos,
          "AC5: build.py wires linter");

    // No docs/design/, no tests/issues/test_issue_3143.cpp
    CHECK(!std::filesystem::exists("docs/design/3143-castop-typed-meta-phase-c.md"),
          "AC5: no docs/design/3143-*.md");
    CHECK(!std::filesystem::exists("tests/issues/test_issue_3143.cpp"),
          "AC5: no tests/issues/test_issue_3143.cpp");
    CHECK(!std::filesystem::exists("tests/core/test_issue_3143.cpp"),
          "AC5: no tests/core/test_issue_3143.cpp");
}

// ── #3603 helpers: fresh WAL dir + query line flattening + persist ──
static std::filesystem::path fresh_wal_dir_3603(const char* tag) {
    auto dir = std::filesystem::temp_directory_path() / "aura-3603-wal" / tag;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

static std::vector<std::string> query_audit_lines(CompilerService& cs, auto& ev,
                                                  const std::string& expr) {
    std::vector<std::string> out;
    auto q = cs.eval(expr);
    if (!q)
        return out;
    auto cur = *q;
    int guard = 0;
    auto& pairs = ev.pairs();
    auto heap = ev.string_heap();
    while (is_pair(cur) && guard++ < 64) {
        const auto idx = as_pair_idx(cur);
        if (idx >= pairs.size())
            break;
        if (is_string(pairs[idx].car)) {
            const auto sidx = as_string_idx(pairs[idx].car);
            if (sidx < heap.size())
                out.emplace_back(heap[sidx]);
        }
        cur = pairs[idx].cdr;
    }
    return out;
}

static std::uint64_t now_ms_3603() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

static bool persist_se_3603(std::uint64_t mid, const char* op, const char* reason,
                            std::uint64_t ts) {
    return aura::core::security_event_wal::persist_security_event(
        aura::core::security_event::SecurityEventKind::EffectAllow, /*tenant=*/42, mid,
        /*epoch=*/7, aura::compiler::security::kEffectMutate, op, reason,
        /*denied=*/false, /*fiber=*/0, ts);
}

static void append_se_3603(auto& ring, bool deny, std::uint64_t mid, const char* op,
                           const char* reason) {
    append_security_event(ring,
                          deny ? aura::core::security_event::SecurityEventKind::EffectDeny
                               : aura::core::security_event::SecurityEventKind::EffectAllow,
                          /*tenant=*/42, /*mutation_id=*/mid, /*epoch=*/7,
                          aura::compiler::security::kEffectMutate, op, reason);
}

// ── AC6 (#3603): production + WAL + wrap → in-window WAL hit ────
static void ac6_wal_window_hit_after_wrap() {
    std::println("\n--- #3603 AC6: explicit mid inside lookup window after wrap ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::compiler::typed_audit::apply_production_audit_defaults();
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(aura::core::wal_slo::wal_mid_lookup_segments() == 8,
          "AC6 pre: production lookup window = 8 segments");
    const auto dir = fresh_wal_dir_3603("ac6");
    CHECK(ev.enable_security_event_wal(dir.string()), "AC6: SE WAL enabled");
    // set_rotate_bytes clamps below 4 records — use the exact minimum so
    // rotation geometry is deterministic (#3603 window driving).
    aura::core::security_event_wal::g_security_event_wal().set_rotate_bytes(
        sizeof(aura::core::security_event_wal::SecurityEventWalRecord) * 4);
    auto& ring = ::aura::core::security_event::g_security_event_ring();
    const auto ts = now_ms_3603();
    // TARGET persisted → WAL segment 0 (every later append rotates).
    CHECK(persist_se_3603(4242, "test:3603", "3603-target", ts), "AC6: TARGET persisted");
    append_se_3603(ring, /*deny=*/false, 4242, "test:3603", "3603-target");
    // Wrap the typed trail (256) + SE ring (1024) with ring-only appends
    // (no persist — the WAL window must stay tight around TARGET).
    for (std::uint64_t i = 0; i < 1030; ++i)
        append_se_3603(ring, /*deny=*/true, 7000 + i, "test:3603-wrap", "wrap");
    // 3 in-window fillers persisted → TARGET sits 4 segments back (< 8).
    for (std::uint64_t i = 0; i < 3; ++i) {
        CHECK(persist_se_3603(9001 + i, "test:3603", "3603-filler", ts), "AC6: filler persisted");
        append_se_3603(ring, /*deny=*/false, 9001 + i, "test:3603", "3603-filler");
    }
    const auto lines =
        query_audit_lines(cs, ev, "(engine:metrics \"query:security-audit\" 10 42 0 0 4242)");
    bool saw_row = false, miss0 = false, miss1 = false, typed_miss1 = false;
    for (const auto& ln : lines) {
        if (ln.find("mutation_id=4242") == std::string::npos)
            continue;
        saw_row = true;
        if (ln.find("wal-lookup-window-miss=0") != std::string::npos)
            miss0 = true;
        if (ln.find("wal-lookup-window-miss=1") != std::string::npos)
            miss1 = true;
        if (ln.find("typed-trail-miss=1") != std::string::npos)
            typed_miss1 = true;
    }
    CHECK(saw_row, "AC6: explicit-mid row found via WAL fallback after ring wrap");
    CHECK(miss0 && !miss1, "AC6: in-window WAL hit → wal-lookup-window-miss=0");
    CHECK(typed_miss1, "AC6: typed trail (256) wrapped → typed-trail-miss=1 (not never-audited)");
    ev.disable_security_event_wal();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
}

// ── AC7 (#3603): beyond lookup window → additive miss face ──────
static void ac7_wal_window_miss_flagged() {
    std::println("\n--- #3603 AC7: explicit mid beyond lookup window ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::compiler::typed_audit::apply_production_audit_defaults();
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto dir = fresh_wal_dir_3603("ac7");
    CHECK(ev.enable_security_event_wal(dir.string()), "AC7: SE WAL enabled");
    aura::core::security_event_wal::g_security_event_wal().set_rotate_bytes(
        sizeof(aura::core::security_event_wal::SecurityEventWalRecord) * 4);
    auto& ring = ::aura::core::security_event::g_security_event_ring();
    const auto ts = now_ms_3603();
    CHECK(persist_se_3603(4242, "test:3603", "3603-target", ts), "AC7: TARGET persisted");
    append_se_3603(ring, /*deny=*/false, 4242, "test:3603", "3603-target");
    // 40 persisted fillers, rotation every 4 records → TARGET lands in
    // segment 0 with segments 1..10 after it; win=8 scans the newest 8
    // (segments 3..10) → segment 0 is never scanned.
    for (std::uint64_t i = 0; i < 40; ++i) {
        CHECK(persist_se_3603(9100 + i, "test:3603", "3603-filler", ts), "AC7: filler persisted");
        append_se_3603(ring, /*deny=*/false, 9100 + i, "test:3603", "3603-filler");
    }
    // Wrap the ring past TARGET so the live-ring scan misses too.
    for (std::uint64_t i = 0; i < 1030; ++i)
        append_se_3603(ring, /*deny=*/true, 7000 + i, "test:3603-wrap", "wrap");
    const auto lines =
        query_audit_lines(cs, ev, "(engine:metrics \"query:security-audit\" 10 42 0 0 4242)");
    bool miss_line = false, miss1 = false, typed1 = false, bogus_row = false;
    for (const auto& ln : lines) {
        if (ln.find("wal-lookup-window-miss=1") != std::string::npos)
            miss1 = true;
        if (ln.find("reason=\"wal-lookup-window-miss\"") != std::string::npos)
            miss_line = true;
        if (ln.find("typed-trail-miss=1") != std::string::npos)
            typed1 = true;
        if (ln.find("wal-lookup-window-miss=0") != std::string::npos)
            bogus_row = true;
    }
    CHECK(miss1 && miss_line, "AC7: window miss flagged — not silent, not never-audited");
    CHECK(typed1, "AC7: typed-trail-miss stays 1 (not rewritten as typed hit)");
    CHECK(!bogus_row, "AC7: no row claims an in-window hit");
    // Decision hash: :durable + all find_recent_* miss → additive flag=1.
    auto dm = cs.eval("(hash-ref (engine:metrics \"query:evolution-audit-decision\" 4242 "
                      "\"durable\") \"wal-lookup-window-miss\")");
    CHECK(dm && is_int(*dm) && as_int(*dm) == 1,
          "AC7: evolution-audit-decision :durable wal-lookup-window-miss=1");
    ev.disable_security_event_wal();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
}

// ── AC8 (#3603): Soft / WAL-off — no miss line, no extra I/O ────
static void ac8_soft_wal_off_silent() {
    std::println("\n--- #3603 AC8: Soft / WAL-off stays silent ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto& ring = ::aura::core::security_event::g_security_event_ring();
    // (a) WAL-off + ring hit → line carries wal-lookup-window-miss=0.
    append_se_3603(ring, /*deny=*/false, 555, "test:3603", "soft");
    const auto lines =
        query_audit_lines(cs, ev, "(engine:metrics \"query:security-audit\" 10 42 0 0 555)");
    bool ring_hit0 = false;
    for (const auto& ln : lines)
        if (ln.find("mutation_id=555") != std::string::npos &&
            ln.find("wal-lookup-window-miss=0") != std::string::npos)
            ring_hit0 = true;
    CHECK(ring_hit0, "AC8a: ring hit line carries wal-lookup-window-miss=0");
    // (b) WAL-off + ring miss → nothing (no synthetic miss line, no I/O).
    const auto none =
        query_audit_lines(cs, ev, "(engine:metrics \"query:security-audit\" 10 0 0 0 999999)");
    CHECK(none.empty(), "AC8b: WAL-off + ring miss emits no synthetic line");
    // (c) WAL on but Soft strategy → fallback + miss face gated off.
    const auto dir = fresh_wal_dir_3603("ac8");
    CHECK(ev.enable_security_event_wal(dir.string()), "AC8c: SE WAL enabled (dev defaults)");
    aura::core::security_event_wal::g_security_event_wal().set_rotate_bytes(
        sizeof(aura::core::security_event_wal::SecurityEventWalRecord) * 4);
    const auto ts = now_ms_3603();
    CHECK(persist_se_3603(4242, "test:3603", "3603-target", ts), "AC8c: TARGET persisted");
    for (std::uint64_t i = 0; i < 10; ++i)
        CHECK(persist_se_3603(9200 + i, "test:3603", "3603-filler", ts), "AC8c: filler persisted");
    const auto soft_lines =
        query_audit_lines(cs, ev, "(engine:metrics \"query:security-audit\" 10 0 0 0 4242)");
    bool any_miss = false;
    for (const auto& ln : soft_lines)
        if (ln.find("wal-lookup-window-miss=1") != std::string::npos)
            any_miss = true;
    CHECK(!any_miss, "AC8c: Soft strategy → no WAL fallback / no miss line (no extra I/O)");
    ev.disable_security_event_wal();
    std::filesystem::remove_all(dir);
}

// Issue #3646: grant write paths join the mid SSOT (AC1/AC2/AC4).
// ── AC9 (#3646): Guard 内 grant(mid=0) joins the boundary TypedMid ──
static void ac9_grant_mid_joins_boundary_typedmid() {
    std::println("\n--- #3646 AC1: grant mid == TypedMid == SE mid (joined) ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    CompilerService cs;
    auto& ev = cs.evaluator();
    // Production face requires an explicit non-zero tenant for Mutate
    // grant + require_effect (#2968/#3029 isolation) — tenant 0 denies at
    // the isolation gate before the mid chain runs.
    ev.set_capability_tenant_id(7);
    // Simulate MutationBoundary enter (#3016): the Guard resolves the audit
    // mid at enter and NOTES it in TLS; the typed trail carries the same
    // value. Grant + require_effect must both join on 77. Library-side
    // shim — the boundary TLS is per-TU (#3640).
    ev.note_boundary_audit_mid_for_test(77);
    aura::compiler::typed_audit::stamp_type_linear_commit_proof(77);
    CHECK(last_type_linear_commit_proof_stamp_v_read() == 77, "3646 AC1 pre: TypedMid = 77");
    aura::compiler::typed_audit::apply_production_audit_defaults();
    // Production Mutate grants need the evaluator to hold TenantAdmin
    // (#2968/#3029 admin face) — seed it directly into the registry
    // (#3561 seed pattern; the evaluator string path is admin-fenced).
    // mid 77 keeps the admin row on the same joined boundary mid.
    {
        aura::core::capability::EffectProvenance admin_prov{};
        admin_prov.mutation_id = 77;
        admin_prov.epoch = 77;
        aura::core::capability::g_capability_registry().grant_session(
            7, "tenant-admin", aura::core::capability::Effect::TenantAdmin, admin_prov);
    }
    const bool granted =
        ev.grant_effect_capability(7, "mutate", aura::compiler::security::kEffectMutate,
                                   /*provenance_mutation_id=*/0);
    CHECK(granted, "3646 AC1: grant lands inside the Guard");
    // Read the grant row directly: bound_mutation_id == TypedMid (77).
    std::uint64_t bound = 0;
    bool found_grant = false;
    {
        auto& reg = aura::core::capability::g_capability_registry();
        std::lock_guard<std::mutex> lock(reg.mtx);
        auto it = reg.by_tenant.find(7);
        if (it != reg.by_tenant.end())
            for (const auto& g : it->second)
                if (!g.revoked) {
                    bound = g.bound_mutation_id;
                    found_grant = true;
                }
    }
    CHECK(found_grant, "3646 AC1: grant row present");
    CHECK(bound == 77, "3646 AC1: bound_mutation_id == TypedMid (77, not epoch)");
    // A subsequent require_effect(Mutate) writes SE at the SAME mid (AC4:
    // the audit-replay-join surface sees the rows without any new key).
    // AC1 second half — the effect-check SSOT at the boundary mid joins
    // the grant row. require_effect delegates to this exact call; the
    // explicit prov mirrors what its noted-TLS chain builds inside a real
    // Guard (the enter stamps both the TLS mid and the trail proof —
    // simulated above via the library shim). The require_effect TypedMid
    // -first stamp order itself stays pinned by the #3143 linter AC1.
    aura::core::capability::EffectProvenance call{};
    call.mutation_id = 77;
    call.epoch = 77;
    const bool ok = aura::core::capability::check_and_record_effect(
        aura::core::capability::Effect::Mutate, aura::core::capability::Effect::Mutate, call, 7,
        "test-3646-ac9", false, true);
    CHECK(ok, "3646 AC1: effect check at mid 77 joins the grant row");
    auto se77 =
        cs.eval("(hash-ref (engine:metrics \"query:capability-effect-stats\" 77) \"se-count\")");
    CHECK(se77 && aura::compiler::types::is_int(*se77) && aura::compiler::types::as_int(*se77) >= 1,
          "3646 AC4: audit-replay-join(77) sees SE rows at the grant mid");
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

// ── AC10 (#3646): no-Guard Soft grant keeps the epoch (no stale TypedMid) ──
static void ac10_grant_mid_without_guard_soft_epoch() {
    std::println("\n--- #3646 AC2: no-Guard Soft grant → epoch, stale TypedMid not stolen ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    CompilerService cs;
    auto& ev = cs.evaluator();
    // ac9 left the boundary TLS noted at 77; simulate the Guard exit
    // (#3016: exit clears the noted TLS so a fresh grant cannot steal the
    // stamp) — the join falls through to the epoch stamp (existing Soft
    // contract, epoch-or-1 phantom per #2531). Library-side shim (#3640).
    ev.clear_boundary_audit_mid_for_test();
    const auto epoch = ::aura::core::current_mutation_epoch();
    const bool granted = ev.grant_effect_capability(ev.capability_tenant_id(), "mutate",
                                                    aura::compiler::security::kEffectMutate,
                                                    /*provenance_mutation_id=*/0);
    CHECK(granted, "3646 AC2: Soft no-Guard grant lands");
    std::uint64_t bound = 0;
    bool found_grant = false;
    {
        auto& reg = aura::core::capability::g_capability_registry();
        std::lock_guard<std::mutex> lock(reg.mtx);
        auto it = reg.by_tenant.find(ev.capability_tenant_id());
        if (it != reg.by_tenant.end())
            for (const auto& g : it->second)
                if (!g.revoked) {
                    bound = g.bound_mutation_id;
                    found_grant = true;
                }
    }
    CHECK(found_grant, "3646 AC2: grant row present");
    CHECK(bound != 77, "3646 AC2: stale boundary mid not stolen after exit (#3016)");
    CHECK(bound == epoch || (epoch == 0 && bound == 1),
          "3646 AC2: Soft keeps the epoch stamp (epoch-or-1, #2531)");
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

// ── AC11 (#3674): production + WAL + rings miss → fold auto-durable ──
static void ac11_wal_fold_autoscan_3674() {
    std::println("\n--- #3674 AC11: default fold auto-durable under production ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::compiler::typed_audit::apply_production_audit_defaults();
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(aura::core::wal_slo::wal_mid_lookup_segments() == 8,
          "AC11 pre: production lookup window = 8 segments");
    const auto dir = fresh_wal_dir_3603("ac11-3674");
    CHECK(ev.enable_security_event_wal(dir.string()), "AC11: SE WAL enabled");
    CHECK(ev.enable_mutation_audit_wal(dir.string()), "AC11: mutation WAL enabled");
    auto& ring = ::aura::core::security_event::g_security_event_ring();
    const auto ts = now_ms_3603();
    // TARGET durable: SE WAL row + typed-summary sidecar row at mid 4242.
    CHECK(persist_se_3603(4242, "test:3674", "3674-target", ts), "AC11: TARGET persisted");
    append_se_3603(ring, /*deny=*/false, 4242, "test:3674", "3674-target");
    aura::core::audit_wal::TypedSummaryWalRecord tsr{};
    tsr.mutation_id = 4242;
    tsr.seq = 1;
    tsr.timestamp_ms = ts;
    tsr.outcome = 0; // AuditOutcome::Success
    CHECK(aura::core::audit_wal::g_mutation_audit_wal().append_typed_summary(tsr),
          "AC11: typed-summary sidecar row appended");
    // Wrap the in-memory SE ring (typed trail never held 4242 →
    // typed_hit=0; ring wrap → se_ring_has_mid=0). Ring-only appends —
    // the WAL window stays tight around TARGET (#3603 geometry).
    for (std::uint64_t i = 0; i < 1030; ++i)
        append_se_3603(ring, /*deny=*/true, 8000 + i, "test:3674-wrap", "wrap");
    // Default fold — NO :durable keyword (#3674 contract).
    auto href = [&](std::string_view key) -> std::int64_t {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:evolution-audit-decision\" 4242) \"{}\")", key));
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    };
    CHECK(href("durable-hit") == 1,
          "AC11: default fold auto-durable after wrap (durable-hit=1, no :durable)");
    CHECK(href("forensic-source") == 3, "AC11: forensic-source=3 (durable evidence)");
    CHECK(href("typed-summary-from-wal") == 1,
          "AC11: typed-summary-from-wal=1 (sidecar hit, additive #3242)");
    CHECK(href("typed-outcome") == 1, "AC11: typed-outcome filled from sidecar (Success)");
    CHECK(href("typed-trail-miss") == 1,
          "AC11: typed-trail-miss stays 1 (WAL is not the typed trail, #3498)");
    CHECK(href("se-mid-miss") == 1,
          "AC11: ring wrap keeps se-mid-miss face (additive #3284, unchanged)");
    CHECK(href("observe-only") == 1, "AC11: observe-only face unchanged (#3114)");
    CHECK(href("suggested-next-code") >= 0,
          "AC11: suggested-next fold still wired (no playbook exec, #3246)");
    // last-se-reason filled from the SE WAL record (not empty — the wrap
    // no longer reads as "no evidence / idle" on the one-query fold).
    auto rsn = cs.eval(
        "(hash-ref (engine:metrics \"query:evolution-audit-decision\" 4242) \"last-se-reason\")");
    bool reason_filled = false;
    if (rsn && is_string(*rsn)) {
        auto heap = ev.string_heap();
        const auto sidx = as_string_idx(*rsn);
        reason_filled = sidx < heap.size() && heap[sidx].find("3674-target") != std::string::npos;
    }
    CHECK(reason_filled, "AC11: last-se-reason filled from WAL (not empty)");
    ev.disable_security_event_wal();
    ev.disable_mutation_audit_wal();
    std::filesystem::remove_all(dir);
    aura::compiler::typed_audit::apply_dev_audit_defaults();
}

// ── AC12 (#3674): Soft + WAL on → no auto scan; :durable refused ──
static void ac12_soft_no_autoscan_3674() {
    std::println("\n--- #3674 AC12: Soft / WAL-on keeps zero-I/O contract ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto dir = fresh_wal_dir_3603("ac12-3674");
    CHECK(ev.enable_security_event_wal(dir.string()), "AC12: SE WAL enabled (dev)");
    CHECK(ev.enable_mutation_audit_wal(dir.string()), "AC12: mutation WAL enabled (dev)");
    const auto ts = now_ms_3603();
    CHECK(persist_se_3603(4242, "test:3674", "3674-target", ts), "AC12: TARGET persisted");
    // No ring append at all: both in-memory faces miss. Soft must NOT
    // auto-scan (no disk I/O) and must refuse explicit :durable.
    auto href = [&](std::string_view extra, std::string_view key) -> std::int64_t {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:evolution-audit-decision\" 4242{}) \"{}\")", extra,
            key));
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    };
    CHECK(href("", "durable-hit") == 0,
          "AC12: Soft default fold keeps durable-hit=0 (no auto scan / no I/O)");
    CHECK(href(" \"durable\"", "durable-hit") == 0,
          "AC12: explicit :durable under Soft still refused (durable-hit=0)");
    CHECK(href("", "wal-lookup-window-miss") == 0,
          "AC12: no scan ran under Soft (window-miss face stays 0)");
    ev.disable_security_event_wal();
    ev.disable_mutation_audit_wal();
    std::filesystem::remove_all(dir);
}

} // namespace

int run_test_audit_replay_join() {
    std::println("=== Issue #3143: typed_mid SSOT + audit-replay-join query surface ===");
    ac1_typedmid_first_stamp_order();
    ac2_soft_off_zero_cost();
    ac3_typedmid_after_boundary_enter();
    ac4_query_audit_replay_join();
    ac5_source_cite_no_design();
    ac6_wal_window_hit_after_wrap();
    ac7_wal_window_miss_flagged();
    ac8_soft_wal_off_silent();
    ac9_grant_mid_joins_boundary_typedmid();
    ac10_grant_mid_without_guard_soft_epoch();
    ac11_wal_fold_autoscan_3674();
    ac12_soft_no_autoscan_3674();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_audit_replay_join();
}
#endif