// cow_column.hh — Issue #4772: chunk-tracked SoA columns + structural-sharing
// images for O(changed) ast:snapshot / ast:restore.
//
// A TrackedColumn<T> is a drop-in for the std::pmr::vector<T> SoA columns of
// FlatAST / StringPool. The live storage stays one contiguous vector (hot
// reads are unchanged: the const API is the base vector's), and every
// write-capable access marks the fixed-size chunk it touches dirty. An image
// (ColumnImage<T>) is a frozen, immutable list of shared chunks. Capturing a
// new image against the previous one (the column's "base") copies only the
// chunks written since that base and shares every other chunk by pointer, so a
// snapshot after a one-define edit costs O(changed chunks + n/chunk pointer
// copies) instead of an O(n) deep copy. Restoring an image against the base
// copies back only the chunks that differ.
//
// Soundness rules (read before touching a column):
//   * The const API never marks. Any non-const access that can write
//     (operator[], at, front/back, data, begin/end, every size change) marks
//     the touched chunk(s) BEFORE returning, so a write through the returned
//     reference is always covered. A reference held across a capture() and
//     written afterwards is NOT covered — do not keep column references
//     across a primitive / mutation boundary (none of FlatAST's code does).
//   * Any wholesale replacement (copy/move assign, assign from a raw vector,
//     clear, swap, a fresh object) sets all_dirty_: the next capture falls
//     back to a content compare against the base for every chunk (sharing
//     only byte-identical chunks) and the next restore copies every chunk.
//   * Dirty cells are written with relaxed atomic_ref stores: mutable cache
//     columns are marked from const methods on reader threads, and the bit is
//     the only shared state they touch.
//   * capture() clears a chunk's bit (exchange) BEFORE copying it, so a
//     concurrent write lands either in this image or re-marks the chunk for
//     the next one; it can never be lost from both.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>
#include <vector>

namespace aura::ast::cow {

// Chunk geometry: ~2 KiB of payload per chunk (pow2 element count, 64..2048).
template <class T> constexpr std::size_t chunk_shift() noexcept {
    if constexpr (sizeof(T) <= 1)
        return 11;
    else if constexpr (sizeof(T) <= 2)
        return 10;
    else if constexpr (sizeof(T) <= 4)
        return 9;
    else if constexpr (sizeof(T) <= 8)
        return 8;
    else if constexpr (sizeof(T) <= 16)
        return 7;
    else
        return 6;
}

// Process-wide capture accounting (tests / bench / stats).
struct CowStats {
    std::atomic<std::uint64_t> chunks_shared{0};
    std::atomic<std::uint64_t> chunks_copied{0};
    std::atomic<std::uint64_t> bytes_copied{0};
    std::atomic<std::uint64_t> restore_chunks_skipped{0};
    std::atomic<std::uint64_t> restore_chunks_copied{0};
};
inline CowStats& cow_stats() noexcept {
    static CowStats s;
    return s;
}

template <class T> struct ColumnImage {
    using chunk_type = std::vector<T>;
    static constexpr std::size_t kShift = chunk_shift<T>();
    static constexpr std::size_t kChunk = std::size_t{1} << kShift;
    std::vector<std::shared_ptr<const chunk_type>> chunks;
    std::size_t size = 0;

    [[nodiscard]] const T& at(std::size_t i) const noexcept {
        return (*chunks[i >> kShift])[i & (kChunk - 1)];
    }
    [[nodiscard]] bool empty() const noexcept { return size == 0; }
};

template <class T, class Base = std::pmr::vector<T>> class TrackedColumn : private Base {
public:
    using base_type = Base;
    using image_type = ColumnImage<T>;
    using value_type = typename Base::value_type;
    using size_type = typename Base::size_type;
    using difference_type = typename Base::difference_type;
    using reference = typename Base::reference;
    using const_reference = typename Base::const_reference;
    using pointer = typename Base::pointer;
    using const_pointer = typename Base::const_pointer;
    using iterator = typename Base::iterator;
    using const_iterator = typename Base::const_iterator;
    using reverse_iterator = typename Base::reverse_iterator;
    using const_reverse_iterator = typename Base::const_reverse_iterator;
    using allocator_type = typename Base::allocator_type;
    static constexpr std::size_t kShift = image_type::kShift;
    static constexpr std::size_t kChunk = image_type::kChunk;

    TrackedColumn() = default;
    template <class A>
        requires std::is_constructible_v<Base, A>
    explicit TrackedColumn(A&& alloc)
        : Base(std::forward<A>(alloc)) {}
    TrackedColumn(const TrackedColumn& o)
        : Base(static_cast<const Base&>(o)) {}
    TrackedColumn(TrackedColumn&& o) noexcept
        : Base(static_cast<Base&&>(o)) {
        o.all_dirty_ = true;
    }
    TrackedColumn& operator=(const TrackedColumn& o) {
        if (this != &o) {
            Base::operator=(static_cast<const Base&>(o));
            all_dirty_ = true;
        }
        return *this;
    }
    TrackedColumn& operator=(TrackedColumn&& o) noexcept {
        if (this != &o) {
            Base::operator=(static_cast<Base&&>(o));
            all_dirty_ = true;
            o.all_dirty_ = true;
        }
        return *this;
    }
    TrackedColumn& operator=(const Base& v) {
        Base::operator=(v);
        all_dirty_ = true;
        return *this;
    }
    TrackedColumn& operator=(Base&& v) {
        Base::operator=(std::move(v));
        all_dirty_ = true;
        return *this;
    }

    // ── read-only surface (never marks) ─────────────────────────────
    using Base::capacity;
    using Base::empty;
    using Base::get_allocator;
    using Base::max_size;
    using Base::size;
    [[nodiscard]] const Base& view() const noexcept { return *this; }
    operator const Base&() const noexcept { return *this; }
    [[nodiscard]] const_reference operator[](size_type i) const noexcept {
        return Base::operator[](i);
    }
    [[nodiscard]] const_reference at(size_type i) const { return Base::at(i); }
    [[nodiscard]] const_reference front() const { return Base::front(); }
    [[nodiscard]] const_reference back() const { return Base::back(); }
    [[nodiscard]] const_pointer data() const noexcept { return Base::data(); }
    [[nodiscard]] const_iterator begin() const noexcept { return Base::begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return Base::end(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return Base::cbegin(); }
    [[nodiscard]] const_iterator cend() const noexcept { return Base::cend(); }
    [[nodiscard]] const_reverse_iterator rbegin() const noexcept { return Base::rbegin(); }
    [[nodiscard]] const_reverse_iterator rend() const noexcept { return Base::rend(); }
    friend bool operator==(const TrackedColumn& a, const TrackedColumn& b) {
        return a.view() == b.view();
    }

    // ── write-capable surface (marks before returning) ──────────────
    [[nodiscard]] reference operator[](size_type i) noexcept {
        mark(i);
        return Base::operator[](i);
    }
    [[nodiscard]] reference at(size_type i) {
        mark(i);
        return Base::at(i);
    }
    [[nodiscard]] reference front() {
        mark(0);
        return Base::front();
    }
    [[nodiscard]] reference back() {
        mark(Base::size() - 1);
        return Base::back();
    }
    [[nodiscard]] pointer data() noexcept {
        all_dirty_ = true;
        return Base::data();
    }
    [[nodiscard]] iterator begin() noexcept {
        all_dirty_ = true;
        return Base::begin();
    }
    [[nodiscard]] iterator end() noexcept {
        all_dirty_ = true;
        return Base::end();
    }
    [[nodiscard]] reverse_iterator rbegin() noexcept {
        all_dirty_ = true;
        return Base::rbegin();
    }
    [[nodiscard]] reverse_iterator rend() noexcept {
        all_dirty_ = true;
        return Base::rend();
    }
    // Explicit escape hatch: whole-vector mutable access (marks all).
    [[nodiscard]] Base& mut() noexcept {
        all_dirty_ = true;
        return *this;
    }

    template <class U> void push_back(U&& v) {
        Base::push_back(std::forward<U>(v));
        grow_marks();
        mark(Base::size() - 1);
    }
    template <class... A> reference emplace_back(A&&... a) {
        auto& r = Base::emplace_back(std::forward<A>(a)...);
        grow_marks();
        mark(Base::size() - 1);
        return r;
    }
    void pop_back() {
        mark(Base::size() - 1);
        Base::pop_back();
    }
    void resize(size_type n) {
        const auto old = Base::size();
        Base::resize(n);
        grow_marks();
        mark_range(std::min(old, n), std::max(old, n));
    }
    void resize(size_type n, const T& v) {
        const auto old = Base::size();
        Base::resize(n, v);
        grow_marks();
        mark_range(std::min(old, n), std::max(old, n));
    }
    void reserve(size_type n) { Base::reserve(n); }
    void shrink_to_fit() { Base::shrink_to_fit(); }
    void clear() noexcept {
        Base::clear();
        all_dirty_ = true;
    }
    template <class... A> void assign(A&&... a) {
        Base::assign(std::forward<A>(a)...);
        all_dirty_ = true;
    }
    template <class... A> iterator insert(const_iterator pos, A&&... a) {
        // Append (insert at end) only touches the new tail — the StringPool
        // buffer grows this way on every intern.
        const auto idx = static_cast<std::size_t>(pos - Base::cbegin());
        const auto old = Base::size();
        auto it = Base::insert(pos, std::forward<A>(a)...);
        if (idx == old) {
            grow_marks();
            mark_range(old, Base::size());
        } else {
            all_dirty_ = true;
        }
        return it;
    }
    template <class... A> iterator emplace(const_iterator pos, A&&... a) {
        all_dirty_ = true;
        return Base::emplace(pos, std::forward<A>(a)...);
    }
    iterator erase(const_iterator pos) {
        all_dirty_ = true;
        return Base::erase(pos);
    }
    iterator erase(const_iterator first, const_iterator last) {
        all_dirty_ = true;
        return Base::erase(first, last);
    }
    void swap(TrackedColumn& o) noexcept {
        Base::swap(static_cast<Base&>(o));
        all_dirty_ = true;
        o.all_dirty_ = true;
    }
    void swap(Base& o) noexcept {
        Base::swap(o);
        all_dirty_ = true;
    }
    friend void swap(TrackedColumn& a, TrackedColumn& b) noexcept { a.swap(b); }

    // ── image API ───────────────────────────────────────────────────
    [[nodiscard]] bool clean() const noexcept {
        if (all_dirty_)
            return false;
        if (dirty_.size() < ((Base::size() + kChunk - 1) >> kShift))
            return false;
        for (std::size_t c = 0; c < dirty_.size(); ++c)
            if (load_bit(c))
                return false;
        return true;
    }
    void mark_all_dirty() noexcept { all_dirty_ = true; }

    // Freeze the current contents. Chunks clean relative to `base` (the image
    // this column was last captured into / restored from) are shared.
    [[nodiscard]] image_type capture(const image_type* base, std::size_t* fresh_out = nullptr,
                                     std::size_t* total_out = nullptr) {
        image_type img;
        const auto n = Base::size();
        const auto nc = (n + kChunk - 1) >> kShift;
        img.size = n;
        img.chunks.resize(nc);
        // Bits are only a fast filter: with all_dirty_ (wholesale write)
        // chunks can still be shared when their bytes equal the base.
        const bool bits_valid = base != nullptr && !all_dirty_;
        if (dirty_.size() < nc)
            dirty_.resize(nc, 1);
        auto& st = cow_stats();
        std::uint64_t shared = 0, copied = 0, bytes = 0;
        for (std::size_t c = 0; c < nc; ++c) {
            const auto lo = c << kShift;
            const auto hi = std::min(n, lo + kChunk);
            const bool was_dirty = exchange_bit(c);
            const bool base_ok = base != nullptr && c < base->chunks.size() && base->chunks[c] &&
                                 base->chunks[c]->size() == hi - lo;
            if (base_ok && bits_valid && !was_dirty) {
                img.chunks[c] = base->chunks[c];
                ++shared;
            } else if (base_ok && same_as_chunk(*base->chunks[c], lo)) {
                // Written but value-identical (a store of the same value, a
                // non-const read): still share — content equality is the
                // soundness condition, the bit is only a fast filter.
                img.chunks[c] = base->chunks[c];
                ++shared;
            } else {
                img.chunks[c] = std::make_shared<const std::vector<T>>(
                    Base::begin() + static_cast<difference_type>(lo),
                    Base::begin() + static_cast<difference_type>(hi));
                ++copied;
                bytes += (hi - lo) * sizeof(T);
            }
        }
        dirty_.resize(nc);
        all_dirty_ = false;
        st.chunks_shared.fetch_add(shared, std::memory_order_relaxed);
        st.chunks_copied.fetch_add(copied, std::memory_order_relaxed);
        st.bytes_copied.fetch_add(bytes, std::memory_order_relaxed);
        if (fresh_out)
            *fresh_out += copied;
        if (total_out)
            *total_out += nc;
        return img;
    }

    // Make the contents equal `img`. With a valid `base` only chunks that are
    // dirty or differ (by pointer) between base and img are copied back.
    void restore_from(const image_type& img, const image_type* base) {
        const bool can_skip = base != nullptr && !all_dirty_;
        const auto old_n = Base::size();
        Base::resize(img.size);
        const auto nc = img.chunks.size();
        if (dirty_.size() < nc)
            dirty_.resize(nc, 1);
        auto& st = cow_stats();
        std::uint64_t skipped = 0, copied = 0;
        for (std::size_t c = 0; c < nc; ++c) {
            const auto lo = c << kShift;
            const bool was_dirty = exchange_bit(c);
            const bool same = can_skip && !was_dirty && c < base->chunks.size() &&
                              base->chunks[c] == img.chunks[c] &&
                              // A grown live tail inside this chunk would be
                              // dirty already; the size guard is belt+braces.
                              lo + img.chunks[c]->size() <= old_n;
            if (same) {
                ++skipped;
                continue;
            }
            std::copy(img.chunks[c]->begin(), img.chunks[c]->end(),
                      Base::begin() + static_cast<difference_type>(lo));
            ++copied;
        }
        dirty_.resize(nc);
        std::fill(dirty_.begin(), dirty_.end(), std::uint8_t{0});
        all_dirty_ = false;
        st.restore_chunks_skipped.fetch_add(skipped, std::memory_order_relaxed);
        st.restore_chunks_copied.fetch_add(copied, std::memory_order_relaxed);
    }

    // Debug / test oracle: contents equal the image element-for-element.
    [[nodiscard]] bool equals_image(const image_type& img) const {
        if (img.size != Base::size())
            return false;
        if constexpr (requires(const T& a, const T& b) { a == b; }) {
            for (std::size_t i = 0; i < img.size; ++i)
                if (!(Base::operator[](i) == img.at(i)))
                    return false;
        }
        return true;
    }

private:
    [[nodiscard]] bool same_as_chunk(const std::vector<T>& ch, std::size_t lo) const {
        if constexpr (std::is_trivially_copyable_v<T> && std::has_unique_object_representations_v<T>) {
            return ch.empty() ||
                   std::memcmp(ch.data(), Base::data() + lo, ch.size() * sizeof(T)) == 0;
        } else if constexpr (requires(const T& a, const T& b) {
                                 { a == b } -> std::convertible_to<bool>;
                             }) {
            for (std::size_t i = 0; i < ch.size(); ++i)
                if (!(ch[i] == Base::operator[](lo + i)))
                    return false;
            return true;
        } else {
            return false;
        }
    }
    void grow_marks() {
        const auto need = (Base::size() + kChunk - 1) >> kShift;
        if (dirty_.size() < need)
            dirty_.resize(need, 1);
    }
    void mark(std::size_t i) noexcept {
        const auto c = i >> kShift;
        if (c < dirty_.size())
            std::atomic_ref<std::uint8_t>(dirty_[c]).store(1, std::memory_order_relaxed);
        // c beyond the bitmap: the chunk is newer than the last capture and
        // capture() treats every unmapped chunk as dirty (resize(…, 1)).
    }
    void mark_range(std::size_t lo, std::size_t hi) noexcept {
        if (lo >= hi)
            return;
        for (auto c = lo >> kShift; c <= ((hi - 1) >> kShift) && c < dirty_.size(); ++c)
            std::atomic_ref<std::uint8_t>(dirty_[c]).store(1, std::memory_order_relaxed);
    }
    [[nodiscard]] bool load_bit(std::size_t c) const noexcept {
        return std::atomic_ref<std::uint8_t>(const_cast<std::uint8_t&>(dirty_[c]))
                   .load(std::memory_order_relaxed) != 0;
    }
    [[nodiscard]] bool exchange_bit(std::size_t c) noexcept {
        return std::atomic_ref<std::uint8_t>(dirty_[c]).exchange(0, std::memory_order_relaxed) != 0;
    }

    std::vector<std::uint8_t> dirty_;
    bool all_dirty_ = true;
};

// Side-table maps (region tables): one change flag, image = shared frozen copy.
template <class Map> class TrackedMap : private Map {
public:
    using base_type = Map;
    using image_type = std::shared_ptr<const Map>;
    using key_type = typename Map::key_type;
    using mapped_type = typename Map::mapped_type;
    using value_type = typename Map::value_type;
    using size_type = typename Map::size_type;
    using iterator = typename Map::iterator;
    using const_iterator = typename Map::const_iterator;
    using allocator_type = typename Map::allocator_type;

    TrackedMap() = default;
    template <class A>
        requires std::is_constructible_v<Map, A>
    explicit TrackedMap(A&& alloc)
        : Map(std::forward<A>(alloc)) {}
    TrackedMap(const TrackedMap& o)
        : Map(static_cast<const Map&>(o)) {}
    TrackedMap(TrackedMap&& o) noexcept
        : Map(static_cast<Map&&>(o)) {
        o.touch();
    }
    TrackedMap& operator=(const TrackedMap& o) {
        if (this != &o) {
            Map::operator=(static_cast<const Map&>(o));
            touch();
        }
        return *this;
    }
    TrackedMap& operator=(TrackedMap&& o) noexcept {
        if (this != &o) {
            Map::operator=(static_cast<Map&&>(o));
            touch();
            o.touch();
        }
        return *this;
    }
    TrackedMap& operator=(Map&& m) {
        Map::operator=(std::move(m));
        touch();
        return *this;
    }
    TrackedMap& operator=(const Map& m) {
        Map::operator=(m);
        touch();
        return *this;
    }

    using Map::bucket_count;
    using Map::empty;
    using Map::get_allocator;
    using Map::size;
    [[nodiscard]] const Map& view() const noexcept { return *this; }
    operator const Map&() const noexcept { return *this; }
    [[nodiscard]] const_iterator find(const key_type& k) const { return Map::find(k); }
    [[nodiscard]] size_type count(const key_type& k) const { return Map::count(k); }
    [[nodiscard]] bool contains(const key_type& k) const { return Map::contains(k); }
    [[nodiscard]] const mapped_type& at(const key_type& k) const { return Map::at(k); }
    [[nodiscard]] const_iterator begin() const noexcept { return Map::begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return Map::end(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return Map::cbegin(); }
    [[nodiscard]] const_iterator cend() const noexcept { return Map::cend(); }

    [[nodiscard]] iterator find(const key_type& k) {
        touch();
        return Map::find(k);
    }
    [[nodiscard]] iterator begin() noexcept {
        touch();
        return Map::begin();
    }
    [[nodiscard]] iterator end() noexcept {
        touch();
        return Map::end();
    }
    mapped_type& operator[](const key_type& k) {
        touch();
        return Map::operator[](k);
    }
    template <class... A> auto insert(A&&... a) {
        touch();
        return Map::insert(std::forward<A>(a)...);
    }
    template <class... A> auto emplace(A&&... a) {
        touch();
        return Map::emplace(std::forward<A>(a)...);
    }
    template <class... A> auto try_emplace(A&&... a) {
        touch();
        return Map::try_emplace(std::forward<A>(a)...);
    }
    template <class... A> auto insert_or_assign(A&&... a) {
        touch();
        return Map::insert_or_assign(std::forward<A>(a)...);
    }
    template <class A> auto erase(A&& a) {
        touch();
        return Map::erase(std::forward<A>(a));
    }
    void clear() noexcept {
        touch();
        Map::clear();
    }
    void reserve(size_type n) { Map::reserve(n); }
    void rehash(size_type n) { Map::rehash(n); }
    [[nodiscard]] Map& mut() noexcept {
        touch();
        return *this;
    }

    [[nodiscard]] bool clean() const noexcept {
        return !std::atomic_ref<std::uint8_t>(const_cast<std::uint8_t&>(changed_))
                    .load(std::memory_order_relaxed);
    }
    void mark_all_dirty() noexcept { touch(); }
    [[nodiscard]] image_type capture(const image_type* base, std::size_t* fresh_out = nullptr,
                                     std::size_t* total_out = nullptr) {
        const bool was = std::atomic_ref<std::uint8_t>(changed_).exchange(0, std::memory_order_relaxed);
        if (total_out)
            *total_out += 1;
        if (base && *base && (!was || **base == static_cast<const Map&>(*this)))
            return *base;
        if (fresh_out)
            *fresh_out += 1;
        return std::make_shared<const Map>(static_cast<const Map&>(*this));
    }
    [[nodiscard]] bool equals_image(const image_type& img) const {
        return img ? *img == static_cast<const Map&>(*this) : Map::empty();
    }
    void restore_from(const image_type& img, const image_type* base) {
        const bool was = std::atomic_ref<std::uint8_t>(changed_).exchange(0, std::memory_order_relaxed);
        if (base && *base && !was && *base == img)
            return;
        if (img)
            Map::operator=(*img);
        else
            Map::clear();
    }

private:
    void touch() noexcept {
        std::atomic_ref<std::uint8_t>(changed_).store(1, std::memory_order_relaxed);
    }
    std::uint8_t changed_ = 1;
};

} // namespace aura::ast::cow
