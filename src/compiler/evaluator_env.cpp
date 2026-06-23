// evaluator_env.cpp — P1-d: Env, EnvFrame, EnvView, and env-frame arena
// aura.compiler.evaluator module partition.

module;
#include <atomic>

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <shared_mutex>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler {

using types::EvalValue;
using namespace types;

// Depth guard: protects Env::lookup against cyclic parent chains
// (thread_local since lookup can be called from multiple fibers)
static constexpr std::size_t MAX_ENV_DEPTH = 1024;
thread_local std::size_t g_env_lookup_depth = 0;
std::optional<EvalValue> Env::lookup(const std::string& n) const {
    // The pre (!n.empty()) is on the declaration in evaluator.ixx.
    if (++g_env_lookup_depth > MAX_ENV_DEPTH) {
        --g_env_lookup_depth;
        return std::nullopt;
    }
    struct _ {
        ~_() { --g_env_lookup_depth; }
    } dec;

    // 1. Check local bindings
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == n) {
            auto& v = it->second;
            // P0 step 2: return raw (cell sentinel if applicable).
            // No cells_ member on Env. Deref centralized in Evaluator
            // scope using its cells_ (or explicit for cell ptr paths).
            return v;
        }
    // 2. Check parent
    if (parent_) {
        return parent_->lookup(n);
    }
    // 2b. Issue #232 fallback: SoA walk via parent_id_ when parent_
    // is null but parent_id_ is set. This happens for env frames
    // materialized by materialize_call_env from a closure captured
    // in a stack env (e.g., named-let → letrec → lambda capture).
    // Without this walk, the closure body can't see bindings
    // from the surrounding scope.
    //
    // Issue #145 P0 follow-up: hold env_frames_mtx_ as a
    // shared lock for the entire walk. env_frame returns a
    // reference into env_frames_; holding the lock prevents
    // the main thread's alloc_env_frame from reallocating the
    // deque's map array underneath us (which would free the
    // map pointer we're reading).
    if (parent_id_ != NULL_ENV_ID && owner_) {
        std::shared_lock<std::shared_mutex> env_rlock(owner_->env_frames_lock());
        const EnvFrame& pfr = owner_->env_frame(parent_id_);
        // Walk the parent frame's bindings (string-keyed)
        for (auto& b : pfr.bindings_) {
            if (b.first == n) {
                if (is_cell(b.second)) {
                    auto ci = as_cell_id(b.second);
                    if (ci < owner_->cells().size())
                        return owner_->cells()[ci];
                }
                return b.second;
            }
        }
        // Recurse via SoA: walk the parent's parent_id_ chain.
        // Capture the result but only return if non-null — if the
        // recursive lookup returns nullopt, fall through to the
        // primitive + ADT fallbacks below (otherwise primitives
        // like `+` and `*` would be reported as unbound variables).
        if (pfr.parent_id != NULL_ENV_ID) {
            Env tmp;
            tmp.set_owner(owner_);
            tmp.set_parent_id(pfr.parent_id);
            if (auto r = tmp.lookup(n))
                return *r;
        }
        // Final fallback: the frame at parent_id_ has the snapshot
        // of the env at capture time. If that env is still live
        // (e.g., it'"'"'s a heap-allocated module env or the top_
        // env), check its live bindings too. The frame is a snapshot
        // and may be stale if bindings were added after the frame
        // was created (e.g., via require in a nested module load).
        // We use the owner_ pointer to find the live env: for index 0
        // it'"'"'s top_, for higher indices we walk the env_frames_
        // pool to find a matching live env. The simplest case
        // (index 0 = top_) is the most common and we handle that
        // directly via owner_->top().
        if (parent_id_ == 0 && owner_) {
            // Check live top_ env'"'"'s bindings
            for (auto it = const_cast<aura::compiler::Env&>(owner_->top_env()).bindings().rbegin();
                 it != const_cast<aura::compiler::Env&>(owner_->top_env()).bindings().rend();
                 ++it) {
                if (it->first == n) {
                    if (is_cell(it->second)) {
                        auto ci = as_cell_id(it->second);
                        if (ci < owner_->cells().size())
                            return owner_->cells()[ci];
                    }
                    return it->second;
                }
            }
        }
    }
    // 3. Fallback: check primitives (allows passing names like `+` as values)
    if (primitives_) {
        auto slot = primitives_->slot_for_name(n);
        if (slot != std::numeric_limits<std::size_t>::max()) {
            return make_primitive(slot);
        }
    }
    // 4. Fallback: check ADT constructors (Issue #108 part 4 Phase 1).
    //    Bypasses Begin scoping. Registered via (adt:register-constructors ...)
    //    from parse_datatype. Returns a primitive ref that, when applied,
    //    builds (cons "CtorName" arg1 arg2 ...).
    {
        // Step 2.3: use per-evaluator adt_runtime_ (FFI pattern).
        // adt_runtime_ lives on the Evaluator, not on Env; access
        // via the owner_ back-pointer (Issue #145 Phase 2.2 set
        // the owner_ on every Env registered with the Evaluator).
        if (owner_) {
            if (auto slot = owner_->adt_runtime().find_ctor(n))
                return make_primitive(*slot);
        }
    }
    return std::nullopt;
}

// ── Env::lookup_binding: returns raw binding (cell sentinel as-is) ─
std::optional<EvalValue> Env::lookup_binding(const std::string& n) const {
    // The pre (!n.empty()) is on the declaration in evaluator.ixx.
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == n)
            return it->second;
    return parent_ ? parent_->lookup_binding(n) : std::nullopt;
}

// Issue #145: SymId fast path. Pushes to bindings_symid_
// (canonical) and mirrors to bindings_ (string form) if
// pool_ is set. The mirror is needed because the lambda body
// uses the string-based lookup to find the param (the parser
// interns names but the body's `lookup(name)` does the
// string-keyed loop). Without the mirror, lambda params would
// be invisible to body code.
void Env::bind_symid(aura::ast::SymId s, types::EvalValue v) {
    bindings_symid_.emplace_back(s, v);
    if (pool_) {
        // Resolve SymId → string once, then write to both
        // arrays. The string resolve is O(string-length) at
        // bind time; the int-compare is O(1) at lookup time.
        // Net: lookup is the hot path, so this is a win.
        std::string_view sv = pool_->resolve(s);
        if (!sv.empty())
            bindings_.emplace_back(std::string(sv), v);
    }
}

// Issue #145: SymId-based lookup. Iterates bindings_symid_
// (which the bind_symid path writes to) with integer compare.
// Most-recent-binding-wins semantics, matching the string-based
// lookup's rbegin/rend iteration order.
std::optional<types::EvalValue> Env::lookup_by_symid(aura::ast::SymId s) const {
    for (auto it = bindings_symid_.rbegin(); it != bindings_symid_.rend(); ++it) {
        if (it->first == s) {
            // P0 step 2: return raw binding (sentinel as-is). No cells_
            // member anymore. Deref (if cell) happens at caller using
            // central Evaluator cells_ when in Evaluator scope, or
            // explicit cells param for mutation paths (lookup_cell_*).
            return it->second;
        }
    }
    // P0: prefer SoA index walk (no parent_ pointer chase) if this
    // legacy Env frame was wired with owner_ + parent_id_ (as done
    // in materialize_call_env for SoA captures).
    if (owner_ && parent_id_ != NULL_ENV_ID) {
        return owner_->lookup_by_symid_chain(parent_id_, s);
    }
    return parent_ ? parent_->lookup_by_symid(s) : std::nullopt;
}

// Issue #207 (Cycle 1): bindings_with_names() — materializes
// the named version of the bindings. Walks the SymId-keyed
// bindings_symid_ array and resolves each SymId to a name
// string via pool_->resolve(). Returns a new vector with
// (name, value) pairs in the same order as bindings_symid_.
//
// This is the "current behavior, as a derived view" per the
// issue body. The legacy bindings_ array still holds the
// string-keyed version, but new code should use this helper
// to read the named view (rather than reading bindings_
// directly). When the migration completes (Cycle 2+), the
// legacy bindings_ array is dropped and bindings_with_names()
// becomes the only path to get a named view.
//
// For envs without pool_ set, the name is rendered as
// "@<symid:N>" where N is the SymId value. This is a
// fallback for envs that haven't been migrated yet.
std::vector<std::pair<std::string, types::EvalValue>> Env::bindings_with_names() const {
    std::vector<std::pair<std::string, types::EvalValue>> out;
    out.reserve(bindings_symid_.size());
    for (const auto& [sym, val] : bindings_symid_) {
        std::string name;
        if (pool_) {
            // pool_->resolve() returns the canonical name
            // (string_view) for this SymId. If the SymId
            // is not in the pool (defensive), the resolved
            // view is empty.
            std::string_view resolved = pool_->resolve(sym);
            if (!resolved.empty())
                name.assign(resolved.data(), resolved.size());
        }
        if (name.empty()) {
            // Fallback: render the SymId as a string for
            // display purposes.
            name = "@symid:" + std::to_string(sym);
        }
        out.emplace_back(std::move(name), val);
    }
    return out;
}

// Issue #145 follow-up / Phase 2.5.0: lookup_by_intern — the
// SymId-first migration scaffold for the eventual bindings_
// drop. Takes a name string, interns via the given pool
// (legacy closure-captured / env-captured pool for backward
// compat; canonical_pool() for new code), then routes through
// lookup_by_symid. The primitive + ADT-constructor fallbacks
// mirror Env::lookup's behavior (step 3 + 4) so callers that
// switch to this helper get the same observable result as
// lookup(name) when no binding is found.
//
// Note: this is a SCOPED migration tool, not a permanent API.
// When Phase 2.5 ships the actual drop of bindings_, the
// helper either goes away (callers intern once + use
// lookup_by_symid directly) or stays as a thin convenience
// wrapper. The 6 current Env::lookup(name) call sites
// (apply_closure parent walk, EnvView parent walk, module
// lookup, fn_name lookup, eval-time lookup, capture lookup)
// will migrate one per Phase 2.5.0 commit.
std::optional<types::EvalValue> Env::lookup_by_intern(const std::string& n,
                                                      const aura::ast::StringPool* pool) const {
    // Resolve which pool to use: legacy passed-in pool if
    // non-null, else fall back to the env's own pool_ (set
    // via set_pool for closures that captured a non-canonical
    // pool), else nullptr. The lookup_by_symid call below
    // will then route through whichever pool is appropriate.
    // Note: use_pool is non-const because intern() mutates the
    // pool. const_cast is safe here because the env holds the
    // pool by non-const pointer (pool_) or by the caller's
    // pointer (legacy). The lookup_by_intern method itself is
    // logically const (no observable env state change beyond
    // the pool's intern side effect, which is idempotent for
    // already-interned names).
    aura::ast::StringPool* use_pool = const_cast<aura::ast::StringPool*>(pool ? pool : pool_);
    if (!use_pool) {
        // No pool available — can't intern. Fall through to
        // the legacy string-based lookup as a last resort.
        return lookup(n);
    }
    auto sym = use_pool->intern(n);
    auto found = lookup_by_symid(sym);
    if (found)
        return found;
    // Fallbacks mirror Env::lookup's primitive + ADT paths.
    // These are not SymId-specific — the slot_for_name lookup
    // uses the string name directly.
    if (primitives_) {
        auto slot = primitives_->slot_for_name(n);
        if (slot != std::numeric_limits<std::size_t>::max()) {
            return make_primitive(slot);
        }
    }
    {
        // Step 2.3: use per-evaluator adt_runtime_ (FFI pattern).
        // adt_runtime_ lives on the Evaluator, not on Env; access
        // via the owner_ back-pointer (see Env::lookup fix above
        // for the same pattern).
        if (owner_) {
            if (auto slot = owner_->adt_runtime().find_ctor(n))
                return make_primitive(*slot);
        }
    }
    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════
// Issue #145 Phase 2.1 — EnvFrame SoA infrastructure
// ═══════════════════════════════════════════════════════════════
//
// EnvFrame is the SoA counterpart to Env. Same data layout, but
// `parent_id_` (EnvId, uint32_t) replaces `parent_` (Env*). The
// methods below are the "local-only" variants — they operate on
// one frame and do NOT walk the parent chain. Walk-aware access
// lives on Evaluator (walk_env_frames, lookup_by_symid_chain).
//
// Today Env and EnvFrame coexist. Migration is Phase 2.2.

// EnvFrame::bind — parallel to Env::bind (which writes only
// to bindings_, no mirror). SymId mirroring happens via
// bind_symid (the fast path). If you need both arrays in sync,
// bind via SymId + have pool_ set.
void EnvFrame::bind(const std::string& n, types::EvalValue v) {
    bindings_.emplace_back(n, v);
}

// EnvFrame::bind_symid — parallel to Env::bind_symid. Mirrors
// to bindings_ when pool_ is set so legacy lookup(string)
// callers still find the param.
void EnvFrame::bind_symid(aura::ast::SymId s, types::EvalValue v) {
    bindings_symid_.emplace_back(s, v);
    if (pool_) {
        std::string_view sv = pool_->resolve(s);
        if (!sv.empty())
            bindings_.emplace_back(std::string(sv), v);
    }
}

// EnvFrame::lookup_local — Env::lookup minus parent walk +
// primitive + ADT fallbacks. Pure frame-local lookup. Use
// Evaluator::walk_env_frames for chain-aware lookup.
//
// P0 migration: EnvFrame no longer holds a cells_ pointer
// (removed from struct for pure data / no pointer-to-heap).
// If the bound value is a cell sentinel, we return it as-is.
// Cell resolution is centralized in the Evaluator's
// lookup_by_symid_chain (and legacy Env paths) using the
// owning Evaluator's central cells_ pmr::vector. This
// makes frames fully index-driven and reallocation-safe.
std::optional<types::EvalValue> EnvFrame::lookup_local(const std::string& n) const {
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
        if (it->first == n) {
            return it
                ->second; // sentinel returned; deref happens at caller (Evaluator chain or legacy)
        }
    }
    return std::nullopt;
}

// EnvFrame::lookup_local_by_symid — Env::lookup_by_symid minus
// parent walk. Pure frame-local SymId compare.
//
// P0 migration: same as lookup_local — return raw sentinel
// (cell or not). Central deref lives in Evaluator.
std::optional<types::EvalValue> EnvFrame::lookup_local_by_symid(aura::ast::SymId s) const {
    for (auto it = bindings_symid_.rbegin(); it != bindings_symid_.rend(); ++it) {
        if (it->first == s) {
            return it->second;
        }
    }
    return std::nullopt;
}

// Evaluator::alloc_env_frame — append a new EnvFrame and return
// its id. The id is the new size()-1 of env_frames_. Returns
// NULL_ENV_ID on overflow (>4G envs, which is unreachable in
// practice for a single evaluator lifetime).
//
// P0 (EnvFrame SoA): frames are allocated with only parent_id +
// primitives_. No cells_ pointer (pure data). Cell resolution
// centralized later in lookup paths using Evaluator::cells_.
aura::compiler::EnvId Evaluator::alloc_env_frame(EnvId parent_id, const Primitives* primitives) {
    // Issue #145 P0 follow-up: unique_lock on env_frames_mtx_
    // to serialize push_back with fiber-thread readers
    // (materialize_call_env). The deque's map array can be
    // reallocated when the deque grows past the current map
    // capacity, freeing the map pointer a fiber thread is
    // reading via env_frame[id]. Unique lock makes the
    // push_back atomic with respect to readers.
    std::unique_lock<std::shared_mutex> wlock(env_frames_mtx_);
    if (env_frames_.size() >= NULL_ENV_ID) {
        // 4G envs reached. Return NULL to signal overflow;
        // callers should treat this as fatal (env allocation
        // exhausted) — but we keep going rather than abort
        // to make the failure mode visible.
        return NULL_ENV_ID;
    }
    EnvFrame fr;
    fr.parent_id = parent_id;
    fr.primitives_ = primitives;
    // Issue #242: stamp the frame with the current defuse_version_
    // so subsequent lookups can detect stale captures. The version
    // is an acquire-load so the frame's metadata (parent_id,
    // primitives_) is visible to threads that observe the bumped
    // version after a memory barrier.
    fr.version_ = defuse_version_.load(std::memory_order_acquire);
    env_frames_.push_back(std::move(fr));
    return static_cast<EnvId>(env_frames_.size() - 1);
}

// Evaluator::alloc_env_frame_from_env — Issue #145 Phase 2.3.
// Mirror an Env's parallel binding arrays into a new frame in
// env_frames_. The new frame's parent_id defaults to
// e.parent_id() (preserving the captured env's parent-chain
// position in the SoA arena); callers can override with an
// explicit parent_id if they want to re-parent the frame.
//
// P0: Only bindings_ + bindings_symid_ are mirrored.
// primitives_/pool_/cells_ deliberately not part of the frame
// (cells_ removed entirely from EnvFrame struct in P0 step 1;
// resolution centralized on Evaluator). This keeps frames
// as pure, index-only data.
aura::compiler::EnvId Evaluator::alloc_env_frame_from_env(const Env& e, EnvId parent_id) {
    EnvId pid = (parent_id != NULL_ENV_ID) ? parent_id : e.parent_id();
    EnvId id = alloc_env_frame(pid);
    if (id == NULL_ENV_ID)
        return NULL_ENV_ID;
    // Issue #145 P0 follow-up: hold a shared lock while
    // mutating the freshly-allocated frame. A concurrent
    // alloc_env_frame on another thread (e.g. a fiber)
    // could push_back and reallocate env_frames_'s map
    // array, invalidating our `fr` reference.
    std::unique_lock<std::shared_mutex> wlock(env_frames_mtx_);
    EnvFrame& fr = env_frames_[id];
    // e is `const`, so .bindings()/.bindings_symid() return
    // std::span (const overload). Use assign from iterators
    // to copy into the frame's vector storage.
    auto bs = e.bindings();
    fr.bindings_.assign(bs.begin(), bs.end());
    auto bss = e.bindings_symid();
    fr.bindings_symid_.assign(bss.begin(), bss.end());
    return id;
}

// Evaluator::materialize_call_env — Issue #145 Phase 2.3.
// Build a fresh Env for evaluating a closure body. When the
// closure's captured env is registered in env_frames_
// (cl.env_id ≠ NULL_ENV_ID), rebuild the call env from the
// frame's bindings and wire the SoA walk (owner_ +
// parent_id_) so lookup_cell_ptr / lookup_cell_index route
// parent lookups through walk_env_frames instead of pointer
// chase. Otherwise fall back to copying the legacy `cl.env`
// raw pointer — this preserves correct behavior for any
// stack-allocated local-eval closures that have not yet been
// registered in the SoA arena.
//
// primitives_/cells_/pool_ are NOT set here — they are
// runtime support pointers that the caller wires after
// materialization (apply_closure sets them inline; TCO tail
// call sites set them via tail_env.set_*). Keeping them out
// of this helper makes the helper usable from any code path
// that has a closure but might not need a fully wired env.
Env Evaluator::materialize_call_env(const Closure& cl) {
    // P0 complete: legacy cl.env path removed. All closures have
    // env_id set at capture time (via alloc_env_frame_from_env).
    // Always use SoA path for GC-safety and no pointer chasing.
    Env ne;
    // Defensive: a closure with env_id == NULL_ENV_ID would
    // trip the env_frame() contract and crash. This can happen
    // when the closure was constructed via a path that
    // skipped alloc_env_frame_from_env (e.g. a ClosureView
    // copy, a default-constructed Closure moved into
    // closures_[cid], or a frame that was pruned by a
    // future gc-temp cycle). Fall back to a fresh Env with
    // no captured bindings — the body will see globals via
    // the workspace walk, which is correct for lambda
    // bodies that don't actually reference the captured
    // scope (the most common case for this failure mode).
    if (cl.env_id == NULL_ENV_ID || cl.env_id >= env_frames_.size()) {
        return ne;
    }
    // Issue #145 P0 follow-up: hold env_frames_mtx_ shared
    // lock for the duration of the frame read. The frame
    // reference must remain valid through .bindings(),
    // .bindings_symid_(), and .parent_id reads. Without
    // this lock, a concurrent alloc_env_frame on the main
    // thread (during eval, mutate:rebind, etc.) could
    // reallocate the deque's map array, freeing the map
    // pointer a fiber thread (running apply_closure →
    // materialize_call_env) is reading.
    std::shared_lock<std::shared_mutex> env_rlock(env_frames_mtx_);
    const EnvFrame& fr = env_frame(cl.env_id);
    // Issue #242: detect a stale frame (captured before the
    // current mutation epoch). The frame's bindings might be
    // inconsistent with the post-mutation state — log a
    // warning + bump the frame's version_ so subsequent
    // lookups see it as fresh. We don't refresh the bindings
    // themselves (that would require re-capturing against a
    // new env, which is out of scope for the P0 ship); the
    // warning + version bump is enough to make the staleness
    // observable and prevent repeated warnings.
    if (fr.version_ < defuse_version_.load(std::memory_order_acquire)) {
        // Mutate the version under the same shared lock. A
        // shared_lock allows multiple readers but blocks
        // writers (alloc_env_frame); since we're not adding
        // or removing frames, just updating a metadata
        // field, the shared lock is sufficient (no other
        // reader depends on version_ being immutable).
        const_cast<EnvFrame&>(fr).version_ = defuse_version_.load(std::memory_order_acquire);
        // Logging is best-effort — a fiber thread might not
        // have a tty. We use std::println(std::cerr, ...) so
        // the warning is always emitted (not just in debug).
        std::println(std::cerr,
                     "[#242 warning] materialize_call_env: stale EnvFrame id={} "
                     "(frame.version_={}, current defuse_version_={}). "
                     "Bindings may be inconsistent with post-mutation state. "
                     "Bumped frame.version_ to silence future warnings.",
                     cl.env_id, fr.version_, defuse_version_.load(std::memory_order_acquire));
    }
    ne.bindings() = fr.bindings_;
    ne.bindings_symid_mut() = fr.bindings_symid_;
    if (fr.parent_id != NULL_ENV_ID) {
        ne.set_owner(this);
        ne.set_parent_id(fr.parent_id);
    }
    // Issue #286: stamp the new Env with the current
    // defuse_version_. This gives the materialized Env the same
    // snapshot semantics as the EnvFrame it was built from,
    // and lets future lookups via walk_env_frames (or
    // Env::lookup_cell_ptr) cheaply detect a stale chain by
    // comparing this stamp against the owner's current
    // defuse_version_. The stamp is captured AFTER the frame
    // version_ check above so a stale frame's bindings are
    // re-stamped to current, and the new Env reflects that
    // current value.
    ne.set_env_version(defuse_version_.load(std::memory_order_acquire));
    return ne;
}

// Issue #242: is_env_frame_stale — true if the frame's
// stamped version is older than the current defuse_version_
// (i.e. captured before a mutation that may have invalidated
// the captured scope). Returns true for invalid ids as a
// safety net so callers can treat invalid frames as stale.
bool Evaluator::is_env_frame_stale(EnvId id) const {
    if (id == NULL_ENV_ID || id >= env_frames_.size())
        return true;
    // env_frames_ is a deque guarded by env_frames_mtx_; a
    // shared_lock keeps the frame alive across the load.
    std::shared_lock<std::shared_mutex> rlock(env_frames_mtx_);
    return env_frames_[id].version_ < defuse_version_.load(std::memory_order_acquire);
}

// Evaluator::lookup_by_symid_chain — demonstrate the SoA walk.
// Walks env_frames_ via index lookup (no pointer chase) and
// returns the first match (closest frame wins, shadowing
// semantics match Env::lookup_by_symid).
//
// P0: Cell deref is now centralized here using the Evaluator's
// own central `cells_` pmr vector (the only owner of the
// re-allocatable cell heap). EnvFrame no longer stores a
// cells_ pointer; frames are pure data + indices. This is
// the canonical path for new SoA code. Legacy Env paths
// (still using Env::cells_ pointer) remain for transition.
std::optional<types::EvalValue> Evaluator::lookup_by_symid_chain(EnvId start,
                                                                 aura::ast::SymId s) const {
    std::optional<types::EvalValue> result;
    const auto version_snap = defuse_version_.load(std::memory_order_acquire);
    walk_env_frames(start, [&](EnvId, const EnvFrame& fr) {
        // Issue #264: skip frames stamped before the current
        // mutation epoch (stale under concurrent mutate/compact).
        if (fr.version_ < version_snap)
            return true;
        auto v = fr.lookup_local_by_symid(s);
        if (v.has_value()) {
            auto val = *v;
            if (is_cell(val)) {
                auto idx = as_cell_id(val);
                if (idx < cells_.size())
                    result = cells_[idx];
                else
                    result = val; // defensive
            } else {
                result = std::move(val);
            }
            return false; // stop walking — closest frame wins
        }
        return true; // continue walking
    });
    return result;
}
void Evaluator::walk_env_frame_roots(std::vector<std::int64_t>& pair_roots_out,
                                     std::vector<std::int64_t>& closure_roots_out) const {
    // De-dup: a pair/closure may be bound in multiple envs.
    // Using a small set per pass; if the size grows beyond
    // a threshold, we fall back to dedup-after-the-fact
    // (mark vectors also de-dup via the set() semantic).
    // For now, just always set — the GC's mark_env_frame_roots
    // is idempotent (set() is a no-op if already set).
    for (const auto& fr : env_frames_) {
        // Walk the name-keyed bindings. bindings_symid_ is
        // populated when pool_ is set; bindings_ is always
        // populated. We walk BOTH to be safe (they should
        // hold the same values, but checking is cheap).
        for (const auto& [name, val] : fr.bindings_) {
            (void)name;
            if (is_pair(val)) {
                pair_roots_out.push_back(static_cast<std::int64_t>(as_pair_idx(val)));
            } else if (is_closure(val)) {
                closure_roots_out.push_back(static_cast<std::int64_t>(as_closure_id(val)));
            }
        }
        for (const auto& [sym, val] : fr.bindings_symid_) {
            (void)sym;
            if (is_pair(val)) {
                pair_roots_out.push_back(static_cast<std::int64_t>(as_pair_idx(val)));
            } else if (is_closure(val)) {
                closure_roots_out.push_back(static_cast<std::int64_t>(as_closure_id(val)));
            }
        }
    }
}
// ── Issue #145: EnvView / ClosureView impls ──────────────────
//
// make_env_view: build a zero-copy view over an Env's
// bindings. The spans stay valid as long as the Env does
// (no vector reallocation expected — the Env's bindings_
// grows monotonically within a single eval).
EnvView make_env_view(const Env& env) {
    EnvView v;
    v.string_bindings = env.bindings();
    // The SymId-keyed array is private; access via a const
    // accessor friend. We add the accessor below.
    v.symid_bindings = env.bindings_symid();
    v.parent = env.parent();
    return v;
}

std::optional<EvalValue> EnvView::lookup(const std::string& name) const {
    for (auto it = string_bindings.rbegin(); it != string_bindings.rend(); ++it)
        if (it->first == name)
            return it->second;
    return parent ? parent->lookup(name) : std::nullopt;
}

std::optional<EvalValue> EnvView::lookup_by_intern(const std::string& n,
                                                   const aura::ast::StringPool* pool) const {
    // Mirror Env::lookup_by_intern: intern via the resolved
    // pool, route through lookup_by_symid, return local
    // symid_bindings lookup if not found, then fall through
    // to the parent walk. EnvView has no primitive/ADT
    // fallbacks (those live on Env, not EnvView), so the
    // behavior matches EnvView::lookup for the "name not
    // found" case: nullopt.
    if (!pool)
        return std::nullopt; // EnvView: no fallback pool
    // const_cast is safe — intern() is logically idempotent
    // (already-interned names are no-ops) and EnvView callers
    // pass a non-const pool pointer (canonical_pool() or
    // closure-captured). The function is logically const
    // (no observable EnvView state change).
    auto sym = const_cast<aura::ast::StringPool*>(pool)->intern(n);
    for (auto it = symid_bindings.rbegin(); it != symid_bindings.rend(); ++it)
        if (it->first == sym)
            return it->second;
    return parent ? parent->lookup_by_intern(n, pool) : std::nullopt;
}

std::optional<EvalValue> EnvView::lookup_by_symid(aura::ast::SymId s) const {
    for (auto it = symid_bindings.rbegin(); it != symid_bindings.rend(); ++it)
        if (it->first == s)
            return it->second;
    return parent ? parent->lookup_by_symid(s) : std::nullopt;
}

ClosureView make_closure_view(const Closure& cl) {
    ClosureView v;
    v.params = std::span<const aura::ast::SymId>(cl.params.data(), cl.params.size());
    v.body_id = cl.body_id;
    v.dotted = cl.dotted;
    v.flat = cl.flat;
    v.pool = cl.pool;
    // P0: no more cl.env; only env_id.
    v.env_id = cl.env_id;
    v.owner_arena = cl.owner_arena;
    v.name = cl.name;
    return v;
}
// ── Env::lookup_cell_ptr: returns EvalValue* ──────────────────
//
// Issue #145 Phase 2.2: parent walk migrates to
// walk_env_frames when owner_ + parent_id_ are set (SoA path).
// Legacy Env* walk preserved as fallback for stack-allocated
// Envs that aren't registered in env_frames_ (local eval scopes
// before Phase 2.6 ships the rename).
EvalValue* Env::lookup_cell_ptr(const std::string& n, std::vector<EvalValue>* cells) const {
    if (!cells)
        return nullptr;
    // 1. Local bindings (no walk needed)
    for (auto& b : bindings_) {
        if (b.first == n) {
            if (is_cell(b.second)) {
                auto ci = as_cell_id(b.second);
                if (ci < cells->size())
                    return &(*cells)[ci];
            }
            return nullptr;
        }
    }
    // 2. Walk parent chain — prefer SoA walk via env_frames_
    //    when owner_ + parent_id_ are both set. Canonical path
    //    for registered Envs (top_, modules_, arena-allocated
    //    envs). Cache-friendly index lookup replaces pointer
    //    chase; shadowing semantics preserved (closest frame
    //    wins, walk_env_frames stops at first match).
    //
    //    Issue #286: snapshot defuse_version_ once at walk
    //    start, then skip frames whose version_ is older than
    //    the snapshot (the stale-frames check from #242). This
    //    keeps lookup behavior consistent with
    //    lookup_by_symid_chain (which already does the check).
    if (owner_ && parent_id_ != NULL_ENV_ID) {
        const auto version_snap =
            owner_->get_defuse_version();
        EvalValue* result = nullptr;
        owner_->walk_env_frames(parent_id_, [&](EnvId, const EnvFrame& f) {
            // Skip frames stamped before the current mutation epoch.
            if (f.version_ < version_snap)
                return true; // continue walking past the stale frame
            for (auto& b : f.bindings_) {
                if (b.first == n) {
                    if (is_cell(b.second)) {
                        auto ci = as_cell_id(b.second);
                        if (ci < cells->size())
                            result = &(*cells)[ci];
                    }
                    return false; // stop walking
                }
            }
            return true;
        });
        return result;
    }
    // 3. Legacy pointer walk (preserved for unregistered Envs).
    //    Same shadowing semantics: closest frame wins.
    for (auto* p = parent_; p; p = p->parent_) {
        for (auto& b : p->bindings_) {
            if (b.first == n) {
                if (is_cell(b.second)) {
                    auto ci = as_cell_id(b.second);
                    if (ci < cells->size())
                        return &(*cells)[ci];
                }
                return nullptr;
            }
        }
    }
    return nullptr;
}

// ── Env::lookup_cell_index: returns uint64_t (stable) ─────────────
//
// Issue #145 Phase 2.2: same dual-path pattern as
// lookup_cell_ptr. SoA walk via env_frames_ when registered,
// legacy pointer walk otherwise.
std::optional<std::uint64_t> Env::lookup_cell_index(const std::string& n) const {
    // 1. Local bindings
    for (auto& b : bindings_) {
        if (b.first == n) {
            if (is_cell(b.second))
                return as_cell_id(b.second);
            return std::nullopt;
        }
    }
    // 2. SoA walk via env_frames_ when registered
    if (owner_ && parent_id_ != NULL_ENV_ID) {
        std::optional<std::uint64_t> result;
        owner_->walk_env_frames(parent_id_, [&](EnvId, const EnvFrame& f) {
            for (auto& b : f.bindings_) {
                if (b.first == n) {
                    if (is_cell(b.second))
                        result = as_cell_id(b.second);
                    return false;
                }
            }
            return true;
        });
        return result;
    }
    // 3. Legacy pointer walk
    for (auto* p = parent_; p; p = p->parent_) {
        for (auto& b : p->bindings_) {
            if (b.first == n) {
                if (is_cell(b.second))
                    return as_cell_id(b.second);
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

} // namespace aura::compiler
