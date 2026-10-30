// fiber_rt_defaults.cpp - Issue #4774: the injectable-hook defaults that let
// the serve Fiber / Scheduler runtime link WITHOUT the Evaluator or the JIT.
//
// src/serve/fiber.cpp and src/serve/scheduler.cpp reach the evaluator side
// only through C-ABI hooks. Almost all of them already have weak no-op
// defaults in src/compiler/fiber_bridge.cpp; the two SETTERS below were the
// remaining strong-only symbols (defined in runtime_ssot.cpp /
// aura_jit_bridge.cpp), so an embedder linking only the serve objects failed
// with two undefined references.
//
// This TU is compiled ONLY into aura_fiber_rt (aura::fiber_rt). It is NOT in
// aura_serve_fiber_objects: those objects are linked straight into
// executables whose strong definitions live in shared libraries, and an
// executable-local weak definition would interpose the .so's strong one at
// run time. In the static aura::fiber_rt archive the defaults are weak as
// well, so a host that also links the real hook owners still gets theirs.
//
// The hook values are kept (not dropped) so an embedder can read them back
// through the getters and wire its own runtime:
//   aura_fiber_rt_current_fiber_id_fn()       - fiber id provider fiber.cpp installs
//   aura_fiber_rt_long_mutation_hook()        - scheduler long-hold callback

#include <atomic>
#include <cstdint>

extern "C" {

using aura_fiber_rt_fiber_id_fn = std::uint64_t (*)();
using aura_fiber_rt_long_mutation_fn = void (*)(std::uint64_t fiber_id, std::uint64_t duration_us);

namespace {
    std::atomic<aura_fiber_rt_fiber_id_fn> g_fiber_rt_fiber_id_fn{nullptr};
    std::atomic<aura_fiber_rt_long_mutation_fn> g_fiber_rt_long_mutation_fn{nullptr};
} // namespace

__attribute__((weak)) void aura_set_current_fiber_id_fn(aura_fiber_rt_fiber_id_fn fn) {
    g_fiber_rt_fiber_id_fn.store(fn, std::memory_order_release);
}

__attribute__((weak)) void
aura_set_long_mutation_scheduler_hook(aura_fiber_rt_long_mutation_fn fn) {
    g_fiber_rt_long_mutation_fn.store(fn, std::memory_order_release);
}

aura_fiber_rt_fiber_id_fn aura_fiber_rt_current_fiber_id_fn() noexcept {
    return g_fiber_rt_fiber_id_fn.load(std::memory_order_acquire);
}

aura_fiber_rt_long_mutation_fn aura_fiber_rt_long_mutation_hook() noexcept {
    return g_fiber_rt_long_mutation_fn.load(std::memory_order_acquire);
}

} // extern "C"
