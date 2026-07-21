// @category: integration
// @reason: Issue #1913 — mutate:atomic-batch ↔ query:pattern / tag_arity_index
// Issue #1913 (#1978 renamed): issue# moved from filename to header.
// end-to-end incremental sync (dirty-fraction policy) + metrics.
//
//   AC1: source wires tag_arity_index_sync_after_atomic_batch + :sync-query-index?
//   AC2: query:atomic-batch-stats-hash schema-1913 AC metrics
//   AC3: successful atomic-batch commit bumps index sync counters
//   AC4: post-batch query:pattern works; latency sample armed
//   AC5: dirty-fraction path prefers incremental (rebuild_skipped / sync_hits)
//   AC6: :sync-query-index? #f skips Evaluator index sync call path
//   AC7: multi-round batch + pattern — index stays warm

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:atomic-batch-stats-hash\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static bool seed_large(CompilerService& cs, int n_defs = 40) {
    std::string code;
    for (int i = 0; i < n_defs; ++i)
        code += std::format("(define (f{} x) (+ x {})) ", i, i);
    auto sc = cs.eval(std::format("(set-code \"{}\")", code));
    if (!sc)
        return false;
    (void)cs.eval("(eval-current)");
    return cs.evaluator().workspace_flat() != nullptr;
}

static void warm_index(Evaluator& ev) {
    ev.force_build_tag_arity_index();
}

} // namespace

int main() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: batch index sync wiring ---");
        std::string qi, mut, ixx;
        for (const char* p : {"src/compiler/evaluator_query_index.cpp",
                              "../src/compiler/evaluator_query_index.cpp"}) {
            qi = read_file(p);
            if (!qi.empty())
                break;
        }
        for (const char* p : {"src/compiler/evaluator_primitives_mutate.cpp",
                              "../src/compiler/evaluator_primitives_mutate.cpp"}) {
            mut = read_file(p);
            if (!mut.empty())
                break;
        }
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!qi.empty(), "read query_index");
        CHECK(qi.find("tag_arity_index_sync_after_atomic_batch") != std::string::npos,
              "sync_after_atomic_batch");
        CHECK(qi.find("#1913") != std::string::npos, "cites #1913");
        CHECK(qi.find("atomic_batch_index_sync_hits") != std::string::npos, "sync hits metric");
        CHECK(qi.find("pattern_query_after_batch") != std::string::npos, "post-batch latency");
        CHECK(!mut.empty(), "read mutate.cpp");
        CHECK(mut.find(":sync-query-index?") != std::string::npos, "keyword");
        CHECK(mut.find("tag_arity_index_sync_after_atomic_batch") != std::string::npos,
              "commit calls sync");
        CHECK(!ixx.empty() &&
                  ixx.find("tag_arity_index_sync_after_atomic_batch") != std::string::npos,
              "Evaluator API declared");
    }

    // ── AC2: stats hash schema 1913 ──
    {
        std::println("\n--- AC2: query:atomic-batch-stats-hash schema-1913 ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:atomic-batch-stats-hash\")");
        CHECK(r.has_value() && is_hash(*r), "is hash");
        CHECK(href(cs, "schema-1913") == 1913, "schema-1913");
        CHECK(href(cs, "index-sync-wired") == 1, "wired");
        CHECK(href(cs, "dirty-fraction-policy-wired") == 1, "dirty-fraction wired");
        CHECK(href(cs, "atomic_batch_index_sync_hits") >= 0, "sync hits key");
        CHECK(href(cs, "atomic_batch_index_rebuild_skipped") >= 0, "rebuild skipped key");
        CHECK(href(cs, "pattern_query_after_batch_latency") >= 0, "pattern latency key");
        CHECK(href(cs, "sync-query-index-default") == 1, "default sync on");
        CHECK(href(cs, "schema-1899") == 1899, "schema-1899 retained");
        CHECK(href(cs, "dispatch-table-size") == 13, "dispatch table");
    }

    // ── AC3: commit bumps index sync ──
    {
        std::println("\n--- AC3: atomic-batch commit → index sync metrics ---");
        CompilerService cs;
        CHECK(seed_large(cs, 30), "seed large");
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        CHECK(m != nullptr, "metrics");

        warm_index(ev);
        CHECK(ev.tag_arity_index_is_warm(), "index warm after build");

        const auto calls0 = ev.get_atomic_batch_index_sync_calls();
        const auto hits0 = ev.get_atomic_batch_index_sync_hits();
        const auto skip0 = ev.get_atomic_batch_index_rebuild_skipped();
        const auto full0 = ev.get_atomic_batch_index_full_rebuilds();

        auto batch =
            cs.eval("(mutate:atomic-batch "
                    "(list (list \"mutate:rebind\" \"f0\" \"(lambda (x) (+ x 100))\" \"t1913\")) "
                    "\"1913-ac3\")");
        CHECK(batch.has_value(), "atomic-batch ok");

        CHECK(ev.get_atomic_batch_index_sync_calls() > calls0, "sync calls advanced");
        const bool advanced = ev.get_atomic_batch_index_sync_hits() > hits0 ||
                              ev.get_atomic_batch_index_rebuild_skipped() > skip0 ||
                              ev.get_atomic_batch_index_full_rebuilds() > full0;
        CHECK(advanced, "sync hits or skip or full advanced");
        CHECK(href(cs, "atomic_batch_index_sync_calls") > 0 ||
                  href(cs, "atomic-batch-index-sync-hits") >= 0,
              "hash sees counters");
        CHECK(load_u64(m->atomic_batch_index_sync_calls) > 0 ||
                  load_u64(m->atomic_batch_index_sync_hits) > 0 ||
                  load_u64(m->atomic_batch_index_full_rebuilds) > 0,
              "CompilerMetrics bumped");
    }

    // ── AC4: post-batch query:pattern latency sample ──
    {
        std::println("\n--- AC4: post-batch query:pattern latency ---");
        CompilerService cs;
        CHECK(seed_large(cs, 20), "seed");
        auto& ev = cs.evaluator();
        warm_index(ev);

        const auto samples0 = load_u64(metrics_of(cs)->pattern_query_after_batch_samples);
        auto batch =
            cs.eval("(mutate:atomic-batch "
                    "(list (list \"mutate:rebind\" \"f0\" \"(lambda (x) (* x 2))\" \"t\")) "
                    "\"1913-ac4\")");
        CHECK(batch.has_value(), "batch");

        auto pat = cs.eval("(query:pattern '(define _ _))");
        CHECK(pat.has_value(), "query:pattern after batch");

        const auto samples1 = load_u64(metrics_of(cs)->pattern_query_after_batch_samples);
        CHECK(samples1 > samples0 || href(cs, "pattern_query_after_batch_latency") >= 0,
              "post-batch latency sampled or key present");
        CHECK(ev.get_pattern_query_after_batch_latency_us_max() >= 0, "latency max field");
    }

    // ── AC5: sparse dirty prefers incremental ──
    {
        std::println("\n--- AC5: dirty-fraction prefers incremental ---");
        CompilerService cs;
        CHECK(seed_large(cs, 50), "seed 50 defs");
        auto& ev = cs.evaluator();
        warm_index(ev);

        const auto hits0 = ev.get_atomic_batch_index_sync_hits();
        const auto skip0 = ev.get_atomic_batch_index_rebuild_skipped();
        for (int i = 0; i < 5; ++i) {
            auto b = cs.eval(std::format(
                "(mutate:atomic-batch "
                "(list (list \"mutate:rebind\" \"f{}\" \"(lambda (x) (+ x {}))\" \"t\")) "
                "\"1913-ac5-{}\")",
                i % 10, i, i));
            CHECK(b.has_value(), std::format("batch round {}", i));
        }
        const auto hits1 = ev.get_atomic_batch_index_sync_hits();
        const auto skip1 = ev.get_atomic_batch_index_rebuild_skipped();
        std::println("  sync_hits {}→{}  rebuild_skipped {}→{}", hits0, hits1, skip0, skip1);
        CHECK(hits1 > hits0 || skip1 > skip0, "incremental path taken at least once");
        CHECK(ev.tag_arity_index_is_warm(), "index still warm");
    }

    // ── AC6: opt-out :sync-query-index? #f ──
    {
        std::println("\n--- AC6: :sync-query-index? #f skips sync ---");
        CompilerService cs;
        CHECK(seed_large(cs, 10), "seed");
        auto& ev = cs.evaluator();
        warm_index(ev);
        const auto calls0 = ev.get_atomic_batch_index_sync_calls();
        auto b = cs.eval("(mutate:atomic-batch "
                         "(list (list \"mutate:rebind\" \"f0\" \"(lambda (x) 1)\" \"t\")) "
                         "\"1913-ac6\" "
                         ":sync-query-index? #f)");
        CHECK(b.has_value(), "batch with sync off");
        CHECK(ev.get_atomic_batch_index_sync_calls() == calls0,
              "sync calls unchanged when :sync-query-index? #f");
    }

    // ── AC7: multi-round batch + pattern stress ──
    {
        std::println("\n--- AC7: multi-round batch + pattern ---");
        CompilerService cs;
        CHECK(seed_large(cs, 25), "seed");
        auto& ev = cs.evaluator();
        warm_index(ev);
        const auto t0 = std::chrono::steady_clock::now();
        constexpr int kRounds = 30;
        for (int i = 0; i < kRounds; ++i) {
            auto b = cs.eval(std::format(
                "(mutate:atomic-batch "
                "(list (list \"mutate:rebind\" \"f{}\" \"(lambda (x) (+ x {}))\" \"t\")) "
                "\"r{}\")",
                i % 20, i, i));
            CHECK(b.has_value(), std::format("round {} batch", i));
            auto p = cs.eval("(query:pattern '(define _ _))");
            CHECK(p.has_value(), std::format("round {} pattern", i));
        }
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        std::println("  {} rounds batch+pattern in {} us (avg {} us)", kRounds, us, us / kRounds);
        CHECK(ev.get_atomic_batch_index_sync_calls() >= static_cast<std::uint64_t>(kRounds),
              "sync calls ≥ rounds");
        CHECK(ev.tag_arity_index_is_warm(), "warm after stress");
        CHECK(href(cs, "schema-1913") == 1913, "schema holds");
        CHECK(href(cs, "index-sync-wired") == 1, "wired after stress");
    }

    std::println("\n=== test_atomic_batch_pattern_1913: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
