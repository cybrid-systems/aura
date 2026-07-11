// ──────────────────────────────────────────────────────────────
//  gap_buffer.hh — single-gap buffer (text-editor style) for
//  O(1) amortized insert/erase + O(1) push_back when the gap
//  is at the end.
//
//  Issue #219: replaces FlatAST's `std::pmr::vector<NodeId>
//  child_data_` with a GapBuffer<NodeId> for fast structural
//  mutation.
//
//  Conventions:
//
//  * `gap_start_` and `gap_end_` are PHYSICAL positions in
//    `data_`. The gap is the half-open range [gap_start_,
//    gap_end_). The pre-gap region is [0, gap_start_); the
//    post-gap region is [gap_end_, capacity_).
//
//  * `size_` is the LOGICAL size (excludes the gap).
//    Invariant: size_ = gap_start_ + (capacity_ - gap_end_).
//
//  * Logical-to-physical mapping:
//      if l < gap_start_:  p = l
//      else:               p = l + (gap_end_ - gap_start_)
//
//  * The header avoids std containers / std allocators so it
//    can be included by C++ modules that already `import
//    std;` (avoids the GCC 16.1 std module + local std
//    #include conflict that blocks reflect.hh).
//
//  * Storage uses raw `::operator new` / `::operator delete`.
//    For trivially-copyable T (the common case for NodeId,
//    SymId, etc.) we use `::memcpy` / `::memmove` to
//    shift elements during insert/erase. For non-trivially-
//    copyable T we use move construction.
//
// Test plan (test_issue_219.cpp):
//   1. basic push_back / size / operator[] / clear
//   2. insert at arbitrary positions (front, middle, end)
//   3. erase at arbitrary positions
//   4. 5000-element insert: 100 ops, each < 10µs
//   5. 5000-element remove: 100 ops, each < 10µs
//   6. 1000 inserts + 1000 erases + 1 compact: < 1ms
//   7. roundtrip: gap buffer survives clear() + reconstruct
//   8. reserve + grow cycle (capacity doubling)
//   9. large insert/erase on pre-built AST-like data
//  10. flat wire format v1 roundtrip with GapBuffer
// ──────────────────────────────────────────────────────────────

#ifndef AURA_CORE_GAP_BUFFER_HH
#define AURA_CORE_GAP_BUFFER_HH

// Use the C standard library headers (NOT the C++ wrappers in
// <cstddef>, <cstring>, etc.) so this header can be included
// in C++ modules that already have `import std;` without
// triggering the GCC 16.1 std module + local std #include ICE.
// (The C headers put symbols in the global namespace, so they
// don't conflict with the std module's `std::` symbols.)
#include <stddef.h>    // size_t, ptrdiff_t
#include <string.h>    // memcpy, memmove
#include <new>         // ::operator new, placement new (no std namespace)
#include <type_traits> // is_trivially_copyable_v, is_trivially_destructible_v
#include <utility>     // std::move, std::swap (uses <utility> which is std-namespace)
#include <stdexcept>   // std::out_of_range
#include <atomic>
#include <cstdint>

namespace aura::ast {

// Issue #1319 Phase 1: process-wide GapBuffer structural-mutate metrics.
// Bumped by GapBuffer::insert/erase so Agents can observe O(1) path usage
// before full FlatAST children_ migration completes.
inline std::atomic<std::uint64_t> g_gap_buffer_insert_total{0};
inline std::atomic<std::uint64_t> g_gap_buffer_erase_total{0};
inline std::atomic<std::uint64_t> g_gap_buffer_structural_mutate_hits{0};

inline void record_gap_buffer_insert() noexcept {
    g_gap_buffer_insert_total.fetch_add(1, std::memory_order_relaxed);
    g_gap_buffer_structural_mutate_hits.fetch_add(1, std::memory_order_relaxed);
}
inline void record_gap_buffer_erase() noexcept {
    g_gap_buffer_erase_total.fetch_add(1, std::memory_order_relaxed);
    g_gap_buffer_structural_mutate_hits.fetch_add(1, std::memory_order_relaxed);
}

template <typename T> class GapBuffer {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;

    constexpr GapBuffer() noexcept = default;

    // Allocator-accepting constructor: GapBuffer doesn't use an
    // allocator (raw `::operator new` + `::operator delete`),
    // but FlatAST's pmr-aware constructors pass a
    // `std::pmr::polymorphic_allocator<std::byte>`. Accept any
    // single-argument and ignore it so the call site is
    // well-formed.
    template <typename A> constexpr GapBuffer(const A&) noexcept {}

    GapBuffer(const GapBuffer& other)
        : size_(other.size_)
        , capacity_(other.capacity_) {
        if (capacity_ == 0) {
            gap_start_ = gap_end_ = 0;
            return;
        }
        data_ = allocate(capacity_);
        if (other.gap_start_ > 0) {
            copy_init(data_, other.data_, other.gap_start_);
        }
        size_type post = other.capacity_ - other.gap_end_;
        if (post > 0) {
            copy_init(data_ + other.gap_end_, other.data_ + other.gap_end_, post);
        }
        gap_start_ = other.gap_start_;
        gap_end_ = other.gap_end_;
    }

    GapBuffer(GapBuffer&& other) noexcept
        : data_(other.data_)
        , size_(other.size_)
        , capacity_(other.capacity_)
        , gap_start_(other.gap_start_)
        , gap_end_(other.gap_end_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
        other.gap_start_ = 0;
        other.gap_end_ = 0;
    }

    GapBuffer& operator=(const GapBuffer& other) {
        if (this == &other)
            return *this;
        GapBuffer tmp(other);
        swap(tmp);
        return *this;
    }

    GapBuffer& operator=(GapBuffer&& other) noexcept {
        if (this == &other)
            return *this;
        destroy_all();
        ::operator delete(data_);
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        gap_start_ = other.gap_start_;
        gap_end_ = other.gap_end_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
        other.gap_start_ = 0;
        other.gap_end_ = 0;
        return *this;
    }

    ~GapBuffer() {
        destroy_all();
        ::operator delete(data_);
    }

    void swap(GapBuffer& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
        std::swap(gap_start_, other.gap_start_);
        std::swap(gap_end_, other.gap_end_);
    }

    // ── Capacity ─────────────────────────────────────────────
    constexpr size_type size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr size_type capacity() const noexcept { return capacity_; }
    constexpr size_type gap_size() const noexcept { return gap_end_ - gap_start_; }

    // ── Element access (logical) ─────────────────────────────
    reference operator[](size_type l) noexcept { return data_[logical_to_physical(l)]; }
    const_reference operator[](size_type l) const noexcept { return data_[logical_to_physical(l)]; }
    reference at(size_type l) {
        if (l >= size_)
            throw std::out_of_range("GapBuffer::at");
        return (*this)[l];
    }
    const_reference at(size_type l) const {
        if (l >= size_)
            throw std::out_of_range("GapBuffer::at");
        return (*this)[l];
    }
    reference front() noexcept { return (*this)[0]; }
    const_reference front() const noexcept { return (*this)[0]; }
    reference back() noexcept { return (*this)[size_ - 1]; }
    const_reference back() const noexcept { return (*this)[size_ - 1]; }

    // Raw data pointer (the underlying buffer; not contiguous if
    // the gap is in the middle). Call compact() first to ensure
    // the data at [data(), data() + size()) is contiguous.
    pointer data() noexcept { return data_; }
    const_pointer data() const noexcept { return data_; }

    // ── Modifiers ────────────────────────────────────────────
    void clear() noexcept {
        destroy_all();
        size_ = 0;
        gap_start_ = 0;
        gap_end_ = capacity_;
    }

    // Resize to exactly n elements. After resize, call
    // compact() if you need a contiguous buffer (e.g. for
    // serialization via data()).
    void resize(size_type n) {
        if (n == size_)
            return;
        if (n < size_) {
            for (size_type i = size_; i > n; --i) {
                erase(i - 1);
            }
        } else {
            for (size_type i = size_; i < n; ++i) {
                push_back(T{});
            }
        }
    }

    void reserve(size_type new_cap) {
        if (new_cap <= capacity_)
            return;
        reallocate_to(new_cap);
    }

    void push_back(const T& v) { insert(size_, v); }
    void push_back(T&& v) { insert(size_, std::move(v)); }

    void insert(size_type pos, const T& v) {
        if (pos > size_)
            pos = size_;
        ensure_room_for_one();
        if (pos <= gap_start_) {
            shift_right(pos, gap_start_ - pos);
            ::new (static_cast<void*>(data_ + pos)) T(v);
            ++gap_start_;
        } else {
            // Post-gap: copy (pos - old_gs) post-gap elements to the
            // pre-gap, then write v at old_ge.
            size_type old_gs = gap_start_;
            size_type old_ge = gap_end_;
            size_type n = pos - old_gs;
            if (n > 0) {
                ::memmove(data_ + old_gs, data_ + old_ge, n * sizeof(T));
            }
            ::new (static_cast<void*>(data_ + old_ge)) T(v);
            gap_start_ = pos;
            // New gap_end_ = old_gap_size + pos - 1 (1 slot for the v,
            // minus the 1 slot the v takes from the old gap).
            gap_end_ = (old_ge - old_gs) + pos - 1;
        }
        ++size_;
        record_gap_buffer_insert(); // #1319
    }

    void insert(size_type pos, T&& v) {
        if (pos > size_)
            pos = size_;
        ensure_room_for_one();
        if (pos <= gap_start_) {
            shift_right(pos, gap_start_ - pos);
            ::new (static_cast<void*>(data_ + pos)) T(std::move(v));
            ++gap_start_;
        } else {
            size_type old_gs = gap_start_;
            size_type old_ge = gap_end_;
            size_type n = pos - old_gs;
            if (n > 0) {
                ::memmove(data_ + old_gs, data_ + old_ge, n * sizeof(T));
            }
            ::new (static_cast<void*>(data_ + old_ge)) T(std::move(v));
            gap_start_ = pos;
            gap_end_ = (old_ge - old_gs) + pos - 1;
        }
        ++size_;
        record_gap_buffer_insert(); // #1319
    }

    // Append a range at the end (pos = size_). Implemented as a
    // loop of push_backs to avoid the complexity of the post-gap
    // range-insert case. The cost is O(n) for n elements, same as
    // the alternative but simpler.
    template <typename It> void append(It first, It last) {
        for (; first != last; ++first) {
            push_back(*first);
        }
    }

    void erase(size_type pos) {
        if (pos >= size_)
            return;
        if (pos < gap_start_) {
            // Pre-gap: shift [pos+1, gap_start_) left by 1.
            // The gap expands by 1 to the LEFT (absorbing the
            // freed slot at the end of the old pre-gap).
            shift_left(pos, gap_start_ - pos - 1);
            --gap_start_;
            // gap_end_ unchanged.
        } else {
            // Post-gap: don't shift. The slot at data_[phys_pos] is
            // now in the gap. The gap expands by 1 to the RIGHT.
            ++gap_end_;
        }
        --size_;
        record_gap_buffer_erase(); // #1319
    }

    // Compact: collapse the gap to the end (no-op if no gap).
    void compact() noexcept {
        if (gap_start_ == gap_end_)
            return;
        size_type post = capacity_ - gap_end_;
        if (post > 0) {
            ::memmove(data_ + gap_start_, data_ + gap_end_, post * sizeof(T));
        }
        gap_start_ = size_;
        gap_end_ = capacity_;
    }

    // Shrink the underlying storage to fit size_ (after
    // compact, the gap is at the end; truncate the gap).
    void shrink_to_fit() {
        compact();
        if (capacity_ > size_) {
            reallocate_to(size_ > 0 ? size_ : 1);
        }
    }

private:
    pointer data_ = nullptr;
    size_type size_ = 0;
    size_type capacity_ = 0;
    size_type gap_start_ = 0;
    size_type gap_end_ = 0;

    // ── Logical <-> physical mapping ─────────────────────────
    constexpr size_type logical_to_physical(size_type l) const noexcept {
        return l < gap_start_ ? l : l + (gap_end_ - gap_start_);
    }

    // ── Storage management ───────────────────────────────────
    static pointer allocate(size_type n) {
        return static_cast<pointer>(::operator new(sizeof(T) * n));
    }

    void ensure_room_for_one() {
        if (gap_start_ == gap_end_) {
            size_type new_cap = capacity_ == 0 ? 4 : capacity_ * 2;
            reallocate_to(new_cap);
        }
    }

    void reallocate_to(size_type new_cap) {
        pointer new_data = allocate(new_cap);
        if (gap_start_ > 0) {
            copy_init(new_data, data_, gap_start_);
        }
        size_type post = capacity_ - gap_end_;
        if (post > 0) {
            copy_init(new_data + gap_start_, data_ + gap_end_, post);
        }
        destroy_all();
        ::operator delete(data_);
        data_ = new_data;
        capacity_ = new_cap;
        gap_start_ = size_;
        gap_end_ = capacity_;
    }

    void destroy_all() noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (size_type i = 0; i < gap_start_; ++i) {
                data_[i].~T();
            }
            for (size_type i = gap_end_; i < capacity_; ++i) {
                data_[i].~T();
            }
        }
    }

    void shift_right(size_type pos, size_type n) {
        if (n == 0)
            return;
        if constexpr (std::is_trivially_copyable_v<T>) {
            ::memmove(data_ + pos + 1, data_ + pos, n * sizeof(T));
        } else {
            for (size_type i = n; i > 0; --i) {
                size_type src = pos + i - 1;
                size_type dst = pos + i;
                ::new (static_cast<void*>(data_ + dst)) T(std::move(data_[src]));
                data_[src].~T();
            }
        }
    }

    void shift_left(size_type pos, size_type n) {
        if (n == 0)
            return;
        if constexpr (std::is_trivially_copyable_v<T>) {
            ::memmove(data_ + pos, data_ + pos + 1, n * sizeof(T));
        } else {
            for (size_type i = 0; i < n; ++i) {
                size_type src = pos + i + 1;
                size_type dst = pos + i;
                data_[dst] = std::move(data_[src]);
                data_[src].~T();
            }
        }
    }

    static void copy_init(pointer dst, const_pointer src, size_type n) {
        if constexpr (std::is_trivially_copyable_v<T>) {
            ::memcpy(dst, src, n * sizeof(T));
        } else {
            for (size_type i = 0; i < n; ++i) {
                ::new (static_cast<void*>(dst + i)) T(src[i]);
            }
        }
    }
};

} // namespace aura::ast

#endif // AURA_CORE_GAP_BUFFER_HH
