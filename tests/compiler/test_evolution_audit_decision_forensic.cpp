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
        CHECK(window.find("if (join_mid != 0 && !typed_hit)") != std::string::npos,
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
    CHECK(src.find("kEvolutionAuditDecisionPlannedKeys = 72") != std::string::npos,
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
    CHECK(src.find("kEvolutionAuditDecisionPlannedKeys = 72") != std::string::npos,
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
    CHECK(src.find("join_mid != 0 && e.mutation_id != join_mid") != std::string::npos,
          "SE walk filters by join_mid (not filt_mid)");
    CHECK(src.find("se_mid_miss = (join_mid != 0 && !se_mid_hit) ? 1 : 0") != std::string::npos,
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
    if (g_failed)
        return 1;
    std::println("evolution-audit-decision forensic-source (#3152): OK ({} passed)", g_passed);
    return 0;
}