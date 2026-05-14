export module aura.core.ast;
import std;

namespace aura::ast {

// ── Common type aliases ──────────────────────────────────────
export using NodeId = std::uint32_t;
export constexpr NodeId NULL_NODE = ~0u;
export using SymId = std::uint32_t;
export constexpr SymId INVALID_SYM = ~0u;

// ── Source location ──────────────────────────────────────────
export struct SourceLocation { std::uint32_t line = 0, column = 0, file = 0; };

// ── Phase tag (Trees-That-Grow) ──────────────────────────────
export struct ParsedPhase { static constexpr std::uint32_t id = 0; };

// ── Node tags ────────────────────────────────────────────────
export enum class NodeTag : std::uint32_t {
    LiteralInt = 0x01, Variable = 0x02, Call = 0x03,
    IfExpr = 0x04, Lambda = 0x05, Let = 0x06, LetRec = 0x07, Define = 0x08,
    Begin = 0x09, Set = 0x0A, Quote = 0x0B, LiteralString = 0x0D, MacroDef = 0x0E,
    TypeAnnotation = 0x0F,
    Coercion = 0x10,
};

// ── Pointer-based AST (used by tree-walker evaluator) ────────
export struct Expr;

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
    SourceLocation loc;
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

} // namespace aura::ast
