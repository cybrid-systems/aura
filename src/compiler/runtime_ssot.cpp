// runtime_ssot.cpp — SSOT definitions for shared runtime globals used by
// both libaura_test_objects.so (evaluator / IR) and the light/full JIT SOs.
//
// Same class of bug as g_tl_arena (see tl_arena_tls.cpp): when the
// definition lives only in aura_jit_runtime.cpp (JIT SOs), aura_test_objects
// has an undefined data symbol that x86_64 glibc refuses to resolve from a
// later-loaded DSO at load time:
//   symbol lookup error: libaura_test_objects.so: undefined symbol: g_pair_slots
// (aarch64 often still resolved via the executable NEEDED list).
//
// Declarations live in runtime_shared.h. This TU is compiled into
// libaura_tl_arena.so (runtime SSOT shared object; name kept for DT_NEEDED
// stability) so aura_test_objects NEEDED-links the provider first.

#include "runtime_shared.h"

#include <cstdlib>
#include <vector>

// ── Pair storage (JIT runtime + evaluator car/cdr fallback) ──
std::vector<PairSlot*> g_pair_slots;

// Heap-owned pair slots (subset of g_pair_slots allocated via malloc).
// Process-exit cleanup frees them; arena-owned slots are not listed here.
std::vector<PairSlot*> g_owned_pair_slots_;

// Arena allocation flag (CLI --no-arena / runtime bridges).
bool g_use_arena = true;

// Flat hash table index space (Phase 4c).
std::vector<FlatHashTable*> g_hash_tables;

// Issue #1998 / B-024: C accessor so tests need not name std::vector.
extern "C" size_t aura_g_owned_pair_slots_size() {
    return g_owned_pair_slots_.size();
}

namespace {
struct PairSlotCleanup {
    ~PairSlotCleanup() {
        for (auto* p : g_owned_pair_slots_) {
            std::free(p);
        }
        g_owned_pair_slots_.clear();
    }
};
[[maybe_unused]] PairSlotCleanup g_pair_slot_cleanup;
} // namespace
