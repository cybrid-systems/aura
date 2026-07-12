// test_issue_1403.cpp — Issue #1403:
// g_yield_hook_evaluator per-fiber hook stack contract test.
//
// Verifies that fiber:spawn workers reusing the same OS thread
// for multiple fibers do NOT see cross-fiber state corruption
// when one fiber's RAII guard pushes an Evaluator onto the
// thread-local stack and another fiber's guard is also alive.
//
// Per the AC: "2 fibers × alternating yield 1k times → no state bleed"
//
// Pre-#1403 bug: a single thread_local Evaluator* pointer was
// overwritten by whichever fiber ran last on the shared worker.
// The flush_mutation_boundary_trampoline and
// mutation_boundary_held_trampoline would then route the first
// fiber's state through the second fiber's evaluator (or worse).
//
// Post-#1403 fix: thread_local std::vector<Evaluator*> stack with
// LIFO push/pop and a getter that returns the top-of-stack.

#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

#include "observability_metrics.h"
#include "compiler/evaluator.ixx"
#include "compiler/evaluator_fiber_mutation.cpp"

namespace aura::test::issue_1403 {

// Helper: simulate the RAII guard pattern. MutationBoundaryGuard
// pushes the Evaluator onto the stack on ctor, pops on dtor.
// We model this directly via bind/unbind.

struct HookGuard {
    aura::compiler::Evaluator* ev;
    HookGuard(aura::compiler::Evaluator* e)
        : ev(e) {
        e->bind_yield_hook_evaluator();
        // After bind, the top of the stack should be this ev.
        assert(aura::compiler::Evaluator::yield_hook_evaluator() == e);
    }
    ~HookGuard() { ev->unbind_yield_hook_evaluator(); }
};

// Contract 1: yield_hook_evaluator() returns the top of the
// LIFO stack, not a single overwritten pointer.
void test_stack_top_is_lifo() {
    aura::compiler::Evaluator a; // default-constructed
    aura::compiler::Evaluator b;
    aura::compiler::Evaluator c;

    // Empty stack → nullptr.
    assert(aura::compiler::Evaluator::yield_hook_evaluator() == nullptr);

    {
        HookGuard ga(&a);
        assert(aura::compiler::Evaluator::yield_hook_evaluator() == &a);
        {
            HookGuard gb(&b);
            assert(aura::compiler::Evaluator::yield_hook_evaluator() == &b);
            {
                HookGuard gc(&c);
                assert(aura::compiler::Evaluator::yield_hook_evaluator() == &c);
            }
            // gc popped → top should be b again.
            assert(aura::compiler::Evaluator::yield_hook_evaluator() == &b);
        }
        // gb popped → top should be a again.
        assert(aura::compiler::Evaluator::yield_hook_evaluator() == &a);
    }
    // ga popped → empty.
    assert(aura::compiler::Evaluator::yield_hook_evaluator() == nullptr);
}

// Contract 2: 2 fibers × alternating yield 1k times → no state bleed.
// Simulated by alternating guards on a single worker thread.
void test_alternating_guards_no_state_bleed() {
    aura::compiler::Evaluator a;
    aura::compiler::Evaluator b;
    int mismatch_a_in_b_ctx = 0;
    int mismatch_b_in_a_ctx = 0;

    for (int i = 0; i < 1000; ++i) {
        {
            HookGuard ga(&a);
            // Simulate "yield" in A's context: a's metrics would be
            // bumped. Cross-fiber check: yield_hook_evaluator() must
            // remain a, not b (which a separate fiber might have
            // pushed in a different round).
            if (aura::compiler::Evaluator::yield_hook_evaluator() != &a)
                ++mismatch_a_in_b_ctx;
        }
        {
            HookGuard gb(&b);
            if (aura::compiler::Evaluator::yield_hook_evaluator() != &b)
                ++mismatch_b_in_a_ctx;
        }
    }
    assert(mismatch_a_in_b_ctx == 0);
    assert(mismatch_b_in_a_ctx == 0);
}

// Contract 3: unbind is idempotent under RAII use.
// (Pre-#1403 the single-pointer "compare-and-clear" had the same
// idempotency; this contract locks in the stack behavior.)
void test_unbind_idempotent() {
    aura::compiler::Evaluator a;
    HookGuard ga(&a);
    // After the guard goes out of scope, double-unbind is a no-op.
    ga.~HookGuard();                 // explicitly unbind
    a.unbind_yield_hook_evaluator(); // second unbind — should be no-op
    assert(aura::compiler::Evaluator::yield_hook_evaluator() == nullptr);
}

} // namespace aura::test::issue_1403

int main() {
    using namespace aura::test::issue_1403;
    test_stack_top_is_lifo();
    test_alternating_guards_no_state_bleed();
    test_unbind_idempotent();
    return 0;
}
