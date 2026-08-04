// @category: unit
// @reason: Issue #2561 — Soft/Sampled blame chain completeness recovery +
//          miss escalation (one-shot Full sample; Soft observe-by-default).
//
//   AC1: Sampled incomplete → recover restores dual fields OR escalate
//   AC2: complete / no miss → zero recover/escalate counters
//   AC3: Full/#2221 hard-reject path preserved; Soft default observe
//   AC4: additive schema-2561 + source-cite
//   AC5: existing blame_commit_* / coercion_provenance_* remain authoritative

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "compiler/coercion_provenance_policy.hh"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.ast;
import aura.compiler.coercion_map;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::blame_soft_escalate_enabled;
using aura::compiler::blame_soft_escalate_pending_for_boundary;
using aura::compiler::clear_coercion_active_mutation_context;
using aura::compiler::CompilerService;
using aura::compiler::consume_blame_soft_escalate_for_boundary;
using aura::compiler::g_blame_commit_check_total;
using aura::compiler::g_blame_commit_incomplete_observe_total;
using aura::compiler::g_blame_commit_reject_total;
using aura::compiler::g_blame_soft_escalate_total;
using aura::compiler::g_blame_soft_recover_fail_total;
using aura::compiler::g_blame_soft_recover_total;
using aura::compiler::g_coercion_provenance_complete_total;
using aura::compiler::g_coercion_provenance_miss_total;
using aura::compiler::kBlameSoftRecoverIssue;
using aura::compiler::maybe_soft_recover_or_escalate_blame;
using aura::compiler::reset_coercion_provenance_miss_policy_for_test;
using aura::compiler::try_recover_blame_chain_soft;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::production_defaults_active;
using aura::compiler::typed_audit::set_strategy;
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_health(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:coercion-provenance-health\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void reset_2561() {
    reset_coercion_provenance_miss_policy_for_test();
    apply_dev_audit_defaults();
    set_strategy(AuditStrategy::Sampled);
    clear_coercion_active_mutation_context();
    g_blame_soft_recover_total.store(0, std::memory_order_relaxed);
    g_blame_soft_recover_fail_total.store(0, std::memory_order_relaxed);
    g_blame_soft_escalate_total.store(0, std::memory_order_relaxed);
    (void)consume_blame_soft_escalate_for_boundary();
}

// ── AC1: recover success + escalate-on-fail ──
static void ac1_recover_or_escalate() {
    std::println("\n--- #2561 AC1: Soft recover dual fields OR escalate ---");
    reset_2561();
    set_strategy(AuditStrategy::Sampled);

    StringPool pool;
    FlatAST flat;
    auto x = pool.intern("x");
    auto xv = flat.add_variable(x);
    auto lit = flat.add_literal(1);
    auto call = flat.add_call(xv, std::array<aura::ast::NodeId, 1>{lit});
    flat.root = call;
    // Log a real mid targeting lit so Soft recover can re-stamp dual fields.
    const auto mid = flat.add_mutation(lit, "soft-narrow", "Any", "Int", "ac1-recover");
    CHECK(mid != 0, "AC1: mutation id non-zero");
    CHECK(!flat.all_mutations().empty(), "AC1: mutation log non-empty");

    const auto rec0 = g_blame_soft_recover_total.load();
    const auto fail0 = g_blame_soft_recover_fail_total.load();
    const auto esc0 = g_blame_soft_escalate_total.load();
    const auto miss0 = g_coercion_provenance_miss_total.load();

    // had_miss_signal=true simulates Soft boundary with incomplete provenance.
    const bool recovered =
        maybe_soft_recover_or_escalate_blame(flat, mid, /*had_miss_signal=*/true);
    CHECK(recovered, "AC1: recover path restores dual fields");
    CHECK(g_blame_soft_recover_total.load() > rec0, "AC1: recover_total bumped");
    CHECK(g_blame_soft_recover_fail_total.load() == fail0, "AC1: no fail on success");
    CHECK(g_blame_soft_escalate_total.load() == esc0, "AC1: no escalate on success");
    // recovery_mode must not bump miss_total.
    CHECK(g_coercion_provenance_miss_total.load() == miss0, "AC1: recovery_mode no miss bump");
    CHECK(flat.provenance(lit) != 0, "AC1: provenance column re-stamped");

    // Fail path: mid with no matching log sites → fail + escalate when env on.
    reset_2561();
    set_strategy(AuditStrategy::Sampled);
    // Unset escalate env for pure Soft observe first.
    unsetenv("AURA_BLAME_SOFT_ESCALATE");
    CHECK(!blame_soft_escalate_enabled(), "AC1: Soft default escalate off");
    const auto fail1 = g_blame_soft_recover_fail_total.load();
    const auto esc1 = g_blame_soft_escalate_total.load();
    FlatAST empty_log;
    // mid nonzero but empty log → recover false, fail++, no escalate (Soft observe).
    const bool r2 =
        maybe_soft_recover_or_escalate_blame(empty_log, /*mid=*/42, /*had_miss_signal=*/true);
    CHECK(!r2, "AC1: empty log recover fails");
    CHECK(g_blame_soft_recover_fail_total.load() > fail1, "AC1: fail_total bumped");
    CHECK(g_blame_soft_escalate_total.load() == esc1, "AC1: Soft observe no escalate");
    CHECK(!blame_soft_escalate_pending_for_boundary(), "AC1: no escalate pending Soft default");

    // Escalate when env=1.
    setenv("AURA_BLAME_SOFT_ESCALATE", "1", 1);
    CHECK(blame_soft_escalate_enabled(), "AC1: env escalate on");
    const auto esc2 = g_blame_soft_escalate_total.load();
    const bool r3 =
        maybe_soft_recover_or_escalate_blame(empty_log, /*mid=*/42, /*had_miss_signal=*/true);
    CHECK(!r3, "AC1: still fail recover");
    CHECK(g_blame_soft_escalate_total.load() > esc2, "AC1: escalate_total bumped");
    CHECK(blame_soft_escalate_pending_for_boundary(), "AC1: escalate pending armed");
    CHECK(consume_blame_soft_escalate_for_boundary(), "AC1: consume one-shot escalate");
    CHECK(!blame_soft_escalate_pending_for_boundary(), "AC1: pending cleared after consume");
    unsetenv("AURA_BLAME_SOFT_ESCALATE");

    // production_defaults also escalate without env (Sampled host under prod).
    reset_2561();
    apply_production_audit_defaults();    // sets Full + production_defaults_active
    set_strategy(AuditStrategy::Sampled); // Soft path still Sampled; prod flags on
    CHECK(production_defaults_active(), "AC1: production defaults");
    unsetenv("AURA_BLAME_SOFT_ESCALATE");
    const auto esc3 = g_blame_soft_escalate_total.load();
    const bool r4 =
        maybe_soft_recover_or_escalate_blame(empty_log, /*mid=*/42, /*had_miss_signal=*/true);
    CHECK(!r4, "AC1: production Sampled still fails recover on empty log");
    CHECK(g_blame_soft_escalate_total.load() > esc3, "AC1: production Sampled escalate_total++");
    CHECK(blame_soft_escalate_pending_for_boundary(), "AC1: production escalate pending");
    (void)consume_blame_soft_escalate_for_boundary();
    apply_dev_audit_defaults();
    reset_2561();
}

// ── AC2: complete chain → zero work ──
static void ac2_complete_zero_work() {
    std::println("\n--- #2561 AC2: complete / no miss → zero recover/escalate ---");
    reset_2561();
    set_strategy(AuditStrategy::Sampled);
    StringPool pool;
    FlatAST flat;
    auto lit = flat.add_literal(1);
    (void)flat.add_mutation(lit, "ok", "A", "B", "complete");
    const auto rec0 = g_blame_soft_recover_total.load();
    const auto fail0 = g_blame_soft_recover_fail_total.load();
    const auto esc0 = g_blame_soft_escalate_total.load();
    // No miss signal → zero counter movement (fast path).
    const bool r = maybe_soft_recover_or_escalate_blame(flat, /*mid=*/1, /*had_miss_signal=*/false);
    CHECK(!r, "AC2: no miss → recover helper returns false");
    CHECK(g_blame_soft_recover_total.load() == rec0, "AC2: recover_total unchanged");
    CHECK(g_blame_soft_recover_fail_total.load() == fail0, "AC2: fail_total unchanged");
    CHECK(g_blame_soft_escalate_total.load() == esc0, "AC2: escalate_total unchanged");
    // mid==0 also zero work.
    (void)maybe_soft_recover_or_escalate_blame(flat, 0, true);
    CHECK(g_blame_soft_recover_total.load() == rec0, "AC2: mid0 no recover");
    CHECK(g_blame_soft_recover_fail_total.load() == fail0, "AC2: mid0 no fail");
}

// ── AC3: Full / Soft observe / #2221 preserved ──
static void ac3_full_and_soft_observe() {
    std::println("\n--- #2561 AC3: Full hard path + Soft observe default ---");
    reset_2561();
    unsetenv("AURA_BLAME_SOFT_ESCALATE");
    CHECK(!blame_soft_escalate_enabled(), "AC3: Soft escalate default off");

    // Full strategy: Soft recover is a no-op (leave #2221 alone).
    set_strategy(AuditStrategy::Full);
    FlatAST flat;
    const auto rec0 = g_blame_soft_recover_total.load();
    const auto fail0 = g_blame_soft_recover_fail_total.load();
    const auto esc0 = g_blame_soft_escalate_total.load();
    const bool r = maybe_soft_recover_or_escalate_blame(flat, 99, true);
    CHECK(!r, "AC3: Full skips Soft recover");
    CHECK(g_blame_soft_recover_total.load() == rec0, "AC3: Full no recover bump");
    CHECK(g_blame_soft_recover_fail_total.load() == fail0, "AC3: Full no fail bump");
    CHECK(g_blame_soft_escalate_total.load() == esc0, "AC3: Full no escalate bump");

    // Source: #2221 hard gate still present.
    const auto pol = read_file("src/compiler/coercion_provenance_policy.hh");
    CHECK(pol.find("kBlameCommitRequireIssue") != std::string::npos ||
              pol.find("#2221") != std::string::npos,
          "AC3: #2221 blame commit require retained");
    const auto gate = read_file("tests/compiler/test_blame_complete_commit_gate.cpp");
    CHECK(!gate.empty() || pol.find("require_blame_complete") != std::string::npos ||
              pol.find("g_require_blame_complete_on_commit") != std::string::npos,
          "AC3: blame-complete commit gate still authoritative");
    CHECK(pol.find("g_require_blame_complete_on_commit") != std::string::npos,
          "AC3: require_blame_complete_on_commit counter present");
}

// ── AC4: schema + source-cite ──
static void ac4_schema_source() {
    std::println("\n--- #2561 AC4: schema-2561 + source-cite ---");
    const auto pol = read_file("src/compiler/coercion_provenance_policy.hh");
    CHECK(pol.find("#2561") != std::string::npos, "AC4: policy cites #2561");
    CHECK(pol.find("g_blame_soft_recover_total") != std::string::npos, "AC4: recover counter");
    CHECK(pol.find("g_blame_soft_escalate_total") != std::string::npos, "AC4: escalate counter");
    CHECK(pol.find("AURA_BLAME_SOFT_ESCALATE") != std::string::npos, "AC4: escalate env");
    CHECK(kBlameSoftRecoverIssue == 2561, "AC4: issue stamp 2561");

    const auto cmap = read_file("src/compiler/coercion_map.ixx");
    CHECK(cmap.find("try_recover_blame_chain_soft") != std::string::npos, "AC4: recover helper");
    CHECK(cmap.find("maybe_soft_recover_or_escalate_blame") != std::string::npos,
          "AC4: boundary helper");
    CHECK(cmap.find("recovery_mode") != std::string::npos, "AC4: recovery_mode on fill");

    const auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(bound.find("maybe_soft_recover_or_escalate_blame") != std::string::npos,
          "AC4: boundary wires Soft recover");
    CHECK(bound.find("#2561") != std::string::npos, "AC4: boundary cites #2561");

    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("schema-2561") != std::string::npos, "AC4: schema-2561 in query");
    CHECK(q.find("blame-soft-recover-total") != std::string::npos, "AC4: recover query key");
    CHECK(q.find("query:type-incremental-fidelity-stats") != std::string::npos,
          "AC4: fidelity query host");

    reset_2561();
    CompilerService cs;
    CHECK(href(cs, "schema-2561") == 2561, "AC4: live fidelity schema-2561");
    CHECK(href(cs, "blame-soft-recover-total") >= 0, "AC4: recover total queryable");
    CHECK(href(cs, "blame-soft-recover-fail-total") >= 0, "AC4: fail total queryable");
    CHECK(href(cs, "blame-soft-escalate-total") >= 0, "AC4: escalate total queryable");
    CHECK(href(cs, "blame-soft-recover-wired") == 1, "AC4: wired flag");
    CHECK(href_health(cs, "schema-2561") == 2561, "AC4: health schema-2561");
}

// ── AC5: existing counters authoritative ──
static void ac5_existing_authoritative() {
    std::println("\n--- #2561 AC5: existing blame/provenance counters authoritative ---");
    // Soft recover must not redefine miss / blame_commit semantics.
    const auto pol = read_file("src/compiler/coercion_provenance_policy.hh");
    CHECK(pol.find("g_coercion_provenance_miss_total") != std::string::npos ||
              read_file("src/compiler/coercion_map.ixx").find("g_coercion_provenance_miss_total") !=
                  std::string::npos,
          "AC5: miss_total still present");
    CHECK(pol.find("g_blame_commit_reject_total") != std::string::npos ||
              pol.find("g_require_blame_complete_on_commit") != std::string::npos,
          "AC5: blame commit counters retained");
    // Touch existing counters (still live).
    (void)g_blame_commit_reject_total.load();
    (void)g_blame_commit_incomplete_observe_total.load();
    (void)g_blame_commit_check_total.load();
    (void)g_coercion_provenance_complete_total.load();
    const auto cmap = read_file("src/compiler/coercion_map.ixx");
    CHECK(cmap.find("if (!recovery_mode)") != std::string::npos,
          "AC5: recovery_mode gates miss/SLO (existing path untouched on normal fill)");
    // try_recover alone on empty is false and does not claim complete.
    FlatAST empty;
    CHECK(!try_recover_blame_chain_soft(empty, 1), "AC5: empty recover false");
}

} // namespace

int run_test_blame_soft_recover() {
    std::println("=== Issue #2561: Soft/Sampled blame recover + escalate ===");
    ac1_recover_or_escalate();
    ac2_complete_zero_work();
    ac3_full_and_soft_observe();
    ac4_schema_source();
    ac5_existing_authoritative();
    apply_dev_audit_defaults();
    reset_2561();
    unsetenv("AURA_BLAME_SOFT_ESCALATE");
    std::println("\n=== #2561: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_blame_soft_recover();
}
#endif
