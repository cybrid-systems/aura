export module aura.compiler.cache;

import std;
import aura.core;
import aura.core.ast_flat;
import aura.core.ast_pool;

using namespace aura::ast;

namespace aura::compiler::cache {

export struct MappedCache;  // forward decl for friend
// setup_pointers defined in cache_impl.cpp (helper, internal linkage)

// ── File header (64 bytes) ────────────────────────────────────
export struct CacheHeader {
    std::uint64_t node_offset;       // 8
    std::uint64_t string_offset;     // 8
    std::uint64_t ir_offset;         // 8
    std::uint64_t source_mtime;      // 8
    std::uint64_t content_hash;      // 8
    char     magic[8];               // 8
    std::uint32_t version;           // 4
    std::uint32_t num_nodes;         // 4
    std::uint32_t num_strings;       // 4
    std::uint32_t num_functions;     // 4
};
static_assert(sizeof(CacheHeader) == 64, "CacheHeader must be 64 bytes");

// ── Mapped cache — mmap'd read-only FlatAST + StringPool ─────
export class MappedCache {
public:
    MappedCache() = default;
    MappedCache(MappedCache&&) noexcept;
    MappedCache& operator=(MappedCache&&) noexcept;
    ~MappedCache();

    bool valid() const { return data_ != nullptr; }

    // FlatAST node access (compatible with NodeView)
    NodeView get(NodeId id) const;
    NodeId root() const { return root_; }
    std::size_t size() const { return num_nodes_; }

    // StringPool access
    std::string_view resolve(SymId id) const;
    std::string_view resolve_slow(SymId id) const;

private:
    friend MappedCache open_cache(const std::string& path);
    friend void setup_pointers(MappedCache& cache);

    void copy_pointers(const MappedCache& other);

    void*  data_ = nullptr;       // mmap base
    std::size_t file_size_ = 0;

    // Pointers into mmap region (set by open_cache)
    const CacheHeader* header_ = nullptr;

    // Column pointers (from node_offset)
    const std::uint8_t*   tags_ = nullptr;
    const std::int64_t*   int_vals_ = nullptr;
    const SymId* sym_ids_ = nullptr;
    const std::uint32_t*  child_begins_ = nullptr;
    const std::uint32_t*  child_counts_ = nullptr;
    const NodeId* child_data_ = nullptr;
    const std::uint32_t*  param_begins_ = nullptr;
    const std::uint32_t*  param_counts_ = nullptr;
    const SymId* param_data_ = nullptr;
    const NodeId* type_ids_ = nullptr;
    const std::uint32_t*  lines_ = nullptr;
    const std::uint32_t*  cols_ = nullptr;

    // String pool pointers (from string_offset)
    const std::uint32_t* str_offsets_ = nullptr;
    const std::uint32_t* str_lengths_ = nullptr;
    const std::uint8_t*  str_data_raw_ = nullptr;

    NodeId root_ = NULL_NODE;
    std::size_t num_nodes_ = 0;
};

// ── Public API ────────────────────────────────────────────────

// Write FlatAST + StringPool to a cache file.
export bool write_cache(const std::string& path,
                        const FlatAST& flat,
                        const StringPool& pool,
                        NodeId root,
                        std::uint64_t source_mtime = 0);

// Open a cache file via mmap (zero-copy read).
export MappedCache open_cache(const std::string& path);

// Delete a cache file.
export bool remove_cache(const std::string& path);

} // namespace aura::compiler::cache
