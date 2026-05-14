# 0 "/home/dev/code/aura/src/core/ast.ixx"
# 1 "/home/dev/code/aura/build_debug//"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/core/ast.ixx"
export module aura.core.ast;
import std;
import aura.core.type;

namespace aura::ast {


export using NodeId = std::uint32_t;
export constexpr NodeId NULL_NODE = ~0u;
export using SymId = std::uint32_t;
export constexpr SymId INVALID_SYM = ~0u;


export struct SourceLocation { std::uint32_t line = 0, column = 0, file = 0; };

export enum class SyntaxMarker : std::uint8_t {
    User = 0,
    MacroIntroduced = 1,
};



export struct ParsedPhase { static constexpr std::uint32_t id = 0; };


export enum class NodeTag : std::uint32_t {
    LiteralInt = 0x01, Variable = 0x02, Call = 0x03,
    IfExpr = 0x04, Lambda = 0x05, Let = 0x06, LetRec = 0x07, Define = 0x08,
    Begin = 0x09, Set = 0x0A, Quote = 0x0B, LiteralString = 0x0D, MacroDef = 0x0E,
    TypeAnnotation = 0x0F,
    Coercion = 0x10,
};
# 45 "/home/dev/code/aura/src/core/ast.ixx"
export class StringPool {
public:
    explicit StringPool(std::pmr::polymorphic_allocator<std::byte> alloc = {})
        : buf_(alloc), hash_tbl_(alloc)
    {

        buf_.push_back('\0');

        rehash(64);
    }


    SymId intern(std::string_view s) {
        if (s.empty()) return 0;

        auto hash = hash_str(s);
        auto mask = hash_capacity_ - 1;
        auto idx = hash & mask;

        while (hash_tbl_[idx] != INVALID_SYM) {
            auto existing = hash_tbl_[idx];
            auto view = resolve(existing);
            if (view == s) return existing;
            idx = (idx + 1) & mask;
        }


        auto offset = static_cast<SymId>(buf_.size());
        buf_.insert(buf_.end(), s.begin(), s.end());
        buf_.push_back('\0');

        hash_tbl_[idx] = offset;


        ++entry_count_;
        if (entry_count_ * 2 > hash_capacity_)
            rehash(hash_capacity_ * 2);

        return offset;
    }


    std::string_view resolve(SymId id) const {
        if (id >= buf_.size()) return {};
        return std::string_view(buf_.data() + id);
    }


    std::size_t data_size() const { return buf_.size(); }


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


    std::pmr::vector<char> buf_;


    std::pmr::vector<SymId> hash_tbl_;
    std::uint32_t hash_capacity_ = 0;
    std::uint32_t entry_count_ = 0;
};


export struct NodeMeta {
    NodeTag tag;
    std::string_view name;
    std::uint8_t fixed_children;
    bool has_var_children;
    bool has_string;
    bool has_int;
    bool has_params;
};




export constexpr std::array<NodeMeta, 16> kNodeMeta = {{
    {NodeTag::LiteralInt, "LiteralInt", 0, false, false, true, false},
    {NodeTag::Variable, "Variable", 0, false, true, false, false},
    {NodeTag::Call, "Call", 1, true, false, false, false},
    {NodeTag::IfExpr, "IfExpr", 3, false, false, false, false},
    {NodeTag::Lambda, "Lambda", 1, false, false, false, true},
    {NodeTag::Let, "Let", 2, false, true, false, false},
    {NodeTag::LetRec, "LetRec", 2, false, true, false, false},
    {NodeTag::Define, "Define", 1, false, true, false, false},
    {NodeTag::Begin, "Begin", 0, true, false, false, false},
    {NodeTag::Set, "Set", 1, false, true, false, false},
    {NodeTag::Quote, "Quote", 1, false, false, false, false},
    {NodeTag::LiteralInt, "<gap>", 0, false, false, false, false},
    {NodeTag::LiteralString, "LiteralString", 0, false, true, false, false},
    {NodeTag::LiteralInt, "<gap>", 0, false, false, false, false},
    {NodeTag::TypeAnnotation, "TypeAnnotation", 1, false, true, false, false},
    {NodeTag::Coercion, "Coercion", 1, false, true, false, false},
}};


export constexpr const NodeMeta& meta(NodeTag tag) {
    return kNodeMeta[static_cast<std::size_t>(tag) - 1];
}


consteval bool validate_node_meta() {

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
    return true;
}
static_assert(validate_node_meta(), "kNodeMeta misaligned with NodeTag enum");


export struct NodeView {
    NodeTag tag = NodeTag::LiteralInt;
    std::int64_t int_value = 0;
    SymId sym_id = INVALID_SYM;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::span<const NodeId> children;
    std::span<const SymId> params;
    SyntaxMarker marker = SyntaxMarker::User;

    bool has_int() const { return tag == NodeTag::LiteralInt; }
    bool has_name() const { return sym_id != INVALID_SYM; }
    NodeId child(std::uint32_t i) const { return children[i]; }
};


export struct Patch {
    NodeId node = NULL_NODE;
    std::uint32_t field_offset = 0;
    std::uint64_t new_value = 0;
};


export class FlatAST {
private:
    NodeId add_node(NodeTag tag, SyntaxMarker m = SyntaxMarker::User) {
        auto id = static_cast<NodeId>(tag_.size());
        tag_.push_back(tag);
        int_val_.push_back(0);
        sym_id_.push_back(INVALID_SYM);
        child_begin_.push_back(0);
        child_count_.push_back(0);
        param_begin_.push_back(0);
        param_count_.push_back(0);
        line_.push_back(0);
        col_.push_back(0);
        marker_.push_back(m);
        type_id_.push_back(0);
        return id;
    }


    std::pmr::vector<NodeTag> tag_;
    std::pmr::vector<std::int64_t> int_val_;
    std::pmr::vector<SymId> sym_id_;
    std::pmr::vector<std::uint32_t> child_begin_;
    std::pmr::vector<std::uint32_t> child_count_;
    std::pmr::vector<NodeId> child_data_;
    std::pmr::vector<std::uint32_t> param_begin_;
    std::pmr::vector<std::uint32_t> param_count_;
    std::pmr::vector<SymId> param_data_;

    std::pmr::vector<std::uint32_t> line_;
    std::pmr::vector<std::uint32_t> col_;

    std::pmr::vector<SyntaxMarker> marker_;
    std::pmr::vector<std::uint32_t> type_id_;

public:
    explicit FlatAST(std::pmr::polymorphic_allocator<std::byte> alloc = {})
        : tag_(alloc), int_val_(alloc), sym_id_(alloc),
          child_begin_(alloc), child_count_(alloc), child_data_(alloc),
          param_begin_(alloc), param_count_(alloc), param_data_(alloc),
          line_(alloc), col_(alloc), type_id_(alloc)
    {}



    NodeId add_literalstring(SymId name) {
        auto id = add_node(NodeTag::LiteralString);
        sym_id_[id] = name;
        child_count_[id] = 0;
        return id;
    }

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
        child_begin_[id] = start - 1;
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

    NodeId add_macrodef(SymId name, const std::vector<SymId>& params, NodeId body) {
        auto id = add_node(NodeTag::MacroDef);
        sym_id_[id] = name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(body);
        child_begin_[id] = start;
        child_count_[id] = 1;

        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), params.begin(), params.end());
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(params.size());
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

    NodeId add_type_annotation(SymId type_name, NodeId inner) {
        auto id = add_node(NodeTag::TypeAnnotation);
        sym_id_[id] = type_name;
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        return id;
    }

    NodeId add_coercion(NodeId inner, std::uint32_t type_id) {
        auto id = add_node(NodeTag::Coercion);
        auto start = static_cast<std::uint32_t>(child_data_.size());
        child_data_.push_back(inner);
        child_begin_[id] = start;
        child_count_[id] = 1;
        type_id_[id] = type_id;
        return id;
    }


    NodeView get(NodeId id) const {
        return NodeView{
            .tag = tag_[id],
            .int_value = int_val_[id],
            .sym_id = sym_id_[id],
            .line = id < line_.size() ? line_[id] : 0,
            .col = id < col_.size() ? col_[id] : 0,
            .children = std::span(child_data_.data() + child_begin_[id],
                                  child_count_[id]),
            .params = std::span(param_data_.data() + param_begin_[id],
                                  param_count_[id]),
            .marker = id < marker_.size() ? marker_[id] : SyntaxMarker::User,
        };
    }


    void set_loc(NodeId id, std::uint32_t line, std::uint32_t col) {
        line_[id] = line;
        col_[id] = col;
    }
    std::uint32_t line(NodeId id) const { return line_[id]; }
    std::uint32_t col(NodeId id) const { return col_[id]; }


    NodeTag& tag(NodeId id) { return tag_[id]; }
    std::int64_t& int_val(NodeId id) { return int_val_[id]; }
    SymId& sym_id(NodeId id) { return sym_id_[id]; }



    std::span<NodeId> children(NodeId id) {
        return std::span(child_data_.data() + child_begin_[id],
                         child_count_[id]);
    }

    void set_child(NodeId id, std::uint32_t idx, NodeId child) {
        child_data_[child_begin_[id] + idx] = child;
    }



    void clear() {
        tag_.clear(); int_val_.clear(); sym_id_.clear();
        child_begin_.clear(); child_count_.clear(); child_data_.clear();
        param_begin_.clear(); param_count_.clear(); param_data_.clear();
        type_id_.clear();
        root = NULL_NODE;
    }

    std::size_t size() const { return tag_.size(); }
    bool empty() const { return tag_.empty(); }



    void set_marker(NodeId id, SyntaxMarker m) {
        if (id < marker_.size()) marker_[id] = m;
    }
    SyntaxMarker marker(NodeId id) const {
        return id < marker_.size() ? marker_[id] : SyntaxMarker::User;
    }



    std::uint32_t type_id(NodeId id) const {
        return id < type_id_.size() ? type_id_[id] : 0;
    }

    void set_type(NodeId id, std::uint32_t tid) {
        if (id < type_id_.size()) type_id_[id] = tid;
    }



    void resolve_type_ids(class aura::core::TypeRegistry& reg, StringPool& pool);

    NodeId root = NULL_NODE;

};


export bool apply_patches(FlatAST& ast, std::span<const Patch> patches);


export void fixup_deltas(FlatAST& ast);




}
