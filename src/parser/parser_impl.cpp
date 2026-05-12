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
    // () → null sentinel (0)
    if (lexer_->peek().kind == TokenKind::RParen) {
        lexer_->consume();
        return arena_.template create<ast::Expr>(ast::LiteralIntNode{{}, 0});
    }
    auto f = lexer_->peek();
    if (f.kind==TokenKind::Identifier) {
        auto kw=f.text;
        if (kw=="if") return parse_if();
        if (kw=="lambda") return parse_lambda();
        if (kw=="let") return parse_let(false);
        if (kw=="letrec") return parse_let(true);
        if (kw=="define") return parse_define();
        if (kw=="begin") return parse_begin();
        if (kw=="set!") return parse_set();
        if (kw=="quote") return parse_quote();
        if (kw=="cond") return parse_cond();
        if (kw=="defmacro") return parse_defmacro();
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

ast::Expr* Parser::parse_defmacro() {
    lexer_->consume(); // 'defmacro'
    // (name . params)
    if (lexer_->consume().kind != TokenKind::LParen) { skip_rparen(); return nullptr; }
    auto name_tok = lexer_->consume();
    if (name_tok.kind != TokenKind::Identifier) { skip_rparen(); return nullptr; }
    ast::MacroDefNode mac{{}, std::string(name_tok.text), {}, nullptr};
    while (lexer_->peek().kind != TokenKind::RParen) {
        auto p = lexer_->consume();
        if (p.kind != TokenKind::Identifier) { skip_rparen(); return nullptr; }
        mac.params.push_back(std::string(p.text));
    }
    lexer_->consume(); // ')'
    // Body is the next expression
    mac.body = parse_expr();
    if (!mac.body) { skip_rparen(); return nullptr; }
    lexer_->consume(); // ')'
    return arena_.template create<ast::Expr>(std::move(mac));
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
ast::Expr* Parser::parse_begin() {
    lexer_->consume(); // consume 'begin'
    ast::BeginNode begin{{}};
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        auto* e = parse_expr();
        if (e) begin.exprs.push_back(e);
        else break;
    }
    lexer_->consume(); // rparen
    return arena_.template create<ast::Expr>(std::move(begin));
}

ast::Expr* Parser::parse_set() {
    lexer_->consume(); // consume 'set!'
    auto n = lexer_->consume();
    if (n.kind != TokenKind::Identifier) { skip_rparen(); return nullptr; }
    auto* v = parse_val();
    if (!v) { skip_rparen(); return nullptr; }
    lexer_->consume(); // rparen
    return arena_.template create<ast::Expr>(ast::SetNode{{}, std::string(n.text), v});
}

ast::Expr* Parser::parse_quote() {
    lexer_->consume(); // consume 'quote'
    auto* v = parse_val();
    if (!v) { skip_rparen(); return nullptr; }
    lexer_->consume(); // rparen
    return arena_.template create<ast::Expr>(ast::QuoteNode{{}, v});
}

ast::Expr* Parser::parse_cond() {
    lexer_->consume(); // consume 'cond'
    // Desugar (cond (t1 e1) (t2 e2) (else en)) -> (if t1 e1 (if t2 e2 en))
    // Collect all clauses first, then build nested-if from the inside out
    struct Clause { ast::Expr* test; ast::Expr* val; };
    std::vector<Clause> clauses;
    while (lexer_->peek().kind != TokenKind::RParen && !lexer_->eof()) {
        if (lexer_->peek().kind != TokenKind::LParen) break;
        lexer_->consume(); // lparen of clause
        auto* test = parse_val();
        if (!test) { skip_rparen(); break; }
        auto* val = parse_val();
        if (!val) { skip_rparen(); break; }
        lexer_->consume(); // rparen of clause
        clauses.push_back({test, val});
    }
    lexer_->consume(); // rparen of cond
    // Build nested if from the inside out
    if (clauses.empty()) return nullptr;
    auto* result = clauses.back().val;
    for (auto it = clauses.rbegin() + 1; it != clauses.rend(); ++it) {
        result = arena_.template create<ast::Expr>(
            ast::IfExprNode{{}, it->test, it->val, result});
    }
    return result;
}

void Parser::skip_rparen(){while(lexer_->peek().kind!=TokenKind::RParen&&!lexer_->eof())lexer_->consume();lexer_->consume();}

} // namespace aura::parser
