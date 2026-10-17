// Issue #4543: the Evaluator string heap with a single, locked mint API.
//
// #2651 requires every string_heap_ growth to be serialized behind
// Evaluator::alloc_storage_lock_. The heap used to be a bare
// std::pmr::vector<std::string>, so any primitive could write the
// `idx = heap.size(); heap.push_back(s);` idiom without the lock; ~650 sites
// did, and TSan caught the race under fiber:spawn sharing (#4519 AC2a:
// free(): invalid pointer / 0xbe-fill teardown SEGV). The idiom is also a
// TOCTOU on the index even when each half is locked.
//
// LockedStringHeap makes that class of bug unrepresentable:
//   * there is no push_back / emplace_back / insert; the only way to grow
//     the heap is mint(), which takes the bound lock, appends, runs the
//     mint hook (Issue #4428 tenant stamp) and returns the new index, all
//     in one critical section;
//   * reads keep the vector surface (size / [] / iteration / implicit
//     const-vector and span views) so read-only helpers are unchanged;
//   * structural rewrites (truncate, clear, GC compaction swap) are
//     explicit, locked member functions; the raw vector is reachable only
//     through unsafe_storage(), whose callers must hold mutex() or run with
//     the world stopped.
//
// An unbound heap (no mutex: standalone test heaps) mints unlocked.
//
// Issue #4548: string-length / string-ref / substring index code points
// (#4472), and used to walk the UTF-8 bytes from 0 on every call — O(n)
// per string-length, O(offset) per string-ref, and substring copied the
// whole source first. Strings of at least kCpIndexMinBytes get a cached
// code-point index, built on first use with one scan:
//   * all ASCII: code point i is byte i (O(1));
//   * otherwise: the byte offset of every kCpIndexStride-th code point, so
//     a lookup walks at most kCpIndexStride - 1 units.
// An entry is trusted only while the slot's data pointer, byte length and
// the heap's write generation all match. The generation moves on every
// structural write (resize / clear / replace / unsafe_storage) and on
// note_write(idx), which every in-place content writer calls
// (string-fill!, f64vector-set!, slot reuse in the agent loop).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utf8_scalar.hh" // Issue #4548: code-point index cache

namespace aura::compiler {

class LockedStringHeap {
public:
    using Vec = std::pmr::vector<std::string>;
    using value_type = std::string;
    using size_type = std::size_t;
    using reference = std::string&;
    using const_reference = const std::string&;
    using iterator = Vec::iterator;
    using const_iterator = Vec::const_iterator;
    using MintHook = void (*)(void* ctx, std::size_t idx) noexcept;

    LockedStringHeap() = default;
    explicit LockedStringHeap(std::pmr::memory_resource* r)
        : v_(r) {}
    LockedStringHeap(std::pmr::memory_resource* r, std::recursive_mutex* m)
        : v_(r)
        , mu_(m) {}
    LockedStringHeap(const LockedStringHeap&) = delete;
    LockedStringHeap& operator=(const LockedStringHeap&) = delete;

    // Issue #4428: called under the lock with the minted index.
    void set_mint_hook(MintHook fn, void* ctx) noexcept {
        hook_ = fn;
        hook_ctx_ = ctx;
    }
    [[nodiscard]] std::recursive_mutex* mutex() const noexcept { return mu_; }

    // The only growth path. Returns the index of the new string.
    template <class... Args> std::uint64_t mint(Args&&... args) {
        Guard g(mu_);
        const auto idx = v_.size();
        v_.emplace_back(std::forward<Args>(args)...);
        if (hook_)
            hook_(hook_ctx_, idx);
        return static_cast<std::uint64_t>(idx);
    }

    // ── reads (vector surface) ─────────────────────────────────────
    [[nodiscard]] size_type size() const noexcept { return v_.size(); }
    [[nodiscard]] bool empty() const noexcept { return v_.empty(); }
    [[nodiscard]] size_type capacity() const noexcept { return v_.capacity(); }
    reference operator[](size_type i) noexcept { return v_[i]; }
    const_reference operator[](size_type i) const noexcept { return v_[i]; }
    reference at(size_type i) { return v_.at(i); }
    const_reference at(size_type i) const { return v_.at(i); }
    reference back() noexcept { return v_.back(); }
    const_reference back() const noexcept { return v_.back(); }
    reference front() noexcept { return v_.front(); }
    const_reference front() const noexcept { return v_.front(); }
    std::string* data() noexcept { return v_.data(); }
    const std::string* data() const noexcept { return v_.data(); }
    iterator begin() noexcept { return v_.begin(); }
    iterator end() noexcept { return v_.end(); }
    const_iterator begin() const noexcept { return v_.begin(); }
    const_iterator end() const noexcept { return v_.end(); }
    const_iterator cbegin() const noexcept { return v_.cbegin(); }
    const_iterator cend() const noexcept { return v_.cend(); }
    [[nodiscard]] const Vec& view() const noexcept { return v_; }
    operator const Vec&() const noexcept { return v_; }
    operator std::span<const std::string>() const noexcept { return {v_.data(), v_.size()}; }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return v_.get_allocator().resource();
    }

    // ── code-point index (Issue #4548) ─────────────────────────────
    static constexpr std::size_t kCpIndexMinBytes = 256;
    static constexpr std::size_t kCpIndexStride = 64;
    static constexpr std::size_t kCpIndexMaxEntries = 4096;

    // In-place content write to slot idx (same size or not): drop every
    // cached index. Call with or without the lock held.
    void note_write(size_type /*idx*/) noexcept {
        Guard g(mu_);
        ++write_gen_;
    }
    [[nodiscard]] std::uint64_t write_generation() const noexcept { return write_gen_; }

    // Code points in slot i; npos when i is out of range.
    [[nodiscard]] std::size_t code_point_count(size_type i) const {
        Guard g(mu_);
        if (i >= v_.size())
            return npos;
        const auto& s = v_[i];
        if (s.size() < kCpIndexMinBytes)
            return utf8_unit_count(s);
        return cp_index(i).cp_len;
    }
    // Code point at character index cp of slot i; -1 when out of range.
    [[nodiscard]] int code_point_at(size_type i, std::size_t cp) const {
        Guard g(mu_);
        if (i >= v_.size())
            return -1;
        const auto& s = v_[i];
        if (s.size() < kCpIndexMinBytes)
            return utf8_code_point_at(s, cp);
        const auto& m = cp_index(i);
        if (cp >= m.cp_len)
            return -1;
        if (m.ascii)
            return static_cast<int>(static_cast<unsigned char>(s[cp]));
        Utf8Unit u;
        std::size_t b = byte_offset_locked(s, m, cp);
        return utf8_unit_at(s, b, u) ? static_cast<int>(u.cp) : -1;
    }
    // Copy of code points [start, end) of slot i; past-the-end clamps,
    // start >= end is "". Only the slice is copied. False when i is out
    // of range.
    bool code_point_slice(size_type i, std::size_t start, std::size_t end, std::string& out) const {
        Guard g(mu_);
        if (i >= v_.size())
            return false;
        const auto& s = v_[i];
        std::size_t sb = 0;
        std::size_t eb = 0;
        if (s.size() < kCpIndexMinBytes) {
            sb = utf8_byte_offset(s, start);
            eb = utf8_byte_offset(s, end);
        } else {
            const auto& m = cp_index(i);
            sb = start >= m.cp_len ? s.size() : byte_offset_locked(s, m, start);
            eb = end >= m.cp_len ? s.size() : byte_offset_locked(s, m, end);
        }
        if (sb >= eb)
            out.clear();
        else
            out.assign(s, sb, eb - sb);
        return true;
    }
    [[nodiscard]] std::size_t cp_index_entries() const noexcept { return cp_meta_.size(); }

    // ── structural writes (locked) ─────────────────────────────────
    // Shrink to n (panic checkpoint restore) or pad with empty strings.
    void resize(size_type n) {
        Guard g(mu_);
        ++write_gen_;
        v_.resize(n);
    }
    void clear() noexcept {
        Guard g(mu_);
        ++write_gen_;
        v_.clear();
    }
    void shrink_to_fit() {
        Guard g(mu_);
        v_.shrink_to_fit();
    }
    void reserve(size_type n) {
        Guard g(mu_);
        v_.reserve(n);
    }
    // GC compaction: install the rebuilt heap.
    void replace(Vec&& v) {
        Guard g(mu_);
        ++write_gen_;
        v_ = std::move(v);
    }
    // Escape hatch for code that must see the raw vector (bulk remap).
    // Caller holds mutex() or runs with every mutator stopped.
    // Issue #4548: the caller may rewrite slots, so cached indexes go.
    Vec& unsafe_storage() noexcept {
        ++write_gen_;
        return v_;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    struct CpIndex {
        const char* data = nullptr;
        std::size_t bytes = 0;
        std::uint64_t gen = 0;
        std::size_t cp_len = 0;
        bool ascii = false;
        std::vector<std::size_t> stride_offsets; // byte offset of cp k*stride
    };

    // Caller holds the lock and has checked i < size() and the threshold.
    const CpIndex& cp_index(size_type i) const {
        const auto& s = v_[i];
        // Any write since the last build invalidates every entry: drop
        // them all so dead strings' indexes do not linger.
        if (cp_meta_gen_ != write_gen_) {
            cp_meta_.clear();
            cp_meta_gen_ = write_gen_;
        }
        auto it = cp_meta_.find(i);
        if (it != cp_meta_.end() && it->second.data == s.data() && it->second.bytes == s.size() &&
            it->second.gen == write_gen_)
            return it->second;
        if (it == cp_meta_.end()) {
            if (cp_meta_.size() >= kCpIndexMaxEntries)
                cp_meta_.clear();
            it = cp_meta_.try_emplace(i).first;
        }
        CpIndex& m = it->second;
        m.data = s.data();
        m.bytes = s.size();
        m.gen = write_gen_;
        m.stride_offsets.clear();
        bool ascii = true;
        {
            const char* p = s.data();
            const std::size_t n = s.size();
            std::size_t k = 0;
            for (; k + 8 <= n; k += 8) {
                std::uint64_t w;
                std::memcpy(&w, p + k, 8);
                if (w & 0x8080808080808080ull) {
                    ascii = false;
                    break;
                }
            }
            for (; ascii && k < n; ++k)
                ascii = static_cast<unsigned char>(p[k]) < 0x80;
        }
        m.ascii = ascii;
        if (ascii) {
            m.cp_len = s.size();
            return m;
        }
        std::size_t b = 0;
        std::size_t n = 0;
        Utf8Unit u;
        while (utf8_unit_at(s, b, u)) {
            if (n % kCpIndexStride == 0)
                m.stride_offsets.push_back(b);
            b += u.nbytes;
            ++n;
        }
        m.cp_len = n;
        return m;
    }
    // Byte offset of code point cp < m.cp_len (non-ASCII: stride + walk).
    static std::size_t byte_offset_locked(const std::string& s, const CpIndex& m,
                                          std::size_t cp) noexcept {
        if (m.ascii)
            return cp;
        std::size_t b = m.stride_offsets[cp / kCpIndexStride];
        Utf8Unit u;
        for (std::size_t k = cp % kCpIndexStride; k > 0 && utf8_unit_at(s, b, u); --k)
            b += u.nbytes;
        return b;
    }

    struct Guard {
        std::recursive_mutex* m;
        explicit Guard(std::recursive_mutex* mm)
            : m(mm) {
            if (m)
                m->lock();
        }
        ~Guard() {
            if (m)
                m->unlock();
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };
    Vec v_;
    std::recursive_mutex* mu_ = nullptr;
    MintHook hook_ = nullptr;
    void* hook_ctx_ = nullptr;
    // Issue #4548: guarded by mu_ like v_.
    std::uint64_t write_gen_ = 0;
    mutable std::uint64_t cp_meta_gen_ = 0;
    mutable std::unordered_map<size_type, CpIndex> cp_meta_;
};

// Generic mint for helpers templated on the heap type (LockedStringHeap or a
// plain std::vector<std::string> scratch heap).
template <class Heap> inline std::uint64_t heap_mint(Heap& h, std::string s) {
    if constexpr (requires { h.mint(std::move(s)); }) {
        return h.mint(std::move(s));
    } else {
        const auto idx = h.size();
        h.push_back(std::move(s));
        return static_cast<std::uint64_t>(idx);
    }
}

} // namespace aura::compiler
