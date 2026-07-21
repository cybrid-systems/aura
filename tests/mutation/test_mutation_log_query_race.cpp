// @category: integration
// @reason: concurrent query_mutation_log + Aura (mutate:rebind ...) loop.
//          Thread A calls query_mutation_log() directly via C++ API
//          (bypasses cs.eval() serialization to actually race at
//          workspace_mtx_); Thread B does Aura eval that triggers
//          typed_mutate internally. The race window is real.
//
// test_mutation_log_query_race.cpp — Issue #1389:
// query_mutation_log iter+append race — lock_workspace_shared
// not held during copy.
//
// Background: query_mutation_log() returned mutation_log_ by
// reference, then copied into a vector. Concurrent typed_mutate
// could push_back mid-copy → vector realloc → UB.
//
// Fix (service.ixx:7090): wrap the copy in
// `lock_workspace_shared()` + RAII UnlockGuard using the same
// const_cast pattern as CompilerMetrics::snapshot (service.ixx:
// 6222-6250).
//
// Tests:
//   AC1: concurrent query_mutation_log + Aura (mutate:rebind ...)
//        loop runs for T seconds without SIGSEGV/SIGABRT
//   AC2: query_mutation_log returns monotonically non-decreasing
//        size under sequential mutate:rebind (smoke test)
//   AC3: query_mutation_log from many concurrent query threads
//        doesn't deadlock (shared_lock semantics)

#include "test_harness.hpp"

import std;
using namespace std::chrono_literals;

import aura.core;
import aura.core.type;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.compiler.service;

namespace aura_mutation_log_query_race_detail {

// Thread A: loops query_mutation_log() directly via C++ API.
// This bypasses cs.eval()'s internal serialization so the call
// actually races at workspace_mtx_ (the fix-under-test).
void thread_query_loop(aura::compiler::CompilerService& cs, std::atomic<bool>& done,
                       std::atomic<int>& query_count, std::atomic<std::size_t>& max_size) {
    while (!done.load(std::memory_order_acquire)) {
        auto entries = cs.query_mutation_log();
        query_count.fetch_add(1, std::memory_order_relaxed);
        auto sz = entries.size();
        auto prev = max_size.load(std::memory_order_relaxed);
        while (sz > prev && !max_size.compare_exchange_weak(prev, sz)) {
        }
    }
}

// Thread B: Aura (mutate:rebind ...) loop. mutate:rebind goes
// through typed_mutate internally, which appends to
// mutation_log_.
//
// CRITICAL: (set! ...) does NOT append to mutation_log_ — only
// typed_mutate-derived primitives do. Using (set! ...) here would
// make the test vacuous (Thread A would observe max_size=0
// regardless of whether the fix is correct).
//
// Issue #1389 spec: "Thread B loops typed_mutate-like mutation
// that appends to the log". mutate:rebind is the Aura-level
// entry point for typed_mutate.
void thread_mutate_loop(aura::compiler::CompilerService& cs, std::atomic<bool>& done,
                        std::atomic<int>& mutation_count) {
    int n = 0;
    while (!done.load(std::memory_order_acquire)) {
        // (mutate:rebind "name" "value-as-source") — value arg is
        // a STRING containing s-expression source (parsed+eval'd
        // by the primitive). Using literal "{}" → x = n each iter.
        std::string src = std::format("(mutate:rebind \"x\" \"{}\")", n);
        auto r = cs.eval(src);
        (void)r; // may fail in edge cases; counter tracks attempts
        n++;
        mutation_count.fetch_add(1, std::memory_order_relaxed);
    }
}

// ── AC1: 2-thread concurrent query + mutate:rebind, no UB ────
bool test_ac1_concurrent_no_ub() {
    std::println("\n--- AC1: 2-thread concurrent query+mutate:rebind, no UB ---");
    aura::compiler::CompilerService cs;

    // Set up: define x so mutate:rebind has a target.
    cs.eval("(set-code \"(define x 0)\")");
    cs.eval("(eval-current)");

    std::atomic<bool> done{false};
    std::atomic<int> query_count{0};
    std::atomic<int> mutation_count{0};
    std::atomic<std::size_t> max_size{0};

    std::thread ta(thread_query_loop, std::ref(cs), std::ref(done), std::ref(query_count),
                   std::ref(max_size));
    std::thread tb(thread_mutate_loop, std::ref(cs), std::ref(done), std::ref(mutation_count));

    std::this_thread::sleep_for(3s);
    done.store(true, std::memory_order_release);

    ta.join();
    tb.join();

    auto q = query_count.load();
    auto m = mutation_count.load();
    auto mx = max_size.load();
    std::println("  AC1: query_count={} mutation_count={} max_size={}", q, m, mx);
    CHECK(q > 0, "AC1: query thread completed iterations (no deadlock)");
    CHECK(m > 0, "AC1: mutate thread completed iterations (no deadlock)");
    CHECK(mx > 0, "AC1: mutation_log_ actually grew (mutate:rebind "
                  "appended; test exercises the race)");
    CHECK(mx < 1000000, "AC1: max_size under sanity bound (no UB / vector corruption)");
    return true;
}

// ── AC2: query_mutation_log returns well-formed vectors ──────
bool test_ac2_well_formed_vectors() {
    std::println("\n--- AC2: query_mutation_log returns well-formed vectors ---");
    aura::compiler::CompilerService cs;

    cs.eval("(set-code \"(define x 0)\")");
    cs.eval("(eval-current)");

    // Initial: empty log
    auto initial = cs.query_mutation_log();
    std::println("  AC2: initial.size()={}", initial.size());
    CHECK(initial.empty(), "AC2: initial log is empty (no spurious entries)");

    // Mutate via typed_mutate path (mutate:rebind), then query.
    // (set! ...) does NOT append to mutation_log_ — only
    // typed_mutate path does.
    cs.eval("(mutate:rebind \"x\" \"1\")");
    auto after_one = cs.query_mutation_log();
    std::println("  AC2: after_one.size()={}", after_one.size());
    CHECK(after_one.size() >= 1, "AC2: log has >= 1 entry after first mutate:rebind");

    // Mutate twice more
    cs.eval("(mutate:rebind \"x\" \"2\")");
    cs.eval("(mutate:rebind \"x\" \"3\")");
    auto after_three = cs.query_mutation_log();
    std::println("  AC2: after_three.size()={}", after_three.size());
    CHECK(after_three.size() >= after_one.size(),
          "AC2: log size non-decreasing (monotonic append-only growth)");

    return true;
}

// ── AC3: multi-thread query doesn't deadlock ─────────────────
bool test_ac3_multi_query_no_deadlock() {
    std::println("\n--- AC3: 4 concurrent query threads, no deadlock ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define x 0)\")");
    cs.eval("(eval-current)");
    // Seed the log with one entry so query threads have something
    // to iterate over (more realistic than iterating empty log).
    cs.eval("(mutate:rebind \"x\" \"1\")");

    std::atomic<bool> done{false};
    std::atomic<int> total_query_count{0};
    std::atomic<std::size_t> shared_max_size{0};

    auto query_worker = [&]() { thread_query_loop(cs, done, total_query_count, shared_max_size); };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(query_worker);
    }

    std::this_thread::sleep_for(2s);
    done.store(true, std::memory_order_release);

    for (auto& t : threads)
        t.join();

    auto q = total_query_count.load();
    std::println("  AC3: total_query_count={} (4 threads × ~2s)", q);
    CHECK(q > 100, "AC3: 4 concurrent query threads completed > 100 iterations "
                   "(shared_lock allows multiple readers; no deadlock)");
    return true;
}

} // namespace aura_mutation_log_query_race_detail

int main() {
    using namespace aura_mutation_log_query_race_detail;
    bool ok = true;
    ok &= test_ac1_concurrent_no_ub();
    ok &= test_ac2_well_formed_vectors();
    ok &= test_ac3_multi_query_no_deadlock();

    int failed = aura::test::g_failed;
    std::println("\n=== Issue #1389 mutation log query race: {} ({} failures) ===",
                 (ok && failed == 0) ? "PASS" : "FAIL", failed);
    return (ok && failed == 0) ? 0 : 1;
}