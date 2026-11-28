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

#include "core/runtime_config.hh"
#include "runtime_shared.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "core/ownership.hh" // #4936 c_alloc / c_free

import std;

// JIT runtime registers the real lock/top-cell clearer. Null when JIT
// SOs are not loaded — ~CompilerService still resolves this symbol from
// libaura_tl_arena.so (never a weak stub in test_objects).
static std::atomic<void (*)(void*)> g_eval_runtime_hook_clearer{nullptr};

extern "C" void aura_register_evaluator_runtime_hook_clearer(void (*fn)(void*)) {
    g_eval_runtime_hook_clearer.store(fn, std::memory_order_release);
}

extern "C" void aura_clear_evaluator_runtime_hooks(void* user) {
    if (auto fn = g_eval_runtime_hook_clearer.load(std::memory_order_acquire))
        fn(user);
}

// ── Pair storage (JIT runtime + evaluator car/cdr fallback) ──
// Issue #898: fixed directory of segments. A segment is never relocated.
// 1024 slots × 65536 segments = 2^26 pairs. The directory is 512KB of
// atomics, zero-initialized, and does not grow.
namespace {
constexpr std::size_t kPairSegSlots = 1024;
constexpr std::size_t kPairMaxSegs = 1u << 16;

struct PairSegment {
    std::atomic<PairSlot*> slots[kPairSegSlots];
    std::atomic<std::uint64_t> tenants[kPairSegSlots];
    PairSegment() {
        for (std::size_t i = 0; i < kPairSegSlots; ++i) {
            slots[i].store(nullptr, std::memory_order_relaxed);
            tenants[i].store(0, std::memory_order_relaxed);
        }
    }
};

std::atomic<PairSegment*>* pair_seg_dir() {
    static std::atomic<PairSegment*> table[kPairMaxSegs];
    return table;
}

PairSegment* pair_segment(std::size_t id, bool create) {
    const std::size_t seg_index = id / kPairSegSlots;
    if (seg_index >= kPairMaxSegs)
        return nullptr;
    std::atomic<PairSegment*>& cell = pair_seg_dir()[seg_index];
    if (PairSegment* existing = cell.load(std::memory_order_acquire))
        return existing;
    if (!create)
        return nullptr;
    auto fresh = std::make_unique<PairSegment>();
    PairSegment* expected = nullptr;
    if (cell.compare_exchange_strong(expected, fresh.get(), std::memory_order_release,
                                     std::memory_order_acquire))
        return fresh.release(); // the directory owns it for the process lifetime
    return expected;
}

// Thread-local cons slabs (#898). Linked once so process exit can free
// them without a per-cons vector push under the workspace write lock.
// Fixed-size slot array: a flexible array member is not ISO C++ and
// test_ir builds with -Wpedantic -Werror.
constexpr std::uint32_t kPairSlabCap = 64;
struct PairSlab {
    PairSlab* next;
    std::uint32_t cap;
    PairSlot slots[kPairSlabCap];
};
std::atomic<PairSlab*> g_pair_slabs{nullptr};
std::atomic<std::uint64_t> g_pair_slab_publishes{0};

struct TlsPairBump {
    PairSlab* slab = nullptr;
    std::uint32_t used = 0;
};
thread_local TlsPairBump tls_pair_bump{};

// One shared fetch_add per cons does not scale: 8 threads on one cache
// line were slower than a single thread (~0.06× aggregate). Claim a
// segment-sized run (1024) so the cons hot path pays that atomic once
// per batch. Unpublished ids in the run stay nullptr; readers treat
// them as absent. clear() bumps the epoch so a thread drops a stale run
// instead of republishing into the pre-reset index space.
constexpr std::uint64_t kPairIdBatch = 1024;
std::atomic<std::uint64_t> g_pair_claim_epoch{1};

struct TlsPairIds {
    std::uint64_t next = 0;
    std::uint64_t end = 0;
    std::uint64_t epoch = 0;
};
thread_local TlsPairIds tls_pair_ids{};
} // namespace

PairSlotTable g_pair_slots;

// Issue #4571 (L2): inline car/cdr reads dir[id >> 10]->slots[id & 1023]
// with two acquire loads (aura_jit.cpp emit_l2_pair_field). These asserts
// pin the layout that lowering hard-codes (kL2PairSegSlots/kL2PairMaxSegs).
static_assert(kPairSegSlots == 1024 && kPairMaxSegs == (1u << 16),
              "#4571: aura_jit.cpp kL2PairSegSlots / kL2PairMaxSegs drift");
static_assert(offsetof(PairSegment, slots) == 0, "#4571: PairSegment::slots must lead");
static_assert(sizeof(std::atomic<PairSlot*>) == sizeof(PairSlot*) &&
                  std::atomic<PairSlot*>::is_always_lock_free,
              "#4571: inline load needs a plain-pointer atomic cell");
static_assert(sizeof(std::atomic<PairSegment*>) == sizeof(PairSegment*),
              "#4571: directory cell is a plain pointer");
static_assert(offsetof(PairSlot, car) == 0 && offsetof(PairSlot, cdr) == 8,
              "#4571: inline car/cdr field offsets");

extern "C" void* aura_pair_seg_dir_base() {
    return static_cast<void*>(pair_seg_dir());
}

// Issue #5636: bytes obtained for pair segments, cons slabs, and owned
// slots. The directory walk is a snapshot; cons itself is unchanged.
extern "C" std::uint64_t aura_pair_storage_bytes() noexcept {
    std::uint64_t bytes = 0;
    std::atomic<PairSegment*>* dir = pair_seg_dir();
    for (std::size_t i = 0; i < kPairMaxSegs; ++i) {
        if (dir[i].load(std::memory_order_relaxed) != nullptr)
            bytes += sizeof(PairSegment);
    }
    for (PairSlab* s = g_pair_slabs.load(std::memory_order_acquire); s != nullptr; s = s->next)
        bytes += sizeof(PairSlab);
    std::scoped_lock<std::mutex> lock(g_owned_pair_slots_mtx);
    bytes += static_cast<std::uint64_t>(g_owned_pair_slots_.capacity()) * sizeof(PairSlot*);
    bytes += static_cast<std::uint64_t>(g_owned_pair_slots_.size()) * sizeof(PairSlot);
    return bytes;
}

// IR MakePair mirror still records one malloc per interpreter cons.
// Arena slots and JIT slabs are not listed here.
//
// Issue #4755: process-global storage, process-global lock. See the note on
// the declaration in runtime_shared.h: the evaluator workspace hook is not
// a process-wide exclusion (single bound Evaluator, and a write acquisition
// adopts a shared hold), so two fiber bodies could push_back here at once.
std::vector<PairSlot*> g_owned_pair_slots_;
std::mutex g_owned_pair_slots_mtx;

// Issue #4747 (application face): how many BODIES are inside an IR drain right
// now, process-wide.
//
// An IR drain keeps its live values in IRInterpreter::call_stack_ / cell_heap_ /
// linear_heap_ / the const caches, and publishes them only through
// ir_inflight::g_providers, which is THREAD-LOCAL. A file-heap reclaim started
// from a sibling fiber's interpreter therefore cannot mark another body's IR
// locals, args or cells, and frees them: the sibling reads Void / () back (the
// #4747 "expected Int, got Void" face, now silent because the app's guard rail
// just sees an empty guard list). The existing stand-down counts only PLAIN
// tree-walkers (#2738 / e5ba83aad); a CLI/serve fiber body is a fiber-owned
// segment, so the count is 0 for it and the sweep proceeds.
//
// A collector that cannot certify a live body must stand down (the #2738 rule);
// this counter is the certification input for the IR-body class, together with
// the drain depth so the sweeping body never counts itself.
// The count is owned by the IRInterpreter that is draining (one instance per
// define), NOT by the thread: a body can yield inside IR and resume on another
// worker (#4716), so a thread-local depth would leak a body that migrated. The
// sweeping interpreter subtracts its own instance.
std::atomic<int> g_ir_active_drains{0};

extern "C" int aura_ir_active_drains_total() noexcept {
    // Issue #4791: seq_cst, the sweep half of the drain-begin handshake below.
    return g_ir_active_drains.load(std::memory_order_seq_cst);
}

// Issue #4791: the drain-begin half of the #4768 sweep gate. Defined with the
// gate further down; forward-declared here so the drain count can use it.
extern "C" void aura_gc_sweep_gate_wait_for_body() noexcept;

extern "C" void aura_ir_drain_begin() noexcept {
    // Issue #4791: a serve-scheduled body is NOT counted by
    // aura_fiber_body_enter (only the CLI thread backend is); its only
    // stand-down input is this drain count. Publishing it was check-then-act
    // from the sweep's side: a sweep that had already asserted the gate and
    // read the count could not see a drain that began after the read, and the
    // body then minted into slots that sweep released (#4791: held list
    // spliced / sibling mint harvested with SIMULTANEOUS spawn). Same Dekker
    // shape as the body counter: publish (seq_cst) BEFORE testing the gate,
    // and wait a gate that is held out. Either the sweep's read sees this
    // drain (and stands down) or this drain sees the sweep (and waits until
    // its release is done).
    g_ir_active_drains.fetch_add(1, std::memory_order_seq_cst);
    aura_gc_sweep_gate_wait_for_body();
}

extern "C" void aura_ir_drain_end() noexcept {
    g_ir_active_drains.fetch_sub(1, std::memory_order_acq_rel);
}

// Issue #4768: AURA_GC_RECLAIM_OFF is the control-arm knob for the file-heap
// sweeps. It used to gate ONLY the IR-side sweep, so a "destructive GC off"
// arm still ran the tree-walker sweep and every experiment that read the knob
// as "collector off" (including #4768's own original premise) measured a
// process in which half the collector was still running. BOTH sweeps now
// consult it, and each one counts the sweep it disarmed. The pair is the
// probe: a control arm that reads (query:reclaim-off-skips-ir/-tw) can prove both
// halves were skipped instead of inferring it from one half's silence.
//
// The counters are bumped only when the sweep had ALREADY passed its
// allocation trigger and its stand-down checks - i.e. when the sweep would
// have run but for the knob. A plain counter-at-entry would be satisfied by
// unrelated stand-downs and prove nothing.
std::atomic<std::uint64_t> g_gc_reclaim_off_skips_ir{0};
std::atomic<std::uint64_t> g_gc_reclaim_off_skips_tw{0};

extern "C" void aura_gc_reclaim_off_note_ir_skip() noexcept {
    g_gc_reclaim_off_skips_ir.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aura_gc_reclaim_off_note_tw_skip() noexcept {
    g_gc_reclaim_off_skips_tw.fetch_add(1, std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_gc_reclaim_off_skips_ir_total() noexcept {
    return g_gc_reclaim_off_skips_ir.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_gc_reclaim_off_skips_tw_total() noexcept {
    return g_gc_reclaim_off_skips_tw.load(std::memory_order_relaxed);
}

// Issue #4768: the quiescence gate for the file-heap sweeps.
//
// The stand-down counters decide whether a sweep may run, but a fiber body can
// START between that decision and the release. It then mints into slots the
// mark never saw - a fresh closure, or a reused pair/vector index - and the
// release frees them underneath the body ("invalid closure: eval_flat",
// "expected Int, got Pair"). The gate closes the window in both directions:
// a sweep asserts it BEFORE it reads the counters, and a body asserts its own
// liveness BEFORE it tests the gate. With seq_cst on both sides either the
// sweep sees the body (and stands down) or the body sees the sweep (and waits
// it out); both being missed is excluded by the total order.
//
// The counters are process-wide: an Evaluator is shared by every fiber body,
// and the file heap is one heap. The thread-local depth lets a body's own
// thread exclude itself from "other bodies" (its own values are roots of the
// sweep it runs).
std::atomic<int> g_fiber_bodies_running{0};
std::atomic<bool> g_file_heap_sweep_active{false};
namespace {
thread_local int t_fiber_body_depth = 0;
// Issue #5274: this body is inside a certified park (aura_fiber_body_park).
thread_local int t_fiber_body_parked = 0;
thread_local bool t_sweep_gate_owner = false;
} // namespace

extern "C" bool aura_gc_sweep_try_begin() noexcept {
    if (t_sweep_gate_owner)
        return false; // nested sweep on the owning thread
    bool expected = false;
    if (!g_file_heap_sweep_active.compare_exchange_strong(expected, true,
                                                          std::memory_order_seq_cst))
        return false; // another thread owns the sweep
    t_sweep_gate_owner = true;
    return true;
}

extern "C" void aura_gc_sweep_end() noexcept {
    if (!t_sweep_gate_owner)
        return;
    t_sweep_gate_owner = false;
    g_file_heap_sweep_active.store(false, std::memory_order_seq_cst);
}

// Issue #4791: how often a body start (body enter or IR drain begin) found a
// sweep holding the gate - i.e. how often the check-then-act window was hit.
// Before #4791 a drain begin in this state minted straight under the release.
std::atomic<std::uint64_t> g_gc_sweep_gate_body_hits{0};
// Issue #4791: how many of those hits actually waited the sweep out (0 under
// the AURA_GC_GATE_DRAIN_NOWAIT control arm for the drain half).
std::atomic<std::uint64_t> g_gc_sweep_gate_body_waits{0};

// Issue #4791: the walker safepoint (evaluator_eval_flat.cpp). Weak: this TU
// also links into units without the tree walker.
extern "C" void aura_tw_safepoint() noexcept __attribute__((weak));

namespace {
// Issue #4791: the measurement control arm. AURA_GC_GATE_DRAIN_NOWAIT=1 keeps
// the drain begin publishing but makes it NOT wait a held gate - the pre-#4791
// behaviour - so a single binary can show the window being hit and the
// corruption it causes. Read once; a test flips it through
// aura_gc_sweep_gate_set_drain_nowait.
std::atomic<int> g_gate_drain_nowait{-1};
bool gate_drain_nowait() noexcept {
    int v = g_gate_drain_nowait.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = ::aura::config::raw(::aura::config::Key::AURA_GC_GATE_DRAIN_NOWAIT);
        v = (e != nullptr && *e != 0 && *e != '0') ? 1 : 0;
        g_gate_drain_nowait.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

void gate_wait_held_sweep(bool may_skip) noexcept {
    // A sweep owner never waits on its own gate (a native finalizer inside a
    // sweep may re-enter Aura and start a body); that path is covered by the
    // newborn bounds instead.
    if (t_sweep_gate_owner || !g_file_heap_sweep_active.load(std::memory_order_seq_cst))
        return;
    g_gc_sweep_gate_body_hits.fetch_add(1, std::memory_order_relaxed);
    if (may_skip && gate_drain_nowait())
        return;
    g_gc_sweep_gate_body_waits.fetch_add(1, std::memory_order_relaxed);
    while (g_file_heap_sweep_active.load(std::memory_order_seq_cst)) {
        // The sweep may be a tree-walker collector waiting (bounded) for this
        // body's attached fiber segment to park. A waiting body is between
        // frames, so park it instead of letting that collector time out.
        if (aura_tw_safepoint != nullptr)
            aura_tw_safepoint();
        std::this_thread::yield();
    }
}
} // namespace

extern "C" void aura_gc_sweep_gate_wait_for_body() noexcept {
    gate_wait_held_sweep(/*may_skip=*/true);
}

extern "C" std::uint64_t aura_gc_sweep_gate_body_hits_total() noexcept {
    return g_gc_sweep_gate_body_hits.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_gc_sweep_gate_body_waits_total() noexcept {
    return g_gc_sweep_gate_body_waits.load(std::memory_order_relaxed);
}

extern "C" void aura_gc_sweep_gate_set_drain_nowait(int on) noexcept {
    g_gate_drain_nowait.store(on != 0 ? 1 : 0, std::memory_order_relaxed);
}

extern "C" void aura_fiber_body_enter() noexcept {
    g_fiber_bodies_running.fetch_add(1, std::memory_order_seq_cst);
    ++t_fiber_body_depth;
    // A sweep that asserted the gate before this body incremented has already
    // (or is about to) read the counters and will not see this body: wait for
    // it instead of minting under its release.
    gate_wait_held_sweep(/*may_skip=*/false);
}

extern "C" void aura_fiber_body_exit() noexcept {
    if (t_fiber_body_depth > 0)
        --t_fiber_body_depth;
    g_fiber_bodies_running.fetch_sub(1, std::memory_order_seq_cst);
}

extern "C" int aura_fiber_bodies_running_other() noexcept {
    const int self = t_fiber_body_depth > 0 ? 1 : 0;
    return g_fiber_bodies_running.load(std::memory_order_seq_cst) - self;
}

// Issue #5274: a body that BLOCKS in a primitive wait is certifiably parked —
// its tree-walker segment is published quiescent (tw_plain_park.h) and this
// counter stops claiming it as uncertifiable for that span. The body counter
// exists to cover a body whose live roots no collector can mark (#4768); a
// parked body's roots ARE markable through the segment registry, so keeping it
// in the count only froze every sweep for the lifetime of a long-lived
// --serve-async game (RSS ~850 MB, ~0.4-1.5 s per outermost mutate).
//
// The pair is asserted on the body's own thread, so t_fiber_body_depth needs
// no extra atomicity; a nested park keeps the outermost one authoritative
// (the caller collapses them). Release increments FIRST and then waits out a
// held sweep: a sweep that starts after the increment stands down on the
// counter, and one that already decided to mark this body is waited out before
// the segment is handed back to the owner.
extern "C" int aura_fiber_body_park() noexcept {
    if (t_fiber_body_depth <= 0 || t_fiber_body_parked != 0)
        return 0;
    t_fiber_body_parked = 1;
    g_fiber_bodies_running.fetch_sub(1, std::memory_order_seq_cst);
    return 1;
}

extern "C" void aura_fiber_body_unpark() noexcept {
    g_fiber_bodies_running.fetch_add(1, std::memory_order_seq_cst);
    t_fiber_body_parked = 0;
    gate_wait_held_sweep(/*may_skip=*/false);
}

// Issue #5274: the same wait for a plain walker that is NOT a body (the CLI /
// serve main thread blocked in accept / recv). AURA_GC_GATE_DRAIN_NOWAIT is a
// measurement arm for a different window (#4791); a parked walker may not skip
// this fence, or it resumes under a release that already marked its segment.
extern "C" void aura_gc_sweep_wait_parked_walker() noexcept {
    gate_wait_held_sweep(/*may_skip=*/false);
}

// Issue #4057: owner-principal stamps parallel to g_pair_slots — same SSOT
// placement so the evaluator partition TUs and the JIT SOs share one table
// (a definition living only in aura_jit_runtime.cpp would repeat the
// g_pair_slots undefined-symbol load-order failure this TU exists for).
PairTenantTable g_pair_slot_tenants;

// Issue #5518/#5631: JIT cons ids get their own range, above the evaluator's
// pairs_ index space. The evaluator sweep only looks at ids below
// pairs_.size(), so it can never void a JIT pair; the mark can classify an id
// by range alone. 1<<24 leaves the evaluator room for a multi-round
// serve-IR cons churn (#4747 mints ~1.2e6 pairs per round; unscoped ids
// are not recycled until process reset) without colliding with JIT ids.
// The base is derived from the configured cap so a small test cap still
// leaves a usable JIT range: 1<<24 normally, else a quarter of the cap.
static std::uint64_t pair_jit_base_5518();
static std::atomic<std::uint64_t> g_pair_jit_water{pair_jit_base_5518()};

extern "C" std::uint64_t aura_pair_jit_base() {
    return pair_jit_base_5518();
}

extern "C" std::uint64_t aura_pair_jit_high_water() {
    return g_pair_jit_water.load(std::memory_order_acquire);
}

namespace {
struct JitPairClaim {
    std::uint64_t next = 0;
    std::uint64_t end = 0;
    std::uint64_t epoch = 0;
};
thread_local JitPairClaim tls_jit_pair_ids;
} // namespace

PairSlot* PairSlotTable::operator[](std::size_t id) {
    // Issue #5518: below the JIT base every directory cell is addressable
    // (the evaluator's pairs_ index is the id; a cell may simply be empty).
    // Above it, only ids actually claimed.
    const std::uint64_t jw = g_pair_jit_water.load(std::memory_order_acquire);
    if (id >= pair_jit_base_5518() && id >= jw)
        return nullptr;
    PairSegment* seg = pair_segment(id, /*create=*/true);
    if (!seg)
        return nullptr;
    return seg->slots[id % kPairSegSlots].load(std::memory_order_acquire);
}

std::size_t PairSlotTable::size() const {
    // Issue #5518: readers use size() as the upper bound for BOTH id ranges
    // (evaluator pairs below the base, JIT pairs above it). Returning only
    // the low cursor made every evaluator-side car/cdr/set-car! reject a
    // live JIT pair as out of bounds (the OSR-resume face of #5631).
    const std::uint64_t low = high_water_.load(std::memory_order_acquire);
    const std::uint64_t jw = g_pair_jit_water.load(std::memory_order_acquire);
    return static_cast<std::size_t>(std::max(low, jw));
}

std::atomic<std::uint64_t> g_pair_capacity_exhausted_5518{0};

// Issue #5518: the pair id space is a fixed 2^26 directory and nothing frees
// an id, so a long-running JIT/AOT process eventually hits the cap. The old
// path returned fixnum 0 from cons (a silent data corruption). Fail loudly
// instead, and let AURA_PAIR_MAX_IDS shrink the cap for tests.
static std::uint64_t pair_id_limit_5518() {
    static const std::uint64_t limit = [] {
        std::uint64_t cap = static_cast<std::uint64_t>(kPairMaxSegs) * kPairSegSlots;
        if (const char* e = std::getenv("AURA_PAIR_MAX_IDS")) {
            const unsigned long long n = std::strtoull(e, nullptr, 10);
            if (n > 0 && n < cap)
                cap = n;
        }
        return cap;
    }();
    return limit;
}

static std::uint64_t pair_jit_base_5518() {
    static const std::uint64_t base = [] {
        const std::uint64_t want = 1ull << 24;
        const std::uint64_t limit = pair_id_limit_5518();
        return (limit > 2 * want) ? want : (limit / 4);
    }();
    return base;
}

static void note_pair_capacity_exhausted_5518() noexcept {
    const std::uint64_t n =
        g_pair_capacity_exhausted_5518.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
        std::fprintf(stderr, "aura: pair heap capacity exhausted (pair ids are not yet recycled); "
                             "cons now yields void instead of fixnum 0 (#5518)\n");
    }
}

// ── Issue #5518 phase 2: JIT slot roots ───────────────────────────────
// A root-mode JIT frame mirrors every tagged slot value into one contiguous
// array and pushes it here for the life of the call. The evaluator mark
// scans the chain, so a pair held only by a native frame survives a
// collection (#5631). Overflows disable reclamation (never the scan of the
// frames that did register).
namespace {
constexpr std::uint32_t kPairRootMax = 256;
struct PairRootRec {
    const std::int64_t* slots;
    std::uint32_t count;
};

// Issue #5518: the root chain is per EXECUTION CONTEXT, not per thread. A
// fiber carries its own context so a migrate between workers keeps push/pop
// balanced; the binder (Fiber::resume / trampoline) points TLS at it. The
// no-fiber path (interpreter, main thread) uses a lazily registered TLS
// fallback.
struct PairRootCtx {
    PairRootRec recs[kPairRootMax];
    std::uint32_t depth = 0;
    std::uint32_t overflow = 0;
    bool registered = false;
};

thread_local PairRootCtx t_pair_roots_thread;
thread_local PairRootCtx* t_pair_roots_cur = nullptr;

std::mutex g_pair_roots_reg_mtx;
std::vector<PairRootCtx*> g_pair_roots_registered;

void roots_register(PairRootCtx* c) {
    if (!c || c->registered)
        return;
    std::scoped_lock lock(g_pair_roots_reg_mtx);
    if (!c->registered) {
        g_pair_roots_registered.push_back(c);
        c->registered = true;
    }
}

void roots_unregister(PairRootCtx* c) {
    if (!c)
        return;
    std::scoped_lock lock(g_pair_roots_reg_mtx);
    if (!c->registered)
        return;
    auto it = std::find(g_pair_roots_registered.begin(), g_pair_roots_registered.end(), c);
    if (it != g_pair_roots_registered.end())
        g_pair_roots_registered.erase(it);
    c->registered = false;
}

// Deregister the fallback at thread exit: a stale pointer would be scanned.
struct ThreadRootGuard {
    ~ThreadRootGuard() { roots_unregister(&t_pair_roots_thread); }
};
thread_local ThreadRootGuard t_pair_roots_guard;

PairRootCtx* roots_ctx() {
    if (t_pair_roots_cur)
        return t_pair_roots_cur;
    roots_register(&t_pair_roots_thread);
    return &t_pair_roots_thread;
}

std::atomic<void (*)()> g_pair_gc_hook{nullptr};
std::atomic<std::uint32_t> g_pair_gc_running{0};
} // namespace

extern "C" void* aura_pair_roots_ctx_new() {
    auto* c = new PairRootCtx();
    roots_register(c);
    return c;
}

extern "C" void aura_pair_roots_ctx_free(void* ctx) {
    auto* c = static_cast<PairRootCtx*>(ctx);
    if (!c)
        return;
    roots_unregister(c);
    delete c;
}

extern "C" void aura_pair_roots_bind(void* ctx) {
    t_pair_roots_cur = static_cast<PairRootCtx*>(ctx);
}

extern "C" void aura_pair_roots_push(const std::int64_t* slots, std::uint32_t count) {
    if (!slots || count == 0)
        return;
    auto* c = roots_ctx();
    if (c->depth >= kPairRootMax) {
        c->overflow = 1;
        return;
    }
    c->recs[c->depth++] = PairRootRec{slots, count};
}

extern "C" void aura_pair_roots_pop() {
    auto* c = roots_ctx();
    if (c->depth != 0)
        --c->depth;
}

extern "C" void aura_pair_roots_scan(void (*cb)(std::int64_t, void*), void* ud) {
    if (!cb)
        return;
    auto* c = roots_ctx();
    for (std::uint32_t i = 0; i < c->depth; ++i) {
        const PairRootRec& rec = c->recs[i];
        for (std::uint32_t j = 0; j < rec.count; ++j)
            cb(rec.slots[j], ud);
    }
}

// Every registered execution context: used by the collector (all parked
// fibers + thread fallbacks) and by the sweep gate.
extern "C" void aura_pair_roots_scan_all(void (*cb)(std::int64_t, void*), void* ud) {
    if (!cb)
        return;
    std::scoped_lock lock(g_pair_roots_reg_mtx);
    for (auto* c : g_pair_roots_registered) {
        for (std::uint32_t i = 0; i < c->depth; ++i) {
            const PairRootRec& rec = c->recs[i];
            for (std::uint32_t j = 0; j < rec.count; ++j)
                cb(rec.slots[j], ud);
        }
    }
}

extern "C" std::uint32_t aura_pair_roots_depth() {
    return roots_ctx()->depth;
}

extern "C" std::uint64_t aura_pair_roots_total_depth() {
    std::uint64_t total = 0;
    std::scoped_lock lock(g_pair_roots_reg_mtx);
    for (auto* c : g_pair_roots_registered)
        total += c->depth;
    return total;
}

extern "C" std::uint32_t aura_pair_roots_overflowed() {
    auto* c = roots_ctx();
    if (c->overflow)
        return 1;
    std::scoped_lock lock(g_pair_roots_reg_mtx);
    for (auto* r : g_pair_roots_registered) {
        if (r->overflow)
            return 1;
    }
    return 0;
}

extern "C" void aura_set_pair_gc_hook(void (*fn)()) {
    g_pair_gc_hook.store(fn, std::memory_order_release);
}

// Run the evaluator collection from an allocation point. Re-entrant calls are
// ignored; the callback itself is only wired when an Evaluator exists.
extern "C" void aura_maybe_collect_pairs() {
    auto* fn = g_pair_gc_hook.load(std::memory_order_acquire);
    if (!fn)
        return;
    if (std::getenv("AURA_PAIR_GC_TRACE")) {
        std::fprintf(stderr, "[5518] collect seam frames=%llu roots=%u\n",
                     static_cast<unsigned long long>(aura_jit_frames_active()),
                     aura_pair_roots_depth());
    }
    std::uint32_t expected = 0;
    if (!g_pair_gc_running.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return;
    fn();
    g_pair_gc_running.store(0, std::memory_order_release);
}

extern "C" std::uint32_t aura_pair_gc_armed() {
    static const bool on = [] {
        // Issue #5518: root-mode reclamation is on by default now that the
        // frame roots, per-fiber contexts and the id ranges are in place.
        // AURA_PAIR_GC=0 is the kill switch.
        const char* v = std::getenv("AURA_PAIR_GC");
        return !(v && v[0] == '0');
    }();
    return on && aura_pair_roots_overflowed() == 0 ? 1u : 0u;
}

// ── Issue #5518 phase 2: JIT frame accounting ─────────────────────────
// The sweep may only free JIT pairs when no native frame can still hold one
// in its slot allocas. The JIT emits enter at the body entry and leave in
// every return; an unwind that skips the leave leaks the count upward, which
// keeps the sweep off (safe direction).
static std::atomic<std::uint64_t> g_jit_frames_active_5518{0};

extern "C" void aura_jit_frame_enter() {
    g_jit_frames_active_5518.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aura_jit_frame_leave() {
    std::uint64_t cur = g_jit_frames_active_5518.load(std::memory_order_relaxed);
    while (cur != 0) {
        if (g_jit_frames_active_5518.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed))
            return;
    }
}

std::uint64_t aura_jit_frames_active() {
    return g_jit_frames_active_5518.load(std::memory_order_relaxed);
}

// ── Issue #5518: pair id recycling ────────────────────────────────────
// An arena scope (tl_arena_push .. tl_arena_pop) mints ids from the fixed
// 2^26 directory. When the scope ends every non-escaping pair it minted is
// dead, so the ids go back on a free batch instead of being burned for the
// life of the process. Hand-off is batched per thread (kFreeBatch) so a
// cons that reuses an id takes no global lock; only a full/empty batch
// touches the pool.
namespace {
std::mutex g_pair_free_mtx;
std::vector<std::uint64_t> g_pair_free_ids;
} // namespace

extern "C" void aura_pair_id_recycle(std::uint64_t id) {
    TLarena& a = g_tl_arena;
    if (a.free_n >= TLarena::kFreeBatch) {
        std::scoped_lock lock(g_pair_free_mtx);
        g_pair_free_ids.insert(g_pair_free_ids.end(), a.free_ids, a.free_ids + a.free_n);
        a.free_n = 0;
    }
    a.free_ids[a.free_n++] = id;
}

std::uint64_t aura_pair_id_take_recycled() {
    TLarena& a = g_tl_arena;
    if (a.free_n == 0) {
        std::scoped_lock lock(g_pair_free_mtx);
        const std::size_t n = std::min<std::size_t>(TLarena::kFreeBatch, g_pair_free_ids.size());
        if (n == 0)
            return kNoRecycledPairId;
        std::copy(g_pair_free_ids.end() - static_cast<std::ptrdiff_t>(n), g_pair_free_ids.end(),
                  a.free_ids);
        g_pair_free_ids.resize(g_pair_free_ids.size() - n);
        a.free_n = static_cast<std::uint32_t>(n);
    }
    return a.free_ids[--a.free_n];
}

// Walk this thread's arena-pair recs back to the saved watermark, release
// each id and stop there. Called from tl_arena_pop / reset / destroy while
// the rec memory is still inside the arena (before the offset rewind).
extern "C" void aura_pair_scope_release_to(TLarena* arena, TLarena::PairIdRec* stop) {
    if (!arena)
        return;
    while (arena->pair_rec_head != stop) {
        auto* rec = arena->pair_rec_head;
        if (!rec)
            break; // defensive: watermark from another thread / lost chain
        g_pair_slots.release(static_cast<std::size_t>(rec->id), rec->slot);
        aura_pair_id_recycle(rec->id);
        arena->pair_rec_head = rec->prev;
    }
    arena->pair_rec_head = stop;
}

// Record a scoped arena pair. No-op outside a scope: an unscoped arena cons
// has no known death point, so its id keeps the old (leak-until-exit)
// contract.
extern "C" void aura_note_arena_pair(std::int64_t id, PairSlot* slot) {
    if (id < 0 || !slot)
        return;
    TLarena& a = g_tl_arena;
    if (a.mark_depth == 0)
        return;
    auto* rec = static_cast<TLarena::PairIdRec*>(
        tl_arena_alloc(&a, sizeof(TLarena::PairIdRec), alignof(TLarena::PairIdRec)));
    if (!rec)
        return;
    rec->id = static_cast<std::uint64_t>(id);
    rec->slot = slot;
    rec->prev = a.pair_rec_head;
    a.pair_rec_head = rec;
}

std::int64_t PairSlotTable::push_back(PairSlot* slot) {
    const auto limit = pair_id_limit_5518();
    for (;;) {
        // Issue #5518: a scoped pop returned ids to this thread's free
        // batch; reuse one before claiming fresh directory space. The
        // placeholder push_back(nullptr) path (IR MakePair grow) must not
        // consume recycled ids: it only advances size() for its own index.
        if (slot != nullptr) {
            const std::uint64_t rid = aura_pair_id_take_recycled();
            if (rid != kNoRecycledPairId) {
                PairSegment* rseg = pair_segment(static_cast<std::size_t>(rid), /*create=*/true);
                if (rseg) {
                    const std::size_t roff = static_cast<std::size_t>(rid) % kPairSegSlots;
                    rseg->tenants[roff].store(0, std::memory_order_release);
                    PairSlot* rexpected = nullptr;
                    if (rseg->slots[roff].compare_exchange_strong(
                            rexpected, slot, std::memory_order_release, std::memory_order_acquire))
                        return static_cast<std::int64_t>(rid);
                }
                continue; // cell taken by a concurrent publisher; take another
            }
        }
        const std::uint64_t ep = g_pair_claim_epoch.load(std::memory_order_acquire);
        std::uint64_t id = 0;
        if (slot != nullptr) {
            // JIT range: a separate cursor, so the evaluator's pairs_ indexes
            // and JIT ids never share an index (#5631).
            if (tls_jit_pair_ids.epoch != ep) {
                tls_jit_pair_ids.next = 0;
                tls_jit_pair_ids.end = 0;
                tls_jit_pair_ids.epoch = ep;
            }
            if (tls_jit_pair_ids.next >= tls_jit_pair_ids.end) {
                const std::uint64_t base =
                    g_pair_jit_water.fetch_add(kPairIdBatch, std::memory_order_acq_rel);
                if (base >= limit) {
                    note_pair_capacity_exhausted_5518();
                    return -1;
                }
                tls_jit_pair_ids.next = base;
                tls_jit_pair_ids.end = (base + kPairIdBatch > limit) ? limit : base + kPairIdBatch;
                tls_jit_pair_ids.epoch = ep;
            }
            id = tls_jit_pair_ids.next++;
            if (id >= limit) {
                note_pair_capacity_exhausted_5518();
                return -1;
            }
        } else {
            if (tls_pair_ids.epoch != ep) {
                tls_pair_ids.next = 0;
                tls_pair_ids.end = 0;
                tls_pair_ids.epoch = ep;
            }
            if (tls_pair_ids.next >= tls_pair_ids.end) {
                const std::uint64_t hw = high_water_.load(std::memory_order_relaxed);
                if (hw >= limit || hw >= pair_jit_base_5518()) {
                    note_pair_capacity_exhausted_5518();
                    return -1;
                }
                const std::uint64_t base =
                    high_water_.fetch_add(kPairIdBatch, std::memory_order_acq_rel);
                if (base >= limit || base >= pair_jit_base_5518()) {
                    note_pair_capacity_exhausted_5518();
                    return -1;
                }
                std::uint64_t end = base + kPairIdBatch;
                if (end > limit)
                    end = limit;
                if (end > pair_jit_base_5518())
                    end = pair_jit_base_5518();
                tls_pair_ids.next = base;
                tls_pair_ids.end = end;
                tls_pair_ids.epoch = ep;
            }
            id = tls_pair_ids.next++;
            if (id >= limit || id >= pair_jit_base_5518())
                return -1;
        }
        PairSegment* seg = pair_segment(static_cast<std::size_t>(id), /*create=*/true);
        if (!seg)
            return -1;
        const std::size_t off = static_cast<std::size_t>(id) % kPairSegSlots;
        // Interpreter MakePair calls push_back(nullptr) only to advance
        // size() past its pairs_ index. Leave the cell empty so store()
        // can install there; do not publish a null pointer.
        if (slot == nullptr)
            return static_cast<std::int64_t>(id);
        // Drop a stale stamp before the pointer is visible (#4057 reset).
        // A lost CAS (mirror installed this hole) must not be retried with
        // a plain store — that would clobber the mirror. The next id in
        // this thread's run is still ours.
        seg->tenants[off].store(0, std::memory_order_release);
        PairSlot* expected = nullptr;
        if (seg->slots[off].compare_exchange_strong(expected, slot, std::memory_order_release,
                                                    std::memory_order_acquire))
            return static_cast<std::int64_t>(id);
    }
}

bool PairSlotTable::release(std::size_t id, PairSlot* expect) {
    const std::uint64_t jw = g_pair_jit_water.load(std::memory_order_acquire);
    if (id >= pair_jit_base_5518() && id >= jw)
        return false;
    PairSegment* seg = pair_segment(id, /*create=*/false);
    if (!seg)
        return false;
    const std::size_t off = id % kPairSegSlots;
    PairSlot* expected = expect;
    const bool cleared = seg->slots[off].compare_exchange_strong(
        expected, nullptr, std::memory_order_release, std::memory_order_acquire);
    if (cleared)
        seg->tenants[off].store(0, std::memory_order_release);
    return cleared;
}

void PairSlotTable::store(std::size_t id, PairSlot* slot) {
    PairSegment* seg = pair_segment(id, /*create=*/true);
    if (!seg)
        return;
    // Install only into an empty cell. A hot-path publisher may already
    // own this index; a plain store would overwrite that pair.
    auto& cell = seg->slots[id % kPairSegSlots];
    PairSlot* expected = nullptr;
    cell.compare_exchange_strong(expected, slot, std::memory_order_release,
                                 std::memory_order_acquire);
}

void PairSlotTable::clear() {
    const std::uint64_t n = high_water_.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t jw =
        g_pair_jit_water.exchange(pair_jit_base_5518(), std::memory_order_acq_rel);
    g_pair_claim_epoch.fetch_add(1, std::memory_order_release);
    // Quiescent reset (aura_reset_runtime). Zero the cells the next claim
    // will reuse so publish can CAS from nullptr, and drop any stamp that
    // would otherwise reattach to the recycled index (#4057).
    if (n == 0 && jw <= pair_jit_base_5518())
        return;
    const auto limit = static_cast<std::uint64_t>(kPairMaxSegs) * kPairSegSlots;
    const std::uint64_t upto = n < limit ? n : limit;
    const std::uint64_t segs = (upto + kPairSegSlots - 1) / kPairSegSlots;
    std::atomic<PairSegment*>* dir = pair_seg_dir();
    for (std::uint64_t s = 0; s < segs; ++s) {
        PairSegment* seg = dir[s].load(std::memory_order_acquire);
        if (!seg)
            continue;
        const std::uint64_t begin = s * kPairSegSlots;
        const std::uint64_t end = (begin + kPairSegSlots < upto) ? begin + kPairSegSlots : upto;
        for (std::uint64_t id = begin; id < end; ++id) {
            const std::size_t off = static_cast<std::size_t>(id - begin);
            seg->slots[off].store(nullptr, std::memory_order_relaxed);
            seg->tenants[off].store(0, std::memory_order_relaxed);
        }
    }
    // Issue #5631: the JIT range reset too, so a recycled process cannot see
    // a stale published cell above the base.
    if (jw > pair_jit_base_5518()) {
        const std::uint64_t jsegs = (jw - pair_jit_base_5518() + kPairSegSlots - 1) / kPairSegSlots;
        for (std::uint64_t s = 0; s < jsegs; ++s) {
            const std::uint64_t begin = pair_jit_base_5518() + s * kPairSegSlots;
            PairSegment* seg = dir[begin / kPairSegSlots].load(std::memory_order_acquire);
            if (!seg)
                continue;
            const std::uint64_t end = std::min<std::uint64_t>(begin + kPairSegSlots, jw);
            for (std::uint64_t id = begin; id < end; ++id) {
                const std::size_t off = static_cast<std::size_t>(id % kPairSegSlots);
                seg->slots[off].store(nullptr, std::memory_order_relaxed);
                seg->tenants[off].store(0, std::memory_order_relaxed);
            }
        }
    }
}

void PairSlotTable::stamp_tenant(std::size_t id, std::uint64_t tenant) {
    PairSegment* seg = pair_segment(id, /*create=*/true);
    if (!seg)
        return;
    seg->tenants[id % kPairSegSlots].store(tenant, std::memory_order_release);
}

std::uint64_t PairSlotTable::load_tenant(std::size_t id) const {
    const std::uint64_t jw = g_pair_jit_water.load(std::memory_order_acquire);
    if (id >= pair_jit_base_5518() && id >= jw)
        return 0;
    PairSegment* seg = pair_segment(id, /*create=*/false);
    if (!seg)
        return 0;
    return seg->tenants[id % kPairSegSlots].load(std::memory_order_acquire);
}

PairTenantTable::Ref PairTenantTable::operator[](std::size_t id) {
    const std::uint64_t snap =
        (id < size_.load(std::memory_order_acquire)) ? g_pair_slots.load_tenant(id) : 0;
    return Ref(this, id, snap);
}

std::size_t PairTenantTable::size() const {
    return static_cast<std::size_t>(size_.load(std::memory_order_acquire));
}

void PairTenantTable::push_back(std::uint64_t tenant) {
    const std::size_t id = static_cast<std::size_t>(size_.fetch_add(1, std::memory_order_acq_rel));
    g_pair_slots.stamp_tenant(id, tenant);
}

void PairTenantTable::resize(std::size_t n, std::uint64_t /*fill*/) {
    // Grow-only. Cells already read as 0 until stamped. A shrink would
    // hide a stamp another cons just published.
    std::uint64_t cur = size_.load(std::memory_order_relaxed);
    while (cur < n &&
           !size_.compare_exchange_weak(cur, static_cast<std::uint64_t>(n),
                                        std::memory_order_release, std::memory_order_relaxed)) {
    }
}

void PairTenantTable::clear() {
    size_.store(0, std::memory_order_release);
}

PairTenantTable::Ref& PairTenantTable::Ref::operator=(std::uint64_t tenant) {
    g_pair_slots.stamp_tenant(id_, tenant);
    // The logical tenant table must cover this index or a later read
    // treats it as short (unstamped). Grow-only, same as resize.
    std::uint64_t cur = table_->size_.load(std::memory_order_relaxed);
    const auto need = static_cast<std::uint64_t>(id_) + 1;
    while (cur < need && !table_->size_.compare_exchange_weak(cur, need, std::memory_order_release,
                                                              std::memory_order_relaxed)) {
    }
    snap_ = tenant;
    return *this;
}

extern "C" PairSlot* aura_pair_tls_bump() {
    if (!tls_pair_bump.slab || tls_pair_bump.used >= tls_pair_bump.slab->cap) {
        auto* slab = static_cast<PairSlab*>(::aura::core::c_alloc(sizeof(PairSlab)));
        if (!slab)
            return nullptr;
        slab->cap = kPairSlabCap;
        slab->next = nullptr;
        PairSlab* head = g_pair_slabs.load(std::memory_order_relaxed);
        do {
            slab->next = head;
        } while (!g_pair_slabs.compare_exchange_weak(head, slab, std::memory_order_release,
                                                     std::memory_order_relaxed));
        g_pair_slab_publishes.fetch_add(1, std::memory_order_relaxed);
        tls_pair_bump.slab = slab;
        tls_pair_bump.used = 0;
    }
    return &tls_pair_bump.slab->slots[tls_pair_bump.used++];
}

extern "C" std::uint64_t aura_pair_slab_publish_count() {
    return g_pair_slab_publishes.load(std::memory_order_relaxed);
}

namespace {
void free_pair_slabs() {
    PairSlab* slab = g_pair_slabs.exchange(nullptr, std::memory_order_acquire);
    while (slab) {
        PairSlab* next = slab->next;
        ::aura::core::c_free(slab);
        slab = next;
    }
}
} // namespace

// Arena allocation flag (CLI --no-arena / runtime bridges).
bool g_use_arena = true;

// Flat hash table index space (Phase 4c).
std::vector<FlatHashTable*> g_hash_tables;
// Issue #4093: owner-principal stamps parallel to g_hash_tables (see
// runtime_shared.h) — stamped at every g_hash_tables.push_back alloc site
// from aura_jit_owner_capability_tenant(), the same #4057 pair-slot shape.
std::vector<std::uint64_t> g_hash_tenants;

// Issue #4110: the stamp every g_hash_tables.push_back site owes the
// parallel array above — one SSOT helper instead of a copy next to each of
// the ~100 evaluator mint sites (query results, observability, persist,
// compile hashes...). Same body as the #4093 vector-constructor stamp:
// resize-to-size first, then stamp the back slot from the owner hook.
// Callers invoke this immediately after their push_back; the back slot IS
// the pushed table (no intervening push).
void aura_hash_stamp_new_table_owner() {
    if (g_hash_tenants.size() < g_hash_tables.size())
        g_hash_tenants.resize(g_hash_tables.size(), 0);
    g_hash_tenants[g_hash_tables.size() - 1] = aura_jit_owner_capability_tenant();
}

// Issue #5190. Default 1 so a table this vector has not classified is
// treated as published (hash-set! still needs Mutate). mint / hash-copy /
// the JIT allocator clear the slot they just published.
namespace {
std::vector<std::uint8_t> g_hash_workspace_reachable;

void ensure_hash_reachable_size(std::uint64_t hidx) {
    if (g_hash_workspace_reachable.size() <= hidx)
        g_hash_workspace_reachable.resize(static_cast<std::size_t>(hidx) + 1, 1);
}
} // namespace

extern "C" void aura_hash_mark_local(std::uint64_t hidx) {
    ensure_hash_reachable_size(hidx);
    g_hash_workspace_reachable[static_cast<std::size_t>(hidx)] = 0;
}

extern "C" void aura_hash_mark_workspace_reachable(std::uint64_t hidx) {
    ensure_hash_reachable_size(hidx);
    g_hash_workspace_reachable[static_cast<std::size_t>(hidx)] = 1;
}

extern "C" int aura_hash_is_workspace_reachable(std::uint64_t hidx) {
    if (hidx >= g_hash_workspace_reachable.size())
        return 1;
    return g_hash_workspace_reachable[static_cast<std::size_t>(hidx)] != 0 ? 1 : 0;
}

namespace {
thread_local std::int64_t t_hash_mutate_stack[8];
thread_local int t_hash_mutate_depth = 0;
} // namespace

extern "C" void aura_hash_mutate_operand_push(std::int64_t hash_val) {
    if (t_hash_mutate_depth < 8)
        t_hash_mutate_stack[t_hash_mutate_depth] = hash_val;
    ++t_hash_mutate_depth;
}

extern "C" void aura_hash_mutate_operand_pop() {
    if (t_hash_mutate_depth > 0)
        --t_hash_mutate_depth;
}

extern "C" int aura_hash_mutate_operand_is_unpublished() {
    if (t_hash_mutate_depth <= 0 || t_hash_mutate_depth > 8)
        return 0;
    const auto hash_val = t_hash_mutate_stack[t_hash_mutate_depth - 1];
    const auto hidx = static_cast<std::uint64_t>(hash_val) >> 6;
    return aura_hash_is_workspace_reachable(hidx) == 0 ? 1 : 0;
}

// Issue #5195: the vector-set! / vector-fill! side of the same channel.
// The caller already classified the target against its own reachability
// table, so this stack carries the yes/no bit; there is no second table to
// keep in sync. No pushed target keeps the Mutate choke (fail closed).
namespace {
// std::array (not a built-in array) keeps the push/pop off the
// cppcoreguidelines-pro-bounds-constant-array-index baseline.
thread_local std::array<std::uint8_t, 8> t_vector_mutate_stack{};
thread_local int t_vector_mutate_depth = 0;
} // namespace

extern "C" void aura_vector_mutate_operand_push(int unpublished) {
    if (t_vector_mutate_depth < static_cast<int>(t_vector_mutate_stack.size()))
        t_vector_mutate_stack.at(static_cast<std::size_t>(t_vector_mutate_depth)) =
            unpublished != 0 ? 1 : 0;
    ++t_vector_mutate_depth;
}

extern "C" void aura_vector_mutate_operand_pop() {
    if (t_vector_mutate_depth > 0)
        --t_vector_mutate_depth;
}

extern "C" int aura_vector_mutate_operand_is_unpublished() {
    if (t_vector_mutate_depth <= 0 ||
        t_vector_mutate_depth > static_cast<int>(t_vector_mutate_stack.size()))
        return 0;
    return t_vector_mutate_stack.at(static_cast<std::size_t>(t_vector_mutate_depth - 1)) != 0 ? 1
                                                                                              : 0;
}

// ── FlatHashTable::create / destroy ──
// Strong defs used to live only in aura_jit_runtime.cpp (JIT SOs).
// evaluator_primitives_* in libaura_test_objects.so call create() for
// every query: hash; CI x86_64 mold --as-needed then strips the JIT
// lib and load fails:
//   undefined symbol: _ZN13FlatHashTable6createEm
// SSOT here (libaura_tl_arena.so) so the evaluator SO always resolves
// them. rebuild() stays in aura_jit_runtime.cpp and calls create().
static constexpr std::uint8_t kHashEmptySlot = 0xFF; // hash_meta.h kEmptySlot

FlatHashTable* FlatHashTable::create(uint64_t cap) {
    auto* ht = static_cast<FlatHashTable*>(::aura::core::c_alloc(total_bytes(cap)));
    if (!ht)
        return nullptr;
    ht->capacity = cap;
    ht->size = 0;
    auto* meta = ht->metadata();
    for (uint64_t i = 0; i < cap; ++i)
        meta[i] = kHashEmptySlot;
    auto* k = ht->keys();
    for (uint64_t i = 0; i < cap; ++i)
        k[i] = 0;
    auto* v = ht->values();
    for (uint64_t i = 0; i < cap; ++i)
        v[i] = 0;
    return ht;
}

void FlatHashTable::destroy(FlatHashTable* ht) {
    if (ht)
        ::aura::core::c_free(ht);
}

// Issue #5519: a published hash table cannot be freed at the grow point -
// a reader may still hold the old FlatHashTable* after dropping its read
// lock (OpHashRef / hash-ref drop it around aura_hash_key_eq). Retire the
// block instead and free it at the next stop-the-world safepoint (the GC
// collector's ScopedSafepoint) or at runtime reset, when no mutator runs.
namespace {
std::mutex g_retired_hash_mtx;
std::vector<FlatHashTable*> g_retired_hash_blocks;
} // namespace

extern "C" void aura_retire_hash_table(FlatHashTable* table) {
    if (table == nullptr)
        return;
    std::scoped_lock<std::mutex> lock(g_retired_hash_mtx);
    g_retired_hash_blocks.push_back(table);
}

extern "C" std::size_t aura_drain_retired_hash_tables() {
    std::vector<FlatHashTable*> retired;
    {
        std::scoped_lock<std::mutex> lock(g_retired_hash_mtx);
        retired.swap(g_retired_hash_blocks);
    }
    for (FlatHashTable* table : retired)
        FlatHashTable::destroy(table);
    return retired.size();
}

extern "C" std::size_t aura_retired_hash_table_count() {
    std::scoped_lock<std::mutex> lock(g_retired_hash_mtx);
    return g_retired_hash_blocks.size();
}

extern "C" std::uint64_t aura_pair_capacity_exhausted_total() {
    return g_pair_capacity_exhausted_5518.load(std::memory_order_relaxed);
}

// ── Live AOT env / linear / table-epoch C ABI ──
// Same load-order class as aura_set_aot_metrics. Strong defs used to
// live only in aura_jit_bridge.cpp / the light stub; libaura_test_objects.so
// then failed (CI mold --as-needed):
//   undefined symbol: aura_aot_func_table_epoch
//   undefined symbol: aura_set_aot_live_env_frame_version
//   undefined symbol: aura_get_aot_live_linear_state_fingerprint
// SSOT here so the evaluator / HotUpdateRegistry TUs always resolve
// them. Full JIT (aura_jit_bridge.cpp) fetch_adds the same
// g_aot_table_epoch object; do NOT weak-stub these in
// runtime_bridge_stub.cpp (would preempt this pointer).
std::atomic<std::uint64_t> g_aot_table_epoch{1};
// Issue #3267: one packed word is the live-bridge SSOT.
// Layout: [env_frame_version : 56][linear_fingerprint : 8].
// env_generation_ is a compact/truncate counter; 56 bits is ample.
// Existing aura_set/get_aot_live_* wrappers CAS/load this word so
// a reader of the combined getter cannot observe a torn pair.
static std::atomic<std::uint64_t> g_aot_live_bridge_state{0};
static constexpr std::uint64_t kLiveBridgeLinMask = 0xffu;
static constexpr unsigned kLiveBridgeVerShift = 8;

static std::uint64_t pack_live_bridge_state(std::uint64_t version, std::uint8_t lin) noexcept {
    return (version << kLiveBridgeVerShift) | static_cast<std::uint64_t>(lin);
}

extern "C" std::uint64_t aura_aot_func_table_epoch(void) {
    return g_aot_table_epoch.load(std::memory_order_acquire);
}

// Fallback bump when no JIT SO is loaded. Full JIT's strong
// aura_aot_bump_func_table_epoch (slot invalidate / notify) interposes
// via ELF prepend. Light tests use this fetch_add on the same atomic
// the getter reads. Weak: test_ir / asan-build compile runtime_ssot.cpp
// AND aura_jit_bridge.cpp as objects in one executable — two strong
// defs were `duplicate symbol: aura_aot_bump_func_table_epoch`.
extern "C" __attribute__((weak)) void aura_aot_bump_func_table_epoch(void) {
    g_aot_table_epoch.fetch_add(1, std::memory_order_acq_rel);
}

extern "C" void aura_set_aot_live_bridge_state(std::uint64_t version, std::uint8_t max_lin) {
    g_aot_live_bridge_state.store(pack_live_bridge_state(version, max_lin),
                                  std::memory_order_release);
}

extern "C" void aura_get_aot_live_bridge_state(std::uint64_t* out_version, std::uint8_t* out_lin) {
    const auto packed = g_aot_live_bridge_state.load(std::memory_order_acquire);
    if (out_version)
        *out_version = packed >> kLiveBridgeVerShift;
    if (out_lin)
        *out_lin = static_cast<std::uint8_t>(packed & kLiveBridgeLinMask);
}

extern "C" void aura_set_aot_live_env_frame_version(std::uint64_t v) {
    auto old = g_aot_live_bridge_state.load(std::memory_order_relaxed);
    while (!g_aot_live_bridge_state.compare_exchange_weak(
        old, pack_live_bridge_state(v, static_cast<std::uint8_t>(old & kLiveBridgeLinMask)),
        std::memory_order_release, std::memory_order_relaxed)) {
    }
}

extern "C" std::uint64_t aura_get_aot_live_env_frame_version(void) {
    return g_aot_live_bridge_state.load(std::memory_order_acquire) >> kLiveBridgeVerShift;
}

extern "C" void aura_set_aot_live_linear_state_fingerprint(std::uint8_t v) {
    auto old = g_aot_live_bridge_state.load(std::memory_order_relaxed);
    while (!g_aot_live_bridge_state.compare_exchange_weak(
        old, pack_live_bridge_state(old >> kLiveBridgeVerShift, v), std::memory_order_release,
        std::memory_order_relaxed)) {
    }
}

extern "C" std::uint8_t aura_get_aot_live_linear_state_fingerprint(void) {
    return static_cast<std::uint8_t>(g_aot_live_bridge_state.load(std::memory_order_acquire) &
                                     kLiveBridgeLinMask);
}

// ── Current-fiber-id hook (Issue #195) ──
// SSOT lives here (libaura_tl_arena.so) so libaura_test_objects.so
// (which NEEDED-links libaura_tl_arena.so) resolves
// aura_set_current_fiber_id_fn at load time without depending on the
// JIT SOs. Same class of bug as g_pair_slots: when the definition
// lives only in aura_jit_runtime.cpp (JIT SOs), aura_test_objects has
// an undefined symbol that x86_64 glibc refuses to resolve from a
// later-loaded DSO at load time:
//   symbol lookup error: libaura_test_objects.so: undefined symbol: aura_set_current_fiber_id_fn
// (mold --as-needed strips the JIT lib from test_issue_* binaries
// that don't directly reference it).
static aura_fiber_id_fn_t g_current_fiber_id_fn = nullptr;

extern "C" void aura_set_current_fiber_id_fn(aura_fiber_id_fn_t fn) {
    g_current_fiber_id_fn = fn;
}
extern "C" aura_fiber_id_fn_t aura_get_current_fiber_id_fn() {
    return g_current_fiber_id_fn;
}

// ── AOT metrics pointer (Issue #1368) ──
// Same load-order class as aura_set_current_fiber_id_fn. Strong
// definition used to live only in aura_jit_bridge.cpp / the light
// stub; libaura_test_objects.so then failed:
//   undefined symbol: aura_set_aot_metrics
static aura::compiler::CompilerMetrics* g_aot_metrics = nullptr;
static std::atomic<std::uint64_t> g_aot_metrics_lazy_init_total{0};
static std::atomic<std::uint64_t> g_aot_metrics_explicit_sets{0};

extern "C" void aura_set_aot_metrics(aura::compiler::CompilerMetrics* m) {
    g_aot_metrics = m;
    if (m)
        g_aot_metrics_explicit_sets.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aura_ensure_aot_metrics(void* metrics) {
    if (!metrics)
        return;
    if (g_aot_metrics == nullptr) {
        g_aot_metrics = static_cast<aura::compiler::CompilerMetrics*>(metrics);
        g_aot_metrics_lazy_init_total.fetch_add(1, std::memory_order_relaxed);
    }
}

extern "C" void* aura_get_aot_metrics(void) {
    return g_aot_metrics;
}

// Issue #3177 follow-up (ASAN stack-use-after-scope): clear the
// process-wide g_aot_metrics pointer iff it currently points to
// `metrics`. Called from Evaluator's destructor before
// aura_cleanup_aot_state — CompilerService declares `evaluator_`
// before `metrics_` (service.ixx ~11503 / ~13793), so C++ destroys
// `metrics_` FIRST (reverse declaration order). By the time
// ~Evaluator() runs, the metrics object is already dead.
// aura_cleanup_aot_state → aot_invalidate_all_stale_slots_for_eval_impl
// calls aot_metrics() (reads g_aot_metrics) and bumps
// aot_reload_fall_back_slot_invalidate_total via fetch_add on dead
// stack memory — ASan catches this as stack-use-after-scope at
// aura_jit_bridge.cpp:2124. Clearing g_aot_metrics before
// aura_cleanup_aot_state makes the metric bumps short-circuit on
// nullptr. The pointer comparison avoids clobbering a still-live
// pointer (e.g. when nested CompilerService shares the global).
extern "C" void aura_clear_aot_metrics_for_eval(void* metrics) {
    if (!metrics)
        return;
    if (g_aot_metrics == static_cast<aura::compiler::CompilerMetrics*>(metrics))
        g_aot_metrics = nullptr;
}

extern "C" std::uint64_t aura_aot_metrics_lazy_init_total(void) {
    return g_aot_metrics_lazy_init_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_aot_metrics_explicit_sets_total(void) {
    return g_aot_metrics_explicit_sets.load(std::memory_order_relaxed);
}

// ── StormLevel C ABI (Issue #2094) ──
// Fallback bitmask when no JIT registry has registered yet (Shape=bit0).
static std::uint8_t (*g_storm_get)(void) = nullptr;
static void (*g_storm_set_shape)(int) = nullptr;
static std::atomic<std::uint8_t> g_storm_fallback{0};

extern "C" void aura_register_storm_c_abi(std::uint8_t (*get)(void), void (*set_shape)(int)) {
    g_storm_get = get;
    g_storm_set_shape = set_shape;
}

extern "C" std::uint8_t aura_hot_update_current_storm_level(void) {
    if (g_storm_get)
        return g_storm_get();
    return g_storm_fallback.load(std::memory_order_relaxed);
}

extern "C" void aura_hot_update_set_shape_storm_active(int active) {
    if (g_storm_set_shape) {
        g_storm_set_shape(active);
        return;
    }
    if (active)
        g_storm_fallback.fetch_or(1, std::memory_order_relaxed);
    else
        g_storm_fallback.fetch_and(static_cast<std::uint8_t>(~1u), std::memory_order_relaxed);
}

// ── AOT slot / per-eval map C ABI ──
// Full JIT registers the real slot table. Light stub leaves these
// null so count/inject stay no-op unless the full bridge is loaded.
static std::size_t (*g_aot_count_behind)(void) = nullptr;
static void (*g_aot_inject)(std::int64_t) = nullptr;
static void (*g_aot_clear_slot)(std::int64_t) = nullptr;
static std::size_t (*g_aot_invalidate)(void*) = nullptr;
static std::uint64_t (*g_aot_map_size)(void) = nullptr;

extern "C" void aura_register_aot_slot_c_abi(std::size_t (*count_behind)(void),
                                             void (*inject)(std::int64_t),
                                             void (*clear_slot)(std::int64_t),
                                             std::size_t (*invalidate)(void*),
                                             std::uint64_t (*map_size)(void)) {
    g_aot_count_behind = count_behind;
    g_aot_inject = inject;
    g_aot_clear_slot = clear_slot;
    g_aot_invalidate = invalidate;
    g_aot_map_size = map_size;
}

extern "C" std::size_t aura_aot_count_live_generation_behind_slots(void) {
    return g_aot_count_behind ? g_aot_count_behind() : 0;
}

extern "C" void aura_aot_inject_live_stale_slot_for_test(std::int64_t func_id) {
    if (g_aot_inject)
        g_aot_inject(func_id);
}

extern "C" void aura_aot_clear_slot_for_test(std::int64_t func_id) {
    if (g_aot_clear_slot)
        g_aot_clear_slot(func_id);
}

extern "C" std::size_t aura_aot_invalidate_all_stale_slots_for_eval(void* eval_ptr) {
    return g_aot_invalidate ? g_aot_invalidate(eval_ptr) : 0;
}

extern "C" std::uint64_t aura_aot_state_map_size(void) {
    return g_aot_map_size ? g_aot_map_size() : 0;
}

// ── Epoch-invariant mode (Issue #2304 / #2501) ──
// Weak stubs in libaura_test_objects.so used to no-op set/get, so
// test_epoch_invariant_* never left mode=0. SSOT here so the setter
// is always live; the full JIT walk reads this same value.
static std::atomic<int> g_epoch_invariant_mode{0};

extern "C" void aura_set_epoch_invariant_mode(int mode) {
    if (mode < 0)
        mode = 0;
    if (mode > 2)
        mode = 2;
    g_epoch_invariant_mode.store(mode, std::memory_order_relaxed);
}

extern "C" int aura_epoch_invariant_mode(void) {
    return g_epoch_invariant_mode.load(std::memory_order_relaxed);
}

static std::atomic<std::uint64_t> g_epoch_invariant_violation_total{0};
static std::atomic<std::uint64_t> g_epoch_invariant_walks_total{0};
static std::atomic<std::uint64_t> g_epoch_invariant_slot_stale_total{0};
static std::atomic<std::uint64_t> g_epoch_invariant_closure_must_deopt_total{0};
// Issue #3540: sid-stale marks from the existing epoch-invariant walk
// (append END per #2906). Distinct from closure-must-deopt (epoch side).
inline constexpr int kEpochInvariantSidStaleIssue = 3540;
static std::atomic<std::uint64_t> g_epoch_invariant_sid_stale_total{0};

extern "C" void aura_epoch_invariant_note_walk(std::uint64_t violations) noexcept {
    g_epoch_invariant_walks_total.fetch_add(1, std::memory_order_relaxed);
    if (violations > 0)
        g_epoch_invariant_violation_total.fetch_add(violations, std::memory_order_relaxed);
}

extern "C" void aura_epoch_invariant_note_slot_stale(std::uint64_t n) noexcept {
    if (n > 0)
        g_epoch_invariant_slot_stale_total.fetch_add(n, std::memory_order_relaxed);
}

extern "C" void aura_epoch_invariant_note_closure_must_deopt(std::uint64_t n) noexcept {
    if (n > 0)
        g_epoch_invariant_closure_must_deopt_total.fetch_add(n, std::memory_order_relaxed);
}

extern "C" void aura_epoch_invariant_note_sid_stale(std::uint64_t n) noexcept {
    if (n > 0)
        g_epoch_invariant_sid_stale_total.fetch_add(n, std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_epoch_invariant_violation_total_v_read(void) {
    return g_epoch_invariant_violation_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_epoch_invariant_walks_total_v_read(void) {
    return g_epoch_invariant_walks_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_epoch_invariant_slot_stale_total_v_read(void) {
    return g_epoch_invariant_slot_stale_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_epoch_invariant_closure_must_deopt_total_v_read(void) {
    return g_epoch_invariant_closure_must_deopt_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_epoch_invariant_sid_stale_total_v_read(void) {
    return g_epoch_invariant_sid_stale_total.load(std::memory_order_relaxed);
}

extern "C" int aura_epoch_invariant_sid_stale_issue(void) {
    return kEpochInvariantSidStaleIssue;
}

static std::size_t (*g_must_deopt_stale)(void) = nullptr;

extern "C" void aura_register_epoch_must_deopt_fn(std::size_t (*fn)(void)) {
    g_must_deopt_stale = fn;
}

extern "C" std::size_t aura_epoch_invariant_must_deopt_stale_live_closures(void) {
    return g_must_deopt_stale ? g_must_deopt_stale() : 0;
}

// Issue #1998 / B-024: C accessor so tests need not name std::vector.
// Issue #4755: reads the vector under the same process-global mutex as the
// push path (a concurrent realloc would otherwise be read mid-move).
extern "C" size_t aura_g_owned_pair_slots_size() {
    std::scoped_lock<std::mutex> owned(g_owned_pair_slots_mtx);
    return g_owned_pair_slots_.size();
}

namespace {
struct PairSlotCleanup : ::aura::core::NoCopy {
    ~PairSlotCleanup() {
        // Issue #4755: walk and clear under the registry mutex so the
        // iteration cannot observe a concurrent push_back's realloc.
        std::scoped_lock<std::mutex> owned(g_owned_pair_slots_mtx);
        for (auto* p : g_owned_pair_slots_) {
            ::aura::core::c_free(p);
        }
        g_owned_pair_slots_.clear();
        // Issue #898: JIT cons slabs are not on g_owned_pair_slots_.
        free_pair_slabs();
        (void)aura_drain_retired_hash_tables(); // Issue #5519
    }
};
[[maybe_unused]] PairSlotCleanup g_pair_slot_cleanup;
} // namespace
