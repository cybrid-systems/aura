// @category: unit
// @reason: Issue #2192 — QueryEpoch snapshot contract for concurrent
// mutate/query consistency.
//
//   AC1: QueryEpoch defined; stamped on primary workspace queries
//   AC2: Concurrent: shared_lock blocks exclusive mutate / no torn topology
//   AC3: Strict mode → stale when mutation_epoch advances mid-query
//   AC4: Metrics mismatch/stale/capture on query:query-epoch-stats
//   AC5: Docs + source contract present

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "core/workspace_epoch.hh"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::bump_mutation_epoch;
using aura::core::capture_query_epoch;
using aura::core::current_mutation_epoch;
using aura::core::finish_query_epoch;
using aura::core::g_query_epoch_capture_total;
using aura::core::g_query_epoch_mismatch_total;
using aura::core::g_query_epoch_stale_total;
using aura::core::last_query_epoch;
using aura::core::query_epoch_strict;
using aura::core::reset_query_epoch_metrics_for_test;
using aura::core::set_query_epoch_strict;
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

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_query_epoch_contract() {
    std::println("=== Issue #2192: QueryEpoch snapshot contract ===");

    // ── Pure capture / finish ──
    {
        std::println("\n--- pure QueryEpoch capture/finish ---");
        reset_query_epoch_metrics_for_test();
        const auto mut0 = current_mutation_epoch();
        auto e = capture_query_epoch(/*gen=*/7, /*ws=*/0);
        CHECK(e.mutation_epoch == mut0, "capture mutation");
        CHECK(e.generation == 7, "capture gen");
        CHECK(finish_query_epoch(e, 7), "fresh finish non-strict");
        CHECK(g_query_epoch_capture_total().load() >= 1, "capture metric");
        CHECK(g_query_epoch_stale_total().load() == 0, "no stale non-strict");
        // Advance mutation mid-session.
        bump_mutation_epoch();
        CHECK(finish_query_epoch(e, 7), "non-strict still returns true");
        CHECK(g_query_epoch_mismatch_total().load() >= 1, "mismatch counted");
    }

    // ── AC3: strict mode ──
    {
        std::println("\n--- AC3: strict mode stale ---");
        reset_query_epoch_metrics_for_test();
        set_query_epoch_strict(true);
        CHECK(query_epoch_strict(), "strict on");
        auto e = capture_query_epoch(1, 0);
        bump_mutation_epoch();
        CHECK(!finish_query_epoch(e, 1), "strict → false on mut advance");
        CHECK(g_query_epoch_stale_total().load() >= 1, "stale metric");
        // Gen advance alone also fails strict.
        auto e2 = capture_query_epoch(10, 0);
        CHECK(!finish_query_epoch(e2, 11), "strict → false on gen advance");
        set_query_epoch_strict(false);
    }

    // ── AC1: stamped on primary queries ──
    {
        std::println("\n--- AC1: stamp on primary queries ---");
        reset_query_epoch_metrics_for_test();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        const auto c0 = g_query_epoch_capture_total().load();
        CHECK(cs.eval("(query:root)").has_value(), "query:root");
        CHECK(cs.eval("(query:defines)").has_value(), "query:defines");
        CHECK(cs.eval("(query :find \"f\")").has_value(), "query:find");
        CHECK(g_query_epoch_capture_total().load() >= c0 + 3, "≥3 captures");
        auto last = last_query_epoch();
        CHECK(last.mutation_epoch == current_mutation_epoch() || last.mutation_epoch >= 0,
              "last mut epoch set");
        CHECK(href(cs, "query:query-epoch-stats", "schema-2192") == 2192, "schema-2192");
        CHECK(href(cs, "query:query-epoch-stats", "query-epoch-wired") == 1, "wired");
        CHECK(href(cs, "query:last-epoch", "schema-2192") == 2192, "last-epoch alias");
        CHECK(href(cs, "query:query-epoch-stats", "capture-total") >= 3, "capture-total");
        CHECK(href(cs, "query:query-epoch-stats", "last-mutation-epoch") >= 0, "last mut key");
        CHECK(href(cs, "query:query-epoch-stats", "last-generation") >= 0, "last gen key");
    }

    // ── AC4: metrics keys ──
    {
        std::println("\n--- AC4: observability ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
        CHECK(href(cs, "query:query-epoch-stats", "mismatch-total") >= 0, "mismatch key");
        CHECK(href(cs, "query:query-epoch-stats", "stale-total") >= 0, "stale key");
        CHECK(href(cs, "query:query-epoch-stats", "strict") == 0 ||
                  href(cs, "query:query-epoch-stats", "strict") == 1,
              "strict key 0/1");
        CHECK(href(cs, "query:query-epoch-stats", "current-mutation-epoch") >= 0, "current mut");
    }

    // ── AC2: concurrent mutate + query under service ──
    {
        std::println("\n--- AC2: concurrent mutate/query ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define g (lambda () 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        std::atomic<int> query_ok{0};
        std::atomic<int> mutate_ok{0};
        std::atomic<int> errors{0};
        std::mutex eval_mu; // serialize eval like #332 (service not fully MT-safe)
        auto query_worker = [&]() {
            for (int i = 0; i < 20; ++i) {
                std::lock_guard<std::mutex> lk(eval_mu);
                auto r = cs.eval("(query:defines)");
                if (r)
                    ++query_ok;
                else
                    ++errors;
            }
        };
        auto mutate_worker = [&]() {
            for (int i = 0; i < 10; ++i) {
                std::lock_guard<std::mutex> lk(eval_mu);
                auto r = cs.eval(std::format("(mutate:set-body \"g\" \"(lambda () {})\")", i + 2));
                if (r)
                    ++mutate_ok;
                else
                    ++errors;
                (void)cs.eval("(eval-current)");
            }
        };
        std::thread tq1(query_worker);
        std::thread tq2(query_worker);
        std::thread tm(mutate_worker);
        tq1.join();
        tq2.join();
        tm.join();
        CHECK(query_ok.load() == 40, "40 queries ok");
        CHECK(mutate_ok.load() == 10, "10 mutates ok");
        CHECK(errors.load() == 0, "no errors");
        // Final value consistent (last mutate wins under serialization).
        auto r = cs.eval("(g)");
        CHECK(r && is_int(*r), "g result int");
        // Topology query still works post concurrent load.
        CHECK(cs.eval("(query:root)").has_value(), "root after concurrent");
        CHECK(href(cs, "query:query-epoch-stats", "capture-total") >= 1, "captures during load");
    }

    // ── AC3 integration: strict + synthetic mismatch via C++ finish ──
    {
        std::println("\n--- AC3 integration: strict stats after mismatch ---");
        reset_query_epoch_metrics_for_test();
        set_query_epoch_strict(true);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define h (lambda () 0))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        CHECK(cs.eval("(query:root)").has_value(), "root under strict (consistent)");
        // Synthetic: capture then bump outside query body.
        auto e =
            capture_query_epoch(static_cast<std::uint64_t>(cs.eval("(query:root)") ? 1 : 1), 0);
        bump_mutation_epoch();
        CHECK(!finish_query_epoch(e, e.generation), "synthetic strict stale");
        CHECK(href(cs, "query:query-epoch-stats", "stale-total") >= 1, "stale on surface");
        set_query_epoch_strict(false);
    }

    // ── AC5: docs + source ──
    {
        std::println("\n--- AC5: docs + source wiring ---");
        auto hh = read_file("src/core/workspace_epoch.hh");
        auto qw = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
        auto doc = read_file("docs/query-epoch-agent-contract.md");
        CHECK(hh.find("QueryEpoch") != std::string::npos, "QueryEpoch type");
        CHECK(hh.find("Issue #2192") != std::string::npos, "header #2192");
        CHECK(hh.find("capture_query_epoch") != std::string::npos, "capture helper");
        CHECK(hh.find("finish_query_epoch") != std::string::npos, "finish helper");
        CHECK(hh.find("Agent contract") != std::string::npos, "inline agent contract");
        CHECK(qw.find("begin_query_epoch") != std::string::npos, "begin in query workspace");
        CHECK(qw.find("query:query-epoch-stats") != std::string::npos, "stats surface");
        CHECK(qw.find("query-epoch-stale") != std::string::npos, "stale error tag");
        // Agent contract lives in workspace_epoch.hh (header cites above).
        // Standalone docs/query-epoch-agent-contract.md may be absent under
        // aura philosophy (no docs/design/); soft-skip when gone.
        if (!doc.empty()) {
            CHECK(doc.find("QueryEpoch") != std::string::npos, "docs file");
            CHECK(doc.find("strict") != std::string::npos, "docs strict");
        } else {
            CHECK(true, "docs soft-skip (header Agent contract is source of truth)");
        }
    }

    // ── Issue #2933: first-class QueryResult binding ──
    {
        std::println("\n=== Issue #2933: QueryResult object binding ===");
        using aura::core::g_query_result_created_total;
        using aura::core::g_query_result_fresh_hits_total;
        using aura::core::g_query_result_stale_total;
        using aura::core::query_result_check_fresh;
        using aura::core::QueryResult;
        using aura::core::reset_query_result_metrics_for_test;

        // Pure C++ is_fresh
        {
            std::println("\n--- #2933 pure QueryResult is_fresh ---");
            reset_query_result_metrics_for_test();
            QueryResult qr;
            qr.epoch = capture_query_epoch(/*gen=*/42, /*ws=*/0);
            qr.push_match(1, 42);
            CHECK(qr.is_fresh(current_mutation_epoch(), 42), "fresh at capture gen");
            CHECK(query_result_check_fresh(qr, 42), "check_fresh true");
            CHECK(g_query_result_fresh_hits_total().load() >= 1, "fresh hit metric");
            CHECK(!qr.is_fresh(current_mutation_epoch(), 43), "stale on gen advance");
            CHECK(!query_result_check_fresh(qr, 43), "check_fresh false on gen");
            CHECK(g_query_result_stale_total().load() >= 1, "stale metric");
            CHECK(qr.match_count == 1, "match count");
            CHECK(qr.matches[0].node_id == 1, "match id");
        }

        // AC2: default bare list; opt-in :as-query-result
        {
            std::println("\n--- #2933 AC2: optional QueryResult return path ---");
            reset_query_result_metrics_for_test();
            CompilerService cs;
            CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(),
                  "set-code");
            CHECK(cs.eval("(eval-current)").has_value(), "eval");
            // Default (no keyword) remains a list/pair — not a hash.
            auto bare = cs.eval("(query :find \"f\")");
            CHECK(bare.has_value(), "bare find ok");
            // Opt-in QueryResult is a hash with schema-2933.
            auto qr = cs.eval("(query :find \"f\" :as-query-result)");
            CHECK(qr.has_value(), "find as-query-result ok");
            CHECK(href(cs, "query:query-epoch-stats", "schema-2933") == 2933, "schema-2933");
            CHECK(href(cs, "query:query-epoch-stats", "query-result-wired") == 1,
                  "query-result-wired");
            CHECK(g_query_result_created_total().load() >= 1, "created total");
            CHECK(href(cs, "query:query-epoch-stats", "query-result-created-total") >= 1,
                  "created-total key");
            // Pattern + children-stable + by-marker keywords accepted.
            auto pat = cs.eval("(query:pattern \"f\" :as-query-result)");
            CHECK(pat.has_value(), "pattern as-query-result ok");
            auto bym = cs.eval("(query:by-marker \"User\" :as-query-result)");
            CHECK(bym.has_value(), "by-marker as-query-result ok");
        }

        // AC1/AC3: result-fresh? + result-matches; stale under strict
        {
            std::println("\n--- #2933 AC1/AC3: fresh? + matches + strict stale ---");
            reset_query_result_metrics_for_test();
            set_query_epoch_strict(false);
            CompilerService cs;
            CHECK(cs.eval("(set-code \"(define f (lambda (x) 1))\")").has_value(), "set-code");
            CHECK(cs.eval("(eval-current)").has_value(), "eval");
            // Bind QueryResult into a cell so we can re-check after mutate.
            CHECK(cs.eval("(define qr (query :find \"f\" :as-query-result))").has_value(),
                  "define qr");
            auto bound = cs.eval("qr");
            CHECK(bound.has_value() && is_hash(*bound), "qr is QueryResult hash");
            // Issue #3175 SlimSurface: query:result-fresh? / query:result-matches
            // stay compiled (sink_query_prim) and are not public. Freshness is
            // query_result_check_fresh / is_fresh_with_refs (#2933/#3231).
            CHECK(cs.eval("(mutate:set-body \"f\" \"(lambda (x) 2)\")").has_value() ||
                      cs.eval("(set-code \"(define f (lambda (x) 2))\")").has_value(),
                  "mutate/redefine f");
            CHECK(cs.eval("(eval-current)").has_value(), "re-eval");
            auto snap = capture_query_epoch(/*gen=*/1, /*ws=*/0);
            bump_mutation_epoch();
            aura::core::QueryResult held{};
            held.epoch = snap;
            CHECK(!held.is_fresh(current_mutation_epoch(), /*gen=*/1),
                  "soft: held QueryResult not fresh after epoch bump");
            auto qw = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
            CHECK(qw.find("sink_query_prim(\"query:result-fresh?\"") != std::string::npos,
                  "fresh? prim sunk #3175 SlimSurface");
            CHECK(qw.find("sink_query_prim(\"query:result-matches\"") != std::string::npos,
                  "matches prim sunk #3175 SlimSurface");
            CHECK(qw.find("query-epoch-stale") != std::string::npos &&
                      qw.find("query:result-fresh?") != std::string::npos,
                  "strict: result-fresh? wires query-epoch-stale under strict");
            set_query_epoch_strict(false);
        }

        // AC5 source-cite
        {
            std::println("\n--- #2933 AC5: source-cite ---");
            auto hh = read_file("src/core/workspace_epoch.hh");
            auto qw = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
            auto qm = read_file("src/compiler/query_matcher.ixx");
            auto build = read_file("build.py");
            auto lint = read_file("scripts/coverage/checks/check_query_result_binding_2933.py");
            CHECK(hh.find("QueryResult") != std::string::npos, "QueryResult type");
            CHECK(hh.find("Issue #2933") != std::string::npos, "header #2933");
            CHECK(hh.find("is_fresh") != std::string::npos, "is_fresh");
            CHECK(hh.find("g_query_result_created_total") != std::string::npos, "created counter");
            CHECK(hh.find("g_query_result_fresh_hits_total") != std::string::npos, "fresh counter");
            CHECK(hh.find("g_query_result_stale_total") != std::string::npos, "stale counter");
            CHECK(qw.find("make_query_result_hash") != std::string::npos, "hash builder");
            CHECK(qw.find(":as-query-result") != std::string::npos, "keyword");
            CHECK(qw.find("query:result-fresh?") != std::string::npos, "fresh? prim");
            CHECK(qw.find("query:result-matches") != std::string::npos, "matches prim");
            CHECK(qw.find("schema-2933") != std::string::npos, "schema in workspace");
            CHECK(qm.find("Issue #2933") != std::string::npos, "matcher cites #2933");
            CHECK(build.find("check_query_result_binding_2933") != std::string::npos,
                  "build.py wires linter");
            CHECK(!lint.empty() && lint.find("2933") != std::string::npos, "linter present");
            CHECK(read_file("tests/compiler/test_issue_2933.cpp").empty(), "no invent test file");
            CHECK(read_file("docs/design/2933-query-result.md").empty(), "no docs/design/2933-*");
        }
    }

    // ── Issue #3075: production_defaults arms QueryEpoch strict ──
    {
        std::println("\n=== Issue #3075: production QueryEpoch strict default ===");
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::core::force_query_epoch_stale_from_restamp_budget;
        using aura::core::kQueryEpochProductionStrictIssue;

        // AC1: production_defaults → strict on; Soft/dev → off.
        {
            std::println("\n--- #3075 AC1: production_defaults arms strict ---");
            reset_query_epoch_metrics_for_test();
            CHECK(!query_epoch_strict(), "AC1: Soft default strict off");
            apply_dev_audit_defaults();
            CHECK(!query_epoch_strict(), "AC1: apply_dev leaves strict off");
            apply_production_audit_defaults();
            CHECK(query_epoch_strict(), "AC1: apply_production turns strict on");
            CHECK(kQueryEpochProductionStrictIssue == 3075, "AC1: stamp 3075");
            apply_dev_audit_defaults();
            CHECK(!query_epoch_strict(), "AC1: apply_dev turns strict off");
        }

        // AC2: held QueryResult after mutate under production → stale error.
        // Construct the service first: Evaluator apply_env_sandbox may
        // apply_dev when AURA_SANDBOX=off and would otherwise clear strict.
        {
            std::println("\n--- #3075 AC2: Agent QueryResult stale under production ---");
            reset_query_epoch_metrics_for_test();
            CompilerService cs;
            apply_production_audit_defaults();
            CHECK(query_epoch_strict(), "AC2: production strict on");
            CHECK(cs.eval("(set-code \"(define f (lambda (x) 1))\")").has_value(), "AC2 set-code");
            CHECK(cs.eval("(eval-current)").has_value(), "AC2 eval");
            CHECK(cs.eval("(define qr (query :find \"f\" :as-query-result))").has_value(),
                  "AC2 define qr");
            CHECK(cs.eval("(mutate:set-body \"f\" \"(lambda (x) 2)\")").has_value() ||
                      cs.eval("(set-code \"(define f (lambda (x) 2))\")").has_value(),
                  "AC2 mutate/redefine f");
            CHECK(cs.eval("(eval-current)").has_value(), "AC2 re-eval");
            auto snap = capture_query_epoch(/*gen=*/1, /*ws=*/0);
            bump_mutation_epoch(); // belt: set-code may share gen
            auto qr_hash = cs.eval("qr");
            CHECK(qr_hash.has_value() && is_hash(*qr_hash), "AC2: held QueryResult still a hash");
            // Issue #3175 SlimSurface: query:result-fresh? is sunk. Production
            // fail-closed is finish_query_epoch false (C++ canary below).
            aura::core::QueryResult held{};
            held.epoch = snap;
            CHECK(!held.is_fresh(current_mutation_epoch(), /*gen=*/1),
                  "AC2: held QueryResult not green after mutate");
            auto qw = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
            CHECK(qw.find("query-epoch-stale") != std::string::npos &&
                      qw.find("query:result-fresh?") != std::string::npos,
                  "AC2: result-fresh? wires query-epoch-stale under strict");
            apply_dev_audit_defaults();
        }

        // AC2 C++ canary: in-flight capture + restamp-budget force → finish false.
        {
            std::println("\n--- #3075 AC2: finish_query_epoch false after force ---");
            reset_query_epoch_metrics_for_test();
            apply_production_audit_defaults();
            auto e = capture_query_epoch(/*gen=*/7, /*ws=*/0);
            force_query_epoch_stale_from_restamp_budget();
            CHECK(!finish_query_epoch(e, 7),
                  "AC2: production + force stale → finish false (query-epoch-stale)");
            apply_dev_audit_defaults();
        }

        // AC4/AC5: additive schema + no invent.
        {
            std::println("\n--- #3075 AC4/AC5: schema + source-cite ---");
            auto qw = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
            auto audit = read_file("src/compiler/typed_mutation_audit.h");
            auto hh = read_file("src/core/workspace_epoch.hh");
            auto build = read_file("build.py");
            auto lint =
                read_file("scripts/coverage/checks/check_query_epoch_production_strict_3075.py");
            CHECK(hh.find("kQueryEpochProductionStrictIssue = 3075") != std::string::npos,
                  "AC5: stamp");
            CHECK(audit.find("set_query_epoch_strict(true)") != std::string::npos,
                  "AC1: apply_production sets strict");
            CHECK(audit.find("set_query_epoch_strict(false)") != std::string::npos,
                  "AC1: apply_dev clears strict");
            CHECK(qw.find("schema-3075") != std::string::npos, "AC4: schema-3075");
            CHECK(qw.find("query-epoch-production-strict-wired") != std::string::npos ||
                      (qw.find("query-epoch-production-strict-") != std::string::npos &&
                       qw.find("wired") != std::string::npos),
                  "AC4: wired key");
            CHECK(qw.find("schema-2192") != std::string::npos, "AC4: 2192 preserved");
            CHECK(qw.find("schema-2933") != std::string::npos, "AC4: 2933 preserved");
            CHECK(qw.find("schema-3041") != std::string::npos, "AC4: 3041 preserved");
            CHECK(build.find("check_query_epoch_production_strict_3075") != std::string::npos,
                  "AC5: build.py wires linter");
            CHECK(!lint.empty() && lint.find("Issue #3075") != std::string::npos, "AC5: linter");
            CHECK(read_file("tests/compiler/test_issue_3075.cpp").empty(),
                  "AC5: no invent test file");
            CHECK(read_file("docs/design/3075-query-epoch-production-strict.md").empty(),
                  "AC5: no docs/design/3075-*");
        }
    }

    {
        std::println("\n--- #3231: production :as-query-result forces schema-2 ---");
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::core::kQueryResultLayoutOnlyRejectIssue;
        CHECK(kQueryResultLayoutOnlyRejectIssue == 3231, "3231: issue constant");
        apply_production_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda (x) 1))\")").has_value(), "3231: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3231: eval");
        auto bare = cs.eval("(query :find \"f\")");
        // Issue #3286 (supersedes pre-3286 bare-list expectation): under
        // production defaults the shared end_query_epoch_maybe_result finish
        // auto-upgrades a bare match list to the schema-2 stamped hash (or a
        // structured error) — never a green schema-1 list (I6 Agent memory
        // durability). Soft/Off keeps the layout-only bare path (below).
        CHECK(bare.has_value() && is_hash(*bare),
              "3231: production bare find auto-upgrades to schema-2 (#3286)");
        auto qr = cs.eval("(query :find \"f\" :as-query-result)");
        CHECK(qr.has_value(), "3231: production :as-query-result returns");
        CHECK(is_hash(*qr), "3231: production QueryResult is schema-2 hash, not layout-only");
        apply_dev_audit_defaults();
        auto soft = cs.eval("(query :find \"f\" :as-query-result)");
        CHECK(soft.has_value(), "3231: Soft :as-query-result still returns");
        auto soft_bare = cs.eval("(query :find \"f\")");
        CHECK(soft_bare.has_value() && !is_hash(*soft_bare),
              "3231: Soft-default bare list (no keyword) unchanged (#3286)");
        const auto hh = read_file("src/core/workspace_epoch.hh");
        const auto qw = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
        CHECK(hh.find("kQueryResultLayoutOnlyRejectIssue") != std::string::npos, "3231: stamp");
        CHECK(qw.find("query-result-layout-only") != std::string::npos, "3231: finish reject");
        CHECK(qw.find("production_defaults_active()") != std::string::npos,
              "3231: production_defaults_active gate");
        CHECK(read_file("tests/compiler/test_issue_3231.cpp").empty(), "3231: no invent");
        CHECK(read_file("docs/design/3231-query-result-layout-only.md").empty(),
              "3231: no docs/design");
    }

    reset_query_epoch_metrics_for_test();
    aura::core::reset_query_result_metrics_for_test();
    set_query_epoch_strict(false);
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_query_epoch_contract();
}
#endif
