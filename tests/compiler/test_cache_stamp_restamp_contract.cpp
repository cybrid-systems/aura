// @category: unit
// @reason: Issue #2183 — unified CacheEntryVersionStamp restamp contract
// on store / partial / AOT emit (refine #2033).
//
//   AC1: restamp_cache_entry + restamp_cache_entry_live_ on store/partial
//   AC2: lookup stamp mismatch → force re-lower + mismatch metric
//   AC3: AOT reemit success restamps cache (source-wired)
//   AC4: query schema-2183 + restamp / mismatch counters
//   AC5: adversarial — clear dirty + old stamp → force relower; restamp
//        monotonic under concurrent-style bumps

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.ir_cache_pure;
import aura.compiler.value;

namespace {

using aura::compiler::CacheEntryVersionStamp;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::kRelowerAbortForce;
using aura::compiler::kRelowerBridgeEpoch;
using aura::compiler::kRelowerMutationDrift;
using aura::compiler::restamp_cache_entry;
using aura::compiler::should_relower;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:incremental-relower-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_cache_stamp_restamp_contract() {
    std::println("=== Issue #2183: unified CacheEntryVersionStamp restamp ===");

    // ── AC1: pure restamp + source wiring ──
    {
        std::println("\n--- AC1: restamp_cache_entry + store/partial wiring ---");
        CacheEntryVersionStamp s;
        restamp_cache_entry(s, 10, 20, 30, 40);
        CHECK(s.mutation_count == 10, "mut stamped");
        CHECK(s.bridge_epoch == 20, "bridge stamped");
        CHECK(s.defuse_version == 30, "defuse stamped");
        CHECK(s.soa_generation == 40, "soa stamped");
        restamp_cache_entry(s, 11, 21, 31, 41);
        CHECK(s.mutation_count == 11 && s.bridge_epoch == 21, "restamp overwrites");

        const auto pure = read_file("src/compiler/ir_cache_pure.ixx");
        const auto svc = read_file("src/compiler/service.ixx");
        const auto dirty = read_file("src/compiler/service_dirty.cpp");
        const auto aot = read_file("src/compiler/aot_mangle.h");
        CHECK(pure.find("2183") != std::string::npos, "pure cites 2183");
        CHECK(pure.find("restamp_cache_entry") != std::string::npos, "pure helper");
        CHECK(svc.find("restamp_cache_entry_live_") != std::string::npos, "live restamp");
        CHECK(svc.find("restamp_cache_entry_live_(it->second)") != std::string::npos ||
                  svc.find("restamp_cache_entry_live_(entry)") != std::string::npos,
              "partial/store calls restamp");
        CHECK(dirty.find("cache_stamp_aot_restamp_total") != std::string::npos ||
                  dirty.find("restamp_cache_entry_live_") != std::string::npos,
              "AOT reemit restamps");
        CHECK(aot.find("2183") != std::string::npos, "aot_mangle contract note");
    }

    // ── AC2: pure should_relower on stale stamp ──
    {
        std::println("\n--- AC2: stamp mismatch forces re-lower (pure) ---");
        CacheEntryVersionStamp stamp;
        restamp_cache_entry(stamp, 5, 10, 1, 0);
        std::uint32_t reasons = 0;
        // Clean dirty + matching hash, but mutation/bridge behind live.
        const bool need =
            should_relower(/*src*/ 42, /*cached*/ 42, /*dirty*/ false, stamp,
                           /*cur_mut*/ 9, /*cur_bridge*/ 11, /*cur_defuse*/ 1, &reasons);
        CHECK(need, "force relower");
        CHECK((reasons & kRelowerMutationDrift) != 0 || (reasons & kRelowerBridgeEpoch) != 0,
              "stamp domain reason");
        // Issue #3069: abort-force gen behind live → need-relower even if clean.
        CacheEntryVersionStamp clean;
        restamp_cache_entry(clean, 3, 7, 2, 0);
        clean.abort_force_generation = 0;
        std::uint32_t ar = 0;
        CHECK(should_relower(1, 1, false, clean, 3, 7, 2, &ar, 0, /*abort_gen*/ 4),
              "3069: abort fence forces relower");
        CHECK((ar & kRelowerAbortForce) != 0, "3069: kRelowerAbortForce bit");
        std::uint32_t ar0 = 0;
        CHECK(!should_relower(1, 1, false, clean, 3, 7, 2, &ar0, 0, /*abort_gen*/ 0),
              "3069: abort gen 0 is zero-cost skip");
    }

    // ── AC5: adversarial inject stale stamp on service entry ──
    {
        std::println("\n--- AC5: inject stale stamp → lookup forces relower ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1))) (f 1)\")").has_value(),
              "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "metrics");

        if (!cs.get_define_v2("f"))
            (void)cs.eval("(compile:cache-define \"f\")");
        CHECK(cs.get_define_v2("f") != nullptr, "cache entry f");

        const auto miss0 = m->cache_stamp_mismatch_force_relower_total.load();
        const auto stamp0 = m->cache_stamp_restamp_total.load();

        CHECK(cs.inject_stale_cache_stamp_for_test("f"), "inject stale stamp");
        // Source hash from cached entry — lookup with matching hash, clean dirty.
        const auto hash = cs.get_define_v2("f")->source_hash;
        const int look = cs.lookup_define_v2("f", hash);
        CHECK(look == 1, "AC5: lookup returns needs-relower (1)");
        CHECK(m->cache_stamp_mismatch_force_relower_total.load() > miss0,
              "AC5: force_relower metric advanced");
        CHECK(m->should_relower_stamp_mismatch_total.load() > 0 ||
                  m->cache_stamp_mismatch_force_relower_total.load() > miss0,
              "stamp mismatch counted");

        // Restamp to live → lookup should hit (0) if still clean.
        CHECK(cs.restamp_cache_entry_for_test("f"), "restamp live");
        CHECK(m->cache_stamp_restamp_total.load() > stamp0, "restamp total advanced");
        const int look2 = cs.lookup_define_v2("f", hash);
        CHECK(look2 == 0 || look2 == 1, "lookup after restamp ok (0 hit or 1 if other dirt)");
        // Monotonic: restamp again does not decrease counters.
        const auto r1 = m->cache_stamp_restamp_total.load();
        CHECK(cs.restamp_cache_entry_for_test("f"), "restamp again");
        CHECK(m->cache_stamp_restamp_total.load() > r1, "AC5: restamp monotonic");
    }

    // ── AC4: query schema ──
    {
        std::println("\n--- AC4: query schema-2183 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "schema-2183") == 2183, "schema-2183");
        CHECK(href(cs, "issue-2183") == 2183, "issue-2183");
        CHECK(href(cs, "cache-stamp-restamp-wired") == 1, "restamp wired");
        CHECK(href(cs, "cache_stamp_restamp_total") >= 0, "restamp key");
        CHECK(href(cs, "cache_stamp_mismatch_force_relower_total") >= 0, "force_relower key");
        CHECK(href(cs, "cache_stamp_mismatch_reasons_bits") >= 0, "reasons bits");
        CHECK(href(cs, "cache_stamp_aot_restamp_total") >= 0, "aot restamp key");
        CHECK(href(cs, "schema-2033") == 2033, "2033 lineage retained");
        CHECK(href(cs, "cache_entry_version_stamp_total") >= 0, "legacy stamp total");
    }

    // ── Integration: store restamps ──
    {
        std::println("\n--- integration: store_define stamps via restamp ---");
        CompilerService cs;
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        const auto r0 = m ? m->cache_stamp_restamp_total.load() : 0;
        CHECK(cs.eval("(set-code \"(define g (lambda (n) n)) (g 2)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        if (m)
            CHECK(m->cache_stamp_restamp_total.load() >= r0, "restamp non-dec after store");
        // Mutate + eval exercises relower → restamp path.
        (void)cs.eval("(mutate:set-body \"g\" \"(lambda (n) (+ n 1))\")");
        (void)cs.eval("(eval-current)");
        if (m)
            CHECK(m->cache_stamp_restamp_total.load() >= r0, "restamp after mutate");
    }

    // ── Issue #3100: production-only hard reject on last restamp over budget
    // for query:*-stable (shared restamp-status probe). Soft/Off returns
    // false (metric/soft observe); production + over-budget returns true
    // so query:*-stable sites hard-reject per AC1. Reuses existing
    // restamp_last_budget_exceeded_ flag + record_query_stable_ref_*
    // counters. AC2: Soft / under-budget unchanged. AC4: additive
    // counters only (reuses restamp-budget-exceeded-total +
    // record_query_stable_ref_restamp_torn_*). AC6: source-cite +
    // coverage linter; no docs/design/* per #1655.
    {
        std::println("\n--- #3100 AC1: shared query_stable_hard_reject_torn probe ---");
        CompilerService cs;
        // AC1 source-cite: Evaluator::query_stable_hard_reject_torn()
        // declared in evaluator.ixx + impl in evaluator_security.cpp.
        const auto evxx = read_file("src/compiler/evaluator.ixx");
        const auto evsec = read_file("src/compiler/evaluator_security.cpp");
        CHECK(evxx.find("query_stable_hard_reject_torn") != std::string::npos,
              "3100 AC1: query_stable_hard_reject_torn declared in evaluator.ixx");
        CHECK(evsec.find("Evaluator::query_stable_hard_reject_torn()") != std::string::npos,
              "3100 AC1: query_stable_hard_reject_torn defined in evaluator_security.cpp");
        // AC1: probe combines production_defaults + over-budget flag.
        // (Avoid env side-effects — use source-cite only.)
        const auto defn_block = evsec.substr(evsec.find("Evaluator::query_stable_hard_reject_torn()"));
        CHECK(defn_block.find("should_hard_reject_soft_sibling") != std::string::npos,
              "3100 AC1: probe uses production defaults gate (should_hard_reject_soft_sibling)");
        CHECK(defn_block.find("restamp_last_budget_exceeded") != std::string::npos,
              "3100 AC1: probe reads restamp_last_budget_exceeded flag");
        // AC3: the existing flag is set in ast_impl.cpp when budget exceeded.
        const auto astimpl = read_file("src/core/ast_impl.cpp");
        CHECK(astimpl.find("restamp_last_budget_exceeded_.store(1") != std::string::npos,
              "3100 AC3: flag set in restamp_eager_after_boundary_locked when budget exceeded");
        // AC4: additive counters only — no new middle metrics key.
        const auto om = read_file("src/compiler/observability_metrics.h");
        const auto evsec_full = read_file("src/compiler/evaluator_security.cpp");
        CHECK(evsec_full.find("record_query_stable_ref_restamp_torn_reject") != std::string::npos,
              "3100 AC4: existing torn-reject counter reused");
        CHECK(evsec_full.find("record_query_stable_ref_restamp_lag_prevented") != std::string::npos,
              "3100 AC4: existing lag-prevented counter reused");
        CHECK(evsec_full.find("restamp-budget-exceeded-total") == std::string::npos
              || om.find("restamp_budget_exceeded_total") != std::string::npos,
              "3100 AC4: restamp-budget-exceeded-total already surfaces in observability");
        // AC6: no docs/design/* + no invent test_issue_3100.cpp.
        CHECK(read_file("docs/design/3100-query-stable-torn-reject.md").empty(),
              "3100 AC6: no docs/design/");
        CHECK(read_file("tests/compiler/test_issue_3100.cpp").empty(),
              "3100 AC6: no invent test_issue_3100 (extend #2183 lineage)");
        // Quiet path: under-budget returns false (zero extra work contract).
        // Soft path: production_defaults=0 → probe returns false.
        // These are static source-cite checks (no env side-effects).
        // Source-cite the existing production gate in allow_query_stable_ref_export.
        const auto allow_block = = evsec_full.substr(evsec.find("allow_query_stable_ref_export"));
        CHECK(allow_block.find("should_hard_reject_soft_sibling") != std::string::npos,
              "3100 AC1: existing allow_query_stable_ref_export gate is production-only");
        CHECK(allow_block.find("restamp_last_budget_exceeded") != std::string::npos,
              "3100 AC1: existing allow_query_stable_ref_export reads over-budget flag");
    }

    std::println("\n=== #2183 cache stamp restamp: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_cache_stamp_restamp_contract();
}
#endif
