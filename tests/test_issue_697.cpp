// @category: integration
// @reason: Issue #697 Declarative Primitives Extension Kit + AI Agent EDA integration

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

namespace aura_issue_697_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:primitives-extension-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::string hash_str(aura::compiler::CompilerService& cs, const char* hash_expr,
                            std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_expr, key));
    if (!r || !aura::compiler::types::is_string(*r))
        return {};
    const auto idx = aura::compiler::types::as_string_idx(*r);
    const auto& heap = cs.evaluator().string_heap();
    if (idx >= heap.size())
        return {};
    return heap[idx];
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

} // namespace aura_issue_697_detail

int main() {
    using namespace aura_issue_697_detail;
    std::println("=== Issue #697: Declarative Primitives Extension Kit ===");

    aura::compiler::CompilerService cs;

    // AC1: query:primitives-extension-stats hash fields
    {
        std::println("\n--- AC1: query:primitives-extension-stats ---");
        auto stats = cs.eval("(query:primitives-extension-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:primitives-extension-stats returns hash");
        CHECK(stat_int(cs, "eda-meta-backfilled") >= 4,
              "eda-meta-backfilled >= 4 SV/EDA primitives");
        CHECK(stat_int(cs, "category-sva") >= 2, "category-sva >= 2 (weaken + coverpoint)");
        CHECK(stat_int(cs, "category-verification") >= 1, "category-verification >= 1");
        CHECK(stat_int(cs, "category-eda") >= 1, "category-eda >= 1");
        CHECK(stat_int(cs, "documented-with-schema") >= 4, "documented-with-schema >= 4");
        CHECK(stat_int(cs, "extension-kit-version") == 2, "extension-kit-version == 2");
        CHECK(stat_int(cs, "registry-slots") > 0, "registry-slots > 0");
    }

    // AC2: primitive:generate-skeleton returns AI-friendly bundle
    {
        std::println("\n--- AC2: primitive:generate-skeleton ---");
        const auto sk_before = stat_int(cs, "skeleton-generations");
        auto sk = cs.eval("(primitive:generate-skeleton "
                          "\"description: add coverpoint bin to covergroup\")");
        CHECK(sk && aura::compiler::types::is_hash(*sk), "generate-skeleton returns hash");
        const auto sk_after = stat_int(cs, "skeleton-generations");
        CHECK(sk_after > sk_before,
              std::format("skeleton-generations grew ({} -> {})", sk_before, sk_after));
        const auto cat = hash_str(cs, "(primitive:generate-skeleton \"coverpoint\")", "category");
        CHECK(cat == "sva", std::format("coverpoint description -> sva category (got {})", cat));
        const auto spec =
            hash_str(cs, "(primitive:generate-skeleton \"add coverpoint to covergroup\")", "spec");
        CHECK(!spec.empty() && spec.find("coverpoint") != std::string::npos,
              "spec mentions coverpoint");
        const auto cpp = hash_str(
            cs, "(primitive:generate-skeleton \"add coverpoint to covergroup\")", "cpp-lambda");
        CHECK(!cpp.empty() && cpp.find("add_mutate") != std::string::npos,
              "cpp-lambda contains add_mutate");
        const auto reg = hash_str(
            cs, "(primitive:generate-skeleton \"add coverpoint to covergroup\")", "registration");
        CHECK(!reg.empty() && reg.find("DEFINE_PRIMITIVE_META") != std::string::npos,
              "registration contains DEFINE_PRIMITIVE_META");
    }

    aura::ast::NodeId property_id = aura::ast::NULL_NODE;
    aura::ast::NodeId coverpoint_id = aura::ast::NULL_NODE;
    seed_sva_workspace(cs, property_id, coverpoint_id);

    // AC3: EDA primitives have category/schema meta via primitive:describe
    {
        std::println("\n--- AC3: primitive:describe EDA meta backfill ---");
        auto desc = cs.eval("(primitive:describe \"eda:weaken-property\")");
        CHECK(desc && aura::compiler::types::is_pair(*desc),
              "describe eda:weaken-property returns pair");
        auto cat = cs.eval("(car (cdr (cdr (cdr (cdr (primitive:describe "
                           "\"eda:weaken-property\"))))))");
        CHECK(cat && aura::compiler::types::is_string(*cat), "category field is string");
        if (cat && aura::compiler::types::is_string(*cat)) {
            const auto idx = aura::compiler::types::as_string_idx(*cat);
            const auto& heap = cs.evaluator().string_heap();
            CHECK(idx < heap.size() && heap[idx] == "sva",
                  std::format("eda:weaken-property category is sva (got {})",
                              idx < heap.size() ? heap[idx] : "?"));
        }
        auto schema = cs.eval("(cdr (cdr (cdr (cdr (cdr (primitive:describe "
                              "\"eda:add-coverpoint-bin\"))))))");
        CHECK(schema && aura::compiler::types::is_string(*schema), "schema field is string");
    }

    // AC4: generated skeleton pattern used in verification closed-loop
    {
        std::println("\n--- AC4: skeleton-guided closed-loop ---");
        const auto snippet = hash_str(
            cs, "(primitive:generate-skeleton \"verification feedback coverage\")", "test-snippet");
        CHECK(!snippet.empty() &&
                  snippet.find("eda:run-verification-feedback") != std::string::npos,
              "verification skeleton test-snippet references feedback primitive");
        auto r =
            cs.eval(std::format("(eda:run-verification-feedback \"coverage.log\" \"{} stress\")",
                                static_cast<int>(coverpoint_id)));
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "feedback closed-loop succeeds with coverpoint target");
        auto weaken = cs.eval(
            std::format("(eda:weaken-property {} \"reset\")", static_cast<int>(property_id)));
        CHECK(weaken && aura::compiler::types::is_bool(*weaken) &&
                  aura::compiler::types::as_bool(*weaken),
              "weaken-property succeeds on seeded property");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 83,
              "stats:count == 83");
    }

    // AC6: fiber stress — generate-skeleton + describe + EDA mutate
    {
        std::println("\n--- AC6: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(primitive:generate-skeleton \"coverpoint bin\")");
                (void)cs.eval("(primitive:describe \"eda:demo-sv-self-evolution\")");
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r = cs.eval("(eda:run-verification-feedback \"coverage.log\" \"0 fiber\")");
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
        CHECK(stat_int(cs, "skeleton-generations") >= 2 * k_iters + 3,
              "skeleton-generations accumulated under fiber stress");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}