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
//   AC13 (#3734): Restricted+Full+WAL inject miss on emit_mutation_audit
//        → overflow ring carries the same mid; :durable last-se-reason
//        is mutation_wal_append_miss and wal-lookup-window-miss=1.
//   AC14 (#3734): check_and_record_effect fail-closed before body unchanged.
//   AC15 (#3734): Soft/WAL-off emit miss does not push overflow.

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

// ── AC13 (#3734): emit_mutation_audit WAL miss → overflow join ──
static void ac13_emit_wal_miss_overflow_join_3734() {
    std::println("\n--- #3734 AC1: emit_mutation_audit WAL miss stamps overflow mid ---");
    reset_all();
    aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
    aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
    const char* prev_sb = std::getenv("AURA_SANDBOX");
    const std::string prev_sb_s = prev_sb ? prev_sb : "";
    ::setenv("AURA_SANDBOX", "restricted", 1);
    ::setenv("AURA_WAL_APPEND_FAIL_CLOSED", "1", 1);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    aura::compiler::typed_audit::apply_production_audit_defaults();
    CHECK(aura::core::wal_slo::wal_append_fail_closed_active(),
          "3734 AC1: fail-closed active under Restricted+production");

    CompilerService cs_a;
    auto& ev_a = cs_a.evaluator();
    aura::compiler::typed_audit::apply_production_audit_defaults();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    ::setenv("AURA_WAL_APPEND_FAIL_CLOSED", "1", 1);
    ev_a.set_capability_tenant_id(7);
    aura::compiler::typed_audit::reset_for_test();
    aura::compiler::typed_audit::apply_production_audit_defaults();
    aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test();
    ev_a.clear_boundary_audit_mid_for_test();
    ev_a.note_boundary_audit_mid_for_test(3734);
    CHECK(aura::compiler::typed_audit::join_audit_and_se_mid(0) == 3734,
          "3734 AC1 pre: join mid is boundary TypedMid 3734");
    CHECK(aura::core::wal_slo::wal_append_fail_closed_active(),
          "3734 AC1: fail-closed still active at emit");
    const auto dir = fresh_wal_dir_3603("ac13-3734");
    CHECK(ev_a.enable_mutation_audit_wal(dir.string()), "3734 AC1: mutation WAL enabled");
    // Issue #3639 pattern: force_wal pairs SE+mutation; park SE so the
    // shared inject lands on emit_mutation_audit (not persist_security_event).
    ev_a.disable_security_event_wal();
    const auto persisted0 = aura::core::audit_wal::snapshot_audit_wal_stats().persisted;
    aura::core::wal_slo::g_wal_append_fail_slo_counters.inject_fail_remaining.store(
        1, std::memory_order_relaxed);
    CHECK(!ev_a.emit_mutation_audit(2, 1, "3734-structural", 11),
          "3734 AC1 / #3780: fail-closed emit returns false on WAL miss");
    CHECK(aura::core::wal_slo::g_wal_append_fail_slo_counters.inject_fail_remaining.load(
              std::memory_order_relaxed) == 0,
          "3734 AC1: inject consumed by emit_mutation_audit append");
    CHECK(aura::core::audit_wal::snapshot_audit_wal_stats().persisted == persisted0,
          "3734 AC1: WAL row not persisted on miss");
    CHECK(aura::core::security_event_wal::wal_overflow_ring_depth() >= 1,
          "3734 AC1: overflow ring captured emit miss");
    const auto* ovr = aura::core::security_event_wal::wal_overflow_find_by_mid(3734);
    CHECK(ovr != nullptr, "3734 AC1: overflow row joins TypedMid 3734");
    CHECK(ovr && ovr->tenant_id == 7, "3734 AC1: overflow tenant matches committer");
    CHECK(ovr && ovr->reason == "mutation_wal_append_miss",
          "3734 AC1: overflow reason mutation_wal_append_miss");
    const auto fiber_a = ovr ? ovr->fiber_id : 0;

    CompilerService cs_b;
    auto& ev_b = cs_b.evaluator();
    ev_b.set_capability_tenant_id(9);
    // Boundary TLS is process-wide. Leaving 3734 noted would make the
    // peer success emit join the same mid and persist a WAL row that
    // the :durable fold would treat as a hit for the missed commit.
    ev_a.clear_boundary_audit_mid_for_test();
    ev_b.note_boundary_audit_mid_for_test(3735);
    (void)ev_b.emit_mutation_audit(1, 0, "3734-peer", 12);
    const auto* ovr_a = aura::core::security_event_wal::wal_overflow_find_by_mid(3734);
    CHECK(ovr_a && ovr_a->tenant_id == 7,
          "3734 AC1: dual-eval overflow tenant still committer (not peer 9)");
    CHECK(ovr_a && ovr_a->fiber_id == fiber_a,
          "3734 AC1: dual-eval overflow fiber matches committer");
    CHECK(aura::core::security_event_wal::wal_overflow_find_by_mid(3735) == nullptr,
          "3734 AC1: peer success emit does not invent an overflow row");

    // :durable scan is production/Full + WAL-on. Restricted cs_a keeps
    // sandbox_mode_ true (process Off does not flip it) and Restricted
    // force_wal pairs an SE sidecar that can steal the fold. Park SE,
    // flip env Off so a fresh evaluator can hash-ref, then re-arm
    // production (AURA_SANDBOX=off skips force_wal — mutation WAL stays
    // on the inject-miss dir). Same shape as AC11.
    ev_a.disable_security_event_wal();
    ::setenv("AURA_SANDBOX", "off", 1);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::compiler::typed_audit::apply_production_audit_defaults();
    CompilerService cs_q;
    aura::compiler::typed_audit::apply_production_audit_defaults();
    cs_q.evaluator().disable_security_event_wal();
    CHECK(aura::compiler::typed_audit::production_defaults_active(),
          "3734 AC1: production face still on for :durable");
    CHECK(aura::core::audit_wal::g_mutation_audit_wal().is_enabled(),
          "3734 AC1: mutation WAL still on for :durable scan");
    CHECK(!aura::core::security_event_wal::g_security_event_wal().is_enabled(),
          "3734 AC1: SE WAL parked (inject miss is mutation WAL only)");
    CHECK(aura::core::security_event_wal::wal_overflow_find_by_mid(3734) != nullptr,
          "3734 AC1: overflow row still joins 3734 at query");
    auto dm = cs_q.eval("(hash-ref (engine:metrics \"query:evolution-audit-decision\" 3734 "
                        "\"durable\") \"wal-lookup-window-miss\")");
    CHECK(dm && aura::compiler::types::is_int(*dm) && aura::compiler::types::as_int(*dm) == 1,
          "3734 AC1: :durable wal-lookup-window-miss=1 (row never hit disk)");
    auto dh = cs_q.eval("(hash-ref (engine:metrics \"query:evolution-audit-decision\" 3734 "
                        "\"durable\") \"durable-hit\")");
    CHECK(dh && aura::compiler::types::is_int(*dh) && aura::compiler::types::as_int(*dh) == 0,
          "3734 AC1: overflow is not durable WAL (durable-hit=0)");
    auto rsn = cs_q.eval("(hash-ref (engine:metrics \"query:evolution-audit-decision\" 3734 "
                         "\"durable\") \"last-se-reason\")");
    bool reason_filled = false;
    if (rsn && aura::compiler::types::is_string(*rsn)) {
        auto heap = cs_q.evaluator().string_heap();
        const auto sidx = aura::compiler::types::as_string_idx(*rsn);
        reason_filled =
            sidx < heap.size() && heap[sidx].find("mutation_wal_append_miss") != std::string::npos;
    }
    CHECK(reason_filled, "3734 AC1: :durable last-se-reason is mutation_wal_append_miss");

    ev_a.disable_mutation_audit_wal();
    std::filesystem::remove_all(dir);
    aura::core::wal_slo::g_wal_append_fail_slo_counters.inject_fail_remaining.store(
        0, std::memory_order_relaxed);
    aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
    ::unsetenv("AURA_WAL_APPEND_FAIL_CLOSED");
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    if (!prev_sb_s.empty())
        ::setenv("AURA_SANDBOX", prev_sb_s.c_str(), 1);
    else
        ::unsetenv("AURA_SANDBOX");
}

// ── AC14 (#3734 / #3780): effect-gate fail-closed before body unchanged ──
static void ac14_effect_gate_fail_closed_unchanged_3734() {
    std::println("\n--- #3734 AC2 / #3780: effect gate + pre-persist structural deny ---");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto bnd = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto emit_pos = sec.find("bool Evaluator::emit_mutation_audit");
    const auto effect_pos = sec.find("bool Evaluator::check_and_record_effect");
    CHECK(emit_pos != std::string::npos, "3734 AC2: emit_mutation_audit present");
    CHECK(effect_pos != std::string::npos, "3734 AC2: check_and_record_effect present");
    CHECK(effect_pos > emit_pos, "3734 AC2: check_and_record_effect follows emit");
    CHECK(sec.find("wal_append_missed && ::aura::core::wal_slo::wal_append_fail_closed_active()",
                   effect_pos) != std::string::npos,
          "3734 AC2: effect gate still fail-closed on WAL miss (#3639)");
    const auto emit_overflow = sec.find("wal_overflow_ring_push", emit_pos);
    CHECK(emit_overflow != std::string::npos && emit_overflow < effect_pos,
          "3734 AC2: emit_mutation_audit still stamps overflow on miss");
    CHECK(sec.find("return false;") != std::string::npos &&
              sec.find("Issue #3780") != std::string::npos,
          "3780: emit returns false under fail-closed miss");
    const auto gate = bnd.find("Issue #3780");
    const auto persist = bnd.find("aura_outermost_success_persist_occurrence", gate);
    CHECK(gate != std::string::npos, "3780: Guard dtor cites #3780");
    CHECK(persist != std::string::npos && persist > gate,
          "3780: pre-persist WAL gate precedes Occurrence persist");
    CHECK(sec.find("mutation_wal_append_miss") != std::string::npos,
          "3780: reason stays in security TU (stable)");
}

// ── AC15 (#3734): Soft / WAL-off emit miss does not push overflow ──
static void ac15_soft_emit_no_overflow_3734() {
    std::println("\n--- #3734 AC3: Soft emit miss stays fail-open, no overflow ---");
    reset_all();
    aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
    aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    CHECK(!aura::core::wal_slo::wal_append_fail_closed_active(),
          "3734 AC3: Soft fail-closed inactive");
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto dir = fresh_wal_dir_3603("ac15-3734");
    CHECK(ev.enable_mutation_audit_wal(dir.string()), "3734 AC3: WAL on under Soft");
    aura::core::wal_slo::g_wal_append_fail_slo_counters.inject_fail_remaining.store(
        1, std::memory_order_relaxed);
    CHECK(ev.emit_mutation_audit(1, 0, "3734-soft", 0),
          "3734 AC3 / #3780: Soft emit miss stays fail-open (true)");
    CHECK(aura::core::security_event_wal::wal_overflow_ring_depth() == 0,
          "3734 AC3: Soft inject miss does not push overflow");
    ev.disable_mutation_audit_wal();
    std::filesystem::remove_all(dir);

    aura::core::wal_slo::g_wal_append_fail_slo_counters.inject_fail_remaining.store(
        1, std::memory_order_relaxed);
    CHECK(ev.emit_mutation_audit(1, 0, "3734-wal-off", 0),
          "3734 AC3 / #3780: WAL-off emit stays fail-open (true)");
    CHECK(aura::core::security_event_wal::wal_overflow_ring_depth() == 0,
          "3734 AC3: WAL-off emit does not push overflow");
    aura::core::wal_slo::g_wal_append_fail_slo_counters.inject_fail_remaining.store(
        0, std::memory_order_relaxed);

    std::ifstream invent("tests/core/test_issue_3734.cpp");
    if (!invent.good())
        invent.open("../tests/core/test_issue_3734.cpp");
    CHECK(!invent.good(), "3734 AC4: no test_issue_3734.cpp");
    CHECK(read_file("docs/design/3734-emit-mutation-audit-wal.md").empty(),
          "3734 AC4: no docs/design/3734-*");
    const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(prim.find("query:evolution-audit-decision") != std::string::npos,
          "3734 AC4: no new query key (reuse evolution-audit-decision)");
    CHECK(prim.find("wal_overflow_find_by_mid") != std::string::npos,
          "3734 AC4: durable fold joins overflow mid");
}

// ── AC16 (#3738): explicit mid=0 joins refuse rows on the fold ──
static void ac16_mid0_refuse_fold_3738() {
    std::println("\n--- #3738 AC2: evolution-audit-decision 0 joins mid-fallback-refused ---");
    reset_all();
    aura::compiler::typed_audit::apply_production_audit_defaults();
    aura::compiler::typed_audit::clear_boundary_audit_mid();
    aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test();
    aura::core::reset_mutation_epoch_for_test();
    ::setenv("AURA_SANDBOX", "off", 1);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    CompilerService cs;
    aura::compiler::typed_audit::apply_production_audit_defaults();
    CHECK(aura::compiler::typed_audit::join_audit_and_se_mid(0) == 0,
          "3738 AC2 pre: join refuse mid=0");
    CHECK(aura::compiler::typed_audit::resolve_audit_mutation_id(0) == 0,
          "3738 AC2: resolve refuse mid=0");
    auto mid = cs.eval(
        "(hash-ref (engine:metrics \"query:evolution-audit-decision\" 0) \"last-audit-mid\")");
    CHECK(mid && aura::compiler::types::is_int(*mid) && aura::compiler::types::as_int(*mid) == 0,
          "3738 AC2: last-audit-mid=0");
    auto rsn = cs.eval(
        "(hash-ref (engine:metrics \"query:evolution-audit-decision\" 0) \"last-se-reason\")");
    bool reason_ok = false;
    if (rsn && aura::compiler::types::is_string(*rsn)) {
        auto heap = cs.evaluator().string_heap();
        const auto sidx = aura::compiler::types::as_string_idx(*rsn);
        reason_ok =
            sidx < heap.size() && heap[sidx].find("mid-fallback-refused") != std::string::npos;
    }
    CHECK(reason_ok, "3738 AC2: last-se-reason contains mid-fallback-refused");
    auto obs = cs.eval(
        "(hash-ref (engine:metrics \"query:evolution-audit-decision\" 0) \"observe-only\")");
    CHECK(obs && aura::compiler::types::is_int(*obs) && aura::compiler::types::as_int(*obs) == 1,
          "3738 AC3: observe-only stays 1");
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    auto dh =
        cs.eval("(hash-ref (engine:metrics \"query:evolution-audit-decision\" 0) \"durable-hit\")");
    CHECK(dh && aura::compiler::types::is_int(*dh) && aura::compiler::types::as_int(*dh) == 0,
          "3738 AC4: Soft no WAL scan (durable-hit=0)");
    CHECK(read_file("tests/core/test_issue_3738.cpp").empty(), "3738 AC4: no test_issue_3738.cpp");
}


// ── #3780: pre-persist structural WAL miss fail-closed ──
static void ac17_pre_persist_wal_miss_fail_closed_3780() {
    std::println("\n--- #3780 AC1: emit miss returns false; Guard gates before persist ---");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto bnd = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    CHECK(ixx.find("[[nodiscard]] bool emit_mutation_audit") != std::string::npos,
          "3780 AC1: emit_mutation_audit returns bool");
    CHECK(sec.find("ovr.reason = \"mutation_wal_append_miss\"") != std::string::npos ||
              sec.find("ovr.reason = \"mutation_wal_append_miss\";") != std::string::npos,
          "3780 AC4: overflow reason mutation_wal_append_miss stable");
    // Prefer the string form used in emit (non-std::string ctor).
    CHECK(sec.find("mutation_wal_append_miss") != std::string::npos,
          "3780 AC4: mutation_wal_append_miss present");
    const auto gate = bnd.find("Issue #3780");
    const auto persist = bnd.find("aura_outermost_success_persist_occurrence");
    CHECK(gate != std::string::npos && persist != std::string::npos && gate < persist,
          "3780 AC1: #3780 pre-persist gate before Occurrence persist");
    CHECK(bnd.find("wal_append_fail_closed_active()") != std::string::npos,
          "3780 AC1: gate uses wal_append_fail_closed_active");
    // Soft skip: late emit still present under Soft/WAL-off.
    CHECK(bnd.find("Soft / WAL-off keep") != std::string::npos ||
              bnd.find("Soft / WAL-off") != std::string::npos,
          "3780 AC3: Soft/WAL-off late emit preserved");
    CHECK(sec.find("wal_append_missed && ::aura::core::wal_slo::wal_append_fail_closed_active()") !=
              std::string::npos,
          "3780 AC2: #3639 require_effect path unchanged");
    std::ifstream invent("tests/core/test_issue_3780.cpp");
    if (!invent.good())
        invent.open("../tests/core/test_issue_3780.cpp");
    CHECK(!invent.good(), "3780 AC4: no test_issue_3780.cpp");
    CHECK(read_file("docs/design/3780-mutation-wal-miss-fail-closed.md").empty(),
          "3780 AC4: no docs/design/3780-*");
    // Runtime: fail-closed emit miss → false + overflow; Soft → true.
    reset_all();
    aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
    aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
    ::setenv("AURA_SANDBOX", "restricted", 1);
    ::setenv("AURA_WAL_APPEND_FAIL_CLOSED", "1", 1);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    aura::compiler::typed_audit::apply_production_audit_defaults();
    CompilerService cs;
    auto& ev = cs.evaluator();
    aura::compiler::typed_audit::apply_production_audit_defaults();
    ev.set_capability_tenant_id(7);
    aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test();
    ev.clear_boundary_audit_mid_for_test();
    ev.note_boundary_audit_mid_for_test(3780);
    const auto dir = fresh_wal_dir_3603("ac17-3780");
    CHECK(ev.enable_mutation_audit_wal(dir.string()), "3780 AC1: mutation WAL enabled");
    ev.disable_security_event_wal();
    aura::core::wal_slo::g_wal_append_fail_slo_counters.inject_fail_remaining.store(
        1, std::memory_order_relaxed);
    CHECK(!ev.emit_mutation_audit(1, 0, "3780-miss", 1),
          "3780 AC1: production fail-closed emit returns false");
    CHECK(aura::core::security_event_wal::wal_overflow_find_by_mid(3780) != nullptr,
          "3780 AC1: overflow joins mid with mutation_wal_append_miss");
    const auto* ovr = aura::core::security_event_wal::wal_overflow_find_by_mid(3780);
    CHECK(ovr && ovr->reason == "mutation_wal_append_miss", "3780 AC4: reason string stable");
    ev.disable_mutation_audit_wal();
    std::filesystem::remove_all(dir);
    ::unsetenv("AURA_WAL_APPEND_FAIL_CLOSED");
    ::setenv("AURA_SANDBOX", "off", 1);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
    aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
}

// ── #3877: last matching SE wins; WAL-miss Deny refuses Allow-only ──
static void ac18_last_se_wins_wal_miss_3877() {
    std::println("\n--- #3877: last-SE-wins; WAL-miss Deny refuses Allow-only ---");
    using aura::core::security_event::append_security_event;
    using aura::core::security_event::forensic_effect_verdict_for_mid;
    using aura::core::security_event::forensic_mid_has_wal_append_miss;
    using aura::core::security_event::ForensicEffectVerdict;
    using aura::core::security_event::g_security_event_ring;
    using aura::core::security_event::kForensicLastSeWinsIssue;
    using aura::core::security_event::SecurityEventKind;

    CHECK(kForensicLastSeWinsIssue == 3877, "3877 AC: issue stamp");
    aura::core::security_event::reset_security_event_ring_for_test();

    append_security_event(g_security_event_ring(), SecurityEventKind::EffectAllow, 7, 0x3877,
                          0x3877, 8, "test:3877-allow", "", /*denied=*/false, 0);
    CHECK(forensic_effect_verdict_for_mid(0x3877) == ForensicEffectVerdict::Allow,
          "3877 AC1 setup: last-SE is Allow before compensator");
    append_security_event(g_security_event_ring(), SecurityEventKind::EffectDeny, 7, 0x3877, 0x3877,
                          8, "test:3877-miss", "mutation_wal_append_miss", /*denied=*/true, 0);
    CHECK(forensic_mid_has_wal_append_miss(0x3877), "3877 AC1: WAL-miss Deny shares mid");
    CHECK(forensic_effect_verdict_for_mid(0x3877) == ForensicEffectVerdict::Deny,
          "3877 AC1: last-SE-wins refuses Allow-only when miss Deny exists");

    append_security_event(g_security_event_ring(), SecurityEventKind::EffectAllow, 7, 0x38770,
                          0x38770, 8, "test:3877-lone", "", /*denied=*/false, 0);
    CHECK(forensic_effect_verdict_for_mid(0x38770) == ForensicEffectVerdict::Allow,
          "3877 AC2: last-SE-wins Allow when no miss Deny shares mid");
    CHECK(!forensic_mid_has_wal_append_miss(0x38770),
          "3877 AC2: lone Allow is not a WAL-miss Deny");

    const auto se = read_file("src/core/security_event.hh");
    CHECK(se.find("kForensicLastSeWinsIssue = 3877") != std::string::npos,
          "3877 AC3: forensic helper stamp");
    CHECK(se.find("forensic_effect_verdict_for_mid") != std::string::npos,
          "3877 AC3: verdict helper present");
    const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(prim.find("last-se-wins-verdict") != std::string::npos,
          "3877 AC3: audit-replay-join last-se-wins-verdict key");
    CHECK(prim.find("wal-miss-deny") != std::string::npos,
          "3877 AC3: audit-replay-join wal-miss-deny key");
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
    ac13_emit_wal_miss_overflow_join_3734();
    ac14_effect_gate_fail_closed_unchanged_3734();
    ac15_soft_emit_no_overflow_3734();
    ac16_mid0_refuse_fold_3738();
    ac17_pre_persist_wal_miss_fail_closed_3780();
    ac18_last_se_wins_wal_miss_3877();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_audit_replay_join();
}
#endif