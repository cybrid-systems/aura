module;

#include <cerrno>
#include <fcntl.h>
#include <stdio.h>  // rename
#include <stdlib.h> // mkstemp
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "../reflect/cache_format.h"

module aura.compiler.cache;
import std;
import aura.compiler.ir;

#include "reflect/ir_module_layout.hh"

// C-linkage bridge to reflection-based IR serialization
// (implemented in ir_reflect_serialize.cpp, compiled with -freflection)
extern "C" {
void aura_ir_serialize(const void* mod, const char** out_data, size_t* out_size);
void aura_ir_deserialize(const char* data, size_t size, void* out_mod);
// #4936: releases a buffer from aura_ir_serialize (same TU as the allocation).
void aura_ir_buffer_free(const char* data) noexcept;
}

namespace aura::compiler::cache {

using namespace aura::ast;

// Issue #5213: a cache directory is trusted only when it is a real
// directory (not a symlink), owned by this euid, and not group- or
// other-writable. Failure is a miss; never chmod a directory we did
// not just create.
static bool cache_dir_trusted_path(const std::string& dir_path) {
    if (dir_path.empty())
        return false;
    struct stat st;
    if (::lstat(dir_path.c_str(), &st) != 0)
        return false;
    if (!S_ISDIR(st.st_mode))
        return false;
    if (st.st_uid != static_cast<uid_t>(::geteuid()))
        return false;
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        return false;
    return true;
}

static bool cache_parent_trusted(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return cache_dir_trusted_path(".");
    if (slash == 0)
        return cache_dir_trusted_path("/");
    return cache_dir_trusted_path(path.substr(0, slash));
}

// ═══════════════════════════════════════════════════════════════
// Write — column-wise FlatAST dump + string table + optional IR
// ═══════════════════════════════════════════════════════════════

// Collect all unique SymIds referenced by nodes, and remap them.
struct StringTable {
    std::vector<std::string> strings; // index → string
    std::pmr::vector<SymId> remapped; // for each node, the new index
};

static StringTable build_string_table(const FlatAST& flat, const StringPool& pool) {
    StringTable tbl;
    std::pmr::vector<SymId> remapped(flat.size(), INVALID_SYM);
    std::unordered_map<SymId, SymId> sym_to_idx; // original SymId → table index

    // First pass: collect unique SymIds
    for (NodeId id = 0; id < flat.size(); ++id) {
        auto v = flat.get(id);
        SymId sid = v.sym_id;
        if (sid == INVALID_SYM)
            continue;

        auto it = sym_to_idx.find(sid);
        if (it != sym_to_idx.end()) {
            remapped[id] = it->second;
        } else {
            auto new_idx = static_cast<SymId>(tbl.strings.size());
            tbl.strings.push_back(std::string(pool.resolve(sid)));
            sym_to_idx[sid] = new_idx;
            remapped[id] = new_idx;
        }
    }
    tbl.remapped = std::move(remapped);
    return tbl;
}

bool write_cache(const std::string& path, const FlatAST& flat, const StringPool& pool, NodeId root,
                 std::uint64_t source_mtime, const aura::ir::IRModule* ir_mod,
                 const std::string* sig_data) {

    auto n = flat.size();
    if (n == 0)
        return false;

    // Issue #5159: serialize the IR module before creating the file. A fault
    // in the serializer must not leave a half-written cache at `path`.
    struct IrBufferFree {
        void operator()(const char* data) const noexcept { aura_ir_buffer_free(data); }
    };
    const char* ir_buf_data = nullptr;
    std::size_t ir_buf_size = 0;
    if (ir_mod)
        aura_ir_serialize(ir_mod, &ir_buf_data, &ir_buf_size);
    const std::unique_ptr<const char[], IrBufferFree> ir_buf{ir_buf_data};
    if (ir_buf_size > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        return false;

    // Refuse to create a cache in a shared or foreign directory (#5213).
    if (!cache_parent_trusted(path))
        return false;

    std::string tmp_name = path + ".XXXXXX";
    int fd = ::mkstemp(tmp_name.data());
    if (fd < 0)
        return false;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (::fchmod(fd, 0600) != 0) {
        ::close(fd);
        ::unlink(tmp_name.c_str());
        return false;
    }
    struct TmpFile {
        int fd = -1;
        std::string path;
        bool published = false;
        ~TmpFile() {
            if (fd >= 0)
                ::close(fd);
            if (!published && !path.empty())
                ::unlink(path.c_str());
        }
    } tmp{fd, std::move(tmp_name), false};

    // Build string table and remapped sym_ids
    auto stbl = build_string_table(flat, pool);

    // Build output columns
    std::vector<std::uint8_t> tags(n);
    std::vector<std::int64_t> int_vals(n, 0);
    std::vector<SymId> sym_ids(n, INVALID_SYM);
    // Issue #220: per-node child counts (1 column) replace the
    // legacy child_begins_ + child_counts_ (2 columns). The
    // per-node starting offset is reconstructed by setup_pointers
    // from the cumulative sum.
    std::vector<std::uint32_t> child_count_per_node(n, 0);
    std::vector<std::uint32_t> param_begins(n, 0);
    std::vector<std::uint32_t> param_counts(n, 0);
    std::vector<NodeId> child_data;
    std::vector<SymId> param_data;
    std::vector<std::uint32_t> lines(n, 0);
    std::vector<std::uint32_t> cols(n, 0);
    std::vector<std::uint32_t> type_ids(n, 0); // Issue #73 Phase 2

    for (NodeId id = 0; id < n; ++id) {
        auto v = flat.get(id);
        tags[id] = static_cast<std::uint8_t>(v.tag);
        int_vals[id] = v.int_value;
        sym_ids[id] = stbl.remapped[id]; // remapped index
        lines[id] = v.line;
        cols[id] = v.col;
        type_ids[id] = flat.type_id(id); // Issue #73: persist TypeId so it
                                         // survives cache load

        // Issue #220: per-node child count (the reader reconstructs
        // the per-node starting offset via cumulative sum).
        child_count_per_node[id] = static_cast<std::uint32_t>(v.children.size());
        child_data.insert(child_data.end(), v.children.begin(), v.children.end());

        param_begins[id] = static_cast<std::uint32_t>(param_data.size());
        param_counts[id] = static_cast<std::uint32_t>(v.params.size());
        param_data.insert(param_data.end(), v.params.begin(), v.params.end());
    }

    // ── Compute sizes and offsets ──────────────────────────────
    auto pad64 = [](std::uint64_t v) { return (v + 7) & ~7ull; };

    std::uint64_t off = sizeof(CacheHeader);
    std::uint64_t tags_off = off;
    off += pad64(n);
    std::uint64_t ints_off = off;
    off += pad64(n * 8);
    std::uint64_t syms_off = off;
    off += pad64(n * 4);
    std::uint64_t ccpn_off = off;
    off += pad64(n * 4);
    std::uint64_t cd_off = off;
    off += pad64(child_data.size() * 4);
    std::uint64_t pb_off = off;
    off += pad64(n * 4);
    std::uint64_t pc_off = off;
    off += pad64(n * 4);
    std::uint64_t pd_off = off;
    off += pad64(param_data.size() * 4);
    std::uint64_t li_off = off;
    off += pad64(n * 4);
    std::uint64_t co_off = off;
    off += pad64(n * 4);
    // Issue #73 Phase 2: type_ids column. 0 = unknown/DYNAMIC.
    std::uint64_t ti_off = off;
    off += pad64(n * 4);

    // ── String table layout (v3+ with offset array) ────────────
    // [num_strings:uint32]
    // [offsets[num_strings]:uint32]  — each offset is byte distance from string_offset
    // [len_0:uint32, data_0:char[len_0]]
    // [len_1:uint32, data_1:char[len_1]]
    // ...
    std::uint64_t str_off = off;
    auto offsets_size = static_cast<std::uint64_t>(stbl.strings.size()) * 4;
    std::uint64_t str_data_start = 4 + offsets_size; // skip num_strings + offset table
    std::vector<std::uint32_t> str_offsets;
    str_offsets.reserve(stbl.strings.size());
    {
        std::uint32_t cur = static_cast<std::uint32_t>(str_data_start);
        for (auto& s : stbl.strings) {
            str_offsets.push_back(cur);
            cur += 4 + static_cast<std::uint32_t>(s.size());
        }
    }

    // ── Write header via std::meta reflection (auto_serialize<CacheHeader>) ─
    CacheHeader header = {};
    std::memcpy(header.magic, "AURACACHE", 8);
    header.version = 5;
    header.num_nodes = static_cast<std::uint32_t>(n);
    header.num_strings = static_cast<std::uint32_t>(stbl.strings.size());
    header.content_hash = static_cast<std::uint64_t>(root);
    header.node_offset = sizeof(CacheHeader);
    header.string_offset = str_off;
    header.source_mtime = source_mtime;
    // Serialize via compile-time reflection
    unsigned char hdr_buf[128];
    size_t hdr_size = 0;
    cache_serialize_header(&header, hdr_buf, &hdr_size);
    // Issue #4936: one checked place converts sizes/offsets to the signed
    // stream types (no narrowing, no C-style casts). Writes go to the
    // mkstemp fd; cursor tracks the ofstream position put_bytes used.
    std::uint64_t cursor = 0;
    bool io_ok = true;
    const auto put_at = [&tmp, &cursor, &io_ok](std::uint64_t off, const void* p,
                                                std::size_t bytes) {
        if (!io_ok)
            return;
        if (bytes == 0) {
            cursor = off;
            return;
        }
        const auto* raw = static_cast<const char*>(p);
        std::size_t done = 0;
        while (done < bytes) {
            const auto w =
                ::pwrite(tmp.fd, raw + done, bytes - done, static_cast<off_t>(off + done));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                io_ok = false;
                return;
            }
            if (w == 0) {
                io_ok = false;
                return;
            }
            done += static_cast<std::size_t>(w);
        }
        cursor = off + static_cast<std::uint64_t>(bytes);
    };
    const auto put_bytes = [&put_at, &cursor](const void* p, std::size_t bytes) {
        put_at(cursor, p, bytes);
    };
    put_at(0, hdr_buf, hdr_size);

    // ── Write columns ────────────────────────────────────────
    put_at(tags_off, tags.data(), n);
    put_at(ints_off, int_vals.data(), n * 8);
    put_at(syms_off, sym_ids.data(), n * 4);
    put_at(ccpn_off, child_count_per_node.data(), n * 4);
    put_at(cd_off, child_data.data(), child_data.size() * 4);
    put_at(pb_off, param_begins.data(), n * 4);
    put_at(pc_off, param_counts.data(), n * 4);
    put_at(pd_off, param_data.data(), param_data.size() * 4);
    put_at(li_off, lines.data(), n * 4);
    put_at(co_off, cols.data(), n * 4);
    // Issue #73 Phase 2: write type_ids column.
    put_at(ti_off, type_ids.data(), n * 4);

    // ── Write string table (v3: num_strings + offsets + data) ──
    std::uint32_t num_strs = static_cast<std::uint32_t>(stbl.strings.size());
    put_at(str_off, &num_strs, 4);
    put_bytes(str_offsets.data(), offsets_size);
    for (auto& s : stbl.strings) {
        std::uint32_t len = static_cast<std::uint32_t>(s.size());
        put_bytes(&len, 4);
        put_bytes(s.data(), len);
    }

    // ── IR data section — auto-serialized via P2996 reflection ─────────
    const auto ir_start = cursor;

    if (ir_mod) {
        auto ir_size = static_cast<std::uint32_t>(ir_buf_size);
        put_bytes(&ir_size, sizeof(ir_size));
        if (ir_buf_size > 0)
            put_bytes(ir_buf.get(), ir_buf_size);
    }

    // ── Type sig section (embedded .aura-type) ────────────────
    header.sig_offset = 0;
    header.sig_size = 0;
    if (sig_data && !sig_data->empty()) {
        const auto sig_start = cursor;
        auto sig_len = static_cast<std::uint32_t>(sig_data->size());
        put_bytes(&sig_len, sizeof(sig_len));
        put_bytes(sig_data->data(), sig_data->size());
        header.sig_offset = static_cast<std::uint32_t>(sig_start);
        header.sig_size = sig_len;
    }

    // ── Rewrite header with IR + sig offsets ───────────────────
    header.ir_offset = ir_start;
    header.num_functions = ir_mod ? 1 : 0; // signal: 1 = has IR section, 0 = no IR
    put_at(0, &header, sizeof(header));
    if (!io_ok)
        return false;

    if (::fsync(tmp.fd) != 0)
        return false;
    if (::close(tmp.fd) != 0) {
        tmp.fd = -1;
        return false;
    }
    tmp.fd = -1;
    if (::rename(tmp.path.c_str(), path.c_str()) != 0)
        return false;
    tmp.published = true;
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Read — mmap-based zero-copy load
// ═══════════════════════════════════════════════════════════════

// Column layout within node data (offsets relative to node_offset):
//   [0]: tags        (n * 1 + pad7)
//   [1]: int_vals    (n * 8)
//   [2]: sym_ids     (n * 4)
//   [3]: child_beg   (n * 4)
//   [4]: child_cnt   (n * 4)
//   [5]: child_data  (total_children * 4)
//   [6]: param_beg   (n * 4)
//   [7]: param_cnt   (n * 4)
//   [8]: param_data  (total_params * 4)
//   [9]: lines       (n * 4)
//  [10]: cols        (n * 4)
//  [11]: type_ids    (n * 4)   // Issue #73 Phase 2: 0 = DYNAMIC/unknown

bool setup_pointers(MappedCache& cache) {
    auto clear = [&cache]() {
        cache.tags_ = nullptr;
        cache.int_vals_ = nullptr;
        cache.sym_ids_ = nullptr;
        cache.child_count_per_node_ = nullptr;
        cache.child_data_ = nullptr;
        cache.cum_begins_.clear();
        cache.param_begins_ = nullptr;
        cache.param_counts_ = nullptr;
        cache.param_data_ = nullptr;
        cache.type_ids_ = nullptr;
        cache.lines_ = nullptr;
        cache.cols_ = nullptr;
        cache.markers_ = nullptr;
        return false;
    };
    if (!cache.header_ || !cache.data_ || cache.header_->node_offset > cache.file_size_)
        return clear();

    const std::uint64_t n = cache.num_nodes_;
    const std::uint64_t node_off = cache.header_->node_offset;
    std::uint64_t limit = cache.file_size_;
    const auto str_off = cache.header_->string_offset;
    if (str_off > node_off && str_off <= cache.file_size_)
        limit = str_off;
    if (n > (UINT64_MAX / 8))
        return clear();

    const auto* base = static_cast<const std::uint8_t*>(cache.data_);
    std::uint64_t pos = node_off;
    // Place a column at `pos`. The payload must lie inside `limit`.
    // Padding advances `pos` the same way the writer does; a later column
    // that falls past `limit` fails the whole cache (#5213).
    const auto take = [&](std::uint64_t nbytes) -> const std::uint8_t* {
        if (nbytes > limit || pos > limit - nbytes)
            return nullptr;
        const auto* p = base + pos;
        if (nbytes > UINT64_MAX - 7)
            return nullptr;
        const auto padded = (nbytes + 7) & ~std::uint64_t{7};
        if (pos > UINT64_MAX - padded)
            return nullptr;
        pos += padded;
        return p;
    };

    const auto* tags = take(n);
    const auto* ints = take(n * 8);
    const auto* syms = take(n * 4);
    const auto* child_counts = take(n * 4);
    if (!tags || !ints || !syms || !child_counts)
        return clear();
    cache.tags_ = tags;
    cache.int_vals_ = reinterpret_cast<const std::int64_t*>(ints);
    cache.sym_ids_ = reinterpret_cast<const SymId*>(syms);
    cache.child_count_per_node_ = reinterpret_cast<const std::uint32_t*>(child_counts);

    std::uint64_t total_children = 0;
    cache.cum_begins_.resize(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i) {
        const auto c = cache.child_count_per_node_[i];
        if (total_children > UINT32_MAX || c > UINT32_MAX - total_children)
            return clear();
        cache.cum_begins_[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(total_children);
        total_children += c;
    }
    if (total_children > UINT64_MAX / 4)
        return clear();
    const auto* child_data = take(total_children * 4);
    const auto* param_begins = take(n * 4);
    const auto* param_counts = take(n * 4);
    if (!child_data || !param_begins || !param_counts)
        return clear();
    cache.child_data_ = reinterpret_cast<const NodeId*>(child_data);
    cache.param_begins_ = reinterpret_cast<const std::uint32_t*>(param_begins);
    cache.param_counts_ = reinterpret_cast<const std::uint32_t*>(param_counts);

    std::uint64_t total_params = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        const auto c = cache.param_counts_[i];
        if (total_params > UINT32_MAX || c > UINT32_MAX - total_params)
            return clear();
        total_params += c;
    }
    for (std::uint64_t i = 0; i < n; ++i) {
        const auto begin = cache.param_begins_[i];
        const auto count = cache.param_counts_[i];
        if (count > total_params || begin > total_params - count)
            return clear();
    }
    if (total_params > UINT64_MAX / 4)
        return clear();
    const auto* param_data = take(total_params * 4);
    const auto* lines = take(n * 4);
    const auto* cols = take(n * 4);
    const auto* type_ids = take(n * 4);
    if (!param_data || !lines || !cols || !type_ids)
        return clear();
    cache.param_data_ = reinterpret_cast<const SymId*>(param_data);
    cache.lines_ = reinterpret_cast<const std::uint32_t*>(lines);
    cache.cols_ = reinterpret_cast<const std::uint32_t*>(cols);
    cache.type_ids_ = reinterpret_cast<const std::uint32_t*>(type_ids);
    return true;
}

MappedCache::MappedCache(MappedCache&& other) noexcept
    : data_(other.data_)
    , file_size_(other.file_size_)
    , header_(other.header_)
    , ir_functions_(std::move(other.ir_functions_))
    , ir_entry_function_id_(other.ir_entry_function_id_)
    , ir_string_pool_(std::move(other.ir_string_pool_))
    , has_ir_cache_(other.has_ir_cache_)
    , root_(other.root_)
    , num_nodes_(other.num_nodes_) {
    copy_pointers(other);
    other.data_ = nullptr;
    other.file_size_ = 0;
    other.header_ = nullptr;
    other.has_ir_cache_ = false;
    other.ir_entry_function_id_ = 0;
    other.root_ = NULL_NODE;
    other.num_nodes_ = 0;
}

MappedCache& MappedCache::operator=(MappedCache&& other) noexcept {
    if (this != &other) {
        if (valid())
            munmap(data_, file_size_);
        data_ = other.data_;
        file_size_ = other.file_size_;
        header_ = other.header_;
        num_nodes_ = other.num_nodes_;
        copy_pointers(other);
        ir_functions_ = std::move(other.ir_functions_);
        ir_entry_function_id_ = other.ir_entry_function_id_;
        ir_string_pool_ = std::move(other.ir_string_pool_);
        has_ir_cache_ = other.has_ir_cache_;
        root_ = other.root_;
        other.data_ = nullptr;
        other.file_size_ = 0;
        other.header_ = nullptr;
        other.has_ir_cache_ = false;
        other.ir_entry_function_id_ = 0;
        other.root_ = NULL_NODE;
        other.num_nodes_ = 0;
    }
    return *this;
}

void MappedCache::copy_pointers(const MappedCache& o) {
    tags_ = o.tags_;
    int_vals_ = o.int_vals_;
    sym_ids_ = o.sym_ids_;
    // Issue #220: per-node children (child_count_per_node_ +
    // child_data_ from mmap; cum_begins_ computed in memory).
    child_count_per_node_ = o.child_count_per_node_;
    child_data_ = o.child_data_;
    cum_begins_ = o.cum_begins_;
    param_begins_ = o.param_begins_;
    param_counts_ = o.param_counts_;
    param_data_ = o.param_data_;
    str_offsets_ = o.str_offsets_;
    str_data_base_ = o.str_data_base_;
    string_section_end_ = o.string_section_end_;
    lines_ = o.lines_;
    cols_ = o.cols_;
    markers_ = o.markers_;
    type_ids_ = o.type_ids_;
}

MappedCache::~MappedCache() {
    if (valid())
        munmap(data_, file_size_);
}

NodeView MappedCache::get(NodeId id) const {
    if (!valid() || id >= num_nodes_)
        return {};
    return NodeView{
        .tag = static_cast<NodeTag>(tags_[id]),
        .int_value = int_vals_[id],
        .sym_id = sym_ids_[id],
        .line = id < num_nodes_ ? lines_[id] : 0,
        .col = id < num_nodes_ ? cols_[id] : 0,
        // Issue #73 Phase 2: surface TypeId on the NodeView so callers
        // can read it directly. Old cache files (no type_ids_ column)
        // get 0 here, which matches flat.type_id()'s DYNAMIC default.
        .type_id = type_ids_ ? type_ids_[id] : 0u,
        .children = std::span(child_data_ + cum_begins_[id], child_count_per_node_[id]),
        .params = std::span(param_data_ + param_begins_[id], param_counts_[id]),
        .marker = markers_ ? static_cast<SyntaxMarker>(markers_[id]) : SyntaxMarker::User,
    };
}

std::string_view MappedCache::resolve(SymId id) const {
    if (!valid() || !str_offsets_ || !str_data_base_ || !header_ || id >= header_->num_strings)
        return "";
    if (string_section_end_ < header_->string_offset)
        return "";
    const auto section = string_section_end_ - header_->string_offset;
    const auto off = static_cast<std::uint64_t>(str_offsets_[id]);
    if (off > section || section - off < 4)
        return "";
    std::uint32_t len = 0;
    std::memcpy(&len, str_data_base_ + off, 4);
    if (static_cast<std::uint64_t>(len) > section - off - 4)
        return "";
    return std::string_view(reinterpret_cast<const char*>(str_data_base_ + off + 4), len);
}

MappedCache open_cache(const std::string& path) {
    MappedCache cache;
    if (path.empty() || !cache_parent_trusted(path))
        return cache;

    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return cache;

    struct stat st;
    if (::fstat(fd, &st) < 0) {
        ::close(fd);
        return cache;
    }
    const bool regular = S_ISREG(st.st_mode);
    const bool owned = st.st_uid == static_cast<uid_t>(::geteuid());
    const bool priv = (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
    const bool sized = st.st_size >= static_cast<off_t>(sizeof(CacheHeader));
    if (!regular || !owned || !priv || !sized) {
        ::close(fd);
        return cache;
    }
    cache.file_size_ = static_cast<std::size_t>(st.st_size);

    void* data = ::mmap(nullptr, cache.file_size_, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (data == MAP_FAILED) {
        cache.file_size_ = 0;
        return cache;
    }

    cache.data_ = data;
    auto* hdr = static_cast<const CacheHeader*>(data);
    if (cache_validate_header(hdr, cache.file_size_) != 0) {
        ::munmap(data, cache.file_size_);
        cache.data_ = nullptr;
        cache.file_size_ = 0;
        return cache;
    }

    cache.header_ = hdr;
    cache.num_nodes_ = hdr->num_nodes;
    cache.root_ = static_cast<NodeId>(hdr->content_hash);
    if (!setup_pointers(cache)) {
        ::munmap(data, cache.file_size_);
        cache.data_ = nullptr;
        cache.file_size_ = 0;
        cache.header_ = nullptr;
        return MappedCache{};
    }

    // ── Wire up string table (v3: offsets array for O(1) resolve) ──
    // Layout: [num_strings:u32, offsets[N]:u32, [len:u32,data:char[]]...]
    std::uint64_t str_end = cache.file_size_;
    if (hdr->ir_offset > hdr->string_offset && hdr->ir_offset <= cache.file_size_)
        str_end = hdr->ir_offset;
    cache.string_section_end_ = str_end;
    if (hdr->num_strings > 0) {
        const auto str_off = static_cast<std::uint64_t>(hdr->string_offset);
        const auto table = 4 + static_cast<std::uint64_t>(hdr->num_strings) * 4;
        if (str_off > cache.file_size_ || table > cache.file_size_ - str_off) {
            ::munmap(data, cache.file_size_);
            cache.data_ = nullptr;
            cache.file_size_ = 0;
            cache.header_ = nullptr;
            return MappedCache{};
        }
        auto* sp = static_cast<const std::uint8_t*>(data) + str_off;
        cache.str_offsets_ = reinterpret_cast<const std::uint32_t*>(sp + 4);
        cache.str_data_base_ = sp;
    }

    // ── Load IR cache (if present) — auto-deserialized via P2996 reflection ─
    cache.ir_functions_.clear();
    cache.ir_string_pool_.clear();
    cache.ir_entry_function_id_ = 0;
    cache.has_ir_cache_ = false;

    // Version 4+ uses reflection-based IR format (size-prefixed blob)
    // Version 3 used a hand-written format — safe to skip (rebuilt on miss)
    // Version 5+ IR blobs include IRInstruction::source_marker (#455).
    // v4 blobs were serialized with a smaller mirror layout — skip them.
    // Issue #5159: a v5 header that claims an IR section must contain the
    // whole size-prefixed blob. A truncated section is not a valid cache
    // (the old skip path reported the AST as "version=3" with no IR).
    if (hdr->version >= 5 && hdr->num_functions > 0) {
        const auto off = static_cast<std::uint64_t>(hdr->ir_offset);
        const bool fits = off <= cache.file_size_ && cache.file_size_ - off >= 4;
        std::uint32_t ir_size = 0;
        if (fits)
            std::memcpy(&ir_size, static_cast<const char*>(data) + off, 4);
        if (!fits || cache.file_size_ - off - 4 < ir_size) {
            ::munmap(data, cache.file_size_);
            cache.data_ = nullptr;
            cache.file_size_ = 0;
            cache.header_ = nullptr;
            return MappedCache{};
        }
        auto* ir_data = static_cast<const char*>(data) + off + 4;
        aura::ir::IRModule ir_mod;
        aura_ir_deserialize(ir_data, ir_size, &ir_mod);
        // Structural gate before any cached IR is executable (#5213).
        if (!aura::ir::validate_ir_module(ir_mod)) {
            ::munmap(data, cache.file_size_);
            cache.data_ = nullptr;
            cache.file_size_ = 0;
            cache.header_ = nullptr;
            cache.has_ir_cache_ = false;
            return MappedCache{};
        }
        cache.ir_functions_ = std::move(ir_mod.functions);
        cache.ir_string_pool_ = std::move(ir_mod.string_pool);
        cache.ir_entry_function_id_ = ir_mod.entry_function_id;
        cache.has_ir_cache_ = true;
    }

    return cache;
}

bool remove_cache(const std::string& path) {
    return std::filesystem::remove(path);
}

} // namespace aura::compiler::cache
