// test_terminal_concurrent.cpp — Issue #1352 (standalone; free-corruption when co-linked)

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

// test_terminal_concurrent.cpp — Issue #1352: per-buffer locks under threads
//
// Note: a single CompilerService/Evaluator is not fully thread-safe for eval.
// Buffer IDs are process-global (#1352 registry), so each thread uses its own
// CompilerService while sharing buffer ids — that stresses registry + rwlocks.


using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

namespace {

std::int64_t make_buf(CompilerService& cs, int w, int h) {
    auto id = cs.eval(std::format("(make-terminal-buffer {} {})", w, h));
    if (!id || !is_int(*id))
        return -1;
    return as_int(*id);
}

bool set_cell(CompilerService& cs, std::int64_t id, int x, int y, int ch, int fg) {
    auto r = cs.eval(std::format("(terminal-set-cell {} {} {} {} {} 0)", id, x, y, ch, fg));
    return r && is_bool(*r) && as_bool(*r);
}

} // namespace

int main() {
    CompilerService setup;

    // AC3: concurrent set-cell on *different* buffers — no deadlock
    {
        auto a = make_buf(setup, 32, 8);
        auto b = make_buf(setup, 32, 8);
        CHECK(a >= 0 && b >= 0, "two buffers");
        std::atomic<int> ok_a{0};
        std::atomic<int> ok_b{0};
        std::atomic<int> fail{0};
        constexpr int kN = 200;
        std::thread t1([&] {
            CompilerService cs;
            for (int i = 0; i < kN; ++i) {
                if (set_cell(cs, a, i % 32, 0, 65, 1))
                    ok_a.fetch_add(1, std::memory_order_relaxed);
                else
                    fail.fetch_add(1, std::memory_order_relaxed);
            }
        });
        std::thread t2([&] {
            CompilerService cs;
            for (int i = 0; i < kN; ++i) {
                if (set_cell(cs, b, i % 32, 0, 66, 2))
                    ok_b.fetch_add(1, std::memory_order_relaxed);
                else
                    fail.fetch_add(1, std::memory_order_relaxed);
            }
        });
        t1.join();
        t2.join();
        CHECK(fail.load() == 0, "no failures across buffers");
        CHECK(ok_a.load() == kN && ok_b.load() == kN, "2×200 set-cell on distinct buffers");
        (void)setup.eval(std::format("(delete-terminal-buffer {})", a));
        (void)setup.eval(std::format("(delete-terminal-buffer {})", b));
    }

    // AC5: 4 threads × N set-cell on same buffer — no race/deadlock
    {
        auto id = make_buf(setup, 64, 16);
        CHECK(id >= 0, "shared buffer");
        std::atomic<int> ok{0};
        std::atomic<int> fail{0};
        constexpr int kThreads = 4;
        constexpr int kN = 200;
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                CompilerService cs;
                for (int i = 0; i < kN; ++i) {
                    const int x = (t * 17 + i) % 64;
                    const int y = (t + i) % 16;
                    const int ch = 48 + (t % 10);
                    if (set_cell(cs, id, x, y, ch, t + 1))
                        ok.fetch_add(1, std::memory_order_relaxed);
                    else
                        fail.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(fail.load() == 0, "same-buffer concurrent: no failures");
        CHECK(ok.load() == kThreads * kN, "4×200 set-cell same buffer");
        auto frame = setup.eval(std::format("(terminal-frame-ansi {})", id));
        CHECK(frame.has_value(), "frame after concurrent writes");
        (void)setup.eval(std::format("(delete-terminal-buffer {})", id));
    }

    // Concurrent read (diff) + write (set-cell)
    {
        auto a = make_buf(setup, 16, 4);
        auto b = make_buf(setup, 16, 4);
        CHECK(a >= 0 && b >= 0, "diff pair");
        for (int i = 0; i < 16; ++i) {
            (void)set_cell(setup, a, i, 0, 46, 7);
            (void)set_cell(setup, b, i, 0, 46, 7);
        }
        std::atomic<int> diffs{0};
        std::atomic<int> writes{0};
        std::atomic<bool> stop{false};
        std::thread reader([&] {
            CompilerService cs;
            while (!stop.load(std::memory_order_relaxed)) {
                auto d = cs.eval(std::format("(terminal-diff-update {} {})", a, b));
                if (d && is_int(*d) && as_int(*d) >= 0)
                    diffs.fetch_add(1, std::memory_order_relaxed);
            }
        });
        std::thread writer([&] {
            CompilerService cs;
            for (int i = 0; i < 200; ++i) {
                if (set_cell(cs, b, i % 16, 0, 88, 1))
                    writes.fetch_add(1, std::memory_order_relaxed);
            }
            stop.store(true, std::memory_order_relaxed);
        });
        writer.join();
        reader.join();
        CHECK(writes.load() == 200, "writer completed 200");
        CHECK(diffs.load() >= 1, "reader got at least one stable diff result");
        (void)setup.eval(std::format("(delete-terminal-buffer {})", a));
        (void)setup.eval(std::format("(delete-terminal-buffer {})", b));
    }

    // Concurrent delete of one buffer while writing another
    {
        auto keep = make_buf(setup, 8, 2);
        auto drop = make_buf(setup, 8, 2);
        std::atomic<int> ok{0};
        std::thread w([&] {
            CompilerService cs;
            for (int i = 0; i < 100; ++i) {
                if (set_cell(cs, keep, 0, 0, 65, 1))
                    ok.fetch_add(1, std::memory_order_relaxed);
            }
        });
        std::thread d([&] {
            CompilerService cs;
            for (int i = 0; i < 20; ++i)
                (void)cs.eval(std::format("(delete-terminal-buffer {})", drop));
        });
        w.join();
        d.join();
        CHECK(ok.load() == 100, "writes on live buffer during sibling deletes");
        (void)setup.eval(std::format("(delete-terminal-buffer {})", keep));
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("terminal concurrent #1352: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
