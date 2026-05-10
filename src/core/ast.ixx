export module aura.core.ast;
import std;

namespace aura::ast {

export struct Expr;

export struct SourceLocation { std::uint32_t line = 0, column = 0, file = 0; };
export struct ParsedPhase { static constexpr std::uint32_t id = 0; };

export enum class NodeTag : std::uint32_t {
    LiteralInt = 0x01, Variable = 0x02, Call = 0x03,
    IfExpr = 0x04, Lambda = 0x05, Let = 0x06, LetRec = 0x07, Define = 0x08,
};

export struct LiteralIntNode { NodeTag tag; std::int64_t value = 0; };
export struct VariableNode   { NodeTag tag; std::string name; };
export struct CallNode       { NodeTag tag; Expr* function = nullptr; std::vector<Expr*> args; };
export struct IfExprNode     { NodeTag tag; Expr* condition = nullptr; Expr* then_branch = nullptr; Expr* else_branch = nullptr; };
export struct LambdaNode     { NodeTag tag; std::vector<std::string> params; Expr* body = nullptr; };
export struct LetNode        { NodeTag tag; std::string name; Expr* value = nullptr; Expr* body = nullptr; };
export struct LetRecNode     { NodeTag tag; std::string name; Expr* value = nullptr; Expr* body = nullptr; };
export struct DefineNode     { NodeTag tag; std::string name; Expr* value = nullptr; };

export struct Expr {
    NodeTag tag;
    std::variant<LiteralIntNode, VariableNode, CallNode, IfExprNode, LambdaNode, LetNode, LetRecNode, DefineNode> payload;

    Expr(LiteralIntNode n) : tag(n.tag), payload(n) {}
    Expr(VariableNode n)   : tag(n.tag), payload(n) {}
    Expr(CallNode n)       : tag(n.tag), payload(n) {}
    Expr(IfExprNode n)     : tag(n.tag), payload(n) {}
    Expr(LambdaNode n)     : tag(n.tag), payload(n) {}
    Expr(LetNode n)        : tag(n.tag), payload(n) {}
    Expr(LetRecNode n)     : tag(n.tag), payload(n) {}
    Expr(DefineNode n)     : tag(n.tag), payload(n) {}
};

} // namespace aura::ast
