// mutation_guard_helpers.hh — Issue #1950 / #1931
// Shared header exposing run_under_mutation_guard template to all
// evaluator_primitives_*.cpp files. Previously the template was
// defined in evaluator_primitives_compile.cpp's anonymous namespace,
// which prevented other primitives files (e.g. mutate:* at
// evaluator_primitives_mutate.cpp) from using the same Guard wrap
// pattern. Issue #1950 / #1931 AC requires 100% Guard wrap on
// compile:* + mutate:* primitives — this header makes the helper
// available to all of them.
//
// AC: "所有 compile:* / mutate:* primitives 100% 包装 Guard +
// StableNodeRef 验证". The Guard dtor (evaluator.ixx,
// batched to ≤6 atomics per #1747 / #1931) owns the defuse_version_ +
// total_mutations_ bump — primitives intentionally removed their
// manual #1904 path. This header keeps the wrap pattern uniform.
// Metrics: mutation_guard_exception_total +
// compile_primitive_stale_ir_prevented_total (#1931 AC).
//
// Include AFTER `module aura.compiler.evaluator;` (+ value imports) so
// Evaluator / EvalValue / CompilerMetrics are in scope. Do not pull
// non-existent compiler/evaluator.h from the global module fragment.

#ifndef AURA_COMPILER_MUTATION_GUARD_HELPERS_HH
#define AURA_COMPILER_MUTATION_GUARD_HELPERS_HH

namespace aura::compiler {

// run_under_mutation_guard — RAII helper wrapping a mutation body
// in a MutationBoundaryGuard. On Guard reject, std::exception, or
// ..., bumps the relevant metrics + returns `on_fail`.
//
// `track_env_compact_violation`: when true, additionally bumps
// mutation_boundary_violation_on_env_compact_total (Issue #1948
// / #1949 path). Default false (most primitives are not env-compact).
template <typename F>
EvalValue run_under_mutation_guard(Evaluator& ev, F&& body,
                                   EvalValue on_fail = types::make_bool(false),
                                   bool track_env_compact_violation = false) {
    bool guard_ok = true;
    auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, /*pending=*/1, &guard_ok);
    if (!gr) {
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->compile_primitive_stale_ir_prevented_total.fetch_add(1, std::memory_order_relaxed);
            if (track_env_compact_violation)
                m->mutation_boundary_violation_on_env_compact_total.fetch_add(
                    1, std::memory_order_relaxed);
        }
        return on_fail;
    }
    if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
        m->compile_primitive_guard_captures_total.fetch_add(1, std::memory_order_relaxed);
    try {
        return std::forward<F>(body)();
    } catch (const std::exception&) {
        guard_ok = false;
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->mutation_guard_exception_total.fetch_add(1, std::memory_order_relaxed);
            m->eda_guard_exception_handled_total.fetch_add(1, std::memory_order_relaxed);
            m->compile_primitive_stale_ir_prevented_total.fetch_add(1, std::memory_order_relaxed);
            if (track_env_compact_violation)
                m->mutation_boundary_violation_on_env_compact_total.fetch_add(
                    1, std::memory_order_relaxed);
        }
        return on_fail;
    } catch (...) {
        // [SILENCE-PRIM-#615] Guard-path uncaught → on_fail + metrics; dtor restores.
        guard_ok = false;
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->mutation_guard_exception_total.fetch_add(1, std::memory_order_relaxed);
            m->eda_guard_uncaught_exception_total.fetch_add(1, std::memory_order_relaxed);
            m->compile_primitive_stale_ir_prevented_total.fetch_add(1, std::memory_order_relaxed);
            if (track_env_compact_violation)
                m->mutation_boundary_violation_on_env_compact_total.fetch_add(
                    1, std::memory_order_relaxed);
        }
        return on_fail;
    }
}

} // namespace aura::compiler

#endif // AURA_COMPILER_MUTATION_GUARD_HELPERS_HH
