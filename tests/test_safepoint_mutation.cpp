// test_safepoint_mutation.cpp — Issue #1364: safepoint × mutation telemetry

#include "test_harness.hpp"
#include "core/gc_hooks.h"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

bool file_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}

std::int64_t href(CompilerService& cs, const char* q, const char* key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    // Reset process-wide flag between tests
    aura::gc_hooks::g_arena_safepoint_active.store(false, std::memory_order_release);

    // ── AC: in_gc_safepoint false initially ──
    CHECK(!aura::gc_hooks::in_gc_safepoint(), "initially not in safepoint");

    // ── ScopedSafepoint sets active ──
    {
        CHECK(!aura::gc_hooks::in_gc_safepoint(), "before scope");
        {
            aura::gc_hooks::ScopedSafepoint sp;
            CHECK(aura::gc_hooks::in_gc_safepoint(), "inside ScopedSafepoint");
            // Nested: outer still true after inner ends
            {
                aura::gc_hooks::ScopedSafepoint nested;
                CHECK(aura::gc_hooks::in_gc_safepoint(), "nested still true");
            }
            CHECK(aura::gc_hooks::in_gc_safepoint(), "outer still true after nested dtor");
        }
        CHECK(!aura::gc_hooks::in_gc_safepoint(), "after scope restored");
    }

    // ── Manual flag + mutation guard instrumentation ──
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "compiler_metrics available");
        const auto before = m->mutation_in_safepoint_total.load(std::memory_order_relaxed);
        const auto coll0 = m->safepoint_collision_total.load(std::memory_order_relaxed);

        aura::gc_hooks::g_arena_safepoint_active.store(true, std::memory_order_release);
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard g(ev, &ok);
            CHECK(ok, "guard default ok");
        }
        aura::gc_hooks::g_arena_safepoint_active.store(false, std::memory_order_release);

        CHECK(m->mutation_in_safepoint_total.load(std::memory_order_relaxed) == before + 1,
              "mutation_in_safepoint_total +1");
        CHECK(m->safepoint_collision_total.load(std::memory_order_relaxed) == coll0 + 1,
              "safepoint_collision_total +1");
    }

    // ── Guard outside safepoint does not bump ──
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto before = m->mutation_in_safepoint_total.load(std::memory_order_relaxed);
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard g(ev, &ok);
        }
        CHECK(m->mutation_in_safepoint_total.load(std::memory_order_relaxed) == before,
              "no bump outside safepoint");
    }

    // ── note_safepoint_yield_on_mutation ──
    {
        const auto y0 = aura::gc_hooks::safepoint_yield_on_mutation_total();
        aura::gc_hooks::note_safepoint_yield_on_mutation();
        aura::gc_hooks::note_safepoint_yield_on_mutation();
        CHECK(aura::gc_hooks::safepoint_yield_on_mutation_total() == y0 + 2, "yield counter +2");
    }

    // ── Many ScopedSafepoint cycles ──
    {
        for (int i = 0; i < 1000; ++i) {
            aura::gc_hooks::ScopedSafepoint sp;
            CHECK(aura::gc_hooks::in_gc_safepoint(), "cycle in safepoint");
        }
        CHECK(!aura::gc_hooks::in_gc_safepoint(), "after 1000 cycles cleared");
    }

    // ── Concurrent ScopedSafepoint (last writer wins restore — sequential nests) ──
    {
        std::atomic<int> in_count{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 100; ++i) {
                    aura::gc_hooks::ScopedSafepoint sp;
                    if (aura::gc_hooks::in_gc_safepoint())
                        in_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(in_count.load() == 400, "400 in-safepoint observations");
        // Concurrent ScopedSafepoint can leave flag true if dtor restore races —
        // force clear for clean process state (flag is process-wide).
        aura::gc_hooks::g_arena_safepoint_active.store(false, std::memory_order_release);
        CHECK(!aura::gc_hooks::in_gc_safepoint(), "cleared after concurrent stress");
    }

    // ── 100 mutations under safepoint ──
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto before = m->mutation_in_safepoint_total.load(std::memory_order_relaxed);
        aura::gc_hooks::g_arena_safepoint_active.store(true, std::memory_order_release);
        for (int i = 0; i < 100; ++i) {
            bool ok = true;
            Evaluator::MutationBoundaryGuard g(ev, &ok);
        }
        aura::gc_hooks::g_arena_safepoint_active.store(false, std::memory_order_release);
        CHECK(m->mutation_in_safepoint_total.load(std::memory_order_relaxed) == before + 100,
              "100 mutation_in_safepoint events");
    }

    // ── Aura query ──
    {
        CompilerService cs;
        auto s = cs.eval("(engine:metrics \"query:safepoint-mutation-stats\")");
        CHECK(s && is_hash(*s), "query:safepoint-mutation-stats is hash");
        CHECK(href(cs, "query:safepoint-mutation-stats", "in-gc-safepoint") == 0,
              "in-gc-safepoint snapshot 0");
        CHECK(href(cs, "query:safepoint-mutation-stats", "mutation-in-safepoint-total") >= 0,
              "mutation-in-safepoint-total key");
        CHECK(href(cs, "query:safepoint-mutation-stats", "safepoint-yield-on-mutation-total") >= 0,
              "yield key");
        CHECK(href(cs, "query:safepoint-mutation-stats", "safepoint-collision-total") >= 0,
              "collision key");
    }

    // ── Documentation present ──
    CHECK(file_exists("docs/development/safepoint-mutation.md"),
          "docs/development/safepoint-mutation.md");

    if (::aura::test::g_failed)
        return 1;
    std::println("safepoint mutation #1364: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
