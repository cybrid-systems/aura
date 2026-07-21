// @category: integration
// @reason: Issue #696 EDA-SV verification closed-loop production stress

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_eda_sv_stress_detail {
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:eda-sv-closedloop-stress-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static int stress_cycles() {
    if (const char* env = std::getenv("AURA_SV_STRESS_CYCLES")) {
        if (env[0] != '\0') {
            char* end = nullptr;
            const long parsed = std::strtol(env, &end, 10);
            if (end != env && parsed > 0)
                return static_cast<int>(parsed);
        }
    }
    return 100;
}

static void build_soc_hierarchy(aura::ast::FlatAST& flat, aura::ast::StringPool& pool) {
    std::size_t batch = 0;
    while (flat.size() < 5000) {
        for (int clk = 0; clk < 10 && flat.size() < 5000; ++clk) {
            std::vector<aura::ast::NodeId> body;
            for (int mp = 0; mp < 4; ++mp) {
                const std::vector<aura::ast::SymId> ports{pool.intern("sig_a"),
                                                          pool.intern("sig_b")};
                body.push_back(
                    flat.add_modport(pool.intern(std::format("mp_{}_{}", batch, mp)), ports));
            }
            (void)flat.add_interface(pool.intern(std::format("iface_{}_{}", batch, clk)), body);
            const auto prop = flat.add_property(pool.intern(std::format("p_{}_{}", batch, clk)),
                                                pool.intern("req ##1 ack"));
            const std::vector<aura::ast::SymId> bins{pool.intern("b0"), pool.intern("b1")};
            const auto cp =
                flat.add_coverpoint(pool.intern(std::format("v_{}_{}", batch, clk)), bins);
            (void)flat.add_covergroup(pool.intern(std::format("cg_{}_{}", batch, clk)),
                                      std::span<const aura::ast::NodeId>(&cp, 1));
            (void)flat.add_assert(pool.intern(std::format("a_{}_{}", batch, clk)), prop);
        }
        ++batch;
    }
}

static void seed_workspace(aura::compiler::CompilerService& cs) {
    cs.eval("(set-code \"(define soc 1)\")");
    cs.eval("(eval-current)");
    auto* ws = cs.workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    if (ws && pool)
        build_soc_hierarchy(*ws, *pool);
}

} // namespace aura_eda_sv_stress_detail

int aura_issue_eda_sv_verification_closedloop_stress_run() {
    using namespace aura_eda_sv_stress_detail;
    const int k_cycles = stress_cycles();
    std::println("=== Issue #696: EDA-SV verification closed-loop stress ===");
    std::println("  stress cycles: {}", k_cycles);

    aura::compiler::CompilerService cs;
    seed_workspace(cs);

    // AC1: orchestration-metrics wired to EDA-SV stress counters
    {
        std::println("\n--- AC1: orchestration-metrics EDA-SV wiring ---");
        auto orch = cs.eval("(stats:get \"query:orchestration-metrics\")");
        CHECK(orch && aura::compiler::types::is_string(*orch),
              "query:orchestration-metrics returns string");
        if (orch && aura::compiler::types::is_string(*orch)) {
            const auto idx = aura::compiler::types::as_string_idx(*orch);
            const auto& heap = cs.evaluator().string_heap();
            if (idx < heap.size()) {
                const auto& json = heap[idx];
                CHECK(json.find("eda_sv_evolution_cycles") != std::string::npos,
                      "orchestration-metrics includes eda_sv_evolution_cycles");
                CHECK(json.find("eda_sv_corruption_detected") != std::string::npos,
                      "orchestration-metrics includes eda_sv_corruption_detected");
            }
        }
    }

    const auto cycles_before = stat_int(cs, "evolution-cycles");

    // AC2: long-run demo closed-loop
    {
        std::println("\n--- AC2: eda:demo-sv-self-evolution {} cycles ---", k_cycles);
        auto r = cs.eval(std::format("(eda:demo-sv-self-evolution \"interface\" {})", k_cycles));
        CHECK(
            r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) > 0,
            std::format("demo successes > 0 (got {})", r ? aura::compiler::types::as_int(*r) : -1));
        const auto cycles_after = stat_int(cs, "evolution-cycles");
        CHECK(cycles_after >= cycles_before + k_cycles,
              std::format("evolution-cycles grew by >= {} ({} -> {})", k_cycles, cycles_before,
                          cycles_after));
        CHECK(stat_int(cs, "corruption-detected") == 0, "no corruption during long-run demo");
    }

    // AC3: query:sv-sva-structure-stats during stress
    {
        std::println("\n--- AC3: query:sv-sva-structure-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:sv-sva-structure-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:sv-sva-structure-stats returns hash");
        auto prop_r =
            cs.eval("(hash-ref (engine:metrics \"query:sv-sva-structure-stats\") 'property-count)");
        CHECK(prop_r && aura::compiler::types::is_int(*prop_r) &&
                  aura::compiler::types::as_int(*prop_r) >= 100,
              "property-count >= 100 after SoC build");
    }

    // AC4: chaos — GC safepoint + hot-swap attempt + fiber stress
    {
        std::println("\n--- AC4: chaos injection + fiber stress ---");
        (void)cs.eval("(mutate:request-gc-safepoint)");
        (void)cs.eval("(hot-swap:fn \"soc\" \"(define soc 2)\")");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 30;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(typecheck-current)");
                if ((i & 3) == 0)
                    (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r = cs.eval("(eda:run-verification-feedback \"coverage.log\" \"0 chaos\")");
                if (r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() > 0,
              std::format("fiber stress: {} successful loops", ok_count.load()));
        CHECK(stat_int(cs, "corruption-detected") == 0, "no corruption after chaos stress");
    }

    // AC5: orchestration-metrics reflects post-stress evolution cycles
    {
        std::println("\n--- AC5: post-stress orchestration-metrics ---");
        auto orch = cs.eval("(stats:get \"query:orchestration-metrics\")");
        CHECK(orch && aura::compiler::types::is_string(*orch),
              "post-stress orchestration-metrics readable");
        if (orch && aura::compiler::types::is_string(*orch)) {
            const auto idx = aura::compiler::types::as_string_idx(*orch);
            const auto& heap = cs.evaluator().string_heap();
            if (idx < heap.size()) {
                CHECK(heap[idx].find("eda_sv_evolution_cycles") != std::string::npos,
                      "post-stress JSON still has eda_sv_evolution_cycles");
            }
        }
        const auto rate = stat_int(cs, "verification-convergence-rate");
        CHECK(rate >= 90, std::format("verification-convergence-rate >= 90% (got {}%)", rate));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_eda_sv_verification_closedloop_stress_run();
}
#endif
