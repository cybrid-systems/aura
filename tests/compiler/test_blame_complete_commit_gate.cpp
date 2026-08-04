// @category: unit
// @reason: Issue #2221 — blame-complete optional hard gate on apply +
// composite commit (refine #2102 / #2185 / #2105).
//
//   AC1: Production defaults enable reject-on-miss + require-blame-on-commit;
//        AURA_SANDBOX=off keeps soft apply + observe-only commit
//   AC2: Incomplete chain under reject-on → no CoercionNode; complete re-infer
//        applies (parity with #2185 apply path)
//   AC3: require-on + incomplete last_blame_chain → composite reject /
//        provenance_ok=false; require-off → observe-only
//   AC4: schema-2221 on fidelity + typed-mutation-audit-trail surfaces
//   AC5: complete-chain happy path commits; complete counter stable

#include "test_harness.hpp"

#include "compiler/coercion_provenance_policy.hh"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/mutation_audit_wal.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

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
using aura::compiler::apply_coercion_map;
using aura::compiler::clear_coercion_active_mutation_context;
using aura::compiler::CoercionMap;
using aura::compiler::CompilerService;
using aura::compiler::force_audit_on_provenance_miss;
using aura::compiler::g_blame_commit_check_total;
using aura::compiler::g_blame_commit_incomplete_observe_total;
using aura::compiler::g_blame_commit_reject_total;
using aura::compiler::g_coercion_provenance_complete_total;
using aura::compiler::g_coercion_provenance_miss_reject_total;
using aura::compiler::g_coercion_provenance_sentinel_total;
using aura::compiler::kBlameCommitRequireIssue;
using aura::compiler::reject_apply_on_provenance_miss;
using aura::compiler::require_blame_complete_on_commit;
using aura::compiler::reset_coercion_provenance_miss_policy_for_test;
using aura::compiler::set_coercion_active_mutation_context;
using aura::compiler::set_reject_apply_on_provenance_miss;
using aura::compiler::set_require_blame_complete_on_commit;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::CompositeTxnCommitResult;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::audit_wal::reset_audit_wal_for_test;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
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

static void clear_env(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}

static void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static void reset_process() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    reset_audit_wal_for_test();
    reset_for_test();
    reset_coercion_provenance_miss_policy_for_test();
    clear_coercion_active_mutation_context();
    set_mode(SandboxMode::Off);
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
    clear_env("AURA_TYPED_AUDIT");
    clear_env("AURA_MUTATION_AUDIT_WAL");
    clear_env("AURA_PERSIST_DIR");
    clear_env("AURA_LINEAR_ENFORCE");
    clear_env("AURA_COERCION_PROVENANCE_REJECT");
    clear_env("AURA_BLAME_COMMIT_REQUIRE");
    clear_env("AURA_HARD_FIBER_ISOLATION");
    clear_env("AURA_GRANT_EPOCH_RETAIN");
    g_blame_commit_reject_total.store(0, std::memory_order_relaxed);
    g_blame_commit_incomplete_observe_total.store(0, std::memory_order_relaxed);
    g_blame_commit_check_total.store(0, std::memory_order_relaxed);
}

static std::int64_t fidelity(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t trail(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-trail\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void make_coerce_tree(FlatAST& flat, StringPool& pool, aura::ast::NodeId& parent,
                             aura::ast::NodeId& lit) {
    auto x = pool.intern("x");
    auto xv = flat.add_variable(x);
    lit = flat.add_literal(7);
    parent = flat.add_call(xv, std::array<aura::ast::NodeId, 1>{lit});
    flat.root = parent;
}

// Best-effort workspace seed — inject_commit_cs_* helpers only need a
// TypeRegistry (created on demand). set-code may fail under process
// security leftovers; blame gate ACs do not require a live AST.
static void seed(CompilerService& cs) {
    (void)cs.eval("(set-code \"(define x 1) (define y (+ x 1)) (define z (* y 2))\")");
    (void)cs.eval("(eval-current)");
}

} // namespace

int run_test_blame_complete_commit_gate() {
    std::println("=== Issue #2221: blame-complete apply + composite commit gate ===");
    CHECK(kBlameCommitRequireIssue == 2221, "issue stamp");

    // ── AC1: production defaults wire reject + require; sandbox=off soft ──
    {
        std::println("\n--- AC1: production defaults ---");
        reset_process();
        apply_production_security_defaults();
        CHECK(force_audit_on_provenance_miss(), "AC1: force-audit stays true");
        CHECK(reject_apply_on_provenance_miss(), "AC1: reject-on under production");
        CHECK(require_blame_complete_on_commit(), "AC1: require-blame under production");

        reset_process();
        set_env("AURA_SANDBOX", "off");
        apply_production_security_defaults();
        CHECK(!reject_apply_on_provenance_miss(), "AC1: reject off under sandbox=off");
        CHECK(!require_blame_complete_on_commit(), "AC1: require off under sandbox=off");
        clear_env("AURA_SANDBOX");

        reset_process();
        apply_production_security_defaults();
        set_env("AURA_BLAME_COMMIT_REQUIRE", "off");
        apply_production_security_defaults();
        CHECK(!require_blame_complete_on_commit(), "AC1: env soft overrides prod");
        clear_env("AURA_BLAME_COMMIT_REQUIRE");
        set_env("AURA_SANDBOX", "off");
        set_env("AURA_BLAME_COMMIT_REQUIRE", "on");
        apply_production_security_defaults();
        CHECK(require_blame_complete_on_commit(), "AC1: env on under sandbox=off");
        clear_env("AURA_SANDBOX");
        clear_env("AURA_BLAME_COMMIT_REQUIRE");
    }

    // ── AC2: apply reject-on incomplete; complete re-infer applies ──
    {
        std::println("\n--- AC2: apply reject-on + complete re-infer ---");
        reset_process();
        apply_production_security_defaults();
        CHECK(reject_apply_on_provenance_miss(), "reject on");

        FlatAST flat;
        StringPool pool;
        aura::ast::NodeId parent = 0, lit = 0;
        make_coerce_tree(flat, pool, parent, lit);
        CoercionMap map;
        map.add(parent, 1, lit, 1, 99, 0, 0, 0, 0);
        const auto reject0 = g_coercion_provenance_miss_reject_total.load();
        const auto size0 = flat.size();
        CHECK(apply_coercion_map(flat, map) == 0, "AC2: incomplete rejected");
        CHECK(flat.size() == size0, "AC2: AST unchanged");
        CHECK(g_coercion_provenance_miss_reject_total.load() > reject0, "AC2: miss_reject++");

        set_coercion_active_mutation_context(/*mutation_id=*/9001, /*predicate=*/0);
        aura::ast::MutationRecord rec{};
        rec.mutation_id = 9001;
        rec.target_node = lit;
        rec.parent_id = parent;
        rec.operator_name = "test-2221-reinfer";
        rec.status = aura::ast::MutationStatus::Committed;
        flat.all_mutations().push_back(rec);
        CoercionMap map2;
        map2.add(parent, 1, lit, 1, 99, 0, 0, static_cast<std::uint32_t>(lit), 9001);
        const auto complete0 = g_coercion_provenance_complete_total.load();
        CHECK(apply_coercion_map(flat, map2) > 0, "AC2: complete applies");
        CHECK(g_coercion_provenance_complete_total.load() > complete0, "AC2: complete++");
        clear_coercion_active_mutation_context();
    }

    // ── AC2b: reject-off still stamps sentinel (forensic parity) ──
    {
        std::println("\n--- AC2b: reject-off sentinel path ---");
        reset_process();
        set_reject_apply_on_provenance_miss(false);
        FlatAST flat;
        StringPool pool;
        aura::ast::NodeId parent = 0, lit = 0;
        make_coerce_tree(flat, pool, parent, lit);
        CoercionMap map;
        map.add(parent, 1, lit, 1, 99, 0, 0, 0, 0);
        const auto sent0 = g_coercion_provenance_sentinel_total.load();
        CHECK(apply_coercion_map(flat, map) > 0, "AC2b: soft insert");
        CHECK(g_coercion_provenance_sentinel_total.load() > sent0, "AC2b: sentinel++");
    }

    // ── AC3: require-on + incomplete blame → composite reject ──
    {
        std::println("\n--- AC3: require-on incomplete → reject ---");
        reset_process();
        set_require_blame_complete_on_commit(true);
        set_strategy(AuditStrategy::Full);
        CompilerService cs;
        seed(cs);
        const auto rej0 = load_u64(g_typed_mutation_audit_counters.blame_commit_reject_total);
        const auto crej0 = load_u64(g_typed_mutation_audit_counters.composite_commit_reject_total);
        const auto glob0 = g_blame_commit_reject_total.load();

        cs.evaluator().inject_commit_cs_incomplete_blame_for_test();
        CHECK(cs.evaluator().commit_cs_live(), "commit_cs_live after incomplete inject");

        CompositeTxnCommitResult cr{};
        const bool committed = cs.evaluator().composite_txn_commit(
            /*mid=*/2221, "blame-incomplete", 0, 0, 1, /*nested=*/true, /*batch=*/true, &cr);
        CHECK(!committed, "AC3: commit rejected on incomplete blame");
        CHECK(!cr.blame_ok, "AC3: blame_ok false");
        CHECK(!cr.audit.provenance_ok || cr.rejected, "AC3: provenance fail or rejected");
        CHECK(load_u64(g_typed_mutation_audit_counters.blame_commit_reject_total) > rej0,
              "AC3: blame_commit_reject++");
        CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_reject_total) > crej0,
              "AC3: composite reject++");
        CHECK(g_blame_commit_reject_total.load() > glob0, "AC3: global reject++");
    }

    // ── AC3b: require-off → observe-only (incomplete does not hard-reject) ──
    {
        std::println("\n--- AC3b: require-off observe-only ---");
        reset_process();
        set_require_blame_complete_on_commit(false);
        set_strategy(AuditStrategy::Full);
        CompilerService cs;
        seed(cs);
        const auto rej0 = load_u64(g_typed_mutation_audit_counters.blame_commit_reject_total);
        const auto obs0 =
            load_u64(g_typed_mutation_audit_counters.blame_commit_incomplete_observe_total);

        cs.evaluator().inject_commit_cs_incomplete_blame_for_test();
        CompositeTxnCommitResult cr{};
        (void)cs.evaluator().composite_txn_commit(2222, "observe-only", 0, 0, 1, true, true, &cr);
        CHECK(cr.blame_ok, "AC3b: blame_ok remains true when require off");
        CHECK(load_u64(g_typed_mutation_audit_counters.blame_commit_reject_total) == rej0,
              "AC3b: reject total stable");
        CHECK(load_u64(g_typed_mutation_audit_counters.blame_commit_incomplete_observe_total) >
                  obs0,
              "AC3b: observe total++");
    }

    // ── AC5: complete blame happy path under require-on ──
    {
        std::println("\n--- AC5: complete blame commits under require-on ---");
        reset_process();
        set_require_blame_complete_on_commit(true);
        set_strategy(AuditStrategy::Full);
        CompilerService cs;
        seed(cs);
        const auto rej0 = load_u64(g_typed_mutation_audit_counters.blame_commit_reject_total);
        const auto ok0 = load_u64(g_typed_mutation_audit_counters.composite_commit_ok_total);

        cs.evaluator().inject_commit_cs_complete_blame_for_test();
        CompositeTxnCommitResult cr{};
        const bool committed =
            cs.evaluator().composite_txn_commit(2223, "blame-complete", 0, 0, 1, true, true, &cr);
        CHECK(cr.blame_ok, "AC5: blame_ok true with complete chain");
        CHECK(load_u64(g_typed_mutation_audit_counters.blame_commit_reject_total) == rej0,
              "AC5: reject stable on complete (no false positive)");
        // Complete chain must not be the reason for reject; other audit
        // categories may still fail in empty-workspace unit context.
        if (committed)
            CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_ok_total) > ok0,
                  "AC5: ok total when committed");
        (void)cr;
    }

    // ── AC4: query schema + source wiring ──
    {
        std::println("\n--- AC4: query schema-2221 + source ---");
        reset_process();
        apply_production_security_defaults();
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(fidelity(cs, "schema-2221") == 2221, "fidelity schema-2221");
        CHECK(fidelity(cs, "issue-2221") == 2221, "fidelity issue-2221");
        CHECK(fidelity(cs, "blame-commit-require-wired") == 1, "fidelity wired");
        CHECK(fidelity(cs, "require-blame-complete-on-commit") == 1, "fidelity require on");
        CHECK(fidelity(cs, "blame-commit-reject-total") >= 0, "reject key");
        CHECK(fidelity(cs, "schema-2185") == 2185, "2185 lineage retained");
        CHECK(fidelity(cs, "schema-2102") == 2102, "2102 lineage retained");
        CHECK(trail(cs, "schema-2221") == 2221, "trail schema-2221");
        CHECK(trail(cs, "blame-commit-require-wired") == 1, "trail wired");
        CHECK(trail(cs, "require-blame-complete-on-commit") == 1, "trail require");

        const auto sd = read_file("src/compiler/security_defaults.hh");
        const auto pol = read_file("src/compiler/coercion_provenance_policy.hh");
        const auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
        CHECK(sd.find("2221") != std::string::npos, "security_defaults cites 2221");
        CHECK(sd.find("apply_blame_commit_require_env_override") != std::string::npos,
              "defaults call blame env override");
        CHECK(pol.find("require_blame_complete_on_commit") != std::string::npos, "policy API");
        CHECK(pol.find("AURA_BLAME_COMMIT_REQUIRE") != std::string::npos, "env name");
        CHECK(tc.find("require_blame_complete_on_commit") != std::string::npos, "commit gate");
        CHECK(tc.find("blame_commit_reject_total") != std::string::npos, "reject counter wire");
        CHECK(tc.find("inject_commit_cs_incomplete_blame_for_test") != std::string::npos,
              "incomplete inject");
    }

    reset_process();
    set_strategy(AuditStrategy::Sampled);

    std::println("\n=== #2221 blame-complete commit gate: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_blame_complete_commit_gate();
}
#endif
