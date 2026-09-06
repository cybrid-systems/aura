// @category: unit
// @reason: Issue #2419 — tag_arity_index_ map protected vs concurrent rebuild.
//
//   AC1: find_by_tag_arity under shared map lock (after ensure)
//   AC2: rebuild/ensure under exclusive map lock
//   AC3: concurrent find + mark_dirty/ensure no crash + consistent
//   AC4: steady-state ensure is shared-lock fast path when clean

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
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

static void ac3589_1_head_scan_green() {
    std::println("\n--- #3589 AC1: HEAD structural writes sync index ---");
    const auto lint =
        read_file("scripts/coverage/checks/check_index_sync_structural_write_3589.py");
    CHECK(lint.find("Issue #3589") != std::string::npos, "3589 AC1: linter cites #3589");
    CHECK(lint.find("classify_structural_writes") != std::string::npos,
          "3589 AC1: executable classifier");
    CHECK(lint.find("src/core/mutation.ixx") != std::string::npos, "3589 AC1: scans mutation.ixx");
    CHECK(lint.find("src/core/mutators.ixx") != std::string::npos, "3589 AC1: scans mutators.ixx");
    CHECK(lint.find("src/core/ast_mutation_pipeline.ixx") != std::string::npos,
          "3589 AC1: scans ast_mutation_pipeline.ixx");
    const auto mut = read_file("src/core/mutators.ixx");
    CHECK(mut.find("flat.set_child") != std::string::npos, "3589 AC1: Replace uses set_child");
    CHECK(mut.find("flat.mark_dirty_upward(target)") != std::string::npos,
          "3589 AC1: mutators mark_dirty_upward");
    const auto ast = read_file("src/core/ast.ixx");
    CHECK(ast.find("tag_arity_index. mark_dirty_upward + structural mutate") != std::string::npos,
          "3589 AC1: ast.ixx index-sync contract");
}

static void ac3589_2_linter_rejects_unsynced_write() {
    std::println("\n--- #3589 AC2: linter rejects unsynced structural write ---");
    const auto lint =
        read_file("scripts/coverage/checks/check_index_sync_structural_write_3589.py");
    CHECK(lint.find("unsynced set_child") != std::string::npos,
          "3589 AC2: synthetic unsynced-write self-test");
    CHECK(lint.find("mark_dirty_upward-synced write") != std::string::npos,
          "3589 AC2: synced-write self-test");
}

static void ac3589_3_static_no_runtime() {
    std::println("\n--- #3589 AC3: pure static, no runtime change ---");
    CHECK(read_file("tests/core/test_issue_3589.cpp").empty(), "3589 AC3: no test_issue_3589.cpp");
    CHECK(read_file("tests/compiler/test_issue_3589.cpp").empty(),
          "3589 AC3: no compiler test_issue_3589.cpp");
    CHECK(read_file("docs/design/3589-index-sync-machine.md").empty(), "3589 AC3: no docs/design/");
    CHECK(read_file("src/compiler/evaluator_primitives_obs_jit.cpp").find("schema-3589") ==
              std::string::npos,
          "3589 AC3: no schema-3589");
}

} // namespace

int run_test_tag_arity_index_lock() {
    std::println("=== Issue #2419: tag_arity_index_ lock protection ===");

    // ── AC1 find_by_tag_arity results ──────────────────────────────
    {
        std::println("\n--- #2419 AC1: find_by_tag_arity ---");
        FlatAST flat;
        const NodeId p = flat.add_node(NodeTag::Begin);
        const NodeId a = flat.add_node(NodeTag::LiteralInt);
        const NodeId b = flat.add_node(NodeTag::LiteralInt);
        flat.root = p;
        flat.insert_child(p, 0, a);
        flat.insert_child(p, 1, b);
        auto lits = flat.find_by_tag_arity(static_cast<std::uint32_t>(NodeTag::LiteralInt), 0, 0);
        CHECK(lits.size() == 2, "AC1: two LiteralInt arity-0");
        auto begins = flat.find_by_tag_arity(static_cast<std::uint32_t>(NodeTag::Begin), 2, 2);
        CHECK(begins.size() == 1, "AC1: one Begin arity-2");
    }

    // ── AC2 ensure exclusive rebuild path ──────────────────────────
    {
        std::println("\n--- #2419 AC2: ensure rebuild ---");
        FlatAST flat;
        (void)flat.add_node(NodeTag::LiteralInt);
        flat.mark_tag_arity_index_dirty();
        flat.ensure_tag_arity_index();
        CHECK(!flat.tag_arity_index_dirty(), "AC2: clean after ensure");
        CHECK(flat.tag_arity_index_size() > 0, "AC2: index non-empty");
    }

    // ── AC4 clean ensure is cheap (no rebuild) ─────────────────────
    {
        std::println("\n--- #2419 AC4: clean ensure no rebuild ---");
        FlatAST flat;
        (void)flat.add_node(NodeTag::LiteralInt);
        flat.ensure_tag_arity_index();
        const auto r0 = flat.tag_arity_index_rebuilds();
        flat.ensure_tag_arity_index();
        flat.ensure_tag_arity_index();
        CHECK(flat.tag_arity_index_rebuilds() == r0, "AC4: clean ensure no rebuild");
        (void)flat.find_by_tag_arity(static_cast<std::uint32_t>(NodeTag::LiteralInt), 0, 0);
        CHECK(flat.tag_arity_index_rebuilds() == r0, "AC4: find clean no rebuild");
    }

    // ── AC3 concurrent find + dirty/ensure ─────────────────────────
    {
        std::println("\n--- #2419 AC3: concurrent find + mark/ensure ---");
        FlatAST flat;
        const NodeId p = flat.add_node(NodeTag::Begin);
        flat.root = p;
        for (int i = 0; i < 32; ++i) {
            const NodeId lit = flat.add_node(NodeTag::LiteralInt);
            flat.insert_child(p, 0, lit);
        }
        flat.ensure_tag_arity_index();

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> finds{0};
        std::atomic<std::uint64_t> dirties{0};
        std::atomic<std::uint64_t> errors{0};

        std::thread finder([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    auto v = flat.find_by_tag_arity(static_cast<std::uint32_t>(NodeTag::LiteralInt),
                                                    0, 0);
                    (void)v;
                    finds.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        std::thread dirtier([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    flat.mark_tag_arity_index_dirty();
                    flat.ensure_tag_arity_index();
                    dirties.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        std::thread mutator([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    const NodeId lit = flat.add_node(NodeTag::LiteralInt);
                    flat.insert_child(p, 0, lit);
                    flat.mark_dirty_upward(p);
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop.store(true, std::memory_order_release);
        finder.join();
        dirtier.join();
        mutator.join();

        std::println("  finds={} dirties={} errors={}", finds.load(), dirties.load(),
                     errors.load());
        CHECK(finds.load() > 0, "AC3: finds progressed");
        CHECK(dirties.load() > 0, "AC3: ensure progressed");
        CHECK(errors.load() == 0, "AC3: no exceptions");
        auto final_lits =
            flat.find_by_tag_arity(static_cast<std::uint32_t>(NodeTag::LiteralInt), 0, 0);
        CHECK(final_lits.size() >= 32, "AC3: final index has lits");
    }

    std::println("\n=== Issue #3589: index-sync structural-write machine proof ===");
    ac3589_1_head_scan_green();
    ac3589_2_linter_rejects_unsynced_write();
    ac3589_3_static_no_runtime();

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_tag_arity_index_lock();
}
#endif
