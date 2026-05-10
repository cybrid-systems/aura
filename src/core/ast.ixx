module;
#include <cstdint>
#include <variant>
#include <string>
#include <vector>

export module aura.core.ast;

namespace aura::ast {

export enum class NodeTag : uint32_t {
    LiteralInt   = 0x01,
    Variable     = 0x02,
    Call         = 0x03,
    IfExpr       = 0x04,
    Lambda       = 0x05,
    Let          = 0x06,
    LetRec       = 0x07,
};

export struct SourceLocation {
    uint32_t line = 0, column = 0, file = 0;
};

export struct ParsedPhase {
    static constexpr uint32_t id = 0;
};

export struct LiteralIntNode {
    NodeTag tag   = NodeTag::LiteralInt;
    int64_t value = 0;
};

export struct VariableNode {
    NodeTag tag        = NodeTag::Variable;
    std::string name;
};

export struct CallNode {
    NodeTag tag                   = NodeTag::Call;
    struct Expr* function         = nullptr;
    std::vector<struct Expr*> args;
};

export struct IfExprNode {
    NodeTag tag                   = NodeTag::IfExpr;
    struct Expr* condition        = nullptr;
    struct Expr* then_branch      = nullptr;
    struct Expr* else_branch      = nullptr;
};

export struct LambdaNode {
    NodeTag tag                   = NodeTag::Lambda;
    std::vector<std::string> params;
    struct Expr* body             = nullptr;
};

export struct LetNode {
    NodeTag tag                   = NodeTag::Let;
    std::string name;
    struct Expr* value            = nullptr;
    struct Expr* body             = nullptr;
};

// Single-binding letrec. Multi-binding desugared to nested.
export struct LetRecNode {
    NodeTag tag                   = NodeTag::LetRec;
    std::string name;
    struct Expr* value            = nullptr;
    struct Expr* body             = nullptr;
};

export struct Expr {
    NodeTag tag;
    std::variant<
        LiteralIntNode, VariableNode, CallNode,
        IfExprNode, LambdaNode, LetNode, LetRecNode
    > payload;

    Expr(LiteralIntNode n) : tag(n.tag), payload(n) {}
    Expr(VariableNode n)   : tag(n.tag), payload(n) {}
    Expr(CallNode n)       : tag(n.tag), payload(n) {}
    Expr(IfExprNode n)     : tag(n.tag), payload(n) {}
    Expr(LambdaNode n)     : tag(n.tag), payload(n) {}
    Expr(LetNode n)        : tag(n.tag), payload(n) {}
    Expr(LetRecNode n)     : tag(n.tag), payload(n) {}
};

} // namespace aura::ast
