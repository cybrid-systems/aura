// @category: integration
// @reason: Issue #497 — StableRef lifecycle soft compact + refresh + stats

#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_497_detail {
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

static std::int64_t snap_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:stable-ref-lifecycle-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_497_detail

int main() {
    using namespace aura_issue_497_detail;

    std::println("=== Issue #497: StableRef long-session lifecycle ===");

    aura::compiler::CompilerService cs;

    // AC1: query:stable-ref-lifecycle-stats fields
    {
        std::println("\n--- AC1: query:stable-ref-lifecycle-stats ---");
        auto stats = cs.eval("(query:stable-ref-lifecycle-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:stable-ref-lifecycle-stats returns hash");
        CHECK(snap_stat(cs, "generation-wrap-count") >= 0, "generation-wrap-count present");
        CHECK(snap_stat(cs, "wrap-epoch") >= 0, "wrap-epoch present");
        CHECK(snap_stat(cs, "current-generation") >= 0, "current-generation present");
        CHECK(snap_stat(cs, "soft-compact-count") >= 0, "soft-compact-count present");
        CHECK(snap_stat(cs, "stale-ref-auto-refresh-count") >= 0,
              "stale-ref-auto-refresh-count present");
        CHECK(snap_stat(cs, "lifecycle-recommendation") >= 0, "lifecycle-recommendation present");
        CHECK(snap_stat(cs, "lifecycle-total") >= 0, "lifecycle-total present");
    }

    CHECK(cs.eval("(set-code \"(define x 1) (define y 2) (define z 3)\")").has_value(),
          "workspace setup");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr, "workspace available");

    aura::ast::NodeId target = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (ws->get(id).tag == aura::ast::NodeTag::Define) {
            target = id;
            break;
        }
    }
    CHECK(target != aura::ast::NULL_NODE, "found Define node");

    // AC2: refresh_if_stale after gen-only invalidation
    {
        std::println("\n--- AC2: refresh_if_stale ---");
        auto ref = ws->make_ref(target);
        CHECK(ref.is_valid_in(*ws), "ref valid at capture");
        const auto gen_before = ws->current_generation();
        ws->bump_generation();
        CHECK(ws->current_generation() != gen_before, "generation bumped");
        CHECK(!ref.is_valid_in(*ws), "ref stale after bump");
        const auto refresh_before = ws->stale_ref_auto_refresh_count();
        CHECK(ref.refresh_if_stale(*ws), "refresh_if_stale succeeds");
        CHECK(ref.is_valid_in(*ws), "ref valid after refresh");
        CHECK(ws->stale_ref_auto_refresh_count() > refresh_before,
              "stale_ref_auto_refresh_count bumped");
        auto view = ref.validate_or_refresh(*ws);
        CHECK(view.has_value(), "validate_or_refresh returns NodeView");
    }

    // AC3: compact_nodes_soft avoids generation bump
    {
        std::println("\n--- AC3: compact_nodes_soft ---");
        const auto gen_before = ws->current_generation();
        const auto soft_before = ws->soft_compact_count();
        aura::ast::FlatAST scratch;
        aura::ast::StringPool pool;
        auto orphan = scratch.add_variable(pool.intern("orphan"));
        (void)orphan;
        scratch.compact_nodes_soft();
        CHECK(scratch.soft_compact_count() >= soft_before, "scratch soft_compact_count observable");
        ws->compact_nodes_soft();
        CHECK(ws->current_generation() == gen_before,
              "compact_nodes_soft does not bump generation");
        CHECK(snap_stat(cs, "soft-compact-count") >= 0, "soft-compact-count in stats hash");
    }

    // AC4: query:stable-ref-stats regression
    {
        std::println("\n--- AC4: stable-ref-stats regression ---");
        auto legacy = cs.eval("(query:stable-ref-stats)");
        CHECK(legacy && aura::compiler::types::is_int(*legacy), "stable-ref-stats regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 94,
              "stats:count == 94");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}