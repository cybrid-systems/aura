module;
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
module aura.compiler.cache;

namespace aura::compiler::cache {

using namespace aura::ast;

// ═══════════════════════════════════════════════════════════════
// Write — column-wise FlatAST dump + string table
// ═══════════════════════════════════════════════════════════════

// Collect all unique SymIds referenced by nodes, and remap them.
struct StringTable {
    std::vector<std::string> strings;           // index → string
    std::pmr::vector<SymId>  remapped;          // for each node, the new index
};

static StringTable build_string_table(const FlatAST& flat, const StringPool& pool) {
    StringTable tbl;
    std::pmr::vector<SymId> remapped(flat.size(), INVALID_SYM);
    std::unordered_map<SymId, SymId> sym_to_idx;  // original SymId → table index

    // First pass: collect unique SymIds
    for (NodeId id = 0; id < flat.size(); ++id) {
        auto v = flat.get(id);
        SymId sid = v.sym_id;
        if (sid == INVALID_SYM) continue;

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

bool write_cache(const std::string& path,
                 const FlatAST& flat,
                 const StringPool& pool,
                 NodeId root,
                 std::uint64_t source_mtime) {

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    auto n = flat.size();
    if (n == 0) return false;

    // Build string table and remapped sym_ids
    auto stbl = build_string_table(flat, pool);

    // Build output columns
    std::vector<std::uint8_t>  tags(n);
    std::vector<std::int64_t>  int_vals(n, 0);
    std::vector<SymId>         sym_ids(n, INVALID_SYM);
    std::vector<std::uint32_t> child_begins(n, 0);
    std::vector<std::uint32_t> child_counts(n, 0);
    std::vector<std::uint32_t> param_begins(n, 0);
    std::vector<std::uint32_t> param_counts(n, 0);
    std::vector<NodeId>        child_data;
    std::vector<SymId>         param_data;
    std::vector<std::uint32_t> lines(n, 0);
    std::vector<std::uint32_t> cols(n, 0);

    for (NodeId id = 0; id < n; ++id) {
        auto v = flat.get(id);
        tags[id] = static_cast<std::uint8_t>(v.tag);
        int_vals[id] = v.int_value;
        sym_ids[id] = stbl.remapped[id];  // remapped index
        lines[id] = v.line;
        cols[id] = v.col;

        child_begins[id] = static_cast<std::uint32_t>(child_data.size());
        child_counts[id] = static_cast<std::uint32_t>(v.children.size());
        child_data.insert(child_data.end(), v.children.begin(), v.children.end());

        param_begins[id] = static_cast<std::uint32_t>(param_data.size());
        param_counts[id] = static_cast<std::uint32_t>(v.params.size());
        param_data.insert(param_data.end(), v.params.begin(), v.params.end());
    }

    // ── Compute sizes and offsets ──────────────────────────────
    auto pad64 = [](std::uint64_t v) { return (v + 7) & ~7ull; };

    std::uint64_t off = sizeof(CacheHeader);
    std::uint64_t tags_off       = off; off += pad64(n);
    std::uint64_t ints_off       = off; off += pad64(n * 8);
    std::uint64_t syms_off       = off; off += pad64(n * 4);
    std::uint64_t cb_off         = off; off += pad64(n * 4);
    std::uint64_t cc_off         = off; off += pad64(n * 4);
    std::uint64_t cd_off         = off; off += pad64(child_data.size() * 4);
    std::uint64_t pb_off         = off; off += pad64(n * 4);
    std::uint64_t pc_off         = off; off += pad64(n * 4);
    std::uint64_t pd_off         = off; off += pad64(param_data.size() * 4);
    std::uint64_t li_off         = off; off += pad64(n * 4);
    std::uint64_t co_off         = off; off += pad64(n * 4);

    // ── String table layout (v3+ with offset array) ────────────
    // [num_strings:uint32]
    // [offsets[num_strings]:uint32]  — each offset is byte distance from string_offset
    // [len_0:uint32, data_0:char[len_0]]
    // [len_1:uint32, data_1:char[len_1]]
    // ...
    std::uint64_t str_off = off;
    auto offsets_size = static_cast<std::uint64_t>(stbl.strings.size()) * 4;
    std::uint64_t str_data_start = 4 + offsets_size;  // skip num_strings + offset table
    std::vector<std::uint32_t> str_offsets;
    str_offsets.reserve(stbl.strings.size());
    {
        std::uint32_t cur = static_cast<std::uint32_t>(str_data_start);
        for (auto& s : stbl.strings) {
            str_offsets.push_back(cur);
            cur += 4 + static_cast<std::uint32_t>(s.size());
        }
    }

    // ── Write header ─────────────────────────────────────────
    CacheHeader header;
    std::memset(&header, 0, sizeof(header));
    std::memcpy(header.magic, "AURACACHE", 8);
    header.version = 3;
    header.num_nodes = static_cast<std::uint32_t>(n);
    header.num_strings = static_cast<std::uint32_t>(stbl.strings.size());
    // Store root NodeId in content_hash (abuse field for now; Phase 2 adds proper field)
    header.content_hash = static_cast<std::uint64_t>(root);
    header.num_functions = 0;
    header.node_offset = sizeof(CacheHeader);
    header.string_offset = str_off;
    header.ir_offset = 0;
    header.source_mtime = source_mtime;

    f.seekp(0);
    f.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // ── Write columns ────────────────────────────────────────
    f.seekp(tags_off); f.write((const char*)tags.data(), n);
    f.seekp(ints_off); f.write((const char*)int_vals.data(), n * 8);
    f.seekp(syms_off); f.write((const char*)sym_ids.data(), n * 4);
    f.seekp(cb_off);   f.write((const char*)child_begins.data(), n * 4);
    f.seekp(cc_off);   f.write((const char*)child_counts.data(), n * 4);
    f.seekp(cd_off);   f.write((const char*)child_data.data(), child_data.size() * 4);
    f.seekp(pb_off);   f.write((const char*)param_begins.data(), n * 4);
    f.seekp(pc_off);   f.write((const char*)param_counts.data(), n * 4);
    f.seekp(pd_off);   f.write((const char*)param_data.data(), param_data.size() * 4);
    f.seekp(li_off);   f.write((const char*)lines.data(), n * 4);
    f.seekp(co_off);   f.write((const char*)cols.data(), n * 4);

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

void setup_pointers(MappedCache& cache) {
    auto n = cache.num_nodes_;
    auto* nd = static_cast<const std::uint8_t*>(cache.data_) + cache.header_->node_offset;
    std::size_t pos = 0;

    auto next_pad = [&](std::size_t sz) -> std::size_t {
        auto r = pos; pos += (sz + 7) & ~7ull; return r;
    };

    cache.tags_        = reinterpret_cast<const std::uint8_t*>(nd + next_pad(n * 1));
    cache.int_vals_    = reinterpret_cast<const std::int64_t*>(nd + next_pad(n * 8));
    cache.sym_ids_     = reinterpret_cast<const SymId*>(nd + next_pad(n * 4));
    cache.child_begins_= reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
    cache.child_counts_= reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));

    std::uint32_t total_children = 0;
    for (std::size_t i = 0; i < n; ++i) total_children += cache.child_counts_[i];
    cache.child_data_  = reinterpret_cast<const NodeId*>(nd + next_pad(total_children * 4));

    cache.param_begins_= reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
    cache.param_counts_= reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));

    std::uint32_t total_params = 0;
    for (std::size_t i = 0; i < n; ++i) total_params += cache.param_counts_[i];
    cache.param_data_  = reinterpret_cast<const SymId*>(nd + next_pad(total_params * 4));

    cache.lines_       = reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
    cache.cols_        = reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
}

MappedCache::MappedCache(MappedCache&& other) noexcept
    : data_(other.data_), file_size_(other.file_size_), header_(other.header_),
      num_nodes_(other.num_nodes_) {
    copy_pointers(other);
    other.data_ = nullptr; other.file_size_ = 0; other.header_ = nullptr;
}

MappedCache& MappedCache::operator=(MappedCache&& other) noexcept {
    if (this != &other) {
        if (valid()) munmap(data_, file_size_);
        data_ = other.data_; file_size_ = other.file_size_; header_ = other.header_;
        num_nodes_ = other.num_nodes_;
        copy_pointers(other);
        other.data_ = nullptr; other.file_size_ = 0; other.header_ = nullptr;
    }
    return *this;
}

void MappedCache::copy_pointers(const MappedCache& o) {
    tags_ = o.tags_; int_vals_ = o.int_vals_; sym_ids_ = o.sym_ids_;
    child_begins_ = o.child_begins_; child_counts_ = o.child_counts_;
    child_data_ = o.child_data_;
    param_begins_ = o.param_begins_; param_counts_ = o.param_counts_;
    param_data_ = o.param_data_;
    str_offsets_ = o.str_offsets_;
    str_data_base_ = o.str_data_base_;
    lines_ = o.lines_; cols_ = o.cols_;
    markers_ = o.markers_;
}

MappedCache::~MappedCache() {
    if (valid()) munmap(data_, file_size_);
}

NodeView MappedCache::get(NodeId id) const {
    if (!valid() || id >= num_nodes_) return {};
    return NodeView{
        .tag      = static_cast<NodeTag>(tags_[id]),
        .int_value = int_vals_[id],
        .sym_id   = sym_ids_[id],
        .line     = id < num_nodes_ ? lines_[id] : 0,
        .col      = id < num_nodes_ ? cols_[id] : 0,
        .children = std::span(child_data_ + child_begins_[id], child_counts_[id]),
        .params   = std::span(param_data_ + param_begins_[id], param_counts_[id]),
        .marker   = markers_ ? static_cast<SyntaxMarker>(markers_[id]) : SyntaxMarker::User,
    };
}

std::string_view MappedCache::resolve(SymId id) const {
    if (!valid() || !str_offsets_ || id >= header_->num_strings) return "";
    auto off = str_offsets_[id];
    auto* p = str_data_base_ + off;
    std::uint32_t len = 0;
    std::memcpy(&len, p, 4);
    return std::string_view(reinterpret_cast<const char*>(p + 4), len);
}

MappedCache open_cache(const std::string& path) {
    MappedCache cache;

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return cache;

    struct stat st;
    if (::fstat(fd, &st) < 0) { ::close(fd); return cache; }
    cache.file_size_ = static_cast<std::size_t>(st.st_size);

    void* data = ::mmap(nullptr, cache.file_size_, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (data == MAP_FAILED) { cache.file_size_ = 0; return cache; }

    cache.data_ = data;
    auto* hdr = static_cast<const CacheHeader*>(data);
    if (std::memcmp(hdr->magic, "AURACACHE", 8) != 0 || hdr->version != 3) {
        ::munmap(data, cache.file_size_);
        cache.data_ = nullptr; cache.file_size_ = 0; return cache;
    }

    cache.header_ = hdr;
    cache.num_nodes_ = hdr->num_nodes;
    cache.root_ = static_cast<NodeId>(hdr->content_hash);
    setup_pointers(cache);

    // ── Wire up string table (v3: offsets array for O(1) resolve) ──
    // Layout: [num_strings:u32, offsets[N]:u32, [len:u32,data:char[]]...]
    auto* sp = static_cast<const std::uint8_t*>(data) + hdr->string_offset;
    cache.str_offsets_ = reinterpret_cast<const std::uint32_t*>(sp + 4);  // skip num_strings
    cache.str_data_base_ = sp;  // base for offset resolution

    return cache;
}

bool remove_cache(const std::string& path) {
    return std::filesystem::remove(path);
}

} // namespace aura::compiler::cache
