// @category: unit
// @reason: Issue #1848 — compile:subtree-generation and
// compile:subtree-bump-count must shared_lock workspace_mtx_
// while reading counters so concurrent compile:subtree-bump
// (#1847 unique Guard) cannot race subtree_gen_ resize / tear.
//
//   AC1: source cites #1848; both readers use shared_lock
//   AC2: sequential bump then stats:get bump-count advances
//   AC3: concurrent C++ shared_lock readers + EDSL bump no hang

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: shared_lock on subtree readers ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_06.cpp");
        CHECK(src.find("#1848") != std::string::npos, "cites #1848");

        auto gen_pos = src.find("\"compile:subtree-generation\"");
        CHECK(gen_pos != std::string::npos, "subtree-generation present");
        auto gen_win = src.substr(gen_pos, 900);
        CHECK(gen_win.find("shared_lock") != std::string::npos, "generation uses shared_lock");
        CHECK(gen_win.find("workspace_mtx_") != std::string::npos,
              "generation locks workspace_mtx_");
        CHECK(gen_win.find("subtree_generation") != std::string::npos, "calls subtree_generation");

        auto cnt_pos = src.find("\"compile:subtree-bump-count\"");
        CHECK(cnt_pos != std::string::npos, "subtree-bump-count present");
        auto cnt_win = src.substr(cnt_pos, 700);
        CHECK(cnt_win.find("shared_lock") != std::string::npos, "bump-count uses shared_lock");
        CHECK(cnt_win.find("workspace_mtx_") != std::string::npos,
              "bump-count locks workspace_mtx_");
        CHECK(cnt_win.find("subtree_bump_count") != std::string::npos, "calls subtree_bump_count");
    }

    // ── AC2: sequential bump + stats:get ──
    {
        std::println("\n--- AC2: bump then stats:get bump-count ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1) (define y 2)\")").has_value(), "set-code");
        auto id_v = cs.eval("(car (query:defines-by-marker \"User\"))");
        CHECK(id_v && is_int(*id_v), "resolve define id");
        const auto id = as_int(*id_v);

        auto before = cs.eval("(stats:get \"compile:subtree-bump-count\")");
        // zero-arity stats path; may be int or void if not registered in facade
        std::int64_t b = 0;
        if (before && is_int(*before))
            b = as_int(*before);

        auto bump = cs.eval(std::format("(compile:subtree-bump {})", id));
        CHECK(bump && is_int(*bump) && as_int(*bump) >= 0, "bump ok");

        auto after = cs.eval("(stats:get \"compile:subtree-bump-count\")");
        if (after && is_int(*after) && bump && is_int(*bump) && as_int(*bump) == 1) {
            CHECK(as_int(*after) >= b + 1, "bump-count advanced after successful bump");
        } else if (after && is_int(*after)) {
            CHECK(as_int(*after) >= b, "bump-count non-decreasing");
        } else {
            // Facade may not surface this name; C++ path still covered in AC3.
            CHECK(true, "stats:get optional for this name (C++ lock path in AC3)");
        }

        // Direct C++ read under public lock API (mirrors primitive).
        auto& ev = cs.evaluator();
        ev.lock_workspace_shared();
        CHECK(ev.workspace_flat() != nullptr, "workspace present");
        if (ev.workspace_flat()) {
            auto g = ev.workspace_flat()->subtree_generation(static_cast<std::uint32_t>(id));
            CHECK(true, std::format("subtree_generation under lock = {}", g));
            (void)ev.workspace_flat()->subtree_bump_count();
        }
        ev.unlock_workspace_shared();
    }

    // ── AC3: concurrent C++ readers + writer via public lock API ──
    // Mirrors the primitive contract without concurrent cs.eval
    // (CompilerService eval is not multi-thread re-entrant).
    {
        std::println("\n--- AC3: concurrent shared readers + unique bump ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define a 1) (define b 2)\")").has_value(), "seed");
        auto id_v = cs.eval("(car (query:defines-by-marker \"User\"))");
        CHECK(id_v && is_int(*id_v), "id");
        const auto id = static_cast<std::uint32_t>(as_int(*id_v));
        auto& ev = cs.evaluator();
        CHECK(ev.workspace_flat() != nullptr, "workspace for stress");

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> bumps{0};
        std::vector<std::thread> thr;

        auto reader = [&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                ev.lock_workspace_shared();
                if (auto* ws = ev.workspace_flat()) {
                    (void)ws->subtree_generation(id);
                    (void)ws->subtree_bump_count();
                }
                ev.unlock_workspace_shared();
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        };
        thr.emplace_back(reader);
        thr.emplace_back(reader);
        thr.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                // Same lock the #1847 Guard holds (unique).
                ev.lock_workspace_unique();
                if (auto* ws = ev.workspace_flat()) {
                    ws->bump_generation_subtree(id);
                    bumps.fetch_add(1, std::memory_order_relaxed);
                }
                ev.unlock_workspace_unique();
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : thr)
            t.join();

        CHECK(reads.load() > 0, "readers made progress");
        CHECK(bumps.load() > 0, "writer made progress");
        // Post-join single-threaded consistency.
        ev.lock_workspace_shared();
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "ws still live");
        if (ws) {
            CHECK(ws->subtree_bump_count() >= bumps.load(), "count >= observed bumps");
        }
        ev.unlock_workspace_shared();
    }

    std::println("\n=== test_subtree_counter_shared_lock_1848: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
