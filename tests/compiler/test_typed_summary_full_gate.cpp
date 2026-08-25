// tests/compiler/test_typed_summary_full_gate.cpp
// @category: unit
// @reason: Issue #3298 — typed-summary persist gate must align with the Full
// hard face. maybe_persist_typed_summary gated on production_defaults_active()
// alone, so Full-only / embed deployments (strategy=Full, production_defaults=0)
// audited fully in-memory but never persisted the #3242 sidecar — trail wrap
// lost "what changed". Fix: gate on
// production_defaults_active() || get_strategy() == AuditStrategy::Full
// (same shape as the rest of typed_audit's hard face). Read-back paths
// (query:security-audit sidecar keys + query:evolution-audit-decision :durable)
// got the same alignment so Full-only can recover the summary after wrap.
//
//   AC1: Full-only (set_strategy(Full), production_defaults=0) + WAL on →
//        persist happens; after trail wrap, :durable read-back recovers
//        outcome + kind (AC3 of the issue)
//   AC2: Sampled (production_defaults=0) → no typed WAL write (zero-cost)
//   AC3: production_defaults path unchanged (regression vs #3242)
//   AC5: source-cite — gate shape matches Full hard face; no new query keys

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/typed_mutation_audit.h"
#include "core/mutation_audit_wal.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"

#include <cstdint>
#include <filesystem>
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
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditOutcome;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::capture_audit_event_forced;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::kTypedMutationAuditTrailSize;
using aura::compiler::typed_audit::MutationKind;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::typed_audit::trail_find_by_mutation_id;
using aura::compiler::typed_audit::TypedMutationAuditEvent;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::audit_wal::reset_audit_wal_for_test;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::reset_security_event_ring_for_test;
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

static void reset_process() {
    reset_capability_effects_for_test();
    reset_audit_wal_for_test();
    reset_security_event_ring_for_test();
    reset_for_test(); // → apply_dev_audit_defaults: Sampled + production_defaults=0
    set_mode(SandboxMode::Off);
}

static std::int64_t href_evol_mid_durable(CompilerService& cs, std::uint64_t mid,
                                          std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:evolution-audit-decision\" {} \"durable\") \"{}\")", mid,
        key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: Full-only persists + durable read-back after wrap ──
static void ac3298_1_full_only_persists() {
    std::println("\n--- 3298 AC1: Full-only (no production_defaults) persists typed summary ---");
    reset_process();
    // Full-only / embed deployment: strategy=Full, production_defaults_active=0.
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "aura-3298-full-only";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    CHECK(cs.evaluator().enable_mutation_audit_wal(dir.string()), "3298 AC1: enable WAL");
    const std::uint64_t mid = 32981;
    capture_audit_event_forced(mid, "op-3298", MutationKind::Structural, 1, 2,
                               AuditOutcome::Rollback, /*target=*/7, /*nodes=*/3);
    CHECK(g_typed_mutation_audit_counters.typed_summary_wal_persisted_total.load() >= 1,
          "3298 AC1: Full-only persists typed summary (production_defaults=0)");
    // Wrap the trail; durable sidecar must still recover outcome/kind.
    for (std::size_t i = 0; i < kTypedMutationAuditTrailSize; ++i) {
        capture_audit_event_forced(97000 + i, "wrap-fill-3298", MutationKind::Other, 1, 1,
                                   AuditOutcome::Success);
    }
    TypedMutationAuditEvent te_miss{};
    CHECK(!trail_find_by_mutation_id(mid, te_miss), "3298 AC1: mid wrapped out of trail");
    CHECK(href_evol_mid_durable(cs, mid, "typed-summary-from-wal") == 1,
          "3298 AC1: Full-only :durable recovers typed summary after wrap");
    CHECK(href_evol_mid_durable(cs, mid, "typed-outcome") == 2,
          "3298 AC1: WAL typed-outcome=Rollback");
    CHECK(href_evol_mid_durable(cs, mid, "typed-kind") ==
              static_cast<std::int64_t>(MutationKind::Structural),
          "3298 AC1: WAL typed-kind=Structural");
    cs.evaluator().disable_mutation_audit_wal();
    fs::remove_all(dir, ec);
}

// ── AC2: Sampled / production_defaults=0 does not persist ──
static void ac3298_2_sampled_no_persist() {
    std::println("\n--- 3298 AC2: Sampled / production_defaults=0 does not persist ---");
    reset_process();
    // Sampled (reset default) + production_defaults=0 → zero-cost (no write).
    CompilerService cs;
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "aura-3298-sampled";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    CHECK(cs.evaluator().enable_mutation_audit_wal(dir.string()), "3298 AC2: enable WAL");
    const auto p0 = g_typed_mutation_audit_counters.typed_summary_wal_persisted_total.load();
    capture_audit_event_forced(32982, "sampled-3298", MutationKind::ReplaceValue, 1, 1,
                               AuditOutcome::Success, /*target=*/1, /*nodes=*/1);
    CHECK(g_typed_mutation_audit_counters.typed_summary_wal_persisted_total.load() == p0,
          "3298 AC2: Sampled no typed WAL write");
    CHECK(href_evol_mid_durable(cs, 32982, "typed-summary-from-wal") == 0,
          "3298 AC2: Sampled :durable typed-summary-from-wal=0");
    cs.evaluator().disable_mutation_audit_wal();
    fs::remove_all(dir, ec);
}

// ── AC3: production_defaults path unchanged (regression vs #3242) ──
static void ac3298_3_production_regression() {
    std::println("\n--- 3298 AC3: production_defaults path unchanged (regression) ---");
    reset_process();
    apply_production_audit_defaults();
    CompilerService cs;
    apply_production_audit_defaults();
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "aura-3298-prod";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    CHECK(cs.evaluator().enable_mutation_audit_wal(dir.string()), "3298 AC3: enable WAL");
    const std::uint64_t mid = 32983;
    capture_audit_event_forced(mid, "op-3298-prod", MutationKind::Structural, 1, 2,
                               AuditOutcome::Success, /*target=*/7, /*nodes=*/3);
    CHECK(g_typed_mutation_audit_counters.typed_summary_wal_persisted_total.load() >= 1,
          "3298 AC3: production_defaults still persists");
    cs.evaluator().disable_mutation_audit_wal();
    fs::remove_all(dir, ec);
}

// ── AC5: source-cite gate alignment (no new keys) ──
static void ac3298_5_source_cite() {
    std::println("\n--- 3298 AC5: source-cite gate alignment (no new keys) ---");
    const auto hooks = read_file("src/compiler/typed_mutation_audit_hooks.cpp");
    CHECK(hooks.find("Issue #3298") != std::string::npos,
          "3298 AC5: typed_mutation_audit_hooks.cpp cites #3298");
    CHECK(hooks.find("production_defaults_active() || get_strategy() == AuditStrategy::Full") !=
              std::string::npos,
          "3298 AC5: gate matches the Full hard face shape");
    CHECK(hooks.find("typed_summary_wal_persisted_total") != std::string::npos,
          "3298 AC5: persist counter unchanged (additive only)");
    // Read-back gate alignment (#3298 AC3 query face).
    const auto prims = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(prims.find("Issue #3298") != std::string::npos,
          "3298 AC5: evaluator_primitives_security.cpp cites #3298 (read-back gate)");
    CHECK(prims.find("get_strategy() == AuditStrategy::Full") != std::string::npos,
          "3298 AC5: read-back gate aligned to Full hard face");
    CHECK(!std::filesystem::exists("docs/design/3298-"), "3298 AC5: no docs/design per #1655");
    CHECK(!std::filesystem::exists("tests/issues/test_issue_3298.cpp"),
          "3298 AC5: no invent test per #81967");
}

} // namespace

int run_test_typed_summary_full_gate() {
    std::println("=== Issue #3298: typed-summary persist gate aligns with Full hard face ===");
    ac3298_1_full_only_persists();
    ac3298_2_sampled_no_persist();
    ac3298_3_production_regression();
    ac3298_5_source_cite();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_typed_summary_full_gate();
}
#endif
