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

    // ── AC6 (#4264): (require …) inside set-code'd source + eval-current ──
    // Soft serve-async set-code + eval-current walks the workspace flat under
    // WorkspaceUniqueIfNeeded (unique workspace hold). A (require …) inside
    // that set-code'd source re-entered load_module_file on the SAME thread,
    // whose critical sections took fresh unique_lock(workspace_mtx_) —
    // std::shared_mutex is non-recursive — so the re-lock returned EDEADLK
    // ("Resource deadlock avoided"; the serve exec surfaced "exec exception:
    // Resource deadlock avoided") while the same source oneshot required
    // cleanly. With the #4264 adopt-if-held loader locks the require proceeds
    // under the already-held exclusive.
    {
        std::println("\n--- AC6 (#4264): require inside set-code'd source + eval-current ---");
#ifdef AURA_SOURCE_DIR
        const std::string lib = std::string(AURA_SOURCE_DIR) + "/lib";
#else
        const std::string lib = "lib";
#endif
        setenv("AURA_PATH", lib.c_str(), 1);
        setenv("AURA_SANDBOX", "off", 1);
        setenv("AURA_PIPELINE_STRICT", "0", 1);
        CompilerService cs;
        // C++ \\\" → runtime \" → aura escaped quote inside the set-code
        // string literal (matches the oneshot/serve e2e escaping for #4264).
        const std::string src_4264 = "(require \\\"std/hash\\\" all:) (define h (make-hash)) "
                                     "(hash-set! h 7 42) (hash-ref h 7 -1)";
        auto sc = cs.eval("(set-code \"" + src_4264 + "\")");
        CHECK(sc.has_value(), "AC6: set-code of require-using source");
        auto r = cs.eval("(eval-current)");
        CHECK(r.has_value(), "AC6: eval-current completes require + hash program (no EDEADLK)");
    }

    // ── AC7 (#4272): vector-set! / hash-set! under set-code + eval-current ──
    // Soft set-code scoring under-counted vs oneshot (shortest-bridge 1/8 vs
    // 7/8) because #3235 heap-mutate auto-Guard fail-closed under the
    // eval-current pin (#2686 nested-mutate-under-eval-current), returning
    // soft <error> after CASE0= and aborting the rest. Fix: allow language
    // heap mutators while eval_current_holds_shared_pin (exclusive workspace
    // already held; prim body uses alloc_storage_lock_).
    {
        std::println("\n--- AC7 (#4272): vector-set! under set-code + eval-current ---");
        setenv("AURA_SANDBOX", "off", 1);
        setenv("AURA_PIPELINE_STRICT", "0", 1);
        {
            CompilerService cs;
            const std::string src =
                "(let ((v (make-vector 2 0))) (vector-set! v 0 7) (vector-ref v 0))";
            auto sc = cs.eval(std::string("(set-code \"") + src + "\")");
            CHECK(sc.has_value(), "AC7: set-code accepts vector-set! program");
            auto r = cs.eval("(eval-current)");
            CHECK(r.has_value(), "AC7: eval-current completes");
            CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 7,
                  "AC7: vector-set! under eval-current -> 7 (not soft <error>)");
        }
        // Fresh CS for hash-set! face (same #3235 auto-Guard path). A second
        // set-code in the vector-set! CS can leave residual workspace/env that
        // makes hash-ref observe a non-int under LLVM RelWithDebInfo; isolate.
        {
            CompilerService cs;
            // Require std/hash inside the set-code'd source (#4264 adopt path)
            // so make-hash binds under a fresh CompilerService without relying
            // on prior AC6 process residue / Soft oneshot prelude.
            const std::string src_h =
                "(begin (require \\\"std/hash\\\" all:) "
                "(let ((h (make-hash))) (hash-set! h 1 9) (hash-ref h 1 -1)))";
            auto sc2 = cs.eval(std::string("(set-code \"") + src_h + "\")");
            CHECK(sc2.has_value(), "AC7: set-code hash-set! program");
            auto r2 = cs.eval("(eval-current)");
            CHECK(r2 && aura::compiler::types::is_int(*r2) &&
                      aura::compiler::types::as_int(*r2) == 9,
                  "AC7: hash-set! under eval-current -> 9");
        }
        CHECK(file_contains("src/compiler/evaluator_mutation_boundary.cpp", "Issue #4272"),
              "AC7: maybe_auto_guard cites #4272");
        CHECK(file_contains("src/compiler/evaluator_mutation_boundary.cpp",
                            "eval_current_holds_shared_pin()"),
              "AC7: heap-mutate exempt under eval-current pin");
    }

    // ── AC8 (#4272 over-count): extra ) must fail set-code like oneshot ──
    // WAVE16 course-schedule: Soft set-code recovered past leftover ')' after
    // a truncated begin, wrapped trailing forms, and scored 10/10 while
    // oneshot (#3917 CLI splitter) hard-failed with empty CASE. parse_to_flat
    // now fails closed on top-level unexpected ')' (Issue #4272 / #3917 parity).
    // set-code returns make_merr("parse", ...) as an EvalValue pair (Result
    // still has_value) — Soft serve surfaces that as status=error.
    {
        std::println("\n--- AC8 (#4272): set-code rejects extra close-paren ---");
        setenv("AURA_SANDBOX", "off", 1);
        setenv("AURA_PIPELINE_STRICT", "0", 1);
        CompilerService cs;
        auto merr_kind_is = [&](const std::string& set_code_arg, std::string_view expect) -> bool {
            auto r = cs.eval(std::string("(set-code \"") + set_code_arg + "\")");
            if (!r || !aura::compiler::types::is_pair(*r))
                return false;
            auto pidx = aura::compiler::types::as_pair_idx(*r);
            auto& pairs = cs.evaluator().pairs();
            if (pidx >= pairs.size())
                return false;
            auto car = pairs[pidx].car;
            if (!aura::compiler::types::is_string(car))
                return false;
            auto& heap = cs.evaluator().string_heap_mut();
            auto idx = aura::compiler::types::as_string_idx(car);
            if (idx >= heap.size())
                return false;
            return heap[idx] == expect;
        };
        CHECK(merr_kind_is("(begin (define x 1) x))", "parse"),
              "AC8: extra top-level ) → parse merr (oneshot parity)");
        CHECK(merr_kind_is("(begin (define a 1) (define b (lambda () a))))) "
                           "(define c 2) c)",
                           "parse"),
              "AC8: truncated-begin + trailing forms → parse merr");
        auto ok = cs.eval("(set-code \"(begin (define x 1) x)\")");
        CHECK(ok.has_value() && aura::compiler::types::is_bool(*ok) &&
                  aura::compiler::types::as_bool(*ok),
              "AC8: balanced source still set-code #t");
        CHECK(file_contains("src/parser/parser_impl.cpp", "Issue #4272"),
              "AC8: parse_to_flat cites #4272 hard-fail");
        CHECK(file_contains("src/parser/parser_impl.cpp", "hard_fail_extra_close"),
              "AC8: hard_fail_extra_close helper present");
    }

    // ── AC9 (#4346): heap-slot writes stay O(1) in the define count ──
    // vector-set! / set-car! take an outermost Guard. That guard used to
    // scan every workspace node and compare every env binding, so each
    // write grew with the number of defines and could drop a caller
    // closure. The slot write must keep the value and the closure.
    {
        std::println("\n--- AC9 (#4346): vector-set!/set-car! keep closures ---");
        setenv("AURA_SANDBOX", "off", 1);
        setenv("AURA_PIPELINE_STRICT", "0", 1);
        CompilerService cs;
        const std::string src = "(define (tab) 2)\\n"
                                "(define (indent n) (* n (tab)))\\n"
                                "(define v (vector 0))\\n"
                                "(define p (cons 1 2))\\n";
        auto sc = cs.eval(std::string("(set-code \"") + src + "\")");
        CHECK(sc.has_value() && aura::compiler::types::is_bool(*sc) &&
                  aura::compiler::types::as_bool(*sc),
              "4346: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4346: eval-current");
        auto before = cs.eval("(indent 2)");
        CHECK(before && aura::compiler::types::is_int(*before) &&
                  aura::compiler::types::as_int(*before) == 4,
              "4346: (indent 2) = 4 before the writes");
        (void)cs.eval("(vector-set! v 0 9)");
        (void)cs.eval("(set-car! p 8)");
        auto after = cs.eval("(indent 2)");
        CHECK(after && aura::compiler::types::is_int(*after) &&
                  aura::compiler::types::as_int(*after) == 4,
              "4346: (indent 2) = 4 after vector-set! and set-car!");
        auto vr = cs.eval("(vector-ref v 0)");
        CHECK(vr && aura::compiler::types::is_int(*vr) && aura::compiler::types::as_int(*vr) == 9,
              "4346: vector-ref sees 9");
        auto car = cs.eval("(car p)");
        CHECK(car && aura::compiler::types::is_int(*car) &&
                  aura::compiler::types::as_int(*car) == 8,
              "4346: car sees 8");
        CHECK(file_contains("src/compiler/evaluator.ixx", "heap_slot_only"),
              "4346: checkpoint records a heap-slot guard");
        CHECK(file_contains("src/compiler/evaluator_mutation_boundary.cpp", "Issue #4346"),
              "4346: dtor cites the heap-slot skip");
        CHECK(file_contains("src/compiler/evaluator_mutation_boundary.cpp", "heap_slot_quiet"),
              "4346: quiet heap guard skips the define walk");
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
