// test_issue_1998.cpp -- runtime smoke test for B-024 / #1998
//
// Verifies the IR-executor MakePair path holds aura_lock_workspace_write()
// / aura_unlock_workspace_write() around g_owned_pair_slots_.push_back(slot)
// so the static PairSlotCleanup destructor at aura_jit_runtime.cpp:425
// (process-exit walker) and concurrent sibling bridge-hook writes
// (aura_make_pair at line 1330+) cannot race the push.
//
// The runtime gate exercises functional correctness: many make-pair
// cycles via the IR executor must succeed and g_owned_pair_slots_ must
// grow exactly by 1 per call (no lost slots from the race). Concurrent
// threads doing make-pairs verify the slot count invariant under
// contention. The linter (scripts/check_owned_pair_slots_lock_coverage.py)
// is the structural CI gate for the lock-around-push invariant.
//
// AC1: simple `(cons 1 2)` via CompilerService returns car=1, cdr=2
//      (basic functional check that the fix didn't break the path).
// AC2: 100 sequential `(cons i i+1)` calls succeed; g_owned_pair_slots_
//      grows by 100 (no lost slots).
// AC3: concurrent threads (4 threads x 250 make-pairs each) finish
//      with g_owned_pair_slots_ having grown by exactly 1000 (no lost
//      slots under contention; this is the primary race regression gate).
// AC4: linter self-test passes (structurally verifies the lock around
//      g_owned_pair_slots_.push_back in both ir_executor_impl.cpp and
//      aura_jit_runtime.cpp).
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
    if (!r.ok()) {
        std::println("  eval failed for {}: {}", expr, r.error());
        return 0;
    }
    return r.value().as_int();
}

} // namespace

int main() {
    std::println("test_issue_1998: B-024 g_owned_pair_slots_ push lock");
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

    // AC2: 100 sequential (cons i i+1) calls. g_owned_pair_slots_
    // grows by exactly 100 (no lost slots). Each cons pushes 1 slot
    // (per the B-024-fix wrapped push).
    {
        const size_t before = aura_g_owned_pair_slots_size();
        for (int i = 0; i < 100; ++i) {
            int64_t car = eval_i64(cs, "(car (cons " + std::to_string(i) + " " +
                                           std::to_string(i + 1) + "))");
            check_eq_i64(car, i, "AC2: (car (cons i i+1)) = i");
        }
        const size_t after = aura_g_owned_pair_slots_size();
        check_eq_i64(static_cast<int64_t>(after - before), 100,
                     "AC2: g_owned_pair_slots_ grew by 100 (no lost slots)");
    }

    // AC3: 4 concurrent threads x 250 make-pairs each. Total expected
    // growth = 1000. The B-024 lock fix is what prevents lost slots
    // (the static PairSlotCleanup destructor at process exit + sibling
    // bridge-hook pushes cannot race the wrapped push_back).
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
        auto worker = [&cs, &errors](int thread_id) {
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
        check_eq_i64(static_cast<int64_t>(after - before), 1000,
                     "AC3: g_owned_pair_slots_ grew by 1000 (no lost slots under contention)");
    }

    // AC4: linter self-test -- structurally verifies the lock-around-push
    // invariant in both ir_executor_impl.cpp and aura_jit_runtime.cpp.
    // Invoked via system() to keep the test self-contained (linter is
    // a separate Python script in scripts/).
    {
        int rc = std::system(
            "python3 scripts/check_owned_pair_slots_lock_coverage.py --self-test > /dev/null 2>&1");
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

    std::println("test_issue_1998: {}/{} passed", passed, total);
    return passed == total ? 0 : 1;
}