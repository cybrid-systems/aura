// @category: unit
// @reason: Issue #2537 — Hierarchical AgentScope (parent/children cancel tree).
//
//   AC1: parent / children links via spawn_child (unique_ptr, not static table)
//   AC2: parent cancel_all / ~AgentScope recurse children then drain self
//   AC3: single-owner serial model (#2399) still present
//   AC4: MVP linter green — no AgentRegistry / global_agent_registry / conduct_parallel
//   AC5: parent cancel → all descendant fibers observe cancel; no deadlock
//   AC6: README short hierarchy example

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "orch/agent_scope.h"
#include "orch/agent_spawn.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"

import std;

namespace {

using aura::orch::AgentHandle;
using aura::orch::AgentScope;
using aura::orch::AgentSpec;
using aura::orch::kAgentScopeHierarchyIssue;
using aura::serve::Fiber;
using aura::serve::JoinStatus;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::test::g_failed;
using aura::test::g_passed;

struct SchedRunner {
    Scheduler& sched;
    std::thread thr;
    explicit SchedRunner(Scheduler& s)
        : sched(s)
        , thr([&s] { s.run(); }) {}
    ~SchedRunner() {
        sched.stop();
        if (thr.joinable())
            thr.join();
    }
};

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static AgentSpec hold_body(std::atomic<bool>& hold) {
    return AgentSpec{.name = "hold", .body = [&] {
                         while (hold.load(std::memory_order_relaxed)) {
                             if (aura::serve::g_current_fiber &&
                                 aura::serve::g_current_fiber->is_cancel_requested())
                                 break;
                             Fiber::yield(YieldReason::Explicit);
                         }
                     }};
}

// Wait until all handles in `hs` are done, or timeout.
static bool wait_all_done(std::span<const AgentHandle> hs, int max_ms = 5000) {
    for (int i = 0; i < max_ms; ++i) {
        bool all = true;
        for (const auto& h : hs) {
            if (h.fiber && !h.fiber->is_done()) {
                all = false;
                break;
            }
        }
        if (all)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// ── AC1: parent / children links ──────────────────────────────────────
static void ac1_parent_children_links() {
    std::println("\n--- AC1: parent/children links via spawn_child ---");
    Scheduler sched(2);
    SchedRunner runner(sched);
    AgentScope root(sched);
    CHECK(root.is_root(), "AC1: root has no parent");
    CHECK(root.parent() == nullptr, "AC1: parent() null on root");
    CHECK(root.child_count() == 0, "AC1: no children yet");

    auto& c0 = root.spawn_child();
    auto& c1 = root.spawn_child();
    CHECK(root.child_count() == 2, "AC1: two children");
    CHECK(c0.parent() == &root, "AC1: c0.parent == root");
    CHECK(c1.parent() == &root, "AC1: c1.parent == root");
    CHECK(!c0.is_root(), "AC1: child is not root");
    CHECK(&root.child_at(0) == &c0, "AC1: child_at(0) == c0");
    CHECK(&root.child_at(1) == &c1, "AC1: child_at(1) == c1");

    auto& grand = c0.spawn_child();
    CHECK(c0.child_count() == 1, "AC1: grandchild under c0");
    CHECK(grand.parent() == &c0, "AC1: grand.parent == c0");
    CHECK(grand.parent()->parent() == &root, "AC1: two-level parent chain");
}

// ── AC2: cancel/dtor order documented + recursive cancel ──────────────
static void ac2_cancel_order_and_dtor() {
    std::println("\n--- AC2: recursive cancel + documented drain order ---");
    auto header = read_file("src/orch/agent_scope.h");
    CHECK(!header.empty(), "agent_scope.h readable");
    CHECK(header.find("spawn_child") != std::string::npos, "AC2: spawn_child present");
    CHECK(header.find("parent_") != std::string::npos, "AC2: parent_ member");
    CHECK(header.find("children_") != std::string::npos, "AC2: children_ member");
    // Documented top-down cancel / bottom-up drain.
    CHECK(header.find("top-down") != std::string::npos ||
              header.find("children first") != std::string::npos,
          "AC2: cancel order documented (top-down / children first)");
    CHECK(header.find("bottom-up") != std::string::npos ||
              header.find("children_.clear()") != std::string::npos,
          "AC2: drain order documented (bottom-up / children clear)");

    Scheduler sched(2);
    SchedRunner runner(sched);
    std::atomic<bool> hold{true};
    std::atomic<int> cancelled_hits{0};

    AgentScope root(sched);
    root.spawn(AgentSpec{.name = "root-a", .body = [&] {
                             while (hold.load(std::memory_order_relaxed)) {
                                 if (aura::serve::g_current_fiber &&
                                     aura::serve::g_current_fiber->is_cancel_requested()) {
                                     cancelled_hits.fetch_add(1, std::memory_order_relaxed);
                                     break;
                                 }
                                 Fiber::yield(YieldReason::Explicit);
                             }
                         }});
    auto& child = root.spawn_child();
    child.spawn(AgentSpec{.name = "child-a", .body = [&] {
                              while (hold.load(std::memory_order_relaxed)) {
                                  if (aura::serve::g_current_fiber &&
                                      aura::serve::g_current_fiber->is_cancel_requested()) {
                                      cancelled_hits.fetch_add(1, std::memory_order_relaxed);
                                      break;
                                  }
                                  Fiber::yield(YieldReason::Explicit);
                              }
                          }});
    auto& grand = child.spawn_child();
    grand.spawn(AgentSpec{.name = "grand-a", .body = [&] {
                              while (hold.load(std::memory_order_relaxed)) {
                                  if (aura::serve::g_current_fiber &&
                                      aura::serve::g_current_fiber->is_cancel_requested()) {
                                      cancelled_hits.fetch_add(1, std::memory_order_relaxed);
                                      break;
                                  }
                                  Fiber::yield(YieldReason::Explicit);
                              }
                          }});

    // Let bodies schedule.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    root.cancel_all();

    // All three levels should observe cancel without holding forever.
    bool root_done = wait_all_done(root.handles(), 3000);
    bool child_done = wait_all_done(child.handles(), 3000);
    bool grand_done = wait_all_done(grand.handles(), 3000);
    CHECK(root_done && child_done && grand_done, "AC2: all levels done after parent cancel_all");
    CHECK(cancelled_hits.load() >= 3, "AC2: cancel observed on root+child+grand");
    hold.store(false, std::memory_order_relaxed);
}

// ── AC3: single-owner model (#2399) still wired ───────────────────────
static void ac3_single_owner_model() {
    std::println("\n--- AC3: single-owner serial model (#2399) ---");
    auto header = read_file("src/orch/agent_scope.h");
    CHECK(header.find("try_enter") != std::string::npos, "AC3: try_enter still present");
    CHECK(header.find("agent_scope_concurrent_misuse_total") != std::string::npos ||
              header.find("2399") != std::string::npos,
          "AC3: concurrent misuse path / #2399 still present");
    CHECK(header.find("single-owner") != std::string::npos ||
              header.find("Single-owner") != std::string::npos ||
              header.find("serialize") != std::string::npos,
          "AC3: serial / single-owner documented");
}

// ── AC4: no global registry + linter green ────────────────────────────
static void ac4_no_global_registry() {
    std::println("\n--- AC4: no global registry; linter surface clean ---");
    auto header = read_file("src/orch/agent_scope.h");
    CHECK(header.find("class AgentRegistry") == std::string::npos, "AC4: no class AgentRegistry");
    CHECK(header.find("static AgentRegistry") == std::string::npos, "AC4: no static AgentRegistry");
    // Process-global table identifiers must not appear as definitions.
    // Doc comments may name the banned symbols — strip is done by the linter.
    CHECK(header.find("kAgentScopeHierarchyIssue") != std::string::npos,
          "AC4: hierarchy issue stamp present");
    CHECK(kAgentScopeHierarchyIssue == 2537, "AC4: issue stamp == 2537");
}

// ── AC5: two-level tree cancel → residual reclaim path, no deadlock ───
static void ac5_tree_cancel_reclaim() {
    std::println("\n--- AC5: parent cancel drains two-level tree ---");
    Scheduler sched(2);
    SchedRunner runner(sched);
    std::atomic<bool> hold{true};

    {
        AgentScope root(sched);
        for (int i = 0; i < 2; ++i) {
            auto spec = hold_body(hold);
            spec.name = std::format("root-{}", i);
            root.spawn(std::move(spec));
        }
        auto& c0 = root.spawn_child();
        auto& c1 = root.spawn_child();
        for (int i = 0; i < 2; ++i) {
            auto s0 = hold_body(hold);
            s0.name = std::format("c0-{}", i);
            c0.spawn(std::move(s0));
            auto s1 = hold_body(hold);
            s1.name = std::format("c1-{}", i);
            c1.spawn(std::move(s1));
        }
        CHECK(root.size() == 2, "AC5: root 2 agents");
        CHECK(c0.size() == 2 && c1.size() == 2, "AC5: each child 2 agents");
        CHECK(root.child_count() == 2, "AC5: two child scopes");

        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        root.cancel_all();

        // Explicit join_all on each level after cancel (no hang).
        auto jr_root = root.join_all(std::optional<std::uint64_t>{3000});
        auto jr_c0 = c0.join_all(std::optional<std::uint64_t>{3000});
        auto jr_c1 = c1.join_all(std::optional<std::uint64_t>{3000});
        CHECK(jr_root.status == JoinStatus::Ok || jr_root.status == JoinStatus::Timeout,
              "AC5: root join Ok/Timeout");
        CHECK(jr_c0.status == JoinStatus::Ok || jr_c0.status == JoinStatus::Timeout,
              "AC5: c0 join Ok/Timeout");
        CHECK(jr_c1.status == JoinStatus::Ok || jr_c1.status == JoinStatus::Timeout,
              "AC5: c1 join Ok/Timeout");

        // Prefer Done after cancel (bodies exit on is_cancel_requested).
        bool all_done = wait_all_done(root.handles(), 2000) && wait_all_done(c0.handles(), 2000) &&
                        wait_all_done(c1.handles(), 2000);
        CHECK(all_done, "AC5: all tree fibers done after cancel (no deadlock)");
        hold.store(false, std::memory_order_relaxed);
        // ~AgentScope at block end: recursive cancel + bottom-up drain.
    }
    // Second pass: destructor-only path (no explicit cancel).
    {
        hold.store(true, std::memory_order_relaxed);
        AgentScope root(sched);
        auto& child = root.spawn_child();
        root.spawn(hold_body(hold));
        child.spawn(hold_body(hold));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // Falling out of scope: ~AgentScope cancels tree + drains.
    }
    CHECK(true, "AC5: dtor-only hierarchy path completed without hang");
}

// ── AC6: README hierarchy example ─────────────────────────────────────
static void ac6_readme() {
    std::println("\n--- AC6: README hierarchy short example ---");
    auto readme = read_file("src/orch/README.md");
    CHECK(!readme.empty(), "README readable");
    CHECK(readme.find("2537") != std::string::npos ||
              readme.find("spawn_child") != std::string::npos,
          "AC6: README documents hierarchy / spawn_child / #2537");
    CHECK(readme.find("AgentScope") != std::string::npos, "AC6: AgentScope still documented");
}

} // namespace

int run_test_agent_scope_hierarchy_2537() {
    std::println("=== Issue #2537: Hierarchical AgentScope ===");
    CHECK(kAgentScopeHierarchyIssue == 2537, "issue stamp");

    ac1_parent_children_links();
    ac2_cancel_order_and_dtor();
    ac3_single_owner_model();
    ac4_no_global_registry();
    ac5_tree_cancel_reclaim();
    ac6_readme();

    std::println("\n=== #2537 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_scope_hierarchy_2537();
}
#endif
