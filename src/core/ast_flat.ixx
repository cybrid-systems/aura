export module aura.core.ast_flat;
import std;
import aura.core;
import aura.core.ast_pool;

namespace aura::ast {

// ── Type aliases ───────────────────────────────────────────────

// ── Node metadata (constexpr table for reflection/validation) ──
export struct NodeMeta {
    NodeTag tag;
    std::string_view name;
    std::uint8_t fixed_children;       // how many children this tag has
    bool has_var_children;        // variable-length children (Call args)
    bool has_string;              // has a name/id string (Variable/Let/Define)
    bool has_int;                 // has an int64 value (LiteralInt)
    bool has_params;              // has param list (Lambda)
};

export constexpr std::array<NodeMeta, 11> kNodeMeta = {{
    {NodeTag::LiteralInt, "LiteralInt", 0, false, false, true,  false},
    {NodeTag::Variable,   "Variable",   0, false, true,  false, false},
    {NodeTag::Call,       "Call",       1, true,  false, false, false},
    {NodeTag::IfExpr,     "IfExpr",     3, false, false, false, false},
    {NodeTag::Lambda,     "Lambda",     1, false, false, false, true},
    {NodeTag::Let,        "Let",        2, false, true,  false, false},
    {NodeTag::LetRec,     "LetRec",     2, false, true,  false, false},
    {NodeTag::Define,     "Define",     1, false, true,  false, false},
    {NodeTag::Begin,      "Begin",      0, true,  false, false, false},
    {NodeTag::Set,        "Set",        1, false, true,  false, false},
    {NodeTag::Quote,      "Quote",      1, false, false, false, false},
}};

export constexpr const NodeMeta& meta(NodeTag tag) {
    return kNodeMeta[static_cast<std::size_t>(tag) - 1];
}

// ── NodeView — lightweight non-owning read view ────────────────
export struct NodeView {
    NodeTag tag = NodeTag::LiteralInt;
    std::int64_t int_value = 0;
    SymId sym_id = INVALID_SYM;
    std::span<const NodeId> children;
    std::span<const SymId> params;

    bool has_int()   const { return tag == NodeTag::LiteralInt; }
    bool has_name()  const { return sym_id != INVALID_SYM; }
    NodeId child(std::uint32_t i) const { return children[i]; }
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
    NodeId add_node(NodeTag tag) {
        auto id = static_cast<NodeId>(tag_.size());
        tag_.push_back(tag);
        int_val_.push_back(0);
        sym_id_.push_back(INVALID_SYM);
        child_begin_.push_back(0);
        child_count_.push_back(0);
        param_begin_.push_back(0);
        param_count_.push_back(0);
        return id;
    }

    // SoA storage (all pmr::vector = arena allocated)
    std::pmr::vector<NodeTag>   tag_;
    std::pmr::vector<std::int64_t> int_val_;
    std::pmr::vector<SymId>     sym_id_;
    std::pmr::vector<std::uint32_t>  child_begin_;
    std::pmr::vector<std::uint32_t>  child_count_;
    std::pmr::vector<NodeId>    child_data_;
    std::pmr::vector<std::uint32_t>  param_begin_;
    std::pmr::vector<std::uint32_t>  param_count_;
    std::pmr::vector<SymId>     param_data_;

public:
    explicit FlatAST(std::pmr::polymorphic_allocator<std::byte> alloc = {})
        : tag_(alloc), int_val_(alloc), sym_id_(alloc),
          child_begin_(alloc), child_count_(alloc), child_data_(alloc),
          param_begin_(alloc), param_count_(alloc), param_data_(alloc)
    {}

    // ── Builders ───────────────────────────────────────────────

    NodeId add_literal(std::int64_t val) {
        auto id = add_node(NodeTag::LiteralInt);
        int_val_[id] = val;
        return id;
    }

    NodeId add_variable(SymId name) {
        auto id = add_node(NodeTag::Variable);
        sym_id_[id] = name;
        return id;
    }

    NodeId add_call(NodeId func, std::span<const NodeId> args) {
        auto id = add_node(NodeTag::Call);
        child_data_.push_back(func);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.insert(child_data_.end(), args.begin(), args.end());
        child_begin_[id] = start - 1; // includes func
        child_count_[id] = 1 + static_cast<std::uint32_t>(args.size());
        return id;
    }

    NodeId add_if(NodeId cond, NodeId then_b, NodeId else_b) {
        auto id = add_node(NodeTag::IfExpr);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(cond);
        child_data_.push_back(then_b);
        child_data_.push_back(else_b);
        child_begin_[id] = start;
        child_count_[id] = 3;
        return id;
    }

    NodeId add_lambda(std::span<const SymId> params, NodeId body) {
        auto id = add_node(NodeTag::Lambda);
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), params.begin(), params.end());
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(params.size());
        child_data_.push_back(body);
        child_begin_[id] = static_cast<std::uint32_t>(child_data_.size() - 1);
        child_count_[id] = 1;
        return id;
    }

    NodeId add_let(SymId name, NodeId val, NodeId body) {
        auto id = add_node(NodeTag::Let);
        sym_id_[id] = name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_data_.push_back(body);
        child_begin_[id] = start;
        child_count_[id] = 2;
        return id;
    }

    NodeId add_letrec(SymId name, NodeId val, NodeId body) {
        auto id = add_node(NodeTag::LetRec);
        sym_id_[id] = name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_data_.push_back(body);
        child_begin_[id] = start;
        child_count_[id] = 2;
        return id;
    }

    NodeId add_define(SymId name, NodeId val) {
        auto id = add_node(NodeTag::Define);
        sym_id_[id] = name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_begin_[id] = start;
        child_count_[id] = 1;
        return id;
    }


    NodeId add_begin(NodeId* exprs, std::uint32_t count) {
        auto id = add_node(NodeTag::Begin);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        for (std::uint32_t i = 0; i < count; ++i) child_data_.push_back(exprs[i]);
        child_begin_[id] = start;
        child_count_[id] = count;
        return id;
    }

    NodeId add_set(SymId name, NodeId val) {
        auto id = add_node(NodeTag::Set);
        sym_id_[id] = name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_begin_[id] = start;
        child_count_[id] = 1;
        return id;
    }

    NodeId add_quote(NodeId val) {
        auto id = add_node(NodeTag::Quote);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(val);
        child_begin_[id] = start;
        child_count_[id] = 1;
        return id;
    }
    // ── Access ─────────────────────────────────────────────────

    NodeView get(NodeId id) const {
        return NodeView{
            .tag      = tag_[id],
            .int_value = int_val_[id],
            .sym_id   = sym_id_[id],
            .children = std::span(child_data_.data() + child_begin_[id],
                                  child_count_[id]),
            .params   = std::span(param_data_.data() + param_begin_[id],
                                  param_count_[id]),
        };
    }

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

    // ── Bulk ───────────────────────────────────────────────────

    void clear() {
        tag_.clear(); int_val_.clear(); sym_id_.clear();
        child_begin_.clear(); child_count_.clear(); child_data_.clear();
        param_begin_.clear(); param_count_.clear(); param_data_.clear();
        root = NULL_NODE;
    }

    std::size_t size() const { return tag_.size(); }
    bool empty() const { return tag_.empty(); }

    NodeId root = NULL_NODE;

};

// ── Patch application ──────────────────────────────────────────
export bool apply_patches(FlatAST& ast, std::span<const Patch> patches);

// ── Delta fixup (for deserialization) ──────────────────────────
export void fixup_deltas(FlatAST& ast);

// ── Bridge from pointer tree to FlatAST ────────────────────────
// Temporary: new code should bypass Expr* entirely.
export NodeId flatten_to_flat(const Expr* expr, FlatAST& ast, StringPool& pool);

} // namespace aura::ast
