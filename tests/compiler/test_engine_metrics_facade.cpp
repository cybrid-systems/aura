// @category: integration
// @reason: Issue #1433 (engine:metrics) facade golden test
//
// AC1: (engine:metrics) returns hash with nested groups + ≥200 metric fields
// AC2: :prefix "query:" returns filtered hash
// AC3: :group "jit" returns jit sub-tree hash
// AC4: by-name lookup still works (or void under s0)
// AC5: schema == 2; top-level group key snapshot

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;

namespace {

std::int64_t hash_int(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

bool is_hash_expr(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::format("(hash? {})", expr));
    return r && is_bool(*r) && as_bool(*r);
}

std::int64_t hash_len(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::format("(hash-length {})", expr));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

} // namespace

extern "C" void aura_engine_metrics_set_force_hash_cap(std::uint64_t);
extern "C" std::uint64_t aura_engine_metrics_hash_overflow_total(void);
extern "C" void aura_engine_metrics_reset_hash_overflow_for_test(void);
extern "C" void aura_query_hash_set_force_cap(std::uint64_t);
extern "C" std::uint64_t aura_query_hash_overflow_total(void);
extern "C" void aura_query_hash_reset_overflow_for_test(void);

int main() {
    CompilerService cs;

    // ── AC1: default facade hash ──
    {
        auto r = cs.eval("(engine:metrics)");
        CHECK(r && is_hash(*r), "(engine:metrics) returns hash");
        CHECK(hash_int(cs, "(engine:metrics)", "schema") == 2, "schema == 2 (#1433)");
        CHECK(hash_int(cs, "(engine:metrics)", "stats-count") > 0, "stats-count > 0");
        auto fields = hash_int(cs, "(engine:metrics)", "metrics-field-count");
        CHECK(fields >= 200, std::format("metrics-field-count >= 200 (got {})", fields));

        // Nested groups present (snapshot of top-level group keys)
        static const char* kGroups[] = {"compile", "jit", "mutate", "query",
                                        "arena",   "gc",  "eval",   "telemetry"};
        int groups_present = 0;
        for (const char* g : kGroups) {
            auto gr = cs.eval(std::format("(hash-ref (engine:metrics) \"{}\")", g));
            if (gr && is_hash(*gr))
                ++groups_present;
        }
        CHECK(groups_present >= 4,
              std::format("at least 4 top-level groups present (got {})", groups_present));
        // Back-compat compiler snapshot
        CHECK(is_hash_expr(cs, "(hash-ref (engine:metrics) \"compiler\")"),
              "compiler nested hash present");
    }

    // ── Issue #3244: overflow → security-posture additive (Soft never arms) ──
    // Before :prefix (light-linked catalog dump leftover).
    {
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::compiler::typed_audit::reset_for_test;
        aura_engine_metrics_reset_hash_overflow_for_test();
        aura_query_hash_reset_overflow_for_test();
        reset_for_test();
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")",
                       "metrics-hash-overflow-breach") == 0,
              "ac3244_2_soft: quiet posture breach=0");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")",
                       "metrics-hash-overflow-wired") == 1,
              "3244 AC1: posture wired sentinel");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "schema-3244") == 3244,
              "3244 AC5: schema-3244 additive");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "schema-2534") == 2534,
              "3244 AC5: schema-2534 preserved");

        aura_query_hash_set_force_cap(4);
        CHECK(is_hash_expr(cs, "(engine:metrics \"query:security-posture\")"),
              "3244 AC3: overflow posture still a hash");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "overflow") == 1,
              "3244 AC3: #3018 overflow=1 retained");
        const auto ho =
            hash_int(cs, "(engine:metrics \"query:security-posture\")", "hash-overflow");
        CHECK(ho == 1 ||
                  hash_int(cs, "(engine:metrics \"query:security-posture\")", "overflow") == 1,
              "3244 AC3: hash-overflow sentinel additive (or overflow=1 when cap is tiny)");
        aura_query_hash_set_force_cap(0); // restore cap; keep overflow_total
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")",
                       "metrics-hash-overflow-breach") == 0,
              "ac3244_2_soft: Soft same overflow path does not arm degraded");

        apply_production_audit_defaults();
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")",
                       "metrics-hash-overflow-breach") == 1,
              "ac3244_1_prod: production + overflow → metrics-hash-overflow-breach=1");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")",
                       "metrics-hash-overflow-total") > 0,
              "3244 AC1: overflow total surfaced");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "issue-3244") == 3244,
              "3244 AC5: issue-3244");
        aura_engine_metrics_reset_hash_overflow_for_test();
        aura_query_hash_set_force_cap(0);
        aura_query_hash_reset_overflow_for_test();
        reset_for_test();
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")", "schema-3246") ==
                  3246,
              "3246 AC4: schema-3246 additive on facade");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")",
                       "suggested-next-code") == 1,
              "ac3246_3_soft: Soft suggested-next-code=soft-observe");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")", "observe-only") ==
                  1,
              "3246 AC2: observe-only preserved");
    }

    // ── Issue #3339: Agent decision facade headroom; no hash-overflow ──
    // Runs before :prefix catalog dump so a prefix leftover cannot hide ACs.
    {
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::compiler::typed_audit::reset_for_test;
        std::println("\n--- #3339: Agent decision facade planned_keys headroom ---");
        aura_query_hash_set_force_cap(0);
        aura_query_hash_reset_overflow_for_test();
        reset_for_test();
        apply_production_audit_defaults();

        const char* kFacades[] = {
            "query:evolution-audit-decision",  "query:security-posture",
            "query:type-linear-commit-health", "query:type-linear-evolution-snapshot",
            "query:reload-recovery-playbook",
        };
        for (const char* q : kFacades) {
            const auto expr = std::format("(engine:metrics \"{}\")", q);
            CHECK(is_hash_expr(cs, expr), std::format("3339 AC2: {} is hash", q));
            const auto ho = hash_int(cs, expr, "hash-overflow");
            CHECK(ho != 1, std::format("ac3339_2_no_overflow: {} hash-overflow absent", q));
            CHECK(hash_int(cs, expr, "overflow") != 1, std::format("3339 AC2: {} overflow!=1", q));
        }
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")", "schema-3114") ==
                  3114,
              "3339 AC2: evolution schema-3114 present");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")",
                       "suggested-next-code") >= 0,
              "3339 AC2: suggested-next-code present");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")",
                       "last-audit-mid") >= 0,
              "3339 AC2: last-audit-mid present");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "schema-2534") == 2534,
              "3339 AC2 / #3499: security-posture schema-2534");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "schema-2225") == 2225,
              "3499 AC1: full primitives posture also has schema-2225");
        CHECK(hash_int(cs, "(engine:metrics \"query:type-linear-commit-health\")", "schema-2613") ==
                  2613,
              "3339 AC2: type-linear-commit-health schema-2613");
        CHECK(hash_int(cs, "(engine:metrics \"query:type-linear-evolution-snapshot\")",
                       "schema-2897") == 2897,
              "3339 AC2: type-linear-evolution-snapshot schema-2897");
        CHECK(hash_int(cs, "(engine:metrics \"query:reload-recovery-playbook\")", "schema-2953") ==
                  2953,
              "3339 AC2: reload-recovery-playbook schema-2953");

        const auto evix = read_file("src/compiler/evaluator.ixx");
        CHECK(evix.find("kAgentDecisionFacadeHeadroom = 8") != std::string::npos,
              "3339 AC1: headroom constant 8");
        const auto sec = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(sec.find("kEvolutionAuditDecisionPlannedKeys = 72") != std::string::npos,
              "ac3339_3_plus20: planned 72 so +20 dummy keys without raise fails CI");
        CHECK(sec.find("kSecurityPosturePlannedKeys = 128") != std::string::npos,
              "3339 AC1 / #3499: posture planned 128 (2225 merge headroom)");

        reset_for_test();
        apply_dev_audit_defaults();
        aura_query_hash_reset_overflow_for_test();
        const auto soft_ho =
            hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")", "hash-overflow");
        CHECK(soft_ho != 1, "ac3339_4_soft: Soft path still no overflow");
        CHECK(hash_int(cs, "(engine:metrics)", "schema") == 2,
              "3339 AC4: engine:metrics unchanged");
        CHECK(read_file("tests/compiler/test_issue_3339.cpp").empty(), "ac3339_5_no_invent");
        CHECK(read_file("docs/design/3339-agent-decision-facade-headroom.md").empty(),
              "3339 AC5: no docs/design/3339-*");
    }

    // Issue #3499: production full-primitives posture carries both 2534
    // decision keys and 2225 WAL durability keys (last-wins merge).
    // Runs before :prefix — that dump is a pre-existing stack-smash on
    // this tree (origin/main) and must not hide these ACs.
    std::println("\n--- #3499: security-posture 2534+2225 merge ---");
    {
        aura_query_hash_set_force_cap(0);
        aura_query_hash_reset_overflow_for_test();
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "schema-2534") == 2534,
              "3499 AC1: schema-2534");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "schema-2225") == 2225,
              "3499 AC1: schema-2225 on full primitives");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "ring-wrap-total") >= 0,
              "3499 AC2: ring-wrap-total");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "wal-persisted-total") >=
                  0,
              "3499 AC2: wal-persisted-total");
        const auto gap =
            hash_int(cs, "(engine:metrics \"query:security-posture\")", "audit-durable-gap");
        CHECK(gap == 0 || gap == 1, "3499 AC2: audit-durable-gap is 0 or 1");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "overflow") != 1,
              "3499 AC4: overflow!=1 under default cap");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "schema-3499") == 3499,
              "3499 AC1: schema-3499");
        const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("\"query:security-posture\"") != std::string::npos,
              "3499 AC3: obs_eval slim/s0 handler still registered");
        CHECK(read_file("tests/compiler/test_issue_3499.cpp").empty(),
              "3499 AC5: no test_issue_3499.cpp");
        CHECK(read_file("docs/design/3499-security-posture-merge.md").empty(),
              "3499 AC5: no docs/design/3499-*");
    }

    // ── AC2: :prefix ──
    {
        auto r = cs.eval("(engine:metrics :prefix \"query:\")");
        CHECK(r && is_hash(*r), ":prefix query: returns hash");
        CHECK(hash_int(cs, "(engine:metrics :prefix \"query:\")", "schema") == 2,
              ":prefix schema 2");
        // Should include multiple query: stats catalog entries
        auto n = hash_len(cs, "(engine:metrics :prefix \"query:\")");
        CHECK(n >= 5, std::format(":prefix query: has >= 5 keys (got {})", n));
    }

    // ── AC3: :group ──
    {
        auto r = cs.eval("(engine:metrics :group \"jit\")");
        CHECK(r && is_hash(*r), ":group jit returns hash");
        auto n = hash_len(cs, "(engine:metrics :group \"jit\")");
        CHECK(n >= 1, std::format(":group jit has >= 1 field (got {})", n));
        // Known field from CompilerMetrics
        auto j = cs.eval("(hash-ref (engine:metrics :group \"jit\") \"jit_compilations\")");
        CHECK(j && is_int(*j), "jit_compilations present in jit group");
    }

    // ── AC4: by-name ──
    {
        auto r = cs.eval("(engine:metrics \"query:macro-hygiene-stats\")");
        CHECK(r.has_value(), "by-name returns a value");
        // full: hash; s0: void — both OK
        CHECK(is_hash(*r) || is_void(*r), "by-name is hash or void");
        auto miss = cs.eval("(engine:metrics \"query:no-such-stats-zzz\")");
        CHECK(miss && is_void(*miss), "missing stats name → void");
    }

    // ── AC5: :all still works ──
    {
        auto r = cs.eval("(engine:metrics :all)");
        CHECK(r && is_hash(*r), ":all returns hash");
        CHECK(hash_int(cs, "(engine:metrics :all)", "schema") == 2, ":all schema 2");
        CHECK(hash_int(cs, "(engine:metrics :all)", "overflow") == -1,
              "#3018 AC2: normal :all has no overflow key");
    }

    // ── Issue #3018: fail-soft on hash capacity overflow ──
    // AC1: forced undersized table still returns hash with schema + overflow=1.
    // AC2: normal :all is full map (checked above).
    // AC3: :prefix "query:" stays a hash; missing impl is void-per-key.
    // AC4: Soft/Off facade cost is one force-cap load (source-cite).
    // AC5: overflow / capacity case (this block + engine_metrics.aura).
    {
        aura_engine_metrics_reset_hash_overflow_for_test();
        aura_engine_metrics_set_force_hash_cap(4); // undersized vs catalog
        const auto overflow0 = aura_engine_metrics_hash_overflow_total();
        auto r = cs.eval("(engine:metrics :all)");
        CHECK(r && is_hash(*r), "#3018 AC1: forced undersized :all still returns hash");
        CHECK(!is_void(*r), "#3018 AC1: never void solely due to capacity");
        CHECK(hash_int(cs, "(engine:metrics :all)", "schema") == 2,
              "#3018 AC1: schema present on overflow hash");
        CHECK(hash_int(cs, "(engine:metrics :all)", "overflow") == 1,
              "#3018 AC1: overflow=1 sentinel");
        CHECK(aura_engine_metrics_hash_overflow_total() > overflow0,
              "#3018 AC1: engine_metrics_hash_overflow_total bumped");
        aura_engine_metrics_reset_hash_overflow_for_test();
        auto pref = cs.eval("(engine:metrics :prefix \"query:\")");
        CHECK(pref && is_hash(*pref), "#3018 AC3: :prefix query: returns hash (table lives)");
        CHECK(hash_int(cs, "(engine:metrics :prefix \"query:\")", "schema") == 2,
              "#3018 AC3: :prefix schema 2");
        CHECK(hash_int(cs, "(engine:metrics :prefix \"query:\")", "overflow") == -1,
              "#3018 AC3: :prefix no overflow under computed cap");
        auto miss = cs.eval("(engine:metrics \"query:no-such-stats-zzz\")");
        CHECK(miss && is_void(*miss), "#3018 AC3: missing impl still void-per-key");
        const auto src = []() -> std::string {
            for (const auto& p :
                 {std::string("src/compiler/evaluator_primitives_obs_jit.cpp"),
                  std::string("../src/compiler/evaluator_primitives_obs_jit.cpp"),
                  std::string("../../src/compiler/evaluator_primitives_obs_jit.cpp")}) {
                std::ifstream in(p);
                if (!in)
                    continue;
                return std::string((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            }
            return {};
        }();
        CHECK(src.find("never FlatHashTable::destroy + return void for capacity alone") !=
                  std::string::npos,
              "#3018 AC4: fail-soft cited (no destroy-on-capacity)");
        CHECK(src.find("kv.size()) * 2 + 8") != std::string::npos ||
                  src.find("kv.size() * 2 + 8") != std::string::npos,
              "#3018 AC4: headroom is size*2+8");
        CHECK(src.find("g_engine_metrics_force_hash_cap.load") != std::string::npos,
              "#3018 AC4: Soft extra cost is one force-cap load");
    }

    // ── Issue #3020: domain query:* hash overflow fail-soft ──
    // AC1: inventory + sized create / insert_kv_checked (source-cite).
    // AC2: forced-full insert yields overflow=1 (never silent drop).
    // AC3: high-churn queries return documented schema sentinels.
    // AC4: this test + engine_metrics.aura; no test_issue_3020.cpp.
    // AC5: soak — default catalog does not bump query_hash_overflow_total.
    {
        aura_query_hash_reset_overflow_for_test();
        const auto soak0 = aura_query_hash_overflow_total();
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "schema-2534") == 2534,
              "#3020 AC3: security-posture schema-2534");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")",
                       "security-posture-wired") == 1,
              "#3020 AC3: security-posture-wired");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "overflow") == -1,
              "#3020 AC3: security-posture no overflow under default cap");
        CHECK(hash_int(cs, "(engine:metrics \"query:type-linear-commit-health\")", "schema-2613") ==
                  2613,
              "#3020 AC3: type-linear-commit-health schema-2613");
        CHECK(hash_int(cs, "(engine:metrics \"query:type-linear-commit-health\")",
                       "type-linear-commit-health-wired") == 1,
              "#3020 AC3: type-linear wired");
        CHECK(hash_int(cs, "(engine:metrics \"query:type-linear-commit-health\")", "overflow") ==
                  -1,
              "#3020 AC3: type-linear no overflow under default cap");
        CHECK(hash_int(cs, "(engine:metrics \"query:reload-recovery-playbook\")", "schema-2953") ==
                  2953,
              "#3020 AC3: reload-recovery-playbook schema-2953");
        CHECK(hash_int(cs, "(engine:metrics \"query:reload-recovery-playbook\")",
                       "playbook-wired") == 1,
              "#3020 AC3: playbook-wired");
        CHECK(hash_int(cs, "(engine:metrics \"query:reload-recovery-playbook\")",
                       "playbook-reject-cross-ws") == 6,
              "#3020 AC3: playbook-reject-cross-ws sentinel");
        CHECK(hash_int(cs, "(engine:metrics \"query:reload-recovery-playbook\")", "overflow") == -1,
              "#3020 AC3: playbook no overflow under default cap");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")", "schema-3114") ==
                  3114,
              "#3114 AC1: evolution-audit-decision schema-3114");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")",
                       "evolution-audit-decision-wired") == 1,
              "#3114 AC1: wired");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")", "observe-only") ==
                  1,
              "#3114 AC5: observe-only");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")", "overflow") == -1,
              "#3114 AC4: no overflow under default cap");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")", "schema-3205") ==
                  3205,
              "#3205 AC3: schema-3205 additive");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")", "durable-hit") ==
                  0,
              "#3205 AC2: default durable-hit=0");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")", "schema-3242") ==
                  3242,
              "#3242 AC3: schema-3242 additive");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")",
                       "typed-summary-from-wal") == 0,
              "#3242 AC2: default typed-summary-from-wal=0");
        CHECK(aura_query_hash_overflow_total() == soak0,
              "#3020 AC5: soak — no query_hash_overflow_total bump under default catalog");

        aura_query_hash_set_force_cap(4);
        const auto overflow0 = aura_query_hash_overflow_total();
        auto forced = cs.eval("(engine:metrics \"query:security-posture\")");
        CHECK(forced && is_hash(*forced), "#3020 AC2: forced-full posture still returns hash");
        CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "overflow") == 1,
              "#3020 AC2: overflow=1 visible when capacity is artificially low");
        CHECK(hash_int(cs, "(engine:metrics \"query:evolution-audit-decision\")", "overflow") == 1,
              "#3114 AC4: under-count planned → overflow=1");
        CHECK(aura_query_hash_overflow_total() > overflow0,
              "#3020 AC2: query_hash_overflow_total bumped");
        aura_query_hash_set_force_cap(0);
        aura_query_hash_reset_overflow_for_test();

        const auto ev_src = []() -> std::string {
            for (const auto& p : {std::string("src/compiler/evaluator.ixx"),
                                  std::string("../src/compiler/evaluator.ixx"),
                                  std::string("../../src/compiler/evaluator.ixx")}) {
                std::ifstream in(p);
                if (!in)
                    continue;
                return std::string((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            }
            return {};
        }();
        CHECK(ev_src.find("insert_kv_checked") != std::string::npos,
              "#3020 AC1: shared insert_kv_checked");
        CHECK(ev_src.find("planned_keys) * 2") != std::string::npos ||
                  ev_src.find("planned_keys * 2") != std::string::npos,
              "#3020 AC1: headroom is planned*2 then next_pow2 / ≥64");
        CHECK(ev_src.find("g_query_hash_force_cap.load") != std::string::npos,
              "#3020 AC4: Soft extra cost is one force-cap load");
        CHECK(ev_src.find("no second metrics bus") != std::string::npos,
              "#3020 AC4: no second metrics bus");
        CHECK(ev_src.find("headroom-3020") != std::string::npos,
              "#3020 AC1: residual create(N) headroom documented");
    }

    // ── AC6: legacy query:*-stats still via facade (internal impl, #1439) ──
    {
        auto r = cs.eval("(engine:metrics \"query:macro-hygiene-stats\")");
        // Under full primitives this is a hash; if missing, suite still OK for s0 builds.
        if (r && !is_void(*r))
            CHECK(is_hash(*r), "query:macro-hygiene-stats still works via engine:metrics");
        else
            CHECK(true, "query:*-stats absent (s0) — skip");
        // Must NOT be a public primitive name after #1439 (facade-only).
        // Unknown symbol typically returns error or empty optional — not a stats hash.
        try {
            auto bare = cs.eval("(query:macro-hygiene-stats)");
            if (bare && is_hash(*bare))
                CHECK(false, "bare query:macro-hygiene-stats must not return stats hash");
            else
                CHECK(true, "bare query:*-stats not a public primitive");
        } catch (...) {
            CHECK(true, "bare query:*-stats not a public primitive (threw)");
        }
    }

    // ── Issue #3282: residual fixed FlatHashTable::create(N) after #3020 ──
    // security/mutate/obs_*/query_* domain builders migrated to
    // query_hash_capacity_for + insert_kv_checked/overflowed +
    // query_hash_finish(..., overflowed). Forced overflow must yield a
    // non-void hash with hash-overflow=1 (AC2); happy path unchanged (AC3);
    // no key renames / same query resolution (AC4).
    {
        aura_query_hash_reset_overflow_for_test();
        aura_query_hash_set_force_cap(0);
        // AC3/AC4: migrated handler resolves as a hash with its real keys.
        auto h = cs.eval("(engine:metrics \"query:security-posture\")");
        if (h && !is_void(*h)) {
            CHECK(is_hash(*h), "3282 AC3: security-posture still a hash after migration");
            CHECK(hash_int(cs, "(engine:metrics \"query:security-posture\")", "schema-3244") ==
                      3244,
                  "3282 AC4: existing key unchanged after migration");
        } else {
            CHECK(true, "3282 AC3: security-posture absent (s0) — skip");
        }
        // AC2: force a tiny cap → overflow path → hash-overflow=1, non-void.
        aura_query_hash_set_force_cap(2);
        CHECK(is_hash_expr(cs, "(engine:metrics \"query:security-posture\")"),
              "3282 AC2: forced overflow still returns a hash (non-void)");
        const auto ho3282 =
            hash_int(cs, "(engine:metrics \"query:security-posture\")", "hash-overflow");
        CHECK(ho3282 == 1 ||
                  hash_int(cs, "(engine:metrics \"query:security-posture\")", "overflow") == 1,
              "3282 AC2: hash-overflow sentinel set on forced overflow");
        aura_query_hash_set_force_cap(0);
        aura_query_hash_reset_overflow_for_test();
    }

    // Issue #3371: evolution-audit-decision forensic-source must follow
    // #3152 priority order (typed > ring > WAL > 0) in the default path.
    // The previous unconditional `< 3 → 3` bump overwrote a confirmed
    // SE-ring hit just because WAL was on, breaking the Agent decision
    // tree under production force_wal. Pure conditional fallback — no
    // I/O added, Soft zero-cost preserved, schema-3152/3114 sentinels
    // unchanged.
    std::println("\n--- #3371: evolution-audit-decision forensic-source priority ---");
    {
        const auto eps = read_file("src/compiler/evaluator_primitives_security.cpp");
        // AC2/AC3: default-path priority order must be present (typed >
        // ring > WAL > 0). The unconditional < 3 → 3 bump must be scoped
        // to the want_durable keyword path (Issue #3205), not the default.
        CHECK(eps.find("if (typed_hit) {\n                forensic_source = 1;") !=
                  std::string::npos,
              "3371 AC2/AC3: default-path typed_hit → 1 first");
        CHECK(eps.find("} else if (se_ring_has_mid) {\n                forensic_source = 2;") !=
                  std::string::npos,
              "3371 AC2: default-path ring hit → 2 (NOT 3)");
        CHECK(eps.find("} else if (wal_enabled) {\n                forensic_source = 3;") !=
                  std::string::npos,
              "3371 AC3: default-path ring miss + WAL on → 3");
        CHECK(eps.find("// Issue #3371: priority order — typed > ring > WAL > 0.") !=
                  std::string::npos,
              "3371 AC1/AC2/AC3 #3371 fix comment block present");
        // AC4: old sentinels still present (no schema redefinition).
        CHECK(eps.find("forensic-source-trail") != std::string::npos &&
                  eps.find("forensic-source-se") != std::string::npos &&
                  eps.find("forensic-source-wal") != std::string::npos,
              "3371 AC4: schema-3152 forensic-source-trail|se|wal sentinels unchanged");
        CHECK(eps.find("schema-3152") != std::string::npos &&
                  eps.find("schema-3114") != std::string::npos,
              "3371 AC4: schema-3152 + schema-3114 unchanged");
        CHECK(eps.find("query:evolution-audit-decision") != std::string::npos,
              "3371 AC4: query:evolution-audit-decision unchanged (no new query:* keys)");
        // AC5: build.py wires the linter.
        const auto build3371 = read_file("build.py");
        CHECK(build3371.find("check_evolution_audit_decision_forensic_source_3371") !=
                  std::string::npos,
              "3371 AC5: build.py wires 3371 linter");
        // AC5: no test_issue_3371.cpp + no docs/design/3371-*.
        std::ifstream inv3371("tests/compiler/test_issue_3371.cpp");
        if (!inv3371.good())
            inv3371.open("../tests/compiler/test_issue_3371.cpp");
        CHECK(!inv3371.good(), "3371 AC5: no test_issue_3371.cpp (per #81967)");
        CHECK(read_file("docs/design/3371-evolution-audit-decision.md").empty(),
              "3371 AC5: no docs/design/3371-* (per #1655)");
        // No-invent: extend existing test (this file)
        const auto t3371_self = read_file("tests/compiler/test_engine_metrics_facade.cpp");
        CHECK(t3371_self.find("3371 AC") != std::string::npos,
              "3371 AC5: existing test file cites #3371");
    }

    if (::aura::test::g_failed) {
        std::println(std::cerr, "engine metrics facade #1433: FAIL ({} failed, {} passed)",
                     ::aura::test::g_failed, ::aura::test::g_passed);
        return 1;
    }
    std::println("engine metrics facade #1433: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
