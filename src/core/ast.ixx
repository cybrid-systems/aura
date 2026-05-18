export module aura.core.ast;
import std;
import aura.core.type;

namespace aura::ast {

// ── Common type aliases ──────────────────────────────────────
export using NodeId = std::uint32_t;
export constexpr NodeId NULL_NODE = ~0u;
export using SymId = std::uint32_t;
export constexpr SymId INVALID_SYM = ~0u;

// ── Source location ──────────────────────────────────────────
export struct SourceLocation { std::uint32_t line = 0, column = 0, file = 0; };
// ── Syntax marker (Trees-That-Grow / hygienic macros) ──────
export enum class SyntaxMarker : std::uint8_t {
    User = 0,
    MacroIntroduced = 1,
    BoolLiteral = 2,  // #t / #f
};


// ── Phase tag (Trees-That-Grow) ──────────────────────────────
export struct ParsedPhase { static constexpr std::uint32_t id = 0; };

// ── Node tags ────────────────────────────────────────────────
export enum class NodeTag : std::uint32_t {
    LiteralInt = 0x01, Variable = 0x02, Call = 0x03,
    IfExpr = 0x04, Lambda = 0x05, Let = 0x06, LetRec = 0x07, Define = 0x08,
    Begin = 0x09, Set = 0x0A, Quote = 0x0B, LiteralString = 0x0D, MacroDef = 0x0E,
    TypeAnnotation = 0x0F,
    Coercion = 0x10,
    LiteralFloat = 0x11,
    Pair = 0x12,
    Export = 0x15,
};



// ── StringPool — arena-backed string interner ──────────────────
//
// Interns strings into a contiguous character buffer and returns
// SymId (std::uint32_t offset into the buffer). The hash table uses
// open addressing with linear probing for cache-friendliness.
//
// All storage is arena-allocated via pmr vector. reset() reclaims
// everything.
//
export class StringPool {
public:
    explicit StringPool(std::pmr::polymorphic_allocator<std::byte> alloc = {})
        : buf_(alloc), hash_tbl_(alloc)
    {
        // Reserve 0 as invalid sentinel
        buf_.push_back('\0');
        // Initialize hash table (power of 2, start small)
        rehash(64);
    }

    // Intern a string — returns a stable SymId
    SymId intern(std::string_view s) {
        if (s.empty()) return 0;

        auto hash = hash_str(s);
        auto mask = hash_capacity_ - 1;
        auto idx = hash & mask;

        while (hash_tbl_[idx] != INVALID_SYM) {
            auto existing = hash_tbl_[idx];
            auto view = resolve(existing);
            if (view == s) return existing;  // already interned
            idx = (idx + 1) & mask;           // linear probe
        }

        // Not found — intern
        auto offset = static_cast<SymId>(buf_.size());
        buf_.insert(buf_.end(), s.begin(), s.end());
        buf_.push_back('\0');  // null terminator

        hash_tbl_[idx] = offset;

        // Grow if load factor > 0.5
        ++entry_count_;
        if (entry_count_ * 2 > hash_capacity_)
            rehash(hash_capacity_ * 2);

        return offset;
    }

    // Resolve a SymId back to string_view
    std::string_view resolve(SymId id) const {
        if (id >= buf_.size()) return {};
        return std::string_view(buf_.data() + id);
    }

    // Total bytes used for string data
    std::size_t data_size() const { return buf_.size(); }

    // Reset all state
    void reset() {
        buf_.clear();
        buf_.push_back('\0');
        hash_tbl_.clear();
        hash_capacity_ = 0;
        entry_count_ = 0;
        rehash(64);
    }

private:
    static std::uint64_t hash_str(std::string_view s) {
        // FNV-1a
        std::uint64_t h = 0xCBF29CE484222325ull;
        for (auto c : s) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 0x100000001B3ull;
        }
        return h;
    }

    void rehash(std::uint32_t new_cap) {
        auto old_tbl = std::move(hash_tbl_);
        auto old_cap = hash_capacity_;

        hash_capacity_ = new_cap;
        hash_tbl_.assign(new_cap, INVALID_SYM);
        entry_count_ = 0;

        for (std::uint32_t i = 0; i < old_cap; ++i) {
            if (old_tbl[i] != INVALID_SYM) {
                auto hash = hash_str(resolve(old_tbl[i]));
                auto mask = hash_capacity_ - 1;
                auto idx = hash & mask;
                while (hash_tbl_[idx] != INVALID_SYM)
                    idx = (idx + 1) & mask;
                hash_tbl_[idx] = old_tbl[i];
                ++entry_count_;
            }
        }
    }

    // Backing store: contiguous null-terminated strings
    std::pmr::vector<char> buf_;

    // Open-addressing hash table (SymId → offset into buf_)
    std::pmr::vector<SymId> hash_tbl_;
    std::uint32_t hash_capacity_ = 0;
    std::uint32_t entry_count_ = 0;
};

// ── Node metadata (constexpr table for reflection/validation) ──
export struct NodeMeta {
    NodeTag tag;
    std::string_view name;
    std::uint8_t fixed_children;       // how many children this tag has
    bool has_var_children;        // variable-length children (Call args)
    bool has_string;              // has a name/id string (Variable/Let/Define)
    bool has_int;                 // has an int64 value (LiteralInt)
    bool has_float;               // has a double value (LiteralFloat)
    bool has_params;              // has param list (Lambda)
};

// Tag-to-metadata mapping, indexed by `tag - 1`.
// Tags must be sequential starting from 1 (LiteralInt = 0x01).
// Gap at 0x0C is filled with a sentinel.
export constexpr std::array<NodeMeta, 21> kNodeMeta = {{
    {NodeTag::LiteralInt, "LiteralInt", 0, false, false, true,  false, false},  // 0x01
    {NodeTag::Variable,   "Variable",   0, false, true,  false, false, false},  // 0x02
    {NodeTag::Call,       "Call",       1, true,  false, false, false, false},  // 0x03
    {NodeTag::IfExpr,     "IfExpr",     3, false, false, false, false, false},  // 0x04
    {NodeTag::Lambda,     "Lambda",     1, false, false, false, false, true},   // 0x05
    {NodeTag::Let,        "Let",        2, false, true,  false, false, false},  // 0x06
    {NodeTag::LetRec,     "LetRec",     2, false, true,  false, false, false},  // 0x07
    {NodeTag::Define,     "Define",     1, false, true,  false, false, false},  // 0x08
    {NodeTag::Begin,      "Begin",      0, true,  false, false, false, false},  // 0x09
    {NodeTag::Set,        "Set",        1, false, true,  false, false, false},  // 0x0A
    {NodeTag::Quote,      "Quote",      1, false, false, false, false, false},  // 0x0B
    {NodeTag::LiteralInt, "<gap>",      0, false, false, false, false, false},  // 0x0C (gap)
    {NodeTag::LiteralString, "LiteralString", 0, false, true,  false, false, false}, // 0x0D
    {NodeTag::LiteralInt, "<gap>",      0, false, false, false, false, false},  // 0x0E (MacroDef — Expr* only)
    {NodeTag::TypeAnnotation, "TypeAnnotation", 1, false, true,  false, false, false}, // 0x0F
    {NodeTag::Coercion, "Coercion", 1, false, true,  false, false, false},  // 0x10
    {NodeTag::LiteralFloat, "LiteralFloat", 0, false, false, false, true, false},  // 0x11
    {NodeTag::Pair,      "Pair",      2, false, false, false, false, false},  // 0x12
    {NodeTag::LiteralInt, "<gap>",      0, false, false, false, false, false},  // 0x13 (gap)
    {NodeTag::LiteralInt, "<gap>",      0, false, false, false, false, false},  // 0x14 (gap)
    {NodeTag::Export, "Export", 0, true, false, false, false, false},  // 0x15
}};


export constexpr const NodeMeta& meta(NodeTag tag) {
    return kNodeMeta[static_cast<std::size_t>(tag) - 1];
}

// Compile-time validation: check known entries, skip gap sentinels
consteval bool validate_node_meta() {
    // Gap entries (0x0C, 0x0E in tag space → indices 11, 13)
    if (kNodeMeta[11].name != "<gap>") return false;
    if (kNodeMeta[13].name != "<gap>") return false;
    if (meta(NodeTag::LiteralInt).name != "LiteralInt") return false;
    if (meta(NodeTag::Call).fixed_children != 1) return false;
    if (meta(NodeTag::Call).has_var_children != true) return false;
    if (meta(NodeTag::Lambda).has_params != true) return false;
    if (meta(NodeTag::IfExpr).fixed_children != 3) return false;
    if (meta(NodeTag::Let).has_string != true) return false;
    if (meta(NodeTag::LiteralInt).has_int != true) return false;
    if (meta(NodeTag::Begin).has_var_children != true) return false;
    if (meta(NodeTag::Coercion).name != "Coercion") return false;
    if (meta(NodeTag::LiteralFloat).name != "LiteralFloat") return false;
    if (meta(NodeTag::LiteralFloat).has_float != true) return false;
    return true;
}
static_assert(validate_node_meta(), "kNodeMeta misaligned with NodeTag enum");

// ── NodeView — lightweight non-owning read view ────────────────
export struct NodeView {
    NodeTag tag = NodeTag::LiteralInt;
    std::int64_t int_value = 0;
    double float_value = 0.0;
    SymId sym_id = INVALID_SYM;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::span<const NodeId> children;
    std::span<const SymId> params;
    SyntaxMarker marker = SyntaxMarker::User;

    bool has_int()   const { return tag == NodeTag::LiteralInt; }
    bool has_float() const { return tag == NodeTag::LiteralFloat; }
    bool has_name()  const { return sym_id != INVALID_SYM; }
    NodeId child(std::uint32_t i) const { return children[i]; }
};

// ── MutationRecord — typed mutation audit log ─────────────────
export enum class MutationStatus : std::uint8_t {
    Committed,
    RolledBack,
};

export struct MutationRecord {
    std::uint64_t mutation_id;
    std::uint64_t timestamp_ms;
    NodeId target_node;
    std::string operator_name;   // "replace-type", "replace-value", ...
    std::string old_type_str;    // format_type(old_type) at mutation time
    std::string new_type_str;    // format_type(new_type) at mutation time
    std::string summary;         // human-readable change description
    MutationStatus status;
    // Rollback data: for apply_patches-style rollback
    std::uint32_t field_offset;  // which SoA column was modified
    std::uint64_t old_value;     // original value before mutation
    std::uint64_t new_value;     // value after mutation
    bool has_rollback_data;      // true if rollback is available
};

// ── Patch — AI mutation descriptor ─────────────────────────────
export struct Patch {
    NodeId node = NULL_NODE;
    std::uint32_t field_offset = 0;
    std::uint64_t new_value = 0;
};

// ── FlatAST — SoA flat index-based AST ─────────────────────────
export class FlatAST {
private:
    [[nodiscard]] NodeId add_node(NodeTag tag, SyntaxMarker m = SyntaxMarker::User) {
        auto id = static_cast<NodeId>(tag_.size());
        tag_.push_back(tag);
        int_val_.push_back(0);
        float_val_.push_back(0.0);
        sym_id_.push_back(INVALID_SYM);
        child_begin_.push_back(0);
        child_count_.push_back(0);
        param_begin_.push_back(0);
        param_count_.push_back(0);
        line_.push_back(0);
        col_.push_back(0);
        marker_.push_back(m);
        type_id_.push_back(0);
        dirty_.push_back(0);
        node_first_mutation_.push_back(0);
        return id;
    }

    // SoA storage (all pmr::vector = arena allocated)
    std::pmr::vector<NodeTag>   tag_;
    std::pmr::vector<std::int64_t> int_val_;
    std::pmr::vector<double>    float_val_;
    std::pmr::vector<SymId>     sym_id_;
    std::pmr::vector<std::uint32_t>  child_begin_;
    std::pmr::vector<std::uint32_t>  child_count_;
    std::pmr::vector<NodeId>    child_data_;
    std::pmr::vector<std::uint32_t>  param_begin_;
    std::pmr::vector<std::uint32_t>  param_count_;
    std::pmr::vector<SymId>     param_data_;
    // Source location (line/col, 1-based)
    std::pmr::vector<std::uint32_t> line_;
    std::pmr::vector<std::uint32_t> col_;
    // Type information (L6.5+): type_id per node, 0 = DYNAMIC
    std::pmr::vector<SyntaxMarker> marker_;
    std::pmr::vector<std::uint8_t> dirty_;
    std::pmr::vector<std::uint32_t> type_id_;
    // Mutation audit log (heap-allocated, small+append-only)
    std::vector<MutationRecord> mutation_log_;
    std::vector<std::uint32_t> node_first_mutation_;
    std::uint64_t next_mutation_id_ = 1;

public:
    explicit FlatAST(std::pmr::polymorphic_allocator<std::byte> alloc = {})
        : tag_(alloc), int_val_(alloc), float_val_(alloc), sym_id_(alloc),
          child_begin_(alloc), child_count_(alloc), child_data_(alloc),
          param_begin_(alloc), param_count_(alloc), param_data_(alloc),
          line_(alloc), col_(alloc), type_id_(alloc)
    {}

    // ── Builders ───────────────────────────────────────────────

    [[nodiscard]] NodeId add_literal_float(double val) {
        auto id = add_node(NodeTag::LiteralFloat);
        float_val_[id] = val;
        return id;
    }

    [[nodiscard]] NodeId add_literalstring(SymId name) {
        auto id = add_node(NodeTag::LiteralString);
        sym_id_[id] = name;
        child_count_[id] = 0;
        return id;
    }

    [[nodiscard]] NodeId add_literal(std::int64_t val) {
        auto id = add_node(NodeTag::LiteralInt);
        int_val_[id] = val;
        return id;
    }

    [[nodiscard]] NodeId add_variable(SymId name) {
        auto id = add_node(NodeTag::Variable);
        sym_id_[id] = name;
        return id;
    }

    [[nodiscard]] NodeId add_call(NodeId func, std::span<const NodeId> args) {
        auto id = add_node(NodeTag::Call);
        child_data_.push_back(func);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.insert(child_data_.end(), args.begin(), args.end());
        child_begin_[id] = start - 1; // includes func
        child_count_[id] = 1 + static_cast<std::uint32_t>(args.size());
        return id;
    }

    [[nodiscard]] NodeId add_if(NodeId cond, NodeId then_b, NodeId else_b) {
        auto id = add_node(NodeTag::IfExpr);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(cond);
        child_data_.push_back(then_b);
        child_data_.push_back(else_b);
        child_begin_[id] = start;
        child_count_[id] = 3;
        return id;
    }

    [[nodiscard]] NodeId add_lambda(std::span<const SymId> params, NodeId body, bool dotted = false) {
        auto id = add_node(NodeTag::Lambda);
        int_val_[id] = dotted ? 1 : 0;  // store dotted flag
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), params.begin(), params.end());
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(params.size());
        child_data_.push_back(body);
        child_begin_[id] = static_cast<std::uint32_t>(child_data_.size() - 1);
        child_count_[id] = 1;
        return id;
    }

    [[nodiscard]] NodeId add_let(SymId name, NodeId val, NodeId body) {
        auto id = add_node(NodeTag::Let);
        sym_id_[id] = name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_data_.push_back(body);
        child_begin_[id] = start;
        child_count_[id] = 2;
        return id;
    }

    [[nodiscard]] NodeId add_letrec(SymId name, NodeId val, NodeId body) {
        auto id = add_node(NodeTag::LetRec);
        sym_id_[id] = name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_data_.push_back(body);
        child_begin_[id] = start;
        child_count_[id] = 2;
        return id;
    }

    [[nodiscard]] NodeId add_define(SymId name, NodeId val) {
        auto id = add_node(NodeTag::Define);
        sym_id_[id] = name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_begin_[id] = start;
        child_count_[id] = 1;
        return id;
    }


    [[nodiscard]] NodeId add_begin(NodeId* exprs, std::uint32_t count) {
        auto id = add_node(NodeTag::Begin);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        for (std::uint32_t i = 0; i < count; ++i) child_data_.push_back(exprs[i]);
        child_begin_[id] = start;
        child_count_[id] = count;
        return id;
    }
    [[nodiscard]] NodeId add_begin(std::span<const NodeId> exprs) {
        auto id = add_node(NodeTag::Begin);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.insert(child_data_.end(), exprs.begin(), exprs.end());
        child_begin_[id] = start;
        child_count_[id] = static_cast<std::uint32_t>(exprs.size());
        return id;
    }

    // Export: (export sym1 sym2 ...) — children = Variable nodes
    [[nodiscard]] NodeId add_export(std::span<const NodeId> syms) {
        auto id = add_node(NodeTag::Export);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.insert(child_data_.end(), syms.begin(), syms.end());
        child_begin_[id] = start;
        child_count_[id] = static_cast<std::uint32_t>(syms.size());
        return id;
    }

    [[nodiscard]] NodeId add_set(SymId name, NodeId val) {
        auto id = add_node(NodeTag::Set);
        sym_id_[id] = name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_begin_[id] = start;
        child_count_[id] = 1;
        return id;
    }

    [[nodiscard]] NodeId add_macrodef(SymId name, const std::vector<SymId>& params, NodeId body, bool dotted = false) {
        auto id = add_node(NodeTag::MacroDef);
        sym_id_[id] = name;
        int_val_[id] = dotted ? 1 : 0;  // store dotted flag in unused int_val_
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(body);
        child_begin_[id] = start;
        child_count_[id] = 1;
        // Store params using the same SoA as Lambda params
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), params.begin(), params.end());
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(params.size());
        return id;
    }

    [[nodiscard]] NodeId add_quote(NodeId val) {
        auto id = add_node(NodeTag::Quote);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_begin_[id] = start;
        child_count_[id] = 1;
        return id;
    }

    [[nodiscard]] NodeId add_pair(NodeId car, NodeId cdr) {
        auto id = add_node(NodeTag::Pair);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(car);
        child_data_.push_back(cdr);
        child_begin_[id] = start;
        child_count_[id] = 2;
        return id;
    }

    [[nodiscard]] NodeId add_type_annotation(SymId type_name, NodeId inner) {
        auto id = add_node(NodeTag::TypeAnnotation);
        sym_id_[id] = type_name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        return id;
    }

    [[nodiscard]] NodeId add_coercion(NodeId inner, std::uint32_t type_id) {
        auto id = add_node(NodeTag::Coercion);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        type_id_[id] = type_id;
        return id;
    }
    // ── Access ─────────────────────────────────────────────────

    NodeView get(NodeId id) const {
        return NodeView{
            .tag      = tag_[id],
            .int_value = int_val_[id],
            .float_value = float_val_[id],
            .sym_id   = sym_id_[id],
            .line     = id < line_.size() ? line_[id] : 0,
            .col      = id < col_.size() ? col_[id] : 0,
            .children = std::span(child_data_.data() + child_begin_[id],
                                  child_count_[id]),
            .params   = std::span(param_data_.data() + param_begin_[id],
                                  param_count_[id]),
            .marker   = id < marker_.size() ? marker_[id] : SyntaxMarker::User,
        };
    }

    // ── Location ──────────────────────────────────────────────
    void set_loc(NodeId id, std::uint32_t line, std::uint32_t col) {
        line_[id] = line;
        col_[id] = col;
    }
    std::uint32_t line(NodeId id) const { return line_[id]; }
    std::uint32_t col(NodeId id) const { return col_[id]; }

    // Direct field access (for mutation)
    NodeTag& tag(NodeId id) { return tag_[id]; }
    std::int64_t& int_val(NodeId id) { return int_val_[id]; }
    SymId& sym_id(NodeId id) { return sym_id_[id]; }

    // ── Child field access ─────────────────────────────────────

    std::span<NodeId> children(NodeId id) {
        return std::span(child_data_.data() + child_begin_[id],
                         child_count_[id]);
    }

    void set_child(NodeId id, std::uint32_t idx, NodeId child) {
        child_data_[child_begin_[id] + idx] = child;
    }

    // Insert a child at position idx (0 = first, child_count = append)
    // Shifts all subsequent children and updates child_begin_ for later nodes.
    void insert_child(NodeId id, std::uint32_t idx, NodeId child) {
        auto pos = child_begin_[id] + std::min(idx, child_count_[id]);
        child_data_.insert(child_data_.begin() + pos, 1, child);
        // Shift child_begin for all nodes after this one (insert grew child_data_)
        for (auto i = id + 1; i < tag_.size(); ++i) {
            child_begin_[i]++;
        }
        child_count_[id]++;
    }

    // Remove a child at position idx by replacing with NULL_NODE
    void remove_child(NodeId id, std::uint32_t idx) {
        if (idx < child_count_[id])
            child_data_[child_begin_[id] + idx] = NULL_NODE;
    }

    // ── Bulk ───────────────────────────────────────────────────

    void clear() {
        tag_.clear(); int_val_.clear(); sym_id_.clear();
        child_begin_.clear(); child_count_.clear(); child_data_.clear();
        param_begin_.clear(); param_count_.clear(); param_data_.clear();
        type_id_.clear();
        root = NULL_NODE;
    }

    std::size_t size() const { return tag_.size(); }
    bool empty() const { return tag_.empty(); }

    // ── Marker access ─────────────────────────────────────────

    void set_marker(NodeId id, SyntaxMarker m) {
        if (id < marker_.size()) marker_[id] = m;
    }
    SyntaxMarker marker(NodeId id) const {
        return id < marker_.size() ? marker_[id] : SyntaxMarker::User;
    }

    // ── Dirty tracking (incremental compilation) ───────────────

    void mark_dirty(NodeId id) {
        if (id >= dirty_.size()) dirty_.resize(id + 1, false);
        dirty_[id] = true;
    }
    void mark_subtree_dirty(NodeId id) {
        mark_dirty(id);
        auto v = get(id);
        for (auto c : v.children) {
            if (c != NULL_NODE) mark_subtree_dirty(c);
        }
    }
    bool is_dirty(NodeId id) const {
        return id < dirty_.size() && dirty_[id];
    }
    // Propagate dirty upward: mark this node AND all ancestors dirty
    void mark_dirty_upward(NodeId id) {
        mark_dirty(id);
        // Scan for parent (linear, but mutate is infrequent)
        for (NodeId pid = 0; pid < tag_.size(); ++pid) {
            auto v = get(pid);
            for (auto c : v.children) {
                if (c == id) {
                    mark_dirty_upward(pid);
                    return;
                }
            }
        }
    }

    // Check if any node in a subtree (including the root) is dirty
    bool has_dirty_subtree(NodeId id) const {
        if (is_dirty(id)) return true;
        auto v = get(id);
        for (auto c : v.children) {
            if (c != NULL_NODE && has_dirty_subtree(c)) return true;
        }
        return false;
    }

    void clear_all_dirty() {
        std::fill(dirty_.begin(), dirty_.end(), false);
    }

    // ── Mutation audit ──────────────────────────────────────────

    // Record a mutation on a node. Returns the mutation_id.
    std::uint64_t add_mutation(NodeId node, std::string_view op_name,
                                std::string_view old_type, std::string_view new_type,
                                std::string_view summary,
                                MutationStatus status = MutationStatus::Committed) {
        return add_mutation_with_rollback(node, op_name, old_type, new_type,
                                           summary, status, 0, 0, 0, false);
    }

    // Record a mutation with rollback data (field_offset + old/new_value)
    std::uint64_t add_mutation_with_rollback(NodeId node, std::string_view op_name,
                                              std::string_view old_type, std::string_view new_type,
                                              std::string_view summary,
                                              MutationStatus status,
                                              std::uint32_t field_offset,
                                              std::uint64_t old_value,
                                              std::uint64_t new_value,
                                              bool has_rollback) {
        std::uint64_t mid = next_mutation_id_++;
        auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count());
        mutation_log_.push_back({mid, now, node, std::string(op_name),
                                  std::string(old_type), std::string(new_type),
                                  std::string(summary), status,
                                  field_offset, old_value, new_value, has_rollback});
        // Auto-mark node AND ancestors dirty on mutation
        mark_dirty_upward(node);

        // Update node_first_mutation_ index
        if (node < node_first_mutation_.size() && node_first_mutation_[node] == 0) {
            node_first_mutation_[node] = static_cast<std::uint32_t>(mutation_log_.size());
        }
        // If node index doesn't exist yet, we'll set it on next get
        return mid;
    }

    // Get mutation history for a specific node (0 == no history)
    // Get mutation history for a specific node (filters from log, O(n) in log size)
    std::vector<MutationRecord> mutation_history(NodeId node) const {
        std::vector<MutationRecord> result;
        for (auto& rec : mutation_log_) {
            if (rec.target_node == node)
                result.push_back(rec);
        }
        return result;
    }

    // Total number of mutations recorded
    std::size_t mutation_count() const { return mutation_log_.size(); }

    // Get all mutation records (unfiltered).
    const std::vector<MutationRecord>& all_mutations() const { return mutation_log_; }
    std::vector<MutationRecord>& all_mutations() { return mutation_log_; }

    // Rollback a mutation by ID. Returns true if successful.
    bool rollback(std::uint64_t mutation_id) {
        for (auto& rec : mutation_log_) {
            if (rec.mutation_id == mutation_id) {
                if (rec.status != MutationStatus::Committed) return false;
                if (!rec.has_rollback_data) return false;
                // Apply old value back to the SoA column
                if (rec.target_node < tag_.size()) {
                    switch (rec.field_offset) {
                    case 0: // int_val_
                        if (rec.target_node < int_val_.size()) {
                            int_val_[rec.target_node] = static_cast<std::int64_t>(rec.old_value);
                            rec.status = MutationStatus::RolledBack;
                            return true;
                        }
                        break;
                    case 1: // type_id_
                        if (rec.target_node < type_id_.size()) {
                            type_id_[rec.target_node] = static_cast<std::uint32_t>(rec.old_value);
                            rec.status = MutationStatus::RolledBack;
                            return true;
                        }
                        break;
                    default: return false;
                    }
                }
                return false;
            }
        }
        return false;
    }

    // Rollback all mutations since (and including) the given ID.
    std::size_t rollback_since(std::uint64_t since_id) {
        std::size_t count = 0;
        for (auto it = mutation_log_.rbegin(); it != mutation_log_.rend(); ++it) {
            if (it->mutation_id >= since_id && it->status == MutationStatus::Committed) {
                if (rollback(it->mutation_id)) ++count;
            }
        }
        return count;
    }

    // ── Type ID access ─────────────────────────────────────────

    std::uint32_t type_id(NodeId id) const {
        return id < type_id_.size() ? type_id_[id] : 0;
    }

    void set_type(NodeId id, std::uint32_t tid) {
        if (id < type_id_.size()) type_id_[id] = tid;
    }

    void set_int(NodeId id, std::int64_t val) {
        if (id < int_val_.size()) int_val_[id] = val;
    }
    void set_float(NodeId id, double val) {
        if (id < float_val_.size()) float_val_[id] = val;
    }
    void set_sym(NodeId id, SymId val) {
        if (id < sym_id_.size()) sym_id_[id] = val;
    }

    // Resolve type names → TypeIds for all TypeAnnotation nodes
    // Requires TypeRegistry for name resolution
    void resolve_type_ids(class aura::core::TypeRegistry& reg, StringPool& pool);

    NodeId root = NULL_NODE;

};

// ── Patch application ──────────────────────────────────────────
export bool apply_patches(FlatAST& ast, std::span<const Patch> patches);

// ── Delta fixup (for deserialization) ──────────────────────────
export void fixup_deltas(FlatAST& ast);

// ── Bridge from pointer tree to FlatAST ────────────────────────


} // namespace aura::ast
