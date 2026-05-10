module aura.core.ast;

namespace aura::ast {

Expr::Expr(LiteralIntNode n) : tag(n.tag), payload(n) {}
Expr::Expr(VariableNode n)   : tag(n.tag), payload(n) {}
Expr::Expr(CallNode n)       : tag(n.tag), payload(n) {}
Expr::Expr(IfExprNode n)     : tag(n.tag), payload(n) {}
Expr::Expr(LambdaNode n)     : tag(n.tag), payload(n) {}
Expr::Expr(LetNode n)        : tag(n.tag), payload(n) {}
Expr::Expr(LetRecNode n)     : tag(n.tag), payload(n) {}
Expr::Expr(DefineNode n)     : tag(n.tag), payload(n) {}

} // namespace aura::ast
