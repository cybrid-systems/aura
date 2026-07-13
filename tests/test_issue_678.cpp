// @category: integration
// @reason: Issue #678 PCV span lifetime safety in concurrent query paths

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <print>
#include <thread>

#include "../src/core/persistent_child_vector.hh"

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;

namespace aura_issue_678_detail {
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

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:span-lifetime-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_678_detail

int aura_issue_678_run() {
    using namespace aura_issue_678_detail;
    std::println("=== Issue #678: span lifetime safety ===");

    // AC1: children_safe_view pins PCV across structural mutation
    {
        std::println("\n--- AC1: children_safe_view survives concurrent mutate ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        aura::ast::FlatAST flat(arena->allocator());
        auto parent = flat.add_literal(0);
        auto c1 = flat.add_literal(1);
        auto c2 = flat.add_literal(2);
        flat.insert_child(parent, 0, c1);
        flat.insert_child(parent, 1, c2);

        auto safe = flat.children_safe_view(parent);
        CHECK(safe.size() == 2, "safe view has 2 children before mutate");

        std::atomic<bool> reader_done{false};
        bool reader_ok = true;
        std::thread reader([&] {
            for (int i = 0; i < 1000; ++i) {
                if (safe.size() != 2) {
                    reader_ok = false;
                    break;
                }
                (void)safe[0];
                (void)safe[1];
            }
            reader_done.store(true, std::memory_order_release);
        });
        std::thread writer([&] {
            for (int i = 0; i < 50; ++i) {
                flat.remove_child(parent, 1);
                flat.insert_child(parent, 1, c2);
            }
        });
        writer.join();
        reader.join();
        CHECK(reader_done.load(), "reader thread completed without crash");
        CHECK(reader_ok, "safe view size stable under concurrent read");
        CHECK(safe.size() == 2, "pinned view still readable after writer");
    }

    // AC2: parent_safe_view counter + generation tagging
    {
        std::println("\n--- AC2: parent_safe_view increments counter ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        aura::ast::FlatAST flat(arena->allocator());
        auto root = flat.add_literal(0);
        auto child = flat.add_literal(7);
        flat.insert_child(root, 0, child);
        const auto before = flat.parent_safe_view_count();
        auto pref = flat.parent_safe_view(child);
        const auto after = flat.parent_safe_view_count();
        CHECK(after > before, "parent_safe_view_count incremented");
        CHECK(pref.id == root, "parent_safe_view returns parent id");
        CHECK(flat.parent_of(child) == root, "parent_of agrees before mutate");
        flat.remove_child(root, 0);
        CHECK(!flat.is_valid(pref), "parent_safe_view ref invalid after mutate");
    }

    // AC3: query:span-lifetime-stats hash fields
    {
        std::println("\n--- AC3: query:span-lifetime-stats ---");
        aura::compiler::CompilerService cs;
        cs.eval("(set-code \"(define x 1)\")");
        cs.eval("(eval-current)");
        auto stats = cs.eval("(query:span-lifetime-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:span-lifetime-stats returns hash");
        auto unsafe = hash_int(cs, "unsafe-access-attempts");
        auto safe = hash_int(cs, "safe-view-hits");
        auto cow = hash_int(cs, "cow-invalidation-detected");
        CHECK(unsafe >= 0, "unsafe-access-attempts present");
        CHECK(safe >= 0, "safe-view-hits present");
        CHECK(cow >= 0, "cow-invalidation-detected present");
    }

    // AC4: concurrent query:pattern + mutate (serialized eval boundary)
    {
        std::println("\n--- AC4: concurrent query + mutate stress ---");
        aura::compiler::CompilerService cs;
        cs.eval("(set-code \"(define x (+ 1 2))\")");
        cs.eval("(eval-current)");
        std::mutex eval_mtx;
        std::atomic<int> query_ok{0};
        std::atomic<int> mutate_ok{0};
        constexpr int k_iters = 40;

        std::thread q([&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(query:pattern \"(+ ?a ?b)\")");
                if (r)
                    query_ok.fetch_add(1, std::memory_order_relaxed);
            }
        });
        std::thread m([&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(define x (+ x 1))");
                if (r)
                    mutate_ok.fetch_add(1, std::memory_order_relaxed);
            }
        });
        q.join();
        m.join();
        CHECK(query_ok.load() == k_iters, "query:pattern iterations succeeded");
        CHECK(mutate_ok.load() == k_iters, "mutate iterations succeeded");
        auto safe_hits = hash_int(cs, "safe-view-hits");
        CHECK(safe_hits > 0, "matcher used safe children views");
    }

    // AC5: stats registry
    {
        std::println("\n--- AC5: stats:list + stats:count ---");
        aura::compiler::CompilerService cs;
        auto r = cs.eval("(stats:count)");
        const auto n =
            r && aura::compiler::types::is_int(*r) ? aura::compiler::types::as_int(*r) : 0;
        CHECK(n >= 57, std::format("stats:count >= 57 (got {})", n));
        auto lst = cs.eval("(stats:list)");
        CHECK(lst.has_value(), "stats:list eval ok");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_678_run();
}
#endif
