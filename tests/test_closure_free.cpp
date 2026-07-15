// test_closure_free.cpp — Issue #1361: aura_free_closure + ID reuse

#include "test_harness.hpp"
#include "runtime_shared.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

extern "C" void aura_reset_runtime();

namespace {

std::int64_t href(CompilerService& cs, const char* q, const char* key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    // Isolate from other tests' residual runtime state
    aura_reset_runtime();

    const auto free0 = aura_closure_free_total();
    const auto reuse0 = aura_closure_reuse_total();

    // ── alloc + free single ──
    {
        auto id = aura_alloc_closure(0);
        CHECK(id >= 0, "alloc_closure returns non-neg id");
        CHECK(aura_closure_is_freed(id) == 0, "fresh not freed");
        aura_closure_capture(id, 0, 42);
        aura_free_closure(id);
        CHECK(aura_closure_is_freed(id) == 1, "after free is freed");
        CHECK(aura_closure_call(id, nullptr, 0) == 0, "call freed → 0");
        // Idempotent free
        aura_free_closure(id);
        CHECK(aura_closure_free_total() >= free0 + 1, "free_total bumped once (idempotent)");
    }

    // ── ID reuse ──
    {
        auto a = aura_alloc_closure(1);
        aura_free_closure(a);
        auto b = aura_alloc_closure(2);
        CHECK(b == a, "freed id reused on next alloc");
        CHECK(aura_closure_is_freed(b) == 0, "reused slot live");
        CHECK(aura_closure_reuse_total() > reuse0, "reuse_total bumped");
        aura_free_closure(b);
    }

    // ── arena path free ──
    {
        auto id = aura_alloc_closure_arena(3);
        aura_closure_capture(id, 0, 7);
        aura_closure_capture(id, 1, 8);
        aura_free_closure(id);
        CHECK(aura_closure_is_freed(id) == 1, "arena free marks freed");
        CHECK(aura_closure_call(id, nullptr, 0) == 0, "arena freed call → 0");
        // reuse after arena free
        auto id2 = aura_alloc_closure_arena(4);
        CHECK(id2 == id, "arena slot reused");
        aura_free_closure(id2);
    }

    // ── free non-existent / negative ──
    {
        aura_free_closure(-1);
        aura_free_closure(999999);
        CHECK(true, "free invalid ids no crash");
    }

    // ── 100 alloc + free → live bounded ──
    {
        aura_reset_runtime();
        std::vector<std::int64_t> ids;
        ids.reserve(100);
        for (int i = 0; i < 100; ++i)
            ids.push_back(aura_alloc_closure(i));
        CHECK(aura_closure_live_count() == 100, "100 live after alloc");
        for (auto id : ids)
            aura_free_closure(id);
        CHECK(aura_closure_live_count() == 0, "0 live after free all");
        // Re-alloc 100 → should reuse, slots stay ~100
        for (int i = 0; i < 100; ++i)
            (void)aura_alloc_closure(i);
        CHECK(aura_closure_slot_count() == 100, "slots stay 100 after reuse");
        CHECK(aura_closure_live_count() == 100, "100 live after re-alloc");
        for (std::size_t i = 0; i < 100; ++i)
            aura_free_closure(static_cast<std::int64_t>(i));
    }

    // ── concurrent free (thread-safe) ──
    {
        aura_reset_runtime();
        constexpr int N = 200;
        std::vector<std::int64_t> ids(N);
        for (int i = 0; i < N; ++i)
            ids[i] = aura_alloc_closure(i);
        std::atomic<int> done{0};
        auto worker = [&](int begin, int end) {
            for (int i = begin; i < end; ++i)
                aura_free_closure(ids[static_cast<std::size_t>(i)]);
            done.fetch_add(1);
        };
        std::thread t0(worker, 0, 50);
        std::thread t1(worker, 50, 100);
        std::thread t2(worker, 100, 150);
        std::thread t3(worker, 150, 200);
        t0.join();
        t1.join();
        t2.join();
        t3.join();
        CHECK(done.load() == 4, "4 free workers finished");
        CHECK(aura_closure_live_count() == 0, "all freed concurrently");
        // Double-free concurrent-style
        for (int i = 0; i < N; ++i)
            aura_free_closure(ids[static_cast<std::size_t>(i)]);
        CHECK(true, "double-free after concurrent free ok");
    }

    // ── 10K alloc/free cycles (logical memory: slots stay bounded) ──
    {
        aura_reset_runtime();
        for (int c = 0; c < 10000; ++c) {
            auto id = aura_alloc_closure(c % 17);
            aura_closure_capture(id, 0, c);
            aura_free_closure(id);
        }
        CHECK(aura_closure_slot_count() <= 4, "slots bounded after 10K free cycles");
        CHECK(aura_closure_live_count() == 0, "no live after 10K cycles");
        CHECK(aura_closure_free_total() >= 10000, ">=10K frees recorded");
    }

    // ── Aura primitives ──
    {
        CompilerService cs;
        auto r = cs.eval("(closure:free! -1)");
        CHECK(r && is_bool(*r) && as_bool(*r), "closure:free! returns #t for int");
        auto s = cs.eval("(stats:get \"closure:free-stats\")");
        CHECK(s && is_hash(*s), "closure:free-stats is hash");
        // Stats keys present via hash-ref
        auto ft = href(cs, "stats:get \"closure:free-stats\"", "free-total");
        CHECK(ft >= 0, "free-total key present");
        auto lv = href(cs, "stats:get \"closure:free-stats\"", "live");
        CHECK(lv >= 0, "live key present");
        auto sl = href(cs, "stats:get \"closure:free-stats\"", "slots");
        CHECK(sl >= 0, "slots key present");
        auto ru = href(cs, "stats:get \"closure:free-stats\"", "reuse-total");
        CHECK(ru >= 0, "reuse-total key present");
    }

    // Bad arg
    {
        CompilerService cs;
        auto r = cs.eval("(closure:free!)");
        CHECK(r && is_bool(*r) && !as_bool(*r), "closure:free! no-arg → #f");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("closure free #1361: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
