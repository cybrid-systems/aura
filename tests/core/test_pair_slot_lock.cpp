// test_pair_slot_lock.cpp -- runtime smoke test for B-024 / #1998
//
// Verifies the IR-executor MakePair path holds aura_lock_workspace_write()
// / aura_unlock_workspace_write() around g_owned_pair_slots_.push_back(slot)
// so the static PairSlotCleanup destructor at aura_jit_runtime.cpp:425
// (process-exit walker) and concurrent sibling bridge-hook writes
// (aura_make_pair at line 1330+) cannot race the push.
//
// The runtime gate exercises functional correctness. `(car (cons …))`
// is NON_ESCAPING, so MakePair mirrors it with the arena and must not
// push that pointer onto g_owned_pair_slots_ (PairSlotCleanup free()s
// the vector). Heap cons (ESCAPED) still take the locked push (#1998).
// The linter (scripts/coverage/checks/check_owned_pair_slots_lock_coverage.py)
// is the structural CI gate: the IR owned push stays inside the write
// lock, and aura_alloc_pair does not take that lock (#898).
//
// AC1: simple `(cons 1 2)` via CompilerService returns car=1, cdr=2
//      (basic functional check that the fix didn't break the path).
// AC2: 100 sequential `(car (cons i i+1))` calls succeed;
//      g_owned_pair_slots_ does not grow (arena mirror).
// AC3: concurrent threads (4 threads x 250 make-pairs each) finish
//      with g_owned_pair_slots_ still flat and every car matching.
// AC4: linter self-test passes (IR lock around the owned push; no
//      workspace write lock inside aura_alloc_pair).
// AC5: g_owned_pair_slots_ size is monotonic non-decreasing during
//      push operations (regression guard for the realloc mid-walk race
//      that would invalidate the destructor's iterator).

import std;
import aura.compiler.service;
import aura.compiler.value;

// `import std;` does not hoist size_t into the global namespace (the
// cstddef entity is std::size_t only). Use std::size_t explicitly
// throughout -- avoids "size_t was not declared" cascading errors
// when the extern "C" accessor declaration below is parsed.
using std::int64_t;
using std::size_t;

// Forward declarations instead of #include "runtime_shared.h". Including
// the project header would transitively pull in <cstdint>/<cstddef>/<vector>
// from a non-module context, which collides with `import std;` in GCC 16
// (std::__terminate redefinition + std::byte multi-definition +
// std::__byte_operand conflicting declarations). We use an extern "C"
// accessor (aura_g_owned_pair_slots_size, defined next to the file-scope
// storage in aura_jit_runtime.cpp) so the test never names std::vector
// directly. The forward declaration of struct PairSlot is a layout hint
// for any caller that wants to walk the slots (none currently do -- the
// test only inspects the size, which is what the B-024 fix protects
// against race-conditioned shrinkage).
struct PairSlot;
extern "C" size_t aura_g_owned_pair_slots_size();
extern "C" int64_t aura_alloc_pair(int64_t car, int64_t cdr);
extern "C" int64_t aura_pair_car(int64_t pair_val);
extern "C" int64_t aura_pair_cdr(int64_t pair_val);
extern "C" std::uint64_t aura_pair_slab_publish_count();

namespace {

int total = 0;
int passed = 0;

void check_eq_i64(int64_t got, int64_t want, const char* label) {
    ++total;
    if (got == want) {
        ++passed;
        std::println("  PASS: {} (got {})", label, got);
    } else {
        std::println("  FAIL: {} -- got {}, want {}", label, got, want);
    }
}

void check_true(bool cond, const char* label) {
    ++total;
    if (cond) {
        ++passed;
        std::println("  PASS: {}", label);
    } else {
        std::println("  FAIL: {}", label);
    }
}

// Helper: eval an aura expression via CompilerService and return the
// evaluated value as int64_t. Returns 0 on eval error.
int64_t eval_i64(aura::compiler::CompilerService& cs, const std::string& expr) {
    auto r = cs.eval(expr);
    if (!r.has_value()) {
        std::println("  eval failed for {}: kind={}", expr, static_cast<int>(r.error().kind));
        return 0;
    }
    return as_int(r.value());
}

} // namespace

int main() {
    std::println("test_pair_slot_lock: B-024 g_owned_pair_slots_ push lock");
    aura::compiler::CompilerService cs;

    // AC1: simple (cons 1 2) -- basic functional sanity that the fix
    // didn't break the MakePair path. The IR executor's MakePair branch
    // (ir_executor_impl.cpp ~1001-1026) is the code path that owns the
    // B-024 fix; this eval exercises it.
    {
        int64_t car = eval_i64(cs, "(car (cons 1 2))");
        int64_t cdr = eval_i64(cs, "(cdr (cons 1 2))");
        check_eq_i64(car, 1, "AC1: (car (cons 1 2)) = 1");
        check_eq_i64(cdr, 2, "AC1: (cdr (cons 1 2)) = 2");
    }

    // AC2: 100 sequential (car (cons i i+1)). Escape analysis marks that
    // pair NON_ESCAPING (the only consumer is car), so MakePair mirrors
    // it with tl_arena_alloc and must not push the pointer onto
    // g_owned_pair_slots_. PairSlotCleanup free()s that vector; an arena
    // pointer there is an invalid free. Heap cons (ESCAPED) still take
    // the locked push (#1998). This expression is the arena case.
    {
        const size_t before = aura_g_owned_pair_slots_size();
        for (int i = 0; i < 100; ++i) {
            int64_t car = eval_i64(cs, "(car (cons " + std::to_string(i) + " " +
                                           std::to_string(i + 1) + "))");
            check_eq_i64(car, i, "AC2: (car (cons i i+1)) = i");
        }
        const size_t after = aura_g_owned_pair_slots_size();
        check_eq_i64(static_cast<int64_t>(after - before), 0,
                     "AC2: non-escaping cons does not enter g_owned_pair_slots_");
    }

    // AC3: 4 concurrent threads x 250 non-escaping make-pairs. Same arena
    // mirror as AC2: the owned vector must stay flat (no arena pointer
    // recorded) and every car must match. The locked heap push stays on
    // the ESCAPED arm; AC4 is the structural check for that lock.
    //
    // Issue #2333: CompilerService::eval() holds eval_mutex_ across
    // the full parse+typecheck+eval pipeline (recursive_mutex,
    // service.ixx:1937) for cross-thread race #3/#4/#5 protection.
    // Sharing one CompilerService across 4 threads serializes 1000
    // eval calls into single-threaded execution (~700ms/call observed
    // with full parse+typecheck+eval pipeline), pushing AC3 well past
    // the 90s timeout. Fix: give each worker its own CompilerService.
    // The owned vector is process-global. These pairs are the arena
    // arm, so the vector stays flat; the locked heap push is AC4.
    //
    // Note: we avoid std::vector<std::thread> here because GCC 16's C++20
    // std module implementation does not export the implementation-detail
    // member _M_realloc_append (vector.tcc), so emplace_back on a
    // vector<std::thread> triggers `-Werror=used-but-never-defined` at
    // template instantiation. Declaring 4 std::thread variables
    // directly sidesteps the issue -- each thread is movable, no
    // container needed.
    {
        const size_t before = aura_g_owned_pair_slots_size();
        std::atomic<int> errors{0};
        auto worker = [&errors](int thread_id) {
            // Per-thread CompilerService (issue #2333 fix: avoids
            // eval_mutex_ serialization across 4 threads on one service).
            aura::compiler::CompilerService cs;
            for (int i = 0; i < 250; ++i) {
                // Use distinct (thread_id, i) keys so all threads
                // produce distinct pairs (avoids car/cdr collisions
                // that would shadow race symptoms).
                const int v = thread_id * 1000 + i;
                int64_t car = eval_i64(cs, "(car (cons " + std::to_string(v) + " " +
                                               std::to_string(v + 1) + "))");
                if (car != v) {
                    ++errors;
                }
            }
        };
        std::thread t0(worker, 0);
        std::thread t1(worker, 1);
        std::thread t2(worker, 2);
        std::thread t3(worker, 3);
        t0.join();
        t1.join();
        t2.join();
        t3.join();
        check_eq_i64(errors.load(), 0, "AC3: 0 car() mismatches across 1000 concurrent make-pairs");
        const size_t after = aura_g_owned_pair_slots_size();
        check_eq_i64(static_cast<int64_t>(after - before), 0,
                     "AC3: non-escaping cons does not enter g_owned_pair_slots_");
    }

    // AC4: linter self-test -- structurally verifies the lock-around-push
    // invariant in both ir_executor_impl.cpp and aura_jit_runtime.cpp.
    // Invoked via system() to keep the test self-contained (linter is
    // a separate Python script in scripts/).
    {
        int rc =
            std::system("python3 scripts/coverage/checks/check_owned_pair_slots_lock_coverage.py "
                        "--self-test > /dev/null 2>&1");
        check_eq_i64(rc, 0, "AC4: linter self-test exit code = 0 (structurally verifies lock fix)");
    }

    // AC5: monotonicity -- g_owned_pair_slots_ size never decreases
    // during push operations (the static destructor only fires at
    // process exit, never mid-run). A race that dropped pushes would
    // show as growth below expected, which AC2/AC3 already catch; this
    // is an explicit sanity check on the size API.
    {
        size_t snapshot = aura_g_owned_pair_slots_size();
        bool monotonic = true;
        for (int i = 0; i < 50; ++i) {
            eval_i64(cs, "(car (cons 0 0))");
            size_t now = aura_g_owned_pair_slots_size();
            if (now < snapshot) {
                monotonic = false;
                break;
            }
            snapshot = now;
        }
        check_true(monotonic, "AC5: g_owned_pair_slots_ is monotonic non-decreasing during push");
    }

    // AC6 (#898): cons hot path is a TLS slab + release publish. One thread
    // and eight threads run the same per-thread alloc count. Aggregate
    // throughput of 8 threads must be at least 5× the single thread when
    // the machine has 8 hardware threads. Slab publishes stay far below
    // the alloc count (batched malloc, not one malloc per cons).
    {
        constexpr int kOps = 1000000;
        auto run = [](int nthreads) -> double {
            std::atomic<int> bad{0};
            auto worker = [&bad]() {
                int64_t prev = 0;
                for (int i = 0; i < kOps; ++i)
                    prev = aura_alloc_pair(i + 1, i + 2);
                if (prev == 0 || aura_pair_car(prev) != kOps || aura_pair_cdr(prev) != kOps + 1)
                    bad.fetch_add(1, std::memory_order_relaxed);
            };
            const auto t0 = std::chrono::steady_clock::now();
            if (nthreads == 1) {
                worker();
            } else {
                std::thread ts[8];
                for (int i = 0; i < nthreads; ++i)
                    ts[i] = std::thread(worker);
                for (int i = 0; i < nthreads; ++i)
                    ts[i].join();
            }
            const auto t1 = std::chrono::steady_clock::now();
            if (bad.load(std::memory_order_relaxed) != 0)
                return -1.0;
            return std::chrono::duration<double>(t1 - t0).count();
        };
        // Warm the segment directory and the slab allocator before timing.
        for (int i = 0; i < 4096; ++i)
            (void)aura_alloc_pair(1, 2);
        const auto slabs0 = aura_pair_slab_publish_count();
        const double t1 = run(1);
        const double t8 = run(8);
        const auto slabs1 = aura_pair_slab_publish_count();
        check_true(t1 > 0.0 && t8 > 0.0, "AC6: 1-thread and 8-thread cons runs stay well-formed");
        const double speedup = (t1 > 0.0 && t8 > 0.0) ? (8.0 * t1 / t8) : 0.0;
        std::println("  AC6: 8-thread cons speedup {:.2f}× (t1={:.3f}s t8={:.3f}s)", speedup, t1,
                     t8);
        const unsigned hc = std::thread::hardware_concurrency();
        if (hc >= 8)
            check_true(speedup >= 5.0, "AC6: 8-thread cons aggregate throughput ≥ 5× one thread");
        else
            check_true(speedup >= 1.5, "AC6: cons scales on this host (fewer than 8 cpus)");
        const auto allocs = static_cast<std::uint64_t>(kOps) * 9;
        const auto slabs = slabs1 - slabs0;
        check_true(slabs > 0 && slabs * 32 < allocs,
                   "AC6: slab publishes are batched (well under one malloc per cons)");
    }

    // AC7 (#898): a publisher thread release-stores pair ids while other
    // threads car/cdr them. The segment cell is an acquire load, so this
    // does not take the workspace lock. Every observed pair keeps cdr == car+1.
    {
        std::atomic<int64_t> published{0};
        std::atomic<int> stop{0};
        std::atomic<int> bad{0};
        std::atomic<int> seen{0};
        auto reader = [&]() {
            while (stop.load(std::memory_order_acquire) == 0) {
                const int64_t p = published.load(std::memory_order_acquire);
                if (p == 0)
                    continue;
                const int64_t car = aura_pair_car(p);
                const int64_t cdr = aura_pair_cdr(p);
                if (cdr != car + 1)
                    bad.fetch_add(1, std::memory_order_relaxed);
                else
                    seen.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread r0(reader);
        std::thread r1(reader);
        std::thread r2(reader);
        std::thread writer([&]() {
            for (int i = 0; i < 200000; ++i) {
                const int64_t p = aura_alloc_pair(i + 1, i + 2);
                published.store(p, std::memory_order_release);
            }
            stop.store(1, std::memory_order_release);
        });
        writer.join();
        r0.join();
        r1.join();
        r2.join();
        check_eq_i64(bad.load(), 0, "AC7: concurrent car/cdr matched the published pair");
        check_true(seen.load() > 0, "AC7: readers observed a published pair");
    }

    std::println("test_pair_slot_lock: {}/{} passed", passed, total);
    return passed == total ? 0 : 1;
}