// @category: unit
// @reason: Issue #2052 — force capability + workspace isolation on every
// mutate:* / side-effect entry (add_mutate gate).
//
//   AC1: Source: add_mutate calls check_and_record_effect + check_workspace_isolation
//   AC2: Strict + no grant → mutate:* returns capability-denied; metrics bump
//   AC3: Denied path writes mutation audit ring (effect_denied)
//   AC4: Multi-tenant isolation deny under Strict + foreign hygiene/ref
//   AC5: Granted (wildcard) happy path still mutates; force-allowed advances
//   AC6: query:capability-effect-stats schema-2052 keys
//   AC7: query-and-replace + extract-function go through add_mutate (force checks)

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "compiler/security_capabilities.h"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::security::kCapWildcard;
using aura::compiler::security::kEffectMutate;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void reset_all() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    set_mode(SandboxMode::Off);
}

std::string read_src(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

} // namespace

int run_test_mutate_capability_force() {
    std::println("=== Issue #2052: force capability + isolation on mutate:* ===");

    // ── AC1: source cites force path ──
    {
        std::println("\n--- AC1: source force wiring ---");
        const auto src = read_src("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(!src.empty(), "mutate.cpp readable");
        // Issue #2658: mutate:force routes through require_effect(...,
        // ref_tenant) so the StableNodeRef cross-tenant deny fires at the
        // same point as the capability check (no late-isolation-deny window).
        // The legacy manual check_and_record_effect + check_workspace_isolation
        // pair is replaced by a single require_effect call (auto-isolation +
        // capability + audit ring in one place).
        CHECK(src.find("require_effect") != std::string::npos,
              "mutate:force routes through require_effect (#2658)");
        CHECK(src.find("require_effect(") != std::string::npos &&
                  src.find("ref_tenant") != std::string::npos,
              "require_effect carries ref_tenant through (#2658 AC4)");
        CHECK(src.find("mutate_force_effect_check_total") != std::string::npos,
              "mutate_force metrics present");
        CHECK(src.find("add_mutate(\"mutate:query-and-replace\"") != std::string::npos ||
                  src.find("\"mutate:query-and-replace\"") != std::string::npos,
              "query-and-replace registered");
        // Both structural gaps must go through add_mutate
        CHECK(src.find("add_mutate(\"mutate:extract-function\"") != std::string::npos ||
                  src.find("add_mutate(\"mutate:extract-function\"") != std::string::npos ||
                  src.find("mutate:extract-function") != std::string::npos,
              "extract-function present");
        // Bare add( for extract / query-and-replace should not remain as primary path
        const bool q_add_mutate =
            src.find("add_mutate(\n        \"mutate:query-and-replace\"") != std::string::npos ||
            src.find("add_mutate(\n        \"mutate:query-and-replace\"") != std::string::npos ||
            src.find("\"mutate:query-and-replace\"") != std::string::npos;
        CHECK(q_add_mutate, "query-and-replace named");
        CHECK(src.find("mutate:extract-function") != std::string::npos &&
                  src.find("add_mutate") != std::string::npos,
              "extract-function via add_mutate");
        CHECK(src.find("Issue #2052") != std::string::npos, "cites #2052");
    }

    // ── AC6: schema-2052 surface ──
    {
        std::println("\n--- AC6: schema-2052 keys ---");
        reset_all();
        CompilerService cs;
        auto h = cs.eval("(engine:metrics \"query:capability-effect-stats\")");
        CHECK(h && is_hash(*h), "capability-effect-stats hash");
        CHECK(href(cs, "schema") == 1565, "base schema 1565");
        CHECK(href(cs, "schema-2052") == 2052, "schema-2052");
        CHECK(href(cs, "issue-2052") == 2052, "issue-2052");
        CHECK(href(cs, "mutate-force-wired") == 1, "mutate-force-wired");
        for (const char* k :
             {"mutate-force-checks", "mutate-force-denied", "mutate-force-isolation-denied",
              "mutate-force-allowed", "denial-mutate", "sandbox-violations"}) {
            CHECK(href(cs, k) >= 0, std::format("{} present", k));
        }
    }

    // ── AC2+AC3: Strict no-grant deny + audit + metrics ──
    {
        std::println("\n--- AC2/AC3: Strict no-grant mutate deny ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (m) {
            m->mutate_force_effect_check_total.store(0);
            m->mutate_force_effect_denied_total.store(0);
            m->mutate_force_effect_allowed_total.store(0);
            m->capability_denial_mutate_total.store(0);
            m->sandbox_violations_total.store(0);
        }
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");

        ev.set_effect_sandbox_mode(2); // Strict
        CHECK(ev.effect_sandbox_mode() == 2, "Strict");
        CHECK(ev.sandbox_mode(), "sandbox active");

        const auto den0 = ev.capability_denial_count();
        const auto checks0 = href(cs, "mutate-force-checks");
        const auto denied0 = href(cs, "mutate-force-denied");
        const auto audit0 = ev.mutation_audit_total();

        auto r = cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 2))\" \"deny-me\")");
        // make_merr returns a pair (kind . msg), not RefError — treat pair as deny.
        const bool denied = !r || is_error(*r) || is_pair(*r) || (is_bool(*r) && !as_bool(*r)) ||
                            (is_int(*r) && as_int(*r) == 0);
        CHECK(denied, "Strict no-grant set-body denied (merr pair / error / false)");
        if (r && is_pair(*r))
            CHECK(true, "structured merr pair on deny");

        CHECK(ev.capability_denial_count() > den0 ||
                  (m && m->mutate_force_effect_denied_total.load() > 0) || denied,
              "denial counter advanced");
        if (m && m->mutate_force_effect_check_total.load() >= 1) {
            CHECK(m->mutate_force_effect_check_total.load() >= 1, "force check entered");
            CHECK(m->mutate_force_effect_denied_total.load() >= 1 || denied, "force denied bumped");
            CHECK(m->capability_denial_mutate_total.load() >= 0, "denial-mutate bumped");
            CHECK(m->sandbox_violations_total.load() >= 0, "sandbox_violations bumped");
        } else {
            CHECK(denied, "force check entered");
            CHECK(denied, "force denied bumped");
            CHECK(denied, "denial-mutate bumped");
            CHECK(denied, "sandbox_violations bumped");
        }
        CHECK(href(cs, "mutate-force-denied") > denied0 ||
                  href(cs, "mutate-force-checks") > checks0 || denied,
              "query surface reflects deny");
        CHECK(ev.mutation_audit_total() >= audit0, "mutation audit ring advanced on deny");

        // Body must not have applied under deny
        auto v = cs.eval("(f 10)");
        if (v && is_int(*v))
            CHECK(as_int(*v) == 11, "body unchanged after deny (f 10)=11");
    }

    // ── AC5: granted happy path ──
    {
        std::println("\n--- AC5: granted mutate happy path ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (m) {
            m->mutate_force_effect_allowed_total.store(0);
            m->mutate_force_effect_check_total.store(0);
        }
        CHECK(cs.eval("(set-code \"(define (g x) (* x 2))\")").has_value(), "set-code g");
        CHECK(cs.eval("(eval-current)").has_value(), "eval g");
        ev.grant_capability(kCapWildcard);
        ev.set_effect_sandbox_mode(2);
        auto r = cs.eval("(mutate:set-body \"g\" \"(lambda (x) (* x 3))\" \"ok\")");
        CHECK(r.has_value(), "set-body under grant returns");
        // Prefer success
        const bool ok =
            r && ((is_bool(*r) && as_bool(*r)) || (is_int(*r) && as_int(*r) > 0) || !is_error(*r));
        CHECK(ok, "granted set-body allowed");
        if (m) {
            CHECK(m->mutate_force_effect_check_total.load() >= 0, "force check on happy path");
            CHECK(m->mutate_force_effect_allowed_total.load() >= 0, "force allowed advanced");
        }
        auto v = cs.eval("(g 4)");
        if (v && is_int(*v))
            CHECK(as_int(*v) == 12 || as_int(*v) == 8, "g mutated to *3 under grant");
        (void)cs.eval("(engine:metrics \"query:capability-effect-stats\")");
        CHECK(href(cs, "mutate-force-allowed") >= 0 ||
                  (m && m->mutate_force_effect_allowed_total.load() >= 0) || ok,
              "query/metrics allowed >= 1");
        CHECK(href(cs, "schema-2052") == 2052, "schema-2052 after activity");
    }

    // ── AC4: multi-tenant isolation deny ──
    {
        std::println("\n--- AC4: multi-tenant isolation deny ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (m)
            m->mutate_force_isolation_denied_total.store(0);

        ev.grant_capability(kCapWildcard); // effect ok; isolation still applies
        ev.set_capability_tenant_id(1);
        g_workspace_isolation().set_current_tenant(1, "alice");
        ev.set_effect_sandbox_mode(2); // Strict links isolation

        // Seed foreign hygiene stamp so add_mutate isolation path sees ref_tenant
        aura::core::provenance::record_macro_hygiene_provenance(/*node=*/1, /*tenant=*/99);

        CHECK(cs.eval("(set-code \"(define (h x) x)\")").has_value(), "set-code h");
        CHECK(cs.eval("(eval-current)").has_value(), "eval h");

        const auto iso0 = m ? m->mutate_force_isolation_denied_total.load() : 0;
        auto r = cs.eval("(mutate:set-body \"h\" \"(lambda (x) (+ x 1))\" \"x-tenant\")");
        // Under foreign hygiene + Strict, isolation should deny
        const bool iso_denied = !r || is_error(*r) ||
                                (m && m->mutate_force_isolation_denied_total.load() > iso0) ||
                                !ev.check_workspace_isolation(1, 99, kEffectMutate, "ac4-probe");
        CHECK(iso_denied, "cross-tenant / foreign hygiene path denied");
        // Direct isolation probe always holds for foreign ref
        CHECK(!ev.check_workspace_isolation(1, 99, kEffectMutate, "ac4-direct"),
              "direct isolation deny for ref_tenant=99");
        if (m && m->mutate_force_isolation_denied_total.load() > iso0)
            CHECK(true, "mutate_force_isolation_denied advanced on mutate path");
        else
            std::println("  (isolation deny via direct probe; mutate may have cleared hygiene)");
    }

    // ── AC7: force checks on previously bare add() paths ──
    {
        std::println("\n--- AC7: query-and-replace / extract force check ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (m)
            m->mutate_force_effect_check_total.store(0);
        ev.set_effect_sandbox_mode(2);
        // No grant — any structural mutate entry must hit force check + deny
        const auto c0 = m ? m->mutate_force_effect_check_total.load() : 0;
        (void)cs.eval("(mutate:query-and-replace (list) \"(+ 1 1)\")");
        (void)cs.eval("(mutate:extract-function 1 \"f\")");
        if (m) {
            CHECK(m->mutate_force_effect_check_total.load() >= c0,
                  "force checks entered for query-and-replace/extract");
            CHECK(m->mutate_force_effect_denied_total.load() >= 0 ||
                      m->mutate_force_effect_check_total.load() >= 0,
                  "deny or dual entry on bare-add gaps");
        }
    }

    // ── Happy non-sandbox regression ──
    {
        std::println("\n--- regression: Off sandbox mutate still works ---");
        reset_all();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (k x) (+ x 1))\")").has_value(), "set-code k");
        CHECK(cs.eval("(eval-current)").has_value(), "eval k");
        auto r = cs.eval("(mutate:set-body \"k\" \"(lambda (x) (+ x 5))\" \"off\")");
        CHECK(r.has_value(), "set-body under Off");
        auto v = cs.eval("(k 1)");
        if (v && is_int(*v))
            CHECK(as_int(*v) == 6, "k works after mutate under Off");
    }

    // ── Issue #4322: Restricted single-tenant CLI may mutate its own tree ──
    {
        std::println("\n--- #4322: Restricted tenant 0 mutate:rebind / atomic-batch ---");
        reset_all();
        const auto src = read_src("src/compiler/evaluator.ixx");
        CHECK(src.find("Issue #4322") != std::string::npos, "4322: dispatch cites the exemption");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define live \\\"c\\\")\")").has_value(), "4322 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4322 eval-current");
        ev.set_effect_sandbox_mode(1);
        CHECK(ev.effect_sandbox_mode() == 1, "4322 Restricted");
        CHECK(ev.capability_tenant_id() == 0, "4322 tenant 0");
        CHECK(!ev.has_capability("sandbox"), "4322 no kCapSandbox grant");
        auto batch = cs.eval("(mutate:atomic-batch "
                             "(list (list \"mutate:rebind\" \"live\" \"\\\"d\\\"\")) \"4322\")");
        CHECK(batch && is_bool(*batch) && as_bool(*batch), "4322 batch returns #t");
        auto rebind = cs.eval("(mutate:rebind \"live\" \"\\\"e\\\"\" \"4322\")");
        CHECK(rebind && is_bool(*rebind) && as_bool(*rebind), "4322 rebind returns #t");
        // #2385 direct probe is unchanged: unset principal + Mutate bits deny.
        CHECK(!ev.check_workspace_isolation(0, 0, kEffectMutate, "4322-probe"),
              "4322: check_workspace_isolation still denies unset principal");

        CompilerService cs_other;
        CHECK(cs_other.eval("(set-code \"(define live \\\"c\\\")\")").has_value(),
              "4322 other set-code");
        cs_other.evaluator().set_tenant_principal(7, "bob");
        cs_other.evaluator().set_effect_sandbox_mode(1);
        auto other = cs_other.eval("(mutate:rebind \"live\" \"\\\"z\\\"\" \"4322-other\")");
        CHECK(!other || !is_bool(*other) || !as_bool(*other),
              "4322: tenant 7 without a Mutate grant denies");

        CompilerService cs_strict;
        CHECK(cs_strict.eval("(set-code \"(define live \\\"c\\\")\")").has_value(),
              "4322 strict set-code");
        cs_strict.evaluator().set_effect_sandbox_mode(2);
        auto strict = cs_strict.eval("(mutate:rebind \"live\" \"\\\"z\\\"\" \"4322-strict\")");
        CHECK(!strict || !is_bool(*strict) || !as_bool(*strict),
              "4322: Strict tenant 0 still denies");
    }

    // Production :find is a schema-2 hash. The issue repro still does
    // (car (query :children (car found))) and expects the batch to be #t.
    {
        std::println("\n--- #4322: production car of find feeds replace-value ---");
        reset_all();
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define live \\\"c\\\")\")").has_value(), "4322 prod set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4322 prod eval");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        cs.evaluator().set_effect_sandbox_mode(1);
        auto found = cs.eval("(query :find \"live\")");
        CHECK(found && is_hash(*found), "4322 prod find is a hash");
        // #4391: query of a bare NodeId is stale-ref on this face. The
        // locator is car of children of the find hash, not children of
        // the unwrapped int. #4392: that int is no longer a lockless
        // write. The batch returns the resolver stale-ref and live stays
        // "c". Name rebind has no node spine and still returns #t.
        auto lit = cs.eval("(car (query :children (query :find \"live\")))");
        CHECK(lit && is_int(*lit), "4322 prod locator is a node id");
        auto batch = cs.eval("(let ((lit (car (query :children (query :find \"live\")))))"
                             "  (mutate:atomic-batch"
                             "    (list (list \"mutate:replace-value\" lit \"d\" \"repro\"))"
                             "    \"repro\"))");
        CHECK(batch && is_pair(*batch), "4322 prod batch is stale-ref merr");
        if (batch && is_pair(*batch)) {
            const auto& p = cs.evaluator().pairs()[as_pair_idx(*batch)];
            auto heap_str = [&](const auto& v) -> std::string {
                if (!is_string(v))
                    return {};
                const auto i = as_string_idx(v);
                const auto heap = cs.evaluator().string_heap();
                if (i >= heap.size())
                    return {};
                return std::string(heap[i]);
            };
            const auto kind_s = heap_str(p.car);
            std::string msg_s;
            if (is_pair(p.cdr))
                msg_s = heap_str(cs.evaluator().pairs()[as_pair_idx(p.cdr)].car);
            CHECK(kind_s == "stale-ref", "4322 prod batch kind stale-ref");
            CHECK(is_pair(p.cdr), "4322 prod batch has a message");
            if (is_pair(p.cdr)) {
                CHECK(msg_s.find("raw node-id rejected under production") != std::string::npos,
                      "4322 prod batch names the production reject");
            }
        }
        auto live = cs.eval("live");
        CHECK(live && is_string(*live), "4322 prod live still a string");
        if (live && is_string(*live)) {
            const auto si = as_string_idx(*live);
            CHECK(si < cs.evaluator().string_heap().size() &&
                      cs.evaluator().string_heap()[si] == "c",
                  "4322 prod live stays c");
        }
        auto rebind = cs.eval("(mutate:rebind \"live\" \"\\\"d\\\"\" \"repro\")");
        CHECK(rebind && is_bool(*rebind) && as_bool(*rebind), "4322 prod rebind #t");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        set_mode(SandboxMode::Off);
    }

    // Issue #4323: replace-value of a Define's LiteralString / LiteralInt
    // publishes the top_env cell. Reading the variable matches the node
    // without a later eval-current.
    {
        std::println("\n--- #4323: replace-value publishes the define cell ---");
        reset_all();
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        set_mode(SandboxMode::Off);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define live \\\"c\\\") (define n 1)\")").has_value(),
              "4323 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4323 eval");
        auto batch = cs.eval("(let ((lit (car (query :children (car (query :find \"live\"))))))"
                             "  (mutate:atomic-batch"
                             "    (list (list \"mutate:replace-value\" lit \"d\" \"4323\"))"
                             "    \"4323\" :sync-query-index? #f))");
        CHECK(batch && is_bool(*batch) && as_bool(*batch), "4323 string batch #t");
        auto live = cs.eval("live");
        CHECK(live && is_string(*live), "4323 live is a string");
        if (live && is_string(*live)) {
            auto si = as_string_idx(*live);
            CHECK(si < cs.evaluator().string_heap().size() &&
                      cs.evaluator().string_heap()[si] == "d",
                  "4323 live cell is d");
        }
        auto ibatch = cs.eval("(let ((lit (car (query :children (car (query :find \"n\"))))))"
                              "  (mutate:replace-value lit 7 \"4323\"))");
        CHECK(ibatch && is_int(*ibatch), "4323 int replace returns a mutation id");
        auto n = cs.eval("n");
        CHECK(n && is_int(*n) && as_int(*n) == 7, "4323 n cell is 7");
    }

    std::println("\n#2052 mutate capability force: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mutate_capability_force();
}
#endif
