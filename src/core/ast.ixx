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
export struct SourceLocation {
    std::uint32_t line = 0, column = 0, file = 0;
};
// ── Syntax marker (Trees-That-Grow / hygienic macros) ──────
export enum class SyntaxMarker : std::uint8_t {
    User = 0,
    MacroIntroduced = 1,
    BoolLiteral = 2, // #t / #f
};


// ── Phase tag (Trees-That-Grow) ──────────────────────────────
export struct ParsedPhase {
    static constexpr std::uint32_t id = 0;
};

// ── Node tags ────────────────────────────────────────────────
export enum class NodeTag : std::uint32_t {
    LiteralInt = 0x01,
    Variable = 0x02,
    Call = 0x03,
    IfExpr = 0x04,
    Lambda = 0x05,
    Let = 0x06,
    LetRec = 0x07,
    Define = 0x08,
    Begin = 0x09,
    Set = 0x0A,
    Quote = 0x0B,
    LiteralString = 0x0D,
    MacroDef = 0x0E,
    TypeAnnotation = 0x0F,
    Coercion = 0x10,
    LiteralFloat = 0x11,
    Pair = 0x12,
    DefineType = 0x13,
    DefineModule = 0x14,
    Export = 0x15,
    Linear = 0x16,
    Move = 0x17,
    Borrow = 0x18,
    MutBorrow = 0x19,
    Drop = 0x1A,
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
        : buf_(alloc)
        , hash_tbl_(alloc) {
        // Reserve 0 as invalid sentinel
        buf_.push_back('\0');
        // Initialize hash table (power of 2, start small)
        rehash(64);
    }

    // Intern a string — returns a stable SymId
    SymId intern(std::string_view s) {
        if (s.empty())
            return 0;

        auto hash = hash_str(s);
        auto mask = hash_capacity_ - 1;
        auto idx = hash & mask;

        while (hash_tbl_[idx] != INVALID_SYM) {
            auto existing = hash_tbl_[idx];
            auto view = resolve(existing);
            if (view == s)
                return existing;    // already interned
            idx = (idx + 1) & mask; // linear probe
        }

        // Not found — intern
        auto offset = static_cast<SymId>(buf_.size());
        buf_.insert(buf_.end(), s.begin(), s.end());
        buf_.push_back('\0'); // null terminator

        hash_tbl_[idx] = offset;

        // Grow if load factor > 0.5
        ++entry_count_;
        if (entry_count_ * 2 > hash_capacity_)
            rehash(hash_capacity_ * 2);

        return offset;
    }

    // Resolve a SymId back to string_view
    std::string_view resolve(SymId id) const {
        if (id >= buf_.size())
            return {};
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
    std::uint8_t fixed_children; // how many children this tag has
    bool has_var_children;       // variable-length children (Call args)
    bool has_string;             // has a name/id string (Variable/Let/Define)
    bool has_int;                // has an int64 value (LiteralInt)
    bool has_float;              // has a double value (LiteralFloat)
    bool has_params;             // has param list (Lambda)
};

// Tag-to-metadata mapping, indexed by `tag - 1`.
// Tags must be sequential starting from 1 (LiteralInt = 0x01).
// Gap at 0x0C is filled with a sentinel.
export constexpr std::array<NodeMeta, 26> kNodeMeta = {{
    {NodeTag::LiteralInt, "LiteralInt", 0, false, false, true, false, false},       // 0x01
    {NodeTag::Variable, "Variable", 0, false, true, false, false, false},           // 0x02
    {NodeTag::Call, "Call", 1, true, false, false, false, false},                   // 0x03
    {NodeTag::IfExpr, "IfExpr", 3, false, false, false, false, false},              // 0x04
    {NodeTag::Lambda, "Lambda", 1, false, false, false, false, true},               // 0x05
    {NodeTag::Let, "Let", 2, false, true, false, false, false},                     // 0x06
    {NodeTag::LetRec, "LetRec", 2, false, true, false, false, false},               // 0x07
    {NodeTag::Define, "Define", 1, false, true, false, false, false},               // 0x08
    {NodeTag::Begin, "Begin", 0, true, false, false, false, false},                 // 0x09
    {NodeTag::Set, "Set", 1, false, true, false, false, false},                     // 0x0A
    {NodeTag::Quote, "Quote", 1, false, false, false, false, false},                // 0x0B
    {NodeTag::LiteralInt, "<gap>", 0, false, false, false, false, false},           // 0x0C (gap)
    {NodeTag::LiteralString, "LiteralString", 0, false, true, false, false, false}, // 0x0D
    {NodeTag::LiteralInt, "<gap>", 0, false, false, false, false,
     false}, // 0x0E (MacroDef — Expr* only)
    {NodeTag::TypeAnnotation, "TypeAnnotation", 1, false, true, false, false, false}, // 0x0F
    {NodeTag::Coercion, "Coercion", 1, false, true, false, false, false},             // 0x10
    {NodeTag::LiteralFloat, "LiteralFloat", 0, false, false, false, true, false},     // 0x11
    {NodeTag::Pair, "Pair", 2, false, false, false, false, false},                    // 0x12
    {NodeTag::LiteralInt, "<gap>", 0, false, false, false, false, false},             // 0x13 (gap)
    {NodeTag::DefineModule, "DefineModule", 0, true, false, false, false, false},    // 0x14
    {NodeTag::Export, "Export", 0, true, false, false, false, false},                 // 0x15
    {NodeTag::Linear, "Linear", 1, false, false, false, false, false},                // 0x16
    {NodeTag::Move, "Move", 1, false, false, false, false, false},                    // 0x17
    {NodeTag::Borrow, "Borrow", 1, false, false, false, false, false},                // 0x18
    {NodeTag::MutBorrow, "MutBorrow", 1, false, false, false, false, false},          // 0x19
    {NodeTag::Drop, "Drop", 1, false, false, false, false, false},                    // 0x1A
}};


export constexpr const NodeMeta& meta(NodeTag tag) {
    return kNodeMeta[static_cast<std::size_t>(tag) - 1];
}

// Compile-time validation: check known entries, skip gap sentinels
consteval bool validate_node_meta() {
    // Gap entries (0x0C, 0x0E in tag space → indices 11, 13)
    if (kNodeMeta[11].name != "<gap>")
        return false;
    if (kNodeMeta[13].name != "<gap>")
        return false;
    if (meta(NodeTag::LiteralInt).name != "LiteralInt")
        return false;
    if (meta(NodeTag::Call).fixed_children != 1)
        return false;
    if (meta(NodeTag::Call).has_var_children != true)
        return false;
    if (meta(NodeTag::Lambda).has_params != true)
        return false;
    if (meta(NodeTag::IfExpr).fixed_children != 3)
        return false;
    if (meta(NodeTag::Let).has_string != true)
        return false;
    if (meta(NodeTag::LiteralInt).has_int != true)
        return false;
    if (meta(NodeTag::Begin).has_var_children != true)
        return false;
    if (meta(NodeTag::Coercion).name != "Coercion")
        return false;
    if (meta(NodeTag::LiteralFloat).name != "LiteralFloat")
        return false;
    if (meta(NodeTag::LiteralFloat).has_float != true)
        return false;
    return true;
}
static_assert(validate_node_meta(), "kNodeMeta misaligned with NodeTag enum");

// ── NodeView — lightweight non-owning read view ────────────────
export struct NodeView {
    NodeId id = NULL_NODE;  // node index in the FlatAST
    NodeTag tag = NodeTag::LiteralInt;
    std::int64_t int_value = 0;
    double float_value = 0.0;
    SymId sym_id = INVALID_SYM;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::span<const NodeId> children;
    std::span<const SymId> params;
    std::span<const NodeId> param_annotations; // annotation node IDs, may be NULL_NODE
    SyntaxMarker marker = SyntaxMarker::User;

    bool has_int() const { return tag == NodeTag::LiteralInt; }
    bool has_float() const { return tag == NodeTag::LiteralFloat; }
    bool has_name() const { return sym_id != INVALID_SYM; }
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
    std::string operator_name; // "replace-type", "replace-value", ...
    std::string old_type_str;  // format_type(old_type) at mutation time
    std::string new_type_str;  // format_type(new_type) at mutation time
    std::string summary;       // human-readable change description
    MutationStatus status;
    // Rollback data: for apply_patches-style rollback
    std::uint32_t field_offset; // which SoA column was modified
    std::uint64_t old_value;    // original value before mutation
    std::uint64_t new_value;    // value after mutation
    bool has_rollback_data;     // true if rollback is available
};

// ── Patch — AI mutation descriptor ─────────────────────────────
export struct Patch {
    NodeId node = NULL_NODE;
    std::uint32_t field_offset = 0;
    std::uint64_t new_value = 0;
};

// ── Match clause metadata (for exhaustiveness checking) ──
// Maps let-node-id → list of constructor SymIds used in match clauses.
// If a wildcard pattern (_) is used, stores a single INVALID_SYM entry.
export struct MatchClauseInfo {
    std::vector<SymId> used_constructors;
    bool has_wildcard = false;
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
        cap_require_count_.push_back(0);
        line_.push_back(0);
        col_.push_back(0);
        marker_.push_back(m);
        type_id_.push_back(0);
        dirty_.push_back(0);
        // value cache initialized lazily (not in arena — module-level vector)
        if (id >= static_cast<NodeId>(value_cache_.size()))
            value_cache_.resize(id + 1, kNotCached);
        else
            value_cache_[id] = kNotCached;
        node_first_mutation_.push_back(0);
        parent_.push_back(NULL_NODE);
        node_gen_.push_back(generation_);
        return id;
    }

    // SoA storage (all pmr::vector = arena allocated)
    std::pmr::vector<NodeTag> tag_;
    std::pmr::vector<std::int64_t> int_val_;
    std::pmr::vector<double> float_val_;
    std::pmr::vector<SymId> sym_id_;
    std::pmr::vector<std::uint32_t> child_begin_;
    std::pmr::vector<std::uint32_t> child_count_;
    std::pmr::vector<NodeId> child_data_;
    std::pmr::vector<NodeId> parent_;
    std::pmr::vector<std::uint32_t> param_begin_;
    std::pmr::vector<std::uint32_t> param_count_;
    std::pmr::vector<std::uint32_t> cap_require_count_;
    std::pmr::vector<SymId> param_data_;
    std::pmr::vector<NodeId> param_annot_data_; // per-param annotation node IDs (NULL_NODE = none)
    // Source location (line/col, 1-based)
    std::pmr::vector<std::uint32_t> line_;
    std::pmr::vector<std::uint32_t> col_;
    // Type information (L6.5+): type_id per node, 0 = DYNAMIC
    std::pmr::vector<SyntaxMarker> marker_;
    std::pmr::vector<std::uint8_t> dirty_;
    std::pmr::vector<std::uint32_t> type_id_;
    // Module-level eval result cache (int64_t = EvalValue serialization).
    // Used by Evaluator::eval_flat for incremental evaluation (Issue #32b).
    // Indexed by NodeId. Zero = not cached. Stored at module level (not arena)
    // because the evaluator outlives individual arena scopes.
    std::vector<std::int64_t> value_cache_;
    std::vector<MutationRecord> mutation_log_;
    std::vector<std::uint32_t> node_first_mutation_;
    std::uint64_t next_mutation_id_ = 1;
    std::uint16_t generation_ = 1;
    std::pmr::vector<std::uint16_t> node_gen_;

    public:

    // Low-level raw node creation (for advanced mutation).
    // Creates a minimal node with the given tag and default fields.
    // Children must be set up manually via set_child/insert_child.
    [[nodiscard]] NodeId add_raw_node(NodeTag tag, SyntaxMarker m = SyntaxMarker::User) {
        return add_node(tag, m);
    }

    std::vector<MatchClauseInfo> match_info_;
    explicit FlatAST(std::pmr::polymorphic_allocator<std::byte> alloc = {})
        : tag_(alloc)
        , int_val_(alloc)
        , float_val_(alloc)
        , sym_id_(alloc)
        , child_begin_(alloc)
        , child_count_(alloc)
        , child_data_(alloc)
        , parent_(alloc)
        , param_begin_(alloc)
        , param_count_(alloc)
        , cap_require_count_(alloc)
        , param_data_(alloc)
    , param_annot_data_(alloc)
        , line_(alloc)
        , col_(alloc)
        , type_id_(alloc)
        , node_gen_(alloc) {}

    // ── Builders ───────────────────────────────────────────────

    // Set parent for all children of the given node
    void link_children(NodeId id) {
        auto begin = child_begin_[id];
        auto end = begin + child_count_[id];
        for (auto i = begin; i < end; ++i) {
            auto cid = child_data_[i];
            if (cid != NULL_NODE)
                parent_[cid] = id;
        }
    }

    [[nodiscard]] NodeId add_literal_float(double val) {
        auto id = add_node(NodeTag::LiteralFloat);
        float_val_[id] = val;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_literalstring(SymId name) {
        auto id = add_node(NodeTag::LiteralString);
        sym_id_[id] = name;
        child_count_[id] = 0;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_literal(std::int64_t val) {
        auto id = add_node(NodeTag::LiteralInt);
        int_val_[id] = val;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_variable(SymId name) {
        auto id = add_node(NodeTag::Variable);
        sym_id_[id] = name;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_call(NodeId func, std::span<const NodeId> args) {
        auto id = add_node(NodeTag::Call);
        child_data_.push_back(func);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.insert(child_data_.end(), args.begin(), args.end());
        child_begin_[id] = start - 1; // includes func
        child_count_[id] = 1 + static_cast<std::uint32_t>(args.size());
        link_children(id);
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
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_lambda(std::span<const SymId> params, NodeId body,
                                    bool dotted = false) {
        return add_lambda(params, {}, body, dotted);
    }
    [[nodiscard]] NodeId add_lambda(std::span<const SymId> params,
                                    std::span<const NodeId> annots,
                                    NodeId body, bool dotted = false) {
        auto id = add_node(NodeTag::Lambda);
        int_val_[id] = dotted ? 1 : 0; // store dotted flag
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), params.begin(), params.end());
        // Store annotations (or NULL_NODE if not provided)
        param_annot_data_.resize(param_annot_data_.size() + params.size(), aura::ast::NULL_NODE);
        for (std::size_t i = 0; i < params.size() && i < annots.size(); ++i)
            param_annot_data_[pstart + i] = annots[i];
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(params.size());
        child_data_.push_back(body);
        child_begin_[id] = static_cast<std::uint32_t>(child_data_.size() - 1);
        child_count_[id] = 1;
        link_children(id);
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
        link_children(id);
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
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_define(SymId name, NodeId val) {
        auto id = add_node(NodeTag::Define);
        sym_id_[id] = name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_begin_[id] = start;
        child_count_[id] = 1;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_define_type(SymId name, std::span<const SymId> params,
                                         std::span<const NodeId> ctors) {
        auto id = add_node(NodeTag::DefineType);
        sym_id_[id] = name;
        // Store type params in param_data_
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), params.begin(), params.end());
        param_annot_data_.resize(param_annot_data_.size() + params.size(), NULL_NODE);
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(params.size());
        // Store constructor nodes in child_data_
        auto cstart = static_cast<std::uint32_t>(child_data_.size());
        child_data_.insert(child_data_.end(), ctors.begin(), ctors.end());
        child_begin_[id] = cstart;
        child_count_[id] = static_cast<std::uint32_t>(ctors.size());
        return id;
    }

    [[nodiscard]] NodeId add_begin(NodeId* exprs, std::uint32_t count) {
        auto id = add_node(NodeTag::Begin);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        for (std::uint32_t i = 0; i < count; ++i)
            child_data_.push_back(exprs[i]);
        child_begin_[id] = start;
        child_count_[id] = count;
        link_children(id);
        return id;
    }
    [[nodiscard]] NodeId add_begin(std::span<const NodeId> exprs) {
        auto id = add_node(NodeTag::Begin);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.insert(child_data_.end(), exprs.begin(), exprs.end());
        child_begin_[id] = start;
        child_count_[id] = static_cast<std::uint32_t>(exprs.size());
        link_children(id);
        return id;
    }

    // Export: (export sym1 sym2 ...) — children = Variable nodes
    [[nodiscard]] NodeId add_define_module(SymId name, std::span<const SymId> type_params) {
        auto id = add_node(NodeTag::DefineModule);
        sym_id_[id] = name;
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), type_params.begin(), type_params.end());
        param_annot_data_.resize(param_annot_data_.size() + type_params.size(), NULL_NODE);
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(type_params.size());
        return id;
    }
    [[nodiscard]] NodeId add_define_module(SymId name, std::span<const SymId> type_params,
                                           std::span<const SymId> cap_require) {
        auto id = add_node(NodeTag::DefineModule);
        sym_id_[id] = name;
        // type params first
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), type_params.begin(), type_params.end());
        param_annot_data_.resize(param_annot_data_.size() + type_params.size(), NULL_NODE);
        // capability requirements after type params (no separator needed, we store count)
        param_data_.insert(param_data_.end(), cap_require.begin(), cap_require.end());
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(type_params.size());
        cap_require_count_[id] = static_cast<std::uint32_t>(cap_require.size());
        return id;
    }

    [[nodiscard]] NodeId add_export(std::span<const NodeId> syms) {
        auto id = add_node(NodeTag::Export);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.insert(child_data_.end(), syms.begin(), syms.end());
        child_begin_[id] = start;
        child_count_[id] = static_cast<std::uint32_t>(syms.size());
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_set(SymId name, NodeId val) {
        auto id = add_node(NodeTag::Set);
        sym_id_[id] = name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_begin_[id] = start;
        child_count_[id] = 1;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_macrodef(SymId name, const std::vector<SymId>& params, NodeId body,
                                      bool dotted = false) {
        auto id = add_node(NodeTag::MacroDef);
        sym_id_[id] = name;
        int_val_[id] = dotted ? 1 : 0; // store dotted flag in unused int_val_
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(body);
        child_begin_[id] = start;
        child_count_[id] = 1;
        // Store params using the same SoA as Lambda params
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), params.begin(), params.end());
        param_annot_data_.resize(param_annot_data_.size() + params.size(), NULL_NODE);
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
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_pair(NodeId car, NodeId cdr) {
        auto id = add_node(NodeTag::Pair);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(car);
        child_data_.push_back(cdr);
        child_begin_[id] = start;
        child_count_[id] = 2;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_type_annotation(SymId type_name, NodeId inner, SymId var_sym = INVALID_SYM) {
        auto id = add_node(NodeTag::TypeAnnotation);
        sym_id_[id] = type_name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        if (var_sym != INVALID_SYM) {
            int_val_[id] = static_cast<std::int64_t>(var_sym);
        }
        link_children(id);
        return id;
    }

    bool has_var_annot(NodeId id) const {
        return id < size() && int_val_[id] != 0;
    }
    SymId var_annot_sym(NodeId id) const {
        return static_cast<SymId>(int_val_[id]);
    }

    [[nodiscard]] NodeId add_coercion(NodeId inner, std::uint32_t type_id) {
        auto id = add_node(NodeTag::Coercion);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        type_id_[id] = type_id;
        link_children(id);
        return id;
    }
    [[nodiscard]] NodeId add_coercion(NodeId inner, std::uint32_t type_tag, std::uint32_t type_id) {
        auto id = add_node(NodeTag::Coercion);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        int_val_[id] = static_cast<std::int64_t>(type_tag);
        type_id_[id] = type_id;
        link_children(id);
        return id;
    }
    // ── M4 Linear ownership builders ───────────────────────────
    [[nodiscard]] NodeId add_linear(NodeId inner) {
        auto id = add_node(NodeTag::Linear);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_move(NodeId inner) {
        auto id = add_node(NodeTag::Move);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_borrow(NodeId inner) {
        auto id = add_node(NodeTag::Borrow);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_mut_borrow(NodeId inner) {
        auto id = add_node(NodeTag::MutBorrow);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_drop(NodeId inner) {
        auto id = add_node(NodeTag::Drop);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        link_children(id);
        return id;
    }

    // ── Access ─────────────────────────────────────────────────

    NodeView get(NodeId id) const {
        return NodeView{
            .id = id,
            .tag = tag_[id],
            .int_value = int_val_[id],
            .float_value = float_val_[id],
            .sym_id = sym_id_[id],
            .line = id < line_.size() ? line_[id] : 0,
            .col = id < col_.size() ? col_[id] : 0,
            .children = std::span(child_data_.data() + child_begin_[id], child_count_[id]),
            .params = std::span(param_data_.data() + param_begin_[id], param_count_[id]),
            .param_annotations = std::span(param_annot_data_.data() + param_begin_[id], param_count_[id]),
            .marker = id < marker_.size() ? marker_[id] : SyntaxMarker::User,
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

    // ── Parent access ──────────────────────────────────────────
    NodeId parent_of(NodeId id) const {
        return id < parent_.size() ? parent_[id] : NULL_NODE;
    }

    // ── Child field access ─────────────────────────────────────

    std::span<NodeId> children(NodeId id) {
        return std::span(child_data_.data() + child_begin_[id], child_count_[id]);
    }

    void set_child(NodeId id, std::uint32_t idx, NodeId child) {
        auto& slot = child_data_[child_begin_[id] + idx];
        // Clear old child's parent
        if (slot != NULL_NODE && slot < parent_.size())
            parent_[slot] = NULL_NODE;
        slot = child;
        // Set new child's parent
        if (child != NULL_NODE && child < parent_.size())
            parent_[child] = id;
    }

    // Insert a child at position idx (0 = first, child_count = append)
    // Shifts all subsequent children and updates child_begin_ for later nodes.
    void insert_child(NodeId id, std::uint32_t idx, NodeId child) {
        auto pos = child_begin_[id] + std::min(idx, child_count_[id]);
        child_data_.insert(child_data_.begin() + pos, 1, child);
        // Shift child_begin only for nodes whose children start at or after pos.
        // Children before the insertion point are unaffected by the shift.
        for (auto i = id + 1; i < tag_.size(); ++i) {
            if (child_begin_[i] >= pos)
                child_begin_[i]++;
        }
        child_count_[id]++;
        // Set new child's parent
        if (child != NULL_NODE && child < parent_.size())
            parent_[child] = id;
    }

    // Remove a child at position idx by replacing with NULL_NODE
    void remove_child(NodeId id, std::uint32_t idx) {
        if (idx < child_count_[id]) {
            auto& slot = child_data_[child_begin_[id] + idx];
            if (slot != NULL_NODE && slot < parent_.size())
                parent_[slot] = NULL_NODE;
            slot = NULL_NODE;
        }
    }

    // ── Bulk ───────────────────────────────────────────────────

    void clear() {
        tag_.clear();
        int_val_.clear();
        float_val_.clear();
        sym_id_.clear();
        child_begin_.clear();
        child_count_.clear();
        child_data_.clear();
        parent_.clear();
        param_begin_.clear();
        param_count_.clear();
        cap_require_count_.clear();
        param_data_.clear();
        param_annot_data_.clear();
        line_.clear();
        col_.clear();
        marker_.clear();
        dirty_.clear();
        type_id_.clear();
        mutation_log_.clear();
        node_first_mutation_.clear();
        node_gen_.clear();
        next_mutation_id_ = 1;
        generation_ = 1;
        match_info_.clear();
        root = NULL_NODE;
    }

    std::size_t size() const { return tag_.size(); }
    bool empty() const { return tag_.empty(); }

    // ── Marker access ─────────────────────────────────────────

    void set_marker(NodeId id, SyntaxMarker m) {
        if (id < marker_.size())
            marker_[id] = m;
    }
    SyntaxMarker marker(NodeId id) const {
        return id < marker_.size() ? marker_[id] : SyntaxMarker::User;
    }

    // ── Dirty tracking (incremental compilation) ───────────────

    void mark_dirty(NodeId id) {
        if (id >= dirty_.size())
            dirty_.resize(id + 1, false);
        dirty_[id] = true;
        clear_cached_value(id);  // invalidate result cache
    }
    void mark_subtree_dirty(NodeId id) {
        mark_dirty(id);
        auto v = get(id);
        for (auto c : v.children) {
            if (c != NULL_NODE)
                mark_subtree_dirty(c);
        }
    }
    bool is_dirty(NodeId id) const { return id < dirty_.size() && dirty_[id]; }
    void clear_dirty(NodeId id) {
        if (id < dirty_.size())
            dirty_[id] = false;
    }

    // ── Value result cache (for incremental eval) ────────────
    // Stores the last EvalValue result for each node.
    // kNotCached = not yet evaluated or cache invalidated.
    // When a node is marked dirty, its cache is cleared automatically.
    static constexpr std::int64_t kNotCached = 0x7FFFFFFFFFFFFFFFLL;  // INT64_MAX as sentinel
    std::int64_t get_cached_value(NodeId id) const {
        return id < static_cast<NodeId>(value_cache_.size()) ? value_cache_[id] : kNotCached;
    }
    void set_cached_value(NodeId id, std::int64_t val) {
        if (id >= static_cast<NodeId>(value_cache_.size()))
            value_cache_.resize(static_cast<std::size_t>(id) + 1, kNotCached);
        value_cache_[id] = val;
    }
    void clear_cached_value(NodeId id) {
        if (id < static_cast<NodeId>(value_cache_.size()))
            value_cache_[id] = kNotCached;
    }

    // ── Match clause metadata ────────────────────────────────
    void set_match_info(NodeId id, MatchClauseInfo info) {
        if (id >= match_info_.size())
            match_info_.resize(id + 1);
        match_info_[id] = std::move(info);
    }
    bool has_match_info(NodeId id) const {
        return id < match_info_.size() &&
               (!match_info_[id].used_constructors.empty() || match_info_[id].has_wildcard);
    }
    const MatchClauseInfo* get_match_info(NodeId id) const {
        if (id < match_info_.size() &&
            (!match_info_[id].used_constructors.empty() || match_info_[id].has_wildcard))
            return &match_info_[id];
        return nullptr;
    }
    // Propagate dirty upward: mark this node AND all ancestors dirty
    // Uses parent_ SoA column for O(depth) traversal (iterative, no recursion)
    void mark_dirty_upward(NodeId id) {
        std::deque<NodeId> queue;
        queue.push_back(id);
        while (!queue.empty()) {
            auto nid = queue.front();
            queue.pop_front();
            mark_dirty(nid);
            auto p = parent_[nid];
            if (p != NULL_NODE)
                queue.push_back(p);
        }
    }

    // Check if any node in a subtree (including the root) is dirty
    bool has_dirty_subtree(NodeId id) const {
        if (is_dirty(id))
            return true;
        auto v = get(id);
        for (auto c : v.children) {
            if (c != NULL_NODE && has_dirty_subtree(c))
                return true;
        }
        return false;
    }

    void clear_all_dirty() {
        std::fill(dirty_.begin(), dirty_.end(), false);
        std::fill(value_cache_.begin(), value_cache_.end(), kNotCached);
    }

    // ── Mutation audit ──────────────────────────────────────────

    // Record a mutation on a node. Returns the mutation_id.
    std::uint64_t add_mutation(NodeId node, std::string_view op_name, std::string_view old_type,
                               std::string_view new_type, std::string_view summary,
                               MutationStatus status = MutationStatus::Committed) {
        return add_mutation_with_rollback(node, op_name, old_type, new_type, summary, status, 0, 0,
                                          0, false);
    }

    // Record a mutation with rollback data (field_offset + old/new_value)
    std::uint64_t add_mutation_with_rollback(NodeId node, std::string_view op_name,
                                             std::string_view old_type, std::string_view new_type,
                                             std::string_view summary, MutationStatus status,
                                             std::uint32_t field_offset, std::uint64_t old_value,
                                             std::uint64_t new_value, bool has_rollback) {
        std::uint64_t mid = next_mutation_id_++;
        auto now =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
        mutation_log_.push_back({mid, now, node, std::string(op_name), std::string(old_type),
                                 std::string(new_type), std::string(summary), status, field_offset,
                                 old_value, new_value, has_rollback});
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
    std::uint64_t next_mutation_id() const { return next_mutation_id_; }

    // Get all mutation records (unfiltered).
    const std::vector<MutationRecord>& all_mutations() const { return mutation_log_; }
    std::vector<MutationRecord>& all_mutations() { return mutation_log_; }

    // Rollback a mutation by ID. Returns true if successful.
    // Current FlatAST generation. Incremented on rollback to invalidate stale NodeIds.
    std::uint16_t generation() const { return generation_; }

    // Check if a NodeId is valid (in-bounds and from the current generation).
    bool is_valid(NodeId id) const {
        return id < tag_.size() && id < node_gen_.size() && node_gen_[id] == generation_;
    }

    // Validate NodeId — panics on stale/dangling NodeIds.
    // Use in debug paths to catch post-rollback staleness early.
    void validate(NodeId id) const {
        if (id != NULL_NODE) [[likely]] {
            if (id >= tag_.size())
                std::abort(); // NodeId out of bounds
            if (id >= node_gen_.size())
                std::abort(); // NodeId generation array too small
            if (node_gen_[id] != generation_)
                std::abort(); // NodeId stale: generation mismatch (was rollback?)
        }
    }

    // Safe get — returns nullopt on stale/invalid NodeId.
    std::optional<NodeView> get_safe(NodeId id) const {
        if (!is_valid(id))
            return std::nullopt;
        return get(id);
    }

    bool rollback(std::uint64_t mutation_id) {
        for (auto& rec : mutation_log_) {
            if (rec.mutation_id == mutation_id) {
                if (rec.status != MutationStatus::Committed)
                    return false;
                if (!rec.has_rollback_data)
                    return false;
                // Apply old value back to the SoA column
                if (rec.target_node < tag_.size()) {
                    switch (rec.field_offset) {
                        case 0: // int_val_
                            if (rec.target_node < int_val_.size()) {
                                int_val_[rec.target_node] =
                                    static_cast<std::int64_t>(rec.old_value);
                                rec.status = MutationStatus::RolledBack;
                                ++generation_;
                                if (generation_ == 0) generation_ = 1; // wrap around
                                return true;
                            }
                            break;
                        case 1: // type_id_
                            if (rec.target_node < type_id_.size()) {
                                type_id_[rec.target_node] =
                                    static_cast<std::uint32_t>(rec.old_value);
                                rec.status = MutationStatus::RolledBack;
                                ++generation_;
                                if (generation_ == 0) generation_ = 1; // wrap around
                                return true;
                            }
                            break;
                        default:
                            return false;
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
                if (rollback(it->mutation_id))
                    ++count;
            }
        }
        return count;
    }

    // ── Type ID access ─────────────────────────────────────────

    std::uint32_t type_id(NodeId id) const { return id < type_id_.size() ? type_id_[id] : 0; }

    void set_type(NodeId id, std::uint32_t tid) {
        if (id < type_id_.size())
            type_id_[id] = tid;
    }

    void set_int(NodeId id, std::int64_t val) {
        if (id < int_val_.size())
            int_val_[id] = val;
    }
    void set_float(NodeId id, double val) {
        if (id < float_val_.size())
            float_val_[id] = val;
    }
    void set_sym(NodeId id, SymId val) {
        if (id < sym_id_.size())
            sym_id_[id] = val;
    }

    // Capability require count for DefineModule nodes
    std::uint32_t cap_require_count(NodeId id) const {
        return id < cap_require_count_.size() ? cap_require_count_[id] : 0;
    }

    // Access a param by apparent index across the combined param+cap_require storage
    SymId param_at(NodeId id, std::uint32_t idx) const {
        if (idx < param_count_[id] + cap_require_count_[id] &&
            param_begin_[id] + idx < param_data_.size())
            return param_data_[param_begin_[id] + idx];
        return INVALID_SYM;
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
