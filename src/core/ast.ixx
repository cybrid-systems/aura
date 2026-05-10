export module aura.core.ast;

import <cstdint>;
import <string>;
import <variant>;
import <vector>;

namespace aura::ast {

// ─── Node tags ───

export enum class NodeTag : uint32_t {
    LiteralInt   = 0x01,
    Variable     = 0x02,
    Call         = 0x03,
    IfExpr       = 0x04,
    Lambda       = 0x05,
    Let          = 0x06,
};

// ─── Source location ───

export struct SourceLocation {
    uint32_t line   = 0;
    uint32_t column = 0;
    uint32_t file   = 0;
};

// ─── Phase definitions (Trees that Grow) ───

export struct ParsedPhase {
    static constexpr uint32_t id = 0;
};

// ─── Node types ───

export struct LiteralIntNode {
    NodeTag tag   = NodeTag::LiteralInt;
    int64_t value = 0;
};

export struct VariableNode {
    NodeTag tag        = NodeTag::Variable;
    std::string name;
};

export struct CallNode {
    NodeTag tag = NodeTag::Call;
};

export struct IfExprNode {
    NodeTag tag = NodeTag::IfExpr;
};

export struct LambdaNode {
    NodeTag tag = NodeTag::Lambda;
};

export struct LetNode {
    NodeTag tag       = NodeTag::Let;
    std::string name;
    struct Expr* value = nullptr;
    struct Expr* body  = nullptr;
};

// ─── Expr variant ───

export struct Expr {
    NodeTag tag;
    std::variant<
        LiteralIntNode, VariableNode, CallNode,
        IfExprNode, LambdaNode, LetNode
    > payload;

    Expr(LiteralIntNode n) : tag(n.tag), payload(n) {}
    Expr(VariableNode n)   : tag(n.tag), payload(n) {}
    Expr(CallNode n)       : tag(n.tag), payload(n) {}
    Expr(IfExprNode n)     : tag(n.tag), payload(n) {}
    Expr(LambdaNode n)     : tag(n.tag), payload(n) {}
    Expr(LetNode n)        : tag(n.tag), payload(n) {}
};

} // namespace aura::ast
