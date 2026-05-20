# 0 "/home/dev/code/aura/src/compiler/cache.ixx"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/cache.ixx"
export module aura.compiler.cache;

import std;
import aura.core;
import aura.compiler.ir;

using namespace aura::ast;

namespace aura::compiler::cache {

export struct MappedCache;



export struct CacheHeader {
    std::uint64_t node_offset;
    std::uint64_t string_offset;
    std::uint64_t ir_offset;
    std::uint64_t source_mtime;
    std::uint64_t content_hash;
    char magic[8];
    std::uint32_t version;
    std::uint32_t num_nodes;
    std::uint32_t num_strings;
    std::uint32_t num_functions;
};
static_assert(sizeof(CacheHeader) == 64, "CacheHeader must be 64 bytes");


export class MappedCache {
public:
    MappedCache() = default;
    MappedCache(MappedCache&&) noexcept;
    MappedCache& operator=(MappedCache&&) noexcept;
    ~MappedCache();

    bool valid() const { return data_ != nullptr; }


    NodeView get(NodeId id) const;
    NodeId root() const { return root_; }
    std::size_t size() const { return num_nodes_; }


    std::string_view resolve(SymId id) const;

private:
    friend MappedCache open_cache(const std::string& path);
    friend void setup_pointers(MappedCache& cache);

    void copy_pointers(const MappedCache& other);

    void* data_ = nullptr;
    std::size_t file_size_ = 0;


    const CacheHeader* header_ = nullptr;


    const std::uint8_t* tags_ = nullptr;
    const std::int64_t* int_vals_ = nullptr;
    const SymId* sym_ids_ = nullptr;
    const std::uint32_t* child_begins_ = nullptr;
    const std::uint32_t* child_counts_ = nullptr;
    const NodeId* child_data_ = nullptr;
    const std::uint32_t* param_begins_ = nullptr;
    const std::uint32_t* param_counts_ = nullptr;
    const SymId* param_data_ = nullptr;
    const NodeId* type_ids_ = nullptr;
    const std::uint32_t* lines_ = nullptr;
    const std::uint32_t* cols_ = nullptr;
    const std::uint8_t* markers_ = nullptr;



    const std::uint32_t* str_offsets_ = nullptr;
    const std::uint8_t* str_data_base_ = nullptr;


    std::vector<aura::ir::IRFunction> ir_functions_;
    std::uint32_t ir_entry_function_id_ = 0;
    std::vector<std::string> ir_string_pool_;
    bool has_ir_cache_ = false;

    NodeId root_ = NULL_NODE;
    std::size_t num_nodes_ = 0;

public:

    bool has_ir() const { return has_ir_cache_; }
    const std::vector<aura::ir::IRFunction>& ir_functions() const { return ir_functions_; }
    std::uint32_t ir_entry() const { return ir_entry_function_id_; }
    const std::vector<std::string>& ir_strings() const { return ir_string_pool_; }
};




export bool write_cache(const std::string& path,
                        const FlatAST& flat,
                        const StringPool& pool,
                        NodeId root,
                        std::uint64_t source_mtime = 0,
                        const aura::ir::IRModule* ir_mod = nullptr);


export MappedCache open_cache(const std::string& path);


export bool remove_cache(const std::string& path);

}
