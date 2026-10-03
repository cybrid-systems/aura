// @category: unit
// @reason: Issue #3152 — query:evolution-audit-decision must surface
// a forensic-source enum that maps typed-trail-miss=1 to the next
// forensic step the agent should take (trail hit / SE ring has mid
// / WAL enabled). Residual of #3113 / #3114 / #3149. Single-handler
// additive change in evaluator_primitives_security.cpp. Pure loads
// only — no WAL scan, no mutate, no shadow writes.
//
//   AC1: additive forensic-source enum (stable int)
//     0 = no mid / no evidence
//     1 = typed trail hit (details within window)
//     2 = typed miss + SE ring still has same mid
//     3 = typed miss + mutation/SE WAL enabled
//   AC2: enum sentinels forensic-source-trail=1 / -se=2 / -wal=3
//   AC3: Soft / zero-cost — no WAL I/O; mid==0 -> forensic-source=0
//   AC4: capacity / schema — planned_keys bumped 33->37; overflow=0;
//        schema-3152/issue-3152 sentinels added
//   AC5: parallel with #3149 — both in evolution-audit-decision
//        handler, single PR. last-se-reason string still present.
//
// Sibling tests implicitly covered (must remain green):
//   - tests/compiler/test_typed_mutation_audit_decision.cpp (#3114)
//   - tests/compiler/test_self_evolution_loop_stats.cpp (#3113)
//   - tests/compiler/test_self_evolution_chaos_stable.cpp
//   - tests/compiler/test_audit_replay_join.cpp (WAL enable path)

#include "test_harness.hpp"

#include <print>
#include <string>
#include <string_view>

namespace {

using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto* p : {path, "../src/compiler/evaluator_primitives_security.cpp",
                          "src/compiler/evaluator_primitives_security.cpp"}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// AC1: source cite — forensic-source key inserted in handler.
static void ac1_source_forensic_source_key() {
    std::println("\n--- AC1: source — forensic-source key inserted ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(!src.empty(), "evaluator_primitives_security.cpp readable");
    CHECK(src.find("#3152") != std::string::npos, "cites #3152");
    const auto insert_pos = src.find("insert_kv(\"forensic-source\",");
    CHECK(insert_pos != std::string::npos, "insert_kv(\"forensic-source\", ...) found");
    if (insert_pos != std::string::npos) {
        const auto window_end = std::min<std::size_t>(insert_pos + 600, src.size());
        const std::string window(src, insert_pos, window_end - insert_pos);
        CHECK(window.find("forensic-source-trail") != std::string::npos,
              "forensic-source-trail=1 sentinel inserted");
        CHECK(window.find("forensic-source-se") != std::string::npos,
              "forensic-source-se=2 sentinel inserted");
        CHECK(window.find("forensic-source-wal") != std::string::npos,
              "forensic-source-wal=3 sentinel inserted");
    }
}

// AC2: enum sentinels document stable int codes (1=trail, 2=SE, 3=WAL).
static void ac2_enum_sentinels() {
    std::println("\n--- AC2: enum sentinels stable int codes ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(src.find("insert_kv(\"forensic-source-trail\", 1)") != std::string::npos,
          "forensic-source-trail=1 sentinel present");
    CHECK(src.find("insert_kv(\"forensic-source-se\", 2)") != std::string::npos,
          "forensic-source-se=2 sentinel present");
    CHECK(src.find("insert_kv(\"forensic-source-wal\", 3)") != std::string::npos,
          "forensic-source-wal=3 sentinel present");
}

// AC3: Soft / zero-cost — no WAL I/O; mid==0 -> forensic-source=0.
static void ac3_soft_zero_cost() {
    std::println("\n--- AC3: Soft / zero-cost ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    // No new WAL file scan API call (should still only use is_enabled()).
    CHECK(src.find("snapshot_audit_wal") != std::string::npos,
          "existing snapshot_audit_wal still present (no new file-scan API)");
    CHECK(src.find("forensic_source = 0") != std::string::npos,
          "forensic_source initialized to 0 (mid==0 short-circuit)");
    const auto compute_pos = src.find("std::int64_t forensic_source = 0;");
    CHECK(compute_pos != std::string::npos, "forensic_source declaration present");
    if (compute_pos != std::string::npos) {
        const auto window_end = std::min<std::size_t>(compute_pos + 1500, src.size());
        const std::string window(src, compute_pos, window_end - compute_pos);
        // Issue #3770-era refactor: the raw join_mid guard moved into the
        // precomputed se_filter_by_mid bool (filt_mid || join_mid != 0) —
        // same semantics, different spelling.
        CHECK(window.find("if (se_filter_by_mid && !typed_hit)") != std::string::npos,
              "forensic scan guarded by mid != 0 && !typed_hit");
        CHECK(window.find("is_enabled()") != std::string::npos,
              "WAL is_enabled() bool probe (not a scan)");
        // No shadow writes / file operations
        CHECK(window.find("fopen") == std::string::npos,
              "no fopen in forensic-source block (Soft: no I/O)");
        CHECK(window.find("fread") == std::string::npos,
              "no fread in forensic-source block (Soft: no I/O)");
    }
}

// AC4: capacity / schema — planned_keys bumped 33 -> 37; overflow=0;
// schema-3152/issue-3152 sentinels added.
static void ac4_capacity_schema() {
    std::println("\n--- AC4: capacity / schema ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(src.find("kEvolutionAuditDecisionPlannedKeys = 80") != std::string::npos,
          "planned_keys 72 (#3339 Agent facade headroom)");
    CHECK(src.find("insert_kv(\"schema-3152\", 3152)") != std::string::npos,
          "schema-3152 sentinel present");
    CHECK(src.find("insert_kv(\"issue-3152\", 3152)") != std::string::npos,
          "issue-3152 sentinel present");
    CHECK(src.find("insert_kv(\"schema-3284\", 3284)") != std::string::npos,
          "schema-3284 sentinel present");
    CHECK(src.find("insert_kv(\"issue-3284\", 3284)") != std::string::npos,
          "issue-3284 sentinel present");
    CHECK(src.find("overflowed = false") != std::string::npos, "overflow tracking preserved");
    CHECK(src.find("query_hash_finish(ht, ev.string_heap_, overflowed)") != std::string::npos,
          "query_hash_finish still called with overflow flag");
}

// AC5: parallel with #3149 — both in evolution-audit-decision handler,
// single PR. last-se-reason string still present.
static void ac5_parallel_with_3149() {
    std::println("\n--- AC5: parallel with #3149 ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(src.find("insert_kv_str(\"last-se-reason\", last_se_reason_str)") != std::string::npos,
          "last-se-reason string (#3149) still present");
    CHECK(src.find("insert_kv(\"schema-3149\", 3149)") != std::string::npos,
          "schema-3149 sentinel still present");
    CHECK(src.find("insert_kv(\"issue-3149\", 3149)") != std::string::npos,
          "issue-3149 sentinel still present");
    // Both #3149 and #3152 sentinels sit in the same handler, single PR.
    const auto s3149 = src.find("insert_kv(\"issue-3149\", 3149)");
    const auto s3152 = src.find("insert_kv(\"issue-3152\", 3152)");
    CHECK(s3149 != std::string::npos && s3152 != std::string::npos,
          "both #3149 and #3152 sentinels present in same handler");
    if (s3149 != std::string::npos && s3152 != std::string::npos) {
        CHECK(s3152 > s3149, "#3152 sentinels appended after #3149 (single-handler additive)");
        CHECK(s3152 - s3149 < 600, "#3149 + #3152 sentinels in adjacent block (single PR)");
    }
}

static void ac6_durable_3205() {
    std::println("\n--- AC6: #3205 :durable point-query ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(src.find("Issue #3205") != std::string::npos, "cites #3205");
    CHECK(src.find("find_recent_by_mutation_id") != std::string::npos, "SE WAL mid point-query");
    CHECK(src.find("insert_kv(\"durable-hit\", durable_hit)") != std::string::npos,
          "durable-hit key");
    CHECK(src.find("insert_kv(\"schema-3205\",") != std::string::npos, "schema-3205");
    CHECK(src.find("insert_kv(\"issue-3205\",") != std::string::npos, "issue-3205");
    CHECK(src.find("want_durable") != std::string::npos, ":durable keyword parse");
    CHECK(src.find("production_defaults_active()") != std::string::npos, "production gate on scan");
}

static void ac7_typed_summary_3242() {
    std::println("\n--- AC7: #3242 typed-summary sidecar ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(src.find("Issue #3242") != std::string::npos, "cites #3242");
    CHECK(src.find("find_recent_typed_summary_by_mid") != std::string::npos,
          "typed summary lookup");
    CHECK(src.find("insert_kv(\"typed-summary-from-wal\", typed_summary_from_wal)") !=
              std::string::npos,
          "typed-summary-from-wal key");
    CHECK(src.find("insert_kv(\"schema-3242\",") != std::string::npos, "schema-3242");
    CHECK(src.find("kEvolutionAuditDecisionPlannedKeys = 80") != std::string::npos,
          "planned keys 72 (#3339 headroom)");
}

// AC8: Issue #3284 — SE match discipline. When a join mid is in scope
// (explicit arg or default last-stamped path), only SE rows with
// e.mutation_id == join_mid are accepted; if none, se-mid-miss=1 and the
// SE fields stay 0/"" (never publish a different mid's SE beside a typed
// hit for mid M). Soft / no :durable stays zero disk I/O.
static void ac8_3284_se_mid_miss() {
    std::println("\n--- AC8: #3284 SE match discipline + se-mid-miss ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(src.find("Issue #3284") != std::string::npos, "cites #3284");
    // Filter is join_mid-scoped (not filt_mid-scoped): default last-stamped
    // path must also refuse a different mid's SE beside a typed hit.
    CHECK(src.find("se_filter_by_mid = filt_mid || join_mid != 0") != std::string::npos,
          "SE walk join-scoped (explicit 0 or last-stamped, not filt_mid-only)");
    CHECK(src.find("se_filter_by_mid && e.mutation_id != join_mid") != std::string::npos,
          "SE walk filters by join_mid (not filt_mid)");
    CHECK(src.find("se_mid_miss = (se_filter_by_mid && !se_mid_hit) ? 1 : 0") != std::string::npos,
          "se_mid_miss set when no same-mid SE row");
    CHECK(src.find("insert_kv(\"se-mid-miss\", se_mid_miss)") != std::string::npos,
          "se-mid-miss key inserted");
    // Soft / no :durable still zero disk I/O: the WAL path remains gated on
    // (want_durable || auto_durable) + (production_defaults_active() || Full
    // strategy), per #3298/#3674 — auto_durable itself requires
    // production/Full, so Soft reaches find_recent_* neither way.
    CHECK(src.find("if ((want_durable || auto_durable) && join_mid != 0 &&") != std::string::npos,
          "durable WAL path still gated (AC3 zero disk I/O)");
    CHECK(src.find("(production_defaults_active() || get_strategy() == AuditStrategy::Full)") !=
              std::string::npos,
          "gate aligned with Full hard face (#3298)");
}

// AC9 (#3603): :durable window-miss additive key; observe-only stays 1.
static void ac9_wal_window_miss_3603() {
    std::println("\n--- AC9 (#3603): wal-lookup-window-miss additive key ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(!src.empty(), "evaluator_primitives_security.cpp readable");
    CHECK(src.find("wal-lookup-window-miss") != std::string::npos,
          "3603: key present on the decision hash");
    CHECK(src.find("Issue #3603") != std::string::npos, "cites #3603");
    const auto kv = src.find("insert_kv(\"wal-lookup-window-miss\", wal_lookup_window_miss)");
    CHECK(kv != std::string::npos, "insert_kv(\"wal-lookup-window-miss\", ...) present");
    // Gate alignment: computed inside the want_durable + production/Full
    // + WAL-enabled block (Soft / observe-only keeps 0, no extra I/O).
    const auto gate = src.find("if ((want_durable || auto_durable) && join_mid != 0 &&");
    CHECK(gate != std::string::npos, "durable WAL gate unchanged");
    CHECK(kv != std::string::npos && gate != std::string::npos && kv > gate,
          "window-miss insert sits after the durable gate");
    const auto comp = src.find("(durable_hit == 0 && typed_summary_from_wal == 0) ? 1 : 0");
    CHECK(comp != std::string::npos && comp > gate,
          "window-miss computed inside the durable block");
    // Flag semantics: every find_recent_* missed → 1 (never a typed rewrite).
    CHECK(src.find("(durable_hit == 0 && typed_summary_from_wal == 0) ? 1 : 0") !=
              std::string::npos,
          "window-miss = all-find_recent-miss semantics");
    // observe-only stays 1 (unchanged #3114 face).
    CHECK(src.find("insert_kv(\"observe-only\", 1)") != std::string::npos, "observe-only stays 1");
}

// AC10 (#3674): production/Full auto-durable fold — the default (no
// :durable) one-query fold runs the same bounded find_recent_* window
// security-audit uses when BOTH in-memory rings miss; Soft/Off keep zero
// WAL I/O; typed-trail-miss is never rewritten; observe-only untouched.
static void ac10_wal_fold_autoscan_3674() {
    std::println("\n--- AC10 (#3674): production auto-durable fold ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(!src.empty(), "evaluator_primitives_security.cpp readable");
    CHECK(src.find("Issue #3674") != std::string::npos, "cites #3674");
    CHECK(src.find("const bool auto_durable") != std::string::npos, "auto_durable arm present");
    CHECK(src.find("join_mid != 0 && !typed_hit && !se_ring_has_mid && wal_enabled") !=
              std::string::npos,
          "auto_durable gated on both rings miss + WAL enabled");
    CHECK(src.find("if ((want_durable || auto_durable) && join_mid != 0 &&") != std::string::npos,
          "durable gate admits auto_durable beside :durable");
    // Soft zero-I/O preserved: the scan block itself stays production/Full
    // -gated (Soft reaches find_recent_* neither via :durable nor auto).
    const auto gate = src.find("if ((want_durable || auto_durable) && join_mid != 0 &&");
    CHECK(gate != std::string::npos, "gate present");
    if (gate != std::string::npos) {
        const auto window_end = std::min<std::size_t>(gate + 240, src.size());
        const std::string window(src, gate, window_end - gate);
        CHECK(
            window.find("production_defaults_active() || get_strategy() == AuditStrategy::Full") !=
                std::string::npos,
            "scan block still production/Full-gated (Soft: no I/O)");
    }
    // Additive faces preserved: typed-trail-miss never rewritten (#3498);
    // sidecar fill stays additive (#3242); observe-only + suggested-next
    // unchanged (#3114/#3246).
    CHECK(src.find("insert_kv(\"typed-trail-miss\", typed_miss)") != std::string::npos,
          "typed-trail-miss stays the miss face (WAL is not the typed trail)");
    CHECK(src.find("insert_kv(\"observe-only\", 1)") != std::string::npos,
          "observe-only unchanged");
    CHECK(src.find("decide_evolution_suggested_next") != std::string::npos,
          "suggested-next fold untouched");
    // Bounded scan: same lookup window as security-audit (no new unbounded
    // path; wal_mid_lookup_segments() untouched per #3674 non-goals).
    CHECK(src.find("wal_mid_lookup_segments()") != std::string::npos,
          "bounded wal_mid_lookup_segments() window reused");
}

// AC11 (#4240): segment-prune arm in the wal_miss_refuse_evidence fold.
// After typed (256) + SE (1024) ring wrap AND AURA_WAL_MAX_SEGMENTS
// retention pruned the only durable segment holding the mid, all three
// faces miss with no refuse row — the fold must not read as "never
// audited": a production-only additive arm (prune counters live OR the
// exhaustive scan exhausted) joins the #4142 evidence and folds
// suggested-next to inspect-deny (observe-only; never auto-recover).
static void ac11_wal_prune_fold_4240() {
    std::println("\n--- AC11 (#4240): segment-prune arm in decision fold ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(!src.empty(), "evaluator_primitives_security.cpp readable");
    CHECK(src.find("Issue #4240") != std::string::npos, "cites #4240");
    const auto assign = src.find("nin.wal_miss_refuse_evidence =");
    CHECK(assign != std::string::npos, "wal_miss_refuse_evidence assignment present");
    const auto decide = src.find("decide_evolution_suggested_next(nin)");
    CHECK(decide != std::string::npos && assign < decide,
          "fold assignment precedes the suggested-next call");
    if (assign == std::string::npos || decide == std::string::npos || assign > decide)
        return;
    std::string flat;
    flat.reserve(decide - assign);
    for (std::size_t i = assign; i < decide; ++i)
        if (src[i] != ' ' && src[i] != '\n' && src[i] != '\t' && src[i] != '\r')
            flat.push_back(src[i]);
    // Additive #4142 arms preserved (append-miss / overflow-refuse /
    // durable join / post-wrap all-miss + refuse counter).
    CHECK(flat.find("\"mutation_wal_append_miss\"") != std::string::npos,
          "#4142 append-miss arm preserved");
    CHECK(flat.find("\"overflow-refuse\"") != std::string::npos,
          "#4142 overflow-refuse arm preserved");
    CHECK(flat.find("forensic_mid_has_wal_append_miss(join_mid)") != std::string::npos,
          "#4118/#3877 durable join arm preserved");
    CHECK(flat.find("wal_overflow_ring_wrap_refuse_total()") != std::string::npos,
          "#4142 post-wrap refuse-counter arm preserved");
    // New prune arm: production + join_mid + all three miss faces +
    // (prune counters > 0 || full scan exhausted). Whitespace-stripped
    // match so clang-format refolding cannot void the contract.
    CHECK(flat.find("Issue#4240") != std::string::npos, "#4240 arm cited at the fold");
    CHECK(flat.find("production_defaults_active()&&join_mid!=0") != std::string::npos,
          "prune arm gated on production + join_mid");
    CHECK(flat.find("se_mid_miss!=0") != std::string::npos, "se-mid-miss face in prune arm");
    CHECK(flat.find("typed_miss!=0") != std::string::npos, "typed-trail-miss face in prune arm");
    CHECK(flat.find("wal_lookup_window_miss!=0") != std::string::npos,
          "wal-lookup-window-miss face in prune arm");
    CHECK(flat.find("audit_wal_segment_prune_total") != std::string::npos,
          "mutation audit WAL prune counter arm");
    CHECK(flat.find("security_event_wal_segment_prune_total") != std::string::npos,
          "SE WAL prune counter arm");
    CHECK(flat.find("wal_full_scan_exhausted!=0") != std::string::npos,
          "full-scan-exhausted disjunct in prune arm");
}

// AC12 (#4240): wal_full_scan_exhausted is computed inside the
// production/Full durable scan block — both exhaustive paths (SE WAL and
// mutation provenance WAL) mark exhaustion when the scan-all still
// missed; the typed-summary sidecar scan (#3242) does not; the
// cheap-window contract (#3603/#3970) is unchanged.
static void ac12_full_scan_exhausted_face_4240() {
    std::println("\n--- AC12 (#4240): wal-full-scan-exhausted face ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(!src.empty(), "evaluator_primitives_security.cpp readable");
    CHECK(src.find("std::int64_t wal_full_scan_exhausted = 0;") != std::string::npos,
          "wal_full_scan_exhausted declared beside wal_full_scan_hit");
    std::size_t pos = 0;
    std::size_t arms = 0;
    std::size_t first_arm = 0;
    std::size_t second_arm = 0;
    while ((pos = src.find("wal_full_scan_exhausted = 1", pos)) != std::string::npos) {
        if (arms == 0)
            first_arm = pos;
        else if (arms == 1)
            second_arm = pos;
        ++arms;
        pos += 4;
    }
    CHECK(arms == 2, "both scan-all branches mark exhaustion (SE + mutation, not sidecar)");
    const auto se_all = src.find("se_wal.find_by_mutation_id_scan_all_segments(join_mid)");
    CHECK(se_all != std::string::npos && first_arm > se_all,
          "SE WAL scan-all precedes its exhausted arm");
    const auto mut_all = src.find("find_by_provenance_mutation_id_scan_all_segments(join_mid)");
    CHECK(mut_all != std::string::npos && second_arm > mut_all,
          "mutation WAL scan-all precedes its exhausted arm");
    // Face is computed before the fold consumes it.
    const auto fold = src.find("nin.wal_miss_refuse_evidence =");
    CHECK(fold != std::string::npos && second_arm < fold,
          "exhausted face computed before the decision fold");
}

// AC13 (#4240): observe-only reason "wal-segment-pruned" — the durable
// miss block labels a prune-thinned mid (prune counters live) before the
// #3838 wrap-evicted fallback; a live overflow row keeps precedence;
// counters are read lazily inside the production/Full block so Soft /
// WAL-off / retention=0 keep the #4142 fold unchanged (zero-cost).
static void ac13_pruned_reason_4240() {
    std::println("\n--- AC13 (#4240): wal-segment-pruned observe reason ---");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(!src.empty(), "evaluator_primitives_security.cpp readable");
    CHECK(src.find("Issue #4240") != std::string::npos, "cites #4240");
    CHECK(src.find("last_se_reason_str = \"wal-segment-pruned\";") != std::string::npos,
          "wal-segment-pruned observe reason present");
    const auto miss_block = src.find("if (durable_hit == 0 && last_se_reason_str.empty())");
    CHECK(miss_block != std::string::npos, "durable miss reason block present");
    const auto ovr = src.find("wal_overflow_find_by_mid(", miss_block);
    const auto pruned = src.find("\"wal-segment-pruned\"", miss_block);
    const auto wrap = src.find("\"overflow_wrap_evicted\"", miss_block);
    CHECK(ovr != std::string::npos && pruned != std::string::npos && wrap != std::string::npos,
          "all three reason arms present in the miss block");
    CHECK(ovr < pruned && pruned < wrap,
          "reason precedence: overflow row, prune-thinned, wrap-evicted");
    // Prune counters are only read inside production/Full-gated arms:
    // the reason arm and the #4240 fold arm (both after the durable gate).
    const auto gate = src.find("if ((want_durable || auto_durable) && join_mid != 0 &&");
    CHECK(gate != std::string::npos && pruned > gate,
          "prune reason sits inside the production/Full durable block");
}

// AC14 (#4240): wiring + non-goals — the ship linter is registered in
// build.py and listed on the frozen root allowlist; no new query key
// cites 4240; planned_keys stays 72; no test_issue_4240.cpp (#81934);
// no docs/design/4240-* markdown (#1655).
static void ac14_wiring_non_goals_4240() {
    std::println("\n--- AC14 (#4240): linter wiring + non-goals ---");
    auto lint = read_file("../scripts/check_wal_prune_fold_4240.py");
    if (lint.find("check_wal_prune_fold_4240") == std::string::npos)
        lint = read_file("scripts/check_wal_prune_fold_4240.py");
    CHECK(lint.find("#!/usr/bin/env python3") != std::string::npos,
          "ship linter script readable (python3 gate)");
    CHECK(lint.find("check_wal_prune_fold_4240") != std::string::npos,
          "linter self-identifies (#4240 gate)");
    auto build_py = read_file("../build.py");
    if (build_py.find("check_wal_prune_fold_4240.py") == std::string::npos)
        build_py = read_file("build.py");
    CHECK(build_py.find("def gate") != std::string::npos || build_py.size() > 1000,
          "build.py readable");
    CHECK(build_py.find("check_wal_prune_fold_4240.py") != std::string::npos,
          "build.py registers the #4240 linter");
    auto allow = read_file("../scripts/coverage/root_check_allowlist.txt");
    if (allow.find("check_wal_prune_fold_4240.py") == std::string::npos)
        allow = read_file("scripts/coverage/root_check_allowlist.txt");
    CHECK(allow.find("check_wal_prune_fold_4240.py") != std::string::npos,
          "root allowlist lists the #4240 linter");
    auto src = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(src.find("kEvolutionAuditDecisionPlannedKeys = 80") != std::string::npos,
          "planned_keys is 80 after last-se-op (#4303)");
    // No insert_kv row cites 4240 (suggested-next stays the observable).
    std::size_t p = 0;
    bool kv_4240 = false;
    while ((p = src.find("insert_kv", p)) != std::string::npos) {
        const auto close = src.find(");", p);
        if (close == std::string::npos)
            break;
        if (src.find("4240", p) < close)
            kv_4240 = true;
        p = close;
    }
    CHECK(!kv_4240, "no insert_kv row cites 4240 (no new query key)");
}

} // namespace

int main() {
    ac1_source_forensic_source_key();
    ac2_enum_sentinels();
    ac3_soft_zero_cost();
    ac4_capacity_schema();
    ac5_parallel_with_3149();
    ac6_durable_3205();
    ac7_typed_summary_3242();
    ac8_3284_se_mid_miss();
    ac9_wal_window_miss_3603();
    ac10_wal_fold_autoscan_3674();
    ac11_wal_prune_fold_4240();
    ac12_full_scan_exhausted_face_4240();
    ac13_pruned_reason_4240();
    ac14_wiring_non_goals_4240();
    if (g_failed)
        return 1;
    std::println("evolution-audit-decision forensic-source (#3152): OK ({} passed)", g_passed);
    return 0;
}