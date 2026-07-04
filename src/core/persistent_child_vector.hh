// ──────────────────────────────────────────────────────────────
//  persistent_child_vector.hh — immutable (copy-on-write) vector
//  for per-node AST children.
//
//  Issue #221 (Issue #179 Cycle 3, slice 1/5): the "final form"
//  of FlatAST's children storage. Replaces the mutable
//  std::pmr::vector<NodeId> (added in #220) with a persistent
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
//  This commit ships the data structure + standalone tests.
//  Integration into FlatAST (slice 2/5), #177 rollback
//  integration (slice 3/5), and migration of structural
//  mutate operations (slice 4/5) are follow-up cycles.
//
//  Header-only, no std::pmr dependency. Uses std::shared_ptr
//  (which is reference-counted atomically — safe to share
//  across threads even though this cycle doesn't exercise
//  that). The shared_ptr copy is O(1) (atomic increment).
//
//  The header is intentionally NOT included from ast.ixx in
//  this cycle; FlatAST keeps its std::pmr::vector<NodeId>
//  children_ field. The persistent design is verified
//  standalone in test_issue_221.cpp.
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
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aura::ast {

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
    // ref-count of the underlying storage. Useful for tests
    // verifying COW semantics (a mutation should leave the
    // old storage with refcount > 1).
    long use_count() const noexcept { return data_.use_count(); }

    // Issue #300 follow-up #1: identity of the shared storage
    // block (for teardown dedup when aliased PCVs exist).
    const void* storage_identity() const noexcept { return data_.get(); }
    // Drop the shared_ptr without running its destructor
    // (used when another PCV slot still owns the refcount).
    void abandon_storage() noexcept {
        data_ = nullptr;
        size_ = 0;
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
        return result;
    }

    PersistentChildVector with_set(size_type i, const T& v) const {
        if (i >= size_)
            return *this; // no-op
        auto out = make_storage_owned(size_);
        const T* src = src_data();
        if (src)
            std::copy(src, src + size_, out->data.get());
        out->data[i] = v;
        auto result = from_storage(out, size_);
        contract_assert(result.size() == size_);
        return result;
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

    // Allocate a fresh shared storage with the given capacity.
    // The elements are uninitialized (the caller fills them).
    static StoragePtr make_storage(size_type n) {
        if (n == 0) {
            return std::make_shared<Storage>();
        }
        return std::make_shared<Storage>(n);
    }

    // Same as make_storage but the elements are filled (the
    // caller writes into the unique_ptr directly). The
    // returned StoragePtr owns the storage; the Storage is
    // mutable inside (the unique_ptr<T[]>), so the caller
    // can write to storage->data[i] before the storage is
    // shared out.
    static StoragePtr make_storage_owned(size_type n) {
        if (n == 0) {
            return std::make_shared<Storage>();
        }
        return std::make_shared<Storage>(n);
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
    SafePCVSpan() noexcept = default;
    SafePCVSpan(std::span<const T> sp,
                std::shared_ptr<const typename PersistentChildVector<T>::Storage> keep)
        : span_(sp)
        , keep_(std::move(keep)) {}

    [[nodiscard]] std::span<const T> span() const noexcept { return span_; }
    [[nodiscard]] typename PersistentChildVector<T>::size_type size() const noexcept {
        return span_.size();
    }
    [[nodiscard]] bool empty() const noexcept { return span_.empty(); }
    [[nodiscard]] const T* data() const noexcept { return span_.data(); }
    [[nodiscard]] const T& operator[](typename PersistentChildVector<T>::size_type i) const {
        return span_[i];
    }

    // Refcount of the kept storage. Mostly for tests; useful
    // diagnostic for AI agents to verify their hold doesn't
    // leak (held across many calls would accumulate).
    [[nodiscard]] long use_count() const noexcept { return keep_.use_count(); }

private:
    std::span<const T> span_;
    std::shared_ptr<const typename PersistentChildVector<T>::Storage> keep_;
};

} // namespace aura::ast

#endif // AURA_CORE_PERSISTENT_CHILD_VECTOR_HH
