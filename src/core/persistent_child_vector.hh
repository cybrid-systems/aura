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
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aura::ast {

template <typename T>
class PersistentChildVector {
public:
    using value_type     = T;
    using size_type      = std::size_t;
    using difference_type = std::ptrdiff_t;
    // All accessors return const references / pointers — the
    // vector is immutable. Mutations go through the with_*
    // methods, which return a new vector.
    using reference      = const T&;
    using const_reference = const T&;
    using pointer        = const T*;
    using const_pointer  = const T*;
    using iterator       = const T*;
    using const_iterator = const T*;

    constexpr PersistentChildVector() noexcept = default;

    PersistentChildVector(std::initializer_list<T> init) {
        if (init.size() == 0) return;
        size_ = init.size();
        data_ = make_storage(init.size());
        std::copy(init.begin(), init.end(), data_->data.get());
    }

    explicit PersistentChildVector(size_type n) {
        if (n == 0) return;
        size_ = n;
        data_ = make_storage(n);
        // value-initialized
        for (std::size_t i = 0; i < n; ++i) data_->data[i] = T{};
    }

    PersistentChildVector(size_type n, const T& v) {
        if (n == 0) return;
        size_ = n;
        data_ = make_storage(n);
        std::fill_n(data_->data.get(), n, v);
    }

    // Copy / move: O(1) (shared_ptr copy).
    PersistentChildVector(const PersistentChildVector&) = default;
    PersistentChildVector(PersistentChildVector&&) noexcept = default;
    PersistentChildVector& operator=(const PersistentChildVector&) = default;
    PersistentChildVector& operator=(PersistentChildVector&&) noexcept = default;

    // ── Capacity (const) ──────────────────────────────────────
    constexpr size_type size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr const_pointer data() const noexcept {
        return data_ ? data_->data.get() : nullptr;
    }
    // ref-count of the underlying storage. Useful for tests
    // verifying COW semantics (a mutation should leave the
    // old storage with refcount > 1).
    long use_count() const noexcept { return data_.use_count(); }

    // ── Element access (const) ───────────────────────────────
    const_reference operator[](size_type i) const noexcept {
        return data_->data[i];
    }
    const_reference at(size_type i) const {
        if (i >= size_) throw std::out_of_range("PersistentChildVector::at");
        return data_->data[i];
    }
    const_reference front() const { return data_->data[0]; }
    const_reference back() const { return data_->data[size_ - 1]; }

    // ── Iterators (const) ──────────────────────────────────────
    const_iterator begin() const noexcept { return data_->data.get(); }
    const_iterator end() const noexcept { return data_->data.get() + size_; }
    const_iterator cbegin() const noexcept { return data_->data.get(); }
    const_iterator cend() const noexcept { return data_->data.get() + size_; }

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
    const T* src_data() const noexcept {
        return data_ ? data_->data.get() : nullptr;
    }

    PersistentChildVector with_push_back(const T& v) const {
        auto out = make_storage_owned(size_ + 1);
        const T* src = src_data();
        if (src) std::copy(src, src + size_, out->data.get());
        out->data[size_] = v;
        return from_storage(out, size_ + 1);
    }

    PersistentChildVector with_push_back(T&& v) const {
        auto out = make_storage_owned(size_ + 1);
        const T* src = src_data();
        if (src) std::move(src, src + size_, out->data.get());
        out->data[size_] = std::move(v);
        return from_storage(out, size_ + 1);
    }

    PersistentChildVector with_insert(size_type pos, const T& v) const {
        if (pos > size_) pos = size_;
        auto out = make_storage_owned(size_ + 1);
        const T* src = src_data();
        if (src) {
            std::copy(src, src + pos, out->data.get());
            std::copy(src + pos, src + size_, out->data.get() + pos + 1);
        }
        out->data[pos] = v;
        return from_storage(out, size_ + 1);
    }

    PersistentChildVector with_erase(size_type pos) const {
        if (pos >= size_) return *this;  // no-op
        auto out = make_storage_owned(size_ - 1);
        const T* src = src_data();
        if (src) {
            std::copy(src, src + pos, out->data.get());
            std::copy(src + pos + 1, src + size_, out->data.get() + pos);
        }
        return from_storage(out, size_ - 1);
    }

    PersistentChildVector with_set(size_type i, const T& v) const {
        if (i >= size_) return *this;  // no-op
        auto out = make_storage_owned(size_);
        const T* src = src_data();
        if (src) std::copy(src, src + size_, out->data.get());
        out->data[i] = v;
        return from_storage(out, size_);
    }

    // ── Comparison ───────────────────────────────────────────
    bool operator==(const PersistentChildVector& other) const {
        if (size_ != other.size_) return false;
        if (size_ == 0) return true;  // both empty
        return std::equal(data_->data.get(),
                          data_->data.get() + size_,
                          other.data_->data.get());
    }
    bool operator!=(const PersistentChildVector& other) const {
        return !(*this == other);
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
        Storage(size_type n) : data(std::make_unique<T[]>(n)), size(n) {}
    };

    using StoragePtr = std::shared_ptr<const Storage>;

    StoragePtr data_;
    size_type size_ = 0;  // mirrored from data_->size for O(1) access

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
    static PersistentChildVector from_storage(StoragePtr s,
                                              size_type n) {
        PersistentChildVector out;
        out.data_ = std::move(s);
        out.size_ = n;
        return out;
    }
};

}  // namespace aura::ast

#endif  // AURA_CORE_PERSISTENT_CHILD_VECTOR_HH
