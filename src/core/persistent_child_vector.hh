// ──────────────────────────────────────────────────────────────
//  persistent_child_vector.hh — immutable (copy-on-write) vector
//  for per-node AST children.
//
//  Issue #221 (Issue #179 Cycle 3) + Issue #2036 (migration complete):
//  the "final form" of FlatAST's children storage. Replaces the
//  mutable std::pmr::vector<NodeId> (added in #220) with a persistent
//  (immutable) version that supports:
//    1. COW semantics: a "mutation" (with_push_back / with_insert /
//       with_erase / with_set) does NOT modify the receiver; it
//       returns a NEW PersistentChildVector with the change
//       applied. The old vector is unchanged, so any caller
//       holding a reference (or a shared_ptr to the old data)
//       continues to see the old contents.
//    2. Back-references: a closure that captured the children
//       list pre-mutation can still read the pre-mutation data
//       after the mutation, because the underlying storage is
//       reference-counted via std::shared_ptr. The old storage
//       is freed when the last shared_ptr referencing it goes
//       out of scope.
//    3. Composability with #177's MutationCheckpoint: a
//       mutation captures a snapshot (shared_ptr) of the
//       pre-mutation vector. On rollback, the snapshot is
//       reinstalled in FlatAST::children_, and the checkpoint
//       keeps the old shared_ptr alive until the rollback
//       boundary is exited.
//
//  Issue #2036 migration end-state (complete):
//    - FlatAST::children_ is std::vector<PersistentChildVector<NodeId>>
//      (no remaining pmr::vector path for children lists).
//    - Default agent-facing children APIs pin storage via SafePCVSpan
//      (children_safe / children_default / children_stable path).
//    - MutationCheckpoint snapshot_children / restore_children share
//      or abandon storage correctly (abandon_storage on teardown).
//    - Raw children() spans remain for single-statement hot paths only;
//      prefer children_safe / children_columnar for multi-round AI loops.
//
//  Header-only, no std::pmr dependency. Uses std::shared_ptr
//  (atomic refcount — safe to share across fibers / threads).
//
//  Issue #2058 / #2140: unique-ownership hot path. When the PCV is the
//  sole holder of storage (use_count()==1) — the common FlatAST pattern
//  of "mutate then drop old view" after a move out of children_[id] —
//  cow_* / with_set exclusive + ensure_unique + in-place writes avoid a
//  new allocation and the second atomic refcount trip. Snapshot /
//  SafePCVSpan holders keep use_count()>1 so with_* still allocate
//  (COW correctness). Issue #2140 extends exclusive in-place to const
//  with_set (AI multi-round local replace-one-child).
//
// Test plan (test_issue_221.cpp):
//   1. Basic: construct, size, operator[], iterators
//   2. COW semantics: with_push_back / with_insert / with_erase
//      / with_set leave the receiver unchanged
//   3. Back-references: old shared_ptr stays valid after a
//      with_push_back on a copy
//   4. Multiple branches: tree of with_* operations, all
//      backed by the same original
//   5. Rollback correctness: capture pre-mutation state, mutate
//      forward, "rollback" by reinstalling the capture, verify
//      the result == pre-mutation
//   6. Empty vector: construct, with_push_back, with_erase (on
//      empty is no-op), with_set (out of range is no-op)
//   7. Comparison: ==, !=
//   8. 5000-element stress: 100 mutations each, < 2µs/op (the
//      cost is allocation, not shift)
//   9. Wire format v3 (per-node list) roundtrip: serialize the
//      persistent vector as the existing #220 wire format
// ──────────────────────────────────────────────────────────────

#ifndef AURA_CORE_PERSISTENT_CHILD_VECTOR_HH
#define AURA_CORE_PERSISTENT_CHILD_VECTOR_HH

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aura::ast {

// Issue #2058: process-wide PCV hot-path metrics (unique in-place vs COW alloc).
// Issue #2406: optional TLS freelist hits for exclusive short-lived allocs.
struct PcvHotpathMetrics {
    std::atomic<std::uint64_t> unique_inplace_total{0};
    std::atomic<std::uint64_t> cow_alloc_total{0};
    std::atomic<std::uint64_t> ensure_unique_clone_total{0};
    std::atomic<std::uint64_t> cow_set_total{0};
    std::atomic<std::uint64_t> cow_insert_total{0};
    std::atomic<std::uint64_t> cow_erase_total{0};
    std::atomic<std::uint64_t> cow_push_total{0};
    // Issue #2140: with_set exclusive vs shared paths.
    std::atomic<std::uint64_t> with_set_exclusive_total{0};
    std::atomic<std::uint64_t> with_set_cow_total{0};
    // Issue #2406 / #2521: TLS scratch freelist (production default ON;
    // AURA_PCV_TLS=0 forces off).
    std::atomic<std::uint64_t> tls_scratch_hit_total{0};
    std::atomic<std::uint64_t> tls_scratch_miss_total{0};
    std::atomic<std::uint64_t> tls_scratch_recycle_total{0};
};
inline PcvHotpathMetrics& g_pcv_hotpath_metrics() noexcept {
    static PcvHotpathMetrics m;
    return m;
}
inline void reset_pcv_hotpath_metrics_for_test() noexcept {
    auto& m = g_pcv_hotpath_metrics();
    m.unique_inplace_total.store(0, std::memory_order_relaxed);
    m.cow_alloc_total.store(0, std::memory_order_relaxed);
    m.ensure_unique_clone_total.store(0, std::memory_order_relaxed);
    m.cow_set_total.store(0, std::memory_order_relaxed);
    m.cow_insert_total.store(0, std::memory_order_relaxed);
    m.cow_erase_total.store(0, std::memory_order_relaxed);
    m.cow_push_total.store(0, std::memory_order_relaxed);
    m.with_set_exclusive_total.store(0, std::memory_order_relaxed);
    m.with_set_cow_total.store(0, std::memory_order_relaxed);
    m.tls_scratch_hit_total.store(0, std::memory_order_relaxed);
    m.tls_scratch_miss_total.store(0, std::memory_order_relaxed);
    m.tls_scratch_recycle_total.store(0, std::memory_order_relaxed);
}

inline constexpr int kPcvHotpathIssue = 2058;
// Issue #2140: exclusive with_set (refcount==1) in-place, no alloc.
inline constexpr int kPcvExclusiveSetIssue = 2140;
// Issue #2406: TLS scratch freelist foundation for exclusive PCV allocs.
// Issue #2521: production default ON (mirror Moving compact / HighMutation).
inline constexpr int kPcvTlsScratchIssue = 2406;
inline constexpr int kPcvTlsDefaultOnIssue = 2521;

// Issue #2406 / #2521: TLS freelist for exclusive short-lived PCV allocs.
// Production default ON (AC1). Override:
//   AURA_PCV_TLS=0|f|F|n|N  → force off (deterministic alloc accounting)
//   AURA_PCV_TLS=1|t|T|y|Y  → force on
// Cross-fiber steal: non-owner thread deletes, does not recycle (AC3).
[[nodiscard]] inline bool pcv_tls_scratch_enabled() noexcept {
    static const bool on = [] {
        if (const char* e = std::getenv("AURA_PCV_TLS")) {
            if (e[0] == '\0')
                return true; // empty → production default
            if (e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' || e[0] == 'N')
                return false;
            if (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y')
                return true;
        }
        return true; // Issue #2521: production default ON
    }();
    return on;
}

// Test override (process-local). nullopt = use env; true/false force.
inline std::atomic<int>& pcv_tls_scratch_test_override() noexcept {
    static std::atomic<int> v{-1}; // -1 env, 0 off, 1 on
    return v;
}
[[nodiscard]] inline bool pcv_tls_scratch_active() noexcept {
    const int o = pcv_tls_scratch_test_override().load(std::memory_order_relaxed);
    if (o == 0)
        return false;
    if (o == 1)
        return true;
    return pcv_tls_scratch_enabled();
}
inline void set_pcv_tls_scratch_for_test(bool on) noexcept {
    pcv_tls_scratch_test_override().store(on ? 1 : 0, std::memory_order_relaxed);
}
inline void clear_pcv_tls_scratch_for_test() noexcept {
    pcv_tls_scratch_test_override().store(-1, std::memory_order_relaxed);
}

template <typename T> class PersistentChildVector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    // All accessors return const references / pointers — the
    // vector is immutable. Mutations go through the with_*
    // methods, which return a new vector.
    using reference = const T&;
    using const_reference = const T&;
    using pointer = const T*;
    using const_pointer = const T*;
    using iterator = const T*;
    using const_iterator = const T*;

    constexpr PersistentChildVector() noexcept = default;

    PersistentChildVector(std::initializer_list<T> init) {
        if (init.size() == 0)
            return;
        size_ = init.size();
        data_ = make_storage(init.size());
        std::copy(init.begin(), init.end(), data_->data.get());
    }

    explicit PersistentChildVector(size_type n) {
        if (n == 0)
            return;
        size_ = n;
        data_ = make_storage(n);
        // value-initialized
        for (std::size_t i = 0; i < n; ++i)
            data_->data[i] = T{};
    }

    PersistentChildVector(size_type n, const T& v) {
        if (n == 0)
            return;
        size_ = n;
        data_ = make_storage(n);
        std::fill_n(data_->data.get(), n, v);
    }

    // Range constructor (used by FlatAST's add_X methods that
    // build a per-node list from a std::span<NodeId> or a
    // temporary std::vector). Constructs in O(n) — no per-
    // element COW copies.
    template <typename It> PersistentChildVector(It first, It last) {
        auto n = static_cast<size_type>(std::distance(first, last));
        if (n == 0)
            return;
        size_ = n;
        data_ = make_storage(n);
        std::copy(first, last, data_->data.get());
    }

    // Fill-constructor: pre-allocates the buffer (one allocation)
    // and calls fill(i) for each element. Saves one allocation vs
    // the range-constructor pattern (which needs a temp std::vector
    // + a PCV copy). Used by FlatAST's add_X methods when the
    // element count is known up front.
    //
    // Example:
    //   children_[id] = PersistentChildVector<NodeId>(
    //       3, [](size_t i) -> NodeId {
    //           return i == 0 ? cond : (i == 1 ? then_b : else_b);
    //       });
    template <typename FillFn> PersistentChildVector(size_type n, FillFn fill) {
        if (n == 0)
            return;
        size_ = n;
        data_ = make_storage(n);
        for (size_type i = 0; i < n; ++i) {
            data_->data[i] = fill(i);
        }
    }

    // Copy / move: O(1) (shared_ptr copy).
    PersistentChildVector(const PersistentChildVector&) = default;
    PersistentChildVector& operator=(const PersistentChildVector&) = default;
    // Custom move: the default move would leave size_ at its old
    // value (size_t is trivially copyable, not moved). The shared_ptr
    // gets reset to null by its own move. The result would be a
    // "moved-from" vector with size > 0 but data() == nullptr — a
    // bug waiting to crash. We explicitly reset size_ on move.
    PersistentChildVector(PersistentChildVector&& other) noexcept
        : data_(std::move(other.data_))
        , size_(other.size_) {
        other.size_ = 0;
    }
    PersistentChildVector& operator=(PersistentChildVector&& other) noexcept {
        if (this != &other) {
            data_ = std::move(other.data_);
            size_ = other.size_;
            other.size_ = 0;
        }
        return *this;
    }

    // ── Capacity (const) ──────────────────────────────────────
    constexpr size_type size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr const_pointer data() const noexcept { return data_ ? data_->data.get() : nullptr; }
    // Issue #1624: SoAColumnarFull — contiguous column view + shape stamp
    // (arity = children count for PCV cache locality / shape matching).
    [[nodiscard]] std::span<const T> columnar_accessor() const noexcept {
        return std::span<const T>(data(), size_);
    }
    [[nodiscard]] constexpr size_type stable_shape_id() const noexcept { return size_; }
    // ref-count of the underlying storage. Useful for tests
    // verifying COW semantics (a mutation should leave the
    // old storage with refcount > 1).
    long use_count() const noexcept { return data_.use_count(); }

    // Issue #2058: sole owner of storage (safe for in-place mutate).
    // Empty / null storage is treated as unique.
    [[nodiscard]] bool is_unique() const noexcept { return !data_ || data_.use_count() == 1; }

    // Issue #300 follow-up #1: identity of the shared storage
    // block (for teardown dedup when aliased PCVs exist).
    const void* storage_identity() const noexcept { return data_.get(); }
    // Drop the shared_ptr without running its destructor
    // (used when another PCV slot still owns the refcount).
    void abandon_storage() noexcept {
        data_ = nullptr;
        size_ = 0;
    }

    // Issue #2058: if shared, clone into private storage (refcount 1).
    // No-op when already unique or empty. Returns true if a clone ran.
    bool ensure_unique() {
        if (!data_ || data_.use_count() == 1)
            return false;
        auto out = make_storage_owned(size_);
        const T* src = src_data();
        if (src && size_ > 0)
            std::copy(src, src + size_, out->data.get());
        data_ = std::move(out);
        g_pcv_hotpath_metrics().ensure_unique_clone_total.fetch_add(1, std::memory_order_relaxed);
        note_pcv_alloc();
        return true;
    }

    // ── Element access (const) ───────────────────────────────
    const_reference operator[](size_type i) const noexcept pre(i < size_) {
        contract_assert(data_ != nullptr);
        return data_->data[i];
    }
    const_reference at(size_type i) const {
        if (i >= size_)
            throw std::out_of_range("PersistentChildVector::at");
        return data_->data[i];
    }
    const_reference front() const { return data_->data[0]; }
    const_reference back() const { return data_->data[size_ - 1]; }

    // ── Iterators (const) ──────────────────────────────────────
    // Safe for empty / default-constructed vectors: when data_
    // is null (no underlying Storage), begin() == end() ==
    // nullptr. The for-range loop on an empty PCV is a no-op.
    const_iterator begin() const noexcept { return data_ ? data_->data.get() : nullptr; }
    const_iterator end() const noexcept { return data_ ? data_->data.get() + size_ : nullptr; }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    // ── COW mutations (return a new vector) ──────────────────
    //
    // Each with_* method:
    //  - Allocates a new buffer (size +/- 1, or size with one
    //    element replaced).
    //  - Copies the old elements (with the change applied).
    //  - Returns the new vector. The receiver is unchanged.

    // Helper: get a pointer to the elements, or nullptr if the
    // storage is empty (no underlying buffer). The with_* methods
    // use this to skip the std::copy when the source is empty
    // (calling std::copy with a null source is UB).
    const T* src_data() const noexcept { return data_ ? data_->data.get() : nullptr; }

    PersistentChildVector with_push_back(const T& v) const {
        auto out = make_storage_owned(size_ + 1);
        const T* src = src_data();
        if (src)
            std::copy(src, src + size_, out->data.get());
        out->data[size_] = v;
        auto result = from_storage(out, size_ + 1);
        contract_assert(result.size() == size_ + 1);
        note_pcv_alloc(); // Issue #2406: TLS hit skips cow_alloc
        return result;
    }

    PersistentChildVector with_push_back(T&& v) const {
        auto out = make_storage_owned(size_ + 1);
        const T* src = src_data();
        if (src)
            std::move(src, src + size_, out->data.get());
        out->data[size_] = std::move(v);
        auto result = from_storage(out, size_ + 1);
        contract_assert(result.size() == size_ + 1);
        note_pcv_alloc();
        return result;
    }

    PersistentChildVector with_insert(size_type pos, const T& v) const {
        if (pos > size_)
            pos = size_;
        auto out = make_storage_owned(size_ + 1);
        const T* src = src_data();
        if (src) {
            std::copy(src, src + pos, out->data.get());
            std::copy(src + pos, src + size_, out->data.get() + pos + 1);
        }
        out->data[pos] = v;
        auto result = from_storage(out, size_ + 1);
        contract_assert(result.size() == size_ + 1);
        note_pcv_alloc();
        return result;
    }

    PersistentChildVector with_erase(size_type pos) const {
        if (pos >= size_)
            return *this; // no-op
        auto out = make_storage_owned(size_ - 1);
        const T* src = src_data();
        if (src) {
            std::copy(src, src + pos, out->data.get());
            std::copy(src + pos + 1, src + size_, out->data.get() + pos);
        }
        auto result = from_storage(out, size_ - 1);
        contract_assert(result.size() == size_ - 1);
        note_pcv_alloc();
        return result;
    }

    // Issue #2140: when this handle is the sole storage owner
    // (use_count()==1), write in place and return *this without allocating.
    // Shared storage (snapshot / SafePCVSpan / multi-holder) always COWs so
    // observers keep pre-mutation data. Empty / OOB is a no-op.
    PersistentChildVector with_set(size_type i, const T& v) const {
        if (i >= size_)
            return *this; // no-op
        // Exclusive path: sole owner may mutate the element buffer in place.
        // unique_ptr<T[]>::get() yields T* even through const Storage — same
        // discipline as cow_set (#2058). Never write when use_count()>1.
        if (data_ && data_.use_count() == 1) {
            data_->data.get()[i] = v;
            g_pcv_hotpath_metrics().unique_inplace_total.fetch_add(1, std::memory_order_relaxed);
            g_pcv_hotpath_metrics().with_set_exclusive_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            return *this; // same storage identity, updated element
        }
        // Shared / null: classic COW allocate + copy.
        auto out = make_storage_owned(size_);
        const T* src = src_data();
        if (src)
            std::copy(src, src + size_, out->data.get());
        out->data[i] = v;
        auto result = from_storage(out, size_);
        contract_assert(result.size() == size_);
        note_pcv_alloc();
        g_pcv_hotpath_metrics().with_set_cow_total.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    // ── Issue #2058: unique-path / COW hybrid mutators ────────
    // Non-const: when this is the sole storage owner, write in place
    // (zero alloc, no extra atomic). When shared (snapshot / pin /
    // SafePCVSpan), allocate like with_* and replace self.
    // FlatAST locked paths should move children_[id] out, call cow_*,
    // then move back — so the common case is unique.

    void cow_set(size_type i, const T& v) {
        g_pcv_hotpath_metrics().cow_set_total.fetch_add(1, std::memory_order_relaxed);
        if (i >= size_)
            return;
        if (is_unique() && data_) {
            // Storage is shared_ptr<const Storage> but unique_ptr<T[]>::get()
            // on const unique_ptr still yields T* — sole owner may write.
            data_->data.get()[i] = v;
            g_pcv_hotpath_metrics().unique_inplace_total.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        *this = with_set(i, v);
    }

    void cow_push_back(const T& v) {
        g_pcv_hotpath_metrics().cow_push_total.fetch_add(1, std::memory_order_relaxed);
        // Exact-size storage: push always allocates a larger buffer.
        // Moving the PCV first (FlatAST pattern) still avoids an extra
        // atomic refcount bump on the assignment path.
        *this = with_push_back(v);
    }

    void cow_insert(size_type pos, const T& v) {
        g_pcv_hotpath_metrics().cow_insert_total.fetch_add(1, std::memory_order_relaxed);
        // Insert always changes size → always need new buffer (no spare capacity).
        *this = with_insert(pos, v);
    }

    void cow_erase(size_type pos) {
        g_pcv_hotpath_metrics().cow_erase_total.fetch_add(1, std::memory_order_relaxed);
        if (pos >= size_)
            return;
        // Erase always changes size → new buffer. Unique still allocates but
        // avoids an extra shared_ptr copy when the caller moved the PCV first.
        *this = with_erase(pos);
    }

    // ── Comparison ───────────────────────────────────────────
    bool operator==(const PersistentChildVector& other) const {
        if (size_ != other.size_)
            return false;
        if (size_ == 0)
            return true; // both empty
        return std::equal(data_->data.get(), data_->data.get() + size_, other.data_->data.get());
    }
    bool operator!=(const PersistentChildVector& other) const { return !(*this == other); }

    // Issue #370: expose the SafePCVSpan<T> as a friend so it
    // can access the private Storage type + data_ member
    // needed to construct a lifetime-pinned view. (Without
    // this friend, SafePCVSpan would need to take its own
    // copy of Storage — doubling the alloc cost.)
    template <typename U> friend class SafePCVSpan;

    // Issue #370: free function form of share_storage. Friend
    // access grants read of the private data_; SafePCVSpan
    // uses this to grab the shared_ptr without needing a
    // public member.
    template <typename U>
    friend typename PersistentChildVector<U>::StoragePtr
    share_storage(const PersistentChildVector<U>& v) noexcept {
        return v.data_;
    }

private:
    // Internal storage. Holds a unique_ptr<T[]> (mutable,
    // since we write into it during construction) and a size.
    // The PersistentChildVector stores a shared_ptr to const
    // Storage, which gives us:
    //  - O(1) copy (shared_ptr refcount++)
    //  - Automatic memory management
    //  - Const-correctness on the outside (the vector's
    //    accessors all take const; the elements are reached
    //    through storage->data, which is mutable in the const
    //    storage because we want the elements to be readable
    //    through const this). Wait — that's not const-correct.
    //    The storage holds a const T[] view via const_cast at
    //    boundary. Below we use a different approach.
    //
    // Actually the cleanest design: storage is const,
    // elements are const, but the storage is initialized with
    // a mutable buffer that's never exposed. The const on
    // Storage makes the elements const, but to write into
    // them during construction we use placement new on a
    // mutable buffer (separate from the shared_ptr). Then the
    // shared_ptr takes ownership of the (now const) buffer.
    //
    // For simplicity, we use a mutable unique_ptr<T[]> inside
    // a const wrapper. The wrapper's const-ness enforces the
    // vector's immutability: once a vector is constructed, you
    // can only get a shared_ptr to its storage; you can't
    // modify the storage without going through a with_*
    // method (which produces a new storage).
    struct Storage {
        std::unique_ptr<T[]> data;
        size_type size;

        Storage() noexcept = default;
        Storage(size_type n)
            : data(std::make_unique<T[]>(n))
            , size(n) {}
    };

    using StoragePtr = std::shared_ptr<const Storage>;

    StoragePtr data_;
    size_type size_ = 0; // mirrored from data_->size for O(1) access

    // Issue #2406: TLS freelist for exclusive short-lived Storage.
    // Pool holds Storage of capacity kTlsMaxElems (logical size still n).
    // Deleter returns to freelist only on the allocating thread; cross-
    // fiber steal falls back to delete (safe). Default path: make_shared.
    static constexpr size_type kTlsMaxElems = 64;
    static constexpr std::size_t kTlsSlots = 4;

    struct TlsFreelist {
        std::unique_ptr<Storage> slots[kTlsSlots]{};
        // owner thread id for cross-fiber safety (0 = unset)
        std::uintptr_t owner_tid = 0;
    };

    static TlsFreelist& tls_freelist() noexcept {
        thread_local TlsFreelist fl;
        return fl;
    }

    static std::uintptr_t current_tid() noexcept {
        // Opaque stable id for this thread; not a system tid.
        thread_local char tls_token{};
        return reinterpret_cast<std::uintptr_t>(&tls_token);
    }

    static void tls_recycle(Storage* raw) noexcept {
        if (!raw) {
            return;
        }
        if (!pcv_tls_scratch_active()) {
            delete raw;
            return;
        }
        auto& fl = tls_freelist();
        const auto tid = current_tid();
        if (fl.owner_tid != 0 && fl.owner_tid != tid) {
            // Allocated on another thread (fiber steal) — free, do not recycle.
            delete raw;
            return;
        }
        fl.owner_tid = tid;
        for (std::size_t i = 0; i < kTlsSlots; ++i) {
            if (!fl.slots[i]) {
                // Reset logical fill for reuse (capacity remains kTlsMaxElems).
                fl.slots[i].reset(raw);
                g_pcv_hotpath_metrics().tls_scratch_recycle_total.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
        }
        delete raw; // freelist full
    }

    static StoragePtr make_from_tls_or_new(size_type n) {
        // Always allocate/reuse capacity kTlsMaxElems when n <= kTlsMaxElems.
        (void)n;
        auto& fl = tls_freelist();
        const auto tid = current_tid();
        if (fl.owner_tid == 0)
            fl.owner_tid = tid;
        auto recycle_del = [](const Storage* p) { tls_recycle(const_cast<Storage*>(p)); };
        for (std::size_t i = 0; i < kTlsSlots; ++i) {
            if (fl.slots[i]) {
                Storage* raw = fl.slots[i].release();
                g_pcv_hotpath_metrics().tls_scratch_hit_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                tls_last_alloc_was_hit() = true;
                return StoragePtr(raw, recycle_del);
            }
        }
        // Miss: fresh alloc into pool capacity (still recyclable).
        g_pcv_hotpath_metrics().tls_scratch_miss_total.fetch_add(1, std::memory_order_relaxed);
        tls_last_alloc_was_hit() = false;
        Storage* raw = new Storage(kTlsMaxElems);
        return StoragePtr(raw, recycle_del);
    }

    // Allocate a fresh shared storage with the given capacity.
    // The elements are uninitialized (the caller fills them).
    static StoragePtr make_storage(size_type n) {
        if (n == 0) {
            return std::make_shared<Storage>();
        }
        // Issue #2406 / #2521: constructors share TLS path when active + small.
        if (pcv_tls_scratch_active() && n <= kTlsMaxElems)
            return make_from_tls_or_new(n);
        return std::make_shared<Storage>(n);
    }

    // Same as make_storage but the elements are filled (the
    // caller writes into the unique_ptr directly). The
    // returned StoragePtr owns the storage; the Storage is
    // mutable inside (the unique_ptr<T[]>), so the caller
    // can write to storage->data[i] before the storage is
    // shared out.
    //
    // Issue #2406 / #2521: when TLS is active (production default)
    // and n is small, prefer freelist (hit avoids malloc). Callers
    // still may bump cow_alloc_total; with_* use note_pcv_alloc() so
    // TLS hits do not inflate cow_alloc (exclusive stress AC2).
    static StoragePtr make_storage_owned(size_type n) {
        if (n == 0) {
            return std::make_shared<Storage>();
        }
        if (pcv_tls_scratch_active() && n <= kTlsMaxElems)
            return make_from_tls_or_new(n);
        return std::make_shared<Storage>(n);
    }

    // Issue #2406: bump cow_alloc only when the last make_storage_owned
    // was not a TLS freelist hit (compare hit counter delta is awkward;
    // use thread_local last-hit flag set in make_from_tls_or_new).
    static bool& tls_last_alloc_was_hit() noexcept {
        thread_local bool hit = false;
        return hit;
    }

    static void note_pcv_alloc() noexcept {
        if (tls_last_alloc_was_hit()) {
            tls_last_alloc_was_hit() = false;
            return; // TLS freelist hit — not a process heap cow_alloc
        }
        g_pcv_hotpath_metrics().cow_alloc_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Convert a freshly-allocated StoragePtr + size to a
    // PersistentChildVector. The storage's data has been
    // written by the caller; the size is recorded.
    static PersistentChildVector from_storage(StoragePtr s, size_type n) {
        PersistentChildVector out;
        out.data_ = std::move(s);
        out.size_ = n;
        return out;
    }

    // (share_storage was moved to a public free function
    // share_storage(pcv) below; see Issue #370.)
};

// ─────────────────────────────────────────────────────────────
//  SafePCVSpan<T> — lifetime-pinned view of a PersistentChildVector
//
//  Issue #370: raw std::span<const T> returned by
//  PersistentChildVector::data() / PersistentChildVector::begin/end()
//  borrows the underlying storage. After a with_* mutation
//  replaces the underlying PCV in FlatAST::children_[id], the
//  span — if held by a closure, MutationRecord, AI Agent state,
//  FFI buffer, etc. — dangles. Even if the storage refcount is
//  >1 (because the holder copies the span pointer), raw pointers
//  into the storage are unsafe across any modification path
//  (including rollback via MutationCheckpoint that frees the
//  storage when the LAST shared_ptr releases it).
//
//  SafePCVSpan fixes this by carrying the shared_ptr alongside
//  the span. As long as the SafePCVSpan is alive, the storage
//  it borrows stays valid. When the SafePCVSpan is destructed,
//  the shared_ptr releases its refcount.
//
//  Usage (preferred over raw span):
//
//    auto safe = flat.children_safe(id);  // SafePCVSpan<NodeId>
//    for (NodeId c : safe.span()) {
//        ...                                 // safe even after mutate
//    }
//    // safe dtor releases the shared_ptr
//
//  Raw std::span (flat.children(id).span()):
//
//    auto span = flat.children(id);         // std::span<const NodeId>
//    // WARNING: span dangles if the PCV at flat.children_[id]
//    //          is replaced via with_* or rollback before span
//    //          is read.
//
//  Trade-off: one atomic increment per call (1 ref bump). The
//  bump is amortized across all reads via the same SafePCVSpan.
//
//  Thread safety: SafePCVSpan holds an atomic shared_ptr; multiple
//  threads can hold independent SafePCVSpans of the same PCV
//  without data races (the storage is immutable).
// ─────────────────────────────────────────────────────────────
template <typename T> class SafePCVSpan {
public:
    using value_type = T;
    using size_type = typename PersistentChildVector<T>::size_type;
    using iterator = const T*;
    using const_iterator = const T*;

    SafePCVSpan() noexcept = default;
    SafePCVSpan(std::span<const T> sp,
                std::shared_ptr<const typename PersistentChildVector<T>::Storage> keep)
        : span_(sp)
        , keep_(std::move(keep)) {}

    [[nodiscard]] std::span<const T> span() const noexcept { return span_; }
    [[nodiscard]] size_type size() const noexcept { return span_.size(); }
    [[nodiscard]] bool empty() const noexcept { return span_.empty(); }
    // Issue #1520: SoAColumnar-compatible data() for pure columnar hot paths.
    [[nodiscard]] const T* data() const noexcept { return span_.data(); }
    // Issue #1624: SoAColumnarFull — preferred name for DOD hot paths.
    [[nodiscard]] std::span<const T> columnar_accessor() const noexcept { return span_; }
    [[nodiscard]] size_type stable_shape_id() const noexcept { return span_.size(); }
    [[nodiscard]] const T& operator[](size_type i) const pre(i < span_.size()) { return span_[i]; }

    // Issue #1520: range-for over pinned children without materializing.
    [[nodiscard]] const_iterator begin() const noexcept { return span_.data(); }
    [[nodiscard]] const_iterator end() const noexcept { return span_.data() + span_.size(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] const_iterator cend() const noexcept { return end(); }

    // Refcount of the kept storage. Mostly for tests; useful
    // diagnostic for AI agents to verify their hold doesn't
    // leak (held across many calls would accumulate).
    [[nodiscard]] long use_count() const noexcept { return keep_.use_count(); }

private:
    std::span<const T> span_;
    std::shared_ptr<const typename PersistentChildVector<T>::Storage> keep_;
};

// Issue #1520 / #1624: compile-time SoAColumnar / SoAColumnarFull shape
// checks for SafePCVSpan + PersistentChildVector (size/empty/data +
// columnar_accessor / stable_shape_id).
namespace detail {
    template <typename C> constexpr bool safe_pcv_soa_shape() {
        return requires(const C& c) {
            { c.size() } -> std::convertible_to<std::size_t>;
            { c.empty() } -> std::convertible_to<bool>;
            { c.data() };
        };
    }
    template <typename C> constexpr bool safe_pcv_soa_full_shape() {
        return safe_pcv_soa_shape<C>() &&
            requires(const C& c)
        {
            {c.columnar_accessor()};
            {c.stable_shape_id()}->std::convertible_to<std::size_t>;
        };
    }
} // namespace detail
static_assert(detail::safe_pcv_soa_shape<SafePCVSpan<std::uint32_t>>(),
              "Issue #1520: SafePCVSpan must expose size/empty/data for SoAColumnar");
static_assert(detail::safe_pcv_soa_full_shape<SafePCVSpan<std::uint32_t>>(),
              "Issue #1624: SafePCVSpan must satisfy SoAColumnarFull shape");
static_assert(detail::safe_pcv_soa_full_shape<PersistentChildVector<std::uint32_t>>(),
              "Issue #1624: PersistentChildVector must satisfy SoAColumnarFull shape");
// Issue #2614: ChildColumnar shape (size/empty/data + begin/end) for hot walks.
namespace detail {
    template <typename C> constexpr bool safe_pcv_child_columnar_shape() {
        return safe_pcv_soa_shape<C>() &&
            requires(const C& c)
        {
            {c.begin()};
            {c.end()};
        };
    }
} // namespace detail
static_assert(detail::safe_pcv_child_columnar_shape<SafePCVSpan<std::uint32_t>>(),
              "Issue #2614: SafePCVSpan must satisfy ChildColumnar shape (begin/end)");
static_assert(detail::safe_pcv_child_columnar_shape<PersistentChildVector<std::uint32_t>>(),
              "Issue #2614: PersistentChildVector must satisfy ChildColumnar shape");

} // namespace aura::ast

#endif // AURA_CORE_PERSISTENT_CHILD_VECTOR_HH
