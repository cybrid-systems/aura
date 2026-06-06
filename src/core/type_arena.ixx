// ═══════════════════════════════════════════════════════════════════
// TypeEntryArena — stable-address chunk allocator for TypeRegistry
// ═══════════════════════════════════════════════════════════════════
//
// Solves the heap-use-after-free in the previous std::vector<Entry>
// storage of TypeRegistry::entries_: push_back could reallocate,
// invalidating every pointer obtained via *_of(). With chunk-based
// bump allocation, the Entry* returned to clients is stable for the
// lifetime of the chunk — only invalidated by explicit reset() (which
// TypeRegistry::compact() pairs with a generation bump so callers
// detect staleness via TypeId::generation).
//
// Style: aligned with aura::ast::ASTArena (src/core/arena.ixx) —
// monotonic chunks that grow geometrically, placement-new for
// construction, bulk reset() for release. Each chunk owns its
// std::byte[] buffer via unique_ptr so destruction is exception-safe.
//
// Why not std::deque<Entry>?
//   std::deque would solve the stability problem in one line, but
//   the project philosophy (escape-analysis-arena.md) prefers explicit
//   chunk arenas for hot-path data: better cache locality within a
//   chunk, predictable allocation cost, and the same interface
//   template as ASTArena so future pooling / alignment tuning can be
//   shared.
//
module;
#include <contracts>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

export module aura.core.type_arena;

namespace aura::core {

// ── TypeEntryArena ─────────────────────────────────────────────
export class TypeEntryArena {
public:
    // Chunk sizing. Initial chunk is small (1 KB worth of slot space)
    // so the first few entries don't waste capacity; growth doubles
    // up to a cap of 256 KB so very large registries don't allocate
    // huge chunks they may never fill.
    static constexpr std::size_t kInitialChunkBytes = 1024;   // 1 KB
    static constexpr std::size_t kMaxChunkBytes     = 256 * 1024;  // 256 KB

    // Default initial chunk size; can be overridden by the caller if
    // they have a known workload (e.g. tests that pre-fill the arena).
    explicit TypeEntryArena(std::size_t initial_chunk_bytes = kInitialChunkBytes) noexcept
        : next_chunk_bytes_(initial_chunk_bytes) {
        if (next_chunk_bytes_ < kMinChunkBytes) {
            next_chunk_bytes_ = kMinChunkBytes;
        }
    }

    // Non-copyable, non-movable: a moved arena would invalidate every
    // outstanding pointer (the whole point of this class is stability).
    TypeEntryArena(const TypeEntryArena&) = delete;
    TypeEntryArena& operator=(const TypeEntryArena&) = delete;
    TypeEntryArena(TypeEntryArena&&) = delete;
    TypeEntryArena& operator=(TypeEntryArena&&) = delete;

    // Allocate storage and construct a T by copying `init`.
    // The returned pointer is stable across subsequent allocations
    // and is invalidated only by reset() (or arena destruction).
    template <typename T>
    [[nodiscard]] T* allocate(const T& init)
        post (r: r != nullptr)
    {
        // Round slot size up to T's alignment so the next bump_slot
        // call also lands on a properly aligned address.
        constexpr std::size_t slot = align_up(sizeof(T), alignof(T));
        void* raw = bump_slot(slot);
        return std::construct_at(static_cast<T*>(raw), init);
    }

    // In-place construction (avoids an extra copy when the caller
    // already has all the field values).
    template <typename T, typename... Args>
    [[nodiscard]] T* construct(Args&&... args)
        post (r: r != nullptr)
    {
        constexpr std::size_t slot = align_up(sizeof(T), alignof(T));
        void* raw = bump_slot(slot);
        return std::construct_at(static_cast<T*>(raw),
                                 std::forward<Args>(args)...);
    }

    // Explicitly destroy a single object (rarely needed — reset()
    // bulk-frees). Caller is responsible for not using the pointer
    // afterwards.
    template <typename T>
    void destroy(T* ptr) noexcept {
        if (ptr) std::destroy_at(ptr);
    }

    // Release all allocated storage in one shot.
    // After reset, no previously-returned pointer is valid.
    void reset()
        post (bytes_used() == 0)
        post (chunk_count() == 0)
    {
        chunks_.clear();
        bytes_used_ = 0;
        next_chunk_bytes_ = kInitialChunkBytes;
    }

    // Total bytes currently in use across all chunks.
    [[nodiscard]] std::size_t bytes_used() const noexcept { return bytes_used_; }

    // Number of chunks currently allocated.
    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }

    // Total capacity across all chunks (sum of chunk bytes).
    [[nodiscard]] std::size_t capacity() const noexcept {
        std::size_t total = 0;
        for (const auto& c : chunks_) total += c.bytes;
        return total;
    }

    // Wasted bytes from alignment padding (capacity - used). Useful
    // for tuning the chunk size in tests / benchmarks.
    [[nodiscard]] std::size_t wasted() const noexcept {
        return capacity() > bytes_used() ? capacity() - bytes_used() : 0;
    }

private:
    static constexpr std::size_t kMinChunkBytes = 64;  // absolute floor

    // Round `n` up to the next multiple of `a`. `a` must be a power
    // of two (we use this only with alignof(T) values, which always
    // are).
    static constexpr std::size_t align_up(std::size_t n, std::size_t a) noexcept {
        return (n + a - 1) & ~(a - 1);
    }

    // Bump a slot of `n` bytes out of the current chunk (or allocate
    // a new one if the current chunk can't fit).
    void* bump_slot(std::size_t n) {
        if (chunks_.empty() || chunks_.back().used + n > chunks_.back().bytes) {
            // Grow the next chunk size (capped). Use at least `n`
            // bytes so the slot always fits even on a tiny first chunk.
            std::size_t next = next_chunk_bytes_ * 2;
            if (next > kMaxChunkBytes) next = kMaxChunkBytes;
            if (next < n) next = align_up(n, 16);
            chunks_.push_back({});
            chunks_.back().bytes = next;
            chunks_.back().data = std::make_unique<std::byte[]>(next);
            chunks_.back().used = 0;
            next_chunk_bytes_ = next;
        }
        void* p = chunks_.back().data.get() + chunks_.back().used;
        chunks_.back().used += n;
        bytes_used_ += n;
        return p;
    }

    struct Chunk {
        std::unique_ptr<std::byte[]> data;
        std::size_t bytes = 0;
        std::size_t used = 0;
    };

    std::vector<Chunk> chunks_;
    std::size_t next_chunk_bytes_;  // size hint for the next chunk
    std::size_t bytes_used_ = 0;
};

} // namespace aura::core
