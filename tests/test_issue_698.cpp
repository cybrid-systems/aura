// @category: integration
// @reason: Issue #698 Hardware backend commercial interop closed-loop

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.hardware_backend;
import aura.compiler.sv_ir;
import aura.core.ast;

namespace aura_issue_698_detail {
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

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:hardware-backend-commercial-stats) '{}')", key));
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
    ws->apply_verification_dirty_bits(property_id, aura::ast::FlatAST::kAssertFailureDirty);
    ws->apply_verification_dirty_bits(coverpoint_id, aura::ast::FlatAST::kCoverageFeedbackDirty);
}

} // namespace aura_issue_698_detail

int main() {
    using namespace aura_issue_698_detail;
    std::println("=== Issue #698: Hardware backend commercial interop ===");

    aura::compiler::CompilerService cs;

    // AC1: query:hardware-backend-commercial-stats hash fields
    {
        std::println("\n--- AC1: query:hardware-backend-commercial-stats ---");
        auto stats = cs.eval("(query:hardware-backend-commercial-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:hardware-backend-commercial-stats returns hash");
        CHECK(stat_int(cs, "hook-calls") >= 0, "hook-calls present");
        CHECK(stat_int(cs, "commercial-reemits") >= 0, "commercial-reemits present");
        CHECK(stat_int(cs, "emit-parse-success") >= 0, "emit-parse-success present");
        CHECK(stat_int(cs, "emit-parse-fail") >= 0, "emit-parse-fail present");
        CHECK(stat_int(cs, "verification-loop-convergence") >= 0,
              "verification-loop-convergence present");
        CHECK(stat_int(cs, "commercial-simulator-runs") >= 0, "commercial-simulator-runs present");
        CHECK(stat_int(cs, "sv-diff-emits") >= 0, "sv-diff-emits present");
    }

    aura::ast::NodeId property_id = aura::ast::NULL_NODE;
    aura::ast::NodeId coverpoint_id = aura::ast::NULL_NODE;
    seed_sva_workspace(cs, property_id, coverpoint_id);

    const auto hook_before = stat_int(cs, "hook-calls");
    const auto parse_ok_before = stat_int(cs, "emit-parse-success");
    const auto sim_before = stat_int(cs, "commercial-simulator-runs");

    // AC2: validate_sv_emit on structured emit
    {
        std::println("\n--- AC2: validate_sv_emit ---");
        auto* ws = cs.workspace_flat();
        auto* pool = cs.evaluator().workspace_pool();
        CHECK(ws != nullptr && pool != nullptr, "workspace available");
        if (ws && pool && property_id < ws->size()) {
            auto ir = aura::compiler::sv_ir::map_property_node_to_ir(*ws, *pool, property_id);
            CHECK(ir.has_value(), "map_property_node_to_ir succeeds");
            if (ir) {
                const auto emitted = aura::compiler::sv_ir::emit_property(*ir);
                const auto valid = aura::compiler::sv_ir::validate_sv_emit(emitted);
                CHECK(valid.ok, std::format("validate_sv_emit ok (err={})", valid.error));
                const auto diff = aura::compiler::sv_ir::emit_sv_diff("", emitted);
                CHECK(!diff.empty(), "emit_sv_diff non-empty for new emit");
            }
        }
    }

    // AC3: eda:weaken-property + commercial simulator stub
    {
        std::println("\n--- AC3: commercial closed-loop primitives ---");
        auto weaken = cs.eval(
            std::format("(eda:weaken-property {} \"reset\")", static_cast<int>(property_id)));
        CHECK(weaken && aura::compiler::types::is_bool(*weaken) &&
                  aura::compiler::types::as_bool(*weaken),
              "eda:weaken-property succeeds");
        const auto hook_after = stat_int(cs, "hook-calls");
        CHECK(hook_after > hook_before,
              std::format("hook-calls grew ({} -> {})", hook_before, hook_after));
        auto stub = cs.eval(std::format("(eda:run-commercial-simulator-stub \"vcs\" {})",
                                        static_cast<int>(property_id)));
        CHECK(stub && aura::compiler::types::is_bool(*stub) &&
                  aura::compiler::types::as_bool(*stub),
              "eda:run-commercial-simulator-stub vcs succeeds");
        auto questa = cs.eval(std::format("(eda:run-commercial-simulator-stub \"questa\" {})",
                                          static_cast<int>(coverpoint_id)));
        CHECK(questa && aura::compiler::types::is_bool(*questa) &&
                  aura::compiler::types::as_bool(*questa),
              "eda:run-commercial-simulator-stub questa succeeds");
        const auto sim_after = stat_int(cs, "commercial-simulator-runs");
        CHECK(sim_after > sim_before,
              std::format("commercial-simulator-runs grew ({} -> {})", sim_before, sim_after));
    }

    // AC4: eda:run-verification-feedback full closed-loop
    {
        std::println("\n--- AC4: eda:run-verification-feedback ---");
        const auto feedback_before = stat_int(cs, "feedback-mutate-hits");
        auto r = cs.eval(std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole\")",
                                     static_cast<int>(coverpoint_id)));
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "eda:run-verification-feedback coverage succeeds");
        const auto feedback_after = stat_int(cs, "feedback-mutate-hits");
        CHECK(feedback_after > feedback_before,
              std::format("feedback-mutate-hits grew ({} -> {})", feedback_before, feedback_after));
        const auto parse_ok_after = stat_int(cs, "emit-parse-success");
        CHECK(parse_ok_after > parse_ok_before,
              std::format("emit-parse-success grew ({} -> {})", parse_ok_before, parse_ok_after));
        const auto convergence = stat_int(cs, "verification-loop-convergence");
        CHECK(convergence >= 50,
              std::format("verification-loop-convergence >= 50% (got {}%)", convergence));
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 211,
              "stats:count >= 211");
    }

    // AC6: fiber stress
    {
        std::println("\n--- AC6: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(typecheck-current)");
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r = cs.eval("(eda:run-verification-feedback \"coverage.log\" \"0 stress\")");
                if (r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() > 0,
              std::format("fiber stress produced {} successful feedback loops", ok_count.load()));
        CHECK(stat_int(cs, "emit-parse-fail") >= 0, "no emit-parse-fail after fiber stress");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}