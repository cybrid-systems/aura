export module aura.compiler.cache;

import std;
import aura.core;
import aura.compiler.ir;

using namespace aura::ast;

namespace aura::compiler::cache {

export struct MappedCache; // forward decl for friend
// setup_pointers defined in cache_impl.cpp (helper, internal linkage)

// ── File header (64 bytes) ────────────────────────────────────
export struct CacheHeader {
    std::uint64_t node_offset;   // 8
    std::uint64_t string_offset; // 8
    std::uint64_t ir_offset;     // 8
    std::uint64_t source_mtime;  // 8
    std::uint64_t content_hash;  // 8
    char magic[8];               // 8
    std::uint32_t version;       // 4
    std::uint32_t num_nodes;     // 4
    std::uint32_t num_strings;   // 4
    std::uint32_t num_functions; // 4
    std::uint32_t sig_offset;    // 4
    std::uint32_t sig_size;      // 4
};
static_assert(sizeof(CacheHeader) == 72, "CacheHeader must be 72 bytes");

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

    // StringPool access (O(1) via offset array)
    std::string_view resolve(SymId id) const;

    // Issue #73 Phase 2: per-node TypeId accessor. 0 means DYNAMIC /
    // unknown (or no type_ids column in this cache file). Cheap,
    // bounds-checked.
    std::uint32_t type_id(NodeId id) const {
        if (id >= num_nodes_ || !type_ids_) return 0;
        return type_ids_[id];
    }

private:
    friend MappedCache open_cache(const std::string& path);
    friend void setup_pointers(MappedCache& cache);

    void copy_pointers(const MappedCache& other);

    void* data_ = nullptr; // mmap base
    std::size_t file_size_ = 0;

    // Pointers into mmap region (set by open_cache)
    const CacheHeader* header_ = nullptr;

    // Column pointers (from node_offset)
    const std::uint8_t* tags_ = nullptr;
    const std::int64_t* int_vals_ = nullptr;
    const SymId* sym_ids_ = nullptr;
    const std::uint32_t* child_begins_ = nullptr;
    const std::uint32_t* child_counts_ = nullptr;
    const NodeId* child_data_ = nullptr;
    const std::uint32_t* param_begins_ = nullptr;
    const std::uint32_t* param_counts_ = nullptr;
    const SymId* param_data_ = nullptr;
    // Issue #73 Phase 2: TypeId per node, populated alongside the
    // FlatAST type_id SoA column. nullptr means "no type_ids column
    // in this cache file" (old cache or too-small mmap).
    const std::uint32_t* type_ids_ = nullptr;
    const std::uint32_t* lines_ = nullptr;
    const std::uint32_t* cols_ = nullptr;
    const std::uint8_t* markers_ = nullptr;

    // String pool pointers (from string_offset)
    // Layout: [num_strings:u32, offsets[num_strings]:u32, [len:u32,data:char[]]...]
    const std::uint32_t* str_offsets_ = nullptr;
    const std::uint8_t* str_data_base_ = nullptr;

    // IR module cache (loaded from ir_offset)
    std::vector<aura::ir::IRFunction> ir_functions_;
    std::uint32_t ir_entry_function_id_ = 0;
    std::vector<std::string> ir_string_pool_;
    bool has_ir_cache_ = false;

    NodeId root_ = NULL_NODE;
    std::size_t num_nodes_ = 0;

public:
    // IR module access
    bool has_ir() const { return has_ir_cache_; }
    const std::vector<aura::ir::IRFunction>& ir_functions() const { return ir_functions_; }
    std::uint32_t ir_entry() const { return ir_entry_function_id_; }
    const std::vector<std::string>& ir_strings() const { return ir_string_pool_; }
    // Signature data access (embedded .aura-type)
    bool has_sig() const {
        return header_ && header_->sig_offset > 0 && header_->sig_size > 0;
    }
    std::string_view sig_data() const {
        if (!has_sig() || !data_) return {};
        auto off = static_cast<std::size_t>(header_->sig_offset);
        // 跳过 4 字节的 size 前缀
        return {static_cast<const char*>(data_) + off + 4, header_->sig_size};
    }
};

// ── Public API ────────────────────────────────────────────────

// Write FlatAST + StringPool to a cache file.
export bool write_cache(const std::string& path, const FlatAST& flat, const StringPool& pool,
                        NodeId root, std::uint64_t source_mtime = 0,
                        const aura::ir::IRModule* ir_mod = nullptr,
                        const std::string* sig_data = nullptr);

// Open a cache file via mmap (zero-copy read).
export MappedCache open_cache(const std::string& path);

// Delete a cache file.
export bool remove_cache(const std::string& path);

} // namespace aura::compiler::cache
