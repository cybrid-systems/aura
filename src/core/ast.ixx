export module aura.core.ast;
import std;

namespace aura::ast {

// ── Common type aliases (shared by both pointer and flat AST) ───
export using NodeId = std::uint32_t;
export constexpr NodeId NULL_NODE = ~0u;
export using SymId = std::uint32_t;
export constexpr SymId INVALID_SYM = ~0u;

// ── Pointer-based AST (legacy, being migrated to flat index AST) ─

export struct Expr;

export struct SourceLocation { std::uint32_t line = 0, column = 0, file = 0; };
export struct ParsedPhase { static constexpr std::uint32_t id = 0; };

export enum class NodeTag : std::uint32_t {
    LiteralInt = 0x01, Variable = 0x02, Call = 0x03,
    IfExpr = 0x04, Lambda = 0x05, Let = 0x06, LetRec = 0x07, Define = 0x08,
    Begin = 0x09, Set = 0x0A, Quote = 0x0B, LiteralString = 0x0D, MacroDef = 0x0E,
    TypeAnnotation = 0x0F,
    Coercion = 0x10,
};

export struct LiteralIntNode { NodeTag tag; std::int64_t value = 0; };
export struct LiteralStringNode { NodeTag tag; std::string value; };
export struct VariableNode   { NodeTag tag; std::string name; };
export struct CallNode       { NodeTag tag; Expr* function = nullptr; std::vector<Expr*> args; };
export struct IfExprNode     { NodeTag tag; Expr* condition = nullptr; Expr* then_branch = nullptr; Expr* else_branch = nullptr; };
export struct LambdaNode     { NodeTag tag; std::vector<std::string> params; Expr* body = nullptr; };
export struct LetNode        { NodeTag tag; std::string name; Expr* value = nullptr; Expr* body = nullptr; };
export struct LetRecNode     { NodeTag tag; std::string name; Expr* value = nullptr; Expr* body = nullptr; };
export struct DefineNode     { NodeTag tag; std::string name; Expr* value = nullptr; };
export struct BeginNode     { NodeTag tag; std::vector<Expr*> exprs; };
export struct SetNode       { NodeTag tag; std::string name; Expr* value = nullptr; };
export struct MacroDefNode  { NodeTag tag; std::string name; std::vector<std::string> params; Expr* body = nullptr; };
export struct QuoteNode     { NodeTag tag; Expr* value = nullptr; };
export struct TypeAnnotationNode { NodeTag tag; Expr* inner_expr = nullptr; std::string type_name; };
export struct CoercionNode { NodeTag tag; Expr* inner_expr = nullptr; std::string to_type_name; };

export struct Expr {
    NodeTag tag;
    SourceLocation loc;   // source position (1-based line/col)
    std::variant<LiteralIntNode, VariableNode, CallNode, IfExprNode, LambdaNode,
                 LetNode, LetRecNode, DefineNode, BeginNode, SetNode, QuoteNode, MacroDefNode, LiteralStringNode, TypeAnnotationNode, CoercionNode> payload;

    Expr(LiteralIntNode n) : tag(n.tag), loc{}, payload(n) {}
    Expr(VariableNode n)   : tag(n.tag), loc{}, payload(n) {}
    Expr(CallNode n)       : tag(n.tag), loc{}, payload(n) {}
    Expr(IfExprNode n)     : tag(n.tag), payload(n) {}
    Expr(LambdaNode n)     : tag(n.tag), payload(n) {}
    Expr(LetNode n)        : tag(n.tag), payload(n) {}
    Expr(LetRecNode n)     : tag(n.tag), payload(n) {}
    Expr(DefineNode n)     : tag(n.tag), payload(n) {}
    Expr(BeginNode n)      : tag(n.tag), payload(n) {}
    Expr(SetNode n)        : tag(n.tag), payload(n) {}
    Expr(QuoteNode n)      : tag(n.tag), payload(n) {}
    Expr(MacroDefNode n)   : tag(n.tag), payload(n) {}
    Expr(LiteralStringNode n) : tag(n.tag), payload(n) {}
    Expr(TypeAnnotationNode n) : tag(n.tag), payload(n) {}
    Expr(CoercionNode n) : tag(n.tag), payload(n) {}
};

// ── Flat index-based AST (DOD prototype) ────────────────────────
//
// Deprecated: use FlatAST (aura.core.ast_flat) for new code.
// This is the Phase 1 prototype kept for backward compatibility.
//
// A single vertex in the flat AST graph.
// All node types share one struct, with unused fields zeroed.
export struct ASTNode {
    NodeTag tag = NodeTag::LiteralInt;
    std::int64_t int_value = 0;                 // LiteralInt
    std::string name;                             // Variable / Let / Define
    std::vector<std::string> params;              // Lambda
    NodeId child0 = NULL_NODE;                   // single-child links
    NodeId child1 = NULL_NODE;
    NodeId child2 = NULL_NODE;
    std::vector<NodeId> children;                 // multi-child (Call args)
};

// Flat AST container — owns all nodes, indexed by NodeId.
export class AST {
public:
    NodeId add_node() {
        auto id = static_cast<NodeId>(nodes_.size());
        nodes_.push_back({});
        return id;
    }

    NodeId add_literal(std::int64_t val) {
        auto id = add_node();
        auto& n = nodes_[id];
        n.tag = NodeTag::LiteralInt;
        n.int_value = val;
        return id;
    }

    NodeId add_variable(std::string_view name) {
        auto id = add_node();
        auto& n = nodes_[id];
        n.tag = NodeTag::Variable;
        n.name = name;
        return id;
    }

    NodeId add_call(NodeId func, std::span<const NodeId> args) {
        auto id = add_node();
        auto& n = nodes_[id];
        n.tag = NodeTag::Call;
        n.child0 = func;
        n.children.assign(args.begin(), args.end());
        return id;
    }

    NodeId add_if(NodeId cond, NodeId then_branch, NodeId else_branch) {
        auto id = add_node();
        auto& n = nodes_[id];
        n.tag = NodeTag::IfExpr;
        n.child0 = cond;
        n.child1 = then_branch;
        n.child2 = else_branch;
        return id;
    }

    NodeId add_lambda(std::vector<std::string> params, NodeId body) {
        auto id = add_node();
        auto& n = nodes_[id];
        n.tag = NodeTag::Lambda;
        n.params = std::move(params);
        n.child0 = body;
        return id;
    }

    NodeId add_let(std::string_view name, NodeId val, NodeId body) {
        auto id = add_node();
        auto& n = nodes_[id];
        n.tag = NodeTag::Let;
        n.name = name;
        n.child0 = val;
        n.child1 = body;
        return id;
    }

    NodeId add_letrec(std::string_view name, NodeId val, NodeId body) {
        auto id = add_node();
        auto& n = nodes_[id];
        n.tag = NodeTag::LetRec;
        n.name = name;
        n.child0 = val;
        n.child1 = body;
        return id;
    }

    NodeId add_define(std::string_view name, NodeId val) {
        auto id = add_node();
        auto& n = nodes_[id];
        n.tag = NodeTag::Define;
        n.name = name;
        n.child0 = val;
        return id;
    }

    // Access
    const ASTNode& operator[](NodeId id) const {
        return nodes_[id];
    }
    ASTNode& operator[](NodeId id) {
        return nodes_[id];
    }

    std::size_t size() const { return nodes_.size(); }
    bool empty() const { return nodes_.empty(); }
    void clear() { nodes_.clear(); root = NULL_NODE; }

    // Root node
    NodeId root = NULL_NODE;

    // Iterator support (for ranges)
    auto begin() { return nodes_.begin(); }
    auto end()   { return nodes_.end(); }
    auto begin() const { return nodes_.begin(); }
    auto end()   const { return nodes_.end(); }

private:
    std::vector<ASTNode> nodes_;
};

// Convert a legacy pointer-tree Expr to a flat index AST.
// Recursively walks the Expr tree, builds flat nodes, returns root ID.
export NodeId flatten_expr(const Expr* expr, AST& ast);

} // namespace aura::ast
