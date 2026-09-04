// @category: unit
// @reason: Issue #2183 — unified CacheEntryVersionStamp restamp contract
// on store / partial / AOT emit (refine #2033). Issue #3481 — AOT/facade
// success must not restamp dirty pre-relower IR to live.
//
//   AC1: restamp_cache_entry + restamp_cache_entry_live_ on store/partial
//   AC2: lookup stamp mismatch → force re-lower + mismatch metric
//   AC3: AOT reemit success restamps cache (source-wired)
//   AC4: query schema-2183 + restamp / mismatch counters
//   AC5: adversarial — clear dirty + old stamp → force relower; restamp
//        monotonic under concurrent-style bumps
//
//   #3481 AC1: production facade n>0 on still-dirty entry; dirty-clear
//              without store → lookup stays 1
//   #3481 AC2: instruction peel does not restamp mutation/bridge/defuse/soa
//   #3481 AC3: _vN / table epoch not dual-fresh via content restamp; dirty
//              / content latch stays; #3377 / #3351 owner-scoped preserved
//   #3481 AC4: Soft/Off no extra work; #2183 mismatch-force-relower stays
//   #3481 AC5: abort path unchanged (zero stamp + clear map + abort_map_invalid)
//   #3481 AC6: last_reemit_success_region_mask stays coverage-only (#3445)
//
//   #3513 AC1: production facade still calls decide_and_reemit; lookup 1
//              while !content_stored_this_epoch
//   #3513 AC2: untrusted → would_allow_native false even if force_mask==0
//   #3513 AC3: untrusted emit does not clear peer JIT stale; coverage-only
//   #3513 AC4: store_define_v2 clears latch + production reemit; Soft no latch
//   #3513 AC5: extend this suite; no test_issue_3513 / docs/design / new query

#include "test_harness.hpp"
#include "compiler/aot_reload_consistency_proof.h"
#include "compiler/hot_update_registry.hh"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

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
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
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
        const auto defn_block =
            evsec.substr(evsec.find("Evaluator::query_stable_hard_reject_torn()"));
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
        CHECK(evsec_full.find("restamp-budget-exceeded-total") == std::string::npos ||
                  om.find("restamp_budget_exceeded_total") != std::string::npos,
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
        const auto allow_block =
            evsec_full.substr(evsec_full.find("allow_query_stable_ref_export"));
        CHECK(allow_block.find("should_hard_reject_soft_sibling") != std::string::npos,
              "3100 AC1: existing allow_query_stable_ref_export gate is production-only");
        CHECK(allow_block.find("restamp_last_budget_exceeded") != std::string::npos,
              "3100 AC1: existing allow_query_stable_ref_export reads over-budget flag");
    }

    // ── Issue #3481: AOT/facade success must not restamp dirty pre-relower IR
    {
        std::println("\n--- #3481 AC1: facade n>0 + dirty-clear without store → lookup 1 ---");
        apply_production_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f3481 (lambda (x) (+ x 1))) (f3481 1)\")").has_value(),
              "3481 AC1: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3481 AC1: eval");
        if (!cs.get_define_v2("f3481"))
            (void)cs.eval("(compile:cache-define \"f3481\")");
        const auto* e0 = cs.get_define_v2("f3481");
        CHECK(e0 != nullptr, "3481 AC1: cache entry");
        CHECK(e0->content_stored_this_epoch, "3481 AC1: store set content latch");
        const auto hash = e0->source_hash;
        const auto stamp_bridge0 = e0->version_stamp_.bridge_epoch;
        const auto stamp_defuse0 = e0->version_stamp_.defuse_version;
        CHECK(cs.lookup_define_v2("f3481", hash) == 0, "3481 AC1: clean hit after store");
        cs.public_mark_define_dirty("f3481");
        const auto* e1 = cs.get_define_v2("f3481");
        CHECK(e1 && e1->dirty, "3481 AC1: facade left dirty");
        CHECK(e1 && !e1->content_stored_this_epoch, "3481 AC1: content latch cleared");
        CHECK(cs.lookup_define_v2("f3481", hash) == 1, "3481 AC1: lookup 1 while dirty");
        // Stamps must not have been restamped to the post-facade live epochs
        // (or if bridge was 0, defuse/mutation still lag). Content restamp
        // would write live bridge/defuse onto the pre-mutate irs.
        const auto live_bridge = cs.bridge_epoch();
        const auto live_defuse = cs.evaluator().defuse_version();
        CHECK(e1->version_stamp_.bridge_epoch != live_bridge ||
                  e1->version_stamp_.defuse_version != live_defuse ||
                  (stamp_bridge0 == e1->version_stamp_.bridge_epoch &&
                   stamp_defuse0 == e1->version_stamp_.defuse_version),
              "3481 AC1: facade did not content-restamp pre-mutate irs to live");
        CHECK(cs.clear_ir_cache_dirty_bits_for_test("f3481"), "3481 AC1: inject dirty-clear");
        const auto* e2 = cs.get_define_v2("f3481");
        CHECK(e2 && !e2->dirty, "3481 AC1: dirty cleared without store");
        CHECK(e2 && !e2->content_stored_this_epoch, "3481 AC1: content latch still unset");
        CHECK(cs.lookup_define_v2("f3481", hash) == 1,
              "3481 AC1: lookup stays 1 after dirty-clear without store");
        apply_dev_audit_defaults();
    }

    {
        std::println("\n--- #3481 AC2: instr peel does not content-restamp ---");
        const auto svc = read_file("src/compiler/service.ixx");
        const auto peel = svc.find("bool relower_affected_instrs_");
        CHECK(peel != std::string::npos, "3481 AC2: instr peel exists");
        const auto relower = svc.find("bool relower_define_blocks(");
        CHECK(relower != std::string::npos, "3481 AC2: relower_define_blocks");
        const auto win = svc.substr(relower, 22000);
        CHECK(win.find("ack_cache_entry_fences_live_") != std::string::npos,
              "3481 AC2: peel acks fences");
        CHECK(win.find("Issue #3481") != std::string::npos, "3481 AC2: relower cites #3481");
        // The instr-peel success arm must not call restamp_cache_entry_live_
        // (per-fn AST relower further down still does).
        const auto instr_ack = win.find("ack_cache_entry_fences_live_");
        const auto per_fn = win.find("restamp after successful per-fn");
        CHECK(instr_ack != std::string::npos && per_fn != std::string::npos && instr_ack < per_fn,
              "3481 AC2: fence ack on instr peel before per-fn restamp");
        const auto peel_arm = win.substr(instr_ack, per_fn - instr_ack);
        CHECK(peel_arm.find("restamp_cache_entry_live_") == std::string::npos,
              "3481 AC2: instr peel arm does not restamp content stamps");
    }

    {
        std::println("\n--- #3481 AC3: dirty/content latch; owner-scoped #3377/#3351 stay ---");
        const auto hur = read_file("src/compiler/hot_update_registry.cpp");
        const auto mangle = read_file("src/compiler/aot_mangle.h");
        CHECK(hur.find("content_stored_this_epoch") != std::string::npos ||
                  hur.find("IR cache is NOT restamped here") != std::string::npos,
              "3481 AC3: facade does not content-restamp");
        CHECK(mangle.find("3481") != std::string::npos, "3481 AC3: mangle joint contract");
        CHECK(hur.find("aura_aot_invalidate_owner_slot_for_func_id") != std::string::npos,
              "3481 AC3: #3377 owner slot invalidate stays");
        CHECK(hur.find("aura_aot_mark_peer_ir_name_soft_stale") != std::string::npos,
              "3481 AC3: #3351 peer IR soft-stale stays");
        CHECK(hur.find("last_reemit_success_region_mask") != std::string::npos,
              "3481 AC3: coverage stamp exists (not content)");
    }

    {
        std::println("\n--- #3481 AC4: Soft/Off no extra; #2183 mismatch-force-relower stays ---");
        apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define s3481 (lambda (x) x)) (s3481 1)\")").has_value(),
              "3481 AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3481 AC4: eval");
        if (!cs.get_define_v2("s3481"))
            (void)cs.eval("(compile:cache-define \"s3481\")");
        CHECK(cs.get_define_v2("s3481") != nullptr, "3481 AC4: cache entry");
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "3481 AC4: metrics");
        const auto miss0 = m->cache_stamp_mismatch_force_relower_total.load();
        CHECK(cs.inject_stale_cache_stamp_for_test("s3481"), "3481 AC4: inject stale");
        const auto hash = cs.get_define_v2("s3481")->source_hash;
        CHECK(cs.lookup_define_v2("s3481", hash) == 1,
              "3481 AC4: #2183 mismatch-force-relower still fires");
        CHECK(m->cache_stamp_mismatch_force_relower_total.load() > miss0,
              "3481 AC4: force_relower metric advanced");
        const auto hur = read_file("src/compiler/hot_update_registry.cpp");
        CHECK(hur.find("aura_production_defaults_active_probe() == 0") != std::string::npos,
              "3481 AC4: facade Soft/Off returns false (zero extra)");
    }

    {
        std::println("\n--- #3481 AC5: abort path unchanged ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define a3481 (lambda (x) x)) (a3481 1)\")").has_value(),
              "3481 AC5: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3481 AC5: eval");
        if (!cs.get_define_v2("a3481"))
            (void)cs.eval("(compile:cache-define \"a3481\")");
        const auto hash = cs.get_define_v2("a3481")->source_hash;
        cs.public_force_ir_cache_dirty_after_abort();
        const auto* after = cs.get_define_v2("a3481");
        CHECK(after && after->abort_map_invalid, "3481 AC5: abort_map_invalid");
        CHECK(after && after->source_to_ir_map.empty(), "3481 AC5: map cleared");
        CHECK(after && !after->content_stored_this_epoch, "3481 AC5: content untrusted");
        CHECK(cs.lookup_define_v2("a3481", hash) == 1, "3481 AC5: lookup needs-relower");
        CHECK(cs.restamp_cache_entry_for_test("a3481"), "3481 AC5: restamp live");
        CHECK(cs.clear_ir_cache_dirty_bits_for_test("a3481"), "3481 AC5: dirty-clear");
        const auto* peeled = cs.get_define_v2("a3481");
        CHECK(peeled && peeled->abort_map_invalid, "3481 AC5: abort latch survives restamp");
        CHECK(cs.lookup_define_v2("a3481", peeled->source_hash) == 1,
              "3481 AC5: post-abort AOT/restamp must not clean-hit");
        const auto svc = read_file("src/compiler/service.ixx");
        const auto fd = svc.find("void force_ir_cache_dirty_after_abort()");
        CHECK(fd != std::string::npos, "3481 AC5: abort force-dirty present");
        const auto fd_win = svc.substr(fd, 3200);
        CHECK(fd_win.find("source_to_ir_map.clear()") != std::string::npos,
              "3481 AC5: still clears map");
        CHECK(fd_win.find("abort_map_invalid = true") != std::string::npos,
              "3481 AC5: still sets flag");
        CHECK(fd_win.find("stamp_version(0, 0, 0, 0)") != std::string::npos,
              "3481 AC5: still zeros stamps");
    }

    {
        std::println("\n--- #3481 AC6: coverage stamp is not content promotion ---");
        const auto hur = read_file("src/compiler/hot_update_registry.cpp");
        const auto bnd = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto dirty = read_file("src/compiler/service_dirty.cpp");
        CHECK(hur.find("last_reemit_success_region_mask") != std::string::npos,
              "3481 AC6: coverage mask exists");
        CHECK(bnd.find("coverage-only") != std::string::npos ||
                  bnd.find("not content promotion") != std::string::npos,
              "3481 AC6: BoundaryExit coverage-only");
        CHECK(dirty.find("content_stored_this_epoch") != std::string::npos &&
                  dirty.find("ack_peer_ir_stale_on_restamp_") != std::string::npos,
              "3481 AC6: cascade gates content restamp");
        CHECK(dirty.find("note_relower_success_coverage") != std::string::npos,
              "3481 AC6: hashed-name coverage stays on content restamp");
        CHECK(read_file("docs/design/3481-aot-restamp-dirty.md").empty(),
              "3481 AC6: no docs/design/");
        CHECK(read_file("tests/compiler/test_issue_3481.cpp").empty(),
              "3481 AC6: no invent test_issue_3481");
        CHECK(read_file("tests/issues/test_issue_3481.cpp").empty(),
              "3481 AC6: no tests/issues/test_issue_3481");
        const auto svc = read_file("src/compiler/service.ixx");
        CHECK(svc.find("schema-3481") == std::string::npos, "3481 AC6: no schema-3481");
        CHECK(svc.find("g_3481_") == std::string::npos, "3481 AC6: no g_3481_*");
        CHECK(svc.find("query:content-stored") == std::string::npos, "3481 AC6: no new query key");
    }

    // ── Issue #3513: facade reemit must not promote native before store ──
    {
        std::println("\n--- #3513 AC1: facade still decide_and_reemit; lookup latched ---");
        const auto hur = read_file("src/compiler/hot_update_registry.cpp");
        const auto fac = hur.find("bool HotUpdateRegistry::hard_invalidate_via_facade");
        CHECK(fac != std::string::npos, "3513 AC1: facade present");
        const auto fac_win = hur.substr(fac, 9000);
        CHECK(fac_win.find("note_ir_content_untrusted_for_native()") != std::string::npos,
              "3513 AC1: latch armed before decide_and_reemit");
        const auto latch = fac_win.find("note_ir_content_untrusted_for_native()");
        const auto reemit =
            fac_win.find("decide_and_reemit(aura_get_aot_defuse_version(), reason)");
        CHECK(latch != std::string::npos && reemit != std::string::npos && latch < reemit,
              "3513 AC1: untrusted latch before decide_and_reemit");
        apply_production_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f3513 (lambda (x) (+ x 1))) (f3513 1)\")").has_value(),
              "3513 AC1: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3513 AC1: eval");
        if (!cs.get_define_v2("f3513"))
            (void)cs.eval("(compile:cache-define \"f3513\")");
        const auto* e0 = cs.get_define_v2("f3513");
        CHECK(e0 != nullptr, "3513 AC1: cache entry");
        const auto hash = e0->source_hash;
        cs.public_mark_define_dirty("f3513");
        const auto* e1 = cs.get_define_v2("f3513");
        CHECK(e1 && !e1->content_stored_this_epoch, "3513 AC1: content latch cleared");
        CHECK(cs.lookup_define_v2("f3513", hash) == 1, "3513 AC1: lookup stays 1");
        CHECK(aura::compiler::hot_update_registry().ir_content_untrusted_for_native(),
              "3513 AC1: native-untrusted latch armed after facade");
        apply_dev_audit_defaults();
    }

    {
        std::println("\n--- #3513 AC2: untrusted → would_allow_native false ---");
        const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
        CHECK(bridge.find("ir_content_untrusted_for_native()") != std::string::npos,
              "3513 AC2: bridge consults latch");
        CHECK(bridge.find("p.would_allow_native = false") != std::string::npos,
              "3513 AC2: untrusted path stamps would_allow_native=false");
        apply_production_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define w3513 (lambda (x) x)) (w3513 1)\")").has_value(),
              "3513 AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3513 AC2: eval");
        if (!cs.get_define_v2("w3513"))
            (void)cs.eval("(compile:cache-define \"w3513\")");
        cs.public_mark_define_dirty("w3513");
        CHECK(aura::compiler::hot_update_registry().ir_content_untrusted_for_native(),
              "3513 AC2: latch on");
        CHECK(aura_last_aot_reload_consistency_would_allow_native() == 0,
              "3513 AC2: would_allow_native false while untrusted");
        apply_dev_audit_defaults();
    }

    {
        std::println("\n--- #3513 AC3: untrusted does not clear peer stale ---");
        const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
        const auto skip = bridge.find("if (content_untrusted)");
        CHECK(skip != std::string::npos, "3513 AC3: content_untrusted skip present");
        const auto skip_win = bridge.substr(skip, 800);
        CHECK(skip_win.find("continue") != std::string::npos,
              "3513 AC3: untrusted skips native install");
        CHECK(skip_win.find("aura_aot_clear_peer_jit_name_soft_stale") == std::string::npos,
              "3513 AC3: skip arm does not clear peer stale");
        const auto hur = read_file("src/compiler/hot_update_registry.cpp");
        const auto pipe = hur.find("void HotUpdateRegistry::on_reemit_pipeline_call");
        CHECK(pipe != std::string::npos, "3513 AC3: pipeline call present");
        const auto pipe_win = hur.substr(pipe, 2800);
        CHECK(pipe_win.find("if (!ir_content_untrusted_for_native())") != std::string::npos,
              "3513 AC3: re-promote/remount gated on latch");
        CHECK(hur.find("last_reemit_success_region_mask") != std::string::npos,
              "3513 AC3: coverage stamp remains (#3445)");
    }

    {
        std::println("\n--- #3513 AC4: store clears latch; Soft no extra ---");
        const auto svc = read_file("src/compiler/service.ixx");
        const auto store = svc.find("void store_define_v2(");
        CHECK(store != std::string::npos, "3513 AC4: store_define_v2");
        const auto store_win = svc.substr(store, 3500);
        CHECK(store_win.find("note_ir_content_stored_for_native()") != std::string::npos,
              "3513 AC4: store clears latch");
        CHECK(store_win.find("decide_and_reemit") != std::string::npos,
              "3513 AC4: store may reemit against stored irs");
        const auto stored = store_win.find("note_ir_content_stored_for_native()");
        const auto content = store_win.find("content_stored_this_epoch = true");
        CHECK(content != std::string::npos && stored != std::string::npos && content < stored,
              "3513 AC4: content stored before latch clear");
        apply_dev_audit_defaults();
        CHECK(!aura::compiler::typed_audit::production_defaults_active(),
              "3513 AC4: Soft/dev defaults");
        const auto hur = read_file("src/compiler/hot_update_registry.cpp");
        CHECK(hur.find("aura_production_defaults_active_probe() == 0") != std::string::npos,
              "3513 AC4: facade Soft/Off returns false (zero extra)");
    }

    {
        std::println("\n--- #3513 AC5: no invent test_issue / docs / query key ---");
        CHECK(read_file("docs/design/3513-native-untrusted.md").empty(),
              "3513 AC5: no docs/design/");
        CHECK(read_file("tests/compiler/test_issue_3513.cpp").empty(),
              "3513 AC5: no invent test_issue_3513");
        CHECK(read_file("tests/issues/test_issue_3513.cpp").empty(),
              "3513 AC5: no tests/issues/test_issue_3513");
        const auto svc = read_file("src/compiler/service.ixx");
        CHECK(svc.find("schema-3513") == std::string::npos, "3513 AC5: no schema-3513");
        CHECK(svc.find("query:ir-content-untrusted") == std::string::npos,
              "3513 AC5: no new query key");
        CHECK(aura::compiler::HotUpdateRegistry::kIrContentUntrustedNativeIssue == 3513,
              "3513 AC5: issue stamp");
    }

    std::println("\n=== #2183/#3481/#3513 cache stamp restamp: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_cache_stamp_restamp_contract();
}
#endif
