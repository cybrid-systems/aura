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

    // Issue #187 (P0): observability for the hash table. 0.0 = empty,
    // 1.0 = fully packed. load_factor (entries / capacity) is the
    // standard metric. We expose it alongside the raw counts so the
    // auto-compact trigger (Issue #187) can decide when to rehash.
    [[nodiscard]] std::size_t entry_count() const noexcept { return entry_count_; }
    [[nodiscard]] std::size_t hash_capacity() const noexcept { return hash_capacity_; }
    [[nodiscard]] double load_factor() const noexcept {
        return hash_capacity_ == 0 ? 0.0
                                   : static_cast<double>(entry_count_) /
                                         static_cast<double>(hash_capacity_);
    }
    [[nodiscard]] std::size_t hash_table_bytes() const noexcept {
        return hash_tbl_.size() * sizeof(SymId);
    }
    // Sum of byte lengths of all interned strings (excludes the
    // trailing NUL per string but includes the leading \0 sentinel).
    [[nodiscard]] std::size_t string_bytes_total() const noexcept {
        std::size_t total = 1; // leading \0
        for (std::uint32_t i = 0; i < hash_capacity_; ++i) {
            if (hash_tbl_[i] != INVALID_SYM) {
                total += resolve(hash_tbl_[i]).size() + 1; // +1 for NUL
            }
        }
        return total;
    }
    // Total memory footprint of this StringPool (buf_ + hash_tbl_ +
    // entry_count_ + hash_capacity_ bookkeeping). For the
    // observability layer to report.
    [[nodiscard]] std::size_t total_bytes() const noexcept {
        return data_size() + hash_table_bytes() + sizeof(*this);
    }
    // Fragmentation ratio of buf_ = (data_size - string_bytes_total)
    // / data_size. 0.0 = perfectly packed, 1.0 = entirely dead bytes.
    // After reset()+re-intern, the leading \0 + the most-recently-
    // appended strings dominate; older strings can leave gaps if the
    // pool has been reset-then-rebuilt. In typical steady state
    // (no reset), buf_ grows monotonically and fragmentation ~0.
    [[nodiscard]] double buf_fragmentation() const noexcept {
        std::size_t ds = data_size();
        return ds == 0 ? 0.0
                       : static_cast<double>(ds - string_bytes_total()) /
                             static_cast<double>(ds);
    }

    // Issue #187 (P0): conservative compact() that rebuilds the
    // hash table at the smallest power-of-2 capacity that still
    // holds all live entries (load factor 0.5-0.6). SymIds (which
    // are offsets into buf_) are NOT remapped — buf_ is monotonic
    // (strings are only appended, never removed), so SymIds stay
    // valid. This compact only reclaims hash_tbl_ memory, not
    // buf_ memory.
    //
    // Returns bytes reclaimed (from hash_tbl_ shrink).
    [[nodiscard]] std::size_t compact() noexcept {
        std::size_t before = hash_table_bytes();
        if (entry_count_ == 0) {
            // Nothing to keep — reset to minimum.
            rehash(64);
        } else {
            // Pick smallest power-of-2 capacity with load <= 0.5.
            std::uint32_t target = 64;
            while (target / 2 < entry_count_) target *= 2;
            if (target < hash_capacity_) {
                rehash(target);
            }
        }
        std::size_t after = hash_table_bytes();
        std::size_t saved = (before > after) ? (before - after) : 0;
        return saved;
    }

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
    // Issue #73 Phase 2: per-view TypeId cache. 0 = DYNAMIC/unknown.
    // Populated by FlatAST::get (from SoA column) and MappedCache::get
    // (from cache column), so callers reading a NodeView don't have to
    // look up the SoA column separately.
    std::uint32_t type_id = 0;
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

// Issue #147: post-mutation invariant check status.
//   NotChecked  — check has not yet been run (default after add_mutation_*).
//   Ok          — check ran, no warnings or violations found.
//   Warnings    — check ran, OwnershipNotes emitted under WarningsOnly mode
//                 (does not block execution).
//   Violations  — check ran, OwnershipNotes emitted under Strict mode
//                 (typed_mutate returns failure with diagnostics).
//
// Diagnostics themselves are surfaced via MutationResult in service.ixx
// (to keep this struct serializable without depending on type_checker.ixx).
export enum class InvariantStatus : std::uint8_t {
    NotChecked = 0,
    Ok = 1,
    Warnings = 2,
    Violations = 3,
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
    // Issue #142: extended rollback data for subtree-level mutations
    // (mutate:replace-subtree). When set, rollback re-parses old_subtree_source
    // and re-attaches the resulting root at (parent_id, child_idx).
    NodeId parent_id = NULL_NODE;       // parent of the replaced subtree slot
    std::uint32_t child_idx = 0;        // child index in parent.children
    std::string old_subtree_source;     // source of the original subtree
    bool has_subtree_rollback = false;  // true if old_subtree_source is valid
    // Issue #147: post-mutation invariant check status. Default NotChecked
    // so audit log records the actual state of the check, not a
    // synthetic "always-OK" claim.
    InvariantStatus invariant_status = InvariantStatus::NotChecked;
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
//
// `used_constructors` collects patterns like (Ctor args...) — definite.
// `candidate_constructors` collects bare-identifier patterns like `Nil` —
// these are ambiguous (could be a constructor or a variable binding) and
// are resolved by the type checker against the actual subject type.
export struct MatchClauseInfo {
    std::vector<SymId> used_constructors;
    std::vector<SymId> candidate_constructors;
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
        // Issue #79: per-node error kind (0 = no error, non-zero = ErrorKind).
        // Populated by the type-checker and runtime evaluator; queryable via
        // the AuraQuery `(has-error? N)` clause.
        error_kind_.push_back(0);
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
    // Issue #79: per-node error kind, 0 = no error, non-zero = ErrorKind
    // enum value. Populated by the type-checker and runtime evaluator;
    // queryable via the AuraQuery `(has-error? N)` clause.
    std::pmr::vector<std::uint8_t> error_kind_;
    // Module-level eval result cache (int64_t = EvalValue serialization).
    // Used by Evaluator::eval_flat for incremental evaluation (Issue #32b).
    // Indexed by NodeId. Zero = not cached. Stored at module level (not arena)
    // because the evaluator outlives individual arena scopes.
    // Issue #67: pmr::vector (was std::vector) — uses the same
    // polymorphic_allocator as the rest of the FlatAST's SoA storage.
    // When the FlatAST is destroyed, the pmr vectors' heap buffers
    // are released along with the rest of the arena's memory.
    // This eliminates the need for free_persistent_state() at process
    // exit.
    std::pmr::vector<std::int64_t> value_cache_;
    std::pmr::vector<MutationRecord> mutation_log_;
    std::pmr::vector<std::uint32_t> node_first_mutation_;
    std::uint64_t next_mutation_id_ = 1;
    std::uint16_t generation_ = 1;
    std::pmr::vector<std::uint16_t> node_gen_;

private:

    public:

    // Low-level raw node creation (for advanced mutation).
    // Creates a minimal node with the given tag and default fields.
    // Children must be set up manually via set_child/insert_child.
    [[nodiscard]] NodeId add_raw_node(NodeTag tag, SyntaxMarker m = SyntaxMarker::User) {
        return add_node(tag, m);
    }

    std::vector<MatchClauseInfo> match_info_;
    // Issue #150 Phase 1: region annotation side-tables.
    // region_by_sym_ is keyed by the define's SymId (the
    // function's bound name). region_by_lambda_id_ is keyed by
    // the lambda's NodeId (for anonymous lambdas that don't
    // have a bound name). Both are populated by the parser
    // (evaluator_impl.cpp) when it sees
    // (performance-region ...) / (evolution-region ...) wrappers.
    // Lowering reads them via get_function_region_for_sym /
    // get_function_region_for_lambda to set
    // IRFunction::region accordingly.
    std::pmr::unordered_map<SymId, std::uint8_t> region_by_sym_;
    std::pmr::unordered_map<NodeId, std::uint8_t> region_by_lambda_id_;
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
        , error_kind_(alloc)
        , node_gen_(alloc)
        , value_cache_(alloc)
        , mutation_log_(alloc)
        , node_first_mutation_(alloc) {}

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

    // Issue #150 Phase 1: region annotation side-table.
    // Maps either a define's name (SymId) or a lambda's
    // NodeId (for anonymous lambdas) to the user's
    // performance-region / evolution-region hint. The
    // lowering pass (FlatFnBuilder in lowering_impl.cpp)
    // reads this table to set IRFunction::region on the
    // corresponding IRFunction. The side-table is keyed by
    // either a SymId (for defines) or a NodeId (for anonymous
    // lambdas) — we use two parallel maps to keep the
    // namespaces separate.
    //
    // The parser populates this via set_function_region(sym, r)
    // for a define, and set_function_region(node_id, r) for a
    // lambda. The lowering pass queries via
    // get_function_region_for_sym(sym) and
    // get_function_region_for_lambda(node_id) — both return
    // std::optional<std::uint8_t>; nullopt means no annotation.
    void set_function_region(SymId name, std::uint8_t region) {
        region_by_sym_[name] = region;
    }
    // Overload tag: pass a 0 literal to disambiguate. We
    // use a sentinel parameter (an unused int) so the two
    // overloads don't collide on the same uint32_t underlying
    // type. Callers use set_function_region_sym and
    // set_function_region_lambda explicitly.
    void set_function_region_lambda(NodeId lambda_id, std::uint8_t region) {
        region_by_lambda_id_[lambda_id] = region;
    }
    [[nodiscard]] std::optional<std::uint8_t>
    get_function_region_for_sym(SymId name) const {
        auto it = region_by_sym_.find(name);
        if (it == region_by_sym_.end()) return std::nullopt;
        return it->second;
    }
    [[nodiscard]] std::optional<std::uint8_t>
    get_function_region_for_lambda(NodeId lambda_id) const {
        auto it = region_by_lambda_id_.find(lambda_id);
        if (it == region_by_lambda_id_.end()) return std::nullopt;
        return it->second;
    }

    // Issue #150 Phase 3: mutation impact analysis helper.
    // Given a SymId (the name that was mutated), find all
    // Define nodes whose value subtree references that
    // sym. Returns the list of Define node IDs whose
    // functions are potentially affected by a
    // mutate:rebind on the given name.
    //
    // Conservative MVP: direct reference only. A function
    // that calls another function which uses the mutated
    // name is NOT flagged (transitive analysis is a
    // follow-up). This is a safe underestimate: a function
    // that DOES directly reference the mutated sym is
    // flagged, and the caller of such a function MAY also
    // need invalidation but isn't flagged. The follow-up
    // (Phase 3b) would walk the call graph from these
    // results to find the transitive set.
    [[nodiscard]] std::pmr::vector<aura::ast::NodeId>
    defines_referencing_sym(SymId sym) const {
        std::pmr::vector<aura::ast::NodeId> result(
            std::pmr::polymorphic_allocator<aura::ast::NodeId>{});
        for (aura::ast::NodeId i = 0; i < size(); ++i) {
            if (i >= size()) break;
            auto v = get(i);
            if (v.tag != aura::ast::NodeTag::Define) continue;
            if (v.sym_id == sym) continue;  // the mutated Define itself
            if (v.children.empty()) continue;
            if (subtree_uses_sym(v.child(0), sym)) {
                result.push_back(i);
            }
        }
        return result;
    }

    // Recursive helper: does the subtree rooted at `root`
    // contain a Variable node whose sym_id == `sym`?
    bool subtree_uses_sym(aura::ast::NodeId root, SymId sym) const {
        if (root == aura::ast::NULL_NODE || root >= size()) return false;
        auto v = get(root);
        if (v.tag == aura::ast::NodeTag::Variable && v.sym_id == sym)
            return true;
        for (auto c : v.children) {
            if (subtree_uses_sym(c, sym)) return true;
        }
        return false;
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
                                      bool dotted = false, bool hygienic = false) {
        auto id = add_node(NodeTag::MacroDef);
        sym_id_[id] = name;
        // Issue #120: encode dotted in bit 0 and hygienic in bit 1 of
        // int_val_ (the existing unused slot for MacroDef).
        int_val_[id] = (hygienic ? 2 : 0) | (dotted ? 1 : 0);
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

    // Issue #120: query the hygienic flag on a MacroDef node.
    // Encoded in bit 1 of int_val_ (dotted is bit 0).
    bool is_hygienic_macrodef(NodeId id) const {
        if (id >= int_val_.size()) return false;
        return (int_val_[id] & 2) != 0;
    }
    bool is_dotted_macrodef(NodeId id) const {
        if (id >= int_val_.size()) return false;
        return (int_val_[id] & 1) != 0;
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
            // Issue #73 Phase 2: populate type_id on the view so
            // callers can read it without a separate flat.type_id()
            // lookup. Same value as flat.type_id(id).
            .type_id = id < type_id_.size() ? type_id_[id] : 0u,
            .children = std::span(child_data_.data() + child_begin_[id], child_count_[id]),
            .params = std::span(param_data_.data() + param_begin_[id], param_count_[id]),
            .param_annotations = std::span(param_annot_data_.data() + param_begin_[id], param_count_[id]),
            .marker = id < marker_.size() ? marker_[id] : SyntaxMarker::User,
        };
    }

    // ── Location ──────────────────────────────────────────────
    void set_loc(NodeId id, std::uint32_t line, std::uint32_t col)
        pre (id < line_.size())
        pre (id < col_.size()) {
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

    void set_marker(NodeId id, SyntaxMarker m)
        // Issue #144: markers are a hygiene signal used by
        // query:pattern and mutate:replace-subtree (Issue #140,
        // #142). A silent no-op on stale id would let a
        // macro-introduced node appear user-written.
        pre (id < marker_.size()) {
        marker_[id] = m;
    }
    SyntaxMarker marker(NodeId id) const {
        return id < marker_.size() ? marker_[id] : SyntaxMarker::User;
    }

    // ── Dirty tracking (incremental compilation) ───────────────
    //
    // Issue #188: per-node dirty bitmask. The dirty_ column is
    // repurposed as a bitmask where each bit represents a different
    // reason for invalidation. This lets the type checker do
    // *targeted* re-analysis (only re-run occurrence-narrowing when
    // a predicate changed, only re-validate ownership when a Linear
    // binding changed, etc.) instead of the coarse "re-infer
    // everything dirty" pass.
    //
    // The old single-bit semantics are preserved: `is_dirty(id)` is
    // `true` iff ANY bit is set, so existing callers see no behavior
    // change. New callers can use `is_dirty_for(id, reason)` for
    // targeted re-analysis.

    // Issue #188: dirty-reason bits. Each can be OR'd together when
    // a single mutation triggers multiple concerns. Bit 0 (kGeneral)
    // is the backward-compatible "this node needs re-inference" bit.
    enum DirtyReason : std::uint8_t {
        kGeneralDirty       = 0x01, // node type must be re-inferred
        kConstraintDirty    = 0x02, // constraints involving this var changed
        kOccurrenceDirty    = 0x04, // occurrence-narrowing affected
        kOwnershipDirty     = 0x08, // Linear/Move/Borrow state changed
        kCoercionDirty      = 0x10, // deferred coercion needs re-apply
    };

    // Issue #188: mark a node dirty for one or more specific reasons.
    // The `kGeneralDirty` bit is set automatically so existing
    // is_dirty() callers still see "this node needs work".
    void mark_dirty(NodeId id, std::uint8_t reasons = kGeneralDirty) {
        if (id >= dirty_.size())
            dirty_.resize(id + 1, 0);
        dirty_[id] |= reasons;
        clear_cached_value(id);  // invalidate result cache
    }
    void mark_subtree_dirty(NodeId id, std::uint8_t reasons = kGeneralDirty) {
        mark_dirty(id, reasons);
        auto v = get(id);
        for (auto c : v.children) {
            if (c != NULL_NODE)
                mark_subtree_dirty(c, reasons);
        }
    }
    // Issue #188: backward-compatible single-bit semantics — true if
    // any dirty bit is set. The pre-#188 callers that asked "is this
    // node dirty?" still get the right answer.
    bool is_dirty(NodeId id) const {
        return id < dirty_.size() && dirty_[id] != 0;
    }
    // Issue #188: targeted check — true if a specific reason bit
    // (or any of the bits in the reason mask) is set. Lets the type
    // checker say "this node's occurrence narrowing is stale but
    // ownership is fine" and re-narrow only.
    bool is_dirty_for(NodeId id, std::uint8_t reason_mask) const {
        return id < dirty_.size() && (dirty_[id] & reason_mask) != 0;
    }
    // Issue #188: return the full dirty bitmask (for diagnostics).
    std::uint8_t dirty_reasons(NodeId id) const {
        return id < dirty_.size() ? dirty_[id] : 0;
    }
    void clear_dirty(NodeId id) {
        if (id < dirty_.size())
            dirty_[id] = 0;
    }
    // Issue #188: clear specific reason bits (leaves others set).
    // Used after a targeted re-analysis pass so the bit for the
    // resolved concern is cleared but other stale reasons remain.
    void clear_dirty_for(NodeId id, std::uint8_t reason_mask) {
        if (id < dirty_.size())
            dirty_[id] &= static_cast<std::uint8_t>(~reason_mask);
    }
    // Issue #188: read-only view of the dirty column for
    // observability/aggregation. Used by the (dirty:counts)
    // primitive to walk all nodes in O(n) without a per-node
    // accessor call.
    [[nodiscard]] const std::pmr::vector<std::uint8_t>& dirty_column() const noexcept {
        return dirty_;
    }

    // ── Node validation (NodeMeta invariants) ─────────────────
    // Checks a single node against its NodeMeta invariants.
    // Returns a description of the first violation, or empty string if valid.
    // If fail_on_error is true, asserts on violation.
    std::string validate_node(NodeId id, bool fail_on_error = true) const;

    // Validate all nodes in the FlatAST. Returns total violations found.
    // If fail_on_error is true, asserts on first violation.
    std::size_t validate_all_nodes(bool fail_on_error = true) const;

    // Validation note type (for non-fatal reporting)
    struct ValidationError {
        NodeId node;
        std::string message;
        std::string expected;
        std::string actual;
    };
    // Validate all nodes, populating errors vector instead of asserting.
    std::size_t validate_all_nodes(std::vector<ValidationError>& errors) const;

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
        if (id >= match_info_.size()) return false;
        const auto& mi = match_info_[id];
        return !mi.used_constructors.empty() || !mi.candidate_constructors.empty() || mi.has_wildcard;
    }
    const MatchClauseInfo* get_match_info(NodeId id) const {
        if (!has_match_info(id)) return nullptr;
        return &match_info_[id];
    }
    // Propagate dirty upward: mark this node AND all ancestors dirty
    // Uses parent_ SoA column for O(depth) traversal (iterative, no recursion)
    // Issue #188: optional `reasons` parameter propagates the bitmask
    // from the leaf to all ancestors. Default is kGeneralDirty for
    // backward compatibility with the 30+ callers that don't yet
    // classify their mutations.
    void mark_dirty_upward(NodeId id, std::uint8_t reasons = kGeneralDirty) {
        std::deque<NodeId> queue;
        queue.push_back(id);
        while (!queue.empty()) {
            auto nid = queue.front();
            queue.pop_front();
            mark_dirty(nid, reasons);
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
        // DO NOT clear value_cache_ here! The value cache persists across
        // eval-current calls so that non-dirty subtrees keep their cached
        // results. Only mark_dirty() (called on mutation) clears individual
        // cache entries. This enables subtree-level incremental re-evaluation:
        // after eval-current, the cache is populated. On the next call, clean
        // nodes return cached results immediately (see eval_flat cache check).
        // When a mutation marks nodes dirty, their cache entries are cleared
        // by mark_dirty() → clear_cached_value().
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
                                 old_value, new_value, has_rollback, NULL_NODE, 0, "", false});
        // Auto-mark node AND ancestors dirty on mutation
        mark_dirty_upward(node);

        // Update node_first_mutation_ index
        if (node < node_first_mutation_.size() && node_first_mutation_[node] == 0) {
            node_first_mutation_[node] = static_cast<std::uint32_t>(mutation_log_.size());
        }
        // If node index doesn't exist yet, we'll set it on next get
        return mid;
    }

    // Issue #142: record a subtree-level mutation (e.g. mutate:replace-subtree).
    // The target_node here is the NEW subtree's root. The old_subtree_source is
    // kept verbatim so rollback can re-parse and re-attach without needing
    // a generation-aware node lookup.
    std::uint64_t add_mutation_subtree(NodeId target_node, NodeId parent_id,
                                       std::uint32_t child_idx,
                                       std::string_view old_subtree_source,
                                       std::string_view op_name,
                                       std::string_view summary) {
        std::uint64_t mid = next_mutation_id_++;
        auto now =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
        mutation_log_.push_back({mid, now, target_node, std::string(op_name), "", "",
                                 std::string(summary), MutationStatus::Committed, 0, 0, 0, false,
                                 parent_id, child_idx, std::string(old_subtree_source), true});
        if (target_node != NULL_NODE)
            mark_dirty_upward(target_node);
        if (parent_id != NULL_NODE)
            mark_dirty_upward(parent_id);
        if (target_node < node_first_mutation_.size() && node_first_mutation_[target_node] == 0) {
            node_first_mutation_[target_node] = static_cast<std::uint32_t>(mutation_log_.size());
        }
        return mid;
    }

    // Get mutation history for a specific node (0 == no history)
    // Get mutation history for a specific node (filters from log, O(n) in log size)
    std::pmr::vector<MutationRecord> mutation_history(NodeId node) const {
        std::pmr::vector<MutationRecord> result;
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
    const std::pmr::vector<MutationRecord>& all_mutations() const { return mutation_log_; }
    std::pmr::vector<MutationRecord>& all_mutations() { return mutation_log_; }

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
                // Issue #142: subtree rollback path. Re-parse the
                // old_subtree_source and re-attach it at (parent_id, child_idx).
                if (rec.has_subtree_rollback) {
                    if (rec.parent_id == NULL_NODE || rec.parent_id >= tag_.size())
                        return false;
                    // Use the workspace's StringPool if available; otherwise
                    // the caller should not have created a subtree record.
                    // We don't have direct access to the pool here, so the
                    // primitive layer (rollback primitive) handles re-parsing
                    // and re-attachment via a higher-level API. This branch
                    // just marks the record as rolled back and bumps generation.
                    rec.status = MutationStatus::RolledBack;
                    ++generation_;
                    if (generation_ == 0) generation_ = 1;
                    if (rec.parent_id < tag_.size())
                        mark_dirty_upward(rec.parent_id);
                    return true;
                }
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

    // ── Error kind access (Issue #79) ────────────────────────
    // 0 = no error, non-zero = ErrorKind enum value. Lets the type-checker
    // and runtime evaluator tag the offending node so AuraQuery
    // `(has-error? N)` can find it without scanning all diagnostics.
    std::uint8_t node_error(NodeId id) const {
        return id < error_kind_.size() ? error_kind_[id] : 0;
    }
    void set_node_error(NodeId id, std::uint8_t kind) {
        if (id < error_kind_.size())
            error_kind_[id] = kind;
    }
    void clear_node_error(NodeId id) {
        if (id < error_kind_.size())
            error_kind_[id] = 0;
    }

    void set_int(NodeId id, std::int64_t val)
        // Issue #144: id must be a valid node. Without this,
        // a stale NodeId from a previous generation would
        // silently no-op (the size check below) and the mutation
        // would vanish without a diagnostic.
        pre (id < int_val_.size()) {
        int_val_[id] = val;
    }
    void set_float(NodeId id, double val)
        pre (id < float_val_.size()) {
        float_val_[id] = val;
    }
    void set_sym(NodeId id, SymId val)
        pre (id < sym_id_.size()) {
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

    // Set a param by index. Used by mutate:rename-symbol to rename
    // Lambda parameters (whose symbols live in param_data_, not
    // sym_id_). Issue #139.
    void set_param_at(NodeId id, std::uint32_t idx, SymId val) {
        if (idx < param_count_[id] + cap_require_count_[id] &&
            param_begin_[id] + idx < param_data_.size())
            param_data_[param_begin_[id] + idx] = val;
    }

    // Issue #139: convenience wrapper that iterates the params of a
    // Lambda (or any node with params) and replaces any param with
    // sym_id matching `oldsym` to `newsym`. Returns the count of
    // replacements. Used by mutate:rename-symbol so the param list
    // of `(lambda (x) ...)` is updated when the caller renames `x`.
    template <typename Callback>
    std::size_t rename_param(NodeId id, SymId oldsym, SymId newsym, Callback next_idx) {
        if (id >= param_count_.size() || id >= param_begin_.size()) return 0;
        std::size_t n = 0;
        for (std::uint32_t i = 0; i < param_count_[id] + cap_require_count_[id]; ++i) {
            if (param_data_[param_begin_[id] + i] == oldsym) {
                param_data_[param_begin_[id] + i] = newsym;
                ++n;
                if constexpr (!std::is_null_pointer_v<Callback>)
                    next_idx(i);
            }
        }
        return n;
    }

    // Resolve type names → TypeIds for all TypeAnnotation nodes
    // Requires TypeRegistry for name resolution
    void resolve_type_ids(class aura::core::TypeRegistry& reg, StringPool& pool);

    NodeId root = NULL_NODE;
};

// ── Patch application ──────────────────────────────────────────
export bool apply_patches(FlatAST& ast, std::span<const Patch> patches)
    pre (!patches.empty());

// ── Delta fixup (for deserialization) ──────────────────────────
export void fixup_deltas(FlatAST& ast);

// ── Bridge from pointer tree to FlatAST ────────────────────────


} // namespace aura::ast
