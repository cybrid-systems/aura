// @category: integration
// @reason: Agent Decision Metrics contract liveness + agent reaction (#1461)
//
// test_issue_1461.cpp — Issue #1461:
// Define & enforce minimal Agent Decision Metrics contract
// (back-off / escalate / commit).
//
// ACs:
//   AC1: docs/design/agent-decision-metrics.md names + semantics
//   AC2: counters live on panic / rollback / long-hold / atomic-batch paths
//   AC3: (agent:decision-metrics) → schema 1461 hash
//   AC4: std/agent closed-loop consults the contract (presence + decide)
//   AC5: inject panic / forced rollback → metrics non-zero + agent reacts
//   AC6: no bare demoted query:*-stats dependence (facade / contract only)

#include "test_harness.hpp"
#include "observability_metrics.h"

#include <chrono>
#include <fstream>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

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

namespace test_issue_1461_detail {

#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::println("  FAIL: {} (line {})", msg, __LINE__);                                   \
            ++g_failed;                                                                            \
        } else {                                                                                   \
            std::println("  PASS: {}", msg);                                                       \
            ++g_passed;                                                                            \
        }                                                                                          \
    } while (0)

bool file_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}

std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f)
        return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::int64_t href_metrics(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format("(hash-ref (agent:decision-metrics) \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

bool metrics_rec_is(CompilerService& cs, const char* expected) {
    auto r = cs.eval(std::format(
        "(equal? (hash-ref (agent:decision-metrics) \"recommendation\") \"{}\")", expected));
    return r && is_bool(*r) && as_bool(*r);
}

bool eval_bool(CompilerService& cs, const char* expr) {
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

void spin_us(std::int64_t min_us) {
    auto t0 = std::chrono::steady_clock::now();
    while (
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count() < min_us) {
    }
}

void ac1_doc() {
    std::println("\n--- AC1: design doc metric names + semantics ---");
    CHECK(file_exists("docs/design/agent-decision-metrics.md"), "agent-decision-metrics.md");
    auto doc = read_file("docs/design/agent-decision-metrics.md");
    CHECK(doc.find("1461") != std::string::npos, "schema 1461");
    CHECK(doc.find("recommendation") != std::string::npos, "recommendation");
    CHECK(doc.find("panic-count") != std::string::npos, "panic-count");
    CHECK(doc.find("rollback-rate") != std::string::npos, "rollback-rate");
    CHECK(doc.find("hold-time-us") != std::string::npos, "hold-time-us");
    CHECK(doc.find("long-hold-count") != std::string::npos, "long-hold-count");
    CHECK(doc.find("atomic-batch-commits") != std::string::npos, "atomic-batch-commits");
    CHECK(doc.find("atomic-batch-rollbacks") != std::string::npos, "atomic-batch-rollbacks");
    CHECK(doc.find("back-off") != std::string::npos || doc.find("back-off") != std::string::npos,
          "back-off decision");
    CHECK(doc.find("escalate") != std::string::npos, "escalate decision");
    CHECK(doc.find("commit") != std::string::npos, "commit decision");
    // Liveness section (this issue)
    CHECK(doc.find("Liveness") != std::string::npos || doc.find("liveness") != std::string::npos ||
              doc.find("bumped") != std::string::npos,
          "liveness / bump semantics documented");
}

void ac3_entry_point() {
    std::println("\n--- AC3: (agent:decision-metrics) stable entry ---");
    CompilerService cs;
    CHECK(cs.eval("(require \"std/agent\" all:)").has_value(), "require std/agent");
    auto m = cs.eval("(agent:decision-metrics)");
    CHECK(m.has_value() && is_hash(*m), "returns hash");
    CHECK(href_metrics(cs, "schema") == 1461, "schema == 1461");
    CHECK(metrics_rec_is(cs, "commit") || metrics_rec_is(cs, "back-off") ||
              metrics_rec_is(cs, "escalate"),
          "recommendation is commit|back-off|escalate");
    CHECK(href_metrics(cs, "panic-count") >= 0, "panic-count int");
    CHECK(href_metrics(cs, "rollback-rate") >= 0, "rollback-rate int");
    CHECK(href_metrics(cs, "hold-time-us") >= 0, "hold-time-us int");
    CHECK(href_metrics(cs, "long-hold-count") >= 0, "long-hold-count int");
    CHECK(href_metrics(cs, "atomic-batch-commits") >= 0, "atomic-batch-commits int");
    CHECK(href_metrics(cs, "atomic-batch-rollbacks") >= 0, "atomic-batch-rollbacks int");
    // Via stats facade only — not inventing demoted bare *-stats
    auto via = cs.eval("(stats:get \"query:fiber-boundary-violation-stats\")");
    CHECK(via.has_value() && is_hash(*via), "panic source via stats:get facade");
}

void ac2_atomic_batch_and_hold_live() {
    std::println("\n--- AC2: atomic-batch + hold-time liveness ---");
    CompilerService cs;
    CHECK(cs.eval("(require \"std/agent\" all:)").has_value(), "require");
    const auto c0 = href_metrics(cs, "atomic-batch-commits");
    const auto r0 = href_metrics(cs, "atomic-batch-rollbacks");
    const auto h0 = href_metrics(cs, "hold-time-us");

    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto ok = cs.eval("(mutate:atomic-batch (list (list \"mutate:rebind\" \"f\" "
                      "\"(lambda (x) (* x 2))\" \"ok\")) \"t-ok\")");
    CHECK(ok.has_value(), "atomic-batch success callable");
    cs.eval("(eval-current)");

    const auto c1 = href_metrics(cs, "atomic-batch-commits");
    const auto h1 = href_metrics(cs, "hold-time-us");
    CHECK(c1 > c0, std::format("atomic-batch-commits {} → {} (bumped)", c0, c1));
    CHECK(h1 >= h0, std::format("hold-time-us {} → {} (non-decreasing)", h0, h1));

    // Force batch failure → rollbacks live
    cs.eval("(mutate:atomic-batch (list (list \"mutate:no-such-op\" \"x\")) \"t-bad\")");
    const auto r1 = href_metrics(cs, "atomic-batch-rollbacks");
    CHECK(r1 > r0, std::format("atomic-batch-rollbacks {} → {} (bumped)", r0, r1));
    const auto rate = href_metrics(cs, "rollback-rate");
    CHECK(rate > 0, std::format("rollback-rate > 0 after batch failure (got {})", rate));
}

void ac2_guard_rollback_and_long_hold() {
    std::println("\n--- AC2: Guard rollback bump + long-hold ---");
    CompilerService cs;
    CHECK(cs.eval("(require \"std/agent\" all:)").has_value(), "require");

    // Failed outermost Guard → mutation_boundary_rollbacks_total (+ fiber stats).
    // Guard ctor optimistically sets *flag_ = true; clear after enter.
    {
        bool ok = true;
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        ok = false; // force failure at dtor
        spin_us(50);
    }
    auto fiber_rb =
        cs.eval("(hash-ref (stats:get \"query:fiber-boundary-violation-stats\") \"rollbacks\")");
    CHECK(fiber_rb.has_value() && is_int(*fiber_rb) && as_int(*fiber_rb) >= 1,
          "fiber rollbacks bumped after failed Guard");

    // Long hold (>1ms) → holds-over-1ms
    {
        bool ok = true;
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        spin_us(1500);
    }
    auto over =
        cs.eval("(hash-ref (stats:get \"query:mutation-boundary-hold-stats\") \"holds-over-1ms\")");
    CHECK(over.has_value() && is_int(*over) && as_int(*over) >= 1,
          "long-hold holds-over-1ms bumped after 1.5ms hold");
    CHECK(href_metrics(cs, "long-hold-count") >= 1, "agent long-hold-count reflects hold-stats");
}

void ac5_inject_panic_agent_escalate() {
    std::println("\n--- AC5: inject recovery-failure → escalate ---");
    CompilerService cs;
    CHECK(cs.eval("(require \"std/agent\" all:)").has_value(), "require");
    CHECK(href_metrics(cs, "panic-count") == 0, "panic-count 0 before inject");
    CHECK(metrics_rec_is(cs, "commit"), "recommendation commit before");

    // Production bump helper (same as fiber resume recovery path uses)
    cs.evaluator().bump_mutation_boundary_recovery_failure();

    CHECK(href_metrics(cs, "panic-count") >= 1, "panic-count non-zero after inject");
    CHECK(metrics_rec_is(cs, "escalate"), "recommendation escalate");
    CHECK(eval_bool(cs, "(eq? (agent:decide) 'escalate)"), "agent:decide is escalate");

    // auto-grow must not blind-retry LLM; safety gate returns escalate
    CHECK(eval_bool(cs, "(eq? (auto-grow \"again\" :source \"(define (y) 1)\" "
                        ":rebind \"y\" \"(lambda () 2)\" :max-tries 1) 'escalate)"),
          "auto-grow returns 'escalate");
}

void ac5_forced_batch_rollback_backoff() {
    std::println("\n--- AC5: forced atomic-batch rollbacks → back-off ---");
    CompilerService cs;
    CHECK(cs.eval("(require \"std/agent\" all:)").has_value(), "require");
    // Need a workspace so the batch path runs (not early-void).
    cs.eval("(set-code \"(define (z x) x)\")");
    cs.eval("(eval-current)");
    // Pure failures → batch rate 100% → back-off (threshold 50)
    for (int i = 0; i < 2; ++i) {
        cs.eval("(mutate:atomic-batch (list (list \"mutate:no-such-op\" \"x\")) \"fail\")");
    }
    const auto rate = href_metrics(cs, "rollback-rate");
    const auto ab_rb = href_metrics(cs, "atomic-batch-rollbacks");
    CHECK(ab_rb >= 2, std::format("atomic-batch-rollbacks >= 2 (got {})", ab_rb));
    CHECK(rate >= 50, std::format("rollback-rate >= 50 (got {})", rate));
    CHECK(metrics_rec_is(cs, "back-off") || metrics_rec_is(cs, "escalate"),
          "recommendation back-off (or escalate if panic also set)");

    // If no panic injected, should be back-off specifically
    if (href_metrics(cs, "panic-count") == 0) {
        CHECK(metrics_rec_is(cs, "back-off"), "exactly back-off");
        CHECK(eval_bool(cs, "(eq? (agent:decide) 'back-off)"), "agent:decide is back-off");
    }
}

void ac4_closed_loop_consults() {
    std::println("\n--- AC4: std/agent closed-loop consults contract ---");
    auto agent = read_file("lib/std/agent.aura");
    CHECK(agent.find("agent:decision-metrics") != std::string::npos, "agent.aura defines metrics");
    CHECK(agent.find("agent-closed-loop-core") != std::string::npos ||
              agent.find("closed-loop") != std::string::npos,
          "closed-loop present");
    // Post-eval metrics call sites
    CHECK(agent.find("post-metrics") != std::string::npos ||
              agent.find("post_metrics") != std::string::npos ||
              (agent.find("agent:decision-metrics") != std::string::npos &&
               agent.find("recommendation") != std::string::npos),
          "metrics consulted around commit");

    CompilerService cs;
    CHECK(cs.eval("(require \"std/agent\" all:)").has_value(), "require");
    CHECK(cs.eval("(agent:loop-stats-reset!)").has_value(), "reset");
    cs.eval("(agent:closed-loop-once :source \"(define (f x) (+ x 1))\" "
            ":rebind \"f\" \"(lambda (x) (* x 2))\" :summary \"ac4\")");
    auto mc = cs.eval("(hash-ref (agent:loop-stats) \"metrics-calls\")");
    CHECK(mc.has_value() && is_int(*mc) && as_int(*mc) > 0,
          "closed-loop-once bumped metrics-calls");
}

void ac6_no_demoted_bare_stats() {
    std::println("\n--- AC6: no bare demoted query:*-stats dependence ---");
    auto agent = read_file("lib/std/agent.aura");
    // Contract uses stats:get / atomic-batch:stats / mutation-log:summary —
    // not hundreds of demoted bare query:foo-stats public names as the API.
    CHECK(agent.find("stats:get") != std::string::npos, "uses stats:get facade");
    CHECK(agent.find("atomic-batch:stats") != std::string::npos, "uses atomic-batch:stats");
    CHECK(agent.find("mutation-log:summary") != std::string::npos, "uses mutation-log:summary");
    // Must not call bare demoted names as primary API
    CHECK(agent.find("(query:siblings") == std::string::npos, "no demoted query:siblings");
    CHECK(agent.find("(query:find-by-name") == std::string::npos, "no demoted find-by-name");
}

} // namespace test_issue_1461_detail

int main() {
    using namespace test_issue_1461_detail;
    std::println("=== Issue #1461 — Agent Decision Metrics contract ===");
    ac1_doc();
    ac3_entry_point();
    ac2_atomic_batch_and_hold_live();
    ac2_guard_rollback_and_long_hold();
    ac5_inject_panic_agent_escalate();
    ac5_forced_batch_rollback_backoff();
    ac4_closed_loop_consults();
    ac6_no_demoted_bare_stats();
    std::println("\n─── #1461 summary: {}/{} passed, {} failed ───", g_passed, g_passed + g_failed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}
