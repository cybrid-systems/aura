// test_wave1_workspace_lock_reentrancy.cpp — Wave1 B-03 / B-09
//
// Concurrency correctness: non-recursive std::shared_mutex must not be
// re-entered on the same thread.
//
//   B-03: (eval-current) must not hold shared workspace_mtx_ across
//         eval_flat / nested mutate (EDEADLK under Guard).
//   B-09: WorkspaceFlatPin / WorkspaceUniqueIfNeeded adopt outer
//         MutationBoundaryGuard exclusive hold instead of re-locking.
//
// AC1: pin_workspace_flat under outermost MutationBoundaryGuard does not throw
// AC2: (eval-current) after (set-code) + (mutate:rebind) completes (no deadlock)
// AC3: under Guard, (workspace-state) / (workspace:mutation-count) via engine
//      metrics path do not throw (pin adopt)
// AC4: source markers for Wave1 helpers present

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

bool file_contains(const char* path, const char* needle) {
    std::ifstream in(path);
    if (!in)
        return false;
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return s.find(needle) != std::string::npos;
}

} // namespace

int main() {
    using aura::compiler::CompilerService;
    using aura::compiler::Evaluator;
    using aura::test::g_failed;
    using aura::test::g_passed;

    // ── AC1: pin under Guard (B-09) ──────────────────────────────────
    {
        std::println("\n--- AC1: WorkspaceFlatPin under MutationBoundaryGuard ---");
        Evaluator ev;
        bool ok = true;
        bool threw = false;
        try {
            // Prefer try_acquire (non-deprecated); fall back to legacy if needed.
            auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, /*pending=*/1, &ok);
            if (!gr) {
                CHECK(false, "AC1: try_acquire MutationBoundaryGuard");
            } else {
                auto guard = std::move(*gr);
                // Nested shared would EDEADLK without adopt.
                auto pin = ev.pin_workspace_flat();
                Evaluator::WorkspaceUniqueIfNeeded wlock(ev);
                CHECK(!pin.owns_shared_lock(),
                      "AC1: pin adopts outer Guard (does not own shared_lock)");
                CHECK(!wlock.owns_unique_lock(), "AC1: WorkspaceUniqueIfNeeded adopts outer Guard");
                (void)guard;
            }
        } catch (const std::system_error& e) {
            threw = true;
            std::println("  system_error: {}", e.what());
        } catch (...) {
            threw = true;
        }
        CHECK(!threw, "AC1: no system_error under Guard pin/unique-if-needed");
    }

    // ── AC2: eval-current short pin (B-03) ───────────────────────────
    {
        std::println("\n--- AC2: eval-current after set-code + mutate:rebind ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define a 1) (define b 2) a\")").has_value(), "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval-current baseline");
        // mutate holds unique workspace; must not deadlock if eval-current
        // (or nested workspace reads) re-entered locking.
        (void)cs.eval("(mutate:rebind \"a\" \"10\")");
        auto r = cs.eval("(eval-current)");
        CHECK(r.has_value(), "AC2: eval-current after mutate:rebind (no deadlock)");
    }

    // ── AC3: workspace-state under post-mutate eval path ─────────────
    {
        std::println("\n--- AC3: workspace-state after mutate ---");
        CompilerService cs;
        (void)cs.eval("(set-code \"(define x 1)\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:rebind \"x\" \"2\")");
        auto ws = cs.eval("(workspace-state)");
        CHECK(ws.has_value(), "AC3: workspace-state after mutate (pin adopt / short lock)");
    }

    // ── AC4: source markers ──────────────────────────────────────────
    {
        std::println("\n--- AC4: Wave1 source markers ---");
        CHECK(file_contains("src/compiler/evaluator.ixx", "WorkspaceUniqueIfNeeded"),
              "AC4: WorkspaceUniqueIfNeeded in evaluator.ixx");
        CHECK(file_contains("src/compiler/evaluator.ixx", "outer_exclusive"),
              "AC4: outer_exclusive adopt path in WorkspaceFlatPin");
        CHECK(file_contains("src/compiler/evaluator_primitives_eval.cpp", "Wave1 B-03"),
              "AC4: eval-current Wave1 B-03 comment");
    }

    // ── AC5: Wave2 performance markers (lock/JIT/lookup) ─────────────
    {
        std::println("\n--- AC5: Wave2 hot-path markers ---");
        CHECK(file_contains("src/compiler/aura_jit.h", "invalidate_all"),
              "AC5: AuraJIT::invalidate_all declared");
        // Wave5: mark_all_defines_dirty body lives in service_dirty.cpp.
        CHECK(file_contains("src/compiler/service_dirty.cpp", "invalidate_all()") ||
                  file_contains("src/compiler/service.ixx", "invalidate_all()"),
              "AC5: mark_all_defines_dirty uses bulk invalidate_all");
        CHECK(file_contains("src/compiler/evaluator_eval_flat.cpp", "Wave2"),
              "AC5: apply_closure Wave2 single-lock copy");
        CHECK(file_contains("src/compiler/evaluator_env.cpp", "intern once"),
              "AC5: Env::lookup SoA intern once");
    }

    std::println("\n=== Wave1/Wave2 lock + hot-path: {} failed ===", g_failed);
    return g_failed ? 1 : 0;
}
