// lifetime_pin.ixx — Issue #2000 Phase 2: LifetimePin real RAII pinning +
// generation stamp + FFI handoff + restamp/invalidate hooks. Refines #1226
// Phase 1 (pure counters). Issue #2048: batch terminal present zero-copy
// handoff holds a LifetimePin for the ANSI/cell buffer under the render
// hotpath soft-gate and arms g_ffi_pin_defer_depth so compact/GC cannot
// reclaim the buffer mid-write (see present_batch_impl).
//
// Phase 3 (#2265): add real pointer remap for Moving densify. The pin
// tracks an object address; if the densify site relocates the tracked
// object, the pin's ptr_ must follow or downstream FFI reads UAF. Soft
// and Force paths don't move objects, so remap is Moving-only and
// never touches the non-moving hot path.
//
// Phase 2 surfaces:
//   - Real pointer + generation stamp storage per pin (ptr_, gen_, arena_id_).
//   - validate(cur_gen, cur_arena_id) → bool: false after compact invalidates.
//   - restamp(new_gen, new_arena_id): pin survived compact, gen bumped.
//     new_gen = 0 keeps current gen (signals "boundary dtor ran" with no
//     observed gen bump — useful for MutationBoundaryGuard dtor wiring
//     where the boundary itself doesn't bump gen, only compact_sweep does).
//   - unpin_on_compact(): pin dead (buffer reclaimed), ptr nulled.
//   - remap(new_ptr, new_gen): Phase 3 (#2265) — densify rewrote the
//     pointee; pin updates ptr_ (and gen when new_gen != 0) to track the
//     new address without going through unpin+repin. Bumps remap counter.
//   - Active sharded registry (Issue #2342) so compact hooks can iterate
//     all live LifetimePin instances tied to an arena and restamp /
//     invalidate / remap them in one pass (no per-pin coordination).
//     Legacy process-wide pin_registry() removed in #2374 (always empty
//     after #2342; densify selective-invalidate walks pin_registry_shards).
//   - Three new stats counters: invalidations, restamps, remaps
//     (Phase 1 retained pins, unpins, ffi_handoffs).
//   - kLifetimePinPhase bumps from 2 to 3.
//   - mark_ffi_handoff stays as the handoff signal (counter + bool flag on
//     the pin for downstream FFI consumers to consult ownership state).
//
// Thread-safety:
//   - registry mutex serializes ctor/dtor/move ctor & assign + global helpers.
//   - ptr_/gen_/arena_id_ are plain fields — validate() reads them without
//     a lock. The worst case is a stale read returning false-negative
//     ("invalid" when actually still valid) — which is safe (FFI just
//     recreates the pin post-compact, no UAF). False-positive is not
//     possible because restamp/unpin_on_compact/rmap are serialized by the
//     registry mutex.

module;

export module aura.core.lifetime_pin;

import std;

export namespace aura::core::lifetime {

inline constexpr int kLifetimePinPhase = 3;
// Issue #2298: general (non-render) object pin-or-remap protocol issue stamp.
inline constexpr int kGeneralObjectPinIssue = 2298;
// Issue #2363: complete GeneralObjectPin adoption for mutate/agent/scratch
// intermediate create sites (refines #2337 single-site demo).
inline constexpr int kGeneralObjectPinAdoptIssue = 2363;
// Issue #2496: GeneralObjectPin adoption coverage gate (inventory vs
// wire_total). Maintain kGeneralObjectPinAdoptSiteCount vs
// general_object_pin_mutate_wire_total. AURA_GENERAL_OBJECT_PIN=required →
// fail-closed on new densify-tracked intermediate create without pin
// (optional runtime mode). Linter fails when a listed inventory site
// lacks wire call (note_general_object_pin_mutate_wire /
// wire_general_object_create_pair).
inline constexpr int kGeneralObjectPinCoverageGateIssue = 2496;
// Expected adopted wire sites (mutate/agent/scratch intermediate create).
// Bumped when a new site calls wire_general_object_create_pair /
// note_general_object_pin_mutate_wire. Dashboard compares wire total
// growth against this inventory for coverage.
//   1 mutate:replace-pattern          (evaluator_primitives_mutate.cpp)
//   2 batch :replace-pattern          (evaluator_eval_flat.cpp)
//   3 require import parse            (evaluator_eval_flat.cpp)
//   4 query:pattern                   (evaluator_primitives_query_workspace.cpp)
//   5 query:pattern guard             (evaluator_primitives_query_workspace.cpp)
//   6 load                            (evaluator_primitives_eval.cpp)
//   7 eval-expr                       (evaluator_primitives_eval.cpp)
// Agent create paths funnel through eval/mutate (no separate temp_arena
// create in evaluator_primitives_agent.cpp).
inline constexpr std::uint64_t kGeneralObjectPinAdoptSiteCount = 7;

// ── Object class × required protocol inventory (#2298 AC5 / #2363) ────
// | Class                         | Protocol                          |
// | AST nodes / StableNodeRef     | generation + StableNodeRef fence  |
// | EnvFrame SoA / EnvFrameRef    | env_gen fence (#2268) + transfer  |
// |                               | (#2295); not LifetimePin          |
// | Closure captures (AOT)        | env_gen remount (#2272) + densify |
// |                               | cell remap (#2297) / RootRemap    |
// | Render / FFI present buffers  | LifetimePin + PinOwner (#2265/    |
// |                               | #2270) + PresentGuard / RenderPin |
// | Linear roots (Move/Drop)      | pin_linear_root (#2280)           |
// | Intermediate general buffers  | pin_or_fail / GeneralObjectPin    |
// | (mutate/agent/scratch create) | (#2298/#2337/#2363) — pin-or-     |
// |                               | remap under Moving densify;       |
// |                               | complete adopt via                |
// |                               | wire_general_object_create_pair   |
// Soft/Force do not relocate create objects → zero remap work (AC4).
// Prefer RootRemapPass (#2294) for densify-tracked roots already in
// object_remap; LifetimePin for external / cross-boundary consumers.

struct LifetimePinStats {
    std::uint64_t pins = 0;
    std::uint64_t unpins = 0;
    std::uint64_t ffi_handoffs = 0;
    std::uint64_t invalidations = 0; // Phase 2: compact reclaimed buffer
    std::uint64_t restamps = 0;      // Phase 2: compact bumped gen, pin still valid
    // Issue #2265 Phase 3: Moving densify remapped ptr_ to a new address
    // while keeping gen/arena intact. Bumps per-pin when remap() succeeds.
    std::uint64_t remaps = 0;
    // Issue #2265 Phase 3: Moving densify expected a pin remap (old_ptr in
    // last_object_remap_) but no matching pin was in the registry.
    // Indicates stale ptr_ or pin destroyed between relocate + remap walk.
    // Agent-visible counter so dashboards can flag orphaned remap entries.
    std::uint64_t remap_misses = 0;
    // Issue #2085: validate() detected gen drift between the pin's stored
    // gen_ and the caller's cur_gen (or arena_id mismatch). Bumps when
    // `validate(cur_gen, cur_arena_id)` returns false for reasons other
    // than ptr being null. Agent-visible counter so dashboards can flag
    // long-held pins that drifted across a Force live_compact cycle.
    std::uint64_t gen_mismatch_total = 0;
    // Issue #2270: pin-owner state machine transition counters. Appended
    // to the existing process-level stats struct so the query surface can
    // read all pin stats from one location (g_lifetime_pin_stats). These
    // are process-level non-atomic counters — same trade-off as the
    // existing fields (stats-only, monotonic, relaxed reads OK). For
    // high-frequency FFI handoff paths, the per-class atomic in
    // CompilerMetrics (observability_metrics.h) is the source of truth;
    // these mirror for parity with the other LifetimePinStats fields.
    std::uint64_t pin_owner_arena_transitions = 0;        // #2270
    std::uint64_t pin_owner_ffi_borrowed_transitions = 0; // #2270
    std::uint64_t pin_owner_ffi_owned_transitions = 0;    // #2270
    // Issue #2298: non-render general object pin-or-remap protocol.
    std::uint64_t general_object_pin_total = 0;               // pin_or_fail / GeneralObjectPin::pin
    std::uint64_t general_object_pin_validate_fail_total = 0; // validate failed
    std::uint64_t general_object_pin_remap_ok_total = 0;      // validate ok after Moving densify
    // Issue #2337 / #2363: adoption wire-up counter for mutate/agent/
    // scratch create paths. Bumped once per call site that wraps
    // GeneralObjectPin(s) around intermediate create buffers (via
    // note_general_object_pin_mutate_wire / wire_general_object_create_pair).
    // Distinct from general_object_pin_total (per-allocate) — this is
    // per-site so dashboards track adoption coverage (#2363 complete).
    std::uint64_t general_object_pin_mutate_wire_total = 0; // #2337/#2363
};

inline LifetimePinStats g_lifetime_pin_stats{};

// Issue #2270: explicit pin ownership state machine. The pre-#2270
// ffi_handoff_ flag (bool) was a single signal; #2270 splits it into
// three explicit states so callers / dashboards can distinguish:
//   - Arena (default after pin() / reset()): arena owns the buffer,
//     Moving + Force + reclaim are all safe.
//   - FfiBorrowed: FFI may read, arena still owns the buffer. FFI
//     must NOT mutate; Moving allowed (arena still owns).
//   - FfiOwned: ownership transferred to FFI; arena must NOT reclaim
//     until return. Moving / Force / reclaim blocked until Released.
//   - None: transient — pin was released; no live owner. Hard-impossible
//     to observe because reset() / dtor always transitions back to
//     Arena (the next pin() call sets it again) — kept as a sentinel
//     for transition validation.
enum class PinOwner : std::uint8_t {
    None = 0,
    Arena = 1,
    FfiBorrowed = 2,
    FfiOwned = 3,
};

class LifetimePin;

// Issue #2342 / #2374: sharded pin registry. Replaces the process-wide
// `pin_registry()` + `pin_registry_mtx()` pair (removed in #2374 — always
// empty after LifetimePin registered only into shards) with N independent
// shards. Each shard owns its own mutex + vector; LifetimePin
// ctor/dtor/move/pin route to one shard by arena_id (default
// shard 0 for arena_id=0). Compact walks that target a specific
// arena touch only the relevant shard; walks that need every pin
// (remap_pins_pointing_to, verify_pins_under_moving_compact,
// invalidate_pins_not_in_new_addrs) take all N shard locks in
// shard-index order (deadlock-safe).
//
// Shard count is a power of 2 so arena_id-based routing is a single
// AND. 16 shards is enough to spread FFI/render pin churn across
// distinct mutexes without exploding memory; can grow if benchmarks
// show saturation.
//
// AC1: scalability — global lock hold time shrinks because most pin
// ctor/dtor traffic targets one shard's mutex, not the global one.
// AC2: Soft / no densify still pays one shard lock (not zero —
// TLS buffer would be required for true zero-lock; per the issue
// "Prefer (1) or (2) for incremental risk" — Option 1 chosen for
// simpler lock-ordering analysis). Documented as incremental; true
// zero-lock via TLS buffer is future work.
// AC3: compact walks remain correct — restamp_all_pins_for_arena
// hits one shard; non-arena walks hit all shards in order.
// AC4: observability — pin_registry_shard_pin_count(idx) +
// pin_registry_shard_max_pin_count() + pin_registry_lock_wait_us_total.
inline constexpr std::size_t kPinRegistryShardCount = 16;
inline constexpr std::size_t kPinRegistryShardMask = kPinRegistryShardCount - 1;

struct PinRegistryShard {
    std::mutex mtx;
    std::vector<LifetimePin*> pins;
};

inline std::array<PinRegistryShard, kPinRegistryShardCount>& pin_registry_shards() noexcept {
    static std::array<PinRegistryShard, kPinRegistryShardCount> shards{};
    return shards;
}

// Route by arena_id (0 lands on shard 0 — default for untyped FFI
// buffers). arena_id != 0 spreads by arena, so compact arena-specific
// walks only touch one shard.
inline std::size_t pin_registry_shard_index(std::uint64_t arena_id) noexcept {
    if (arena_id == 0)
        return 0;
    return static_cast<std::size_t>(arena_id) & kPinRegistryShardMask;
}

// Process-level cumulative lock-wait microseconds. Bumped inside
// each shard's lock_guard ctor (best-effort; the std::chrono
// resolution is microseconds). Used by AC4 observability.
inline std::atomic<std::uint64_t> g_pin_registry_lock_wait_us_total{0};
// Issue #2496: process-wide counter for AURA_GENERAL_OBJECT_PIN=required
// fail-closed enforcement. Bumped when a densify-tracked intermediate
// create fails the required-mode gate (new create without pin under
// production Moving + restricted). Agent dashboards surface regression.
// Already inside export namespace — bare inline (not export inline).
inline std::atomic<std::uint64_t> g_general_object_pin_required_enforced_total{0};
// Issue #2496: AURA_GENERAL_OBJECT_PIN=required preference (process-wide).
// -1 = env/default unset, 0 = off, 1 = required. Applied by
// apply_general_object_pin_required_env().
inline std::atomic<int> g_general_object_pin_required_pref{-1};

// Issue #2496: read AURA_GENERAL_OBJECT_PIN env var at process start
// (called from production security defaults). Values: "required" / "1" /
// "true" / "yes" / "on" → enable fail-closed mode. "off" / "0" / "false"
// / "no" → disable. Unset → -1 (no preference; Soft path).
inline void apply_general_object_pin_required_env() noexcept {
    const char* e = std::getenv("AURA_GENERAL_OBJECT_PIN");
    if (!e || !*e)
        return;
    std::string_view v(e);
    if (v == "required" || v == "1" || v == "true" || v == "yes" || v == "on") {
        g_general_object_pin_required_pref.store(1, std::memory_order_release);
    } else if (v == "off" || v == "0" || v == "false" || v == "no") {
        g_general_object_pin_required_pref.store(0, std::memory_order_release);
    }
}
inline std::uint64_t pin_registry_lock_wait_us_total() noexcept {
    return g_pin_registry_lock_wait_us_total.load(std::memory_order_relaxed);
}

// pin_registry_shard_pin_count / pin_registry_total_pinned_count /
// pin_registry_shard_max_pin_count are defined after LifetimePin (they
// call pinned() — incomplete type here would fail -Werror builds).

class LifetimePin {
public:
    LifetimePin() noexcept {
        // Issue #2342: route to shard by arena_id_ (0 → shard 0).
        shard_index_ = pin_registry_shard_index(arena_id_);
        auto& shard = pin_registry_shards()[shard_index_];
        const auto wait_start = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(shard.mtx);
        const auto wait_end = std::chrono::steady_clock::now();
        g_pin_registry_lock_wait_us_total.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(wait_end - wait_start)
                    .count()),
            std::memory_order_relaxed);
        shard.pins.push_back(this);
        owner_ = PinOwner::Arena; // Issue #2270: default Arena on pin().
        ++g_lifetime_pin_stats.pins;
        ++g_lifetime_pin_stats.pin_owner_arena_transitions;
    }
    ~LifetimePin() noexcept {
        // Issue #2342: deregister from shard_index_'s shard (not a
        // global search). Defensive bounds check — shard_index_ is
        // set in ctor and updated only via pin() / move.
        if (shard_index_ >= kPinRegistryShardCount)
            return;
        auto& shard = pin_registry_shards()[shard_index_];
        std::lock_guard<std::mutex> lock(shard.mtx);
        auto& vec = shard.pins;
        auto it = std::find(vec.begin(), vec.end(), this);
        if (it != vec.end())
            vec.erase(it);
        owner_ = PinOwner::None; // Issue #2270: terminal None on dtor.
        ++g_lifetime_pin_stats.unpins;
    }
    LifetimePin(const LifetimePin&) = delete;
    LifetimePin& operator=(const LifetimePin&) = delete;

    LifetimePin(LifetimePin&& o) noexcept
        : ptr_(o.ptr_)
        , gen_(o.gen_)
        , arena_id_(o.arena_id_)
        , ffi_handoff_(o.ffi_handoff_)
        // Declaration order: shard_index_ before owner_ (#2342 / -Werror=reorder).
        , shard_index_(o.shard_index_) // Issue #2342: transfer shard routing.
        , owner_(o.owner_) {
        o.ptr_ = nullptr;
        o.gen_ = 0;
        o.arena_id_ = 0;
        o.ffi_handoff_ = false;
        o.owner_ = PinOwner::None; // Issue #2270: moved-from resets to None.
        o.shard_index_ = 0;        // Issue #2342: source's shard_index_ is now stale.
        auto& shard = pin_registry_shards()[shard_index_];
        std::lock_guard<std::mutex> lock(shard.mtx);
        auto& vec = shard.pins;
        auto it = std::find(vec.begin(), vec.end(), &o);
        if (it != vec.end())
            *it = this;
        else
            vec.push_back(this);
    }
    LifetimePin& operator=(LifetimePin&& o) noexcept {
        if (this != &o) {
            // Issue #2342: deregister this from current shard first.
            if (shard_index_ < kPinRegistryShardCount) {
                auto& my_shard = pin_registry_shards()[shard_index_];
                std::lock_guard<std::mutex> my_lock(my_shard.mtx);
                auto& my_vec = my_shard.pins;
                auto my_it = std::find(my_vec.begin(), my_vec.end(), this);
                if (my_it != my_vec.end())
                    my_vec.erase(my_it);
            }
            ptr_ = o.ptr_;
            gen_ = o.gen_;
            arena_id_ = o.arena_id_;
            ffi_handoff_ = o.ffi_handoff_;
            owner_ = o.owner_;             // Issue #2270: transfer ownership state.
            shard_index_ = o.shard_index_; // Issue #2342: transfer shard routing.
            o.ptr_ = nullptr;
            o.gen_ = 0;
            o.arena_id_ = 0;
            o.ffi_handoff_ = false;
            o.owner_ = PinOwner::None; // Issue #2270: reset moved-from.
            o.shard_index_ = 0;
            // Register in destination shard.
            auto& new_shard = pin_registry_shards()[shard_index_];
            std::lock_guard<std::mutex> new_lock(new_shard.mtx);
            auto& new_vec = new_shard.pins;
            auto src_it = std::find(new_vec.begin(), new_vec.end(), &o);
            if (src_it != new_vec.end())
                *src_it = this;
            else
                new_vec.push_back(this);
        }
        return *this;
    }

    // Pin a buffer pointer with a generation stamp. arena_id = 0 means
    // "no specific arena — generic FFI buffer" (validate still checks gen).
    void pin(void* p, std::uint64_t g, std::uint64_t arena_id = 0) noexcept {
        // Issue #2342: re-route to correct shard if arena_id changed.
        // Locks both old + new shards in idx order (deadlock-safe).
        const auto old_idx = shard_index_;
        const auto new_idx = pin_registry_shard_index(arena_id);
        ptr_ = p;
        gen_ = g;
        arena_id_ = arena_id;
        ffi_handoff_ = false;
        if (new_idx != old_idx) {
            auto& old_shard = pin_registry_shards()[old_idx];
            auto& new_shard = pin_registry_shards()[new_idx];
            if (old_idx < new_idx) {
                std::lock_guard<std::mutex> old_lock(old_shard.mtx);
                std::lock_guard<std::mutex> new_lock(new_shard.mtx);
                auto& old_vec = old_shard.pins;
                auto old_it = std::find(old_vec.begin(), old_vec.end(), this);
                if (old_it != old_vec.end())
                    old_vec.erase(old_it);
                new_shard.pins.push_back(this);
            } else {
                std::lock_guard<std::mutex> new_lock(new_shard.mtx);
                std::lock_guard<std::mutex> old_lock(old_shard.mtx);
                auto& old_vec = old_shard.pins;
                auto old_it = std::find(old_vec.begin(), old_vec.end(), this);
                if (old_it != old_vec.end())
                    old_vec.erase(old_it);
                new_shard.pins.push_back(this);
            }
            shard_index_ = new_idx;
        }
    }

    [[nodiscard]] bool pinned() const noexcept { return ptr_ != nullptr; }
    [[nodiscard]] void* ptr() const noexcept { return ptr_; }
    [[nodiscard]] std::uint64_t gen() const noexcept { return gen_; }
    [[nodiscard]] std::uint64_t arena_id() const noexcept { return arena_id_; }

    // Validate pin against current generation + arena id. Returns false if
    // pin was invalidated (ptr nulled) or gen / arena_id mismatch.
    // Issue #2085: bump gen_mismatch_total on arena_id mismatch or gen
    // mismatch (ptr-null returns false but is not a mismatch).
    [[nodiscard]] bool validate(std::uint64_t cur_gen,
                                std::uint64_t cur_arena_id = 0) const noexcept {
        if (!ptr_)
            return false;
        if (arena_id_ != 0 && cur_arena_id != 0 && arena_id_ != cur_arena_id) {
            ++g_lifetime_pin_stats.gen_mismatch_total;
            return false;
        }
        if (gen_ != cur_gen) {
            ++g_lifetime_pin_stats.gen_mismatch_total;
            return false;
        }
        return true;
    }

    // Compact hook (Phase 2): pin survived compact, gen bumped to track
    // new generation. new_gen = 0 keeps current gen (boundary-dtor use).
    // new_arena_id = 0 keeps current arena_id.
    void restamp(std::uint64_t new_gen = 0, std::uint64_t new_arena_id = 0) noexcept {
        if (!ptr_)
            return;
        if (new_gen != 0)
            gen_ = new_gen;
        if (new_arena_id != 0)
            arena_id_ = new_arena_id;
        ++g_lifetime_pin_stats.restamps;
    }

    // Compact hook (Phase 2): buffer dead, pin invalidated. Subsequent
    // validate() returns false; FFI consumer must re-pin if it needs the
    // buffer again.
    void unpin_on_compact() noexcept {
        if (!ptr_)
            return;
        ptr_ = nullptr;
        gen_ = 0;
        arena_id_ = 0;
        ffi_handoff_ = false;
        ++g_lifetime_pin_stats.invalidations;
    }

    // Compact hook (Phase 3 / #2265): Moving densify rewrote the
    // pointee to a new address. Update ptr_ (and gen when new_gen != 0)
    // to follow the move WITHOUT going through unpin+repin. arena_id is
    // unchanged (densify keeps the arena boundary). FFI consumers that
    // cached `ptr()` see the new address on next call; `validate(...)`
    // continues to succeed against the new gen. Bumps remaps counter.
    void remap(void* new_ptr, std::uint64_t new_gen = 0) noexcept {
        if (!new_ptr)
            return; // fail closed: do not null an existing pin's ptr_
        ptr_ = new_ptr;
        if (new_gen != 0)
            gen_ = new_gen;
        ++g_lifetime_pin_stats.remaps;
    }

    // FFI handoff signal. Phase 1: counter bump only. Phase 2: also flips
    // an internal flag so downstream consumers (e.g. ffi_hot batch dispatch)
    // can consult ownership transfer state without re-querying the caller.
    void mark_ffi_handoff() noexcept {
        ffi_handoff_ = true;
        ++g_lifetime_pin_stats.ffi_handoffs;
        // Issue #2270: default to Borrowed (FFI may read, arena still
        // owns). Callers wanting full transfer must call mark_ffi_owned().
        owner_ = PinOwner::FfiBorrowed;
        ++g_lifetime_pin_stats.pin_owner_ffi_borrowed_transitions;
    }
    // Issue #2270: full ownership transfer (FFI may read + mutate;
    // arena must NOT reclaim until release_ffi() / dtor).
    void mark_ffi_owned() noexcept {
        ffi_handoff_ = true;
        ++g_lifetime_pin_stats.ffi_handoffs;
        owner_ = PinOwner::FfiOwned;
        ++g_lifetime_pin_stats.pin_owner_ffi_owned_transitions;
    }
    // Issue #2270: explicit release back to Arena (FFI returned the
    // buffer; arena reclaims ownership). Safe to call from FFI's
    // reclaim path. Idempotent — second call is a no-op.
    void release_ffi() noexcept {
        ffi_handoff_ = false;
        owner_ = PinOwner::Arena;
        ++g_lifetime_pin_stats.pin_owner_arena_transitions;
    }
    [[nodiscard]] bool ffi_handoff() const noexcept { return ffi_handoff_; }
    // Issue #2270: state-machine accessor (replaces ad-hoc bool checks).
    [[nodiscard]] PinOwner owner() const noexcept { return owner_; }
    // Issue #2270: true iff FFI holds any form of ownership
    // (Borrowed OR Owned). Used by dashboards / observability.
    [[nodiscard]] bool ffi_holds_ownership() const noexcept {
        return owner_ == PinOwner::FfiBorrowed || owner_ == PinOwner::FfiOwned;
    }
    // Issue #2270: true iff arena must NOT reclaim (FFI has full
    // ownership). Used by Moving / Force hard-mutex to distinguish
    // render pins blocking Moving from Borrowed pins that don't.
    [[nodiscard]] bool blocks_arena_reclaim() const noexcept {
        return owner_ == PinOwner::FfiOwned;
    }

private:
    void* ptr_ = nullptr;
    std::uint64_t gen_ = 0;
    std::uint64_t arena_id_ = 0;
    bool ffi_handoff_ = false;
    // Issue #2342: shard routing. Ctor sets to shard 0 (arena_id_=0
    // default). pin() re-routes when arena_id_ changes. move ctor /
    // assign transfer from source. Used by dtor to deregister from
    // the correct shard (avoids O(N) search across all shards).
    mutable std::uint32_t shard_index_ = 0;
    // Issue #2270: owner state machine. Default Arena (set in ctor).
    // Updated by mark_ffi_handoff / mark_ffi_owned / release_ffi / dtor.
    PinOwner owner_ = PinOwner::Arena;
};

// Restamp every live LifetimePin tied to `arena_id`. arena_id == 0
// matches all (use for boundary-wide restamp). new_gen == 0 keeps current
// gen. Returns # restamped (counter-bumped).
//
// Issue #2375 (#2342 regression): when arena_id == 0, walk ALL shards in
// index order (same deadlock-safe order as remap_pins_pointing_to /
// verify_pins_under_moving_compact). Pre-#2375 only walked shards[0], so
// boundary-wide restamp (evaluator_mutation_boundary) silently missed
// pins in shards[1..15] → stale gen / false-positive validate / FFI UAF.
// arena_id != 0 still walks a single shard (no perf regression).
inline std::size_t restamp_all_pins_for_arena(std::uint64_t arena_id,
                                              std::uint64_t new_gen = 0) noexcept {
    if (arena_id == 0) {
        // "matches all" — every shard, index order (deadlock-safe).
        std::size_t total = 0;
        for (std::size_t i = 0; i < kPinRegistryShardCount; ++i) {
            auto& shard = pin_registry_shards()[i];
            std::lock_guard<std::mutex> lock(shard.mtx);
            for (auto* p : shard.pins) {
                if (!p || !p->pinned())
                    continue;
                // restamp(new_gen, 0) keeps each pin's arena_id (0 = keep).
                p->restamp(new_gen, /*new_arena_id=*/0);
                ++total;
            }
        }
        return total;
    }
    // Arena-specific: single shard only (LifetimePin routes by arena_id).
    const auto idx = pin_registry_shard_index(arena_id);
    auto& shard = pin_registry_shards()[idx];
    std::lock_guard<std::mutex> lock(shard.mtx);
    std::size_t n = 0;
    for (auto* p : shard.pins) {
        if (!p || !p->pinned())
            continue;
        if (p->arena_id() != arena_id)
            continue;
        p->restamp(new_gen, arena_id);
        ++n;
    }
    return n;
}

// Invalidate every live LifetimePin tied to `arena_id`. Returns # invalidated.
// Issue #2375: arena_id == 0 walks all shards (GC FFI pin sweep at
// evaluator_gc.cpp); arena_id != 0 walks one shard only.
inline std::size_t invalidate_all_pins_for_arena(std::uint64_t arena_id) noexcept {
    if (arena_id == 0) {
        std::size_t total = 0;
        for (std::size_t i = 0; i < kPinRegistryShardCount; ++i) {
            auto& shard = pin_registry_shards()[i];
            std::lock_guard<std::mutex> lock(shard.mtx);
            for (auto* p : shard.pins) {
                if (!p || !p->pinned())
                    continue;
                p->unpin_on_compact();
                ++total;
            }
        }
        return total;
    }
    const auto idx = pin_registry_shard_index(arena_id);
    auto& shard = pin_registry_shards()[idx];
    std::lock_guard<std::mutex> lock(shard.mtx);
    std::size_t n = 0;
    for (auto* p : shard.pins) {
        if (!p || !p->pinned())
            continue;
        if (p->arena_id() != arena_id)
            continue;
        p->unpin_on_compact();
        ++n;
    }
    return n;
}

// Issue #2374 / #2265: selective invalidate after Moving densify.
// Walks all pin_registry_shards (legacy pin_registry() was empty post-#2342
// and the densify walk there was a no-op). Skips pins whose ptr_ is already
// a remapped new address (in `new_addrs`). Non-remapped pins for this arena
// are fail-closed via unpin_on_compact. Distinct from verify_pins_under_
// moving_compact (which only detects pins still pointing at *old* densified
// addresses). Returns # invalidated.
inline std::size_t
invalidate_pins_not_in_new_addrs(std::uint64_t arena_id,
                                 const std::unordered_set<void*>& new_addrs) noexcept {
    std::size_t invalidated = 0;
    for (std::size_t i = 0; i < kPinRegistryShardCount; ++i) {
        auto& shard = pin_registry_shards()[i];
        std::lock_guard<std::mutex> lock(shard.mtx);
        for (auto* p : shard.pins) {
            if (!p || !p->pinned())
                continue;
            if (arena_id != 0 && p->arena_id() != arena_id)
                continue;
            // Skip remapped pins (their ptr_ is in new_addrs).
            if (new_addrs.count(p->ptr()) > 0)
                continue;
            p->unpin_on_compact();
            ++invalidated;
        }
    }
    return invalidated;
}

// Issue #2265: Moving densify walks `last_object_remap_` (built by
// relocate_tracked_objects_for_moving_) and remaps every live pin whose
// ptr_ matches the old address. Pins not present in the registry
// (stale ptr_ or destroyed between relocate + remap walk) bump
// `remap_misses` so dashboards can flag orphaned remap entries.
// arena_id_filter (0 = any arena) narrows the walk to one arena.
// O(pin_count); only called on Moving densify success path (AC3
// zero-cost guarantee holds for Soft/Force / no-compact).
// Returns (remapped_count, miss_count).
struct RemapResult {
    std::size_t remapped = 0;
    std::size_t misses = 0;
};
inline RemapResult remap_pins_pointing_to(void* old_ptr, void* new_ptr, std::uint64_t new_gen = 0,
                                          std::uint64_t arena_id_filter = 0) noexcept {
    RemapResult out{};
    if (!old_ptr || !new_ptr)
        return out;
    // Issue #2342: iterate all shards in shard-index order (deadlock-safe).
    // arena_id_filter == 0 means "any arena" so all shards are visited.
    // arena_id_filter != 0 still visits all shards but only processes
    // pins whose arena_id matches (filter inside loop body).
    for (std::size_t i = 0; i < kPinRegistryShardCount; ++i) {
        auto& shard = pin_registry_shards()[i];
        std::lock_guard<std::mutex> lock(shard.mtx);
        for (auto* p : shard.pins) {
            if (!p || !p->pinned())
                continue;
            if (arena_id_filter != 0 && p->arena_id() != arena_id_filter)
                continue;
            if (p->ptr() != old_ptr) {
                // No match: bump miss only if this pin's arena IS the filter
                // (otherwise the pin belongs to a different arena entirely
                // and isn't a candidate for this remap).
                if (arena_id_filter != 0 && p->arena_id() == arena_id_filter)
                    ++g_lifetime_pin_stats.remap_misses;
                continue;
            }
            p->remap(new_ptr, new_gen);
            ++out.remapped;
        }
    }
    return out;
}

// Aggregate stats convenience (Phase 3): # remap calls invoked across
// the registry. Useful for tests + observability snapshots.
inline std::uint64_t lifetime_pin_remap_total() noexcept {
    return g_lifetime_pin_stats.remaps;
}
inline std::uint64_t lifetime_pin_remap_miss_total() noexcept {
    return g_lifetime_pin_stats.remap_misses;
}

// Issue #2266: # Moving compact runs where verify_pins_under_moving_compact
// fail-closed returned false. Mirrors the process-level atomic for tests +
// observability snapshots.
// Issue #2268 fixup: declaration moved up from below (was line 355 in
// #2266) so the inline reader function (mergebot's #2263 added a second
// definition after the atomics block; we keep that one and drop the
// pre-atomics duplicate that would otherwise clash with it) sees the
// atomic without an ODR-use-before-declaration error in C++20 module
// compilation.
inline std::atomic<std::uint64_t> g_moving_compact_pin_contract_fail_total{0};

// Issue #2324: the AC5 reader function for the contract-fail counter.
// The atomic above is bumped by verify_pins_under_moving_compact when
// it returns false (#2266 contract). test_moving_compact_2166.cpp's
// AC_M6 negative test reads this counter to verify the bump. The
// function was missing from the source (the comment above said it was
// 'declared earlier in the file' but never actually defined), causing
// the build to fail on `tests/core/test_moving_compact_2166.cpp` with
// "lifetime_pin_contract_fail_total is not a member of aura::core::lifetime".
inline std::uint64_t lifetime_pin_contract_fail_total() noexcept {
    return g_moving_compact_pin_contract_fail_total.load(std::memory_order_relaxed);
}
// Per-shard pin count (takes the shard's lock briefly). Cheap
// enough for AC5 stress-test snapshots. Defined after LifetimePin
// so pinned() is a complete type.
inline std::size_t pin_registry_shard_pin_count(std::size_t shard_idx) noexcept {
    if (shard_idx >= kPinRegistryShardCount)
        return 0;
    auto& s = pin_registry_shards()[shard_idx];
    std::lock_guard<std::mutex> lock(s.mtx);
    std::size_t n = 0;
    for (auto* p : s.pins)
        if (p && p->pinned())
            ++n;
    return n;
}

// Max pin count across all shards (for AC4 load-balance dashboards).
inline std::size_t pin_registry_shard_max_pin_count() noexcept {
    std::size_t m = 0;
    for (std::size_t i = 0; i < kPinRegistryShardCount; ++i)
        m = std::max(m, pin_registry_shard_pin_count(i));
    return m;
}

// Total live pins across all shards (replacement for the existing
// live_pin_count() monotonic reader; preserved as a thin shim).
inline std::size_t pin_registry_total_pinned_count() noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < kPinRegistryShardCount; ++i) {
        auto& s = pin_registry_shards()[i];
        std::lock_guard<std::mutex> lock(s.mtx);
        for (auto* p : s.pins)
            if (p && p->pinned())
                ++total;
    }
    return total;
}

// Total live pins (for tests + observability). Issue #2342: delegate
// to pin_registry_total_pinned_count() which iterates all shards.
inline std::size_t live_pin_count() noexcept {
    return pin_registry_total_pinned_count();
}

// Issue #2256: Moving-compact hard contract. Every live pointer
// that crosses a Moving compact must be either pinned (still valid
// across the move) OR explicitly remapped. The pin-or-remap
// accumulator tracks how many live pins were honored (pin_hits) and
// how many had to be remapped (remap_us). When pin_count >= some
// bound, Moving compact yields to Force — keeps the hard contract.
inline std::atomic<std::uint64_t> g_moving_compact_pin_hits_total{0};
inline std::atomic<std::uint64_t> g_moving_compact_remap_us_total{0};
inline std::atomic<std::uint64_t> g_moving_compact_count_total{0};
inline std::atomic<std::uint64_t> g_moving_compact_bytes_reclaimed_total{0};
// Issue #2266: # Moving compact runs where a live pin still pointed at an
// old densified address after the remap walk (i.e., the remap walked the
// registry but missed this pin). Bumped by verify_pins_under_moving_compact
// when it returns false. Production gates on this counter to detect silent
// pin-or-remap contract violations under sustained AI multi-round self-mod.
// Moved up from below (was line 351 in #2266) so the inline reader
// lifetime_pin_contract_fail_total() declared earlier in the file
// can see the atomic without an ODR-use-before-declaration error
// (#2268 fixup).

// Issue #2280: forward declaration — verify_pins_under_moving_compact
// (defined below) calls verify_linear_pins_under_moving_compact
// (defined further below). Declare the latter here so the wrapper
// resolves the name without an ODR-use-before-declaration error.
inline bool
verify_linear_pins_under_moving_compact(const std::unordered_set<void*>& old_addresses) noexcept;

// Hard-contract verification: under Moving compact, every live pin
// must be honored (return true if honored, false if compact must yield).
// Cost model: O(pin_count). The AC3 zero-cost guarantee holds when
// no Moving compact runs (the function is never called from the
// production hot path — only from the compact driver).
//
// Issue #2266: previously this function ALWAYS returned true (observe-only).
// It now fail-closes if any pin for `arena_id` still has `ptr_` that
// appears as a key in the densify's `old_addresses` set (meaning the
// remap walked the registry but missed this pin). The caller (Moving
// densify site in arena.ixx) passes the set of `last_object_remap_` keys
// (the OLD addresses that were just densified) so the check is exact:
// if any pin still points at an old address, the remap missed it.
//
// Issue #2280: this function also verifies the *linear* pin contract
// (verify_linear_pins_under_moving_compact below). Live linear roots
// (linear_rt::Owned|Borrowed|MutBorrowed) must be either remapped to
// a new address or removed from the linear_roots registry before the
// Moving compact is allowed to proceed. The combined result is
// returned: if either check fails, the compact must yield.
inline bool
verify_pins_under_moving_compact(std::uint64_t arena_id,
                                 const std::unordered_set<void*>& old_addresses) noexcept {
    // Issue #2342: iterate all shards in shard-index order
    // (deadlock-safe — matches the per-call-site lock ordering used by
    // remap_pins_pointing_to). The global lock is replaced by N
    // short shard locks (one per shard, in order). arena_id != 0
    // filters inside the loop body (pins with mismatched arena are
    // skipped). arena_id == 0 visits every pinned entry.
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t honored = 0;
    bool arena_ok = true;
    for (std::size_t i = 0; i < kPinRegistryShardCount; ++i) {
        auto& shard = pin_registry_shards()[i];
        std::lock_guard<std::mutex> lock(shard.mtx);
        for (auto* p : shard.pins) {
            if (!p || !p->pinned())
                continue;
            if (arena_id != 0 && p->arena_id() != arena_id)
                continue;
            // Contract: pin's ptr_ must NOT be in old_addresses (it was either
            // remapped to a new address, or invalidated). If pin still points
            // at an old address, the remap walk missed it → fail closed.
            if (old_addresses.count(p->ptr()) > 0) {
                g_moving_compact_pin_contract_fail_total.fetch_add(1, std::memory_order_relaxed);
                g_moving_compact_pin_hits_total.fetch_add(honored, std::memory_order_relaxed);
                g_moving_compact_remap_us_total.fetch_add(
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count()),
                    std::memory_order_relaxed);
                g_moving_compact_count_total.fetch_add(1, std::memory_order_relaxed);
                arena_ok = false;
                // Release locks (RAII) on return; continue scanning
                // remaining shards to honor the rest of the contract
                // (no skip — caller will see false and yield the
                // compact, but the contract counters reflect every
                // miss). For correctness, break out of the loop
                // early but still bump counters for each remaining
                // miss. Trade-off: skip remaining misses (faster)
                // vs. scan all (more accurate counters). For #2342,
                // skip remaining misses (call to yield is the primary
                // signal; missed pin total is a follow-up enrichment).
                break;
            }
            ++honored;
        }
        if (!arena_ok)
            break;
    }
    if (arena_ok) {
        const auto t1 = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        g_moving_compact_pin_hits_total.fetch_add(honored, std::memory_order_relaxed);
        g_moving_compact_remap_us_total.fetch_add(static_cast<std::uint64_t>(us),
                                                  std::memory_order_relaxed);
        g_moving_compact_count_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (!arena_ok)
        return false;
    // Issue #2280: linear pin check (separate mutex; no deadlock risk
    // because we already released all shard locks via RAII).
    return verify_linear_pins_under_moving_compact(old_addresses);
}

// Issue #2280: epoch-scoped linear pin contract. Live linear objects
// (linear_rt::Owned|Borrowed|MutBorrowed) must be registered as pin
// roots at LinearWrap materialization time and unregistered at Move
// consume / Drop time. verify_pins_under_moving_compact extends to
// check that every live linear root is either remapped to a new
// address (in the densify's old_addresses → must have a new mapping)
// or no longer present in the linear_roots registry (Drop/Move
// consume ran). If a live linear root is still in old_addresses after
// the remap walk, the compact missed it → bump linear_pin_miss_total
// + fail closed (the arena caller's verify_pins_under_moving_compact
// will yield the compact).
//
// Design: a simple registry (unordered_set<void*>) tracks live linear
// roots. pin_linear_root / unpin_linear_root are inline free functions
// callable from the JIT lowering (aura_jit.cpp OpLinearWrap/OpMoveOp/
// OpDropOp) and the env frame binding paths (evaluator_env.cpp).
// verify_linear_pins_under_moving_compact iterates the registry and
// checks each root.
//
// AC3 (zero extra atomics when no linear pins): the linear_roots
// registry is a no-op when empty (early-return below), and the
// linear pin check is O(linear_root_count), not O(all pins).
inline std::atomic<std::uint64_t> g_linear_pin_total{0};
inline std::atomic<std::uint64_t> g_linear_unpin_total{0};
inline std::atomic<std::uint64_t> g_linear_pin_miss_total{0};

// Registry of live linear roots (addresses that must survive Moving
// compact). Function-static so it's process-wide and accessible from
// inline free functions. Mutex-guarded for thread safety.
inline std::unordered_set<void*>& linear_roots() {
    static std::unordered_set<void*> s;
    return s;
}
inline std::mutex& linear_roots_mtx() {
    static std::mutex m;
    return m;
}

// Pin a live linear root. Adds to the registry + bumps counter.
// Called from OpLinearWrap runtime execution (aura_jit.cpp) and from
// env frame binding paths when a slot transitions to
// linear_rt::Owned|Borrowed|MutBorrowed.
inline void pin_linear_root(void* obj) noexcept {
    if (!obj)
        return;
    std::lock_guard<std::mutex> lock(linear_roots_mtx());
    linear_roots().insert(obj);
    g_linear_pin_total.fetch_add(1, std::memory_order_relaxed);
}

// Unpin a live linear root (on Move consume / Drop). Removes from
// the registry + bumps counter.
inline void unpin_linear_root(void* obj) noexcept {
    if (!obj)
        return;
    std::lock_guard<std::mutex> lock(linear_roots_mtx());
    linear_roots().erase(obj);
    g_linear_unpin_total.fetch_add(1, std::memory_order_relaxed);
}

// Snapshot for tests + observability.
struct LinearRootSnapshot {
    std::uint64_t pin_total = 0;
    std::uint64_t unpin_total = 0;
    std::uint64_t pin_miss_total = 0;
    std::size_t live_count = 0;
};
inline LinearRootSnapshot linear_root_snapshot() noexcept {
    LinearRootSnapshot s;
    s.pin_total = g_linear_pin_total.load(std::memory_order_relaxed);
    s.unpin_total = g_linear_unpin_total.load(std::memory_order_relaxed);
    s.pin_miss_total = g_linear_pin_miss_total.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(linear_roots_mtx());
    s.live_count = linear_roots().size();
    return s;
}

// Reset for tests only. Production leaves linear_roots alone (the
// live linear bindings are the source of truth).
inline void reset_linear_roots_for_test() noexcept {
    std::lock_guard<std::mutex> lock(linear_roots_mtx());
    linear_roots().clear();
    g_linear_pin_total.store(0, std::memory_order_relaxed);
    g_linear_unpin_total.store(0, std::memory_order_relaxed);
    g_linear_pin_miss_total.store(0, std::memory_order_relaxed);
}

// ── Issue #2298: pin-or-fail for non-render general objects ─────────
// Fail-closed pin of a densify-tracked create object (or any cross-
// boundary buffer that is neither EnvFrame-fenced nor Render/FFI).
// Returns false if p is null (does not pin). Bumps general_object_pin_total.
inline bool pin_or_fail(LifetimePin& pin, void* p, std::uint64_t gen,
                        std::uint64_t arena_id = 0) noexcept {
    if (!p)
        return false;
    pin.pin(p, gen, arena_id);
    ++g_lifetime_pin_stats.general_object_pin_total;
    return true;
}

// Validate a general-object pin after compact / densify. On success after
// Moving densify (gen advanced), bumps general_object_pin_remap_ok_total
// when the pin is still live (ptr remapped or address-stable). On fail
// bumps general_object_pin_validate_fail_total (AC2 fail-closed).
inline bool validate_general_object(const LifetimePin& pin, std::uint64_t cur_gen,
                                    std::uint64_t cur_arena_id = 0,
                                    bool count_remap_ok = false) noexcept {
    if (!pin.validate(cur_gen, cur_arena_id)) {
        ++g_lifetime_pin_stats.general_object_pin_validate_fail_total;
        return false;
    }
    if (count_remap_ok)
        ++g_lifetime_pin_stats.general_object_pin_remap_ok_total;
    return true;
}

// RAII helper for non-render intermediate buffers (mutate/agent/scratch).
// Uses LifetimePin under the hood so Moving densify remaps via the
// existing pin registry (#2265). Not for AST nodes (use StableNodeRef).
class GeneralObjectPin {
public:
    GeneralObjectPin() = default;
    ~GeneralObjectPin() = default;
    GeneralObjectPin(const GeneralObjectPin&) = delete;
    GeneralObjectPin& operator=(const GeneralObjectPin&) = delete;
    GeneralObjectPin(GeneralObjectPin&&) = default;
    GeneralObjectPin& operator=(GeneralObjectPin&&) = default;

    // Pin buffer; returns false on null. Arena ownership (not FFI).
    // NOTE: replaces any prior ptr on this pin (LifetimePin::pin overwrites).
    // For pool+flat pairs use two GeneralObjectPin instances (see
    // wire_general_object_create_pair — Issue #2363).
    bool pin(void* p, std::uint64_t gen, std::uint64_t arena_id = 0) noexcept {
        return pin_or_fail(pin_, p, gen, arena_id);
    }

    [[nodiscard]] bool validate(std::uint64_t cur_gen, std::uint64_t cur_arena_id = 0,
                                bool count_remap_ok = false) const noexcept {
        return validate_general_object(pin_, cur_gen, cur_arena_id, count_remap_ok);
    }

    [[nodiscard]] void* ptr() const noexcept { return pin_.ptr(); }
    [[nodiscard]] bool pinned() const noexcept { return pin_.pinned(); }
    [[nodiscard]] LifetimePin& raw() noexcept { return pin_; }
    [[nodiscard]] const LifetimePin& raw() const noexcept { return pin_; }

private:
    LifetimePin pin_;
};

// Issue #2337 / #2363: bump wire counter once per adopted create site.
inline void note_general_object_pin_mutate_wire() noexcept {
    ++g_lifetime_pin_stats.general_object_pin_mutate_wire_total;
}

// Issue #2363: pin both intermediate create buffers (typically
// StringPool + FlatAST). Uses two GeneralObjectPin objects because
// LifetimePin::pin replaces the prior pointer. Bumps wire total once
// per call site. Soft / null args still bump wire (site was adopted).
inline bool wire_general_object_create_pair(GeneralObjectPin& pin_a, GeneralObjectPin& pin_b,
                                            void* a, void* b, std::uint64_t gen = 0,
                                            std::uint64_t arena_id = 0) noexcept {
    const bool ok_a = a ? pin_a.pin(a, gen, arena_id) : false;
    const bool ok_b = b ? pin_b.pin(b, gen, arena_id) : false;
    note_general_object_pin_mutate_wire();
    return ok_a && ok_b;
}

// Issue #2280: linear pin check under Moving compact. Returns true if
// every live linear root is either remapped (not in old_addresses) or
// already removed from the registry. Bumps g_linear_pin_miss_total on
// miss. AC3: empty linear_roots() → no atomic ops, O(1) early-return.
inline bool
verify_linear_pins_under_moving_compact(const std::unordered_set<void*>& old_addresses) noexcept {
    std::lock_guard<std::mutex> lock(linear_roots_mtx());
    auto& roots = linear_roots();
    if (roots.empty())
        return true; // AC3: no linear pins → no extra atomics
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t honored = 0;
    for (auto* root : roots) {
        // Contract: live linear root must NOT be in old_addresses (it
        // was either remapped to a new address, or the binding was
        // dropped/moved). If a live linear root is still in
        // old_addresses, the Moving densify missed it → fail closed.
        if (old_addresses.count(root) > 0) {
            g_linear_pin_miss_total.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ++honored;
    }
    // No atomics bumped on the happy path (AC3: honored count is
    // for tests, not a per-run counter — the existing arena check
    // already records g_moving_compact_count_total via the wrapper).
    (void)t0;
    (void)honored;
    return true;
}

} // namespace aura::core::lifetime