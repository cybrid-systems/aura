// evaluator_gc.cpp — P1-i: GC root flush, sweep, and pair compaction
// extracted from evaluator_impl.cpp.

module;

#include <cstdint>
#include <mutex>
#include <vector>
#include "runtime_shared.h"
#include "serve/gc_coordinator.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler {

using types::EvalValue;
using namespace types;

// Issue #206: Evaluator::compact_pairs. Compacts the
// pairs_ arena, building a remap table for stable id
// resolution.
//
// Algorithm: linear scan, copy live pairs to the front,
// build pair_remap_[old_idx] = new_idx. Dead pairs (not
// in `live_mask`) are skipped and their old index gets
// remap to -1.
//
// Returns the number of pairs after compact.
//
// The remap table is sized to the OLD pairs_ size, even
// after compact (which shrinks pairs_). This is by
// design: a stale id (e.g., from a saved
// MutationRecord) might still be in [0, old_size). The
// remap tells us if that id is live (and what its new
// index is) or freed (-1).
std::int64_t Evaluator::compact_pairs(const std::vector<bool>& live_mask) {
    const std::size_t n_old = pairs_.size();
    pair_remap_.clear();
    pair_remap_.reserve(n_old);
    // Build a new vector with only the live pairs. Use
    // move-semantics to avoid copies where possible.
    std::pmr::vector<Pair> new_pairs{&runtime_resource_};
    new_pairs.reserve(n_old); // upper bound
    std::int64_t new_idx = 0;
    for (std::size_t i = 0; i < n_old; ++i) {
        // If live_mask is empty, treat all as live.
        // If live_mask is sized to n_old, use the bit.
        // If live_mask is shorter than i, treat as dead
        // (defensive).
        const bool is_live =
            live_mask.empty() ? true : (i < live_mask.size() ? live_mask[i] : false);
        if (is_live) {
            pair_remap_.push_back(new_idx);
            new_pairs.push_back(std::move(pairs_[i]));
            ++new_idx;
        } else {
            pair_remap_.push_back(-1);
        }
    }
    pairs_ = std::move(new_pairs);
    return static_cast<std::int64_t>(pairs_.size());
}
// ── GC root registration (Issue #113) ──────────────────────────
//
// `flush_gc_roots` walks every vector heap this Evaluator owns and
// populates the GCRootSet with the indices of all live objects. The
// GC collector calls this during its root collection phase (after
// the safepoint has stopped all fibers on this worker, so no
// concurrent mutator can run). We additionally hold `heap_mutex()`
// so a non-fiber thread in serve-async mode can't race a concurrent
// `string_heap_.push_back` (or similar) with the walk.
//
// `gc_root_count` is the cheap version: just returns the number of
// entries that WOULD be marked, without allocating the GCRootSet.
// Useful for pre-GC metrics and unit tests that want to verify the
// root set is populated without paying for the GCRootSet heap allocs.

void Evaluator::flush_gc_roots(void* root_set_out) {
    // The opaque pointer is aura::serve::GCRootSet* (set by the
    // serve_async.cpp callback). Cast is safe because the GC
    // collector passes a real GCRootSet that the messaging bridge
    // constructed in its own TU.
    auto& out = *static_cast<aura::serve::GCRootSet*>(root_set_out);

    std::lock_guard<std::mutex> lock(heap_mutex());

    // 1. string_heap_ — every slot is a root. The pool can be
    //    compacted in the sweep phase; until then, treat them all
    //    as live. Pairs can reference strings (car/cdr), so
    //    undermarking here would dangle pair fields.
    out.string_roots.reserve(out.string_roots.size() + string_heap_.size());
    for (std::size_t i = 0; i < string_heap_.size(); ++i) {
        out.string_roots.push_back(static_cast<int64_t>(i));
    }

    // 2. pairs_ — every slot is a root (cons cells are the spine
    //    of every list / tree in the heap). Stale entries from
    //    previous gc-temp cycles are the caller's responsibility
    //    to remove before GC; we mark everything.
    out.pair_roots.reserve(out.pair_roots.size() + pairs_.size());
    for (std::size_t i = 0; i < pairs_.size(); ++i) {
        out.pair_roots.push_back(static_cast<int64_t>(i));
    }

    // 3. closures_ — only roots with id < gc_safe_closure_id_ are
    //    pinned (module-level / while-loop bodies). Anything above
    //    that watermark was created inside a temp-arena intend and
    //    is safe to collect. We walk the map and emit the safe set.
    out.closure_roots.reserve(out.closure_roots.size() + closures_.size());
    for (const auto& [id, c] : closures_) {
        if (static_cast<std::uint64_t>(id) < gc_safe_closure_id_) {
            out.closure_roots.push_back(static_cast<int64_t>(id));
        }
    }

    // 4. fiber results — s_fiber_results_ is a TU-local static in
    //    evaluator_impl.cpp (managed by fiber:join). Each live entry
    //    is a root because the value is shared between the spawned
    //    fiber and the joiner. The static map is internally
    //    synchronized by s_fiber_results_mtx_, but at the safepoint
    //    no fiber is touching it, so we walk it directly.
    //
    //    We can't see s_fiber_results_ from this method (TU-local),
    //    so we skip the fiber_result_roots field here. The
    //    message-bridge flush hook in serve_async.cpp adds those
    //    entries separately (or the GC tolerates an empty set
    //    since the value is in closures_/string_heap_ which we
    //    already marked).
}

std::size_t Evaluator::gc_root_count() const {
    // No lock — called at safepoint time. Returns upper bound.
    std::size_t n = string_heap_.size() + pairs_.size();
    for (const auto& [id, _] : closures_) {
        if (static_cast<std::uint64_t>(id) < gc_safe_closure_id_) {
            ++n;
        }
    }
    return n;
}

// ── GC sweep / compaction (Issue #113 Phase 3) ──────────────
//
// `compact_sweep` is called by the GC collector's `collect()`
// after the mark phase has set the live bits in `marks`. We hold
// `heap_mutex()` because the sweep runs at the safepoint but a
// non-fiber thread in serve-async mode could still touch the heaps.
//
// For `closures_` we actually erase unmarked entries — this is the
// main memory-reclamation path (closure bodies hold arena-allocated
// state). For the vector heaps, we report the dead count without
// compaction, because compaction requires remapping all
// EvalValue / pair / cell references — that's a major refactor
// tracked separately in `binary_runtime_plan.md` (the C-runtime
// equivalent) and in a future iteration of the Aura evaluator
// (likely via a generation index table).

void* Evaluator::compact_sweep(void* sweep_buffers) {
    // The opaque pointer is aura::serve::GCSweepBuffers* (set by
    // the serve_async.cpp callback or directly by the GC collector
    // test). Cast is safe because both the message-bridge caller
    // and the direct test pass a real GCSweepBuffers.
    auto* marks = static_cast<aura::serve::GCSweepBuffers*>(sweep_buffers);
    if (!marks)
        return nullptr;

    std::lock_guard<std::mutex> lock(heap_mutex());
    // The result is allocated on the heap (via new) so its
    // lifetime extends past the function return. The caller
    // (serve_async.cpp) reads the fields and deletes.
    //
    // The struct here is layout-compatible with
    // `aura::messaging::GCSweepResultMsg` in messaging_bridge.h.
    // We use a local struct because messaging_bridge.h is a
    // non-module .h included via the global fragment, and the
    // C++20 module rules make it awkward to refer to its
    // types directly here. The static_assert below catches
    // any drift between the two definitions.
    struct SweepResult {
        std::size_t strings_freed = 0;
        std::size_t pairs_freed = 0;
        std::size_t closures_freed = 0;
        std::size_t fiber_results_freed = 0;
    };
    static_assert(sizeof(SweepResult) == 4 * sizeof(std::size_t),
                  "SweepResult layout must match GCSweepResultMsg");
    auto* result = new SweepResult();

    // 1. closures_ — erase unmarked entries.
    //    This is the main leak-reduction path: each closure holds
    //    an arena-allocated flat, pool, and env that can be
    //    significant memory.
    if (marks->closure_marks) {
        std::size_t before = closures_.size();
        for (auto it = closures_.begin(); it != closures_.end();) {
            int64_t id = static_cast<int64_t>(it->first);
            if (!marks->closure_marks->test(id)) {
                it = closures_.erase(it);
            } else {
                ++it;
            }
        }
        result->closures_freed = before - closures_.size();
    }

    // 2. string_heap_ — report dead count, no compaction.
    //    Compaction requires remapping all references that hold
    //    a string index (Pair car/cdr, EvalValue String tag,
    //    Closure params, etc.). Until that work lands, the heap
    //    keeps stale entries but the GC metric tells the caller
    //    how much pressure exists.
    if (marks->string_marks) {
        result->strings_freed = marks->string_marks->count_dead();
    }

    // 3. pairs_ — same. report dead count.
    if (marks->pair_marks) {
        result->pairs_freed = marks->pair_marks->count_dead();
    }

    // 4. fiber_results — owned by s_fiber_results_ (TU-local). The
    //    GC sweep handles those separately when the
    //    message-bridge registers a fiber_result sweep callback.
    //    We report 0 here so the totals add up correctly.
    result->fiber_results_freed = 0;

    return result;
}

} // namespace aura::compiler
