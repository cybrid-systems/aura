// @category: unit
// @reason: Issue #1878 — atomic-batch strong atomicity + multi-tenant
// isolation under Strict sandbox (sibling to #1900 dispatch coverage).
//
//   AC1: source cites #1878; strong atomicity docs; weak metric surface
//   AC2: atomic-batch:stats / stats-hash expose atomicity + tenant fields
//   AC3: successful batch under strong mode bumps strong commits; weak=0
//   AC4: Strict + :tenant-target foreign → deny + tenant-isolation-denials
//   AC5: unsupported op still clear error (no silent full success)
//   AC6: single-tenant batch under Strict still succeeds (no regression)

#include "compiler/observability_metrics.h"
#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"
#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

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
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

void reset_all() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    set_mode(SandboxMode::Off);
}

std::int64_t stats_href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (stats:get \"atomic-batch:stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t qhref(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:atomic-batch-stats-hash\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

bool setup_ws(CompilerService& cs) {
    return cs.eval("(set-code \"(define f (lambda (x) (* x 2))) (define g 1)\")").has_value();
}

} // namespace

int main() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: #1878 source surface ---");
        auto mut = read_first({"src/compiler/evaluator_primitives_mutate.cpp",
                               "../src/compiler/evaluator_primitives_mutate.cpp"});
        auto obs = read_first({"src/compiler/evaluator_primitives_obs_eval_12.cpp",
                               "../src/compiler/evaluator_primitives_obs_eval_12.cpp"});
        auto q = read_first({"src/compiler/evaluator_primitives_mutation.cpp",
                             "../src/compiler/evaluator_primitives_mutation.cpp"});
        auto hdr = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        CHECK(!mut.empty() && mut.find("#1878") != std::string::npos, "mutate cites #1878");
        CHECK(mut.find("STRONG") != std::string::npos || mut.find("strong") != std::string::npos,
              "strong atomicity documented");
        CHECK(mut.find(":tenant-target") != std::string::npos, ":tenant-target keyword");
        CHECK(mut.find("tenant-isolation-denied") != std::string::npos, "tenant deny path");
        CHECK(mut.find("bump_atomic_batch_strong_atomicity_commit") != std::string::npos,
              "strong commit bump");
        CHECK(!obs.empty() && obs.find("#1878") != std::string::npos, "stats cites #1878");
        CHECK(obs.find("weak-atomicity-used") != std::string::npos, "weak metric in stats");
        CHECK(obs.find("atomicity-mode") != std::string::npos, "atomicity-mode in stats");
        CHECK(!q.empty() && q.find("schema-1878") != std::string::npos, "stats-hash schema-1878");
        CHECK(!hdr.empty() && hdr.find("atomic_batch_weak_atomicity_used") != std::string::npos,
              "weak metric field");
        CHECK(hdr.find("atomic_batch_tenant_isolation_denials") != std::string::npos,
              "tenant denials field");
    }

    // ── AC2: stats surfaces ──
    {
        std::println("\n--- AC2: atomic-batch stats #1878 fields ---");
        reset_all();
        CompilerService cs;
        auto h = cs.eval(R"((stats:get "atomic-batch:stats"))");
        CHECK(h && is_hash(*h), "atomic-batch:stats hash");
        CHECK(stats_href(cs, "atomicity-mode") == 1, "atomicity-mode strong=1");
        CHECK(stats_href(cs, "weak-atomicity-used") == 0, "weak-atomicity-used starts 0");
        CHECK(stats_href(cs, "schema-1878") == 1878, "schema-1878");
        CHECK(stats_href(cs, "tenant-isolation-denials") >= 0, "tenant-isolation-denials field");
        auto qh = cs.eval(R"((engine:metrics "query:atomic-batch-stats-hash"))");
        CHECK(qh && is_hash(*qh), "query:atomic-batch-stats-hash hash");
        CHECK(qhref(cs, "atomicity-mode") == 1, "hash atomicity-mode");
        CHECK(qhref(cs, "schema-1878") == 1878, "hash schema-1878");
        CHECK(qhref(cs, "weak-atomicity-used") == 0, "hash weak=0");
    }

    // ── AC3: strong commit path ──
    {
        std::println("\n--- AC3: strong atomicity commit ---");
        reset_all();
        CompilerService cs;
        CHECK(setup_ws(cs), "workspace");
        auto& ev = cs.evaluator();
        const auto strong0 = ev.atomic_batch_strong_atomicity_commits_total();
        const auto weak0 = ev.atomic_batch_weak_atomicity_used_total();
        const auto inter0 = ev.atomic_batch_interleaved_prevented_total();
        auto r = cs.eval("(mutate:atomic-batch "
                         "(list (list \"mutate:rebind\" \"f\" \"(lambda (x) (* x 3))\")) \"ac3\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "batch succeeds");
        CHECK(ev.atomic_batch_strong_atomicity_commits_total() > strong0, "strong commits bumped");
        CHECK(ev.atomic_batch_weak_atomicity_used_total() == weak0, "weak still 0");
        CHECK(ev.atomic_batch_interleaved_prevented_total() > inter0, "interleaved-prevented");
        CHECK(stats_href(cs, "strong-atomicity-commits") >= 1, "stats strong commits");
        CHECK(stats_href(cs, "weak-atomicity-used") == 0, "stats weak 0");
    }

    // ── AC4: Strict + foreign :tenant-target deny ──
    {
        std::println("\n--- AC4: Strict cross-tenant batch deny ---");
        reset_all();
        CompilerService cs;
        CHECK(setup_ws(cs), "workspace");
        auto& ev = cs.evaluator();
        // Principal tenant 7, Strict sandbox, no cross-tenant grant.
        ev.set_tenant_principal(7, "bob");
        ev.set_effect_sandbox_mode(2);
        // Grant wildcard so capability effect model allows Mutate, but
        // isolation still blocks foreign tenant-target.
        ev.grant_capability(kCapWildcard);
        const auto den0 = ev.atomic_batch_tenant_isolation_denials_total();
        auto r = cs.eval("(mutate:atomic-batch "
                         "(list (list \"mutate:rebind\" \"f\" \"(lambda (x) (* x 9))\")) "
                         "\"ac4\" :tenant-target 8)");
        // Should fail (error pair or non-#t).
        const bool denied = !r || is_error(*r) || (is_bool(*r) && !as_bool(*r)) || !is_bool(*r);
        CHECK(denied, "foreign :tenant-target denied under Strict");
        CHECK(ev.atomic_batch_tenant_isolation_denials_total() > den0,
              "tenant-isolation-denials bumped");
        CHECK(stats_href(cs, "tenant-isolation-denials") >= 1, "stats denials");
        std::println("  denials={}", ev.atomic_batch_tenant_isolation_denials_total());

        // Same tenant target allowed under Strict.
        auto ok = cs.eval("(mutate:atomic-batch "
                          "(list (list \"mutate:rebind\" \"f\" \"(lambda (x) (* x 5))\")) "
                          "\"ac4-self\" :tenant-target 7)");
        CHECK(ok && is_bool(*ok) && as_bool(*ok), "same-tenant :tenant-target allowed");
    }

    // ── AC5: unsupported op clear error ──
    {
        std::println("\n--- AC5: unsupported op clear error ---");
        reset_all();
        CompilerService cs;
        CHECK(setup_ws(cs), "workspace");
        const auto u0 = stats_href(cs, "unsupported-op-total");
        auto r = cs.eval("(mutate:atomic-batch "
                         "(list (list \"mutate:not-a-real-op\" \"x\" \"1\")) \"ac5\")");
        const bool failed = !r || !is_bool(*r) || !as_bool(*r);
        CHECK(failed, "unsupported op does not succeed as #t");
        CHECK(stats_href(cs, "unsupported-op-total") > u0 || failed,
              "unsupported metric or hard error");
    }

    // ── AC6: single-tenant Strict no regression ──
    {
        std::println("\n--- AC6: single-tenant Strict batch ok ---");
        reset_all();
        CompilerService cs;
        CHECK(setup_ws(cs), "workspace");
        auto& ev = cs.evaluator();
        ev.grant_capability(kCapWildcard);
        ev.set_tenant_principal(1, "solo");
        ev.set_effect_sandbox_mode(2);
        auto r = cs.eval("(mutate:atomic-batch "
                         "(list (list \"mutate:rebind\" \"g\" \"42\")) \"ac6\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "Strict single-tenant batch succeeds");
        CHECK(ev.atomic_batch_weak_atomicity_used_total() == 0, "still strong (weak=0)");
    }

    reset_all();
    std::println("\n=== test_atomic_batch_tenant_1878: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
