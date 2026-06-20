module;
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "../reflect/cache_format.h"
module aura.compiler.cache;

// C-linkage bridge to reflection-based IR serialization
// (implemented in ir_reflect_serialize.cpp, compiled with -freflection)
extern "C" {
void aura_ir_serialize(const void* mod, const char** out_data, size_t* out_size);
void aura_ir_deserialize(const char* data, size_t size, void* out_mod);
}

namespace aura::compiler::cache {

using namespace aura::ast;

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

    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;

    auto n = flat.size();
    if (n == 0)
        return false;

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
    header.version = 4;
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
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(hdr_buf), hdr_size);

    // ── Write columns ────────────────────────────────────────
    f.seekp(tags_off);
    f.write((const char*)tags.data(), n);
    f.seekp(ints_off);
    f.write((const char*)int_vals.data(), n * 8);
    f.seekp(syms_off);
    f.write((const char*)sym_ids.data(), n * 4);
    f.seekp(ccpn_off);
    f.write((const char*)child_count_per_node.data(), n * 4);
    f.seekp(cd_off);
    f.write((const char*)child_data.data(), child_data.size() * 4);
    f.seekp(pb_off);
    f.write((const char*)param_begins.data(), n * 4);
    f.seekp(pc_off);
    f.write((const char*)param_counts.data(), n * 4);
    f.seekp(pd_off);
    f.write((const char*)param_data.data(), param_data.size() * 4);
    f.seekp(li_off);
    f.write((const char*)lines.data(), n * 4);
    f.seekp(co_off);
    f.write((const char*)cols.data(), n * 4);
    // Issue #73 Phase 2: write type_ids column.
    f.seekp(ti_off);
    f.write((const char*)type_ids.data(), n * 4);

    // ── Write string table (v3: num_strings + offsets + data) ──
    f.seekp(str_off);
    std::uint32_t num_strs = static_cast<std::uint32_t>(stbl.strings.size());
    f.write(reinterpret_cast<const char*>(&num_strs), 4);
    f.write(reinterpret_cast<const char*>(str_offsets.data()), offsets_size);
    for (auto& s : stbl.strings) {
        std::uint32_t len = static_cast<std::uint32_t>(s.size());
        f.write(reinterpret_cast<const char*>(&len), 4);
        f.write(s.data(), len);
    }

    // ── IR data section — auto-serialized via P2996 reflection ─────────
    auto ir_start = f.tellp();

    if (ir_mod) {
        const char* ir_buf_data = nullptr;
        size_t ir_buf_size = 0;
        aura_ir_serialize(ir_mod, &ir_buf_data, &ir_buf_size);
        auto ir_size = static_cast<std::uint32_t>(ir_buf_size);
        f.write(reinterpret_cast<const char*>(&ir_size), sizeof(ir_size));
        f.write(ir_buf_data, ir_buf_size);
        delete[] ir_buf_data;
    }

    // ── Type sig section (embedded .aura-type) ────────────────
    header.sig_offset = 0;
    header.sig_size = 0;
    if (sig_data && !sig_data->empty()) {
        auto sig_start = f.tellp();
        auto sig_len = static_cast<std::uint32_t>(sig_data->size());
        f.write(reinterpret_cast<const char*>(&sig_len), sizeof(sig_len));
        f.write(sig_data->data(), sig_data->size());
        header.sig_offset = static_cast<std::uint64_t>(sig_start);
        header.sig_size = sig_len;
    }

    // ── Rewrite header with IR + sig offsets ───────────────────
    header.ir_offset = static_cast<std::uint64_t>(ir_start);
    header.num_functions = ir_mod ? 1 : 0; // signal: 1 = has IR section, 0 = no IR
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(&header), sizeof(header));

    f.close();
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

void setup_pointers(MappedCache& cache) {
    auto n = cache.num_nodes_;
    auto* nd = static_cast<const std::uint8_t*>(cache.data_) + cache.header_->node_offset;
    std::size_t pos = 0;

    auto next_pad = [&](std::size_t sz) -> std::size_t {
        auto r = pos;
        pos += (sz + 7) & ~7ull;
        return r;
    };

    cache.tags_ = reinterpret_cast<const std::uint8_t*>(nd + next_pad(n * 1));
    cache.int_vals_ = reinterpret_cast<const std::int64_t*>(nd + next_pad(n * 8));
    cache.sym_ids_ = reinterpret_cast<const SymId*>(nd + next_pad(n * 4));
    // Issue #220: per-node child counts (1 column) replace the
    // legacy child_begins_ + child_counts_ (2 columns). The
    // per-node starting offset is computed in cum_begins_ below
    // (O(1) lookup at get() time).
    cache.child_count_per_node_ = reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));

    std::uint32_t total_children = 0;
    cache.cum_begins_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        cache.cum_begins_[i] = total_children;
        total_children += cache.child_count_per_node_[i];
    }
    cache.child_data_ = reinterpret_cast<const NodeId*>(nd + next_pad(total_children * 4));

    cache.param_begins_ = reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
    cache.param_counts_ = reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));

    std::uint32_t total_params = 0;
    for (std::size_t i = 0; i < n; ++i)
        total_params += cache.param_counts_[i];
    cache.param_data_ = reinterpret_cast<const SymId*>(nd + next_pad(total_params * 4));

    cache.lines_ = reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
    cache.cols_ = reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
    // Issue #73 Phase 2: read type_ids column. If old cache file is
    // missing the column (size < 4*n), type_ids_ stays nullptr and
    // flat.type_id() returns 0 — graceful forward-compat.
    std::uint64_t consumed = pos;
    if (consumed + n * 4 <=
        static_cast<std::uint64_t>(cache.file_size_ - cache.header_->node_offset)) {
        cache.type_ids_ = reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
    } else {
        // Skip past the expected slot without consuming — leaves pos
        // for any future column additions.
        next_pad(n * 4);
    }
}

MappedCache::MappedCache(MappedCache&& other) noexcept
    : data_(other.data_)
    , file_size_(other.file_size_)
    , header_(other.header_)
    , num_nodes_(other.num_nodes_) {
    copy_pointers(other);
    other.data_ = nullptr;
    other.file_size_ = 0;
    other.header_ = nullptr;
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
        other.data_ = nullptr;
        other.file_size_ = 0;
        other.header_ = nullptr;
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
    lines_ = o.lines_;
    cols_ = o.cols_;
    markers_ = o.markers_;
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
    if (!valid() || !str_offsets_ || id >= header_->num_strings)
        return "";
    auto off = str_offsets_[id];
    auto* p = str_data_base_ + off;
    std::uint32_t len = 0;
    std::memcpy(&len, p, 4);
    return std::string_view(reinterpret_cast<const char*>(p + 4), len);
}

MappedCache open_cache(const std::string& path) {
    MappedCache cache;

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return cache;

    struct stat st;
    if (::fstat(fd, &st) < 0) {
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
    if (cache_validate_header(hdr) != 0) {
        ::munmap(data, cache.file_size_);
        cache.data_ = nullptr;
        cache.file_size_ = 0;
        return cache;
    }

    cache.header_ = hdr;
    cache.num_nodes_ = hdr->num_nodes;
    cache.root_ = static_cast<NodeId>(hdr->content_hash);
    setup_pointers(cache);

    // ── Wire up string table (v3: offsets array for O(1) resolve) ──
    // Layout: [num_strings:u32, offsets[N]:u32, [len:u32,data:char[]]...]
    auto* sp = static_cast<const std::uint8_t*>(data) + hdr->string_offset;
    cache.str_offsets_ = reinterpret_cast<const std::uint32_t*>(sp + 4); // skip num_strings
    cache.str_data_base_ = sp; // base for offset resolution

    // ── Load IR cache (if present) — auto-deserialized via P2996 reflection ─
    cache.ir_functions_.clear();
    cache.ir_string_pool_.clear();
    cache.ir_entry_function_id_ = 0;
    cache.has_ir_cache_ = false;

    // Version 4+ uses reflection-based IR format (size-prefixed blob)
    // Version 3 used a hand-written format — safe to skip (rebuilt on miss)
    if (hdr->ir_offset > 0 && hdr->num_functions > 0 && hdr->version >= 4) {
        auto* ir_data = static_cast<const char*>(data) + hdr->ir_offset;

        // Read size prefix
        std::uint32_t ir_size = 0;
        std::memcpy(&ir_size, ir_data, 4);
        ir_data += 4;

        // Deserialize via reflection
        aura::ir::IRModule ir_mod;
        aura_ir_deserialize(ir_data, ir_size, &ir_mod);

        // Populate MappedCache fields
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
