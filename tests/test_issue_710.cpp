// @category: integration
// @reason: Issue #710 verify_tool/diagnostic Guard + StableRef + dirty closed-loop

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_710_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t guard_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:verify-tool-guard-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void seed_sva_workspace(aura::compiler::CompilerService& cs, aura::ast::NodeId& property_id,
                               aura::ast::NodeId& coverpoint_id) {
    cs.eval("(set-code \"(define seed 1)\")");
    cs.eval("(eval-current)");
    auto* ws = cs.workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    if (!ws || !pool)
        return;
    property_id = ws->add_property(pool->intern("p_req"), pool->intern("req ack"));
    const std::vector<aura::ast::SymId> bins{pool->intern("low"), pool->intern("high")};
    coverpoint_id = ws->add_coverpoint(pool->intern("req"), bins);
    const std::vector<aura::ast::NodeId> cps{coverpoint_id};
    (void)ws->add_covergroup(pool->intern("cg_req"), cps);
}

} // namespace aura_issue_710_detail

int main() {
    using namespace aura_issue_710_detail;

    std::println("=== Issue #710: verify_tool Guard + StableRef closed-loop ===");

    aura::compiler::CompilerService cs;

    // AC1: query:verify-tool-guard-stats hash fields
    {
        std::println("\n--- AC1: query:verify-tool-guard-stats ---");
        auto stats = cs.eval("(query:verify-tool-guard-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:verify-tool-guard-stats returns hash");
        CHECK(guard_stat(cs, "guard-captures") >= 0, "guard-captures present");
        CHECK(guard_stat(cs, "dirty-propagation") >= 0, "dirty-propagation present");
        CHECK(guard_stat(cs, "stable-ref-hits") >= 0, "stable-ref-hits present");
        CHECK(guard_stat(cs, "feedback-mutate-success") >= 0, "feedback-mutate-success present");
    }

    aura::ast::NodeId property_id = aura::ast::NULL_NODE;
    aura::ast::NodeId coverpoint_id = aura::ast::NULL_NODE;
    seed_sva_workspace(cs, property_id, coverpoint_id);

    const auto guard_before = guard_stat(cs, "guard-captures");
    const auto dirty_before = guard_stat(cs, "dirty-propagation");
    const auto ref_before = guard_stat(cs, "stable-ref-hits");
    const auto feedback_before = guard_stat(cs, "feedback-mutate-success");

    // AC2: verify:parse-coverage under Guard + StableNodeRef
    {
        std::println("\n--- AC2: verify:parse-coverage guard wiring ---");
        auto r = cs.eval(std::format("(verify:parse-coverage \"{} hole_a\n\")",
                                     static_cast<int>(coverpoint_id)));
        CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) >= 1,
              "verify:parse-coverage marks SV coverpoint");
        CHECK(guard_stat(cs, "guard-captures") > guard_before,
              std::format("guard-captures grew ({} -> {})", guard_before,
                          guard_stat(cs, "guard-captures")));
        CHECK(guard_stat(cs, "stable-ref-hits") > ref_before,
              "stable-ref-hits grew after parse-coverage");
        CHECK(guard_stat(cs, "dirty-propagation") > dirty_before,
              "dirty-propagation grew after parse-coverage");
        CHECK(guard_stat(cs, "feedback-mutate-success") > feedback_before,
              "feedback-mutate-success grew after parse-coverage");
    }

    // AC3: eda:run-verification-feedback integrated closed-loop
    {
        std::println("\n--- AC3: eda:run-verification-feedback guard ---");
        const auto fb_before = guard_stat(cs, "feedback-mutate-success");
        auto r = cs.eval(std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole\")",
                                     static_cast<int>(coverpoint_id)));
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "eda:run-verification-feedback succeeds under Guard");
        CHECK(guard_stat(cs, "feedback-mutate-success") > fb_before,
              "feedback-mutate-success grew after eda feedback");
    }

    // AC4: diagnostic check-preconditions StableNodeRef validation
    {
        std::println("\n--- AC4: check-preconditions StableRef ---");
        const auto ref4_before = guard_stat(cs, "stable-ref-hits");
        auto ok = cs.eval(
            std::format("(check-preconditions {} \"Bool\")", static_cast<int>(property_id)));
        CHECK(ok && aura::compiler::types::is_bool(*ok) && aura::compiler::types::as_bool(*ok),
              "check-preconditions passes on valid property node (type compat)");
        CHECK(guard_stat(cs, "stable-ref-hits") > ref4_before,
              "stable-ref-hits grew after check-preconditions");
        auto bad = cs.eval("(check-preconditions 999999 0)");
        CHECK(bad && aura::compiler::types::is_bool(*bad) && !aura::compiler::types::as_bool(*bad),
              "check-preconditions rejects invalid node id");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 211,
              "stats:count == 211");
    }

    // AC6: fiber stress — verify parse + feedback + guard stats
    {
        std::println("\n--- AC6: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval(std::format("(verify:parse-failures \"{} fail\")",
                                          static_cast<int>(property_id)));
                (void)cs.eval("(eda:run-verification-feedback \"coverage.log\" \"0 stress\")");
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r = cs.eval("(query:verify-tool-guard-stats)");
                if (r && aura::compiler::types::is_hash(*r))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() > 0,
              std::format("fiber stress produced {} guard-stats queries", ok_count.load()));
        CHECK(guard_stat(cs, "guard-captures") > guard_before,
              "guard-captures accumulated under fiber stress");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}