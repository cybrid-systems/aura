// atomic_column.hh - Issue #5476: an atomically readable SoA column.
//
// FlatAST's node metadata columns (node_gen_ today, dirty/type later) are
// read by tree-walk bodies on other fibers while a mutation boundary
// restamps them. The live storage is a TrackedColumn (pmr::vector) whose
// growth reallocates, so std::atomic_ref on its elements is not enough: a
// reader can address freed storage. This wrapper keeps the TrackedColumn
// as the image/copy-of-record store and mirrors every value into a
// std::deque<std::atomic<T>>, whose element addresses are stable across
// growth. Reads (including the non-const proxy's conversion) come from
// the mirror with acquire/release; writes go to both.
//
// Image API delegates to the TrackedColumn, so capture/restore/equals/clean
// keep the existing COW semantics. restore and wholesale assignment rebuild
// the mirror from the store.
#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/cow_column.hh"

namespace aura::ast::cow {

template <class T> class AtomicTrackedColumn {
public:
    using store_type = TrackedColumn<T>;
    using image_type = typename store_type::image_type;
    using value_type = T;
    using size_type = std::size_t;

    AtomicTrackedColumn() = default;
    template <class A>
        requires std::is_constructible_v<store_type, A>
    explicit AtomicTrackedColumn(A&& alloc)
        : store_(std::forward<A>(alloc)) {}

    AtomicTrackedColumn(const AtomicTrackedColumn& o)
        : store_(o.store_) {
        rebuild_mirror();
    }
    AtomicTrackedColumn(AtomicTrackedColumn&& o) noexcept
        : store_(std::move(o.store_))
        , mirror_(std::move(o.mirror_))
        , mirror_size_(o.mirror_size_.load(std::memory_order_relaxed)) {}
    AtomicTrackedColumn& operator=(const AtomicTrackedColumn& o) {
        if (this != &o) {
            store_ = o.store_;
            rebuild_mirror();
        }
        return *this;
    }
    AtomicTrackedColumn& operator=(AtomicTrackedColumn&& o) noexcept {
        if (this != &o) {
            store_ = std::move(o.store_);
            mirror_ = std::move(o.mirror_);
            mirror_size_.store(o.mirror_size_.load(std::memory_order_relaxed),
                               std::memory_order_release);
        }
        return *this;
    }
    // Wholesale replacement from a raw column (compact_nodes rebuilds a
    // pmr vector and assigns it in one move).
    template <class U>
        requires std::is_assignable_v<store_type&, U&&>
    AtomicTrackedColumn& operator=(U&& v) {
        store_ = std::forward<U>(v);
        rebuild_mirror();
        return *this;
    }

    // Readers call size() from tree-walk bodies while a parser/mutation
    // appends on another fiber; the deque's own size() is not atomic, so the
    // published counter is the read face (the store remains the image).
    [[nodiscard]] size_type size() const noexcept {
        return mirror_size_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    // Compatibility view for the (unused) *_view() span accessors. It is
    // the COW store, not the mirror: do not read it concurrently with a
    // writer — new code should use operator[] (the atomic mirror).
    [[nodiscard]] const T* data() const noexcept { return store_.data(); }
    [[nodiscard]] size_type capacity() const noexcept { return store_.capacity(); }
    [[nodiscard]] auto get_allocator() const { return store_.get_allocator(); }
    void reserve(size_type n) { store_.reserve(n); }
    // Snapshot copy source: the COW store (mirror values are mirrored on
    // every write, so under the writer's lock this equals the live column).
    [[nodiscard]] const typename store_type::base_type& view() const noexcept {
        return store_.view();
    }

    // Read-only: the mirror is the only element storage a reader touches.
    [[nodiscard]] T operator[](size_type i) const noexcept {
        return mirror_[i].load(std::memory_order_acquire);
    }
    [[nodiscard]] T at(size_type i) const { return mirror_.at(i).load(std::memory_order_acquire); }
    [[nodiscard]] T front() const noexcept {
        return mirror_.front().load(std::memory_order_acquire);
    }
    [[nodiscard]] T back() const noexcept { return mirror_.back().load(std::memory_order_acquire); }

    // Write-capable proxy: a read through it loads the mirror; an assignment
    // updates the image store and the mirror.
    class Ref {
    public:
        Ref(AtomicTrackedColumn& col, size_type i) noexcept
            : col_(&col)
            , i_(i) {}
        operator T() const noexcept { return col_->mirror_[i_].load(std::memory_order_acquire); }
        Ref& operator=(T v) noexcept {
            col_->store_[i_] = v; // marks the chunk dirty for the image
            col_->mirror_[i_].store(v, std::memory_order_release);
            return *this;
        }
        Ref& operator=(const Ref& o) noexcept { return *this = static_cast<T>(o); }
        Ref& operator|=(T v) noexcept {
            const T prev = col_->mirror_[i_].fetch_or(v, std::memory_order_acq_rel);
            col_->store_[i_] = static_cast<T>(prev | v);
            return *this;
        }
        Ref& operator&=(T v) noexcept {
            const T prev = col_->mirror_[i_].load(std::memory_order_acquire);
            const T next = static_cast<T>(prev & v);
            col_->store_[i_] = next;
            col_->mirror_[i_].store(next, std::memory_order_release);
            return *this;
        }

    private:
        AtomicTrackedColumn* col_;
        size_type i_;
    };
    [[nodiscard]] Ref operator[](size_type i) noexcept { return Ref(*this, i); }
    [[nodiscard]] Ref at(size_type i) { return Ref(*this, i); }
    [[nodiscard]] Ref front() noexcept { return Ref(*this, 0); }
    [[nodiscard]] Ref back() noexcept { return Ref(*this, size() - 1); }

    // Direct cell operations for writers that used std::atomic_ref on the
    // old vector. The mirror is the atomic of record; the store keeps the
    // COW image/dirty semantics.
    [[nodiscard]] T load(size_type i,
                         std::memory_order order = std::memory_order_acquire) const noexcept {
        return mirror_[i].load(order);
    }
    void store(size_type i, T v, std::memory_order order = std::memory_order_release) noexcept {
        store_[i] = v; // marks the chunk dirty
        mirror_[i].store(v, order);
    }
    [[nodiscard]] T fetch_or(size_type i, T v,
                             std::memory_order order = std::memory_order_acq_rel) noexcept {
        const T prev = mirror_[i].fetch_or(v, order);
        store_[i] = static_cast<T>(prev | v);
        return prev;
    }

    void push_back(T v) {
        store_.push_back(v);
        mirror_.emplace_back(v);
        mirror_size_.store(mirror_.size(), std::memory_order_release);
    }
    void clear() noexcept {
        store_.clear();
        mirror_.clear();
        mirror_size_.store(0, std::memory_order_release);
    }
    void resize(size_type n) {
        const auto old = store_.size();
        store_.resize(n);
        if (n > old) {
            for (auto i = old; i < n; ++i)
                mirror_.emplace_back(T{});
        } else {
            while (mirror_.size() > n)
                mirror_.pop_back();
        }
        mirror_size_.store(mirror_.size(), std::memory_order_release);
    }
    void resize(size_type n, T v) {
        const auto old = store_.size();
        store_.resize(n, v);
        if (n > old) {
            for (auto i = old; i < n; ++i)
                mirror_.emplace_back(v);
        } else if (n < old) {
            while (mirror_.size() > n)
                mirror_.pop_back();
        }
        mirror_size_.store(mirror_.size(), std::memory_order_release);
    }
    // Serialization helpers: a contiguous copy of the current values and a
    // raw assign back (the image store is the copy-of-record).
    [[nodiscard]] std::vector<T> snapshot() const {
        std::vector<T> out;
        out.reserve(size());
        for (std::size_t i = 0; i < size(); ++i)
            out.push_back(mirror_[i].load(std::memory_order_acquire));
        return out;
    }
    void assign_from_raw(const T* p, size_type n) {
        store_.assign(p, p + n);
        rebuild_mirror();
    }

    // ── image API (delegates to the TrackedColumn) ────────────────────
    [[nodiscard]] image_type capture(const image_type* base, std::size_t* fresh = nullptr,
                                     std::size_t* total = nullptr) {
        return store_.capture(base, fresh, total);
    }
    void restore_from(const image_type& img, const image_type* base) {
        store_.restore_from(img, base);
        rebuild_mirror();
    }
    [[nodiscard]] bool equals_image(const image_type& img) const {
        return store_.equals_image(img);
    }
    [[nodiscard]] bool clean() const noexcept { return store_.clean(); }

private:
    void rebuild_mirror() {
        mirror_.clear();
        const auto& view = store_.view();
        for (const auto& v : view)
            mirror_.emplace_back(v);
        mirror_size_.store(mirror_.size(), std::memory_order_release);
    }

    store_type store_{};
    std::deque<std::atomic<T>> mirror_;
    std::atomic<size_type> mirror_size_{0};
};

} // namespace aura::ast::cow
