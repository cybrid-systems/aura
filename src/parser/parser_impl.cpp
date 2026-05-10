module aura.parser.parser;
import std;

namespace aura::parser {

ParseResult Parser::parse(std::string_view s) {
    lexer_.emplace(s); ParseResult r; r.arena=&arena_; r.root=parse_expr();
    if (r.root) r.success=true; else r.error="parse error"; return r;
}
ast::Expr* Parser::parse_expr() {
    if (!lexer_) return nullptr;
    auto tok = lexer_->peek();
    switch (tok.kind) {
    case TokenKind::Integer: return parse_int(lexer_->consume());
    case TokenKind::Identifier: { auto t=lexer_->consume(); return arena_.template create<ast::Expr>(ast::VariableNode{{},std::string(t.text)}); }
    case TokenKind::LParen: lexer_->consume(); return parse_list();
    default: return nullptr;
    }
}
ast::Expr* Parser::parse_int(Token tok) {
    try { auto v=std::stoll(std::string(tok.text)); return arena_.template create<ast::Expr>(ast::LiteralIntNode{{},v}); }
    catch(...) { return nullptr; }
}
ast::Expr* Parser::parse_list() {
    auto f = lexer_->peek();
    if (f.kind==TokenKind::Identifier) {
        auto kw=f.text;
        if (kw=="if") return parse_if();
        if (kw=="lambda") return parse_lambda();
        if (kw=="let") return parse_let(false);
        if (kw=="letrec") return parse_let(true);
        if (kw=="define") return parse_define();
    }
    auto* func = parse_expr(); if (!func) { skip_rparen(); return nullptr; }
    ast::CallNode call;
    while (lexer_->peek().kind!=TokenKind::RParen&&!lexer_->eof()) { auto* a=parse_expr(); if(a) call.args.push_back(a); else break; }
    lexer_->consume(); call.function=func;
    return arena_.template create<ast::Expr>(std::move(call));
}
ast::Expr* Parser::parse_if() { lexer_->consume(); auto c=parse_expr(),t=parse_expr(),e=parse_expr(); lexer_->consume(); return arena_.template create<ast::Expr>(ast::IfExprNode{{},c,t,e}); }
ast::Expr* Parser::parse_lambda() {
    lexer_->consume(); if(lexer_->consume().kind!=TokenKind::LParen) return nullptr;
    ast::LambdaNode lam;
    while (lexer_->peek().kind!=TokenKind::RParen) { auto t=lexer_->consume(); if(t.kind!=TokenKind::Identifier) return nullptr; lam.params.push_back(std::string(t.text)); }
    lexer_->consume(); lam.body=parse_expr(); if(!lam.body) return nullptr; lexer_->consume();
    return arena_.template create<ast::Expr>(std::move(lam));
}
ast::Expr* Parser::parse_define() {
    lexer_->consume(); auto n=lexer_->consume(); if(n.kind!=TokenKind::Identifier) return nullptr;
    auto* v=parse_val(); if(!v) return nullptr; lexer_->consume();
    return arena_.template create<ast::Expr>(ast::DefineNode{{},std::string(n.text),v});
}
ast::Expr* Parser::parse_let(bool r) {
    lexer_->consume(); if(lexer_->consume().kind!=TokenKind::LParen) return nullptr;
    struct B{std::string n;ast::Expr*v;}; std::vector<B> bs;
    while (lexer_->peek().kind!=TokenKind::RParen) {
        if(lexer_->consume().kind!=TokenKind::LParen) return nullptr;
        auto n=lexer_->consume(); if(n.kind!=TokenKind::Identifier) return nullptr;
        auto* v=parse_val(); if(!v) return nullptr; bs.push_back({std::string(n.text),v});
        if(lexer_->consume().kind!=TokenKind::RParen) return nullptr;
    }
    lexer_->consume(); auto* body=parse_expr(); if(!body) return nullptr;
    if(lexer_->peek().kind==TokenKind::RParen) lexer_->consume();
    for(auto it=bs.rbegin();it!=bs.rend();++it) {
        if(r) body=arena_.template create<ast::Expr>(ast::LetRecNode{{},it->n,it->v,body});
        else  body=arena_.template create<ast::Expr>(ast::LetNode{{},it->n,it->v,body});
    }
    return body;
}
ast::Expr* Parser::parse_val() {
    auto tok=lexer_->peek();
    switch(tok.kind){
    case TokenKind::Integer: return parse_int(lexer_->consume());
    case TokenKind::Identifier: return arena_.template create<ast::Expr>(ast::VariableNode{{},std::string(lexer_->consume().text)});
    case TokenKind::LParen: lexer_->consume(); return parse_list();
    default: return nullptr;
    }
}
void Parser::skip_rparen(){while(lexer_->peek().kind!=TokenKind::RParen&&!lexer_->eof())lexer_->consume();lexer_->consume();}

} // namespace aura::parser
