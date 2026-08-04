// @category: unit
// @reason: Issue #2564 — ADT match exhaustiveness goal table + delta
//          reverify roots for Soft delta fidelity.
//
//   AC1: note goals + invalidate by ADT → reverify roots; Soft recheck path
//   AC2: no ADT/match mutation → zero invalidate / reverify-root counters
//   AC3: table size capped (AURA_ADT_GOAL_TABLE_CAP / default 256)
//   AC4: additive schema-2564 + source-cite
//   AC5: #2223/#2264 hard-gate + match_sites_present remain authoritative

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.type;

namespace {

using aura::compiler::AdtMatchGoal;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::ConstraintSystem;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::test::g_failed;
using aura::test::g_passed;

struct UnitCs {
    TypeRegistry reg;
    ConstraintSystem cs;
    CompilerMetrics m;
    UnitCs()
        : cs(reg) {
        cs.set_metrics(&m);
    }
};

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
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: note + invalidate → reverify roots ──
static void ac1_note_invalidate_reverify() {
    std::println("\n--- #2564 AC1: note goals + invalidate → reverify roots ---");
    UnitCs u;
    u.cs.set_current_epoch(1);
    u.cs.note_adt_match_goal(/*match_node=*/10, /*adt_type_id=*/42, /*hash=*/0xabc);
    u.cs.note_adt_match_goal(/*match_node=*/11, /*adt_type_id=*/42, /*hash=*/0xdef);
    u.cs.note_adt_match_goal(/*match_node=*/12, /*adt_type_id=*/99, /*hash=*/1);
    CHECK(u.cs.adt_match_goals_size() == 3, "AC1: three goals noted");
    CHECK(u.m.adt_goal_note_total.load() == 3, "AC1: note_total == 3");

    // Upsert same match_node
    u.cs.note_adt_match_goal(10, 42, 0x111);
    CHECK(u.cs.adt_match_goals_size() == 3, "AC1: upsert does not grow table");
    CHECK(u.cs.adt_match_goals_for_test()[0].covered_variants_hash == 0x111 ||
              u.cs.adt_match_goals_for_test()[1].covered_variants_hash == 0x111 ||
              u.cs.adt_match_goals_for_test()[2].covered_variants_hash == 0x111,
          "AC1: upsert updates hash");

    const auto n = u.cs.invalidate_adt_goals_for(42);
    CHECK(n == 2, "AC1: invalidate drops 2 goals for adt 42");
    CHECK(u.cs.adt_match_goals_size() == 1, "AC1: one goal remains (adt 99)");
    CHECK(u.cs.adt_reverify_roots_size() == 2, "AC1: two reverify roots");

    auto roots = u.cs.drain_adt_reverify_roots();
    CHECK(roots.size() == 2, "AC1: drain returns 2 roots");
    CHECK(u.cs.adt_reverify_roots_size() == 0, "AC1: drain clears roots");

    // Source wiring
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("note_adt_match_goal") != std::string::npos, "AC1: note wired in impl");
    CHECK(tci.find("invalidate_adt_goals_for") != std::string::npos ||
              tci.find("g_adt_reverify_pending_tls") != std::string::npos,
          "AC1: invalidate / pending reverify path");
    CHECK(tci.find("drain_adt_reverify_roots") != std::string::npos, "AC1: drain in partial path");
    CHECK(tci.find("absorb_pending_adt_reverify_roots") != std::string::npos, "AC1: absorb TLS");
}

// ── AC2: empty → zero ──
static void ac2_zero_work() {
    std::println("\n--- #2564 AC2: no goals → zero invalidate ---");
    UnitCs u;
    CHECK(u.cs.adt_match_goals_size() == 0, "AC2: empty table");
    CHECK(u.cs.invalidate_adt_goals_for(1) == 0, "AC2: invalidate empty → 0");
    CHECK(u.cs.adt_reverify_roots_size() == 0, "AC2: no roots");
    CHECK(u.m.adt_goal_invalidate_total.load() == 0, "AC2: invalidate counter 0");
    CHECK(u.m.adt_reverify_root_total.load() == 0, "AC2: reverify counter 0");
    auto roots = u.cs.drain_adt_reverify_roots();
    CHECK(roots.empty(), "AC2: drain empty");
}

// ── AC3: cap ──
static void ac3_cap() {
    std::println("\n--- #2564 AC3: table size capped ---");
    const auto cap = ConstraintSystem::adt_goal_table_cap();
    CHECK(cap >= 1, "AC3: cap >= 1");
    CHECK(cap <= 1'000'000, "AC3: cap sane");

    UnitCs u;
    // Force small cap via env is process-static after first call — test drop path
    // by overflowing default if we fill past size... better test: fill many and
    // ensure size never exceeds cap.
    const auto n = std::min(cap + 5, cap + 5);
    for (std::size_t i = 1; i <= n; ++i)
        u.cs.note_adt_match_goal(static_cast<std::uint32_t>(i), 1, i);
    CHECK(u.cs.adt_match_goals_size() <= cap, "AC3: size <= cap");
    if (n > cap)
        CHECK(u.m.adt_goal_cap_drop_total.load() > 0, "AC3: cap drop when overflow");

    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("AURA_ADT_GOAL_TABLE_CAP") != std::string::npos, "AC3: env cap");
    CHECK(tci.find("adt_goal_cap_drop_total") != std::string::npos, "AC3: cap drop counter");
}

// ── AC4: schema ──
static void ac4_schema() {
    std::println("\n--- #2564 AC4: schema-2564 + source-cite ---");
    const auto tixx = read_file("src/compiler/type_checker.ixx");
    CHECK(tixx.find("AdtMatchGoal") != std::string::npos, "AC4: AdtMatchGoal struct");
    CHECK(tixx.find("note_adt_match_goal") != std::string::npos, "AC4: note API");
    CHECK(tixx.find("invalidate_adt_goals_for") != std::string::npos, "AC4: invalidate API");
    CHECK(tixx.find("#2564") != std::string::npos, "AC4: cites #2564");

    const auto om = read_file("src/compiler/observability_metrics.h");
    CHECK(om.find("adt_goal_invalidate_total") != std::string::npos, "AC4: metrics");
    CHECK(om.find("adt_reverify_root_total") != std::string::npos, "AC4: reverify metric");

    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("schema-2564") != std::string::npos, "AC4: schema-2564");
    CHECK(q.find("adt-goal-table-size") != std::string::npos, "AC4: table-size key");
    CHECK(q.find("adt-goal-invalidate-total") != std::string::npos, "AC4: invalidate key");
    CHECK(q.find("adt-reverify-root-total") != std::string::npos, "AC4: reverify key");

    CompilerService cs;
    CHECK(href(cs, "schema-2564") == 2564, "AC4: live schema-2564");
    CHECK(href(cs, "adt-goal-table-wired") == 1, "AC4: wired");
    CHECK(href(cs, "adt-goal-invalidate-total") >= 0, "AC4: invalidate queryable");
    CHECK(href(cs, "adt-reverify-root-total") >= 0, "AC4: reverify queryable");
    CHECK(href(cs, "adt-goal-table-size") >= 0, "AC4: table size queryable");
}

// ── AC5: hard-gate retained ──
static void ac5_hard_gate_retained() {
    std::println("\n--- #2564 AC5: #2223/#2264 hard-gate retained ---");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(aud.find("adt_exhaustiveness_hard_gate_wired") != std::string::npos ||
              aud.find("adt_invariant_fail") != std::string::npos,
          "AC5: ADT invariant counters present");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("match_sites") != std::string::npos ||
              emb.find("match_sites_present") != std::string::npos,
          "AC5: match_sites force audit retained");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("check_match_exhaustiveness") != std::string::npos,
          "AC5: exhaustiveness check retained");
    CHECK(tci.find("invalidate_match_exhaust_for_adt_type") != std::string::npos,
          "AC5: existing invalidate retained");
    // Goal path does not remove hard-gate
    CHECK(read_file("tests/compiler/test_adt_hard_gate_exhaustiveness.cpp").size() > 0 ||
              read_file("tests/compiler/test_adt_exhaustiveness_audit.cpp").size() > 0,
          "AC5: existing ADT hard-gate tests still present");
}

} // namespace

int run_test_adt_match_goal_table() {
    std::println("=== Issue #2564: ADT match goal table + reverify roots ===");
    ac1_note_invalidate_reverify();
    ac2_zero_work();
    ac3_cap();
    ac4_schema();
    ac5_hard_gate_retained();
    std::println("\n=== #2564: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_adt_match_goal_table();
}
#endif
