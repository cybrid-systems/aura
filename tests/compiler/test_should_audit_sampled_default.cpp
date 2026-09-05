// @category: unit
// @reason: Issue #2818 — cold-start Full default; Sampled under-sample only via
// apply_dev_audit_defaults opt-in; warn metric when Sampled+ratio>1 without opt-in.
//
//   AC1: cold-start default Full (strategy + should_audit always hits)
//   AC2: apply_dev Sampled/4 under-samples ~75% of 100 ids
//   AC3: Sampled+ratio>1 without apply_dev bumps audit_strategy_default_warnings_total
//   AC4: schema-2818 query keys; this suite + linter; no docs/design/2818-*

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "core/security_event.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::kSecurityEventRingSize;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::test::g_failed;
using aura::test::g_passed;
namespace ta = aura::compiler::typed_audit;

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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Mirror cold-start static defaults (Full/ratio=1, no production flag, no dev opt-in).
static void restore_cold_start_audit_defaults() {
    ta::set_strategy(ta::AuditStrategy::Full);
    ta::set_sample_ratio(1);
    ta::g_typed_mutation_audit_counters.production_defaults_active.store(0,
                                                                         std::memory_order_relaxed);
    ta::g_typed_mutation_audit_counters.dev_audit_opt_in.store(0, std::memory_order_relaxed);
    ta::g_typed_mutation_audit_counters.audits_considered.store(0, std::memory_order_relaxed);
    ta::g_typed_mutation_audit_counters.samples_skipped.store(0, std::memory_order_relaxed);
    ta::g_typed_mutation_audit_counters.audit_strategy_default_warnings_total.store(
        0, std::memory_order_relaxed);
    ta::g_typed_mutation_audit_counters.audit_strategy_default_warning_fired.store(
        0, std::memory_order_relaxed);
}

} // namespace

int run_test_should_audit_sampled_default() {
    std::println("=== Issue #2818: should_audit Full default / Sampled opt-in ===");
    CHECK(true, "ac2818: issue stamp");

    // ── AC1: source + cold-start Full ──
    {
        std::println("\n--- AC1: cold-start Full; 100 should_audit all hit ---");
        auto h = read_file("src/compiler/typed_mutation_audit.h");
        CHECK(!h.empty(), "AC1: audit header readable");
        CHECK(h.find("Issue #2818") != std::string::npos, "AC1: cites #2818");
        CHECK(h.find("audit_strategy_default_warnings_total") != std::string::npos,
              "AC1: warning metric");
        CHECK(h.find("dev_audit_opt_in") != std::string::npos, "AC1: dev opt-in flag");
        CHECK(h.find("maybe_warn_sampled_without_opt_in") != std::string::npos, "AC1: warn helper");
        // Default strategy initializer is Full.
        CHECK(h.find("AuditStrategy::Full}") != std::string::npos ||
                  h.find("AuditStrategy::Full)}") != std::string::npos ||
                  h.find("static_cast<std::uint32_t>(AuditStrategy::Full)") != std::string::npos,
              "AC1: static default strategy Full");
        CHECK(h.find("sample_ratio{1}") != std::string::npos, "AC1: static sample_ratio=1");

        restore_cold_start_audit_defaults();
        CHECK(ta::get_strategy() == ta::AuditStrategy::Full, "AC1: strategy Full");
        CHECK(ta::get_sample_ratio() == 1, "AC1: ratio 1");
        CHECK(!ta::production_defaults_active(), "AC1: production flag off at cold start");
        CHECK(ta::g_typed_mutation_audit_counters.dev_audit_opt_in.load() == 0,
              "AC1: no dev opt-in at cold start");

        int hits = 0;
        for (std::uint64_t id = 1; id <= 100; ++id) {
            if (ta::should_audit(id))
                ++hits;
        }
        CHECK(hits == 100, std::format("AC1: Full audits all 100 (got {})", hits));
        CHECK(ta::g_typed_mutation_audit_counters.audits_considered.load() == 100,
              "AC1: audits_considered == 100");
        CHECK(ta::g_typed_mutation_audit_counters.samples_skipped.load() == 0,
              "AC1: samples_skipped == 0 under Full");
        CHECK(ta::g_typed_mutation_audit_counters.audit_strategy_default_warnings_total.load() == 0,
              "AC1: no warning under Full");
        // requires_invariant_hard_gate under Full
        CHECK(ta::requires_invariant_hard_gate(/*nodes=*/1, /*linear=*/false, /*strict=*/false),
              "AC1: Full forces invariant hard gate for small dirty");
    }

    // ── AC2: apply_dev under-samples ──
    {
        std::println("\n--- AC2: apply_dev Sampled/4 under-samples ~75% ---");
        ta::reset_for_test(); // apply_dev_audit_defaults
        CHECK(ta::get_strategy() == ta::AuditStrategy::Sampled, "AC2: Sampled");
        CHECK(ta::get_sample_ratio() == 4, "AC2: ratio 4");
        CHECK(ta::g_typed_mutation_audit_counters.dev_audit_opt_in.load() == 1,
              "AC2: dev opt-in set");

        ta::g_typed_mutation_audit_counters.audits_considered.store(0, std::memory_order_relaxed);
        ta::g_typed_mutation_audit_counters.samples_skipped.store(0, std::memory_order_relaxed);

        int hits = 0;
        for (std::uint64_t id = 1; id <= 100; ++id) {
            if (ta::should_audit(id))
                ++hits;
        }
        const auto considered =
            ta::g_typed_mutation_audit_counters.audits_considered.load(std::memory_order_relaxed);
        const auto skipped =
            ta::g_typed_mutation_audit_counters.samples_skipped.load(std::memory_order_relaxed);
        CHECK(considered == 100, std::format("AC2: audits_considered==100 (got {})", considered));
        // mutation_id % 4 == 0 → 25 hits for ids 1..100 (4,8,...,100)
        CHECK(hits == 25, std::format("AC2: Sampled/4 hits 25/100 (got {})", hits));
        CHECK(skipped == 75, std::format("AC2: samples_skipped==75 (got {})", skipped));
        // Explicit opt-in → no default warning.
        CHECK(ta::g_typed_mutation_audit_counters.audit_strategy_default_warnings_total.load() == 0,
              "AC2: apply_dev does not warn");
        // Small non-linear under Sampled: no hard gate.
        CHECK(!ta::requires_invariant_hard_gate(1, false, false),
              "AC2: Sampled small non-linear soft path");
    }

    // ── AC3: Sampled without opt-in warns once ──
    {
        std::println("\n--- AC3: Sampled ratio>1 without opt-in → warn metric ---");
        ta::reset_for_test();
        // Strip opt-in while keeping Sampled/4 (misconfig / set_strategy path).
        ta::set_strategy(ta::AuditStrategy::Sampled);
        ta::set_sample_ratio(4);
        ta::g_typed_mutation_audit_counters.dev_audit_opt_in.store(0, std::memory_order_relaxed);
        ta::g_typed_mutation_audit_counters.production_defaults_active.store(
            0, std::memory_order_relaxed);
        ta::g_typed_mutation_audit_counters.audit_strategy_default_warnings_total.store(
            0, std::memory_order_relaxed);
        ta::g_typed_mutation_audit_counters.audit_strategy_default_warning_fired.store(
            0, std::memory_order_relaxed);

        (void)ta::should_audit(1); // skip path; triggers warn
        const auto w1 =
            ta::g_typed_mutation_audit_counters.audit_strategy_default_warnings_total.load();
        CHECK(w1 == 1, std::format("AC3: first under-sample warn (got {})", w1));
        (void)ta::should_audit(2);
        (void)ta::should_audit(3);
        const auto w2 =
            ta::g_typed_mutation_audit_counters.audit_strategy_default_warnings_total.load();
        CHECK(w2 == 1, "AC3: warn is one-shot");
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2818 query keys ---");
        ta::apply_production_audit_defaults();
        CompilerService cs;
        CHECK(href(cs, "schema-2818") == 2818, "AC4: schema-2818");
        CHECK(href(cs, "issue-2818") == 2818, "AC4: issue-2818");
        CHECK(href(cs, "audit-strategy-default-warnings-total") >= 0, "AC4: warn total key");
        CHECK(href(cs, "dev-audit-opt-in") == 0, "AC4: production clears dev opt-in");
        CHECK(href(cs, "strategy") == 2, "AC4: Full strategy=2");
        CHECK(href(cs, "samples-skipped") >= 0, "AC4: samples-skipped key");
        CHECK(href(cs, "audits-considered") >= 0, "AC4: audits-considered key");

        auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(obs.find("schema-2818") != std::string::npos, "AC4: obs cites schema-2818");
        CHECK(obs.find("audit-strategy-default-warnings-total") != std::string::npos,
              "AC4: obs warn key");
    }

    // ── #3530 AC1: production setters refuse Sampled + ratio>1 ──
    {
        std::println("\n--- #3530 AC1: production Sampled+ratio>1 setter refuse ---");
        CHECK(ta::kSampledProductionGateIssue == 3530, "3530 AC1: issue stamp");
        ta::apply_production_audit_defaults();
        ta::g_typed_mutation_audit_counters.production_sampled_ratio_deny_total.store(
            0, std::memory_order_relaxed);
        CHECK(ta::production_defaults_active(), "3530 AC1: production on");
        CHECK(ta::get_strategy() == ta::AuditStrategy::Full, "3530 AC1: Full after defaults");
        CHECK(ta::get_sample_ratio() == 1, "3530 AC1: ratio 1 after defaults");
        CHECK(ta::production_sampled_allowed(), "3530 AC1: Full is allowed");

        ta::set_strategy(ta::AuditStrategy::Sampled); // ratio=1 → allowed
        CHECK(ta::get_strategy() == ta::AuditStrategy::Sampled,
              "3530 AC1: Sampled/1 allowed under production");
        ta::set_sample_ratio(4);
        CHECK(ta::get_sample_ratio() == 1, "3530 AC1: ratio>1 refused (stays 1)");
        CHECK(ta::g_typed_mutation_audit_counters.production_sampled_ratio_deny_total.load() >= 1,
              "3530 AC1: deny_total bumped on set_sample_ratio");
        CHECK(ta::should_audit(1), "3530 AC1: Sampled/1 never skips");

        ta::apply_production_audit_defaults();
        ta::g_typed_mutation_audit_counters.production_sampled_ratio_deny_total.store(
            0, std::memory_order_relaxed);
        ta::set_sample_ratio(4); // Full ignores ratio
        CHECK(ta::get_sample_ratio() == 4, "3530 AC1: Full may hold ratio>1");
        ta::set_strategy(ta::AuditStrategy::Sampled);
        CHECK(ta::get_strategy() == ta::AuditStrategy::Full,
              "3530 AC1: Sampled refused while ratio>1 (stays Full)");
        CHECK(ta::g_typed_mutation_audit_counters.production_sampled_ratio_deny_total.load() >= 1,
              "3530 AC1: deny_total bumped on set_strategy");
    }

    // ── #3530 AC2: Soft Sampled skip is zero extra SE ──
    {
        std::println("\n--- #3530 AC2: Soft Sampled skip zero-cost (no SE) ---");
        ta::reset_for_test();
        CHECK(ta::get_strategy() == ta::AuditStrategy::Sampled, "3530 AC2: Sampled");
        CHECK(ta::get_sample_ratio() == 4, "3530 AC2: ratio 4");
        CHECK(!ta::production_defaults_active(), "3530 AC2: production off");
        reset_security_event_ring_for_test();
        const auto seq0 = g_security_event_ring().seq.load(std::memory_order_relaxed);
        const auto total0 = g_security_event_ring().total.load(std::memory_order_relaxed);
        CHECK(!ta::should_audit(1), "3530 AC2: mid 1 skipped");
        CHECK(!ta::should_audit(2), "3530 AC2: mid 2 skipped");
        CHECK(!ta::should_audit(3), "3530 AC2: mid 3 skipped");
        CHECK(ta::should_audit(4), "3530 AC2: mid 4 hit");
        CHECK(g_security_event_ring().seq.load(std::memory_order_relaxed) == seq0,
              "3530 AC2: no SE seq bump on Soft skip");
        CHECK(g_security_event_ring().total.load(std::memory_order_relaxed) == total0,
              "3530 AC2: no SE total bump on Soft skip");
    }

    // ── #3530 AC3: leftover production Sampled skip emits joinable SE ──
    {
        std::println("\n--- #3530 AC3: production leftover skip → audit-skipped SE ---");
        ta::apply_production_audit_defaults();
        ta::inject_sampled_ratio_for_test(4);
        CHECK(ta::production_defaults_active(), "3530 AC3: production on");
        CHECK(ta::get_strategy() == ta::AuditStrategy::Sampled, "3530 AC3: leftover Sampled");
        CHECK(ta::get_sample_ratio() == 4, "3530 AC3: leftover ratio 4");
        reset_security_event_ring_for_test();
        ta::g_typed_mutation_audit_counters.samples_skipped.store(0, std::memory_order_relaxed);

        constexpr int kN = 10000;
        int hits = 0;
        int skips = 0;
        for (std::uint64_t id = 1; id <= static_cast<std::uint64_t>(kN); ++id) {
            if (ta::should_audit(id))
                ++hits;
            else
                ++skips;
        }
        CHECK(hits == kN / 4, std::format("3530 AC3: hits 2500/10000 (got {})", hits));
        CHECK(skips == kN - kN / 4, std::format("3530 AC3: skips 7500 (got {})", skips));
        CHECK(ta::g_typed_mutation_audit_counters.samples_skipped.load() ==
                  static_cast<std::uint64_t>(skips),
              "3530 AC3: samples_skipped matches");
        const auto se_total = g_security_event_ring().total.load(std::memory_order_relaxed);
        CHECK(se_total == static_cast<std::uint64_t>(skips),
              std::format("3530 AC3: SE total == skip count (got {})", se_total));

        // Last skip mid=9999 (9999 % 4 != 0) is joinable in the ring window.
        const auto seq = g_security_event_ring().seq.load(std::memory_order_relaxed);
        bool found_skip = false;
        const std::size_t n = std::min<std::size_t>(seq, kSecurityEventRingSize);
        for (std::size_t i = 0; i < n; ++i) {
            const auto& e = g_security_event_ring().ring[i];
            if (std::string_view(e.op) == "audit-skipped" &&
                std::string_view(e.reason) == "sampled-ratio-skip" && e.mutation_id != 0 &&
                !e.denied) {
                found_skip = true;
                break;
            }
        }
        CHECK(found_skip, "3530 AC3: ring has audit-skipped / sampled-ratio-skip + mid");
    }

    // ── #3530 AC4: additive query keys; no invent ──
    {
        std::println("\n--- #3530 AC4: schema-3530 additive; no new query:* ---");
        ta::apply_production_audit_defaults();
        CompilerService cs;
        CHECK(href(cs, "schema-3530") == 3530, "3530 AC4: schema-3530");
        CHECK(href(cs, "issue-3530") == 3530, "3530 AC4: issue-3530");
        CHECK(href(cs, "production-sampled-ratio-deny-total") >= 0, "3530 AC4: deny total key");
        CHECK(href(cs, "production-sampled-ratio-deny-wired") == 1, "3530 AC4: wired");
        CHECK(href(cs, "sampled-ratio-skip-se-wired") == 1, "3530 AC4: skip SE wired");
        CHECK(href(cs, "schema-2818") == 2818, "3530 AC4: existing schema-2818 retained");

        const auto h = read_file("src/compiler/typed_mutation_audit.h");
        CHECK(h.find("kSampledProductionGateIssue = 3530") != std::string::npos,
              "3530 AC4: stamp in SSOT");
        CHECK(h.find("sampled-ratio-skip") != std::string::npos, "3530 AC4: skip reason");
        CHECK(h.find("audit-skipped") != std::string::npos, "3530 AC4: skip op");
        CHECK(h.find("production_sampled_ratio_deny_total") != std::string::npos,
              "3530 AC4: counter END");
        CHECK(h.find("production_sampled_allowed") != std::string::npos, "3530 AC4: gate helper");
        const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(obs.find("\"query:sampled-ratio-skip\"") == std::string::npos,
              "3530 AC4: no new query:* name");
        CHECK(obs.find("schema-3530") != std::string::npos, "3530 AC4: obs cites schema-3530");
        CHECK(read_file("tests/compiler/test_issue_3530.cpp").empty(),
              "3530 AC4: no test_issue_3530.cpp");
        CHECK(read_file("tests/compiler/test_should_audit_sampled_skipped_se.cpp").empty(),
              "3530 AC4: folded into existing should_audit test");
        CHECK(read_file("docs/design/3530-sampled-ratio-skip.md").empty(),
              "3530 AC4: no docs/design/3530-*");
        CHECK(read_file("scripts/coverage/checks/check_sampled_production_gate.py").empty(),
              "3530 AC4: no substring-only py linter");
    }

    // Leave process on dev Sampled for subsequent tests in shared binaries.
    ta::reset_for_test();

    std::println("\n=== #2818/#3530 should_audit Full default: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_should_audit_sampled_default();
}
#endif
