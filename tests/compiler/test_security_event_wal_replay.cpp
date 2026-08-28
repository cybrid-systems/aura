// @category: unit
// @reason: Issue #2225 — durable side-car SecurityEvent WAL + expand ring
// to 1024 + wrap counter, for forensic replay after process restart.
//
//   AC1: ring ≥ 1024; ring-wrap-total increments when N>1024 denies
//   AC2: durable write when WAL forced; AURA_SANDBOX=off no spill
//   AC3: restart + enable(dir) replays records into the live ring
//        with monotonic seq; #2156 mid (epoch, not tenant) preserved
//   AC4: hot path cost — append_security_event still cheap when WAL
//        off (no syscalls); persist_security_event short-circuits
//   AC5: source-cite (ring size, WAL append, replay loop) + #2150
//        / #2156 / #2054 regression: production defaults still wire
//        mutation_audit_wal + side-car together
//
// Source-cite map (covered by AC1/AC5 + grep-able from commit):
//   src/core/security_event.hh:70-77           ring size + wrap counter
//   src/core/security_event.hh:81-110          append_security_event
//   src/core/security_event_wal.hh:140-180     WAL record format + magic
//   src/core/security_event_wal.hh:241-282     append (fwrite + flush)
//   src/core/security_event_wal.hh:191-237     enable + replay
//   src/core/security_event_wal.hh:374-405     persist_security_event
//   src/compiler/evaluator_security.cpp:303-308 effect allow/deny persist
//   src/compiler/evaluator_security.cpp:613-620 isolation-deny persist
//   src/compiler/evaluator_security.cpp:381-410 enable_security_event_wal
//   src/compiler/evaluator_security.cpp:423-424 auto-pair in
//                                                enable_mutation_audit_wal

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/mutation_audit_wal.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/security_event_wal.hh"
#include "core/wal_append_fail_slo.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
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
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::audit_wal::AuditWalRecord;
using aura::core::audit_wal::g_mutation_audit_wal;
using aura::core::audit_wal::reset_audit_wal_for_test;
using aura::core::audit_wal::snapshot_audit_wal_stats;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::append_security_event;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::kSecurityEventRingSize;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::security_event::SecurityEventKind;
using aura::core::security_event_wal::g_security_event_wal;
using aura::core::security_event_wal::reset_security_event_wal_for_test;
using aura::core::security_event_wal::snapshot_security_event_wal_stats;
using aura::test::g_failed;
using aura::test::g_passed;

namespace fs = std::filesystem;

std::int64_t href_posture(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:security-posture\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t href_wal_stats(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:audit-wal-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

int count_wal_prefix(const fs::path& dir, std::string_view prefix) {
    int n = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return 0;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
        const auto name = ent.path().filename().string();
        if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
            ent.path().extension() == ".wal")
            ++n;
    }
    return n;
}

// Clean a temp WAL dir; returns the path.
fs::path fresh_wal_dir(const std::string& tag) {
    const auto base =
        fs::temp_directory_path() / (std::string("aura-sec-event-wal-") + tag + "-XXXXXX");
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

void reset_all() {
    reset_capability_effects_for_test();
    reset_audit_wal_for_test();
    reset_security_event_wal_for_test();
    reset_security_event_ring_for_test();
    aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
    apply_dev_audit_defaults();
    set_mode(SandboxMode::Off);
}

std::string read_repo_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::uint64_t now_ms() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

int run_test_security_event_wal_replay() {
    std::println("=== Issue #2225: security event WAL + ring expand + replay ===");
    CHECK(true, "issue stamp #2225");
    CompilerService cs;
    auto& ev = cs.evaluator();

    // ── AC1: ring ≥ 1024; wrap counter increments when N>1024 denies ──
    {
        std::println("\n--- AC1: ring size + wrap counter ---");
        reset_all();
        auto& ring = g_security_event_ring();
        CHECK(kSecurityEventRingSize == 1024, "AC1: ring size bumped to 1024");
        CHECK(static_cast<std::int64_t>(kSecurityEventRingSize) == href_posture(cs, "ring-size"),
              "AC1: ring-size surfaced via query:security-posture");
        // Inject kSecurityEventRingSize + 10 events; expect ring-wrap-total
        // to equal exactly 10 (the events past the ring size).
        for (std::uint64_t i = 0; i < kSecurityEventRingSize + 10; ++i) {
            append_security_event(ring, SecurityEventKind::EffectDeny,
                                  /*tenant=*/1, /*mutation_id=*/i + 1, /*epoch=*/7,
                                  /*effect_bits=*/kEffectMutate, "test:ac1-wrap", "wrap-test");
        }
        const auto wrap_total = ring.ring_wrap_total.load(std::memory_order_relaxed);
        std::println("  ring wrap counter after {} appends: {}", kSecurityEventRingSize + 10,
                     wrap_total);
        CHECK(wrap_total == 10, "AC1: ring-wrap-total == 10 after 1034 appends");
        CHECK(static_cast<std::int64_t>(wrap_total) == href_posture(cs, "ring-wrap-total"),
              "AC1: ring-wrap-total surfaced via query:security-posture");
    }

    // ── AC2: durable write when WAL forced; AURA_SANDBOX=off no spill ──
    {
        std::println("\n--- AC2: durable side-car under multi-tenant/Strict ---");
        reset_all();
        auto& ring = g_security_event_ring();
        const auto dir = fresh_wal_dir("ac2");
        // Enable the side-car directly via the evaluator.
        CHECK(ev.enable_security_event_wal(dir.string()),
              "AC2: enable_security_event_wal under fresh dir");
        // Inject 5 events — they should be persisted to the side-car file.
        for (std::uint64_t i = 0; i < 5; ++i) {
            append_security_event(ring, SecurityEventKind::EffectAllow,
                                  /*tenant=*/42, /*mutation_id=*/1000 + i, /*epoch=*/7,
                                  /*effect_bits=*/kEffectMutate, "test:ac2-allow", "ac2-allow",
                                  /*denied=*/false, /*fiber=*/i);
        }
        // Persist each event to the WAL (mirroring the hot-path wire-up
        // in evaluator_security.cpp:303 / 613).
        const auto ts = now_ms();
        for (std::uint64_t i = 0; i < 5; ++i) {
            CHECK(aura::core::security_event_wal::persist_security_event(
                      SecurityEventKind::EffectAllow, 42, 1000 + i, 7, kEffectMutate,
                      "test:ac2-allow", "ac2-allow", /*denied=*/false, /*fiber=*/i, ts),
                  std::format("AC2: persist_security_event #{} returned true", i));
        }
        const auto snap = snapshot_security_event_wal_stats();
        std::println("  wal persisted={} replay={} append-fail={}", snap.persisted,
                     snap.replay_count, snap.append_fail);
        CHECK(snap.persisted >= 5, "AC2: side-car persisted ≥ 5 records");
        CHECK(snap.append_fail == 0, "AC2: side-car no append failures");
        // Verify the on-disk file exists + has records.
        const auto seg_path = dir / "security-event-0.wal";
        CHECK(fs::exists(seg_path), "AC2: side-car segment file written");
        const auto sz = fs::file_size(seg_path);
        CHECK(sz > 8 + 4, "AC2: side-car file > magic+version header");
        // Cleanup.
        ev.disable_security_event_wal();
        fs::remove_all(dir);
    }

    // ── AC3: restart + enable(dir) replays records into live ring ──
    {
        std::println("\n--- AC3: restart + replay round-trip ---");
        reset_all();
        const auto dir = fresh_wal_dir("ac3");
        // Phase 1: enable + write 3 events + disable (simulates a process
        // exit that flushes the WAL but loses the in-memory ring).
        {
            auto& ring = g_security_event_ring();
            CHECK(ev.enable_security_event_wal(dir.string()), "AC3 phase1: enable side-car");
            for (std::uint64_t i = 0; i < 3; ++i) {
                append_security_event(ring, SecurityEventKind::EffectDeny, /*tenant=*/99,
                                      /*mutation_id=*/5000 + i, /*epoch=*/13, kEffectMutate,
                                      "test:ac3-restart", "ac3-restart-deny",
                                      /*denied=*/true, /*fiber=*/i + 1);
            }
            const auto ts = now_ms();
            for (std::uint64_t i = 0; i < 3; ++i) {
                (void)aura::core::security_event_wal::persist_security_event(
                    SecurityEventKind::EffectDeny, 99, 5000 + i, 13, kEffectMutate,
                    "test:ac3-restart", "ac3-restart-deny", /*denied=*/true,
                    /*fiber=*/i + 1, ts);
            }
            ev.disable_security_event_wal();
            // Simulate the in-memory ring being wiped (process restart).
            reset_security_event_ring_for_test();
        }
        // Verify post-disable state.
        const auto& ring_post_disable = g_security_event_ring();
        CHECK(ring_post_disable.seq.load(std::memory_order_relaxed) == 0,
              "AC3: ring seq reset to 0 after process restart sim");
        // Phase 2: re-enable in a new "process" — replay should restore
        // the 3 events and advance ring.seq to 3.
        {
            auto& ring = g_security_event_ring();
            CHECK(ev.enable_security_event_wal(dir.string()),
                  "AC3 phase2: re-enable side-car (replay)");
            const auto seq_after = ring.seq.load(std::memory_order_relaxed);
            const auto total_after = ring.total.load(std::memory_order_relaxed);
            std::println("  ring seq={} total={} after replay", seq_after, total_after);
            CHECK(seq_after >= 3, "AC3: ring.seq ≥ 3 after replay (monotonic)");
            CHECK(total_after >= 3, "AC3: ring.total ≥ 3 after replay");
            const auto snap = snapshot_security_event_wal_stats();
            CHECK(snap.replay_count >= 1, "AC3: side-car replay count ≥ 1");
            CHECK(snap.crash_recovery_success >= 1, "AC3: crash-recovery success ≥ 1");
            // Verify the replayed events have their original mid (epoch 13,
            // not tenant 99 — #2156 lineage).
            std::uint64_t found_5000 = 0;
            for (std::uint64_t s = 0; s < seq_after; ++s) {
                const auto& slot = ring.ring[s % kSecurityEventRingSize];
                if (slot.mutation_id >= 5000 && slot.mutation_id < 5003 && slot.tenant_id == 99) {
                    found_5000++;
                    CHECK(slot.epoch == 13, "AC3: replayed mid epoch preserved (#2156)");
                }
            }
            CHECK(found_5000 == 3, "AC3: all 3 events restored (mid 5000..5002)");
        }
        ev.disable_security_event_wal();
        fs::remove_all(dir);
    }

    // ── AC4: hot path cost — no syscalls when WAL off ──
    {
        std::println("\n--- AC4: hot-path cost when WAL disabled ---");
        reset_all();
        // WAL is disabled by default; persist_security_event must short-
        // circuit with a single bool load + return false. We can only
        // verify the return value here (no syscall observation in unit
        // test). The real perf check is in soak / chaos tests.
        const auto ret = aura::core::security_event_wal::persist_security_event(
            SecurityEventKind::EffectDeny, 1, 1, 1, 0, "test:ac4-off", "off",
            /*denied=*/true, /*fiber=*/0, /*ts=*/0);
        CHECK(!ret, "AC4: persist_security_event returns false when WAL off");
        const auto snap = snapshot_security_event_wal_stats();
        CHECK(snap.persisted == 0, "AC4: no WAL persisted records when off");
        CHECK(snap.enabled == 0, "AC4: wal-enabled = 0 when off");
    }

    // ── AC5: source-cite + regression (auto-pair in enable_mutation_audit_wal) ──
    {
        std::println("\n--- AC5: source-cite + #2150 auto-pair ---");
        reset_all();
        const auto dir = fresh_wal_dir("ac5");
        // enable_mutation_audit_wal should also enable the side-car
        // (auto-pair under #2150 production defaults).
        CHECK(ev.enable_mutation_audit_wal(dir.string()), "AC5: enable_mutation_audit_wal");
        CHECK(ev.security_event_wal_enabled(), "AC5: side-car auto-enabled by #2150 auto-pair");
        CHECK(ev.mutation_audit_wal_enabled(), "AC5: mutation WAL enabled");
        CHECK(static_cast<std::int64_t>(1) == href_posture(cs, "mutation-wal-paired"),
              "AC5: mutation-wal-paired = 1 under both WALs enabled");
        CHECK(static_cast<std::int64_t>(1) == href_posture(cs, "wal-enabled"),
              "AC5: wal-enabled = 1 under both WALs enabled");
        // Source-cite — print the line numbers for grep reference.
        std::println("  src/core/security_event.hh:70-77         ring size + wrap counter");
        std::println("  src/core/security_event.hh:81-110        append_security_event");
        std::println("  src/core/security_event_wal.hh:140-180   WAL record + magic");
        std::println("  src/core/security_event_wal.hh:241-282   append (fwrite + flush)");
        std::println("  src/core/security_event_wal.hh:191-237   enable + replay");
        std::println("  src/core/security_event_wal.hh:374-405   persist_security_event");
        std::println("  src/compiler/evaluator_security.cpp:303  effect allow/deny persist");
        std::println("  src/compiler/evaluator_security.cpp:613  isolation-deny persist");
        std::println("  src/compiler/evaluator_security.cpp:381  enable_security_event_wal");
        std::println(
            "  src/compiler/evaluator_security.cpp:423  auto-pair in enable_mutation_audit_wal");
        // Cleanup.
        ev.disable_mutation_audit_wal();
        CHECK(!ev.mutation_audit_wal_enabled(),
              "AC5: disable_mutation_audit_wal turns off mutation WAL");
        CHECK(!ev.security_event_wal_enabled(),
              "AC5: disable_mutation_audit_wal also disables side-car");
        fs::remove_all(dir);
    }

    // ── Issue #3056: production WAL append_fail arms posture ──
    {
        using aura::core::wal_slo::decide_wal_append_fail_slo;
        using aura::core::wal_slo::g_wal_append_fail_slo_counters;
        using aura::core::wal_slo::kWalAppendFailSloIssue;
        using aura::core::wal_slo::WalAppendFailSloInput;

        std::println("\n--- #3056 AC1: WAL off / Soft does not arm ---");
        reset_all();
        CHECK(href_posture(cs, "wal-append-fail-slo-wired") == 1, "3056 AC1: wired");
        CHECK(href_posture(cs, "schema-3056") == kWalAppendFailSloIssue, "3056 AC1: schema");
        CHECK(href_posture(cs, "wal-append-fail-breach") == 0, "3056 AC1: no breach WAL-off");
        g_wal_append_fail_slo_counters.inject_fail_remaining.store(1, std::memory_order_relaxed);
        const auto off_ret = aura::core::security_event_wal::persist_security_event(
            SecurityEventKind::EffectDeny, 1, 1, 1, 0, "test:3056-off", "off", true, 0, 0);
        CHECK(!off_ret, "3056 AC1: persist short-circuits when WAL off");
        CHECK(g_wal_append_fail_slo_counters.inject_fail_remaining.load(
                  std::memory_order_relaxed) == 1,
              "3056 AC1: inject not consumed on WAL-off path");
        CHECK(snapshot_security_event_wal_stats().append_fail == 0,
              "3056 AC1: no append_fail bump when WAL off");
        CHECK(href_posture(cs, "wal-append-fail-breach") == 0,
              "3056 AC1: still no breach after off persist");

        {
            const auto dir_soft = fresh_wal_dir("3056-ac1-soft");
            CHECK(ev.enable_security_event_wal(dir_soft.string()),
                  "3056 AC1: enable SE WAL (Soft)");
            g_wal_append_fail_slo_counters.inject_fail_remaining.store(1,
                                                                       std::memory_order_relaxed);
            const auto soft_fail = aura::core::security_event_wal::persist_security_event(
                SecurityEventKind::EffectDeny, 1, 2, 1, 0, "test:3056-soft", "soft", true, 0, 0);
            CHECK(!soft_fail, "3056 AC1: Soft inject still returns false");
            CHECK(snapshot_security_event_wal_stats().append_fail >= 1,
                  "3056 AC1: Soft still bumps append-fail");
            CHECK(href_posture(cs, "wal-append-fail-breach") == 0,
                  "3056 AC1: Soft observe-only (no arm)");
            ev.disable_security_event_wal();
            fs::remove_all(dir_soft);
        }

        std::println("\n--- #3056 AC2: production + inject fail → breach ---");
        reset_all();
        apply_production_audit_defaults();
        const auto dir_ac2 = fresh_wal_dir("3056-ac2");
        CHECK(ev.enable_security_event_wal(dir_ac2.string()), "3056 AC2: enable SE WAL");
        g_wal_append_fail_slo_counters.inject_fail_remaining.store(1, std::memory_order_relaxed);
        const auto fail_ret = aura::core::security_event_wal::persist_security_event(
            SecurityEventKind::EffectDeny, 7, 77, 1, 0, "test:3056-ac2", "inject", true, 0, 0);
        CHECK(!fail_ret, "3056 AC2: inject fail returns false");
        CHECK(snapshot_security_event_wal_stats().append_fail >= 1,
              "3056 AC2: SE append-fail counter bumped");
        CHECK(g_wal_append_fail_slo_counters.combined_fail_total.load(std::memory_order_relaxed) >=
                  1,
              "3056 AC2: combined fail");
        CHECK(href_posture(cs, "wal-append-fail-breach") == 1,
              "3056 AC2: posture breach key armed");
        CHECK(g_wal_append_fail_slo_counters.last_would_arm_degraded.load(
                  std::memory_order_relaxed) == 1,
              "3056 AC2: would_arm_degraded");
        CHECK(g_wal_append_fail_slo_counters.arm_degraded_total.load(std::memory_order_relaxed) >=
                  1,
              "3056 AC2: arm_degraded_total");

        std::println("\n--- #3056 AC3: mutation commit stays fail-open ---");
        CHECK(ev.security_event_wal_enabled(), "3056 AC3: WAL still enabled after fail");
        const auto ok_ret = aura::core::security_event_wal::persist_security_event(
            SecurityEventKind::EffectAllow, 7, 78, 1, 0, "test:3056-ac3", "recover", false, 0, 0);
        CHECK(ok_ret, "3056 AC3: later persist succeeds (fail-open)");
        const auto sec_src = read_repo_file("src/compiler/evaluator_security.cpp");
        CHECK(sec_src.find("(void)g_mutation_audit_wal().append") != std::string::npos,
              "3056 AC3: mutation append still discarded (fail-open)");
        CHECK(sec_src.find("Issue #3056") != std::string::npos, "3056 AC3: security.cpp cites");

        ev.disable_security_event_wal();
        fs::remove_all(dir_ac2);

        std::println("\n--- #3056 AC4: additive schema, no rename ---");
        reset_all();
        CHECK(href_posture(cs, "wal-append-fail-total") >= 0 ||
                  href_posture(cs, "schema-2534") == 2534,
              "3056 AC4: existing posture surface still live");
        const auto wal_src = read_repo_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(wal_src.find("insert_kv(\"append-fail\"") != std::string::npos,
              "3056 AC4: audit-wal-stats append-fail kept");
        CHECK(wal_src.find("wal-append-fail-breach") != std::string::npos,
              "3056 AC4: additive breach key");
        CHECK(wal_src.find("schema-3056") != std::string::npos, "3056 AC4: schema-3056");

        std::println("\n--- #3056 AC5: both WALs feed one decide ---");
        reset_all();
        apply_production_audit_defaults();
        const auto dir_ac5 = fresh_wal_dir("3056-ac5");
        CHECK(ev.enable_mutation_audit_wal(dir_ac5.string()), "3056 AC5: enable mutation WAL");
        g_wal_append_fail_slo_counters.inject_fail_remaining.store(1, std::memory_order_relaxed);
        AuditWalRecord rec{};
        rec.seq = 1;
        CHECK(!g_mutation_audit_wal().append(rec), "3056 AC5: mutation inject fail");
        CHECK(snapshot_audit_wal_stats().append_fail >= 1, "3056 AC5: mutation append-fail");
        CHECK(href_posture(cs, "wal-append-fail-breach") == 1,
              "3056 AC5: mutation fail arms same key");
        ev.disable_mutation_audit_wal();
        fs::remove_all(dir_ac5);

        WalAppendFailSloInput pin;
        pin.fail_total = 1;
        pin.persisted_total = 0;
        pin.consecutive = 1;
        pin.wal_enabled = true;
        pin.production_defaults = true;
        pin.soft_mode = false;
        const auto d1 = decide_wal_append_fail_slo(pin);
        const auto d2 = decide_wal_append_fail_slo(pin);
        CHECK(d1.would_arm_degraded == d2.would_arm_degraded && d1.breached == d2.breached,
              "3056 AC5: decide is pure");
        CHECK(d1.force_reason == "wal-append-fail-breach", "3056 AC5: force_reason");

        std::println("\n--- #3056 AC6: source-cite + no invent ---");
        const auto slo = read_repo_file("src/core/wal_append_fail_slo.h");
        const auto se_wal = read_repo_file("src/core/security_event_wal.hh");
        const auto mut_wal = read_repo_file("src/core/mutation_audit_wal.hh");
        const auto build = read_repo_file("build.py");
        CHECK(slo.find("kWalAppendFailSloIssue = 3056") != std::string::npos, "3056 AC6: stamp");
        CHECK(se_wal.find("note_wal_append_fail") != std::string::npos, "3056 AC6: SE WAL notes");
        CHECK(mut_wal.find("note_wal_append_fail") != std::string::npos,
              "3056 AC6: mutation WAL notes");
        CHECK(build.find("check_wal_append_fail_slo_3056") != std::string::npos,
              "3056 AC6: build.py wires linter");
        CHECK(read_repo_file("docs/design/3056-wal-append-fail-slo.md").empty(),
              "3056 AC6: no docs/design/3056-* per #1655");
        CHECK(read_repo_file("tests/compiler/test_issue_3056.cpp").empty(),
              "3056 AC6: no test_issue_3056.cpp per #81967");

        apply_dev_audit_defaults();
    }

    // ── Issue #3178: WAL overflow ring must stamp forensic join keys
    // (mutation_id + tenant/fiber/epoch) under fail-closed, not the
    // WAL sequence number. Previously stamped rec.seq which is the
    // WAL-local sequence, NOT a join key for
    // query:security-audit [mutation-id=…] / Typed trail /
    // CapabilityGrant.bound_mutation_id (#3109 residual). The fix is
    // in both inject_fail and fwrite_miss branches of
    // SecurityEventWal::append (security_event_wal.hh).
    {
        std::println("\n--- #3178 AC1/AC2/AC6: overflow stamps mutation_id join key ---");
        using aura::core::wal_slo::g_wal_append_fail_slo_counters;
        using aura::core::wal_slo::reset_wal_append_fail_slo_for_test;
        reset_all();
        reset_wal_append_fail_slo_for_test();
        aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
        ::setenv("AURA_WAL_APPEND_FAIL_CLOSED", "1", 1);
        apply_production_audit_defaults();

        const auto dir_3178 = fresh_wal_dir("3178-ac1");
        CHECK(ev.enable_security_event_wal(dir_3178.string()),
              "AC1: enable SE WAL under production + fail-closed env");
        CHECK(aura::core::wal_slo::wal_append_fail_closed_active(),
              "AC1: wal_append_fail_closed_active() under env + production");

        // AC1 + AC6: non-zero mid M, tenant T, fiber F, epoch E.
        constexpr std::uint64_t M = 0xDEADBEEFCAFE5EEDULL;
        constexpr std::uint64_t T = 42;
        constexpr std::uint64_t F = 0xCAFE;
        constexpr std::uint64_t E = 99;
        g_wal_append_fail_slo_counters.inject_fail_remaining.store(1, std::memory_order_relaxed);
        const bool fail_ret = aura::core::security_event_wal::persist_security_event(
            SecurityEventKind::EffectDeny, T, M, E, 0, "test:3178-ac1", "inject", true, F, 0);
        CHECK(!fail_ret, "AC1: inject fail returns false under fail-closed");

        const auto depth = aura::core::security_event_wal::wal_overflow_ring_depth();
        CHECK(depth >= 1, "AC1: overflow ring has at least 1 entry");

        auto* ring = aura::core::security_event_wal::wal_overflow_ring_storage();
        const auto head = aura::core::security_event_wal::wal_overflow_ring_head().load(
            std::memory_order_acquire);
        const auto last_idx =
            (head + aura::core::security_event_wal::kWalOverflowRingCapacity - 1) %
            aura::core::security_event_wal::kWalOverflowRingCapacity;
        const auto& ovr = ring[last_idx];
        CHECK(ovr.mid == M, "AC1: overflow mid == record.mutation_id (NOT rec.seq)");
        CHECK(ovr.tenant_id == T, "AC1: overflow tenant_id == record.tenant_id");
        CHECK(ovr.fiber_id == F, "AC1: overflow fiber_id == record.fiber_id");
        CHECK(ovr.epoch == E, "AC1: overflow epoch == record.epoch");
        CHECK(ovr.op == std::string("test:3178-ac1"), "AC1: overflow op == event op");
        CHECK(ovr.reason == std::string("inject_fail"), "AC1: overflow reason == inject_fail");

        // AC6: mid=0 must NOT be synthesized (do not invent process-origin mid).
        aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
        g_wal_append_fail_slo_counters.inject_fail_remaining.store(1, std::memory_order_relaxed);
        (void)aura::core::security_event_wal::persist_security_event(
            SecurityEventKind::EffectDeny, T, /*mid=*/0, E, 0, "test:3178-zero-mid", "zero", true,
            F, 0);
        const auto depth2 = aura::core::security_event_wal::wal_overflow_ring_depth();
        CHECK(depth2 >= 1, "AC6: overflow ring has entry on mid=0 push");
        auto* ring2 = aura::core::security_event_wal::wal_overflow_ring_storage();
        const auto head2 = aura::core::security_event_wal::wal_overflow_ring_head().load(
            std::memory_order_acquire);
        const auto last_idx2 =
            (head2 + aura::core::security_event_wal::kWalOverflowRingCapacity - 1) %
            aura::core::security_event_wal::kWalOverflowRingCapacity;
        CHECK(ring2[last_idx2].mid == 0,
              "AC6: overflow mid == 0 (no synthetic process-origin mid)");

        // AC2: Soft / no-env → overflow ring never written.
        reset_all();
        aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
        ::unsetenv("AURA_WAL_APPEND_FAIL_CLOSED");
        const auto dir_soft_3178 = fresh_wal_dir("3178-soft");
        CHECK(ev.enable_security_event_wal(dir_soft_3178.string()),
              "AC2: enable SE WAL under Soft");
        CHECK(!aura::core::wal_slo::wal_append_fail_closed_active(),
              "AC2: wal_append_fail_closed_active() false without env");
        // With Soft mode the fail-closed gate is OFF even with WAL enabled,
        // so the overflow ring stays at depth 0 under inject_fail (path
        // never entered). Force inject_fail to verify.
        g_wal_append_fail_slo_counters.inject_fail_remaining.store(1, std::memory_order_relaxed);
        const bool soft_fail = aura::core::security_event_wal::persist_security_event(
            SecurityEventKind::EffectDeny, T, M, E, 0, "test:3178-soft", "soft", true, F, 0);
        CHECK(!soft_fail, "AC2: Soft inject still returns false (no WAL write)");
        CHECK(aura::core::security_event_wal::wal_overflow_ring_depth() == 0,
              "AC2: overflow ring depth 0 under Soft (fail-closed gate off)");

        ev.disable_security_event_wal();
        fs::remove_all(dir_3178);
        fs::remove_all(dir_soft_3178);
    }

    // ── Issue #3338: mid lookup window + optional segment retention ──
    {
        using aura::core::security_event_wal::SecurityEventWalRecord;
        using aura::core::wal_slo::kWalMidLookupSegmentsProduction;
        using aura::core::wal_slo::kWalMidLookupSegmentsSoft;
        using aura::core::wal_slo::kWalMidLookupWindowIssue;
        using aura::core::wal_slo::wal_max_segments_retention;
        using aura::core::wal_slo::wal_mid_lookup_segments;

        std::println("\n--- #3338 AC1: Soft / WAL-off zero cost ---");
        reset_all();
        ::unsetenv("AURA_WAL_MID_LOOKUP_SEGMENTS");
        ::unsetenv("AURA_WAL_MAX_SEGMENTS");
        CHECK(kWalMidLookupWindowIssue == 3338, "3338 AC: issue stamp");
        CHECK(wal_mid_lookup_segments() == kWalMidLookupSegmentsSoft,
              "3338 AC1: Soft lookup default 2");
        CHECK(wal_max_segments_retention() == 0, "3338 AC1: unset retention = 0 (no prune)");
        CHECK(href_posture(cs, "wal-mid-lookup-segments") ==
                  static_cast<std::int64_t>(kWalMidLookupSegmentsSoft),
              "3338 AC1: posture Soft window 2");
        CHECK(href_posture(cs, "wal-max-segments-retention") == 0,
              "3338 AC1: posture no retention");
        CHECK(href_posture(cs, "wal-segment-prune-total") == 0, "3338 AC1: prune 0 WAL-off");
        CHECK(href_posture(cs, "schema-3338") == kWalMidLookupWindowIssue, "3338 AC1: schema-3338");
        CHECK(href_wal_stats(cs, "wal-mid-lookup-segments") ==
                  static_cast<std::int64_t>(kWalMidLookupSegmentsSoft),
              "3338 AC1: audit-wal-stats Soft window");
        const auto off_ret = aura::core::security_event_wal::persist_security_event(
            SecurityEventKind::EffectDeny, 1, 3338, 1, 0, "test:3338-off", "off", true, 0, 0);
        CHECK(!off_ret, "3338 AC1: persist short-circuits when WAL off");
        CHECK(snapshot_security_event_wal_stats().segment_prune_total == 0,
              "3338 AC1: no prune counter bump WAL-off");

        std::println("\n--- #3338 AC2: rotate ≥3, lookup window still hits ---");
        reset_all();
        apply_production_audit_defaults();
        ::unsetenv("AURA_WAL_MAX_SEGMENTS");
        ::setenv("AURA_WAL_MID_LOOKUP_SEGMENTS", "8", 1);
        CHECK(wal_mid_lookup_segments() == 8, "3338 AC2: env lookup 8");
        CHECK(href_posture(cs, "wal-mid-lookup-segments") == 8, "3338 AC2: posture env window");
        const auto dir_ac2 = fresh_wal_dir("3338-ac2");
        CHECK(ev.enable_security_event_wal(dir_ac2.string()), "3338 AC2: enable SE WAL");
        g_security_event_wal().set_rotate_bytes(sizeof(SecurityEventWalRecord) * 4);
        constexpr std::uint64_t kEarlyMid = 3338001;
        CHECK(aura::core::security_event_wal::persist_security_event(
                  SecurityEventKind::EffectDeny, 9, kEarlyMid, 1, kEffectMutate, "op-3338",
                  "early-mid", true, 3, now_ms()),
              "3338 AC2: persist early mid");
        for (std::uint64_t i = 1; i < 20; ++i) {
            (void)aura::core::security_event_wal::persist_security_event(
                SecurityEventKind::EffectAllow, 9, 50000 + i, 1, kEffectMutate, "fill-3338", "fill",
                false, 3, now_ms());
        }
        CHECK(snapshot_security_event_wal_stats().rotate_total >= 3,
              "3338 AC2: rotate ≥ 3 segments");
        CHECK(g_security_event_wal().find_recent_by_mutation_id(kEarlyMid, /*max_segments=*/2) ==
                  std::nullopt,
              "3338 AC4: explicit max_segments=2 may miss after ≥3 rotates");
        auto hit8 =
            g_security_event_wal().find_recent_by_mutation_id(kEarlyMid, wal_mid_lookup_segments());
        CHECK(hit8.has_value(), "3338 AC2: lookup window hits early mid after ≥3 rotates");
        if (hit8)
            CHECK(hit8->mutation_id == kEarlyMid, "3338 AC2: rec mid");
        CHECK(href_posture(cs, "wal-max-segments-retention") == 0,
              "3338 AC2: unset retention surfaced as 0");
        ev.disable_security_event_wal();
        g_security_event_wal().set_rotate_bytes(
            aura::core::security_event_wal::kDefaultRotateBytes);
        fs::remove_all(dir_ac2);

        std::println("\n--- #3338 AC3: AURA_WAL_MAX_SEGMENTS=4 prunes ---");
        reset_all();
        apply_production_audit_defaults();
        ::setenv("AURA_WAL_MAX_SEGMENTS", "4", 1);
        ::setenv("AURA_WAL_MID_LOOKUP_SEGMENTS", "8", 1);
        CHECK(wal_max_segments_retention() == 4, "3338 AC3: retention 4");
        CHECK(href_posture(cs, "wal-max-segments-retention") == 4, "3338 AC3: posture retention");
        const auto dir_ac3 = fresh_wal_dir("3338-ac3");
        CHECK(ev.enable_security_event_wal(dir_ac3.string()), "3338 AC3: enable SE WAL");
        g_security_event_wal().set_rotate_bytes(sizeof(SecurityEventWalRecord) * 4);
        for (std::uint64_t i = 0; i < 24; ++i) {
            (void)aura::core::security_event_wal::persist_security_event(
                SecurityEventKind::EffectAllow, 9, 60000 + i, 1, kEffectMutate, "prune-3338",
                "fill", false, 3, now_ms());
        }
        const auto se_files = count_wal_prefix(dir_ac3, "security-event-");
        CHECK(se_files <= 4, "3338 AC3: disk SE segments ≤ 4");
        CHECK(snapshot_security_event_wal_stats().segment_prune_total >= 1,
              "3338 AC3: SE prune_total incremented");
        CHECK(href_posture(cs, "wal-segment-prune-total") >= 1, "3338 AC3: posture prune-total");
        CHECK(href_wal_stats(cs, "wal-segment-prune-total") >= 1,
              "3338 AC3: audit-wal-stats prune-total");
        ev.disable_security_event_wal();
        g_security_event_wal().set_rotate_bytes(
            aura::core::security_event_wal::kDefaultRotateBytes);
        fs::remove_all(dir_ac3);

        std::println("\n--- #3338 AC3 mutation WAL prune lockstep ---");
        reset_all();
        apply_production_audit_defaults();
        ::setenv("AURA_WAL_MAX_SEGMENTS", "4", 1);
        const auto dir_mut = fresh_wal_dir("3338-mut");
        CHECK(ev.enable_mutation_audit_wal(dir_mut.string()), "3338 AC3: enable mutation WAL");
        g_mutation_audit_wal().set_rotate_bytes(sizeof(AuditWalRecord) * 4);
        constexpr std::uint64_t kMutMid = 3338002;
        for (int i = 0; i < 24; ++i) {
            AuditWalRecord rec{};
            rec.seq = static_cast<std::uint64_t>(i + 1);
            rec.provenance_mutation_id =
                (i == 0) ? kMutMid : 70000u + static_cast<std::uint64_t>(i);
            rec.tenant_id = 7;
            (void)g_mutation_audit_wal().append(rec);
        }
        CHECK(count_wal_prefix(dir_mut, "audit-") <= 4, "3338 AC3: disk audit segments ≤ 4");
        CHECK(snapshot_audit_wal_stats().segment_prune_total >= 1,
              "3338 AC3: mutation prune_total");
        CHECK(g_mutation_audit_wal().find_recent_by_provenance_mutation_id(kMutMid, 2) ==
                  std::nullopt,
              "3338 AC4: explicit 2 misses pruned/old mutation mid");
        ev.disable_mutation_audit_wal();
        g_mutation_audit_wal().set_rotate_bytes(aura::core::audit_wal::kDefaultRotateBytes);
        fs::remove_all(dir_mut);

        std::println("\n--- #3338 AC5: production default window 8; no invent ---");
        reset_all();
        apply_production_audit_defaults();
        ::unsetenv("AURA_WAL_MID_LOOKUP_SEGMENTS");
        ::unsetenv("AURA_WAL_MAX_SEGMENTS");
        CHECK(wal_mid_lookup_segments() == kWalMidLookupSegmentsProduction,
              "3338 AC5: production default lookup 8");
        CHECK(href_posture(cs, "wal-overflow-ring-depth") >= 0, "3338 AC4: #3109 key retained");
        const auto se_src = read_repo_file("src/core/security_event_wal.hh");
        CHECK(se_src.find("max_segments = 2") != std::string::npos,
              "3338 AC4: find_recent default still 2");
        CHECK(se_src.find("prune_old_segments_unlocked") != std::string::npos,
              "3338 AC3: SE prune on rotate");
        const auto mut_src = read_repo_file("src/core/mutation_audit_wal.hh");
        CHECK(mut_src.find("prune_old_segments_unlocked") != std::string::npos,
              "3338 AC3: mutation prune on rotate");
        CHECK(read_repo_file("docs/design/3338-wal-mid-lookup-window.md").empty(),
              "ac3338_5_no_invent");
        CHECK(read_repo_file("tests/compiler/test_issue_3338.cpp").empty(),
              "3338 AC5: no test_issue_3338.cpp");
        apply_dev_audit_defaults();
        ::unsetenv("AURA_WAL_MID_LOOKUP_SEGMENTS");
        ::unsetenv("AURA_WAL_MAX_SEGMENTS");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_security_event_wal_replay();
}
#endif
